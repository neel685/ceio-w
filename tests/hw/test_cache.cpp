#include "ceio/hw/cache.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace ceio::hw {
namespace {

CacheConfig make(std::uint64_t sets, std::uint32_t ways, std::uint32_t ddio, bool hash = false) {
  CacheConfig c;
  c.ways = ways;
  c.ddio_ways = ddio;
  c.line_bytes = 64;
  c.size_bytes = sets * ways * 64;
  c.hash_index = hash;
  return c;
}

}  // namespace

TEST(LinesOf, Ranges) {
  EXPECT_EQ(lines_of(0, 64, 64).count, 1u);
  EXPECT_EQ(lines_of(0, 65, 64).count, 2u);
  EXPECT_EQ(lines_of(63, 2, 64).count, 2u);
  EXPECT_EQ(lines_of(128, 1500, 64).first, 2u);
  EXPECT_EQ(lines_of(128, 1500, 64).count, 24u);  // ceil(1500/64)
  EXPECT_EQ(lines_of(100, 0, 64).count, 0u);
}

TEST(CacheConfig, ValidationRejectsBadShapes) {
  EXPECT_THROW(SetAssocCache(make(4, 4, 5)), std::invalid_argument);  // ddio > ways
  EXPECT_THROW(SetAssocCache(make(4, 4, 0)), std::invalid_argument);
  CacheConfig odd = make(4, 4, 2);
  odd.size_bytes += 64;
  EXPECT_THROW(SetAssocCache{odd}, std::invalid_argument);
  CacheConfig line = make(4, 4, 2);
  line.line_bytes = 48;
  EXPECT_THROW(SetAssocCache{line}, std::invalid_argument);
  EXPECT_EQ(make(1024, 12, 6).ddio_bytes(), 1024u * 6 * 64);
}

TEST(Cache, CpuReadMissThenHit) {
  SetAssocCache c(make(16, 4, 2, true));
  EXPECT_FALSE(c.cpu_read(7).hit);
  EXPECT_TRUE(c.cpu_read(7).hit);
  EXPECT_EQ(c.stats().cpu_read_misses, 1u);
  EXPECT_EQ(c.stats().cpu_read_hits, 1u);
  EXPECT_FALSE(c.is_dirty(7));
}

TEST(Cache, LruEvictsLeastRecentlyUsed) {
  SetAssocCache c(make(1, 4, 2));
  for (std::uint64_t l = 0; l < 4; ++l) c.cpu_read(l);
  c.cpu_read(0);  // 1 is now LRU
  c.cpu_read(4);
  EXPECT_TRUE(c.contains(0));
  EXPECT_FALSE(c.contains(1));
  EXPECT_TRUE(c.contains(4));
  EXPECT_EQ(c.stats().evictions, 1u);
  EXPECT_EQ(c.stats().writebacks, 0u);  // clean eviction
}

TEST(Cache, IoWritesAllocateOnlyIntoDdioWays) {
  SetAssocCache c(make(1, 4, 1));
  for (std::uint64_t l = 0; l < 4; ++l) c.cpu_read(l);  // lines 0..3 fill ways 0..3
  c.io_write(10);  // may only evict from way 3 (holding line 3)
  EXPECT_FALSE(c.contains(3));
  c.io_write(11);  // evicts 10, which no CPU ever read
  EXPECT_FALSE(c.contains(10));
  EXPECT_TRUE(c.contains(0) && c.contains(1) && c.contains(2) && c.contains(11));
  EXPECT_EQ(c.stats().io_write_allocs, 2u);
  EXPECT_EQ(c.stats().leaky_evictions, 1u);
  EXPECT_EQ(c.stats().writebacks, 1u);  // line 10 was dirty
}

TEST(Cache, IoWriteHitOutsideDdioWaysUpdatesInPlace) {
  SetAssocCache c(make(1, 4, 1));
  c.cpu_read(0);  // lands in way 0, a non-DDIO way
  const CacheAccess a = c.io_write(0);
  EXPECT_TRUE(a.hit);
  EXPECT_EQ(c.stats().io_write_hits, 1u);
  EXPECT_EQ(c.stats().io_write_hits_non_ddio, 1u);  // "DMA bloating"
  EXPECT_TRUE(c.is_dirty(0));
  EXPECT_EQ(c.io_unread_lines(), 1u);
}

TEST(Cache, DirtyEvictionOfDeadBufferIsUnnecessaryWriteback) {
  SetAssocCache c(make(1, 2, 2));
  c.io_write(0);
  c.cpu_read(0);
  c.io_write(1);
  c.cpu_read(1);
  c.mark_dead(0);  // driver recycled the buffer holding line 0
  const CacheAccess a = c.io_write(2);
  ASSERT_TRUE(a.writeback_line.has_value());
  EXPECT_EQ(*a.writeback_line, 0u);
  EXPECT_EQ(c.stats().writebacks, 1u);
  EXPECT_EQ(c.stats().dead_writebacks, 1u);
  EXPECT_EQ(c.stats().leaky_evictions, 0u);
}

TEST(Cache, WriteClearsDeadFlag) {
  SetAssocCache c(make(1, 2, 2));
  c.io_write(0);
  c.mark_dead(0);
  c.io_write(0);  // buffer reused: data live again
  c.io_write(1);
  c.io_write(2);  // evicts 0
  EXPECT_EQ(c.stats().writebacks, 1u);
  EXPECT_EQ(c.stats().dead_writebacks, 0u);
}

