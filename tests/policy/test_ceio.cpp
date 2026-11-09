// CEIO end-to-end: correctness of the fast/slow path machinery and the
// paper's core claims, each checked against the baseline datapath.

#include "ceio/policy/ceio.hpp"

#include <gtest/gtest.h>

#include "ceio/exp/experiment.hpp"

namespace ceio::exp {
namespace {

/// One CPU-involved KV flow on the CEIO testbed, optionally sharing the host
/// with one RDMA-write (CPU-bypass) flow.
Scenario kv(double gbps, IoPolicy policy, std::uint32_t ring = 1024, double rdma_gbps = 0,
            std::uint32_t rdma_msg = 4 << 20) {
  Scenario s = ceio_testbed();
  s.warmup = kMillisecond;
  s.measure = 2 * kMillisecond;
  s.policy = policy;
  QueueSetup q;
  q.app = host::AppProfile::kv();
  q.ring_size = ring;
  q.pool_buffers = 2 * ring;  // larger than the ring, leaving buffers for replies
  s.queues.push_back(q);
  FlowSetup f;
  f.spec.id = 1;
  f.spec.packet_bytes = 512;
  f.spec.message_bytes = 512;
  f.spec.traffic.rate_gbps = gbps;
  s.flows.push_back(f);
  if (rdma_gbps > 0) {
    QueueSetup r;
    r.kind = nic::QueueKind::kRdma;
    r.app = host::AppProfile::rdma_sink();
    r.mr_bytes = 256ULL << 20;
    s.queues.push_back(r);
    s.queues[0].pool_buffers = 4096;
    FlowSetup b;
    b.spec.id = 2;
    b.spec.packet_bytes = 4096;  // RoCE 4 KB MTU
    b.spec.message_bytes = rdma_msg;
    b.spec.traffic.rate_gbps = rdma_gbps;
    b.queue = 1;
    s.flows.push_back(b);
  }
  return s;
}

}  // namespace

TEST(Ceio, PlentyOfCreditsBehavesLikeBaseline) {
  const Results base = run(kv(10, IoPolicy::kBaseline));
  const Results ceio = run(kv(10, IoPolicy::kCeio));
  EXPECT_EQ(ceio.ceio_total_credits, 3072);  // [CEIO] Eq. (1): 6 MB / 2 KB
  EXPECT_EQ(ceio.ceio.slow_packets, 0u);
  EXPECT_EQ(ceio.drops_nic, 0u);
  EXPECT_EQ(ceio.reordered, 0u);
  EXPECT_NEAR(ceio.goodput_gbps, base.goodput_gbps, 0.1);
  EXPECT_TRUE(ceio.ceio_credits_conserved);
}

class CeioSlowPath : public ::testing::TestWithParam<policy::CeioConfig::Recv> {};

TEST_P(CeioSlowPath, ScarceCreditsUseSlowPathWithoutLossOrReordering) {
  // With a single credit nearly every packet goes through on-NIC memory. The
  // elastic buffer must neither drop nor reorder ([CEIO] Sec. 4.2, phase exclusivity).
  Scenario s = kv(10, IoPolicy::kCeio);
  s.ceio.total_credits = 1;
  s.ceio.recv = GetParam();
  const Results r = run(s);
  EXPECT_GT(r.ceio.slow_packets, r.sent_packets / 2);
  EXPECT_GT(r.ceio.drained_packets, r.sent_packets / 2);
  EXPECT_EQ(r.reordered, 0u);
  EXPECT_EQ(r.drops_nic + r.drops_switch + r.ceio.nic_mem_drops, 0u);
  EXPECT_NEAR(r.goodput_gbps, 10.0, 0.3);
  EXPECT_TRUE(r.ceio_credits_conserved);
}

INSTANTIATE_TEST_SUITE_P(RecvModes, CeioSlowPath,
                         ::testing::Values(policy::CeioConfig::Recv::kSync, policy::CeioConfig::Recv::kAsync));

TEST(Ceio, FlowsMoveBetweenPathsAndStayInOrder) {
  // Overload makes the flow exhaust its credits, fall to the slow path, and be
  // re-enabled by NIC-driven drains (pause, drain, re-enable) many times.
  const Results r = run(kv(26, IoPolicy::kCeio, 4096));
  EXPECT_GE(r.ceio.to_slow, 2u);
  EXPECT_GE(r.ceio.to_fast, 2u);
  EXPECT_GT(r.ceio.upgraded_packets, 0u);
  EXPECT_EQ(r.reordered, 0u);
  EXPECT_EQ(r.drops_nic, 0u);
  EXPECT_TRUE(r.ceio_credits_conserved);
}

TEST(Ceio, SlowPathAddsLatency) {
  // [CEIO] Table 3: the slow path costs extra PCIe crossings and NIC memory.
  Scenario fast = kv(5, IoPolicy::kCeio);
  Scenario slow = kv(5, IoPolicy::kCeio);
  slow.ceio.total_credits = 1;
  const Results f = run(fast), sl = run(slow);
  EXPECT_GT(sl.latency.mean_us, f.latency.mean_us + 0.5);
  EXPECT_LT(sl.latency.mean_us, f.latency.mean_us + 5.0);
}

TEST(Ceio, SlowPathIsSlowForSmallMessagesOnly) {
  // [CEIO] Fig. 11: forced onto the slow path, throughput is far below the fast
  // path for small messages (NIC-core bound) and catches up at 4 KB.
  auto sat = [](std::uint32_t pkt, std::int64_t credits) {
    Scenario s = ceio_testbed();
    s.warmup = 500 * kMicrosecond;
    s.measure = 500 * kMicrosecond;
    s.policy = IoPolicy::kCeio;
    s.ceio.total_credits = credits;
    QueueSetup q;
    q.app = host::AppProfile::echo();
    q.buffer_bytes = 4096;
    q.ring_size = 1024;
    q.pool_buffers = 8192;
    s.queues.push_back(q);
    FlowSetup f;
    f.spec.id = 1;
    f.spec.packet_bytes = pkt;
    f.spec.message_bytes = pkt;
    f.spec.traffic.rate_gbps = 50;
    s.flows.push_back(f);
    return find_saturation(s, 0.001, 5).offered_gbps;
  };
  EXPECT_LT(sat(512, 1), 0.5 * sat(512, 0));
  EXPECT_GT(sat(4096, 1), 0.78 * sat(4096, 0));
}

TEST(Ceio, AsyncFetchBeatsSyncWhenSlowPathIsBusy) {
  // [CEIO] Sec. 4.2: async_recv overlaps slow-path fetches with packet processing.
  Scenario a = kv(26, IoPolicy::kCeio), b = kv(26, IoPolicy::kCeio);
  a.ceio.total_credits = b.ceio.total_credits = 4;
  a.ceio.recv = policy::CeioConfig::Recv::kAsync;
  b.ceio.recv = policy::CeioConfig::Recv::kSync;
  const Results async_r = run(a), sync_r = run(b);
  EXPECT_GT(async_r.goodput_gbps, 1.1 * sync_r.goodput_gbps);
}

TEST(Ceio, OverloadKeepsMoreUnreadDataInTheLlc) {
  // [CEIO] Fig. 9: when the backlog would outgrow the DDIO ways, credits divert
  // the excess to on-NIC memory, raising the CPU hit rate without drops.
  const Results base = run(kv(26, IoPolicy::kBaseline, 4096));
  const Results ceio = run(kv(26, IoPolicy::kCeio, 4096));
  EXPECT_GT(ceio.queues[0].llc_read_hit_rate, base.queues[0].llc_read_hit_rate + 0.15);
  EXPECT_LT(ceio.cache.leaky_evictions, base.cache.leaky_evictions);
  EXPECT_GE(ceio.goodput_gbps, base.goodput_gbps);
  EXPECT_EQ(ceio.drops_nic, 0u);
  EXPECT_EQ(ceio.reordered, 0u);
}

TEST(Ceio, EquationOneOverestimatesCapacityForSmallPackets) {
  // Finding (docs/spec.md Sec. 5.4): C_total = DDIO bytes / buffer size assumes buffers
  // pack the LLC perfectly. With 512 B packets in 2 KB buffers, Intel set indexing
  // puts every packet on a quarter of the sets, so 3072 credits overfill the DDIO
  // ways and fewer credits do better. Motivates footprint-aware credits (CEIO-W).
  Scenario eq1 = kv(26, IoPolicy::kCeio, 4096), quarter = kv(26, IoPolicy::kCeio, 4096);
  quarter.ceio.total_credits = 768;
  const Results a = run(eq1), b = run(quarter);
  EXPECT_GT(b.queues[0].llc_read_hit_rate, a.queues[0].llc_read_hit_rate + 0.2);
  EXPECT_GT(b.goodput_gbps, a.goodput_gbps);
}

TEST(Ceio, ProtectsCpuInvolvedFlowFromRdmaTraffic) {
  // [CEIO] Sec. 4.1: lazy release (credits return only when a whole 4 MB message is
  // processed) exhausts the RDMA flow's credits and moves it to the slow path, so
  // the backlogged CPU-involved flow keeps its LLC and is not dropped.
  // Throughput under mixed flows ([CEIO] Fig. 10 / Table 4) depends on congestion
  // control reacting to CEIO's slow-path signal and is covered by the ceio_mixed
  // experiment (with DCTCP). Without CC this overload never clears and the KV flow
  // ends up sharing NIC cores with RDMA drains on the slow path.
  const Results base = run(kv(26, IoPolicy::kBaseline, 1024, 150));
  const Results ceio = run(kv(26, IoPolicy::kCeio, 1024, 150));
  EXPECT_GT(ceio.queues[0].llc_read_hit_rate, base.queues[0].llc_read_hit_rate + 0.3);
  EXPECT_GT(base.flows[0].drops_nic, 0u);
  EXPECT_GT(ceio.flows[1].slow_packets, ceio.flows[1].delivered_packets / 2);
  EXPECT_NEAR(ceio.flows[1].goodput_gbps, 150.0, 7.5);
  EXPECT_EQ(ceio.flows[0].drops_nic, 0u);
  EXPECT_EQ(ceio.reordered, 0u);
  EXPECT_TRUE(ceio.ceio_credits_conserved);
}

TEST(Ceio, CounterPollingOvershootsIdealDetectionDoesNot) {
  // NIC cores learn about consumption by polling rule hit counters, so a flow can
  // overdraw its credits before its rule is switched to on-NIC memory.
  // A 20 us control-loop pass (e.g. ~2000 flows at 10 ns each) lets a 26 Gbps flow
  // overdraw by tens of credits.
  Scenario polled = kv(26, IoPolicy::kCeio, 4096), ideal = kv(26, IoPolicy::kCeio, 4096);
  polled.ceio.poll_base = 20 * kMicrosecond;
  ideal.ceio.poll_base = 20 * kMicrosecond;
  ideal.ceio.ideal_detection = true;
  const Results p = run(polled), i = run(ideal);
  EXPECT_LT(p.ceio.min_credit_balance, -10);
  EXPECT_GE(i.ceio.min_credit_balance, 0);
  EXPECT_EQ(i.reordered, 0u);
}

TEST(Ceio, IdleFlowIsDeactivated) {
  // [CEIO] Sec. 4.1 Q3: a flow idle past the timeout returns its credits.
  Scenario s = kv(5, IoPolicy::kCeio);
  s.warmup = 0;
  FlowSetup second = s.flows[0];
  second.spec.id = 3;
  second.spec.stop = 500 * kMicrosecond;
  s.flows.push_back(second);
  s.ceio.idle_timeout = 100 * kMicrosecond;
  const Results r = run(s);
  EXPECT_EQ(r.ceio.deactivations, 1u);
  EXPECT_EQ(r.reordered, 0u);
  EXPECT_TRUE(r.ceio_credits_conserved);
}

}  // namespace ceio::exp
