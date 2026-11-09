#include "ceio/sim/stats.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "ceio/sim/rng.hpp"

namespace ceio {

TEST(Histogram, EmptyIsZero) {
  Histogram h;
  EXPECT_EQ(h.count(), 0u);
  EXPECT_EQ(h.quantile(0.99), 0u);
  EXPECT_EQ(h.min(), 0u);
  EXPECT_DOUBLE_EQ(h.mean(), 0.0);
}

TEST(Histogram, SmallValuesAreExact) {
  Histogram h;
  for (std::uint64_t v = 1; v <= 100; ++v) h.record(v);
  EXPECT_EQ(h.quantile(0.5), 50u);
  EXPECT_EQ(h.quantile(0.99), 99u);
  EXPECT_EQ(h.quantile(1.0), 100u);
  EXPECT_EQ(h.quantile(0.0), 1u);
  EXPECT_DOUBLE_EQ(h.mean(), 50.5);
}

TEST(Histogram, BucketsAreContiguousAndMonotone) {
  for (std::size_t b = 1; b < 7000; ++b) {
    ASSERT_EQ(Histogram::bucket_lower(b), Histogram::bucket_upper(b - 1) + 1) << "bucket " << b;
    ASSERT_EQ(Histogram::bucket_of(Histogram::bucket_lower(b)), b);
    ASSERT_EQ(Histogram::bucket_of(Histogram::bucket_upper(b)), b);
  }
  EXPECT_NO_THROW(Histogram::bucket_of(UINT64_MAX));
}

TEST(Histogram, LargeValueRelativeErrorBelowOnePercent) {
  Rng r(5);
  Histogram h;
  std::vector<std::uint64_t> values;
  for (int i = 0; i < 100000; ++i) {
    const auto v = static_cast<std::uint64_t>(r.exponential(1.0 / 1e6)) + 1;  // ~1 us in ps
    values.push_back(v);
    h.record(v);
  }
  std::sort(values.begin(), values.end());
  for (double q : {0.5, 0.9, 0.99, 0.999}) {
    const auto exact = values[static_cast<std::size_t>(std::ceil(q * 100000)) - 1];
    const auto approx = h.quantile(q);
    EXPECT_GE(approx, exact);
    EXPECT_LE(static_cast<double>(approx), static_cast<double>(exact) * 1.008) << "q=" << q;
  }
}

TEST(Histogram, MergeEqualsCombinedRecording) {
  Histogram a, b, c;
  for (std::uint64_t v = 0; v < 1000; ++v) {
    (v % 2 ? a : b).record(v * 37);
    c.record(v * 37);
  }
  a.merge(b);
  EXPECT_EQ(a.count(), c.count());
  EXPECT_EQ(a.min(), c.min());
  EXPECT_EQ(a.max(), c.max());
  for (double q : {0.1, 0.5, 0.99}) EXPECT_EQ(a.quantile(q), c.quantile(q));
}

TEST(Histogram, InvalidQuantileThrows) {
  Histogram h;
  h.record(1);
  EXPECT_THROW(h.quantile(1.5), std::invalid_argument);
}

TEST(TimeWeighted, PiecewiseConstantMean) {
  TimeWeighted tw(0, 0.0);
  tw.set(10, 4.0);   // 0 over [0,10)
  tw.set(20, 2.0);   // 4 over [10,20)
  // 2 over [20,40)  -> area = 0 + 40 + 40 = 80 over 40
  EXPECT_DOUBLE_EQ(tw.mean(40), 2.0);
  EXPECT_DOUBLE_EQ(tw.max(), 4.0);
  tw.add(40, 1.0);
  EXPECT_DOUBLE_EQ(tw.value(), 3.0);
  EXPECT_THROW(tw.set(30, 1.0), std::invalid_argument);
}

}  // namespace ceio
