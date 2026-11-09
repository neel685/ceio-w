#pragma once

#include <cstdint>

#include "ceio/hw/cache.hpp"
#include "ceio/hw/dram.hpp"
#include "ceio/sim/time.hpp"

namespace ceio::hw {

struct MemTiming {
  Tick llc_hit = 20 * kNanosecond;          ///< CPU load-to-use on LLC hit
  Tick io_write_update = 500 * kPicosecond;  ///< IIO service per line on DDIO hit
  Tick io_write_alloc = 1 * kNanosecond;     ///< IIO service per line on DDIO allocate (64 GB/s)
  std::uint32_t cpu_mlp = 10;               ///< max outstanding line fills per core
  Tick cpu_issue_gap = 500 * kPicosecond;   ///< spacing between consecutive line requests
};

/// LLC + DRAM, as seen by CPU cores and by the IIO (device) path.
///
/// Pure timing model: each call returns completion times and updates cache
/// state immediately; callers turn those times into events.
class MemorySystem {
 public:
  MemorySystem(const CacheConfig& cache, const DramConfig& dram, const MemTiming& timing = {});

  /// CPU reads [addr, addr+bytes). Returns when the last line is available.
  Tick cpu_read(Tick now, std::uint64_t addr, std::uint64_t bytes);
  /// CPU writes [addr, addr+bytes) (write-allocate). Returns when all lines are owned.
  Tick cpu_write(Tick now, std::uint64_t addr, std::uint64_t bytes);

  /// IIO commits one inbound DMA line. Returns when the IIO may start the next line
  /// (includes waiting for DRAM to accept a writeback forced by the allocation).
  Tick io_write_line(Tick now, std::uint64_t line);
  /// Device reads one line. Returns when the data is ready to send.
  Tick io_read_line(Tick now, std::uint64_t line);

  /// Metric hint: buffer [addr, addr+bytes) was recycled; its data is dead.
  void mark_dead(std::uint64_t addr, std::uint64_t bytes);

  std::uint32_t line_bytes() const { return cache_.config().line_bytes; }
  SetAssocCache& cache() { return cache_; }
  const SetAssocCache& cache() const { return cache_; }
  Dram& dram() { return dram_; }
  const Dram& dram() const { return dram_; }
  const MemTiming& timing() const { return timing_; }

 private:
  Tick cpu_access(Tick now, std::uint64_t addr, std::uint64_t bytes, bool write);

  SetAssocCache cache_;
  Dram dram_;
  MemTiming timing_;
};

}  // namespace ceio::hw
