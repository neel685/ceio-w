// Baselines compared against CEIO: DCTCP senders, HostCC, ShRing, Toasty; and
// the paper-scenario presets.

#include <gtest/gtest.h>

#include "ceio/exp/experiment.hpp"
#include "ceio/exp/presets.hpp"
#include "ceio/host/refill.hpp"
#include "ceio/net/congestion.hpp"
#include "ceio/policy/hostcc.hpp"

namespace ceio {

// ---------------------------------------------------------------------------
// DCTCP
// ---------------------------------------------------------------------------

namespace {
struct CcRig {
  Simulator sim;
  net::TrafficSource src;
  net::DctcpController cc;
  static net::FlowSpec spec() {
    net::FlowSpec f;
    f.packet_bytes = 1000;
    f.traffic.rate_gbps = 10.0;
    return f;
  }
  explicit CcRig(net::DctcpConfig cfg = {}) : src(sim, spec(), Rng(1), [](net::Packet) {}), cc(sim, src, cfg) {}
};
}  // namespace

TEST(Dctcp, MarksCutTheRateInProportionToAlpha) {
  CcRig r;
  r.cc.start();
  for (int w = 0; w < 50; ++w) {  // every ACK marked for 50 RTTs: alpha -> ~1
    for (int i = 0; i < 10; ++i) r.cc.on_delivered(true);
    r.sim.run_until(r.sim.now() + 20 * kMicrosecond);
  }
  EXPECT_GT(r.cc.alpha(), 0.9);
  EXPECT_LT(r.src.rate_gbps(), 1.0);
  EXPECT_GE(r.src.rate_gbps(), 0.1);  // min rate
}

TEST(Dctcp, LossHalvesAndCleanWindowsGrowBackToDemand) {
  CcRig r;
  r.cc.start();
  r.cc.on_lost();
  r.sim.run_until(21 * kMicrosecond);
  EXPECT_NEAR(r.src.rate_gbps(), 5.0, 1e-9);
  // One 1000 B packet per 20 us RTT = +0.4 Gbps per window, capped at the 10 Gbps demand.
  for (int w = 0; w < 30; ++w) {
    r.cc.on_delivered(false);
    r.sim.run_until(r.sim.now() + 20 * kMicrosecond);
  }
  EXPECT_DOUBLE_EQ(r.src.rate_gbps(), 10.0);
}

TEST(Dctcp, NotificationCountsAsMarkedAck) {
  CcRig r;
  r.cc.start();
  r.cc.on_notification();
  r.sim.run_until(21 * kMicrosecond);
  EXPECT_LT(r.src.rate_gbps(), 10.0);
  EXPECT_EQ(r.cc.reductions(), 1u);
}

// ---------------------------------------------------------------------------
// HostCC
// ---------------------------------------------------------------------------

TEST(HostCc, MarksArrivingPacketsWhileIioIsBackedUp) {
  // Slow DRAM + a tiny LLC: every DMA line forces a writeback, so the IIO fills.
  Simulator sim;
  hw::CacheConfig c;
  c.ways = 2;
  c.ddio_ways = 1;
  c.size_bytes = 64ULL * 2 * 64;
  hw::DramConfig d;
  d.channels = 1;
  d.channel_gbytes_per_s = 2.0;
  d.write_queue_lines = 2;
  hw::MemorySystem mem(c, d);
  hw::PcieHost pcie(sim, mem);
  nic::Nic nic(sim, pcie);
  policy::HostCcController hcc(sim, nic, pcie, nullptr, policy::HostCcConfig{});
  hcc.start();
  for (int i = 0; i < 256; ++i) pcie.dma_write(static_cast<std::uint64_t>(i) * 2048, 2048, [] {});
  sim.run_until(20 * kMicrosecond);
  EXPECT_TRUE(hcc.congested());
  net::Packet p;
  p.flow = 1;
  p.bytes = 64;
  nic.receive(p);  // no rule: dropped later, but marked on ingress first
  EXPECT_EQ(hcc.stats().marked_packets, 1u);
  sim.run_until(2 * kMillisecond);  // HostCC samples forever: bound the run
  EXPECT_FALSE(hcc.congested());
  EXPECT_GT(hcc.stats().congested_samples, 0u);
}

TEST(HostCc, HostLocalResponseThrottlesTheCoRunnerAndRestoresIt) {
  // While the IIO is backed up, HostCC cuts the co-runner's memory bandwidth
  // (Intel MBA); once the congestion clears it ramps it back up.
  Simulator sim;
  hw::CacheConfig c;
  c.ways = 2;
  c.ddio_ways = 1;
  c.size_bytes = 64ULL * 2 * 64;
  hw::DramConfig d;
  d.channels = 1;
  d.channel_gbytes_per_s = 2.0;
  d.write_queue_lines = 2;
  hw::MemorySystem mem(c, d);
  hw::PcieHost pcie(sim, mem);
  nic::Nic nic(sim, pcie);
  host::MemoryHog hog(sim, mem, 1ULL << 40, 1ULL << 20, 1.0, 0.0, Rng(3));
  policy::HostCcController hcc(sim, nic, pcie, &hog, policy::HostCcConfig{});
  hcc.start();
  for (int i = 0; i < 256; ++i) pcie.dma_write(static_cast<std::uint64_t>(i) * 2048, 2048, [] {});
  sim.run_until(20 * kMicrosecond);
  EXPECT_DOUBLE_EQ(hog.gbytes_per_s(), 0.1);  // throttled to the 10 % floor
  sim.run_until(2 * kMillisecond);
  EXPECT_DOUBLE_EQ(hog.gbytes_per_s(), 1.0);  // restored after congestion cleared
}

// ---------------------------------------------------------------------------
// ShRing
// ---------------------------------------------------------------------------

TEST(ShRing, MergesCpuQueuesIntoOneSharedRing) {
  exp::Scenario s = exp::ceio_mixed_flows(exp::Scheme::kShRing, 4);
  ASSERT_EQ(s.queues.size(), 5u);  // one shared ring + 4 RDMA queues
  EXPECT_EQ(s.queues[0].cores, 4u);
  EXPECT_EQ(s.queues[0].ring_size, 4096u);
  for (const auto& f : s.flows) {
    if (f.spec.id < 100) {
      EXPECT_EQ(f.queue, 0u);
    } else {
      EXPECT_GT(f.queue, 0u);
    }
  }
}

TEST(ShRing, AllCoresShareTheLoadWithoutLoss) {
  exp::Scenario s = exp::ceio_testbed();
  s.warmup = 500 * kMicrosecond;
  s.measure = kMillisecond;
  for (int i = 0; i < 4; ++i) {
    exp::QueueSetup q;
    q.app = host::AppProfile::kv();
    s.queues.push_back(q);
    exp::FlowSetup f;
    f.spec.id = static_cast<net::FlowId>(i + 1);
    f.spec.packet_bytes = 512;
    f.spec.traffic.rate_gbps = 5;
    f.queue = static_cast<std::uint32_t>(i);
    s.flows.push_back(f);
  }
  exp::SchemeOptions o;
  o.dctcp = false;
  exp::apply_scheme(s, exp::Scheme::kShRing, o);
  const exp::Results r = exp::run(s);
  EXPECT_EQ(r.drops_nic, 0u);
  EXPECT_NEAR(r.goodput_gbps, 20.0, 0.5);
  ASSERT_EQ(r.queues.size(), 4u);
  for (const auto& q : r.queues) EXPECT_GT(q.processed, 500u) << "core " << q.core;  // all cores work
  EXPECT_LE(r.queues[0].distinct_buffers_used, 8192u);
}

// ---------------------------------------------------------------------------
// Toasty
// ---------------------------------------------------------------------------

TEST(ToastyRefill, FollowsPseudocodeOne) {
  host::ToastyRefill t;
  host::RefillContext c;
  c.ring_capacity = 4096;
  c.link_gbps = 100;
  c.packet_bytes = 512;
  c.since_last = 10 * kMicrosecond;
  // Low load (20 x 512 B in 10 us ~ 8 Gbps), short of k*N_RXQ = 200: refill warm only.
  c.dmaed_since_last = 20;
  c.recycled_since_last = 30;
  c.ring_available = 100;
  EXPECT_EQ(t.to_post(c), 30u);
  // High load (200 packets in 10 us ~ 82 Gbps): top up to k*N_RXQ with cold buffers too.
  c.dmaed_since_last = 200;
  c.ring_available = 500;
  EXPECT_EQ(t.to_post(c), 1500u);
  // Enough available, low load: post nothing (drain back to a small working set).
  c.dmaed_since_last = 20;
  c.ring_available = 300;
  EXPECT_EQ(t.to_post(c), 0u);
  // Enough available, high load: only the recently recycled buffers.
  c.dmaed_since_last = 200;
  c.ring_available = 3000;
  c.recycled_since_last = 180;
  EXPECT_EQ(t.to_post(c), 180u);
  // Idle start: the floor lets traffic begin.
  c.dmaed_since_last = 0;
  c.recycled_since_last = 0;
  c.ring_available = 0;
  EXPECT_EQ(t.to_post(c), 64u);
}

namespace {
exp::Scenario nat(std::uint32_t pool, std::uint32_t ring, exp::ToastyPart* part = nullptr) {
  exp::Scenario s = exp::toasty_testbed();
  s.warmup = kMillisecond;
  s.measure = kMillisecond;
  exp::QueueSetup q;
  q.app = host::AppProfile::nat();
  q.pool_buffers = pool;
  q.ring_size = ring;
  q.buffer_bytes = 4096;
  q.core.ghz = 2.0;
  if (part) exp::apply_toasty(q, *part);
  s.queues.push_back(q);
  exp::FlowSetup f;
  f.spec.id = 1;
  f.spec.packet_bytes = 512;
  f.spec.traffic.rate_gbps = 50;
  s.flows.push_back(f);
  return s;
}
}  // namespace

TEST(Toasty, BeatsDefaultPoolAndItsPartsAlone) {
  // [Toasty] Fig. 7 and Fig. 18: full Toasty > LIFO pool only > adaptive refill
  // only ~ default, all with the default 16384-buffer pool and 4096-entry ring.
  auto full = exp::ToastyPart::kFull, user = exp::ToastyPart::kUserOnly, kernel = exp::ToastyPart::kKernelOnly;
  const double def = exp::find_saturation(nat(16384, 4096), 0.001, 6).offered_gbps;
  const double t_full = exp::find_saturation(nat(16384, 4096, &full), 0.001, 6).offered_gbps;
  const double t_user = exp::find_saturation(nat(16384, 4096, &user), 0.001, 6).offered_gbps;
  const double t_kernel = exp::find_saturation(nat(16384, 4096, &kernel), 0.001, 6).offered_gbps;
  EXPECT_GT(t_full, 1.1 * def);
  EXPECT_GT(t_full, t_user);
  EXPECT_GT(t_user, t_kernel);
}

TEST(Toasty, KeepsWorkingSetSmallAndCutsWritebacks) {
  // [Toasty] Fig. 3 / Fig. 4 at a sustainable load.
  auto full = exp::ToastyPart::kFull;
  exp::Scenario a = nat(16384, 4096), b = nat(16384, 4096, &full);
  a.flows[0].spec.traffic.rate_gbps = b.flows[0].spec.traffic.rate_gbps = 30;
  const exp::Results def = exp::run(a), toasty = exp::run(b);
  EXPECT_LT(toasty.queues[0].distinct_buffers_used, 2048u);
  EXPECT_EQ(def.queues[0].distinct_buffers_used, 16384u);
  EXPECT_GT(toasty.io_write_hit_rate, def.io_write_hit_rate + 0.3);
  EXPECT_LT(toasty.dram_write_gbytes_s, 0.5 * def.dram_write_gbytes_s);
}

TEST(Toasty, AbsorbsMicroburstsLikeALargePool) {
  // [Toasty] Table 7: no drops under 2048-packet microbursts, unlike a small pool.
  auto full = exp::ToastyPart::kFull;
  auto burst = [](exp::Scenario s) {
    s.flows[0].spec.traffic.pattern = net::TrafficSpec::Pattern::kMicroburst;
    s.flows[0].spec.traffic.rate_gbps = 8;
    s.measure = 3 * kMillisecond;
    return exp::run(s);
  };
  EXPECT_EQ(burst(nat(16384, 4096, &full)).drops_nic, 0u);
  EXPECT_GT(burst(nat(512, 512)).drops_nic, 500u);
}

// ---------------------------------------------------------------------------
// Paper-scenario presets
// ---------------------------------------------------------------------------

class PresetSmoke : public ::testing::TestWithParam<exp::Scheme> {};

TEST_P(PresetSmoke, DynamicFlowsRunAndSample) {
  const exp::Scenario s = exp::ceio_dynamic_flows(GetParam(), 500 * kMicrosecond, 2);
  const exp::Results r = exp::run(s);
  EXPECT_EQ(r.flows.size(), 10u);  // 8 KV + 2 bypass
  EXPECT_FALSE(r.samples.empty());
  EXPECT_GT(r.delivered_packets, 10000u);
  if (GetParam() != exp::Scheme::kShRing) EXPECT_EQ(r.reordered, 0u);
  if (GetParam() == exp::Scheme::kCeio) EXPECT_TRUE(r.ceio_credits_conserved);
}

INSTANTIATE_TEST_SUITE_P(Schemes, PresetSmoke,
                         ::testing::Values(exp::Scheme::kBaseline, exp::Scheme::kHostCc, exp::Scheme::kShRing,
                                           exp::Scheme::kCeio, exp::Scheme::kToasty),
                         [](const auto& info) { return exp::scheme_name(info.param); });

TEST(Presets, SchemeNamesRoundTrip) {
  for (auto s : {exp::Scheme::kBaseline, exp::Scheme::kHostCc, exp::Scheme::kShRing, exp::Scheme::kCeio,
                 exp::Scheme::kToasty})
    EXPECT_EQ(exp::scheme_from_name(exp::scheme_name(s)), s);
  EXPECT_THROW(exp::scheme_from_name("nope"), std::invalid_argument);
}

}  // namespace ceio
