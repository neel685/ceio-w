#include "ceio/net/congestion.hpp"

#include <algorithm>

namespace ceio::net {

DctcpController::DctcpController(Simulator& sim, TrafficSource& src, const DctcpConfig& cfg)
    : sim_(sim),
      src_(src),
      cfg_(cfg),
      demand_(src.spec().traffic.rate_gbps),
      additive_gbps_(gbps(static_cast<std::uint64_t>(cfg.additive_packets * src.spec().packet_bytes), cfg.rtt)) {}

void DctcpController::start() {
  sim_.schedule_in(cfg_.rtt, [this] { window(); });
}

void DctcpController::on_delivered(bool ecn) {
  sim_.schedule_in(cfg_.feedback_delay, [this, ecn] {
    ++acked_;
    if (ecn) ++marked_;
  });
}

void DctcpController::on_notification() {
  sim_.schedule_in(cfg_.feedback_delay, [this] {
    ++acked_;
    ++marked_;
  });
}

void DctcpController::on_lost() {
  sim_.schedule_in(cfg_.feedback_delay, [this] { ++lost_; });
}

void DctcpController::window() {
  const double rate = src_.rate_gbps();
  double next = rate;
  if (lost_ > 0) {
    next = rate * 0.5;
    alpha_ = std::min(1.0, (1 - cfg_.g) * alpha_ + cfg_.g);
    ++reductions_;
  } else if (marked_ > 0) {
    const double f = static_cast<double>(marked_) / static_cast<double>(std::max<std::uint64_t>(acked_, 1));
    alpha_ = (1 - cfg_.g) * alpha_ + cfg_.g * f;
    next = rate * (1 - alpha_ / 2);
    ++reductions_;
  } else {
    alpha_ = (1 - cfg_.g) * alpha_;
    if (acked_ > 0 || rate < demand_) next = rate + additive_gbps_;
  }
  src_.set_rate_gbps(std::clamp(next, cfg_.min_gbps, demand_));
  acked_ = marked_ = lost_ = 0;
  sim_.schedule_in(cfg_.rtt, [this] { window(); });
}

}  // namespace ceio::net
