#include "ceio/hw/cache.hpp"

#include <stdexcept>

#include "ceio/sim/rng.hpp"

namespace ceio::hw {

LineRange lines_of(std::uint64_t addr, std::uint64_t bytes, std::uint32_t line_bytes) {
  if (bytes == 0) return {addr / line_bytes, 0};
  const std::uint64_t first = addr / line_bytes;
  const std::uint64_t last = (addr + bytes - 1) / line_bytes;
  return {first, last - first + 1};
}

void CacheConfig::validate() const {
  if (line_bytes == 0 || (line_bytes & (line_bytes - 1)) != 0)
    throw std::invalid_argument("CacheConfig: line_bytes must be a power of two");
  if (ways == 0 || ways > 64) throw std::invalid_argument("CacheConfig: ways must be in [1, 64]");
  if (ddio_ways == 0 || ddio_ways > ways)
    throw std::invalid_argument("CacheConfig: ddio_ways must be in [1, ways]");
  if (size_bytes % (static_cast<std::uint64_t>(ways) * line_bytes) != 0 || sets() == 0)
    throw std::invalid_argument("CacheConfig: size_bytes must be a positive multiple of ways*line_bytes");
  if (index == Index::kSliced && (slices == 0 || sets() % slices != 0))
    throw std::invalid_argument("CacheConfig: sets must be divisible by slices");
}

double CacheStats::io_write_hit_rate() const {
  const auto total = io_write_hits + io_write_allocs;
  return total ? static_cast<double>(io_write_hits) / static_cast<double>(total) : 0.0;
}

double CacheStats::cpu_read_hit_rate() const {
  const auto total = cpu_read_hits + cpu_read_misses;
  return total ? static_cast<double>(cpu_read_hits) / static_cast<double>(total) : 0.0;
}

SetAssocCache::SetAssocCache(const CacheConfig& cfg) : cfg_(cfg) {
  cfg_.validate();
  sets_ = cfg_.sets();
  ways_.resize(sets_ * cfg_.ways);
}

std::uint64_t SetAssocCache::set_of(std::uint64_t line) const {
  switch (cfg_.index) {
    case CacheConfig::Index::kSliced: {
      const std::uint64_t per = sets_ / cfg_.slices;
      return (mix64(line) % cfg_.slices) * per + line % per;
    }
    case CacheConfig::Index::kHash:
      return mix64(line) % sets_;
    case CacheConfig::Index::kModulo:
      break;
  }
  return line % sets_;
}

SetAssocCache::Way* SetAssocCache::find(std::uint64_t line, std::uint32_t* way_out) {
  Way* base = &ways_[set_of(line) * cfg_.ways];
  for (std::uint32_t w = 0; w < cfg_.ways; ++w) {
    if ((base[w].flags & kValid) && base[w].tag == line) {
      if (way_out) *way_out = w;
      return &base[w];
    }
  }
  return nullptr;
}

const SetAssocCache::Way* SetAssocCache::find(std::uint64_t line) const {
  return const_cast<SetAssocCache*>(this)->find(line, nullptr);
}

void SetAssocCache::set_io_unread(Way& w, bool on) {
  const bool was = (w.flags & kIoUnread) != 0;
  if (on && !was) {
    w.flags |= kIoUnread;
    ++io_unread_lines_;
  } else if (!on && was) {
    w.flags = static_cast<std::uint8_t>(w.flags & ~kIoUnread);
    --io_unread_lines_;
  }
}

SetAssocCache::Way& SetAssocCache::allocate(std::uint64_t set, std::uint32_t lo, std::uint32_t hi,
                                            std::uint64_t line, CacheAccess& out) {
  Way* base = &ways_[set * cfg_.ways];
  Way* victim = nullptr;
  for (std::uint32_t w = lo; w < hi; ++w) {
    if (!(base[w].flags & kValid)) {
      victim = &base[w];
      break;
    }
    if (!victim || base[w].stamp < victim->stamp) victim = &base[w];
  }
  if (victim->flags & kValid) {
    ++stats_.evictions;
    if (victim->flags & kDirty) {
      ++stats_.writebacks;
      if (victim->flags & kDead) ++stats_.dead_writebacks;
      out.writeback_line = victim->tag;
    }
    if (victim->flags & kIoUnread) ++stats_.leaky_evictions;
    set_io_unread(*victim, false);
  } else {
    ++resident_;
  }
  victim->tag = line;
  victim->flags = kValid;
  victim->stamp = ++clock_;
  return *victim;
}

CacheAccess SetAssocCache::io_write(std::uint64_t line) {
  CacheAccess out;
  std::uint32_t way = 0;
  if (Way* w = find(line, &way)) {
    out.hit = true;
    ++stats_.io_write_hits;
    if (way < cfg_.ways - cfg_.ddio_ways) ++stats_.io_write_hits_non_ddio;
    w->flags = static_cast<std::uint8_t>((w->flags | kDirty) & ~kDead);
    set_io_unread(*w, true);
    w->stamp = ++clock_;
    return out;
  }
  ++stats_.io_write_allocs;
  Way& w = allocate(set_of(line), cfg_.ways - cfg_.ddio_ways, cfg_.ways, line, out);
  w.flags |= kDirty;
  set_io_unread(w, true);
  return out;
}

CacheAccess SetAssocCache::io_read(std::uint64_t line) {
  CacheAccess out;
  if (Way* w = find(line)) {
    out.hit = true;
    ++stats_.io_read_hits;
    w->stamp = ++clock_;
  } else {
    ++stats_.io_read_misses;
  }
  return out;
}

CacheAccess SetAssocCache::cpu_read(std::uint64_t line) {
  CacheAccess out;
  if (Way* w = find(line)) {
    out.hit = true;
    ++stats_.cpu_read_hits;
    set_io_unread(*w, false);
    w->stamp = ++clock_;
    return out;
  }
  ++stats_.cpu_read_misses;
  allocate(set_of(line), 0, cfg_.ways, line, out);
  return out;
}

CacheAccess SetAssocCache::cpu_write(std::uint64_t line) {
  CacheAccess out;
  if (Way* w = find(line)) {
    out.hit = true;
    ++stats_.cpu_write_hits;
    w->flags = static_cast<std::uint8_t>((w->flags | kDirty) & ~kDead);
    w->stamp = ++clock_;
    return out;
  }
  ++stats_.cpu_write_misses;
  Way& w = allocate(set_of(line), 0, cfg_.ways, line, out);
  w.flags |= kDirty;
  return out;
}

void SetAssocCache::mark_dead(std::uint64_t line) {
  if (Way* w = find(line)) {
    w->flags |= kDead;
    set_io_unread(*w, false);
  }
}

void SetAssocCache::invalidate(std::uint64_t line) {
  if (Way* w = find(line)) {
    set_io_unread(*w, false);
    w->flags = 0;
    --resident_;
  }
}

bool SetAssocCache::contains(std::uint64_t line) const { return find(line) != nullptr; }

bool SetAssocCache::is_dirty(std::uint64_t line) const {
  const Way* w = find(line);
  return w && (w->flags & kDirty);
}

}  // namespace ceio::hw
