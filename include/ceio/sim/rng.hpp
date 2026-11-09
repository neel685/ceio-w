#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ceio {

/// Deterministic, portable random number generator (xoshiro256**).
///
/// std:: distributions differ between standard libraries, so all
/// distributions are implemented here to keep results identical across
/// platforms. Each component should draw from its own named sub-stream
/// (see derive()) so that adding a component does not perturb the others.
class Rng {
 public:
  explicit Rng(std::uint64_t seed);

  /// Independent generator for a named sub-stream; same (seed, name) gives the same stream.
  Rng derive(std::string_view name) const;

  std::uint64_t next_u64();
  /// Uniform double in [0, 1) with 53 random bits.
  double uniform01();
  /// Uniform double in [lo, hi).
  double uniform(double lo, double hi) { return lo + (hi - lo) * uniform01(); }
  /// Unbiased uniform integer in [lo, hi] (inclusive). Requires lo <= hi.
  std::uint64_t uniform_int(std::uint64_t lo, std::uint64_t hi);
  bool bernoulli(double p) { return uniform01() < p; }
  /// Exponential with the given rate (mean = 1/rate).
  double exponential(double rate);
  /// Standard normal via Box–Muller (no cached second value, for simplicity).
  double normal(double mean, double stddev);
  /// Poisson with the given mean (Knuth for small means, Hörmann PTRS otherwise).
  std::uint64_t poisson(double mean);

  std::uint64_t seed() const { return seed_; }

 private:
  std::uint64_t seed_;
  std::array<std::uint64_t, 4> s_{};
};

/// SplitMix64 step; also used as a general-purpose 64-bit mixer.
std::uint64_t splitmix64(std::uint64_t& state);
/// Stateless 64-bit finaliser (good avalanche); used for hashing addresses.
std::uint64_t mix64(std::uint64_t x);

}  // namespace ceio
