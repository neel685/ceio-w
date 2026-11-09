#include "ceio/sim/rng.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace ceio {

namespace {

constexpr std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

std::uint64_t fnv1a(std::string_view s) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (char c : s) {
    h ^= static_cast<std::uint8_t>(c);
    h *= 0x100000001b3ULL;
  }
  return h;
}

}  // namespace

std::uint64_t splitmix64(std::uint64_t& state) {
  state += 0x9e3779b97f4a7c15ULL;
  return mix64(state);
}

std::uint64_t mix64(std::uint64_t z) {
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

Rng::Rng(std::uint64_t seed) : seed_(seed) {
  std::uint64_t st = seed;
  for (auto& w : s_) w = splitmix64(st);
}

Rng Rng::derive(std::string_view name) const { return Rng(mix64(seed_ ^ fnv1a(name))); }

std::uint64_t Rng::next_u64() {
  const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
  const std::uint64_t t = s_[1] << 17;
  s_[2] ^= s_[0];
  s_[3] ^= s_[1];
  s_[1] ^= s_[2];
  s_[0] ^= s_[3];
  s_[2] ^= t;
  s_[3] = rotl(s_[3], 45);
  return result;
}

double Rng::uniform01() { return static_cast<double>(next_u64() >> 11) * 0x1.0p-53; }

std::uint64_t Rng::uniform_int(std::uint64_t lo, std::uint64_t hi) {
  if (lo > hi) throw std::invalid_argument("Rng::uniform_int: lo > hi");
  const std::uint64_t span = hi - lo;
  if (span == UINT64_MAX) return next_u64();
  const std::uint64_t range = span + 1;
  // Rejection sampling: discard the final partial block to remove modulo bias.
  const std::uint64_t limit = UINT64_MAX - (UINT64_MAX % range);
  std::uint64_t x;
  do {
    x = next_u64();
  } while (x >= limit);
  return lo + x % range;
}

double Rng::exponential(double rate) {
  if (!(rate > 0.0)) throw std::invalid_argument("Rng::exponential: rate must be > 0");
  return -std::log1p(-uniform01()) / rate;
}

double Rng::normal(double mean, double stddev) {
  double u1 = uniform01();
  while (u1 <= 0.0) u1 = uniform01();
  const double u2 = uniform01();
  return mean + stddev * std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * std::numbers::pi * u2);
}

std::uint64_t Rng::poisson(double mean) {
  if (mean < 0.0) throw std::invalid_argument("Rng::poisson: mean must be >= 0");
  if (mean == 0.0) return 0;
  if (mean < 30.0) {
    // Knuth's multiplication method.
    const double l = std::exp(-mean);
    std::uint64_t k = 0;
    double p = uniform01();
    while (p > l) {
      ++k;
      p *= uniform01();
    }
    return k;
  }
  // Hörmann (1993) transformed rejection with squeeze (PTRS).
  const double slam = std::sqrt(mean);
  const double loglam = std::log(mean);
  const double b = 0.931 + 2.53 * slam;
  const double a = -0.059 + 0.02483 * b;
  const double invalpha = 1.1239 + 1.1328 / (b - 3.4);
  const double vr = 0.9277 - 3.6224 / (b - 2.0);
  while (true) {
    const double u = uniform01() - 0.5;
    const double v = uniform01();
    const double us = 0.5 - std::fabs(u);
    const double k = std::floor((2.0 * a / us + b) * u + mean + 0.43);
    if (us >= 0.07 && v <= vr) return static_cast<std::uint64_t>(k);
    if (k < 0.0 || (us < 0.013 && v > us)) continue;
    if (std::log(v) + std::log(invalpha) - std::log(a / (us * us) + b) <=
        -mean + k * loglam - std::lgamma(k + 1.0)) {
      return static_cast<std::uint64_t>(k);
    }
  }
}

}  // namespace ceio
