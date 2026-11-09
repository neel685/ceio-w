#pragma once

#include <cstdint>
#include <memory>

#include <algorithm>

#include "ceio/sim/time.hpp"

namespace ceio::host {

/// What the driver knows when deciding how many buffers to post to the NIC.
struct RefillContext {
  std::uint32_t ring_capacity = 0;
  std::uint32_t ring_available = 0;   ///< N_avail: posted + in-flight to the NIC
  std::uint32_t pool_free = 0;
  std::uint32_t recycled_since_last = 0;  ///< N_FQ
  std::uint32_t dmaed_since_last = 0;     ///< N_RXQ
  Tick since_last = 0;
  double link_gbps = 0.0;
  std::uint32_t packet_bytes = 0;      ///< recent average packet size
};

/// Decides how many free buffers the driver hands to the NIC per iteration.
class RefillPolicy {
 public:
  virtual ~RefillPolicy() = default;
  virtual std::uint32_t to_post(const RefillContext& ctx) = 0;
  /// The driver actually posted `n` buffers (may be fewer than requested).
  virtual void on_posted(std::uint32_t /*n*/) {}
};

/// Default driver behaviour: keep the descriptor ring full.
class KeepFullRefill final : public RefillPolicy {
 public:
  std::uint32_t to_post(const RefillContext& ctx) override {
    return ctx.ring_capacity > ctx.ring_available ? ctx.ring_capacity - ctx.ring_available : 0;
  }
};

struct ToastyRefillConfig {
  double k = 10.0;               ///< reserve multiplier ([Toasty] Sec. 3.2, k = 10)
  double util_threshold = 0.5;   ///< link-utilisation threshold (50 %)
  /// Floor on available descriptors. Pseudocode 1 posts k * N_RXQ, which is 0
  /// before the first packet; a small floor lets traffic start.
  std::uint32_t min_reserve = 64;
};

/// [Toasty] Pseudocode 1 (verified against the PDF): keep about k times the
/// last iteration's DMA count available, refilling with recently recycled
/// (cache-warm, given a LIFO pool) buffers when the link is lightly loaded and
/// pulling in colder buffers only when utilisation is high.
class ToastyRefill final : public RefillPolicy {
 public:
  explicit ToastyRefill(const ToastyRefillConfig& cfg = {}) : cfg_(cfg) {}
  std::uint32_t to_post(const RefillContext& ctx) override {
    const auto target = static_cast<std::uint32_t>(cfg_.k * ctx.dmaed_since_last);
    const double rate = ctx.since_last > 0
                            ? gbps(static_cast<std::uint64_t>(ctx.dmaed_since_last) * ctx.packet_bytes, ctx.since_last)
                            : 0.0;
    const bool high = rate >= cfg_.util_threshold * ctx.link_gbps;
    std::uint32_t fill;
    if (ctx.ring_available < target) {
      fill = high ? target - ctx.ring_available : std::min(target, ctx.recycled_since_last);  // lines 6-10
    } else {
      fill = high ? std::min(target, ctx.recycled_since_last) : 0;  // lines 11-15
    }
    if (ctx.ring_available + fill < cfg_.min_reserve) fill = cfg_.min_reserve - ctx.ring_available;
    return fill;
  }

 private:
  ToastyRefillConfig cfg_;
};

}  // namespace ceio::host
