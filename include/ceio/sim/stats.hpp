#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include "ceio/sim/time.hpp"

namespace ceio {

/// Log-linear histogram for non-negative integers (e.g. latencies in ps).
///
/// Values below 2*kSub are recorded exactly; above that, each power-of-two
/// range is split into kSub buckets, so the relative error is below 1/kSub
/// (< 0.8 % with kSub = 128). Memory use is fixed (~60 KB) regardless of range.
class Histogram {
 public:
  static constexpr int kSubBits = 7;
  static constexpr std::uint64_t kSub = 1ULL << kSubBits;

  Histogram();

  void record(std::uint64_t value, std::uint64_t count = 1);
  void merge(const Histogram& other);
  void reset();

  std::uint64_t count() const { return count_; }
  std::uint64_t min() const { return count_ ? min_ : 0; }
  std::uint64_t max() const { return max_; }
  double mean() const { return count_ ? sum_ / static_cast<double>(count_) : 0.0; }
  /// Value at quantile q in [0, 1]: the smallest recorded bucket covering
  /// ceil(q * count) samples, reported as that bucket's upper bound, clamped
  /// to [min, max]. Returns 0 for an empty histogram.
  std::uint64_t quantile(double q) const;

  static std::size_t bucket_of(std::uint64_t v);
  static std::uint64_t bucket_lower(std::size_t b);
  static std::uint64_t bucket_upper(std::size_t b);

 private:
  std::vector<std::uint64_t> buckets_;
  std::uint64_t count_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_ = 0;
  double sum_ = 0.0;
};

/// Time-weighted average of a piecewise-constant signal (e.g. queue occupancy).
class TimeWeighted {
 public:
  explicit TimeWeighted(Tick start = 0, double initial = 0.0) : start_(start), last_(start), value_(initial), max_(initial) {}

  /// Records that the signal changes to `value` at time `now` (now must not decrease).
  void set(Tick now, double value);
  void add(Tick now, double delta) { set(now, value_ + delta); }

  double value() const { return value_; }
  double max() const { return max_; }
  /// Average over [start, now].
  double mean(Tick now) const;

 private:
  Tick start_;
  Tick last_;
  double value_;
  double max_;
  double area_ = 0.0;
};

}  // namespace ceio
