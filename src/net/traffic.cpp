#include "ceio/net/traffic.hpp"

#include <algorithm>
#include <stdexcept>

namespace ceio::net {

std::uint32_t FlowSpec::packets_per_message() const {
  return std::max<std::uint32_t>(1, (message_bytes + packet_bytes - 1) / packet_bytes);
}

Tick FlowSpec::next_active(Tick t) const {
  if (t >= stop) return ~Tick{0};
  if (active.empty()) return t;
  for (const auto& [on, off] : active) {
    if (t < on) return on;
    if (t < off) return t;
  }
  return ~Tick{0};
}

TrafficSource::TrafficSource(Simulator& sim, const FlowSpec& spec, Rng rng, Sink sink)
    : sim_(sim), spec_(spec), rng_(rng), sink_(std::move(sink)), rate_gbps_(spec.traffic.rate_gbps) {
  if (spec_.packet_bytes == 0) throw std::invalid_argument("FlowSpec: packet_bytes must be > 0");
  if (spec_.message_bytes < spec_.packet_bytes) spec_.message_bytes = spec_.packet_bytes;
  if (!(spec_.traffic.rate_gbps > 0.0)) throw std::invalid_argument("TrafficSpec: rate must be > 0");
}

void TrafficSource::start() {
  if (spec_.traffic.pattern == TrafficSpec::Pattern::kPoissonRate) {
    sim_.schedule_at(std::max(sim_.now(), spec_.start), [this] { redraw_rate(); });
  }
  sim_.schedule_at(std::max(sim_.now(), spec_.start), [this] { emit_message(); });
}

void TrafficSource::set_rate_gbps(double gbps) { rate_gbps_ = std::max(gbps, 1e-3); }

void TrafficSource::redraw_rate() {
  if (sim_.now() >= spec_.stop) return;
  const double q = spec_.traffic.rate_quantum_gbps;
  const double draw = static_cast<double>(rng_.poisson(spec_.traffic.rate_gbps / q)) * q;
  rate_gbps_ = std::max(draw, q * 0.01);  // a zero draw becomes a near-silent epoch
  sim_.schedule_in(spec_.traffic.epoch, [this] { redraw_rate(); });
}

Tick TrafficSource::gap_for_message() const {
  const std::uint64_t msg_bytes =
      static_cast<std::uint64_t>(spec_.packets_per_message()) * spec_.packet_bytes;
  return transfer_time(msg_bytes, rate_gbps_);
}

void TrafficSource::emit_message() {
  if (sim_.now() >= spec_.stop) return;
  const Tick resume = spec_.next_active(sim_.now());
  if (resume == ~Tick{0}) return;
  if (resume > sim_.now()) {  // off period: wake up when the flow turns on again
    sim_.schedule_at(resume, [this] { emit_message(); });
    return;
  }
  const auto& t = spec_.traffic;
  const std::uint32_t msgs =
      t.pattern == TrafficSpec::Pattern::kMicroburst
          ? std::max<std::uint32_t>(1, t.burst_packets / spec_.packets_per_message())
          : 1;
  for (std::uint32_t m = 0; m < msgs; ++m) {
    const std::uint32_t n = spec_.packets_per_message();
    for (std::uint32_t i = 0; i < n; ++i) {
      Packet p;
      p.id = packet_id(spec_.id, seq_);
      p.flow = spec_.id;
      p.bytes = spec_.packet_bytes;
      p.seq = seq_++;
      p.msg_id = msg_;
      p.msg_pkts = n;
      p.msg_index = i;
      p.msg_last = (i + 1 == n);
      p.sent = sim_.now();
      sink_(p);
    }
    ++msg_;
  }
  schedule_next_message();
}

void TrafficSource::schedule_next_message() {
  const auto& t = spec_.traffic;
  Tick gap = 0;
  switch (t.pattern) {
    case TrafficSpec::Pattern::kConstant:
    case TrafficSpec::Pattern::kPoissonRate:
      gap = gap_for_message();
      break;
    case TrafficSpec::Pattern::kPoisson: {
      const double mean_ps = static_cast<double>(gap_for_message());
      gap = static_cast<Tick>(rng_.exponential(1.0 / mean_ps));
      break;
    }
    case TrafficSpec::Pattern::kMicroburst:
      gap = t.burst_period;
      break;
  }
  sim_.schedule_in(std::max<Tick>(gap, 1), [this] { emit_message(); });
}

}  // namespace ceio::net
