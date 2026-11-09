#include "ceio/hw/dram.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>

namespace ceio::hw {

TEST(Dram, IdleReadLatencyIsServicePlusIdleLatency) {
  Dram d(DramConfig{});
  const Tick svc = d.line_service_time();
  EXPECT_NEAR(to_ns(svc), 64.0 / 19.2, 0.001);  // 3.33 ns per line per channel
  const DramAccess a = d.read(1000, 42);
  EXPECT_EQ(a.accepted, 1000 + svc);
  EXPECT_EQ(a.completed, 1000 + svc + 90 * kNanosecond);
  EXPECT_EQ(d.stats().reads, 1u);
  EXPECT_EQ(d.stats().bytes_read, 64u);
}

TEST(Dram, SameChannelRequestsQueue) {
  Dram d(DramConfig{});
  const Tick svc = d.line_service_time();
  DramAccess last{};
  for (int i = 0; i < 10; ++i) last = d.read(0, 7);  // same line -> same channel
  EXPECT_EQ(last.accepted, 10 * svc);
}

TEST(Dram, AggregateBandwidthApproachesChannelSum) {
  DramConfig cfg;
  Dram d(cfg);
  constexpr std::uint64_t kLines = 80'000;
  Tick end = 0;
  for (std::uint64_t l = 0; l < kLines; ++l) end = std::max(end, d.read(0, l).accepted);
  const double achieved_gbytes = static_cast<double>(kLines * 64) / to_ns(end);
  const double peak = cfg.channels * cfg.channel_gbytes_per_s;
  EXPECT_LE(achieved_gbytes, peak * 1.0001);
  EXPECT_GT(achieved_gbytes, peak * 0.95);
}

TEST(Dram, PostedWritesBlockOnlyWhenWriteQueueIsFull) {
  DramConfig cfg;
  cfg.channels = 1;
  cfg.write_queue_lines = 4;
  Dram d(cfg);
  const Tick svc = d.line_service_time();
  for (int i = 0; i < 5; ++i) EXPECT_EQ(d.write(0, 1).admitted, 0u) << i;  // backlog <= 4 lines
  EXPECT_EQ(d.write(0, 1).admitted, svc);  // 6th write waits one slot
  EXPECT_EQ(d.stats().writes, 6u);
  EXPECT_EQ(d.stats().bytes_written, 6u * 64);
}

TEST(Dram, ValidatesConfig) {
  DramConfig bad;
  bad.channels = 0;
  EXPECT_THROW(Dram{bad}, std::invalid_argument);
  bad = {};
  bad.channel_gbytes_per_s = 0;
  EXPECT_THROW(Dram{bad}, std::invalid_argument);
}

}  // namespace ceio::hw
