#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ceio/hw/pcie.hpp"
#include "ceio/net/packet.hpp"
#include "ceio/sim/simulator.hpp"
#include "ceio/sim/stats.hpp"

namespace ceio::nic {

using QueueId = std::uint32_t;

enum class QueueKind {
  /// DPDK / AF_XDP style: the driver posts buffers into a descriptor ring, the NIC
  /// DMAs each packet into the next posted buffer and writes one completion per packet
  /// (CPU-involved flows).
  kDescriptor,
  /// RDMA write into a registered memory region; one completion per message
  /// (write-with-immediate). CPU-bypass flows.
  kRdma,
};

struct RxQueueConfig {
  QueueKind kind = QueueKind::kDescriptor;
  std::uint32_t ring_size = 4096;    ///< descriptor ring capacity (kDescriptor)
  std::uint32_t cq_entries = 4096;   ///< completion ring slots (each one CQE)
  std::uint64_t cq_base = 0;         ///< host address of the completion ring
  std::uint64_t mr_base = 0;         ///< registered region (kRdma)
  std::uint64_t mr_bytes = 0;
};

/// A packet (or, for RDMA, a whole message) visible to the driver.
struct RxCompletion {
  net::Packet pkt;              ///< for RDMA: the message's last packet
  std::uint64_t buf_addr = 0;   ///< buffer (kDescriptor) or message start (kRdma)
  std::uint32_t bytes = 0;      ///< packet bytes, or message bytes for RDMA
  std::uint64_t cqe_addr = 0;
  Tick completed = 0;           ///< when the CQE became visible in host memory
};

struct RxQueueStats {
  std::uint64_t dma_packets = 0;   ///< packets DMA'd to the host (monotonic)
  std::uint64_t completions = 0;
  std::uint64_t waited_for_descriptor = 0;  ///< packets that found the ring empty
};

/// Host-visible state of one receive queue, owned by the Nic.
class RxQueue {
 public:
  RxQueue(QueueId id, const RxQueueConfig& cfg);

  QueueId id() const { return id_; }
  const RxQueueConfig& config() const { return cfg_; }

  /// Descriptors currently posted and unused by the NIC.
  std::uint32_t posted() const { return static_cast<std::uint32_t>(ring_.size()); }
  std::uint32_t ring_free_slots() const { return cfg_.ring_size - posted(); }

  /// Completions waiting for the driver, in arrival order.
  std::deque<RxCompletion>& completions() { return completions_; }
  /// Called (once per new completion) to wake a polling core.
  void set_notify(std::function<void()> fn) { notify_ = std::move(fn); }
  const RxQueueStats& stats() const { return stats_; }
  void reset_stats() { stats_ = {}; }
  /// Packets DMA'd since creation; never reset (drivers diff it per iteration).
  std::uint64_t dma_total() const { return dma_total_; }

