// End-to-end tests: whole-host scenarios, invariants, and the emergent
// behaviours that later phases (CEIO, Toasty, CEIO-W) are measured against.

#include "ceio/exp/experiment.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace ceio::exp {
namespace {

/// One NF core on the Toasty testbed, saturating 100 Gbps of 512 B packets.
Scenario toasty_nf(const char* app, std::uint32_t pool, std::uint32_t ring, double rate = 100.0,
                   host::PoolOrder order = host::PoolOrder::kFifo) {
  Scenario s = toasty_testbed();
  s.warmup = kMillisecond;
  s.measure = kMillisecond;
  QueueSetup q;
  q.app = host::AppProfile::by_name(app);
  q.pool_buffers = pool;
  q.ring_size = ring;
  q.buffer_bytes = 4096;
  q.order = order;
  q.core.ghz = 2.0;
  s.queues.push_back(q);
  FlowSetup f;
  f.spec.id = 1;
  f.spec.packet_bytes = 512;
  f.spec.message_bytes = 512;
  f.spec.traffic.rate_gbps = rate;
  s.flows.push_back(f);
  return s;
}

}  // namespace

TEST(Experiment, ValidationCatchesBadScenarios) {
  Scenario s = toasty_nf("nat", 1024, 1024);
  s.flows[0].queue = 3;
  EXPECT_THROW(run(s), std::invalid_argument);
  s = toasty_nf("nat", 1024, 1024);
  s.flows[0].spec.packet_bytes = 9000;
  EXPECT_THROW(run(s), std::invalid_argument);
  s = toasty_nf("nat", 1024, 1024);
  s.flows.push_back(s.flows[0]);
  EXPECT_THROW(run(s), std::invalid_argument);
}

TEST(Experiment, UnderloadDeliversEverythingWithoutLoss) {
  const Results r = run(toasty_nf("nat", 4096, 1024, 10.0));
  EXPECT_EQ(r.drops_nic + r.drops_switch, 0u);
  EXPECT_NEAR(r.goodput_gbps, 10.0, 0.2);
  // Everything sent in the window is delivered, except packets still in flight.
  EXPECT_LE(r.delivered_packets, r.sent_packets + 50);
  EXPECT_GE(r.delivered_packets + 50, r.sent_packets);
  EXPECT_GT(r.latency.p50_us, 0.7);  // >= switch + PCIe + processing
  EXPECT_LT(r.latency.p99_us, 10.0);
  // At low load each wake-up handles ~1 packet, so per-poll and refill overheads
  // are poorly amortised; the core is still far from saturated.
  EXPECT_LT(r.queues[0].core_busy_fraction, 0.6);
}

TEST(Experiment, DeterministicForEqualSeeds) {
  Scenario s = toasty_nf("kv", 2048, 1024, 30.0);
  s.flows[0].spec.traffic.pattern = net::TrafficSpec::Pattern::kPoisson;
  const Results a = run(s), b = run(s);
  EXPECT_EQ(a.delivered_packets, b.delivered_packets);
  EXPECT_EQ(a.cache.cpu_read_misses, b.cache.cpu_read_misses);
  EXPECT_EQ(a.latency.p999_us, b.latency.p999_us);
  EXPECT_EQ(a.events, b.events);
  s.seed = 2;
  EXPECT_NE(run(s).latency.p999_us, a.latency.p999_us);
}

TEST(Experiment, PacketConservationUnderOverload) {
  // Every packet the sender emitted is delivered, dropped, or still queued.
  Scenario s = toasty_nf("nat", 16384, 4096, 100.0);
  s.warmup = 0;
  s.measure = 4 * kMillisecond;
  const Results r = run(s);
  const std::uint64_t accounted = r.delivered_packets + r.drops_nic + r.drops_switch;
  EXPECT_LE(accounted, r.sent_packets);
  // In flight at most: switch buffer (4 MB / 512 B) + NIC buffer (1 MB / 512 B) +
  // every pool buffer holding an unprocessed packet + one batch.
  EXPECT_LT(r.sent_packets - accounted, 8192u + 2048 + 16384 + 64);
  EXPECT_GT(r.drops_nic + r.drops_switch, 0u);
}

