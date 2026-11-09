#pragma once

#include <cstdint>
#include <memory>

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
};

/// Default driver behaviour: keep the descriptor ring full.
class KeepFullRefill final : public RefillPolicy {
 public:
  std::uint32_t to_post(const RefillContext& ctx) override {
    return ctx.ring_capacity > ctx.ring_available ? ctx.ring_capacity - ctx.ring_available : 0;
  }
};

}  // namespace ceio::host
