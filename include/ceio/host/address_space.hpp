#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ceio::host {

struct Region {
  std::string name;
  std::uint64_t base;
  std::uint64_t bytes;
  bool contains(std::uint64_t addr) const { return addr >= base && addr < base + bytes; }
};

/// Bump allocator for the simulated physical address space. Every buffer,
/// ring and table has a real, non-overlapping address so that cache set
/// mapping and footprint effects are faithful.
class AddressSpace {
 public:
  explicit AddressSpace(std::uint64_t base = 1ULL << 32) : next_(base) {}

  /// Allocates `bytes` aligned to `align` (a power of two). Returns the base address.
  std::uint64_t allocate(const std::string& name, std::uint64_t bytes, std::uint64_t align = 4096);

  const std::vector<Region>& regions() const { return regions_; }
  /// Region containing `addr`, or nullptr.
  const Region* find(std::uint64_t addr) const;

 private:
  std::uint64_t next_;
  std::vector<Region> regions_;
};

}  // namespace ceio::host
