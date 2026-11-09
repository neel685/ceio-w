#pragma once

#include <cstdint>
#include <deque>
#include <functional>

#include "ceio/hw/memory_system.hpp"
#include "ceio/sim/simulator.hpp"
#include "ceio/sim/stats.hpp"

namespace ceio::hw {

struct PcieConfig {
  double link_gbps = 504.0;            ///< per-direction data rate (Gen5 x16 after 128b/130b)
  std::uint32_t max_payload = 256;     ///< MPS: TLPs split on MPS-aligned boundaries
  std::uint32_t tlp_overhead = 24;     ///< framing + header + LCRC + amortised DLLPs, bytes
  Tick one_way_latency = 300 * kNanosecond;
  /// Posted-write credits (bytes) advertised by the IIO buffer.
  std::uint64_t iio_credit_bytes = 64 * 1024;
  /// Delay between the IIO freeing space and the NIC seeing the credit.
  Tick credit_return_latency = 300 * kNanosecond;
  void validate() const;
};

struct PcieStats {
  std::uint64_t dma_writes = 0;
  std::uint64_t dma_write_bytes = 0;
  std::uint64_t write_tlps = 0;
  std::uint64_t dma_reads = 0;
  std::uint64_t dma_read_bytes = 0;
  std::uint64_t credit_stalls = 0;  ///< times the NIC had data but no posted credits
};

/// PCIe link between one NIC and the host, plus the host IIO write buffer.
///
/// Upstream posted writes (NIC -> host) are split into MPS-sized TLPs, each
/// needing IIO credits before it is sent. The IIO commits TLPs in order, one
/// line at a time, through MemorySystem::io_write_line(); slow commits (e.g.
/// DDIO allocations that force DRAM writebacks) back up the IIO, exhaust
/// credits and stall the NIC. IIO occupancy is the HostCC congestion signal.
class PcieHost {
 public:
  using Callback = std::function<void()>;

  PcieHost(Simulator& sim, MemorySystem& mem, const PcieConfig& cfg = {});

  /// NIC writes [addr, addr+bytes) to host memory. `done` runs once every line is
  /// committed by the IIO (i.e. visible to CPUs). Completions run in issue order.
  void dma_write(std::uint64_t addr, std::uint32_t bytes, Callback done);
  /// NIC reads [addr, addr+bytes) from host memory; `done` runs when the last
  /// completion reaches the NIC.
  void dma_read(std::uint64_t addr, std::uint32_t bytes, Callback done);
  /// CPU posted write to a NIC register (e.g. doorbell); `done` runs on arrival at the NIC.
  void mmio_write(std::uint32_t bytes, Callback done);
  /// Round-trip latency of a CPU read of a NIC register (uncontended).
  Tick mmio_read_latency() const;

  std::uint64_t credits_in_use() const { return cfg_.iio_credit_bytes - credits_free_; }
  std::uint64_t iio_occupancy_bytes() const { return iio_bytes_; }
  std::uint64_t queued_write_bytes() const { return queued_bytes_; }
  const TimeWeighted& iio_occupancy() const { return iio_occ_; }
  const PcieStats& stats() const { return stats_; }
  const PcieConfig& config() const { return cfg_; }

 private:
  struct WriteReq {
    std::uint64_t addr;
    std::uint32_t bytes;
    std::uint32_t sent = 0;
    Callback done;
  };
  struct Tlp {
    std::uint64_t addr;
    std::uint32_t bytes;
    Callback done;  // set only on the last TLP of a request
  };

  void pump();
  void iio_arrive(Tlp tlp);
  void iio_next();
  Tick send(Tick& link_free, std::uint32_t wire_bytes);

  Simulator& sim_;
  MemorySystem& mem_;
  PcieConfig cfg_;

  std::deque<WriteReq> pending_;
  std::uint64_t queued_bytes_ = 0;
  std::uint64_t credits_free_;
  bool stalled_ = false;

  std::deque<Tlp> iio_queue_;
  bool iio_busy_ = false;
  std::uint64_t iio_bytes_ = 0;
  TimeWeighted iio_occ_;

  Tick up_free_ = 0;    // upstream link (NIC -> host) free time
  Tick down_free_ = 0;  // downstream link (host -> NIC) free time
  PcieStats stats_;
};

}  // namespace ceio::hw