TEST(Experiment, LargeFifoPoolCausesUnnecessaryWritebacks) {
  // [Toasty] Sec. 2, Fig. 3: at a sustainable load, a FIFO pool whose footprint exceeds
  // the DDIO ways DMAs into cold buffers: PCIe write hits fall, and every
  // allocation writes back a dead (already recycled) dirty line.
  const Results small = run(toasty_nf("l2fwd", 512, 512, 30.0));
  const Results big = run(toasty_nf("l2fwd", 16384, 4096, 30.0));
  EXPECT_GT(small.io_write_hit_rate, big.io_write_hit_rate + 0.4);
  EXPECT_GT(big.cache.dead_writebacks, 3 * small.cache.dead_writebacks + 1000);
  EXPECT_GT(big.dram_write_gbytes_s, 2 * small.dram_write_gbytes_s);
  EXPECT_EQ(big.queues[0].distinct_buffers_used, 16384u);
}

TEST(Experiment, LargeFifoPoolLowersSaturationThroughput) {
  // [Toasty] Fig. 2 / Fig. 7: saturation throughput (loss <= 0.1 %) of the default
  // large pool is below that of a pool sized to the DDIO ways.
  Scenario fits = toasty_nf("nat", 4096, 4096, 50.0);
  Scenario big = toasty_nf("nat", 16384, 4096, 50.0);
  for (Scenario* s : {&fits, &big}) {
    s->warmup = kMillisecond;  // long enough for a 4096-buffer pool to cycle once
    s->measure = kMillisecond;
  }
  const SaturationResult a = find_saturation(fits, 0.001, 6);
  const SaturationResult b = find_saturation(big, 0.001, 6);
  EXPECT_GT(a.offered_gbps, 1.1 * b.offered_gbps);
  EXPECT_LE(a.at_rate.drops_nic + a.at_rate.drops_switch, a.at_rate.sent_packets / 1000);
  EXPECT_EQ(a.runs, 7);
}

TEST(Experiment, SmallPoolDropsUnderMicrobursts) {
  // [Toasty] Table 2: a tiny pool runs dry during a 2048-packet microburst even
  // though the average load is low; a large pool absorbs it.
  auto burst = [](std::uint32_t pool, std::uint32_t ring) {
    Scenario s = toasty_nf("nat", pool, ring, 8.0);
    s.flows[0].spec.traffic.pattern = net::TrafficSpec::Pattern::kMicroburst;
    s.flows[0].spec.traffic.burst_packets = 2048;
    s.flows[0].spec.traffic.burst_period = kMillisecond;
    s.nic.rx_buffer_bytes = 256 * 1024;
    s.measure = 3 * kMillisecond;
    return run(s);
  };
  const Results tiny = burst(256, 256);
  const Results large = burst(16384, 4096);
  EXPECT_GT(tiny.drops_nic, 100u);
  EXPECT_EQ(large.drops_nic, 0u);
}

TEST(Experiment, LifoPoolAloneDoesNotFixAFullRing) {
  // [Toasty] Sec. 3.2 / Fig. 18: with a full 4096-entry FIFO RX ring, the NIC still
  // cycles through >= 4096 buffers, so a LIFO pool alone leaves the working set
  // too large. Shrinking the ring is what lets LIFO keep buffers warm.
  const Results lifo_full = run(toasty_nf("l2fwd", 16384, 4096, 30.0, host::PoolOrder::kLifo));
  const Results lifo_small_ring = run(toasty_nf("l2fwd", 16384, 512, 30.0, host::PoolOrder::kLifo));
  EXPECT_GT(lifo_small_ring.io_write_hit_rate, lifo_full.io_write_hit_rate + 0.2);
  EXPECT_LT(lifo_small_ring.queues[0].distinct_buffers_used, lifo_full.queues[0].distinct_buffers_used);
}