TEST(Cache, IoUnreadTracking) {
  SetAssocCache c(make(8, 4, 2, true));
  c.io_write(5);
  c.io_write(6);
  EXPECT_EQ(c.io_unread_lines(), 2u);
  c.cpu_read(5);
  EXPECT_EQ(c.io_unread_lines(), 1u);
  c.invalidate(6);
  EXPECT_EQ(c.io_unread_lines(), 0u);
  EXPECT_FALSE(c.contains(6));
}

TEST(Cache, IoReadNeverAllocates) {
  SetAssocCache c(make(8, 4, 2, true));
  EXPECT_FALSE(c.io_read(3).hit);
  EXPECT_FALSE(c.contains(3));
  c.cpu_write(3);
  EXPECT_TRUE(c.io_read(3).hit);
  EXPECT_EQ(c.stats().cpu_write_misses, 1u);
}

// ---------------------------------------------------------------------------
// Emergent behaviours the rest of the simulator relies on.
// ---------------------------------------------------------------------------

namespace {

/// Cycles DMA writes over a ring of `ring_lines` lines (FIFO buffer reuse), with
/// the CPU consuming each line immediately and recycling it. Returns the I/O
/// write hit rate of the final pass.
double ring_io_hit_rate(SetAssocCache& c, std::uint64_t ring_lines, int passes) {
  for (int p = 0; p < passes; ++p) {
    if (p == passes - 1) c.reset_stats();
    for (std::uint64_t l = 0; l < ring_lines; ++l) {
      c.io_write(l);
      c.cpu_read(l);
      c.mark_dead(l);
    }
  }
  return c.stats().io_write_hit_rate();
}

}  // namespace

TEST(CacheEmergent, FootprintWithinDdioCapacityAlwaysHits) {
  // 1024 sets x 16 ways, 2 DDIO ways -> 2048 DDIO lines (128 KB).
  SetAssocCache c(make(1024, 16, 2));
  EXPECT_DOUBLE_EQ(ring_io_hit_rate(c, 2048, 3), 1.0);
  EXPECT_EQ(c.stats().writebacks, 0u);
}

TEST(CacheEmergent, FootprintBeyondDdioCapacityCausesDeadWritebacks) {
  // [Toasty] Sec. 1 "unnecessary writebacks": a FIFO ring twice the DDIO capacity
  // never finds its buffers warm, and every DMA evicts a dead dirty line.
  SetAssocCache c(make(1024, 16, 2));
  EXPECT_DOUBLE_EQ(ring_io_hit_rate(c, 4096, 3), 0.0);
  EXPECT_EQ(c.stats().dead_writebacks, c.stats().writebacks);
  EXPECT_EQ(c.stats().writebacks, 4096u);
}

TEST(CacheEmergent, HashedIndexDegradesGracefully) {
  // With sliced-LLC hashing, capacity effects are statistical, not a cliff.
  SetAssocCache small(make(1024, 16, 2, true));
  EXPECT_GT(ring_io_hit_rate(small, 512, 3), 0.85);  // 25 % of DDIO capacity
  SetAssocCache big(make(1024, 16, 2, true));
  EXPECT_LT(ring_io_hit_rate(big, 8192, 3), 0.05);  // 400 % of DDIO capacity
}

TEST(CacheEmergent, LeakyDmaAppearsWhenBacklogExceedsDdio) {
  // [CEIO] Sec. 2.2 / [Toasty] Sec. 1: the CPU lags the NIC by `lag` lines. Lines are
  // evicted before being read once the unread backlog exceeds the DDIO ways.
  auto run = [](std::uint64_t lag) {
    SetAssocCache c(make(1024, 16, 2));
    constexpr std::uint64_t kLines = 64 * 1024;  // large ring: no reuse effects
    std::uint64_t misses = 0;
    for (std::uint64_t l = 0; l < kLines + lag; ++l) {
      if (l < kLines) c.io_write(l);
      if (l >= lag) misses += c.cpu_read(l - lag).hit ? 0U : 1U;
    }
    return std::pair{c.stats().leaky_evictions, misses};
  };
  const auto [leaky_small, miss_small] = run(512);  // a quarter of the DDIO capacity
  EXPECT_EQ(leaky_small, 0u);
  EXPECT_EQ(miss_small, 0u);
  const auto [leaky_big, miss_big] = run(4096);  // twice the DDIO capacity
  EXPECT_GT(leaky_big, 60'000u);
  EXPECT_GT(miss_big, 60'000u);
}

TEST(CacheEmergent, ConsumedLinesCompeteWithUnreadOnes) {
  // A CPU read makes a consumed line most-recently-used, so with lag == sets each
  // set's two DDIO ways hold one unread and one just-consumed line, and LRU evicts
  // the unread one. Leaky DMA can therefore start below the nominal DDIO capacity
  // (here at 50 %) unless consumed buffers are recycled into the DMA stream quickly,
  // which is the behaviour Toasty and CEIO-W exploit.
  SetAssocCache c(make(1024, 16, 2));
  constexpr std::uint64_t kLag = 1024, kLines = 16 * 1024;
  for (std::uint64_t l = 0; l < kLines + kLag; ++l) {
    if (l < kLines) c.io_write(l);
    if (l >= kLag) c.cpu_read(l - kLag);
  }
  EXPECT_GT(c.stats().leaky_evictions, kLines / 4);  // ~44 % of lines with this mapping
}

}  // namespace ceio::hw
