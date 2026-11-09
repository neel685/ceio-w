#pragma once

#include <cstdint>

namespace ceio {

/// Simulated time in picoseconds. 2^64 ps is ~213 days, far beyond any run.
using Tick = std::uint64_t;

inline constexpr Tick kPicosecond = 1;
inline constexpr Tick kNanosecond = 1'000;
inline constexpr Tick kMicrosecond = 1'000 * kNanosecond;
inline constexpr Tick kMillisecond = 1'000 * kMicrosecond;
inline constexpr Tick kSecond = 1'000 * kMillisecond;

/// Converts a (possibly fractional) nanosecond value to ticks, rounding to nearest.
constexpr Tick from_ns(double ns) { return static_cast<Tick>(ns * 1'000.0 + 0.5); }
constexpr Tick from_us(double us) { return from_ns(us * 1'000.0); }

constexpr double to_ns(Tick t) { return static_cast<double>(t) / 1'000.0; }
constexpr double to_us(Tick t) { return static_cast<double>(t) / 1'000'000.0; }
constexpr double to_sec(Tick t) { return static_cast<double>(t) / 1e12; }

/// Time to serialise `bytes` over a channel of `gbps` (10^9 bit/s), rounded to nearest ps.
constexpr Tick transfer_time(std::uint64_t bytes, double gbps) {
  return static_cast<Tick>(static_cast<double>(bytes) * 8'000.0 / gbps + 0.5);
}

/// Bandwidth in Gbps achieved by moving `bytes` in `duration`.
constexpr double gbps(std::uint64_t bytes, Tick duration) {
  return duration == 0 ? 0.0 : static_cast<double>(bytes) * 8'000.0 / static_cast<double>(duration);
}

}  // namespace ceio
