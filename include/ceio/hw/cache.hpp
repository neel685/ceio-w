#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace ceio::hw {

/// Line-granular byte address helpers.
struct LineRange {
  std::uint64_t first;  ///< first line index (byte address / line size)
  std::uint64_t count;  ///< number of lines touched
};
LineRange lines_of(std::uint64_t addr, std::uint64_t bytes, std::uint32_t line_bytes);

struct CacheConfig {
  std::uint64_t size_bytes = 12ULL << 20;  ///< total LLC capacity
  std::uint32_t ways = 12;
  std::uint32_t line_bytes = 64;
  /// Number of ways I/O (DDIO) writes may allocate into (the highest-numbered ways).
  std::uint32_t ddio_ways = 6;
  enum class Index {
    /// Intel-style: slice = hash(line) % slices, set within slice = line % sets_per_slice.
    /// Low address bits pick the set directly, so buffers whose packets only fill the
    /// first lines of each page alias onto a fraction of the sets.
    kSliced,
    kHash,    ///< set = hash(line) % sets (idealised uniform spreading)
    kModulo,  ///< set = line % sets (analytical tests)
  };
  Index index = Index::kSliced;
  std::uint32_t slices = 8;

  std::uint64_t sets() const { return size_bytes / (static_cast<std::uint64_t>(ways) * line_bytes); }
  std::uint64_t sets_per_slice() const { return sets() / slices; }
  std::uint64_t ddio_bytes() const { return sets() * ddio_ways * line_bytes; }
  void validate() const;  ///< throws std::invalid_argument on inconsistent values
};

struct CacheStats {
  std::uint64_t io_write_hits = 0;      ///< DDIO write-update (PCIe write hit)
  std::uint64_t io_write_hits_non_ddio = 0;  ///< subset of hits outside DDIO ways ("DMA bloating")
  std::uint64_t io_write_allocs = 0;    ///< DDIO write-allocate (PCIe write miss)
  std::uint64_t io_read_hits = 0;
  std::uint64_t io_read_misses = 0;
  std::uint64_t cpu_read_hits = 0;
  std::uint64_t cpu_read_misses = 0;
  std::uint64_t cpu_write_hits = 0;
  std::uint64_t cpu_write_misses = 0;
  std::uint64_t evictions = 0;
  std::uint64_t writebacks = 0;          ///< dirty evictions (DRAM writes)
  std::uint64_t dead_writebacks = 0;     ///< writebacks of recycled-buffer lines (wasted)
  std::uint64_t leaky_evictions = 0;     ///< I/O-written lines evicted before any CPU read

  double io_write_hit_rate() const;
  double cpu_read_hit_rate() const;
};

/// Result of one line access.
struct CacheAccess {
  bool hit = false;
  /// Line index written back to DRAM because of this access, if any.
  std::optional<std::uint64_t> writeback_line;
};

/// Set-associative last-level cache with DDIO way partitioning and true LRU.
///
/// Semantics are specified in docs/spec.md Sec. 4. All functions take *line
/// indices* (byte address / line_bytes); use lines_of() to convert ranges.
class SetAssocCache {
 public:
  explicit SetAssocCache(const CacheConfig& cfg);

  /// Device (DDIO) write. Hit anywhere -> update in place; miss -> allocate in DDIO ways.
  CacheAccess io_write(std::uint64_t line);
  /// Device read (e.g. TX DMA). Never allocates.
  CacheAccess io_read(std::uint64_t line);
  /// CPU load. Allocates on miss (caller fetches from DRAM).
  CacheAccess cpu_read(std::uint64_t line);
  /// CPU store (write-allocate). Marks dirty.
  CacheAccess cpu_write(std::uint64_t line);

  /// Metric hint: the buffer owning `line` was recycled, so its contents are dead.
  void mark_dead(std::uint64_t line);
  /// Drops a line without writeback (e.g. modelling a clean-invalidate hint).
  void invalidate(std::uint64_t line);

  bool contains(std::uint64_t line) const;
  bool is_dirty(std::uint64_t line) const;
  /// Number of resident lines written by I/O and not yet read by a CPU.
  std::uint64_t io_unread_lines() const { return io_unread_lines_; }
  std::uint64_t resident_lines() const { return resident_; }

  const CacheConfig& config() const { return cfg_; }
  const CacheStats& stats() const { return stats_; }
  void reset_stats() { stats_ = {}; }

 private:
  enum Flag : std::uint8_t { kValid = 1, kDirty = 2, kIoUnread = 4, kDead = 8 };
  struct Way {
    std::uint64_t tag = 0;   // full line index
    std::uint64_t stamp = 0; // LRU timestamp; larger = more recent
    std::uint8_t flags = 0;
  };

  std::uint64_t set_of(std::uint64_t line) const;
  Way* find(std::uint64_t line, std::uint32_t* way_out = nullptr);
  const Way* find(std::uint64_t line) const;
  /// Picks a victim in ways [lo, hi) of `set` (invalid first, then LRU), evicts it, returns it.
  Way& allocate(std::uint64_t set, std::uint32_t lo, std::uint32_t hi, std::uint64_t line,
                CacheAccess& out);
  void set_io_unread(Way& w, bool on);

  CacheConfig cfg_;
  std::uint64_t sets_;
  std::vector<Way> ways_;  // sets_ * cfg_.ways, row-major by set
  std::uint64_t clock_ = 0;
  std::uint64_t io_unread_lines_ = 0;
  std::uint64_t resident_ = 0;
  CacheStats stats_;
};

}  // namespace ceio::hw
