#include "ceio/nic/nic.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

namespace ceio::nic {
namespace {

hw::CacheConfig cache() {
  hw::CacheConfig c;
  c.ways = 12;
  c.ddio_ways = 6;
  c.size_bytes = 2048ULL * 12 * 64;
  return c;
}

struct Rig {
  Simulator sim;
  hw::MemorySystem mem{cache(), hw::DramConfig{}};
  hw::PcieHost pcie{sim, mem};
  Nic nic;
  explicit Rig(NicConfig cfg = {}) : nic(sim, pcie, cfg) {}

  QueueId desc_queue(std::uint32_t ring = 8) {
    RxQueueConfig q;
    q.ring_size = ring;
    q.cq_entries = ring;
    q.cq_base = 0x100000;
    const QueueId id = nic.add_queue(q);
    nic.set_rule(1, SteeringRule{SteerAction::kHostQueue, id, 0});
    return id;
  }
};

net::Packet pkt(std::uint64_t seq, std::uint32_t bytes = 1024, net::FlowId flow = 1) {
  net::Packet p;
  p.flow = flow;
  p.seq = seq;
  p.bytes = bytes;
  return p;
}

}  // namespace

TEST(Nic, DeliversIntoPostedBuffersInOrder) {
  Rig r;
  const QueueId q = r.desc_queue();
  r.nic.post_buffers(q, {0x200000, 0x201000, 0x202000});
  int notified = 0;
  r.nic.queue(q).set_notify([&] { ++notified; });
  for (std::uint64_t i = 0; i < 3; ++i) r.nic.receive(pkt(i));
  r.sim.run();
  auto& cq = r.nic.queue(q).completions();
  ASSERT_EQ(cq.size(), 3u);
  EXPECT_EQ(notified, 3);
  for (std::uint64_t i = 0; i < 3; ++i) {
    EXPECT_EQ(cq[i].pkt.seq, i);
    EXPECT_EQ(cq[i].buf_addr, 0x200000 + i * 0x1000);
    EXPECT_EQ(cq[i].cqe_addr, 0x100000 + i * 64);
  }
  EXPECT_GT(cq[0].completed, r.nic.config().pipeline_latency + r.pcie.config().one_way_latency);
  EXPECT_EQ(r.nic.queue(q).posted(), 0u);
  EXPECT_EQ(r.nic.rx_buffer_used(), 0u);
  EXPECT_EQ(r.nic.rule(1)->hits, 3u);
  // Packet data reached the LLC through DDIO.
  EXPECT_TRUE(r.mem.cache().contains(0x200000 / 64));
}

TEST(Nic, PacketsWaitForDescriptorsThenProceed) {
  Rig r;
  const QueueId q = r.desc_queue();
  for (std::uint64_t i = 0; i < 4; ++i) r.nic.receive(pkt(i));
  r.sim.run();
  EXPECT_EQ(r.nic.queue(q).completions().size(), 0u);
  EXPECT_EQ(r.nic.rx_buffer_used(), 4u * 1024);
  EXPECT_GT(r.nic.queue(q).stats().waited_for_descriptor, 0u);
  r.nic.post_buffers(q, {0x200000, 0x201000, 0x202000, 0x203000});
  r.sim.run();
  ASSERT_EQ(r.nic.queue(q).completions().size(), 4u);
  EXPECT_EQ(r.nic.queue(q).completions().back().pkt.seq, 3u);
  EXPECT_EQ(r.nic.rx_buffer_used(), 0u);
}

TEST(Nic, DropsWhenOnChipBufferIsFull) {
  NicConfig cfg;
  cfg.rx_buffer_bytes = 4096;
  Rig r(cfg);
  r.desc_queue();
  int drops = 0;
  r.nic.set_drop_hook([&](const net::Packet&) { ++drops; });
  for (std::uint64_t i = 0; i < 10; ++i) r.nic.receive(pkt(i));  // no descriptors posted
  r.sim.run();
  EXPECT_EQ(drops, 6);
  EXPECT_EQ(r.nic.stats().drops_buffer_full, 6u);
  EXPECT_DOUBLE_EQ(r.nic.rx_buffer_occupancy().max(), 4096.0);
}

TEST(Nic, StarvedQueueDropsOnlyItsOwnPackets) {
  NicConfig cfg;
  cfg.queue_wait_bytes = 4096;
  Rig r(cfg);
  const QueueId starved = r.desc_queue();
  RxQueueConfig other;
  other.ring_size = 64;
  other.cq_entries = 64;
  other.cq_base = 0x300000;
  const QueueId ok = r.nic.add_queue(other);
  r.nic.set_rule(2, SteeringRule{SteerAction::kHostQueue, ok, 0});
  std::vector<std::uint64_t> bufs;
  for (std::uint64_t i = 0; i < 64; ++i) bufs.push_back(0x400000 + i * 4096);
  r.nic.post_buffers(ok, bufs);
  for (std::uint64_t i = 0; i < 64; ++i) {
    r.nic.receive(pkt(i, 1024, 1));  // queue `starved` has no descriptors
    r.nic.receive(pkt(i, 1024, 2));
  }
  r.sim.run();
  EXPECT_EQ(r.nic.stats().drops_no_descriptor, 60u);  // 4 KB of headroom = 4 packets
  EXPECT_EQ(r.nic.stats().drops_buffer_full, 0u);
  EXPECT_EQ(r.nic.queue(ok).completions().size(), 64u);
  EXPECT_EQ(r.nic.queue(starved).completions().size(), 0u);
}

TEST(Nic, SteeringActions) {
  Rig r;
  const QueueId q = r.desc_queue();
  r.nic.set_rule(2, SteeringRule{SteerAction::kDrop, q, 0});
  r.nic.set_rule(3, SteeringRule{SteerAction::kNicMemory, q, 0});
  std::vector<net::Packet> slow;
  r.nic.receive(pkt(0, 64, 2));
  r.nic.receive(pkt(0, 64, 3));  // no slow-path handler yet -> dropped
  r.nic.receive(pkt(0, 64, 99)); // no rule
  r.sim.run();
  EXPECT_EQ(r.nic.stats().drops_rule, 2u);
  EXPECT_EQ(r.nic.stats().drops_no_rule, 1u);
  r.nic.set_slow_path([&](net::Packet p, QueueId) { slow.push_back(p); });
  r.nic.receive(pkt(1, 64, 3));
  r.sim.run();
  ASSERT_EQ(slow.size(), 1u);
  EXPECT_EQ(r.nic.stats().to_nic_memory, 2u);
  EXPECT_EQ(r.nic.rule(3)->hits, 2u);
}

TEST(Nic, DeliverToHostBypassesSteeringAndOnChipBuffer) {
  Rig r;
  const QueueId q = r.desc_queue();
  r.nic.deliver_to_host(pkt(0), q);  // no descriptor yet: waits in order
  r.sim.run();
  EXPECT_EQ(r.nic.queue(q).completions().size(), 0u);
  EXPECT_EQ(r.nic.rx_buffer_used(), 0u);  // not counted against the on-chip buffer
  r.nic.post_buffers(q, {0x200000});
  r.sim.run();
  EXPECT_EQ(r.nic.queue(q).completions().size(), 1u);
  EXPECT_EQ(r.nic.rule(1)->hits, 0u);
}

TEST(Nic, RdmaCompletesOncePerMessage) {
  Rig r;
  RxQueueConfig qc;
  qc.kind = QueueKind::kRdma;
  qc.cq_entries = 16;
  qc.cq_base = 0x100000;
  qc.mr_base = 0x4000000;
  qc.mr_bytes = 1 << 20;
  const QueueId q = r.nic.add_queue(qc);
  r.nic.set_rule(5, SteeringRule{SteerAction::kHostQueue, q, 0});
  for (std::uint32_t m = 0; m < 2; ++m) {
    for (std::uint32_t i = 0; i < 4; ++i) {
      net::Packet p = pkt(m * 4 + i, 1024, 5);
      p.msg_id = m;
      p.msg_pkts = 4;
      p.msg_index = i;
      p.msg_last = i == 3;
      r.nic.receive(p);
    }
  }
  r.sim.run();
  auto& cq = r.nic.queue(q).completions();
  ASSERT_EQ(cq.size(), 2u);
  EXPECT_EQ(cq[0].bytes, 4096u);
  EXPECT_EQ(cq[0].buf_addr, 0x4000000u);
  EXPECT_EQ(cq[1].buf_addr, 0x4000000u + 4096);
  EXPECT_EQ(r.nic.queue(q).stats().dma_packets, 8u);
  EXPECT_THROW(r.nic.post_buffers(q, {0}), std::logic_error);
}

TEST(Nic, RingOverflowIsALogicError) {
  Rig r;
  const QueueId q = r.desc_queue(2);
  EXPECT_THROW(r.nic.post_buffers(q, {1, 2, 3}), std::logic_error);
}

}  // namespace ceio::nic
