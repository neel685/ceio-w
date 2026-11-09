#include "ceio/net/link.hpp"

namespace ceio::net {

SwitchPort::SwitchPort(Simulator& sim, const LinkConfig& cfg, Deliver deliver)
    : sim_(sim), cfg_(cfg), deliver_(std::move(deliver)), occ_(sim.now()) {}

void SwitchPort::enqueue(Packet p) {
  if (queued_bytes_ + p.bytes > cfg_.buffer_bytes) {
    ++stats_.dropped;
    if (on_drop_) on_drop_(p);
    return;
  }
  if (queued_bytes_ >= cfg_.ecn_threshold_bytes) {
    p.ecn = true;
    ++stats_.ecn_marked;
  }
  ++stats_.enqueued;
  queued_bytes_ += p.bytes;
  occ_.set(sim_.now(), static_cast<double>(queued_bytes_));
  queue_.push_back(p);
  if (!busy_) transmit_next();
}

void SwitchPort::transmit_next() {
  if (queue_.empty()) {
    busy_ = false;
    return;
  }
  busy_ = true;
  const Tick tx = transfer_time(queue_.front().bytes + kWireOverhead, cfg_.rate_gbps);
  sim_.schedule_in(tx, [this] {
    Packet p = queue_.front();
    queue_.pop_front();
    queued_bytes_ -= p.bytes;
    occ_.set(sim_.now(), static_cast<double>(queued_bytes_));
    ++stats_.delivered;
    stats_.bytes_delivered += p.bytes;
    sim_.schedule_in(cfg_.propagation, [this, p]() mutable {
      p.nic_arrival = sim_.now();
      deliver_(p);
    });
    transmit_next();
  });
}

}  // namespace ceio::net
