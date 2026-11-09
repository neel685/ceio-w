#include "ceio/sim/rng.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <stdexcept>

namespace ceio {

TEST(Rng, SameSeedSameSequence) {
  Rng a(42), b(42);
  for (int i = 0; i < 1000; ++i) ASSERT_EQ(a.next_u64(), b.next_u64());
}

TEST(Rng, DifferentSeedsDiffer) {
  Rng a(1), b(2);
  int same = 0;
  for (int i = 0; i < 1000; ++i) same += a.next_u64() == b.next_u64();
  EXPECT_EQ(same, 0);
}

TEST(Rng, GoldenValuesArePortable) {
  // Pins the generator so results are identical across platforms and releases.
  // If this test changes, every published result must be regenerated.
  Rng r(12345);
  EXPECT_EQ(r.next_u64(), 0xbe6a36374160d49bULL);
  EXPECT_EQ(r.next_u64(), 0x214aaa0637a688c6ULL);
  EXPECT_EQ(r.next_u64(), 0xf69d16de9954d388ULL);
  EXPECT_EQ(Rng(12345).derive("nic").next_u64(), 0x5cff9977b4012514ULL);
  std::uint64_t st = 0;
  EXPECT_EQ(splitmix64(st), 0xe220a8397b1dcdafULL);  // reference SplitMix64 output for seed 0
}

TEST(Rng, DerivedStreamsAreStableAndIndependent) {
  Rng root(7);
  Rng a1 = root.derive("nic"), a2 = root.derive("nic"), b = root.derive("cpu");
  const auto x = a1.next_u64();
  EXPECT_EQ(x, a2.next_u64());
  EXPECT_NE(x, b.next_u64());
  // Deriving must not consume from the parent.
  Rng fresh(7);
  EXPECT_EQ(root.next_u64(), fresh.next_u64());
}

TEST(Rng, Uniform01InRange) {
  Rng r(3);
  double sum = 0;
  for (int i = 0; i < 100000; ++i) {
    const double u = r.uniform01();
    ASSERT_GE(u, 0.0);
    ASSERT_LT(u, 1.0);
    sum += u;
  }
  EXPECT_NEAR(sum / 100000, 0.5, 0.01);
}

TEST(Rng, UniformIntBoundsAndBalance) {
  Rng r(9);
  std::array<int, 6> counts{};
  constexpr int kN = 60000;
  for (int i = 0; i < kN; ++i) {
    const auto v = r.uniform_int(10, 15);
    ASSERT_GE(v, 10u);
    ASSERT_LE(v, 15u);
    ++counts[v - 10];
  }
  // Chi-square with 5 degrees of freedom; 20.5 is the p = 0.001 critical value.
  double chi2 = 0;
  for (int c : counts) chi2 += (c - kN / 6.0) * (c - kN / 6.0) / (kN / 6.0);
  EXPECT_LT(chi2, 20.5);
  EXPECT_EQ(r.uniform_int(5, 5), 5u);
  EXPECT_THROW(r.uniform_int(6, 5), std::invalid_argument);
}

TEST(Rng, ExponentialMean) {
  Rng r(11);
  double sum = 0;
  for (int i = 0; i < 200000; ++i) sum += r.exponential(4.0);
  EXPECT_NEAR(sum / 200000, 0.25, 0.005);
  EXPECT_THROW(r.exponential(0.0), std::invalid_argument);
}

TEST(Rng, NormalMoments) {
  Rng r(13);
  double sum = 0, sq = 0;
  constexpr int kN = 200000;
  for (int i = 0; i < kN; ++i) {
    const double x = r.normal(3.0, 2.0);
    sum += x;
    sq += x * x;
  }
  const double mean = sum / kN;
  EXPECT_NEAR(mean, 3.0, 0.02);
  EXPECT_NEAR(std::sqrt(sq / kN - mean * mean), 2.0, 0.02);
}

class PoissonMoments : public ::testing::TestWithParam<double> {};

TEST_P(PoissonMoments, MeanAndVarianceMatch) {
  const double lambda = GetParam();
  Rng r(17);
  double sum = 0, sq = 0;
  constexpr int kN = 100000;
  for (int i = 0; i < kN; ++i) {
    const auto k = static_cast<double>(r.poisson(lambda));
    sum += k;
    sq += k * k;
  }
  const double mean = sum / kN;
  const double var = sq / kN - mean * mean;
  EXPECT_NEAR(mean, lambda, 0.02 * lambda + 0.02);
  EXPECT_NEAR(var, lambda, 0.05 * lambda + 0.05);
}

// Covers both the Knuth branch (< 30) and the PTRS branch (>= 30).
INSTANTIATE_TEST_SUITE_P(SmallAndLarge, PoissonMoments, ::testing::Values(0.5, 4.0, 29.0, 30.0, 250.0, 5000.0));

TEST(Rng, PoissonEdgeCases) {
  Rng r(1);
  EXPECT_EQ(r.poisson(0.0), 0u);
  EXPECT_THROW(r.poisson(-1.0), std::invalid_argument);
}

}  // namespace ceio
