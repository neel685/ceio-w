#include "ceio/policy/ceio.hpp"

#include <algorithm>
#include <stdexcept>

namespace ceio::policy {

CeioController::CeioController(Simulator& sim, nic::Nic& nic, const CeioConfig& cfg,
                               const std::vector<CeioFlowInfo>& flows, std::int64_t total_credits)
    : sim_(sim), nic_(nic), cfg_(cfg), cm_(total_credits, cfg.literal_split), nic_core_free_(cfg.nic_cores, 0) {
  for (const auto& info : flows) {
    if (flows_.count(info.id)) throw std::invalid_argument("CeioController: duplicate flow");
    flows_[info.id].info = info;
    by_queue_[info.queue].push_back(info.id);
  }
}

void CeioController::start() {
  nic_.set_steer_hook([this](const net::Packet& p, nic::SteeringRule& r) { on_steer(p, r); });
  nic_.set_slow_path([this](net::Packet p, nic::QueueId q) { on_slow_path(p, q); });
  std::map<Tick, std::vector<net::FlowId>> by_start;
  for (auto& [id, f] : flows_) {
    if (nic::SteeringRule* r = nic_.rule(id)) r->action = nic::SteerAction::kNicMemory;
    by_start[f.info.start].push_back(id);
  }
  for (auto& [t, ids] : by_start)
    sim_.schedule_at(std::max(t, sim_.now()), [this, group = ids] { establish(group); });
  sim_.schedule_in(0, [this] { control_loop(); });
  sim_.schedule_in(cfg_.reactivate_period, [this] { reactivation_tick(); });
}

void CeioController::reset_stats() {
  stats_ = {};
  stats_.nic_mem_max_bytes = static_cast<double>(nic_mem_used_);
  for (auto& [id, f] : flows_) f.slow_total = 0;
}

std::uint64_t CeioController::slow_packets(net::FlowId f) const {
  auto it = flows_.find(f);
  return it == flows_.end() ? 0 : it->second.slow_total;
}

void CeioController::set_mode(Flow& f, Mode m) {
  if (f.mode == m) return;
  f.mode = m;
  if (nic::SteeringRule* r = nic_.rule(f.info.id))
    r->action = m == Mode::kFast ? nic::SteerAction::kHostQueue : nic::SteerAction::kNicMemory;
  ++(m == Mode::kFast ? stats_.to_fast : stats_.to_slow);
}

void CeioController::update_mode(Flow& f) {
  const net::FlowId id = f.info.id;
  if (!cm_.active(id) || f.paused) return;
  const std::int64_t credits = cm_.credits(id);
  if (f.mode == Mode::kFast) {
    if (credits <= 0) set_mode(f, Mode::kSlow);
    return;
  }
  if (credits <= 0 || f.draining > 0) return;
  if (f.nic_mem.empty()) {
    set_mode(f, Mode::kFast);  // phase exclusivity: nothing of this flow is behind
    return;
  }
  // Backlog left and enough credits to promote all of it: re-enable through a
  // NIC-driven drain. Host-fetched data must not be in flight for this queue.
  auto c = claimed_.find(f.info.queue);
  const bool fetch_pending = c != claimed_.end() && c->second > 0;
  if (!f.info.rdma && !fetch_pending && credits >= static_cast<std::int64_t>(f.nic_mem.size())) start_upgrade(f);
}

void CeioController::start_upgrade(Flow& f) {
  f.paused = true;
  const net::FlowId id = f.info.id;
  while (!f.nic_mem.empty()) {
    net::Packet p = f.nic_mem.front();
    f.nic_mem.pop_front();
    release_nic_mem(f, p);
    p.slow = false;  // lands in the fast ring and holds a credit until processed
    cm_.consume(id, 1);
    ++f.upgrading;
    ++stats_.upgraded_packets;
    sim_.schedule_at(nic_mem_read(id, p.bytes), [this, p, id] {
      Flow& g = flows_.at(id);
      nic_.deliver_to_host(p, g.info.queue);
      if (--g.upgrading == 0) finish_upgrade(g);
    });
  }
}

void CeioController::finish_upgrade(Flow& f) {
  // Everything older is now queued for DMA; the held packets follow in order,
  // as long as credits last. The rest goes to on-NIC memory and the flow stays slow.
  const net::FlowId id = f.info.id;
  f.paused = false;
  while (!f.held.empty() && cm_.credits(id) > 0) {
    cm_.consume(id, 1);
    ++stats_.fast_packets;
    nic_.deliver_to_host(f.held.front(), f.info.queue);
    f.held.pop_front();
  }
  if (f.held.empty()) {
    set_mode(f, Mode::kFast);
    return;
  }
  for (net::Packet& p : f.held) {
    p.slow = true;
    f.nic_mem.push_back(p);
    f.nic_mem_bytes += p.bytes;
    nic_mem_used_ += p.bytes;
    ++stats_.slow_packets;
    ++f.slow_total;
  }
  f.held.clear();
  nic_.queue(f.info.queue).poke();
}

void CeioController::establish(const std::vector<net::FlowId>& ids) {
  cm_.add_flows(ids);
  for (net::FlowId id : ids) {
    Flow& f = flows_.at(id);
    f.established = true;
    f.last_arrival = sim_.now();
  }
  for (auto& [id, f] : flows_) update_mode(f);  // newcomers took credits from others
}

void CeioController::apply_release(Flow& f) {
  if (f.pending_release == 0) return;
  cm_.release(f.info.id, f.pending_release);
  f.pending_release = 0;
}

// ---------------------------------------------------------------------------
// NIC side
// ---------------------------------------------------------------------------

void CeioController::on_steer(const net::Packet& p, nic::SteeringRule& rule) {
  auto it = flows_.find(p.flow);
  if (it == flows_.end()) return;
  Flow& f = it->second;
  f.last_arrival = sim_.now();
  if (!cm_.active(p.flow)) {
    if (f.established && !f.waiting_reactivation) {
      f.waiting_reactivation = true;
      reactivate_.push_back(p.flow);
    }
    rule.action = nic::SteerAction::kNicMemory;
    return;
  }
  if (f.paused) {
    rule.action = nic::SteerAction::kNicMemory;  // diverted to the on-chip hold queue
    return;
  }
  if (f.mode == Mode::kSlow) return;
  if (cfg_.ideal_detection) {
    if (cm_.credits(p.flow) <= 0) {
      set_mode(f, Mode::kSlow);  // rewrites `rule`: this packet already goes slow
      return;
    }
    cm_.consume(p.flow, 1);
  } else {
    ++f.fast_hits;  // the hardware counter; the NIC core reads it in control_loop()
  }
  ++stats_.fast_packets;
}

void CeioController::on_slow_path(net::Packet p, nic::QueueId) {
  auto it = flows_.find(p.flow);
  if (it == flows_.end()) return;
  Flow& f = it->second;
  if (f.paused) {
    ++stats_.held_packets;
    f.held.push_back(p);
    return;
  }
  if (nic_mem_used_ + p.bytes > cfg_.nic_mem_bytes) {
    ++stats_.nic_mem_drops;
    return;
  }
  p.slow = true;
  f.nic_mem.push_back(p);
  f.nic_mem_bytes += p.bytes;
  nic_mem_used_ += p.bytes;
  stats_.nic_mem_max_bytes = std::max(stats_.nic_mem_max_bytes, static_cast<double>(nic_mem_used_));
  ++stats_.slow_packets;
  ++f.slow_total;
  if (f.nic_mem_bytes > cfg_.cc_threshold_bytes) {
    ++stats_.cc_signals;
    f.nic_mem.back().ecn = true;  // the slow path backs up: ask the sender to slow down
    if (on_congestion_ && (!f.sent_cnp || sim_.now() - f.last_cnp >= cfg_.cnp_interval)) {
      f.sent_cnp = true;
      f.last_cnp = sim_.now();
      on_congestion_(p.flow);  // NIC-generated notification (CNP), rate-limited
    }
  }
  if (f.info.rdma) {
    if (p.msg_last) drain_rdma(f);  // event-driven: write-with-immediate completes a message
  } else {
    nic_.queue(f.info.queue).poke();  // let a waiting driver fetch it
  }
}

void CeioController::control_loop() {
  for (auto& [id, f] : flows_) {
    if (!f.established) continue;
    if (!cfg_.ideal_detection && f.fast_hits != f.hits_seen) {
      cm_.consume(id, static_cast<std::int64_t>(f.fast_hits - f.hits_seen));
      f.hits_seen = f.fast_hits;
    }
    apply_release(f);
    stats_.min_credit_balance = std::min(stats_.min_credit_balance, cm_.credits(id));
    if (cm_.active(id) && sim_.now() - f.last_arrival > cfg_.idle_timeout && f.nic_mem.empty() &&
        f.draining == 0) {
      cm_.deactivate(id);
      set_mode(f, Mode::kSlow);
      ++stats_.deactivations;
      for (auto& [j, g] : flows_) update_mode(g);
      continue;
    }
    update_mode(f);
  }
  const Tick period = cfg_.poll_base + cfg_.poll_per_flow * flows_.size();
  sim_.schedule_in(std::max<Tick>(period, 1), [this] { control_loop(); });
}

void CeioController::reactivation_tick() {
  std::vector<net::FlowId> batch;
  while (!reactivate_.empty() && batch.size() < cfg_.reactivate_per_tick) {
    const net::FlowId id = reactivate_.front();
    reactivate_.pop_front();
    flows_.at(id).waiting_reactivation = false;
    if (!cm_.active(id)) batch.push_back(id);
  }
  if (!batch.empty()) {
    cm_.add_flows(batch);
    stats_.reactivations += batch.size();
    for (net::FlowId id : batch) flows_.at(id).last_arrival = sim_.now();
    for (auto& [id, f] : flows_) update_mode(f);
  }
  sim_.schedule_in(cfg_.reactivate_period, [this] { reactivation_tick(); });
}

Tick CeioController::nic_mem_read(net::FlowId flow, std::uint32_t bytes) {
  // The flow's NIC core moves the packet, then its data streams out of on-NIC memory.
  Tick core_done = sim_.now();
  if (!nic_core_free_.empty()) {
    Tick& core = nic_core_free_[flow % nic_core_free_.size()];
    core = std::max(sim_.now(), core) + cfg_.nic_core_per_packet;
    core_done = core;
  }
  const Tick start = std::max(core_done, nic_mem_free_);
  nic_mem_free_ = start + transfer_time(bytes, cfg_.nic_mem_gbps);
  return nic_mem_free_ + cfg_.nic_mem_latency;
}

void CeioController::release_nic_mem(Flow& f, const net::Packet& p) {
  f.nic_mem_bytes -= p.bytes;
  nic_mem_used_ -= p.bytes;
}

void CeioController::drain_rdma(Flow& f) {
  while (!f.nic_mem.empty()) {
    net::Packet p = f.nic_mem.front();
    f.nic_mem.pop_front();
    release_nic_mem(f, p);
    ++f.draining;
    const net::FlowId id = f.info.id;
    sim_.schedule_at(nic_mem_read(id, p.bytes), [this, p, id] {
      Flow& g = flows_.at(id);
      nic_.deliver_to_host(p, g.info.queue, [this, id] {
        Flow& h = flows_.at(id);
        --h.draining;
        ++stats_.drained_packets;
        update_mode(h);
      });
    });
  }
}

// ---------------------------------------------------------------------------
// Host side (CEIO library in the driver)
// ---------------------------------------------------------------------------

std::uint32_t CeioController::pending_slow(nic::QueueId q) const {
  auto it = by_queue_.find(q);
  if (it == by_queue_.end()) return 0;
  std::uint64_t n = 0;
  for (net::FlowId id : it->second) {
    const Flow& f = flows_.at(id);
    if (f.info.rdma || f.paused) continue;
    // Flows with credits for their whole backlog are re-enabled by the NIC; the
    // host library only pulls data of flows that are out of credits.
    if (cm_.active(id) && cm_.credits(id) >= static_cast<std::int64_t>(f.nic_mem.size())) continue;
    n += f.nic_mem.size();
  }
  auto c = claimed_.find(q);
  const std::uint64_t claimed = c == claimed_.end() ? 0 : c->second;
  return static_cast<std::uint32_t>(n > claimed ? n - claimed : 0);
}

host::RxExtension::PollAction CeioController::on_poll(host::CpuCore& core, std::size_t fast_ready) {
  const nic::QueueId q = core.queue();
  if (!core.pool()) return PollAction::kNone;  // RDMA queues are drained by NIC cores
  const std::uint32_t pending = pending_slow(q);
  std::uint32_t& inflight = drain_inflight_[q];
  if (cfg_.recv == CeioConfig::Recv::kSync) {
    if (fast_ready > 0) return PollAction::kNone;
    if (inflight == 0 && pending > 0) request_drain(core, std::min(cfg_.drain_batch, pending));
  } else if (pending > 0 && inflight < cfg_.async_window) {
    request_drain(core, std::min({cfg_.drain_batch, pending, cfg_.async_window - inflight}));
  }
  return fast_ready == 0 && inflight > 0 ? PollAction::kWait : PollAction::kNone;
}

void CeioController::request_drain(host::CpuCore& core, std::uint32_t n) {
  host::BufferPool* pool = core.pool();
  std::vector<std::uint64_t> bufs;
  while (bufs.size() < n) {
    auto b = pool->get();
    if (!b) break;
    bufs.push_back(*b);
  }
  if (bufs.empty()) return;
  const nic::QueueId q = core.queue();
  const auto k = static_cast<std::uint32_t>(bufs.size());
  drain_inflight_[q] += k;
  claimed_[q] += k;
  ++stats_.drain_requests;
  // The request (buffer addresses) reaches the NIC cores as an MMIO write.
  nic_.pcie().mmio_write(8 + 8 * k, [this, q, bufs = std::move(bufs), pool]() mutable {
    drain_descriptor(q, std::move(bufs), pool);
  });
}

void CeioController::drain_descriptor(nic::QueueId q, std::vector<std::uint64_t> bufs, host::BufferPool* pool) {
  claimed_[q] -= static_cast<std::uint32_t>(bufs.size());
  const auto& ids = by_queue_.at(q);
  for (std::uint64_t buf : bufs) {
    // Oldest buffered packet across the queue's flows (arrival order).
    Flow* src = nullptr;
    for (net::FlowId id : ids) {
      Flow& f = flows_.at(id);
      if (f.info.rdma || f.paused || f.nic_mem.empty()) continue;
      if (!src || f.nic_mem.front().nic_arrival < src->nic_mem.front().nic_arrival) src = &f;
    }
    if (!src) {  // nothing left (e.g. raced with a shrinking backlog)
      pool->put(buf);
      --drain_inflight_[q];
      continue;
    }
    net::Packet p = src->nic_mem.front();
    src->nic_mem.pop_front();
    release_nic_mem(*src, p);
    ++src->draining;
    const net::FlowId id = src->info.id;
    sim_.schedule_at(nic_mem_read(id, p.bytes), [this, p, q, buf, id] {
      nic_.deliver_to_buffer(p, q, buf, [this, q, id] {
        Flow& f = flows_.at(id);
        --f.draining;
        --drain_inflight_[q];
        ++stats_.drained_packets;
        update_mode(f);
      });
    });
  }
}

void CeioController::on_processed(host::CpuCore&, const nic::RxCompletion& c) {
  if (c.fast_pkts > 0 && flows_.count(c.pkt.flow)) unreleased_[c.pkt.flow] += c.fast_pkts;
}

void CeioController::on_batch_end(host::CpuCore& core) {
  auto it = by_queue_.find(core.queue());
  if (it == by_queue_.end()) return;
  std::vector<std::pair<net::FlowId, std::int64_t>> released;
  for (net::FlowId id : it->second) {
    auto u = unreleased_.find(id);
    if (u == unreleased_.end() || u->second == 0) continue;
    released.emplace_back(id, u->second);
    u->second = 0;
  }
  if (released.empty()) return;
  // Lazy release: the head-pointer update reaches the NIC cores over PCIe.
  nic_.pcie().mmio_write(8, [this, released = std::move(released)] {
    for (const auto& [id, n] : released) {
      Flow& f = flows_.at(id);
      f.pending_release += n;
      if (cfg_.ideal_detection) {
        apply_release(f);
        update_mode(f);
      }
    }
  });
}

}  // namespace ceio::policy
