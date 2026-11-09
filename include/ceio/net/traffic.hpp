#pragma once

#include <cstdint>
#include <functional>

#include "ceio/net/packet.hpp"
#include "ceio/sim/rng.hpp"
#include "ceio/sim/simulator.hpp"

namespace ceio::net {

/// How a flow's offered load varies over time.
struct TrafficSpec {
  enum class Pattern {
    kConstant,      ///< evenly spaced messages at `rate_gbps`
    kPoisson,       ///< exponential message inter-arrivals with mean rate `rate_gbps`
    kMicroburst,    ///< `burst_packets` back-to-back every `burst_period` ([Toasty] Sec. 2)
    kPoissonRate,   ///< constant rate redrawn every `epoch`, rate ~ Poisson(mean) ([Toasty] Sec. 2)
  };
  Pattern pattern = Pattern::kConstant;
  double rate_gbps = 10.0;              ///< mean offered load (frame bytes, excluding wire overhead)
  std::uint32_t burst_packets = 2048;
  Tick burst_period = kMillisecond;
  Tick epoch = 5 * kMillisecond;
  /// Granularity of the Poisson rate draw for kPoissonRate, in Gbps.
  double rate_quantum_gbps = 1.0;
};

/// One sender. Messages are split into `packet_bytes` frames that leave
/// back-to-back; message starts follow the traffic pattern.
struct FlowSpec {
  FlowId id = 0;
  std::uint32_t packet_bytes = 1024;
  std::uint32_t message_bytes = 1024;  ///< >= packet_bytes for multi-packet messages
  TrafficSpec traffic;
  Tick start = 0;
  Tick stop = ~Tick{0};
  std::uint32_t packets_per_message() const;
};

/// Open-loop traffic generator for one flow. It emits packets through `sink`
/// (normally the switch port). Its rate can be changed at run time, which is
/// the hook congestion control uses.
class TrafficSource {
 public:
  using Sink = std::function<void(Packet)>;

  TrafficSource(Simulator& sim, const FlowSpec& spec, Rng rng, Sink sink);
  void start();

  /// Overrides the current sending rate (Gbps); used by congestion control.
  void set_rate_gbps(double gbps);
  double rate_gbps() const { return rate_gbps_; }
  const FlowSpec& spec() const { return spec_; }
  std::uint64_t packets_sent() const { return seq_; }

  /// Deterministic unique id: flow in the top 24 bits, per-flow sequence below.
  static std::uint64_t packet_id(FlowId flow, std::uint64_t seq) {
    return (static_cast<std::uint64_t>(flow) << 40) | seq;
  }

 private:
  void emit_message();
  void schedule_next_message();
  void redraw_rate();
  Tick gap_for_message() const;

  Simulator& sim_;
  FlowSpec spec_;
  Rng rng_;
  Sink sink_;
  double rate_gbps_;
  std::uint64_t seq_ = 0;
  std::uint64_t msg_ = 0;
};

}  // namespace ceio::net
