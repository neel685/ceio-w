#include "ceio/exp/presets.hpp"

#include <algorithm>
#include <stdexcept>

#include "ceio/sim/rng.hpp"

namespace ceio::exp {

std::string scheme_name(Scheme s) {
  switch (s) {
    case Scheme::kBaseline: return "baseline";
    case Scheme::kHostCc: return "hostcc";
    case Scheme::kShRing: return "shring";
    case Scheme::kCeio: return "ceio";
    case Scheme::kToasty: return "toasty";
    case Scheme::kCeioW: return "ceio-w";
  }
  return "?";
}

Scheme scheme_from_name(const std::string& n) {
  for (Scheme s : {Scheme::kBaseline, Scheme::kHostCc, Scheme::kShRing, Scheme::kCeio, Scheme::kToasty,
                   Scheme::kCeioW})
    if (scheme_name(s) == n) return s;
  throw std::invalid_argument("unknown scheme '" + n + "'");
}

void apply_toasty(QueueSetup& q, ToastyPart part) {
  if (part != ToastyPart::kKernelOnly) q.order = host::PoolOrder::kLifo;
  if (part != ToastyPart::kUserOnly) {
    q.refill = QueueSetup::Refill::kToasty;
    q.core.tx_eager = true;  // the kernel-side change also expedites TX completion
  }
}

void apply_scheme(Scenario& s, Scheme scheme, const SchemeOptions& opt) {
  s.dctcp.enabled = opt.dctcp;
  s.name += "/" + scheme_name(scheme);
  switch (scheme) {
    case Scheme::kBaseline:
      return;
    case Scheme::kHostCc:
      s.policy = IoPolicy::kHostCc;
      return;
    case Scheme::kCeio:
      s.policy = IoPolicy::kCeio;
      if (opt.warm_pool)
        for (auto& q : s.queues)
          if (q.kind == nic::QueueKind::kDescriptor) {
            q.order = host::PoolOrder::kLifo;
            q.core.tx_eager = true;
          }
      return;
    case Scheme::kCeioW:
      s.policy = IoPolicy::kCeio;
      s.ceio.variant = policy::CeioConfig::Variant::kCeioW;
      for (auto& q : s.queues)
        if (q.kind == nic::QueueKind::kDescriptor) {  // CEIO-W warm buffers
          q.order = host::PoolOrder::kLifo;
          q.core.tx_eager = true;
        }
      return;
    case Scheme::kToasty:
      for (auto& q : s.queues)
        if (q.kind == nic::QueueKind::kDescriptor) apply_toasty(q);
      return;
    case Scheme::kShRing:
      break;
  }
  // ShRing: one shared ring + pool for all CPU-involved cores.
  std::vector<QueueSetup> queues;
  std::vector<std::uint32_t> remap(s.queues.size());
  QueueSetup shared;
  std::uint32_t cores = 0;
  auto merge = [&](const QueueSetup& q) { return q.kind == nic::QueueKind::kDescriptor && q.shareable; };
  for (std::size_t i = 0; i < s.queues.size(); ++i) {
    if (!merge(s.queues[i])) continue;
    if (cores == 0) shared = s.queues[i];
    cores += s.queues[i].cores;
    remap[i] = 0;
  }
  if (cores == 0) return;
  shared.cores = cores;
  shared.ring_size = opt.shring_ring;
  shared.pool_buffers = opt.shring_ring * 2;  // ring plus room for replies and backlog
  queues.push_back(shared);
  for (std::size_t i = 0; i < s.queues.size(); ++i) {
    if (merge(s.queues[i])) continue;
    remap[i] = static_cast<std::uint32_t>(queues.size());
    queues.push_back(s.queues[i]);
  }
  s.queues = std::move(queues);
  for (auto& f : s.flows) f.queue = remap[f.queue];
}

namespace {

/// Deterministic start offset in [0, one message gap): identical flows started
/// at the same instant stay phase-locked forever (e.g. every RDMA flow's 1 MB
/// message hits the switch at once), which real senders never do.
Tick jitter(net::FlowId id, std::uint32_t message_bytes, double gbps_rate) {
  const Tick gap = transfer_time(message_bytes, gbps_rate);
  return static_cast<Tick>(mix64(id) % std::max<Tick>(gap, 1));
}

void add_kv_flow(Scenario& s, net::FlowId id, const KvWorkload& kv, Tick start, Tick stop,
                 bool shareable = true) {
  QueueSetup q;
  q.shareable = shareable;
  q.app = host::AppProfile::kv();
  q.ring_size = kv.ring;
  q.pool_buffers = kv.pool;
  s.queues.push_back(q);
  FlowSetup f;
  f.spec.id = id;
  f.spec.packet_bytes = kv.packet_bytes;
  f.spec.message_bytes = kv.packet_bytes;
  f.spec.traffic.rate_gbps = kv.demand_gbps;
  f.spec.start = start + jitter(id, kv.packet_bytes, kv.demand_gbps);
  f.spec.stop = stop;
  f.queue = static_cast<std::uint32_t>(s.queues.size() - 1);
  s.flows.push_back(f);
}

void add_bypass_flow(Scenario& s, net::FlowId id, const BypassWorkload& b, Tick start, Tick stop) {
  QueueSetup q;
  q.kind = nic::QueueKind::kRdma;
  q.app = host::AppProfile::rdma_sink();
  q.mr_bytes = 256ULL << 20;
  s.queues.push_back(q);
  FlowSetup f;
  f.spec.id = id;
  f.spec.packet_bytes = b.packet_bytes;
  f.spec.message_bytes = b.message_bytes;
  f.spec.traffic.rate_gbps = b.rate_gbps;
  f.spec.start = start + jitter(id, b.message_bytes, b.rate_gbps);
  f.spec.stop = stop;
  f.queue = static_cast<std::uint32_t>(s.queues.size() - 1);
  s.flows.push_back(f);
}

/// [CEIO] Sec. 2.3 testbed: two servers connected back to back, so congestion
/// signals come only from the receiving host (no switch ECN marking).
Scenario ceio_back_to_back() {
  Scenario s = ceio_testbed();
  s.link.ecn_threshold_bytes = s.link.buffer_bytes;
  return s;
}

Scenario base(const std::string& name, Tick total) {
  Scenario s = ceio_back_to_back();
  s.name = name;
  s.warmup = 0;
  s.measure = total;
  s.sample_interval = 250 * kMicrosecond;
  return s;
}

}  // namespace

Scenario ceio_dynamic_flows(Scheme scheme, Tick phase, int phases, const KvWorkload& kv,
                            const BypassWorkload& bypass, const SchemeOptions& opt) {
  Scenario s = base("ceio-dynamic", kLeadIn + phase * static_cast<Tick>(phases));
  // KV flows 1..8; at the end of phase p, flows 2p-1 and 2p stop and two
  // bypass flows take their place.
  for (int i = 0; i < 8; ++i) {
    const int replaced_at = i / 2 + 1;  // phase index at which this flow is replaced
    const Tick stop = replaced_at < phases ? kLeadIn + phase * static_cast<Tick>(replaced_at) : ~Tick{0};
    add_kv_flow(s, static_cast<net::FlowId>(i + 1), kv, 0, stop);
  }
  for (int p = 1; p < phases && p <= 4; ++p)
    for (int j = 0; j < 2; ++j)
      add_bypass_flow(s, static_cast<net::FlowId>(100 + 2 * p + j), bypass, kLeadIn + phase * static_cast<Tick>(p),
                      ~Tick{0});
  apply_scheme(s, scheme, opt);
  return s;
}

Scenario ceio_network_burst(Scheme scheme, Tick phase, int phases, const KvWorkload& kv,
                            const SchemeOptions& opt) {
  Scenario s = base("ceio-burst", kLeadIn + phase * static_cast<Tick>(phases));
  for (int i = 0; i < 8; ++i) add_kv_flow(s, static_cast<net::FlowId>(i + 1), kv, 0, ~Tick{0});
  // Two burst flows per odd phase; each burst gets its own flow id and core.
  net::FlowId id = 50;
  for (int p = 1; p < phases; p += 2)
    for (int j = 0; j < 2; ++j)
      add_kv_flow(s, id++, kv, kLeadIn + phase * static_cast<Tick>(p), kLeadIn + phase * static_cast<Tick>(p + 1),
                  /*shareable=*/false);
  apply_scheme(s, scheme, opt);
  return s;
}

Scenario ceio_mixed_flows(Scheme scheme, int cpu_flows, const KvWorkload& kv, const BypassWorkload& bypass,
                          const SchemeOptions& opt) {
  if (cpu_flows < 0 || cpu_flows > 8) throw std::invalid_argument("ceio_mixed_flows: cpu_flows in [0, 8]");
  Scenario s = ceio_back_to_back();
  s.name = "ceio-mixed-" + std::to_string(cpu_flows) + ":" + std::to_string(8 - cpu_flows);
  s.warmup = 2 * kMillisecond;
  s.measure = 10 * kMillisecond;  // long enough for ~30 one-MB messages per bypass flow
  for (int i = 0; i < cpu_flows; ++i) add_kv_flow(s, static_cast<net::FlowId>(i + 1), kv, 0, ~Tick{0});
  for (int i = 0; i < 8 - cpu_flows; ++i)
    add_bypass_flow(s, static_cast<net::FlowId>(100 + i), bypass, 0, ~Tick{0});
  apply_scheme(s, scheme, opt);
  return s;
}

}  // namespace ceio::exp
