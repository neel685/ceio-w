#include "ceio/sim/time.hpp"

#include <gtest/gtest.h>

namespace ceio {

TEST(Time, UnitConstants) {
  EXPECT_EQ(kMicrosecond, 1'000'000u);
  EXPECT_EQ(kSecond, 1'000'000'000'000u);
  EXPECT_EQ(from_ns(1.5), 1'500u);
  EXPECT_EQ(from_us(2.0), 2'000'000u);
  EXPECT_DOUBLE_EQ(to_ns(from_ns(41.8)), 41.8);
}

TEST(Time, TransferTime) {
  // 1500 B at 200 Gbps = 60 ns.
  EXPECT_EQ(transfer_time(1500, 200.0), from_ns(60.0));
  // [CEIO] Sec. 1: a 1024 B packet at 200 Gbps leaves 41.8 ns per packet, i.e. the
  // frame plus 20 B of preamble and inter-frame gap.
  EXPECT_NEAR(to_ns(transfer_time(1024 + 20, 200.0)), 41.8, 0.05);
}

TEST(Time, GbpsRoundTrip) {
  const Tick t = transfer_time(1'000'000, 100.0);
  EXPECT_NEAR(gbps(1'000'000, t), 100.0, 1e-6);
  EXPECT_EQ(gbps(123, 0), 0.0);
}

}  // namespace ceio
