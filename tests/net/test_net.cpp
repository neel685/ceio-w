#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "ceio/net/link.hpp"
#include "ceio/net/traffic.hpp"

namespace ceio::net {
namespace {

FlowSpec flow(TrafficSpec::Pattern p, double gbps, std::uint32_t pkt = 1000, std::uint32_t msg = 1000) {
  FlowSpec f;
  f.id = 7;
  f.packet_bytes = pkt;
  f.message_bytes = msg;
  f.traffic.pattern = p;
  f.traffic.rate_gbps = gbps;
  return f;
}

std::vector<Packet> collect(FlowSpec f, Tick until, std::uint64_t seed = 1) {
  Simulator sim;
  std::vector<Packet> out;
  TrafficSource src(sim, f, Rng(seed), [&](Packet p) { out.push_back(p); });
  src.start();
  sim.run_until(until);
  return out;
}

}  // namespace

TEST(Traffic, ConstantRateHitsTarget) {
  // 10 Gbps of 1000 B frames = 1.25 Mpps -> 1250 packets per ms.
  const auto pkts = collect(flow(TrafficSpec::Pattern::kConstant, 10.0), kMillisecond - 1);
  EXPECT_EQ(pkts.size(), 1250u);
  EXPECT_EQ(pkts[1].sent - pkts[0].sent, transfer_time(1000, 10.0));
  for (std::size_t i = 0; i < pkts.size(); ++i) EXPECT_EQ(pkts[i].seq, i);
}

TEST(Traffic, MultiPacketMessagesAreMarked) {
  const auto pkts = collect(flow(TrafficSpec::Pattern::kConstant, 10.0, 1000, 3500), 10 * kMicrosecond);
  ASSERT_GE(pkts.size(), 8u);
  EXPECT_EQ(pkts[0].msg_pkts, 4u);  // ceil(3500/1000)
  EXPECT_EQ(pkts[0].msg_index, 0u);
  EXPECT_FALSE(pkts[0].msg_last);
  EXPECT_TRUE(pkts[3].msg_last);
  EXPECT_EQ(pkts[3].msg_index, 3u);
  EXPECT_EQ(pkts[4].msg_id, 1u);
  EXPECT_EQ(pkts[4].msg_index, 0u);
}

TEST(Traffic, PoissonMeanRate) {
  const auto pkts = collect(flow(TrafficSpec::Pattern::kPoisson, 10.0), 20 * kMillisecond);
  EXPECT_NEAR(static_cast<double>(pkts.size()), 25'000.0, 25'000.0 * 0.03);
}

TEST(Traffic, MicroburstShape) {
  FlowSpec f = flow(TrafficSpec::Pattern::kMicroburst, 1.0, 512, 512);
  f.traffic.burst_packets = 2048;
  f.traffic.burst_period = kMillisecond;
  const auto pkts = collect(f, 3 * kMillisecond - 1);
  ASSERT_EQ(pkts.size(), 3u * 2048);
  EXPECT_EQ(pkts[0].sent, pkts[2047].sent);           // a burst leaves at once
  EXPECT_EQ(pkts[2048].sent - pkts[0].sent, kMillisecond);
}

TEST(Traffic, PoissonRateRedrawsPerEpoch) {
  FlowSpec f = flow(TrafficSpec::Pattern::kPoissonRate, 20.0);
  f.traffic.epoch = kMillisecond;
  const auto pkts = collect(f, 50 * kMillisecond);
  // Average rate over many epochs is the mean; per-epoch rates vary.
  EXPECT_NEAR(static_cast<double>(pkts.size()), 125'000.0, 125'000.0 * 0.1);
  std::vector<std::size_t> per_epoch(50, 0);
  for (const auto& p : pkts) ++per_epoch[p.sent / kMillisecond];
  std::size_t lo = *std::min_element(per_epoch.begin(), per_epoch.end());
  std::size_t hi = *std::max_element(per_epoch.begin(), per_epoch.end());
  EXPECT_GT(hi, lo + 500);
}

TEST(Traffic, SameSeedSameTrace) {
  auto a = collect(flow(TrafficSpec::Pattern::kPoisson, 5.0), kMillisecond, 3);
  auto b = collect(flow(TrafficSpec::Pattern::kPoisson, 5.0), kMillisecond, 3);
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) EXPECT_EQ(a[i].sent, b[i].sent);
}

