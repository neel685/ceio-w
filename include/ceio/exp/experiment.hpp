#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ceio/exp/scenario.hpp"
#include "ceio/hw/cache.hpp"

namespace ceio::exp {

struct LatencySummary {
  std::uint64_t count = 0;
  double mean_us = 0, p50_us = 0, p99_us = 0, p999_us = 0, max_us = 0;
};

struct FlowResult {
  net::FlowId id = 0;
  std::uint32_t queue = 0;
  std::uint64_t sent_packets = 0;
  std::uint64_t delivered_packets = 0;
  std::uint64_t delivered_bytes = 0;
  std::uint64_t drops_switch = 0;
  std::uint64_t drops_nic = 0;
  std::uint64_t slow_packets = 0;  ///< CEIO: packets that went through on-NIC memory
  std::uint64_t reordered = 0;     ///< deliveries older than one already delivered
  double goodput_gbps = 0;
  double mpps = 0;
  LatencySummary latency;  ///< sender emit -> application done
};

struct Sample {
  double t_ms = 0;                 ///< end of the sampling interval
  std::vector<double> flow_gbps;   ///< per flow, in Scenario::flows order
};

struct QueueResult {
  std::uint32_t queue = 0;
  std::uint32_t core = 0;
  std::string app;
  double core_busy_fraction = 0;
  double llc_read_hit_rate = 0;  ///< this core's loads only (packets, CQEs, tables)
  std::uint64_t processed = 0;
  std::uint64_t reply_drops = 0;
  std::uint64_t waited_for_descriptor = 0;
  std::uint32_t distinct_buffers_used = 0;  ///< over the whole run ([Toasty] Fig. 4)
};

/// Everything measured in the window [warmup, warmup + measure).
struct Results {
  std::string scenario;
  std::uint64_t seed = 0;
  double measured_ms = 0;

  double goodput_gbps = 0;
  double mpps = 0;
  LatencySummary latency;
  std::uint64_t sent_packets = 0;
  std::uint64_t delivered_packets = 0;
  std::uint64_t drops_switch = 0;
  std::uint64_t drops_nic = 0;
  std::uint64_t ecn_marked = 0;
  std::uint64_t reordered = 0;

  std::string policy = "baseline";
  policy::CeioStats ceio;           ///< zero unless policy == kCeio
  std::int64_t ceio_total_credits = 0;
  bool ceio_credits_conserved = true;  ///< CreditManager invariant at the end of the run
  policy::HostCcStats hostcc;
  std::uint64_t cc_rate_reductions = 0;  ///< DCTCP windows that cut a sender's rate

  hw::CacheStats cache;
  double io_write_hit_rate = 0;   ///< PCIe write hit rate
  double cpu_read_hit_rate = 0;   ///< LLC hit rate of CPU loads (packets, tables, CQEs)
  double dram_read_gbytes_s = 0;
  double dram_write_gbytes_s = 0;

  std::uint64_t pcie_credit_stalls = 0;
  double iio_occupancy_mean_bytes = 0;
  double iio_occupancy_max_bytes = 0;
  double nic_buffer_max_bytes = 0;

  std::vector<FlowResult> flows;
  std::vector<QueueResult> queues;   ///< one entry per core
  std::vector<Sample> samples;

  std::uint64_t events = 0;
  double wall_seconds = 0;
};

/// Builds the host described by `s`, runs it, and returns the measurements.
/// Deterministic: equal scenarios give equal results (except wall_seconds).
Results run(const Scenario& s);

/// Serialises results (and the key scenario parameters) as a JSON object.
std::string to_json(const Results& r);

struct SaturationResult {
  double offered_gbps = 0;  ///< highest aggregate CPU-involved load that passed
  Results at_rate;          ///< the run at that load
  int runs = 0;
};

/// Saturation throughput as measured in [Toasty] Sec. 4.1 / [CEIO] Sec. 6: the highest
/// offered load at which loss stays within `max_loss`. All CPU-involved
/// (descriptor-queue) flows are scaled together; RDMA flows keep their rates.
/// A run passes when drops <= max_loss * sent and the undelivered backlog does
/// not grow beyond max(64 packets, 1 % of sent), which catches overload before
/// the buffers fill. Bisection over [0, hi_gbps], hi_gbps defaulting to line rate.
SaturationResult find_saturation(const Scenario& s, double max_loss = 0.001, int iterations = 8,
                                 double hi_gbps = 0);

}  // namespace ceio::exp
