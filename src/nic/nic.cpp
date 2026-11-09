#include "ceio/nic/nic.hpp"

#include <stdexcept>
#include <string>

namespace ceio::nic {

RxQueue::RxQueue(QueueId id, const RxQueueConfig& cfg) : id_(id), cfg_(cfg) {
  if (cfg_.cq_entries == 0) throw std::invalid_argument("RxQueueConfig: cq_entries must be > 0");
  if (cfg_.kind == QueueKind::kDescriptor && cfg_.ring_size == 0)
    throw std::invalid_argument("RxQueueConfig: ring_size must be > 0");
  if (cfg_.kind == QueueKind::kRdma && cfg_.mr_bytes == 0)
    throw std::invalid_argument("RxQueueConfig: RDMA queue needs a memory region");
}

Nic::Nic(Simulator& sim, hw::PcieHost& pcie, const NicConfig& cfg)
    : sim_(sim), pcie_(pcie), cfg_(cfg), buf_occ_(sim.now()) {}

QueueId Nic::add_queue(const RxQueueConfig& cfg) {
  const auto id = static_cast<QueueId>(queues_.size());
  queues_.emplace_back(id, cfg);
  return id;
}

SteeringRule* Nic::rule(net::FlowId flow) {
  auto it = rules_.find(flow);
  return it == rules_.end() ? nullptr : &it->second;
}

void Nic::reset_stats() {
  stats_ = {};
  buf_occ_ = TimeWeighted(sim_.now(), static_cast<double>(buf_used_));
  for (auto& q : queues_) q.reset_stats();
}

void Nic::receive(net::Packet p) {
  ++stats_.rx_packets;
  sim_.schedule_in(cfg_.pipeline_latency, [this, p] { steer(p); });
}

void Nic::drop(const net::Packet& p, std::uint64_t& counter) {
  ++counter;
  if (on_drop_) on_drop_(p);
}

void Nic::steer(net::Packet p) {
  SteeringRule* r = rule(p.flow);
  if (!r) return drop(p, stats_.drops_no_rule);
  ++r->hits;
  switch (r->action) {
    case SteerAction::kDrop:
      return drop(p, stats_.drops_rule);
    case SteerAction::kNicMemory:
      ++stats_.to_nic_memory;
      if (!slow_path_) return drop(p, stats_.drops_rule);
      return slow_path_(p, r->queue);
    case SteerAction::kHostQueue:
      break;
  }
  RxQueue& q = queues_.at(r->queue);
  if (q.cfg_.kind == QueueKind::kDescriptor && q.ring_.empty() &&
      q.waiting_bytes_ + p.bytes > cfg_.queue_wait_bytes)
    return drop(p, stats_.drops_no_descriptor);
  if (buf_used_ + p.bytes > cfg_.rx_buffer_bytes) return drop(p, stats_.drops_buffer_full);
  buf_used_ += p.bytes;
  buf_occ_.set(sim_.now(), static_cast<double>(buf_used_));
  if (q.cfg_.kind == QueueKind::kRdma) return dma_rdma(q, p, true);
  q.waiting_.push_back(p);
  q.waiting_bytes_ += p.bytes;
  try_dispatch(q);
}

void Nic::try_dispatch(RxQueue& q) {
  while (!q.waiting_.empty()) {
    if (q.ring_.empty()) {
      ++q.stats_.waited_for_descriptor;
      return;  // packets stay in on-chip memory until the driver posts buffers
    }
    const std::uint64_t buf = q.ring_.front();
    q.ring_.pop_front();
    net::Packet p = q.waiting_.front();
    q.waiting_.pop_front();
    q.waiting_bytes_ -= p.bytes;
    dma_descriptor(q, p, buf, true);
  }
}

void Nic::post_buffers(QueueId qid, const std::vector<std::uint64_t>& addrs) {
  RxQueue& q = queues_.at(qid);
  if (q.cfg_.kind != QueueKind::kDescriptor)
    throw std::logic_error("Nic::post_buffers: queue " + std::to_string(qid) + " has no descriptor ring");
  if (addrs.size() > q.ring_free_slots())
    throw std::logic_error("Nic::post_buffers: descriptor ring overflow on queue " + std::to_string(qid));
  for (auto a : addrs) q.ring_.push_back(a);
  try_dispatch(q);
}

void Nic::deliver_to_host(net::Packet p, QueueId qid) {
  RxQueue& q = queues_.at(qid);
  if (q.cfg_.kind == QueueKind::kRdma) return dma_rdma(q, p, false);
  if (q.ring_.empty()) throw std::logic_error("Nic::deliver_to_host: no posted descriptor");
  const std::uint64_t buf = q.ring_.front();
  q.ring_.pop_front();
  dma_descriptor(q, p, buf, false);
}

std::uint64_t Nic::next_cqe(RxQueue& q) {
  const std::uint64_t slot = q.cq_index_++ % q.cfg_.cq_entries;
  return q.cfg_.cq_base + slot * cfg_.cqe_bytes;
}

void Nic::release_on_chip(std::uint32_t bytes) {
  buf_used_ -= bytes;
  buf_occ_.set(sim_.now(), static_cast<double>(buf_used_));
}

void Nic::dma_descriptor(RxQueue& q, net::Packet p, std::uint64_t buf, bool on_chip) {
  ++q.stats_.dma_packets;
  ++q.dma_total_;
  const std::uint32_t bytes = p.bytes;
  pcie_.dma_write(buf, bytes, [this, bytes, on_chip] {
    if (on_chip) release_on_chip(bytes);
  });
  const std::uint64_t cqe = next_cqe(q);
  pcie_.dma_write(cqe, cfg_.cqe_bytes, [this, qid = q.id_, p, buf, bytes, cqe] {
    complete(queues_[qid], RxCompletion{p, buf, bytes, cqe, sim_.now()});
  });
}

void Nic::dma_rdma(RxQueue& q, net::Packet p, bool on_chip) {
  ++q.stats_.dma_packets;
  ++q.dma_total_;
  const std::uint32_t bytes = p.bytes;
  if (q.mr_offset_ + bytes > q.cfg_.mr_bytes) q.mr_offset_ = 0;  // wrap the region
  const std::uint64_t addr = q.cfg_.mr_base + q.mr_offset_;
  q.mr_offset_ += bytes;
  if (p.msg_index == 0 || !q.msg_start_.count(p.flow)) q.msg_start_[p.flow] = addr;
  pcie_.dma_write(addr, bytes, [this, bytes, on_chip] {
    if (on_chip) release_on_chip(bytes);
  });
  if (!p.msg_last) return;
  const std::uint64_t start = q.msg_start_[p.flow];
  const std::uint32_t msg_bytes = p.msg_pkts * bytes;
  const std::uint64_t cqe = next_cqe(q);
  pcie_.dma_write(cqe, cfg_.cqe_bytes, [this, qid = q.id_, p, start, msg_bytes, cqe] {
    complete(queues_[qid], RxCompletion{p, start, msg_bytes, cqe, sim_.now()});
  });
}

void Nic::complete(RxQueue& q, RxCompletion c) {
  ++q.stats_.completions;
  q.completions_.push_back(c);
  if (q.notify_) q.notify_();
}

}  // namespace ceio::nic