TEST(Traffic, StopTimeAndRateOverride) {
  Simulator sim;
  std::uint64_t n = 0;
  FlowSpec f = flow(TrafficSpec::Pattern::kConstant, 10.0);
  f.stop = 100 * kMicrosecond;
  TrafficSource src(sim, f, Rng(1), [&](Packet) { ++n; });
  src.start();
  src.set_rate_gbps(5.0);
  sim.run_until(kMillisecond);
  EXPECT_EQ(n, 63u);  // 5 Gbps of 1000 B = 1.6 us gap -> packets at 0..99.2 us
}

TEST(Traffic, OnOffScheduleGatesSending) {
  FlowSpec f = flow(TrafficSpec::Pattern::kConstant, 10.0);
  f.active = {{100 * kMicrosecond, 200 * kMicrosecond}, {500 * kMicrosecond, 600 * kMicrosecond}};
  const auto pkts = collect(f, kMillisecond);
  ASSERT_FALSE(pkts.empty());
  for (const auto& p : pkts) {
    const bool in1 = p.sent >= 100 * kMicrosecond && p.sent < 200 * kMicrosecond;
    const bool in2 = p.sent >= 500 * kMicrosecond && p.sent < 600 * kMicrosecond;
    EXPECT_TRUE(in1 || in2) << p.sent;
  }
  EXPECT_NEAR(static_cast<double>(pkts.size()), 250.0, 2.0);  // 2 x 100 us at 1.25 Mpps
  EXPECT_EQ(f.next_active(300 * kMicrosecond), 500 * kMicrosecond);
  EXPECT_EQ(f.next_active(700 * kMicrosecond), ~Tick{0});
}

TEST(SwitchPort, SerialisesAtLineRateWithWireOverhead) {
  Simulator sim;
  LinkConfig cfg;
  cfg.rate_gbps = 100.0;
  cfg.propagation = 0;
  std::vector<Tick> arrivals;
  SwitchPort port(sim, cfg, [&](Packet p) { arrivals.push_back(p.nic_arrival); });
  for (int i = 0; i < 10; ++i) {
    Packet p;
    p.bytes = 1004;
    port.enqueue(p);
  }
  sim.run();
  ASSERT_EQ(arrivals.size(), 10u);
  EXPECT_EQ(arrivals[0], transfer_time(1024, 100.0));
  EXPECT_EQ(arrivals[9], 10 * transfer_time(1024, 100.0));
  EXPECT_EQ(port.stats().bytes_delivered, 10'040u);
}

TEST(SwitchPort, TailDropAndEcnMarking) {
  Simulator sim;
  LinkConfig cfg;
  cfg.buffer_bytes = 10'000;
  cfg.ecn_threshold_bytes = 3'000;
  std::vector<Packet> got;
  int drops = 0;
  SwitchPort port(sim, cfg, [&](Packet p) { got.push_back(p); });
  port.set_drop_hook([&](const Packet&) { ++drops; });
  for (int i = 0; i < 20; ++i) {
    Packet p;
    p.bytes = 1000;
    p.seq = static_cast<std::uint64_t>(i);
    port.enqueue(p);
  }
  sim.run();
  EXPECT_EQ(got.size(), 10u);
  EXPECT_EQ(drops, 10);
  EXPECT_FALSE(got[2].ecn);
  EXPECT_TRUE(got[3].ecn);  // queue held 3000 B when it arrived
  EXPECT_EQ(port.stats().ecn_marked, 7u);
  EXPECT_EQ(port.queued_bytes(), 0u);
}

}  // namespace ceio::net
