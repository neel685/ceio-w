#pragma once

#include <cstdint>
#include <deque>
#include <functional>

#include "ceio/net/packet.hpp"
#include "ceio/sim/simulator.hpp"
#include "ceio/sim/stats.hpp"

namespace ceio::net {

struct LinkConfig {
  double rate_gbps = 200.0;
  Tick propagation = 500 * kNanosecond;
  std::uint64_t buffer_bytes = 4ULL << 20;   ///< switch egress buffer for the port
  std::uint64_t ecn_threshold_bytes = 200 * 1024;  ///< DCTCP-style marking threshold K
};

struct LinkStats {
  std::uint64_t enqueued = 0;
  std::uint64_t delivered = 0;
  std::uint64_t dropped = 0;
  std::uint64_t ecn_marked = 0;
  std::uint64_t bytes_delivered = 0;
};

/// The switch egress port facing the receiver: a FIFO with tail drop and ECN
/// marking, serialising frames onto the wire at line rate.
class SwitchPort {
 public:
  using Deliver = std::function<void(Packet)>;
  /// Called for every drop (e.g. to notify a congestion controller).
  using DropHook = std::function<void(const Packet&)>;

  SwitchPort(Simulator& sim, const LinkConfig& cfg, Deliver deliver);

  void enqueue(Packet p);
  void set_drop_hook(DropHook h) { on_drop_ = std::move(h); }

  std::uint64_t queued_bytes() const { return queued_bytes_; }
  const TimeWeighted& occupancy() const { return occ_; }
  const LinkStats& stats() const { return stats_; }
  void reset_stats() {
    stats_ = {};
    occ_ = TimeWeighted(sim_.now(), static_cast<double>(queued_bytes_));
  }
  const LinkConfig& config() const { return cfg_; }

 private:
  void transmit_next();

  Simulator& sim_;
  LinkConfig cfg_;
  Deliver deliver_;
  DropHook on_drop_;
  std::deque<Packet> queue_;
  std::uint64_t queued_bytes_ = 0;
  bool busy_ = false;
  TimeWeighted occ_;
  LinkStats stats_;
};

}  // namespace ceio::net
