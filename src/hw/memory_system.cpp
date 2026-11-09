#include "ceio/hw/memory_system.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace ceio::hw {

MemorySystem::MemorySystem(const CacheConfig& cache, const DramConfig& dram, const MemTiming& timing)
    : cache_(cache), dram_(dram), timing_(timing) {
  if (cache.line_bytes != dram.line_bytes)
    throw std::invalid_argument("MemorySystem: cache and DRAM line sizes differ");
  if (timing_.cpu_mlp == 0) throw std::invalid_argument("MemorySystem: cpu_mlp must be > 0");
}

Tick MemorySystem::cpu_access(Tick now, std::uint64_t addr, std::uint64_t bytes, bool write) {
  const LineRange r = lines_of(addr, bytes, line_bytes());
  if (r.count == 0) return now;
  // Ring of completion times for the last `cpu_mlp` requests: request i cannot
  // issue before request i - mlp has completed (finite line-fill buffers).
  std::vector<Tick> slots(std::min<std::uint64_t>(timing_.cpu_mlp, r.count), now);
  Tick last_done = now;
  for (std::uint64_t i = 0; i < r.count; ++i) {
    const std::uint64_t line = r.first + i;
    Tick& slot = slots[i % slots.size()];
    const Tick issue = std::max(now + i * timing_.cpu_issue_gap, slot);
    const CacheAccess acc = write ? cache_.cpu_write(line) : cache_.cpu_read(line);
    Tick done;
    if (acc.hit) {
      done = issue + timing_.llc_hit;
    } else {
      if (acc.writeback_line) dram_.write(issue, *acc.writeback_line);
      done = dram_.read(issue, line).completed;  // demand fill or RFO
    }
    slot = done;
    last_done = std::max(last_done, done);
  }
  return last_done;
}

Tick MemorySystem::cpu_read(Tick now, std::uint64_t addr, std::uint64_t bytes) {
  return cpu_access(now, addr, bytes, false);
}

Tick MemorySystem::cpu_write(Tick now, std::uint64_t addr, std::uint64_t bytes) {
  return cpu_access(now, addr, bytes, true);
}

Tick MemorySystem::io_write_line(Tick now, std::uint64_t line) {
  const CacheAccess acc = cache_.io_write(line);
  Tick t = now + (acc.hit ? timing_.io_write_update : timing_.io_write_alloc);
  if (acc.writeback_line) t = std::max(t, dram_.write(now, *acc.writeback_line).admitted);
  return t;
}

Tick MemorySystem::io_read_line(Tick now, std::uint64_t line) {
  const CacheAccess acc = cache_.io_read(line);
  return acc.hit ? now + timing_.llc_hit : dram_.read(now, line).completed;
}

void MemorySystem::mark_dead(std::uint64_t addr, std::uint64_t bytes) {
  const LineRange r = lines_of(addr, bytes, line_bytes());
  for (std::uint64_t i = 0; i < r.count; ++i) cache_.mark_dead(r.first + i);
}

}  // namespace ceio::hw
