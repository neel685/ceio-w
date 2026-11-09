#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace ceio::host {

/// Order in which free buffers are handed out again.
enum class PoolOrder {
  kFifo,  ///< default AF_XDP / DPDK ring mempool: least recently freed first
  kLifo,  ///< [Toasty] Sec. 3.1 / DPDK stack mempool: most recently freed (cache-warm) first
};

/// Fixed pool of equally sized packet buffers in one contiguous region
/// (an AF_XDP UMEM or a DPDK mempool).
class BufferPool {
 public:
  BufferPool(std::uint64_t base, std::uint32_t count, std::uint32_t buffer_bytes, PoolOrder order);

  /// Takes a free buffer, or nullopt if none is free.
  std::optional<std::uint64_t> get();
  /// Returns a buffer. Throws on addresses outside the pool or on double free.
  void put(std::uint64_t addr);

  std::uint32_t free_count() const;
  std::uint32_t capacity() const { return count_; }
  std::uint32_t buffer_bytes() const { return buffer_bytes_; }
  std::uint64_t base() const { return base_; }
  std::uint64_t region_bytes() const { return static_cast<std::uint64_t>(count_) * buffer_bytes_; }
  PoolOrder order() const { return order_; }
  std::uint32_t index_of(std::uint64_t addr) const;
  bool owns(std::uint64_t addr) const;

  /// Number of times each buffer has been handed out ([Toasty] Fig. 4).
  const std::vector<std::uint64_t>& use_counts() const { return uses_; }
  /// Number of distinct buffers handed out since construction.
  std::uint32_t distinct_used() const;

 private:
  std::uint64_t base_;
  std::uint32_t count_;
  std::uint32_t buffer_bytes_;
  PoolOrder order_;
  std::deque<std::uint32_t> free_;  // FIFO: pop front / push back; LIFO: pop back / push back
  std::vector<bool> in_use_;
  std::vector<std::uint64_t> uses_;
};

}  // namespace ceio::host
