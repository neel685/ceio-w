#include "ceio/host/buffer_pool.hpp"

#include <stdexcept>
#include <string>

namespace ceio::host {

BufferPool::BufferPool(std::uint64_t base, std::uint32_t count, std::uint32_t buffer_bytes, PoolOrder order)
    : base_(base), count_(count), buffer_bytes_(buffer_bytes), order_(order), in_use_(count, false), uses_(count, 0) {
  if (count == 0 || buffer_bytes == 0) throw std::invalid_argument("BufferPool: empty pool");
  // LIFO pops from the back, so store indices reversed: the initial handout order
  // (0, 1, 2, ...) is then the same for both policies.
  for (std::uint32_t i = 0; i < count; ++i) free_.push_back(order_ == PoolOrder::kLifo ? count - 1 - i : i);
}

bool BufferPool::owns(std::uint64_t addr) const {
  return addr >= base_ && addr < base_ + region_bytes() && (addr - base_) % buffer_bytes_ == 0;
}

std::uint32_t BufferPool::index_of(std::uint64_t addr) const {
  if (!owns(addr)) throw std::invalid_argument("BufferPool: address " + std::to_string(addr) + " not in pool");
  return static_cast<std::uint32_t>((addr - base_) / buffer_bytes_);
}

std::optional<std::uint64_t> BufferPool::get() {
  if (free_.empty()) return std::nullopt;
  std::uint32_t idx;
  if (order_ == PoolOrder::kFifo) {
    idx = free_.front();
    free_.pop_front();
  } else {
    idx = free_.back();
    free_.pop_back();
  }
  in_use_[idx] = true;
  ++uses_[idx];
  return base_ + static_cast<std::uint64_t>(idx) * buffer_bytes_;
}

void BufferPool::put(std::uint64_t addr) {
  const std::uint32_t idx = index_of(addr);
  if (!in_use_[idx]) throw std::logic_error("BufferPool: double free of buffer " + std::to_string(idx));
  in_use_[idx] = false;
  free_.push_back(idx);
}

std::uint32_t BufferPool::free_count() const { return static_cast<std::uint32_t>(free_.size()); }

std::uint32_t BufferPool::distinct_used() const {
  std::uint32_t n = 0;
  for (auto u : uses_) n += u > 0;
  return n;
}

}  // namespace ceio::host
