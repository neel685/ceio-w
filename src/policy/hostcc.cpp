#include "ceio/policy/hostcc.hpp"

#include <algorithm>

namespace ceio::policy {

HostCcController::HostCcController(Simulator& sim, nic::Nic& nic, hw::PcieHost& pcie, host::MemoryHog* hog,
                                   const HostCcConfig& cfg)
    : sim_(sim), nic_(nic), pcie_(pcie), hog_(hog), cfg_(cfg), hog_target_(hog ? hog->gbytes_per_s() : 0) {}

void HostCcController::start() {
  nic_.set_ingress_hook([this](net::Packet& p) {
    if (!congested_) return;
    if (!p.ecn) ++stats_.marked_packets;
    p.ecn = true;
  });
  sim_.schedule_in(cfg_.sample_period, [this] { sample(); });
}

void HostCcController::sample() {
  ++stats_.samples;
  congested_ = pcie_.iio_occupancy_bytes() > cfg_.iio_threshold_bytes;
  if (congested_) ++stats_.congested_samples;
  if (hog_ && cfg_.host_local_response && hog_target_ > 0) {
    const double now = hog_->gbytes_per_s();
    const double next = congested_ ? std::max(now * cfg_.mba_decrease, hog_target_ * cfg_.mba_floor)
                                   : std::min(now + hog_target_ * cfg_.mba_recover, hog_target_);
    if (next != now) hog_->set_gbytes_per_s(next);
  }
  sim_.schedule_in(cfg_.sample_period, [this] { sample(); });
}

}  // namespace ceio::policy
