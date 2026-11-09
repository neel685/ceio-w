#include "ceio/hw/memory_system.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace ceio::hw {
namespace {

CacheConfig cache_cfg(std::uint64_t sets = 1024, std::uint32_t ways = 12, std::uint32_t ddio = 6) {
  CacheConfig c;
  c.ways = ways;
  c.ddio_ways = ddio;
  c.size_bytes = sets * ways * 64;
  return c;
}

}  // namespace

TEST(MemorySystem, CpuReadMissThenHitLatency) {
  MemorySystem m(cache_cfg(), DramConfig{});
  const Tick svc = m.dram().line_service_time();
  EXPECT_EQ(m.cpu_read(0, 4096, 64), svc + 90 * kNanosecond);
  EXPECT_EQ(m.cpu_read(1'000'000, 4096, 64), 1'000'000 + 20 * kNanosecond);
}

TEST(MemorySystem, DdioWrittenDataHitsForCpu) {
  MemorySystem m(cache_cfg(), DramConfig{});
  for (std::uint64_t l = 0; l < 16; ++l) m.io_write_line(0, 64 + l);  // 1 KB packet at 4 KB
  // 16 hits with 10 fill buffers: lines 10..15 wait for lines 0..5. Line 15 issues
  // when line 5 completes (5 gaps + 20 ns) and finishes 20 ns later.
  EXPECT_EQ(m.cpu_read(0, 64 * 64, 1024), 5 * m.timing().cpu_issue_gap + 2 * 20 * kNanosecond);
  EXPECT_EQ(m.cache().stats().cpu_read_misses, 0u);
}

TEST(MemorySystem, MemoryLevelParallelismBoundsMissLatency) {
  auto time_for = [](std::uint32_t mlp) {
    MemTiming t;
    t.cpu_mlp = mlp;
    MemorySystem m(cache_cfg(), DramConfig{}, t);
    return m.cpu_read(0, 0, 1024);  // 16 cold lines
  };
  const Tick t1 = time_for(1), t4 = time_for(4), t16 = time_for(16);
  EXPECT_GT(t1, t4);
  EXPECT_GT(t4, t16);
  EXPECT_GT(t1, 16 * 90 * kNanosecond);   // fully serialised
  EXPECT_LT(t16, 120 * kNanosecond);      // fully overlapped
}

TEST(MemorySystem, IoWriteTimingHitVsAllocate) {
  MemorySystem m(cache_cfg(), DramConfig{});
  EXPECT_EQ(m.io_write_line(0, 9), m.timing().io_write_alloc);
  EXPECT_EQ(m.io_write_line(100, 9), 100 + m.timing().io_write_update);
}

TEST(MemorySystem, IoAllocationWithDirtyVictimWritesBack) {
  MemorySystem m(cache_cfg(1, 1, 1), DramConfig{});
  m.io_write_line(0, 0);
  m.io_write_line(0, 1);  // evicts dirty line 0
  EXPECT_EQ(m.dram().stats().writes, 1u);
  EXPECT_EQ(m.cache().stats().writebacks, 1u);
}

TEST(MemorySystem, IoCommitStallsWhenDramWriteQueueIsFull) {
  DramConfig d;
  d.channels = 1;
  d.channel_gbytes_per_s = 1.0;  // 64 ns per line
  d.write_queue_lines = 2;
  MemorySystem m(cache_cfg(1, 1, 1), d);
  Tick t = 0;
  for (std::uint64_t l = 0; l < 20; ++l) t = m.io_write_line(t, l);  // every commit writes back
  // After the queue fills, commits proceed at DRAM write bandwidth (~64 ns/line).
  EXPECT_GT(t, 14 * 64 * kNanosecond);
}

TEST(MemorySystem, MarkDeadCoversWholeRange) {
  MemorySystem m(cache_cfg(), DramConfig{});
  for (std::uint64_t l = 0; l < 4; ++l) m.io_write_line(0, l);
  m.mark_dead(0, 256);
  EXPECT_EQ(m.cache().io_unread_lines(), 0u);
}

TEST(MemorySystem, RejectsMismatchedLineSizes) {
  DramConfig d;
  d.line_bytes = 128;
  EXPECT_THROW(MemorySystem(cache_cfg(), d), std::invalid_argument);
}

}  // namespace ceio::hw
