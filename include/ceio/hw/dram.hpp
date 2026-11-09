#pragma once

#include <cstdint>
#include <vector>

#include "ceio/sim/time.hpp"

namespace ceio::hw {

struct DramConfig {
  std::uint32_t channels = 8;
  /// Effective per-channel bandwidth in GB/s (bytes, not bits).
  double channel_gbytes_per_s = 19.2;
  Tick idle_latency = 90 * kNanosecond;
  std::uint32_t line_bytes = 64;
  /// Posted-write queue depth per channel, in lines. A writer is only blocked
  /// when the channel backlog exceeds this many line transfers.
  std::uint32_t write_queue_lines = 32;
  void validate() const;
};

struct DramStats {
  std::uint64_t reads = 0;
  std::uint64_t writes = 0;
  std::uint64_t bytes_read = 0;
  std::uint64_t bytes_written = 0;
  Tick total_read_latency = 0;  ///< sum over reads of (completion - request)
  double mean_read_latency_ns() const;
};

/// Timing of a single DRAM line access.
struct DramAccess {
  Tick admitted;   ///< when a posted write enters the channel's write queue (writer unblocked)
  Tick accepted;   ///< when the channel finished transferring the line (bandwidth slot)
  Tick completed;  ///< when the data is available to the requester (reads)
};

/// Bandwidth- and latency-limited DRAM. Lines are interleaved across channels
/// by address hash; each channel is a FIFO server with a fixed per-line service
/// time, so queueing delay grows under load and co-runner pressure.
///
/// This is a pure timing model: it does not schedule events itself.
class Dram {
 public:
  explicit Dram(const DramConfig& cfg);

  DramAccess read(Tick now, std::uint64_t line);
  /// Writes are posted: requesters wait only for `admitted`.
  DramAccess write(Tick now, std::uint64_t line);

  Tick line_service_time() const { return service_; }
  /// Earliest time the channel serving `line` becomes free.
  Tick channel_free_at(std::uint64_t line) const;
  const DramConfig& config() const { return cfg_; }
  const DramStats& stats() const { return stats_; }
  void reset_stats() { stats_ = {}; }

 private:
  DramAccess access(Tick now, std::uint64_t line);
  std::size_t channel_of(std::uint64_t line) const;

  DramConfig cfg_;
  Tick service_;
  std::vector<Tick> free_at_;
  DramStats stats_;
};

}  // namespace ceio::hw
