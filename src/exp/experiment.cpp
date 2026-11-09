#include "ceio/exp/experiment.hpp"

#include <chrono>
#include <iomanip>
#include <memory>
#include <sstream>
#include <unordered_map>

#include "ceio/host/address_space.hpp"
#include "ceio/sim/stats.hpp"

namespace ceio::exp {

namespace {

LatencySummary summarize(const Histogram& h) {
  LatencySummary s;
  s.count = h.count();
  if (!s.count) return s;
  s.mean_us = h.mean() / 1e6;
  s.p50_us = static_cast<double>(h.quantile(0.5)) / 1e6;
  s.p99_us = static_cast<double>(h.quantile(0.99)) / 1e6;
  s.p999_us = static_cast<double>(h.quantile(0.999)) / 1e6;
  s.max_us = static_cast<double>(h.max()) / 1e6;
  return s;
}

struct FlowAcc {
  std::uint64_t sent = 0, delivered = 0, bytes = 0, drop_sw = 0, drop_nic = 0, reordered = 0;
  bool seen = false;
  std::uint64_t last = 0;  // highest seq (or RDMA message id) delivered so far
  Histogram latency;
};

}  // namespace

Results run(const Scenario& s) {
  s.validate();
  const auto wall0 = std::chrono::steady_clock::now();

  Simulator sim;
  const Rng root(s.seed);
  hw::MemorySystem mem(s.cache, s.dram, s.mem);
  hw::PcieHost pcie(sim, mem, s.pcie);
  nic::Nic nic(sim, pcie, s.nic);
  host::AddressSpace as;

  std::vector<FlowAcc> acc(s.flows.size());
  std::unordered_map<net::FlowId, std::size_t> flow_index;
  for (std::size_t k = 0; k < s.flows.size(); ++k) flow_index[s.flows[k].spec.id] = k;
  bool measuring = s.warmup == 0;

  // Queues, pools and cores.
  std::vector<std::unique_ptr<host::BufferPool>> pools;
  std::vector<std::unique_ptr<host::CpuCore>> cores;
  for (std::size_t i = 0; i < s.queues.size(); ++i) {
    const QueueSetup& q = s.queues[i];
    const std::string tag = "q" + std::to_string(i);
    nic::RxQueueConfig qc;
    qc.kind = q.kind;
    qc.ring_size = q.ring_size;
    qc.cq_entries = q.kind == nic::QueueKind::kDescriptor ? q.ring_size : 1024;
    qc.cq_base = as.allocate(tag + ".cq", static_cast<std::uint64_t>(qc.cq_entries) * s.nic.cqe_bytes);
    host::BufferPool* pool = nullptr;
    if (q.kind == nic::QueueKind::kRdma) {
      qc.mr_bytes = q.mr_bytes;
      qc.mr_base = as.allocate(tag + ".mr", q.mr_bytes, 2ULL << 20);
    } else {
      const std::uint64_t base =
          as.allocate(tag + ".pool", static_cast<std::uint64_t>(q.pool_buffers) * q.buffer_bytes, 2ULL << 20);
      pools.push_back(std::make_unique<host::BufferPool>(base, q.pool_buffers, q.buffer_bytes, q.order));
      pool = pools.back().get();
    }
    const nic::QueueId qid = nic.add_queue(qc);
    const std::uint64_t table = q.app.table_bytes ? as.allocate(tag + ".table", q.app.table_bytes) : 0;
    const std::uint64_t desc = as.allocate(tag + ".desc", static_cast<std::uint64_t>(q.ring_size) * q.core.descriptor_bytes);
    cores.push_back(std::make_unique<host::CpuCore>(sim, mem, nic, qid, pool, q.app, q.core,
                                                    root.derive("core" + std::to_string(i)), table, desc,
                                                    s.link.rate_gbps));
    cores.back()->set_done_hook([&, rdma = q.kind == nic::QueueKind::kRdma](const nic::RxCompletion& c, Tick done) {
      auto it = flow_index.find(c.pkt.flow);
      if (it == flow_index.end()) return;
      FlowAcc& a = acc[it->second];
      const std::uint64_t order = rdma ? c.pkt.msg_id : c.pkt.seq;
      const bool late = a.seen && order < a.last;
      a.last = a.seen ? std::max(a.last, order) : order;
      a.seen = true;
      if (!measuring) return;
      if (late) ++a.reordered;
      a.delivered += rdma ? c.pkt.msg_pkts : 1;
      a.bytes += c.bytes;
      a.latency.record(done - c.pkt.sent);
    });
  }

  // Wire and senders.
  net::SwitchPort port(sim, s.link, [&nic](net::Packet p) { nic.receive(p); });
  auto count_drop = [&](const net::Packet& p, bool at_switch) {
    if (!measuring) return;
    auto it = flow_index.find(p.flow);
    if (it == flow_index.end()) return;
    (at_switch ? acc[it->second].drop_sw : acc[it->second].drop_nic)++;
  };
  port.set_drop_hook([&](const net::Packet& p) { count_drop(p, true); });
  nic.set_drop_hook([&](const net::Packet& p) { count_drop(p, false); });

  std::vector<std::unique_ptr<net::TrafficSource>> sources;
  for (std::size_t k = 0; k < s.flows.size(); ++k) {
    const FlowSetup& f = s.flows[k];
    nic.set_rule(f.spec.id, nic::SteeringRule{nic::SteerAction::kHostQueue, f.queue, 0});
    sources.push_back(std::make_unique<net::TrafficSource>(
        sim, f.spec, root.derive("flow" + std::to_string(f.spec.id)), [&, k](net::Packet p) {
          if (measuring) ++acc[k].sent;
          port.enqueue(p);
        }));
  }

  // I/O policy.
  std::unique_ptr<policy::CeioController> ceio;
  std::int64_t total_credits = 0;
  if (s.policy == IoPolicy::kCeio) {
    total_credits = s.ceio.total_credits;
    if (total_credits == 0) {
      std::uint32_t buf = 2048;
      for (const auto& q : s.queues)
        if (q.kind == nic::QueueKind::kDescriptor) {
          buf = q.buffer_bytes;
          break;
        }
      total_credits = static_cast<std::int64_t>(s.cache.ddio_bytes() / buf);  // [CEIO] Eq. (1)
    }
    std::vector<policy::CeioFlowInfo> infos;
    for (const auto& f : s.flows)
      infos.push_back({f.spec.id, cores[f.queue]->queue(), s.queues[f.queue].kind == nic::QueueKind::kRdma,
                       f.spec.start});
    ceio = std::make_unique<policy::CeioController>(sim, nic, s.ceio, infos, total_credits);
    for (auto& c : cores) c->set_extension(ceio.get());
    ceio->start();
  }

  std::unique_ptr<host::MemoryHog> hog;
  if (s.pressure.gbytes_per_s > 0) {
    const std::uint64_t base = as.allocate("hog", s.pressure.region_bytes, 2ULL << 20);
    hog = std::make_unique<host::MemoryHog>(sim, mem, base, s.pressure.region_bytes, s.pressure.gbytes_per_s,
                                            s.pressure.write_fraction, root.derive("hog"));
  }

  for (auto& c : cores) c->start();
  for (auto& src : sources) src->start();
  if (hog) hog->start();

  if (s.warmup > 0) {
    sim.schedule_at(s.warmup, [&] {
      measuring = true;
      mem.cache().reset_stats();
      mem.dram().reset_stats();
      pcie.reset_stats();
      port.reset_stats();
      nic.reset_stats();
      for (auto& c : cores) c->reset_stats();
      if (ceio) ceio->reset_stats();
    });
  }
  const Tick end = s.warmup + s.measure;
  sim.run_until(end);

  // Collect.
  Results r;
  r.scenario = s.name;
  r.seed = s.seed;
  r.measured_ms = to_us(s.measure) / 1000.0;
  const double secs = to_sec(s.measure);
  Histogram all;
  for (std::size_t k = 0; k < s.flows.size(); ++k) {
    const FlowAcc& a = acc[k];
    FlowResult f;
    f.id = s.flows[k].spec.id;
    f.queue = s.flows[k].queue;
    f.sent_packets = a.sent;
    f.delivered_packets = a.delivered;
    f.delivered_bytes = a.bytes;
    f.drops_switch = a.drop_sw;
    f.drops_nic = a.drop_nic;
    f.reordered = a.reordered;
    if (ceio) f.slow_packets = ceio->slow_packets(f.id);
    r.reordered += a.reordered;
    f.goodput_gbps = static_cast<double>(a.bytes) * 8 / secs / 1e9;
    f.mpps = static_cast<double>(a.delivered) / secs / 1e6;
    f.latency = summarize(a.latency);
    all.merge(a.latency);
    r.goodput_gbps += f.goodput_gbps;
    r.mpps += f.mpps;
    r.sent_packets += a.sent;
    r.delivered_packets += a.delivered;
    r.drops_switch += a.drop_sw;
    r.drops_nic += a.drop_nic;
    r.flows.push_back(f);
  }
  r.latency = summarize(all);
  r.ecn_marked = port.stats().ecn_marked;
  if (ceio) {
    r.policy = "ceio";
    r.ceio = ceio->stats();
    r.ceio_total_credits = total_credits;
    r.ceio_credits_conserved = ceio->credits().invariant_ok();
  }

  r.cache = mem.cache().stats();
  r.io_write_hit_rate = r.cache.io_write_hit_rate();
  r.cpu_read_hit_rate = r.cache.cpu_read_hit_rate();
  r.dram_read_gbytes_s = static_cast<double>(mem.dram().stats().bytes_read) / secs / 1e9;
  r.dram_write_gbytes_s = static_cast<double>(mem.dram().stats().bytes_written) / secs / 1e9;

  r.pcie_credit_stalls = pcie.stats().credit_stalls;
  r.iio_occupancy_mean_bytes = pcie.iio_occupancy().mean(end);
  r.iio_occupancy_max_bytes = pcie.iio_occupancy().max();
  r.nic_buffer_max_bytes = nic.rx_buffer_occupancy().max();

  for (std::size_t i = 0; i < cores.size(); ++i) {
    QueueResult q;
    q.queue = static_cast<std::uint32_t>(i);
    q.app = cores[i]->app().name;
    q.core_busy_fraction = static_cast<double>(cores[i]->busy_time()) / static_cast<double>(s.measure);
    q.processed = cores[i]->stats().packets;
    q.llc_read_hit_rate = cores[i]->stats().llc_read_hit_rate();
    q.reply_drops = cores[i]->stats().reply_drops;
    q.waited_for_descriptor = nic.queue(cores[i]->queue()).stats().waited_for_descriptor;
    if (cores[i]->pool()) q.distinct_buffers_used = cores[i]->pool()->distinct_used();
    r.queues.push_back(q);
  }

  r.events = sim.events_processed();
  r.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
  return r;
}

SaturationResult find_saturation(const Scenario& s, double max_loss, int iterations, double hi_gbps) {
  s.validate();
  double base = 0;
  for (const auto& f : s.flows)
    if (s.queues[f.queue].kind == nic::QueueKind::kDescriptor) base += f.spec.traffic.rate_gbps;
  if (base <= 0) throw std::invalid_argument("find_saturation: no CPU-involved flows to scale");
  if (hi_gbps <= 0) hi_gbps = s.link.rate_gbps;

  auto attempt = [&](double gbps) {
    Scenario t = s;
    for (auto& f : t.flows)
      if (t.queues[f.queue].kind == nic::QueueKind::kDescriptor) f.spec.traffic.rate_gbps *= gbps / base;
    return run(t);
  };
  auto passes = [&](const Results& r) {
    std::uint64_t sent = 0, delivered = 0, drops = 0;
    for (const auto& f : r.flows) {
      if (s.queues[f.queue].kind != nic::QueueKind::kDescriptor) continue;
      sent += f.sent_packets;
      delivered += f.delivered_packets;
      drops += f.drops_nic + f.drops_switch;
    }
    const double backlog = static_cast<double>(sent) - static_cast<double>(delivered + drops);
    return static_cast<double>(drops) <= max_loss * static_cast<double>(sent) &&
           backlog <= std::max(64.0, 0.01 * static_cast<double>(sent));
  };

  SaturationResult out;
  double lo = 0, hi = hi_gbps;
  Results top = attempt(hi);
  ++out.runs;
  if (passes(top)) {
    out.offered_gbps = hi;
    out.at_rate = top;
    return out;
  }
  for (int i = 0; i < iterations; ++i) {
    const double mid = 0.5 * (lo + hi);
    Results r = attempt(mid);
    ++out.runs;
    if (passes(r)) {
      lo = mid;
      out.offered_gbps = mid;
      out.at_rate = std::move(r);
    } else {
      hi = mid;
    }
  }
  return out;
}

namespace {

class Json {
 public:
  Json() { os_ << std::setprecision(10); }
  Json& open(const char* key = nullptr, char brace = '{') {
    sep(key);
    os_ << brace;
    first_ = true;
    return *this;
  }
  Json& close(char brace = '}') {
    os_ << brace;
    first_ = false;
    return *this;
  }
  template <class T>
  Json& kv(const char* key, const T& v) {
    sep(key);
    write(v);
    return *this;
  }
  std::string str() const { return os_.str(); }

