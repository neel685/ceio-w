#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

#include "ceio/host/core.hpp"
#include "ceio/host/rx_extension.hpp"
#include "ceio/nic/nic.hpp"
#include "ceio/policy/credit_manager.hpp"
#include "ceio/sim/simulator.hpp"

namespace ceio::policy {

struct CeioConfig {
  /// Total credits; 0 = DDIO capacity / buffer size (Eq. 1; 6 MB / 2 KB = 3072).
  std::int64_t total_credits = 0;
  bool literal_split = false;  ///< Algorithm 1 line 12 applied literally (ablation)
  /// Check credits per packet in the match-action stage (idealised) instead of
  /// having NIC cores poll rule hit counters ([CEIO] Sec. 4.1, the real design).
  bool ideal_detection = false;
  /// NIC-core control loop: one pass over all flows takes base + per_flow * flows.
  Tick poll_base = 200 * kNanosecond;
  Tick poll_per_flow = 10 * kNanosecond;

  /// On-NIC memory used by the elastic buffer (BlueField-3: 16 GB DDR).
  std::uint64_t nic_mem_bytes = 16ULL << 30;
  Tick nic_mem_latency = 500 * kNanosecond;
  double nic_mem_gbps = 400.0;
  /// NIC (ARM) cores serving the slow path. Each flow is pinned to one core ("an
  /// on-NIC core is assigned to monitor this flow", [CEIO] Sec. 4.1); each moved packet
  /// costs it this long. 150 ns ~ 6.7 Mpps per flow, which puts a single flow's
  /// slow path at ~1/3 of the fast path at 512 B and equal at 4 KB ([CEIO] Fig. 11).
  /// BlueField-3 has 16 Arm cores.
  std::uint32_t nic_cores = 16;
  Tick nic_core_per_packet = 150 * kNanosecond;

  enum class Recv {
    kSync,   ///< recv(): fetch slow-path data only when the fast ring is empty, and block
    kAsync,  ///< async_recv(): fetch slow-path data while fast-path packets are processed
  };
  Recv recv = Recv::kAsync;
  std::uint32_t drain_batch = 32;    ///< packets per slow-path fetch request
  std::uint32_t async_window = 64;   ///< max slow-path packets in flight per queue

  /// Active-flow strategy ([CEIO] Sec. 4.1 Q3).
  Tick idle_timeout = kSecond;
  Tick reactivate_period = 100 * kMicrosecond;
  std::uint32_t reactivate_per_tick = 32;

  /// Per-flow NIC-memory backlog above which slow-path packets are ECN-marked:
  /// the slow path is producing faster than it drains ([CEIO] Sec. 4.1). DCTCP-K sized.
  std::uint64_t cc_threshold_bytes = 32 * 1024;
  /// Minimum gap between NIC-generated notifications per flow (DCQCN CNP timer).
  Tick cnp_interval = 50 * kMicrosecond;
};

struct CeioFlowInfo {
  net::FlowId id = 0;
  nic::QueueId queue = 0;
  bool rdma = false;  ///< CPU-bypass flow (RDMA write); drained by NIC cores per message
  Tick start = 0;     ///< connection establishment (credit assignment)
};

struct CeioStats {
  std::uint64_t fast_packets = 0;
  std::uint64_t slow_packets = 0;       ///< buffered in on-NIC memory
  std::uint64_t to_slow = 0;            ///< fast -> slow transitions
  std::uint64_t to_fast = 0;            ///< slow -> fast transitions
  std::uint64_t drain_requests = 0;
  std::uint64_t drained_packets = 0;     ///< fetched by the host library (recv / async_recv)
  std::uint64_t upgraded_packets = 0;    ///< pushed to the fast ring by NIC cores on re-enable
  std::uint64_t held_packets = 0;        ///< arrived while the fast path was paused
  std::uint64_t nic_mem_drops = 0;
  std::uint64_t cc_signals = 0;         ///< packets marked because the slow path backed up
  std::uint64_t deactivations = 0;
  std::uint64_t reactivations = 0;
  double nic_mem_max_bytes = 0;
  std::int64_t min_credit_balance = 0;  ///< most negative balance seen (detection lag)
};

/// CEIO ([CEIO] Sec. 3-Sec. 5): credit-based flow control at the NIC plus elastic
/// buffering in on-NIC memory, with the host library side (credit release on
/// head-pointer update, ordered slow-path fetching behind recv/async_recv).
///
/// Per flow: FAST packets are DMA'd to the host ring (each takes a credit);
/// when credits run out the flow's rule is switched to on-NIC memory (SLOW).
/// A SLOW flow returns to FAST only after its NIC-memory backlog is fully
/// drained and it has credits again (phase exclusivity), so delivery stays in
/// order without per-packet reordering.
class CeioController final : public host::RxExtension {
 public:
  CeioController(Simulator& sim, nic::Nic& nic, const CeioConfig& cfg,
                 const std::vector<CeioFlowInfo>& flows, std::int64_t total_credits);

