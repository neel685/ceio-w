// CEIO-W (docs/ceio-w.md): correctness invariants and the research claims,
// each checked against CEIO under the same model.

#include <gtest/gtest.h>

#include <stdexcept>

#include "ceio/exp/experiment.hpp"
#include "ceio/exp/presets.hpp"

namespace ceio::exp {
namespace {

/// One KV core, 512 B requests, 4096-entry ring, no congestion control.
Scenario kv(double gbps, Scheme scheme) {
  Scenario s = ceio_testbed();
  s.warmup = kMillisecond;
  s.measure = 2 * kMillisecond;
  QueueSetup q;
  q.app = host::AppProfile::kv();
  q.ring_size = 4096;
  q.pool_buffers = 8192;
  s.queues.push_back(q);
  FlowSetup f;
  f.spec.id = 1;
  f.spec.packet_bytes = 512;
  f.spec.message_bytes = 512;
  f.spec.traffic.rate_gbps = gbps;
  s.flows.push_back(f);
  SchemeOptions o;
  o.dctcp = false;
  apply_scheme(s, scheme, o);
  return s;
}

/// Eight KV cores with DCTCP sharing the host with a 100 GB/s memory co-runner.
Scenario pressured(Scheme scheme) {
  Scenario s = ceio_mixed_flows(scheme, 8);
  s.warmup = kMillisecond;
  s.measure = 3 * kMillisecond;
  s.pressure.gbytes_per_s = 100;
  return s;
}

}  // namespace

TEST(CeioW, BelowCapacityReusesAHandfulOfWarmBuffers) {
  // Cache footprint: same throughput as CEIO, but the address footprint collapses and with it
  // the write-allocate misses and wasted writebacks CEIO's credits do not bound.
  const Results ceio = run(kv(15, Scheme::kCeio)), w = run(kv(15, Scheme::kCeioW));
  EXPECT_NEAR(w.goodput_gbps, ceio.goodput_gbps, 0.1);
  EXPECT_EQ(ceio.queues[0].distinct_buffers_used, 8192u);
  EXPECT_LT(w.queues[0].distinct_buffers_used, 64u);
  EXPECT_GT(w.io_write_hit_rate, 0.95);
  EXPECT_LT(ceio.io_write_hit_rate, 0.2);
  EXPECT_LT(w.cache.dead_writebacks, ceio.cache.dead_writebacks / 100 + 1);
  EXPECT_EQ(w.reordered + w.drops_nic, 0u);
}

TEST(CeioW, OverloadKeepsFootprintBoundedAndOutperformsCeio) {
  const Results ceio = run(kv(26, Scheme::kCeio)), w = run(kv(26, Scheme::kCeioW));
  EXPECT_GT(w.goodput_gbps, 1.3 * ceio.goodput_gbps);
  EXPECT_GT(w.queues[0].llc_read_hit_rate, 0.95);
  EXPECT_GT(w.io_write_hit_rate, 0.9);
  EXPECT_LT(w.queues[0].distinct_buffers_used, 2048u);
  EXPECT_LE(w.ceio.w_max_in_use, w.ceio_total_credits);  // footprint never exceeds Eq. (1)
  EXPECT_LT(w.latency.p999_us, ceio.latency.p999_us);
  EXPECT_EQ(w.reordered, 0u);  // ring-empty switches keep arrival order
  EXPECT_EQ(w.drops_nic, 0u);  // still elastic: nothing dropped
}

TEST(CeioW, AdaptiveAllocationBeatsEqualSplitUnderOverload) {
  // Ablation: an equal split hands the single flow the whole budget, so its
  // ring (and footprint) grows far beyond what its consumption needs.
  Scenario equal = kv(26, Scheme::kCeioW);
  equal.ceio.w_adaptive_alloc = false;
  const Results a = run(kv(26, Scheme::kCeioW)), e = run(equal);
  EXPECT_GT(a.goodput_gbps, 1.05 * e.goodput_gbps);
  EXPECT_LT(a.queues[0].distinct_buffers_used, e.queues[0].distinct_buffers_used);
}

TEST(CeioW, SurvivesMemoryPressureThatCollapsesCeio) {
  // Throughput: with DRAM near saturation, every cold-buffer DMA forces a writeback that
  // stalls the IIO. Warm, footprint-bounded buffers avoid it; the adaptive budget
  // (adaptive budget) shrinks the footprint as the host's miss counters rise.
  const Results ceio = run(pressured(Scheme::kCeio));
  const Results toasty = run(pressured(Scheme::kToasty));
  const Results w = run(pressured(Scheme::kCeioW));
  Scenario fixed = pressured(Scheme::kCeioW);
  fixed.ceio.w_adaptive_budget = false;
  const Results st = run(fixed);
  EXPECT_GT(w.mpps, 3 * ceio.mpps);
  EXPECT_GT(w.mpps, toasty.mpps);
  EXPECT_GT(w.ceio.w_budget_cuts, 0u);
  EXPECT_LT(w.ceio.w_budget_min, w.ceio_total_credits / 2);
  EXPECT_GT(w.mpps, st.mpps);                        // the adaptive budget helps...
  EXPECT_LT(w.latency.p999_us, st.latency.p999_us);  // ...and cuts the tail
  EXPECT_EQ(w.drops_nic + w.reordered, 0u);
}

TEST(CeioW, BulkFlowsAreCappedAndKeepTheirThroughput) {
  // Adaptive allocation: RDMA flows (identified by transport type) get a small fixed cap and use the
  // slow path beyond it, without losing throughput.
  Scenario s = ceio_mixed_flows(Scheme::kCeioW, 2);
  s.warmup = kMillisecond;
  s.measure = 5 * kMillisecond;
  const Results r = run(s);
  double bypass = 0;
  std::uint64_t bypass_slow = 0;
  for (const auto& f : r.flows)
    if (f.id >= 100) {
      bypass += f.goodput_gbps;
      bypass_slow += f.slow_packets;
    }
  EXPECT_GT(bypass, 0.9 * 150);
  EXPECT_GT(bypass_slow, 0u);
  EXPECT_EQ(r.reordered, 0u);
  EXPECT_EQ(r.drops_nic, 0u);
}

TEST(CeioW, RejectsSharedQueuesItCannotAccount) {
  Scenario s = kv(10, Scheme::kCeioW);
  FlowSetup second = s.flows[0];
  second.spec.id = 2;
  s.flows.push_back(second);  // two CPU-involved flows on one ring
  EXPECT_THROW(run(s), std::invalid_argument);
}

}  // namespace ceio::exp
