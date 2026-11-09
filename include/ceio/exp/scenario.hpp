#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ceio/host/app.hpp"
#include "ceio/host/buffer_pool.hpp"
#include "ceio/host/core.hpp"
#include "ceio/hw/memory_system.hpp"
#include "ceio/hw/pcie.hpp"
#include "ceio/net/link.hpp"
#include "ceio/net/traffic.hpp"
#include "ceio/nic/nic.hpp"
#include "ceio/policy/ceio.hpp"

namespace ceio::exp {

/// One receive queue served by one dedicated core (RSS-style 1:1 mapping).
struct QueueSetup {
  nic::QueueKind kind = nic::QueueKind::kDescriptor;
  std::uint32_t ring_size = 4096;
  std::uint32_t pool_buffers = 16384;     ///< UMEM / mempool size (kDescriptor)
  std::uint32_t buffer_bytes = 2048;
  host::PoolOrder order = host::PoolOrder::kFifo;
  std::uint64_t mr_bytes = 64ULL << 20;   ///< registered region (kRdma)
  host::AppProfile app = host::AppProfile::echo();
  host::CoreConfig core;
};

struct FlowSetup {
  net::FlowSpec spec;
  std::uint32_t queue = 0;  ///< index into Scenario::queues
};

/// Memory-intensive co-runner ([Toasty] Sec. 4.3); disabled when bandwidth is 0.
struct PressureSetup {
  double gbytes_per_s = 0.0;
  std::uint64_t region_bytes = 512ULL << 20;
  double write_fraction = 0.3;
};

/// Receive-path I/O management scheme under test.
enum class IoPolicy {
  kBaseline,  ///< legacy DDIO datapath (DPDK / AF_XDP defaults)
  kCeio,      ///< [CEIO]: credit-based flow control + elastic on-NIC buffering
};

/// Complete, self-contained description of one simulation run.
struct Scenario {
  std::string name = "custom";
  std::uint64_t seed = 1;
  Tick warmup = 2 * kMillisecond;
  Tick measure = 4 * kMillisecond;

  hw::CacheConfig cache;
  hw::DramConfig dram;
  hw::MemTiming mem;
  hw::PcieConfig pcie;
  nic::NicConfig nic;
  net::LinkConfig link;

  std::vector<QueueSetup> queues;
  std::vector<FlowSetup> flows;
  PressureSetup pressure;

  IoPolicy policy = IoPolicy::kBaseline;
  policy::CeioConfig ceio;

  /// Throws std::invalid_argument describing the first inconsistency.
  void validate() const;
};

/// Hardware of the [CEIO] testbed: Xeon Silver 4309Y (12 MB, 12-way LLC, 6 DDIO
/// ways, 8 slices), 8x DDR4-3200, BlueField-3 on PCIe Gen5 x16, 200 Gbps.
Scenario ceio_testbed();
/// Hardware of the [Toasty] testbed: Xeon Gold 5418Y (45 MB, 15-way LLC, 2 DDIO
/// ways, 24 slices), Intel E810 on PCIe Gen4 x16, 100 Gbps.
Scenario toasty_testbed();

}  // namespace ceio::exp
