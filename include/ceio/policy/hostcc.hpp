#pragma once

#include <cstdint>

#include "ceio/host/core.hpp"
#include "ceio/hw/pcie.hpp"
#include "ceio/nic/nic.hpp"
#include "ceio/sim/simulator.hpp"

namespace ceio::policy {

struct HostCcConfig {
  Tick sample_period = 1 * kMicrosecond;
  /// IIO occupancy above which the host is congested (HostCC's I_T threshold).
  std::uint64_t iio_threshold_bytes = 8 * 1024;
  /// Host-local response: throttle the memory co-runner (Intel MBA) while
  /// congested, restore it gradually afterwards.
  bool host_local_response = true;
  double mba_decrease = 0.5;   ///< multiplicative cut per congested sample
  double mba_recover = 0.05;   ///< fraction of the original rate restored per clear sample
  double mba_floor = 0.1;      ///< never throttle below this fraction
};

struct HostCcStats {
  std::uint64_t samples = 0;
  std::uint64_t congested_samples = 0;
  std::uint64_t marked_packets = 0;
};

/// HostCC ([CEIO] [1], Agarwal et al. SIGCOMM'23) as configured in [CEIO] Sec. 6.1:
/// samples IIO buffer occupancy; while it exceeds the threshold the host is
/// congested, so (network response) arriving packets are ECN-marked, which
/// makes DCTCP senders slow down, and (host-local response) the co-runner's
/// memory bandwidth is throttled. The signal is reactive: occupancy rises only
/// after the memory subsystem is already backed up.
class HostCcController {
 public:
  HostCcController(Simulator& sim, nic::Nic& nic, hw::PcieHost& pcie, host::MemoryHog* hog,
                   const HostCcConfig& cfg);
  void start();
  bool congested() const { return congested_; }
  const HostCcStats& stats() const { return stats_; }
  void reset_stats() { stats_ = {}; }

 private:
  void sample();
  Simulator& sim_;
  nic::Nic& nic_;
  hw::PcieHost& pcie_;
  host::MemoryHog* hog_;
  HostCcConfig cfg_;
  double hog_target_ = 0;
  bool congested_ = false;
  HostCcStats stats_;
};

}  // namespace ceio::policy
