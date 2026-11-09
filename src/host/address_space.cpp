#include "ceio/host/address_space.hpp"

#include <stdexcept>

namespace ceio::host {

std::uint64_t AddressSpace::allocate(const std::string& name, std::uint64_t bytes, std::uint64_t align) {
  if (bytes == 0) throw std::invalid_argument("AddressSpace: zero-size region '" + name + "'");
  if (align == 0 || (align & (align - 1)) != 0)
    throw std::invalid_argument("AddressSpace: alignment must be a power of two");
  const std::uint64_t base = (next_ + align - 1) & ~(align - 1);
  next_ = base + bytes;
  regions_.push_back(Region{name, base, bytes});
  return base;
}

const Region* AddressSpace::find(std::uint64_t addr) const {
  for (const auto& r : regions_)
    if (r.contains(addr)) return &r;
  return nullptr;
}

}  // namespace ceio::host
