#include "ceio/hw/pcie.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

namespace ceio::hw {
namespace {

CacheConfig big_cache() {
  CacheConfig c;
  c.ways = 12;
  c.ddio_ways = 6;
  c.size_bytes = 16384ULL * 12 * 64;  // 12 MB
  return c;
}

struct Rig {
  Simulator sim;
  MemorySystem mem;
  PcieHost pcie;
  explicit Rig(PcieConfig p = {}, CacheConfig c = big_cache(), DramConfig d = {})
      : mem(c, d), pcie(sim, mem, p) {}
};

}  // namespace

TEST(Pcie, SingleSmallWriteLatency) {
  Rig r;
  Tick done = 0;
  r.pcie.dma_write(0, 64, [&] { done = r.sim.now(); });
  r.sim.run();
  const Tick expect = transfer_time(64 + 24, 504.0) + 300 * kNanosecond + r.mem.timing().io_write_alloc;
  EXPECT_EQ(done, expect);
  EXPECT_EQ(r.pcie.stats().write_tlps, 1u);
}

TEST(Pcie, WritesSplitOnMaxPayloadBoundaries) {
  Rig r;
  r.pcie.dma_write(200, 1500, [] {});  // 200..1700: 56 + 5x256 + 164
  r.sim.run();
  EXPECT_EQ(r.pcie.stats().write_tlps, 7u);
  EXPECT_EQ(r.pcie.stats().dma_write_bytes, 1500u);
}

TEST(Pcie, CompletionsArriveInIssueOrder) {
  Rig r;
  std::vector<int> order;
  for (int i = 0; i < 200; ++i)
    r.pcie.dma_write(static_cast<std::uint64_t>(i) * 2048, i % 3 == 0 ? 1500 : 64,
                     [&, i] { order.push_back(i); });
  r.sim.run();
  ASSERT_EQ(order.size(), 200u);
  for (int i = 0; i < 200; ++i) EXPECT_EQ(order[static_cast<std::size_t>(i)], i);
}

TEST(Pcie, LargeTransferApproachesLinkEfficiency) {
  PcieConfig p;
  p.iio_credit_bytes = 1 << 20;  // credits never limit
  Rig r(p);
  constexpr int kChunks = 2048;  // 4 MB in 2 KB DMAs
  for (int i = 0; i < kChunks; ++i) r.pcie.dma_write(static_cast<std::uint64_t>(i) * 2048, 2048, [] {});
  r.sim.run();
  const double achieved = gbps(static_cast<std::uint64_t>(kChunks) * 2048, r.sim.now());
  const double ideal = 504.0 * 256.0 / (256.0 + 24.0);  // payload efficiency
  EXPECT_LT(achieved, ideal);
  EXPECT_GT(achieved, ideal * 0.97);  // within 3 % (one-way latency amortised)
}

TEST(Pcie, ScarceCreditsLimitThroughput) {
  PcieConfig p;
  p.iio_credit_bytes = 512;  // two TLPs in flight
  Rig r(p);
  for (int i = 0; i < 256; ++i) r.pcie.dma_write(static_cast<std::uint64_t>(i) * 2048, 2048, [] {});
  r.sim.run();
  const double achieved = gbps(256ULL * 2048, r.sim.now());
  // Bandwidth-delay bound: 512 B per ~600 ns credit round trip ~= 6.8 Gbps.
  EXPECT_LT(achieved, 8.0);
  EXPECT_GT(r.pcie.stats().credit_stalls, 0u);
}

TEST(Pcie, DramPressureBacksUpIio) {
  // A tiny LLC forces every DMA line to evict a dirty line; slow DRAM then makes
  // the IIO the bottleneck. IIO occupancy (HostCC's congestion signal) must rise.
  CacheConfig tiny;
  tiny.ways = 2;
  tiny.ddio_ways = 1;
  tiny.size_bytes = 64ULL * 2 * 64;
  DramConfig slow;
  slow.channels = 1;
  slow.channel_gbytes_per_s = 4.0;
  slow.write_queue_lines = 4;

  Rig pressured(PcieConfig{}, tiny, slow);
  Rig relaxed;
  for (Rig* r : {&pressured, &relaxed}) {
    for (int i = 0; i < 512; ++i) r->pcie.dma_write(static_cast<std::uint64_t>(i) * 2048, 2048, [] {});
    r->sim.run();
  }
  EXPECT_GT(pressured.pcie.iio_occupancy().max(), 32.0 * 1024);  // over half the credits
  EXPECT_LT(relaxed.pcie.iio_occupancy().max(), 2.0 * 1024);
  EXPECT_GT(pressured.pcie.stats().credit_stalls, 0u);
  EXPECT_GT(pressured.sim.now(), 5 * relaxed.sim.now());
  EXPECT_EQ(pressured.pcie.iio_occupancy_bytes(), 0u);  // drained at the end
  EXPECT_EQ(pressured.pcie.credits_in_use(), 0u);
}

TEST(Pcie, DmaReadRoundTrip) {
  Rig r;
  Tick done = 0;
  r.pcie.dma_read(4096, 64, [&] { done = r.sim.now(); });
  r.sim.run();
  // Request up + DRAM miss + completion down: ~600 ns of wire latency plus ~93 ns of DRAM.
  EXPECT_GT(done, 690 * kNanosecond);
  EXPECT_LT(done, 700 * kNanosecond);
  EXPECT_EQ(r.pcie.stats().dma_reads, 1u);
}

TEST(Pcie, MmioWriteAndReadLatency) {
  Rig r;
  Tick arrived = 0;
  r.pcie.mmio_write(8, [&] { arrived = r.sim.now(); });
  r.sim.run();
  EXPECT_EQ(arrived, transfer_time(8 + 24, 504.0) + 300 * kNanosecond);
  EXPECT_GT(r.pcie.mmio_read_latency(), 600 * kNanosecond);
  EXPECT_LT(r.pcie.mmio_read_latency(), 1000 * kNanosecond);  // [CEIO] Sec. 3: up to 1000 ns
}

TEST(Pcie, RejectsInvalidUse) {
  Rig r;
  EXPECT_THROW(r.pcie.dma_write(0, 0, [] {}), std::invalid_argument);
  EXPECT_THROW(r.pcie.dma_read(0, 0, [] {}), std::invalid_argument);
  PcieConfig bad;
  bad.max_payload = 300;
  EXPECT_THROW(Rig{bad}, std::invalid_argument);
}

}  // namespace ceio::hw