  /// Installs the NIC hooks and schedules connection setup and control loops.
  void start();

  // host::RxExtension
  PollAction on_poll(host::CpuCore& core, std::size_t fast_ready) override;
  void on_processed(host::CpuCore& core, const nic::RxCompletion& c) override;
  void on_batch_end(host::CpuCore& core) override;

  const CeioStats& stats() const { return stats_; }
  void reset_stats();
  /// Called when the NIC triggers congestion control for a flow ([CEIO] Sec. 4.1).
  void set_congestion_hook(std::function<void(net::FlowId)> h) { on_congestion_ = std::move(h); }
  const CreditManager& credits() const { return cm_; }
  std::uint64_t slow_packets(net::FlowId f) const;
  std::uint64_t nic_mem_used() const { return nic_mem_used_; }
  const CeioConfig& config() const { return cfg_; }

 private:
  enum class Mode { kFast, kSlow };
  struct Flow {
    CeioFlowInfo info;
    Mode mode = Mode::kSlow;  // no credits until the connection is established
    bool established = false;
    bool waiting_reactivation = false;
    std::deque<net::Packet> nic_mem;
    std::uint64_t nic_mem_bytes = 0;
    std::uint64_t fast_hits = 0;   // the fast rule's hit counter
    std::uint64_t hits_seen = 0;   // last value the NIC core read
    Tick last_arrival = 0;
    std::int64_t pending_release = 0;  // released by the host, not yet applied
    std::uint32_t draining = 0;        // host-fetched slow-path packets in flight
    std::uint64_t slow_total = 0;
    // Re-enabling the fast path ([CEIO] Sec. 4.1): pause it, let NIC cores push the
    // NIC-memory backlog to the fast ring, then release the packets held meanwhile.
    bool paused = false;
    std::uint32_t upgrading = 0;       // backlog packets not yet handed to the DMA engine
    std::deque<net::Packet> held;      // on-chip, arrived while paused
    Tick last_cnp = 0;
    bool sent_cnp = false;
  };

  void on_steer(const net::Packet& p, nic::SteeringRule& rule);
  void on_slow_path(net::Packet p, nic::QueueId q);
  void establish(const std::vector<net::FlowId>& ids);
  void control_loop();
  void reactivation_tick();
  void apply_release(Flow& f);
  void update_mode(Flow& f);
  void set_mode(Flow& f, Mode m);
  void start_upgrade(Flow& f);
  void finish_upgrade(Flow& f);
  std::uint32_t pending_slow(nic::QueueId q) const;
  void request_drain(host::CpuCore& core, std::uint32_t n);
  void drain_descriptor(nic::QueueId q, std::vector<std::uint64_t> bufs, host::BufferPool* pool);
  void drain_rdma(Flow& f);
  Tick nic_mem_read(net::FlowId flow, std::uint32_t bytes);
  void release_nic_mem(Flow& f, const net::Packet& p);

  Simulator& sim_;
  nic::Nic& nic_;
  CeioConfig cfg_;
  CreditManager cm_;
  std::map<net::FlowId, Flow> flows_;
  std::unordered_map<nic::QueueId, std::vector<net::FlowId>> by_queue_;
  std::unordered_map<nic::QueueId, std::uint32_t> drain_inflight_;  // requested, not yet completed
  std::unordered_map<nic::QueueId, std::uint32_t> claimed_;         // requested, not yet at the NIC
  std::unordered_map<net::FlowId, std::int64_t> unreleased_;  // host side, per batch
  std::deque<net::FlowId> reactivate_;
  std::uint64_t nic_mem_used_ = 0;
  Tick nic_mem_free_ = 0;
  std::vector<Tick> nic_core_free_;
  CeioStats stats_;
  std::function<void(net::FlowId)> on_congestion_;
};

}  // namespace ceio::policy