TEST(Experiment, CpuBypassTrafficPollutesTheCache) {
  // [CEIO] Sec. 2.2 / Fig. 3: when a CPU-involved flow is backlogged, CPU-bypass (RDMA)
  // traffic flushes the shared DDIO ways and evicts its packets before the CPU
  // reads them, costing hit rate and throughput.
  auto mixed = [](double bypass_gbps) {
    Scenario s = ceio_testbed();
    s.warmup = kMillisecond;
    s.measure = 2 * kMillisecond;
    QueueSetup cpu;
    cpu.app = host::AppProfile::kv();
    cpu.pool_buffers = 4096;  // larger than the ring, leaving buffers for replies
    cpu.ring_size = 1024;
    s.queues.push_back(cpu);
    FlowSetup f;
    f.spec.id = 1;
    f.spec.packet_bytes = 512;
    f.spec.message_bytes = 512;
    f.spec.traffic.rate_gbps = 26.0;  // just above one KV core's capacity
    f.queue = 0;
    s.flows.push_back(f);
    if (bypass_gbps > 0) {
      QueueSetup rdma;
      rdma.kind = nic::QueueKind::kRdma;
      rdma.app = host::AppProfile::rdma_sink();
      rdma.mr_bytes = 256ULL << 20;
      s.queues.push_back(rdma);
      FlowSetup b;
      b.spec.id = 2;
      b.spec.packet_bytes = 1024;
      b.spec.message_bytes = 64 * 1024;
      b.spec.traffic.rate_gbps = bypass_gbps;
      b.queue = 1;
      s.flows.push_back(b);
    }
    return run(s);
  };
  const Results alone = mixed(0);
  const Results shared = mixed(150);
  EXPECT_GT(shared.cache.leaky_evictions, alone.cache.leaky_evictions + 1000);
  // Judge the CPU-involved core on its own loads (the RDMA core's CQE hits would
  // otherwise inflate the global rate).
  EXPECT_LT(shared.queues[0].llc_read_hit_rate, alone.queues[0].llc_read_hit_rate - 0.3);
  EXPECT_LT(shared.flows[0].goodput_gbps, 0.9 * alone.flows[0].goodput_gbps);
  // The starved KV queue drops only its own packets; the RDMA flow is unaffected.
  EXPECT_EQ(shared.flows[1].drops_nic, 0u);
  EXPECT_NEAR(shared.flows[1].goodput_gbps, 150.0, 5.0);
}

TEST(Experiment, MemoryPressureEvictsAppDataAndAddsLatency) {
  // The co-runner pollutes the LLC and loads DRAM. (Toasty's Sec. 4.3 throughput claim
  // is covered by the memory_pressure experiment.)
  auto with = [](double gbs) {
    Scenario s = toasty_nf("nat", 4096, 4096, 30.0);
    s.pressure.gbytes_per_s = gbs;
    return run(s);
  };
  const Results quiet = with(0), loud = with(120);
  EXPECT_LT(loud.queues[0].llc_read_hit_rate, quiet.queues[0].llc_read_hit_rate - 0.05);
  EXPECT_GT(loud.latency.mean_us, quiet.latency.mean_us);
  EXPECT_GT(loud.dram_read_gbytes_s, 50.0);
}

TEST(Experiment, JsonContainsKeyFields) {
  Scenario s = toasty_nf("echo", 1024, 512, 5.0);
  s.measure = 200 * kMicrosecond;
  const std::string j = to_json(run(s));
  for (const char* k : {"\"goodput_gbps\":", "\"io_write_hit_rate\":", "\"p999_us\":", "\"flows\":[{",
                        "\"queues\":[{", "\"dead_writebacks\":"})
    EXPECT_NE(j.find(k), std::string::npos) << k;
  EXPECT_EQ(j.front(), '{');
  EXPECT_EQ(j.back(), '}');
}

}  // namespace ceio::exp