 private:
  void sep(const char* key) {
    if (!first_) os_ << ',';
    first_ = false;
    if (key) os_ << '"' << key << "\":";
  }
  void write(const std::string& v) { os_ << '"' << v << '"'; }
  void write(double v) { os_ << v; }
  template <class T>
  void write(const T& v) { os_ << v; }
  std::ostringstream os_;
  bool first_ = true;
};

void latency_json(Json& j, const char* key, const LatencySummary& l) {
  j.open(key)
      .kv("count", l.count)
      .kv("mean_us", l.mean_us)
      .kv("p50_us", l.p50_us)
      .kv("p99_us", l.p99_us)
      .kv("p999_us", l.p999_us)
      .kv("max_us", l.max_us)
      .close();
}

}  // namespace

std::string to_json(const Results& r) {
  Json j;
  j.open()
      .kv("scenario", r.scenario)
      .kv("seed", r.seed)
      .kv("measured_ms", r.measured_ms)
      .kv("goodput_gbps", r.goodput_gbps)
      .kv("mpps", r.mpps)
      .kv("sent_packets", r.sent_packets)
      .kv("delivered_packets", r.delivered_packets)
      .kv("drops_switch", r.drops_switch)
      .kv("drops_nic", r.drops_nic)
      .kv("ecn_marked", r.ecn_marked)
      .kv("reordered", r.reordered)
      .kv("policy", r.policy);
  latency_json(j, "latency", r.latency);
  if (r.policy == "ceio") {
    j.open("ceio")
        .kv("total_credits", r.ceio_total_credits)
        .kv("fast_packets", r.ceio.fast_packets)
        .kv("slow_packets", r.ceio.slow_packets)
        .kv("to_slow", r.ceio.to_slow)
        .kv("to_fast", r.ceio.to_fast)
        .kv("drain_requests", r.ceio.drain_requests)
        .kv("drained_packets", r.ceio.drained_packets)
        .kv("upgraded_packets", r.ceio.upgraded_packets)
        .kv("held_packets", r.ceio.held_packets)
        .kv("nic_mem_drops", r.ceio.nic_mem_drops)
        .kv("nic_mem_max_bytes", r.ceio.nic_mem_max_bytes)
        .kv("cc_signals", r.ceio.cc_signals)
        .kv("deactivations", r.ceio.deactivations)
        .kv("reactivations", r.ceio.reactivations)
        .kv("min_credit_balance", r.ceio.min_credit_balance)
        .close();
  }
  j.open("cache")
      .kv("io_write_hit_rate", r.io_write_hit_rate)
      .kv("cpu_read_hit_rate", r.cpu_read_hit_rate)
      .kv("io_write_hits", r.cache.io_write_hits)
      .kv("io_write_allocs", r.cache.io_write_allocs)
      .kv("cpu_read_hits", r.cache.cpu_read_hits)
      .kv("cpu_read_misses", r.cache.cpu_read_misses)
      .kv("writebacks", r.cache.writebacks)
      .kv("dead_writebacks", r.cache.dead_writebacks)
      .kv("leaky_evictions", r.cache.leaky_evictions)
      .close();
  j.open("memory").kv("dram_read_gbytes_s", r.dram_read_gbytes_s).kv("dram_write_gbytes_s", r.dram_write_gbytes_s).close();
  j.open("pcie")
      .kv("credit_stalls", r.pcie_credit_stalls)
      .kv("iio_occupancy_mean_bytes", r.iio_occupancy_mean_bytes)
      .kv("iio_occupancy_max_bytes", r.iio_occupancy_max_bytes)
      .close();
  j.kv("nic_buffer_max_bytes", r.nic_buffer_max_bytes);
  j.open("flows", '[');
  for (const auto& f : r.flows) {
    j.open()
        .kv("id", f.id)
        .kv("queue", f.queue)
        .kv("sent_packets", f.sent_packets)
        .kv("delivered_packets", f.delivered_packets)
        .kv("delivered_bytes", f.delivered_bytes)
        .kv("drops_switch", f.drops_switch)
        .kv("drops_nic", f.drops_nic)
        .kv("slow_packets", f.slow_packets)
        .kv("reordered", f.reordered)
        .kv("goodput_gbps", f.goodput_gbps)
        .kv("mpps", f.mpps);
    latency_json(j, "latency", f.latency);
    j.close();
  }
  j.close(']');
  j.open("queues", '[');
  for (const auto& q : r.queues) {
    j.open()
        .kv("queue", q.queue)
        .kv("app", q.app)
        .kv("core_busy_fraction", q.core_busy_fraction)
        .kv("llc_read_hit_rate", q.llc_read_hit_rate)
        .kv("processed", q.processed)
        .kv("reply_drops", q.reply_drops)
        .kv("waited_for_descriptor", q.waited_for_descriptor)
        .kv("distinct_buffers_used", q.distinct_buffers_used)
        .close();
  }
  j.close(']');
  j.kv("events", r.events).kv("wall_seconds", r.wall_seconds).close();
  return j.str();
}

}  // namespace ceio::exp