 private:
  friend class Nic;
  std::uint64_t dma_total_ = 0;
  QueueId id_;
  RxQueueConfig cfg_;
  std::deque<std::uint64_t> ring_;        // posted buffer addresses, hardware FIFO
  std::deque<net::Packet> waiting_;       // packets in NIC memory waiting for a descriptor
  std::uint64_t waiting_bytes_ = 0;
  std::deque<RxCompletion> completions_;
  std::uint64_t cq_index_ = 0;
  std::uint64_t mr_offset_ = 0;
  std::unordered_map<net::FlowId, std::uint64_t> msg_start_;  // RDMA: flow -> current message base
  std::function<void()> notify_;
  RxQueueStats stats_;
};

enum class SteerAction {
  kHostQueue,  ///< legacy / fast path: DMA to the queue's host buffers
  kNicMemory,  ///< CEIO slow path: buffer in on-NIC memory (handled by the slow-path hook)
  kDrop,
};

/// One match-action rule in the NIC flow engine. `hits` is the per-rule
/// counter CEIO's flow controller polls.
struct SteeringRule {
  SteerAction action = SteerAction::kHostQueue;
  QueueId queue = 0;
  std::uint64_t hits = 0;
};

struct NicConfig {
  /// On-chip packet buffer shared by all receive queues (headroom before PCIe).
  std::uint64_t rx_buffer_bytes = 1ULL << 20;
  /// Max bytes one descriptor queue may hold on-chip while it has no posted
  /// buffers; beyond this its packets are dropped (ConnectX "rx_out_of_buffer",
  /// E810 per-queue drops), so one starved queue cannot exhaust the shared buffer.
  std::uint64_t queue_wait_bytes = 32 * 1024;
  /// Parse + match-action pipeline latency.
  Tick pipeline_latency = 100 * kNanosecond;
  std::uint32_t cqe_bytes = 64;
};

struct NicStats {
  std::uint64_t rx_packets = 0;
  std::uint64_t drops_buffer_full = 0;
  std::uint64_t drops_no_descriptor = 0;  ///< per-queue wait limit exceeded
  std::uint64_t drops_rule = 0;
  std::uint64_t drops_no_rule = 0;
  std::uint64_t to_nic_memory = 0;
};

/// Receive side of a NIC: flow engine, on-chip buffer, per-queue descriptor
/// rings / RDMA regions, and the DMA engine (via PcieHost).
class Nic {
 public:
  using DropHook = std::function<void(const net::Packet&)>;
  /// Handles packets steered with SteerAction::kNicMemory (installed by CEIO).
  using SlowPathHook = std::function<void(net::Packet, QueueId)>;

  Nic(Simulator& sim, hw::PcieHost& pcie, const NicConfig& cfg = {});

  QueueId add_queue(const RxQueueConfig& cfg);
  RxQueue& queue(QueueId q) { return queues_.at(q); }
  const RxQueue& queue(QueueId q) const { return queues_.at(q); }
  std::size_t num_queues() const { return queues_.size(); }

  void set_rule(net::FlowId flow, SteeringRule rule) { rules_[flow] = rule; }
  SteeringRule* rule(net::FlowId flow);

  /// A frame arrives from the wire.
  void receive(net::Packet p);
  /// Descriptors arrive at the NIC (after the driver's doorbell).
  void post_buffers(QueueId q, const std::vector<std::uint64_t>& addrs);
  /// DMAs `p` into the host for queue `q`, bypassing steering. Used by the slow
  /// path to drain on-NIC memory; `p` does not occupy the on-chip buffer.
  void deliver_to_host(net::Packet p, QueueId q);

  void set_drop_hook(DropHook h) { on_drop_ = std::move(h); }
  void set_slow_path(SlowPathHook h) { slow_path_ = std::move(h); }

  std::uint64_t rx_buffer_used() const { return buf_used_; }
  const TimeWeighted& rx_buffer_occupancy() const { return buf_occ_; }
  const NicStats& stats() const { return stats_; }
  void reset_stats();
  const NicConfig& config() const { return cfg_; }
  hw::PcieHost& pcie() { return pcie_; }

 private:
  void steer(net::Packet p);
  void drop(const net::Packet& p, std::uint64_t& counter);
  void try_dispatch(RxQueue& q);
  void dma_descriptor(RxQueue& q, net::Packet p, std::uint64_t buf, bool on_chip);
  void dma_rdma(RxQueue& q, net::Packet p, bool on_chip);
  std::uint64_t next_cqe(RxQueue& q);
  void complete(RxQueue& q, RxCompletion c);
  void release_on_chip(std::uint32_t bytes);

  Simulator& sim_;
  hw::PcieHost& pcie_;
  NicConfig cfg_;
  std::vector<RxQueue> queues_;
  std::unordered_map<net::FlowId, SteeringRule> rules_;
  DropHook on_drop_;
  SlowPathHook slow_path_;
  std::uint64_t buf_used_ = 0;
  TimeWeighted buf_occ_;
  NicStats stats_;
};

}  // namespace ceio::nic
