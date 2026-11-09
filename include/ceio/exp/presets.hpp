#pragma once

#include <string>

#include "ceio/exp/scenario.hpp"

namespace ceio::exp {

/// The receive-path schemes compared in [CEIO] Sec. 6 and [Toasty] Sec. 4.
enum class Scheme {
  kBaseline,  ///< DDIO, one ring + FIFO pool per core, keep-full refill
  kHostCc,    ///< reactive rate control from IIO occupancy ([CEIO] [1])
  kShRing,    ///< one shared receive ring below LLC capacity ([CEIO] [61])
  kCeio,      ///< credits + elastic on-NIC buffering ([CEIO])
  kToasty,    ///< LIFO pool + adaptive refill + eager TX ([Toasty])
  kCeioW,     ///< CEIO-W (docs/ceio-w.md): warm, footprint-aware, ring-as-credit
};

std::string scheme_name(Scheme s);
Scheme scheme_from_name(const std::string& name);  ///< throws std::invalid_argument

struct SchemeOptions {
  std::uint32_t shring_ring = 4096;  ///< shared ring entries ([CEIO] Sec. 6.1)
  bool dctcp = true;                 ///< senders run DCTCP ([CEIO] Sec. 2.3)
  bool warm_pool = false;            ///< ablation: give CEIO a LIFO pool + eager TX (warm buffers only)
};

/// Turns a scenario with one descriptor queue per core into `scheme`:
/// ShRing merges all CPU-involved queues into one shared ring served by the
/// same cores; Toasty switches every CPU-involved queue to its driver;
/// HostCC/CEIO select the controller. RDMA queues are left as they are.
void apply_scheme(Scenario& s, Scheme scheme, const SchemeOptions& opt = {});

/// Toasty ablations ([Toasty] Fig. 18).
enum class ToastyPart { kFull, kUserOnly /* LIFO pool */, kKernelOnly /* adaptive refill */ };
void apply_toasty(QueueSetup& q, ToastyPart part = ToastyPart::kFull);

/// eRPC key-value workload of [CEIO] Sec. 6.1: 144 B requests (16 B key, 64 B value),
/// one dedicated core per flow, each sender demanding `demand_gbps`.
struct KvWorkload {
  std::uint32_t packet_bytes = 144;
  double demand_gbps = 8.0;     ///< above one KV core's capacity, so cores saturate
  std::uint32_t ring = 4096;    ///< eRPC/DPDK-scale per-core ring: 8 cores far exceed the DDIO ways
  std::uint32_t pool = 8192;
};

/// LineFS-like CPU-bypass flow: RDMA writes of `message_bytes` chunks.
struct BypassWorkload {
  double rate_gbps = 25.0;
  std::uint32_t message_bytes = 1 << 20;
  std::uint32_t packet_bytes = 4096;
};

/// Phase-based scenarios start with a 1 ms lead-in (senders' DCTCP converges);
/// phase p covers [lead_in + p * phase, lead_in + (p + 1) * phase).
inline constexpr Tick kLeadIn = kMillisecond;

/// [CEIO] Fig. 4a / 10a, dynamic flow distribution: 8 CPU-involved flows; every
/// `phase`, two of them are replaced by CPU-bypass flows (`phases` phases total).
Scenario ceio_dynamic_flows(Scheme scheme, Tick phase = 2 * kMillisecond, int phases = 3,
                            const KvWorkload& kv = {}, const BypassWorkload& bypass = {},
                            const SchemeOptions& opt = {});
/// [CEIO] Fig. 4b / 10b, network burst: 8 CPU-involved flows plus two burst
/// CPU-involved flows (two extra cores) active in every other phase.
Scenario ceio_network_burst(Scheme scheme, Tick phase = 2 * kMillisecond, int phases = 4,
                            const KvWorkload& kv = {}, const SchemeOptions& opt = {});
/// [CEIO] Table 4, mixed I/O flows: `cpu_flows` CPU-involved + (8 - cpu_flows)
/// CPU-bypass flows, steady.
Scenario ceio_mixed_flows(Scheme scheme, int cpu_flows, const KvWorkload& kv = {},
                          const BypassWorkload& bypass = {}, const SchemeOptions& opt = {});

/// [CEIO] Fig. 12, flow churn: `n_flows` echo flows spread over `cores`
/// cores; every `slot` a fresh random set of `active` flows sends (together
/// `offered_gbps`, 512 B packets). Open loop (no CC), below CPU capacity, so
/// differences come from credit management alone.
struct ChurnWorkload {
  int n_flows = 1024;
  Tick slot = 100 * kMicrosecond;
  int cores = 8;
  int active = 16;
  double offered_gbps = 60.0;
  Tick duration = 4 * kMillisecond;
};
Scenario ceio_flow_churn(Scheme scheme, const ChurnWorkload& w = {}, const SchemeOptions& opt = {});

}  // namespace ceio::exp
