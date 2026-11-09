#include "ceio/sim/stats.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace ceio {

namespace {
// Highest bucket index + 1: shifts up to 63 - kSubBits, plus the linear range.
constexpr std::size_t kNumBuckets = (64 - Histogram::kSubBits + 1) * Histogram::kSub;
}  // namespace

Histogram::Histogram() : buckets_(kNumBuckets, 0) {}

std::size_t Histogram::bucket_of(std::uint64_t v) {
  if (v < 2 * kSub) return static_cast<std::size_t>(v);
  const int msb = 63 - std::countl_zero(v);
  const int shift = msb - kSubBits;  // v >> shift lies in [kSub, 2*kSub)
  return static_cast<std::size_t>(static_cast<std::uint64_t>(shift + 1) * kSub + ((v >> shift) - kSub));
}

std::uint64_t Histogram::bucket_lower(std::size_t b) {
  if (b < 2 * kSub) return b;
  const std::size_t shift = b / kSub - 1;
  const std::uint64_t sub = b % kSub + kSub;
  return sub << shift;
}

std::uint64_t Histogram::bucket_upper(std::size_t b) {
  if (b < 2 * kSub) return b;
  const std::size_t shift = b / kSub - 1;
  const std::uint64_t lower = bucket_lower(b);
  const std::uint64_t width = 1ULL << shift;
  return lower + (width - 1);  // never overflows: lower <= 2^64 - width
}

void Histogram::record(std::uint64_t value, std::uint64_t count) {
  if (count == 0) return;
  buckets_[bucket_of(value)] += count;
  count_ += count;
  min_ = std::min(min_, value);
  max_ = std::max(max_, value);
  sum_ += static_cast<double>(value) * static_cast<double>(count);
}

void Histogram::merge(const Histogram& other) {
  for (std::size_t i = 0; i < buckets_.size(); ++i) buckets_[i] += other.buckets_[i];
  count_ += other.count_;
  if (other.count_) {
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
  }
  sum_ += other.sum_;
}

void Histogram::reset() {
  std::fill(buckets_.begin(), buckets_.end(), 0);
  count_ = 0;
  min_ = std::numeric_limits<std::uint64_t>::max();
  max_ = 0;
  sum_ = 0.0;
}

std::uint64_t Histogram::quantile(double q) const {
  if (q < 0.0 || q > 1.0) throw std::invalid_argument("Histogram::quantile: q outside [0,1]");
  if (count_ == 0) return 0;
  const auto rank = std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(count_))));
  std::uint64_t seen = 0;
  for (std::size_t b = 0; b < buckets_.size(); ++b) {
    seen += buckets_[b];
    if (seen >= rank) return std::clamp(bucket_upper(b), min_, max_);
  }
  return max_;
}

void TimeWeighted::set(Tick now, double value) {
  if (now < last_) throw std::invalid_argument("TimeWeighted::set: time went backwards");
  area_ += value_ * static_cast<double>(now - last_);
  last_ = now;
  value_ = value;
  max_ = std::max(max_, value);
}

double TimeWeighted::mean(Tick now) const {
  if (now <= start_) return value_;
  double area = area_;
  if (now > last_) area += value_ * static_cast<double>(now - last_);
  return area / static_cast<double>(now - start_);
}

}  // namespace ceio
