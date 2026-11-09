#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "ceio/host/app.hpp"
#include "ceio/host/buffer_pool.hpp"
#include "ceio/host/refill.hpp"
#include "ceio/host/rx_extension.hpp"
#include "ceio/hw/memory_system.hpp"
#include "ceio/hw/pcie.hpp"
#include "ceio/nic/nic.hpp"
#include "ceio/sim/rng.hpp"
#include "ceio/sim/simulator.hpp"

namespace ceio::host {

class CpuCore;

/// Driver state of one receive ring, shared by every core that serves it
/// (one core normally; several for ShRing's shared receive ring).
struct SharedRing {
  std::uint32_t in_flight_posts = 0;  ///< descriptors written but not yet seen by the NIC
  std::uint64_t desc_index = 0;
  std::vector<CpuCore*> cores;
};

struct CoreConfig {
  double ghz = 2.8;
  std::uint32_t batch = 32;           ///< max completions taken per poll
  double poll_cycles = 60;            ///< fixed cost of one poll iteration
  double refill_cycles_per_buf = 8;   ///< driver cost per posted descriptor
  std::uint32_t descriptor_bytes = 16;
  Tick wake_latency = 20 * kNanosecond;  ///< busy-poll loop granularity when idle
  /// While processing packet i, prefetch packet i+1's CQE and first
  /// `prefetch_bytes` (as DPDK/AF_XDP loops do). Hides LLC hits, not DRAM misses.
  bool prefetch_next = true;
  std::uint32_t prefetch_bytes = 128;
  Tick l1_latency = 1 * kNanosecond;  ///< cost of touching already-prefetched data
  /// Transmit replies as soon as they are built ([Toasty] Sec. 3.2) instead of in one
  /// burst at the end of the RX batch (default AF_XDP / DPDK tx_burst behaviour).
  bool tx_eager = false;
};

struct CoreStats {
  std::uint64_t packets = 0;        ///< completions processed (packets or RDMA messages)
  std::uint64_t bytes = 0;
  std::uint64_t replies = 0;
  std::uint64_t reply_drops = 0;    ///< no free buffer for a reply
  std::uint64_t polls = 0;
  std::uint64_t refills = 0;
  std::uint64_t buffers_posted = 0;
  std::uint64_t llc_read_hits = 0;    ///< this core's CPU loads that hit the LLC
  std::uint64_t llc_read_misses = 0;  ///< ... and that missed (e.g. leaky-DMA victims)
  Tick busy = 0;
  double llc_read_hit_rate() const {
    const auto n = llc_read_hits + llc_read_misses;
    return n ? static_cast<double>(llc_read_hits) / static_cast<double>(n) : 0.0;
  }
};

/// One polling CPU core running a driver + application on one receive queue
/// (run-to-completion, as in DPDK / AF_XDP busy-poll mode).
///
/// Each packet is processed in its own event so that cache state interleaves
/// with concurrent DMA at packet granularity.
class CpuCore {
 public:
  /// Called once per processed completion with the time processing finished.
  using DoneHook = std::function<void(const nic::RxCompletion&, Tick done)>;

  CpuCore(Simulator& sim, hw::MemorySystem& mem, nic::Nic& nic, nic::QueueId queue,
          BufferPool* pool, const AppProfile& app, const CoreConfig& cfg, Rng rng,
          std::uint64_t table_base, std::uint64_t desc_ring_base, double link_gbps,
          std::shared_ptr<SharedRing> ring = nullptr);

  void set_refill_policy(std::unique_ptr<RefillPolicy> p) { refill_ = std::move(p); }
  void set_done_hook(DoneHook h) { on_done_ = std::move(h); }
  void set_extension(RxExtension* ext) { ext_ = ext; }

  /// Posts the initial descriptors and starts polling.
  void start();

  const CoreStats& stats() const { return stats_; }
  void reset_stats() { stats_ = {}; busy_since_ = sim_.now(); }
  /// Busy time including the current busy period up to now.
  Tick busy_time() const;
  nic::QueueId queue() const { return queue_; }
  BufferPool* pool() const { return pool_; }
  const AppProfile& app() const { return app_; }

 private:
  void wake();
  void poll();
  void next();
  void finish(const nic::RxCompletion& c, std::optional<std::uint64_t> tx_buf);
  Tick process(Tick t, const nic::RxCompletion& c, std::uint32_t prefetched,
               std::optional<std::uint64_t>& tx_buf);
  Tick refill(Tick t);
  void recycle(std::uint64_t buf);
  void transmit(std::uint64_t buf, std::uint32_t bytes);
  void flush_tx();
  void go_idle();
  Tick cycles(double c) const;

  Simulator& sim_;
  hw::MemorySystem& mem_;
  nic::Nic& nic_;
  nic::QueueId queue_;
  BufferPool* pool_;  // nullptr for RDMA queues
  AppProfile app_;
  CoreConfig cfg_;
  Rng rng_;
  std::uint64_t table_base_;
  std::uint64_t desc_base_;
  double link_gbps_;
  std::unique_ptr<RefillPolicy> refill_;
  DoneHook on_done_;
  RxExtension* ext_ = nullptr;
  bool waiting_ext_ = false;  // busy-waiting for data an extension is fetching

  std::deque<nic::RxCompletion> batch_;
  struct Prefetch {
    std::uint64_t cqe_addr;
    Tick ready;
    std::uint32_t bytes;
  };
  std::optional<Prefetch> prefetched_;
  bool idle_ = true;
  bool wake_pending_ = false;
  Tick busy_since_ = 0;
  std::shared_ptr<SharedRing> ring_;
  std::vector<std::pair<std::uint64_t, std::uint32_t>> pending_tx_;  // batched replies
  std::uint32_t recycled_since_refill_ = 0;
  std::uint64_t dmaed_at_last_refill_ = 0;
  Tick last_refill_ = 0;
  CoreStats stats_;
};

/// Memory-intensive co-runner (Intel MLC-like, [Toasty] Sec. 4.3): issues random
/// line reads/writes over a private region at a fixed bandwidth, polluting the
/// LLC and consuming DRAM bandwidth.
class MemoryHog {
 public:
  MemoryHog(Simulator& sim, hw::MemorySystem& mem, std::uint64_t base, std::uint64_t bytes,
            double gbytes_per_s, double write_fraction, Rng rng);
  void start();
  /// Changes the co-runner's bandwidth (e.g. HostCC's MBA-style host-local response).
  void set_gbytes_per_s(double gbs);
  double gbytes_per_s() const { return gbs_; }

 private:
  void tick();
  Simulator& sim_;
  hw::MemorySystem& mem_;
  std::uint64_t base_;
  std::uint64_t lines_;
  double write_fraction_;
  Rng rng_;
  double gbs_;
  Tick period_;
  bool running_ = false;
  static constexpr std::uint32_t kLinesPerTick = 16;
};

}  // namespace ceio::host
