#include "ceio/host/core.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace ceio::host {

CpuCore::CpuCore(Simulator& sim, hw::MemorySystem& mem, nic::Nic& nic, nic::QueueId queue,
                 BufferPool* pool, const AppProfile& app, const CoreConfig& cfg, Rng rng,
                 std::uint64_t table_base, std::uint64_t desc_ring_base, double link_gbps)
    : sim_(sim),
      mem_(mem),
      nic_(nic),
      queue_(queue),
      pool_(pool),
      app_(app),
      cfg_(cfg),
      rng_(rng),
      table_base_(table_base),
      desc_base_(desc_ring_base),
      link_gbps_(link_gbps),
      refill_(std::make_unique<KeepFullRefill>()) {
  const auto kind = nic_.queue(queue_).config().kind;
  if (kind == nic::QueueKind::kDescriptor && !pool_)
    throw std::invalid_argument("CpuCore: descriptor queue needs a buffer pool");
  if (app_.table_lookups > 0 && app_.table_bytes < 64)
    throw std::invalid_argument("CpuCore: app has lookups but no table");
  if (!(cfg_.ghz > 0.0)) throw std::invalid_argument("CpuCore: ghz must be > 0");
  nic_.queue(queue_).set_notify([this] { wake(); });
}

Tick CpuCore::cycles(double c) const { return from_ns(c / cfg_.ghz); }

Tick CpuCore::busy_time() const { return stats_.busy + (idle_ ? 0 : sim_.now() - busy_since_); }

void CpuCore::start() {
  last_refill_ = sim_.now();
  dmaed_at_last_refill_ = nic_.queue(queue_).dma_total();
  idle_ = false;
  busy_since_ = sim_.now();
  const Tick t = refill(sim_.now());
  sim_.schedule_at(t, [this] { poll(); });
}

void CpuCore::wake() {
  if (!idle_ || wake_pending_) return;
  wake_pending_ = true;
  sim_.schedule_in(cfg_.wake_latency, [this] {
    wake_pending_ = false;
    idle_ = false;
    busy_since_ = sim_.now();
    poll();
  });
}

void CpuCore::go_idle() {
  if (idle_) return;
  stats_.busy += sim_.now() - busy_since_;
  idle_ = true;
}

void CpuCore::poll() {
  ++stats_.polls;
  auto& cq = nic_.queue(queue_).completions();
  if (cq.empty()) return go_idle();
  const std::size_t n = std::min<std::size_t>(cfg_.batch, cq.size());
  for (std::size_t i = 0; i < n; ++i) {
    batch_.push_back(cq.front());
    cq.pop_front();
  }
  sim_.schedule_in(cycles(cfg_.poll_cycles), [this] { next(); });
}

void CpuCore::next() {
  if (batch_.empty()) {
    const Tick t = refill(sim_.now());
    sim_.schedule_at(t, [this] { poll(); });
    return;
  }
  const nic::RxCompletion c = batch_.front();
  batch_.pop_front();
  const Tick now = sim_.now();
  // The simulator is single-threaded, so cache-stat deltas around this packet's
  // accesses belong to this core.
  const hw::CacheStats before = mem_.cache().stats();

  Tick t;
  std::uint32_t prefetched = 0;
  if (prefetched_ && prefetched_->cqe_addr == c.cqe_addr) {
    t = std::max(now, prefetched_->ready) + cfg_.l1_latency;
    prefetched = prefetched_->bytes;
  } else {
    t = mem_.cpu_read(now, c.cqe_addr, 64);
  }
  prefetched_.reset();
  if (cfg_.prefetch_next && !batch_.empty()) {
    const nic::RxCompletion& n = batch_.front();
    const std::uint32_t pf = app_.per_message ? 0 : std::min({cfg_.prefetch_bytes, app_.read_bytes, n.bytes});
    Tick ready = mem_.cpu_read(now, n.cqe_addr, 64);
    if (pf > 0) ready = std::max(ready, mem_.cpu_read(now, n.buf_addr, pf));
    prefetched_ = Prefetch{n.cqe_addr, ready, pf};
  }

  std::optional<std::uint64_t> tx;
  t = process(t, c, prefetched, tx);
  const hw::CacheStats& after = mem_.cache().stats();
  stats_.llc_read_hits += after.cpu_read_hits - before.cpu_read_hits;
  stats_.llc_read_misses += after.cpu_read_misses - before.cpu_read_misses;
  sim_.schedule_at(t, [this, c, tx] {
    finish(c, tx);
    next();
  });
}

Tick CpuCore::process(Tick t, const nic::RxCompletion& c, std::uint32_t prefetched,
                      std::optional<std::uint64_t>& tx) {
  const std::uint32_t rd = std::min(app_.read_bytes, c.bytes);
  if (rd > prefetched) t = mem_.cpu_read(t, c.buf_addr + prefetched, rd - prefetched);
  if (app_.table_lookups > 0) {
    const std::uint64_t lines = app_.table_bytes / 64;
    for (std::uint32_t i = 0; i < app_.table_lookups; ++i)
      t = mem_.cpu_read(t, table_base_ + rng_.uniform_int(0, lines - 1) * 64, 64);
  }
  t += cycles(app_.cycles_per_packet + app_.cycles_per_byte * rd);
  if (app_.write_bytes > 0) t = mem_.cpu_store(t, c.buf_addr, std::min(app_.write_bytes, c.bytes));
  if (!app_.per_message && app_.reply == AppProfile::Reply::kNewBuffer) {
    tx = pool_->get();
    if (tx) t = mem_.cpu_store(t, *tx, app_.reply_bytes);
  }
  return t;
}

void CpuCore::finish(const nic::RxCompletion& c, std::optional<std::uint64_t> tx) {
  ++stats_.packets;
  stats_.bytes += c.bytes;
  if (on_done_) on_done_(c, sim_.now());
  if (app_.per_message || !pool_) return;

  switch (app_.reply) {
    case AppProfile::Reply::kNone:
      recycle(c.buf_addr);
      break;
    case AppProfile::Reply::kInPlace: {
      ++stats_.replies;
      const std::uint64_t buf = c.buf_addr;
      nic_.pcie().dma_read(buf, c.bytes, [this, buf] { recycle(buf); });
      break;
    }
    case AppProfile::Reply::kNewBuffer:
      recycle(c.buf_addr);
      if (tx) {
        ++stats_.replies;
        const std::uint64_t buf = *tx;
        nic_.pcie().dma_read(buf, app_.reply_bytes, [this, buf] { recycle(buf); });
      } else {
        ++stats_.reply_drops;
      }
      break;
  }
}

void CpuCore::recycle(std::uint64_t buf) {
  mem_.mark_dead(buf, pool_->buffer_bytes());
  pool_->put(buf);
  ++recycled_since_refill_;
}

Tick CpuCore::refill(Tick t) {
  if (!pool_) return t;
  auto& q = nic_.queue(queue_);
  RefillContext ctx;
  ctx.ring_capacity = q.config().ring_size;
  ctx.ring_available = q.posted() + in_flight_posts_;
  ctx.pool_free = pool_->free_count();
  ctx.recycled_since_last = recycled_since_refill_;
  ctx.dmaed_since_last = static_cast<std::uint32_t>(q.dma_total() - dmaed_at_last_refill_);
  ctx.since_last = sim_.now() - last_refill_;
  ctx.link_gbps = link_gbps_;
  ctx.packet_bytes = stats_.packets ? static_cast<std::uint32_t>(stats_.bytes / stats_.packets) : 0;

  recycled_since_refill_ = 0;
  dmaed_at_last_refill_ = q.dma_total();
  last_refill_ = sim_.now();

  const std::uint32_t room = ctx.ring_capacity - ctx.ring_available;
  const std::uint32_t n = std::min({refill_->to_post(ctx), ctx.pool_free, room});
  if (n == 0) return t;

  std::vector<std::uint64_t> addrs;
  addrs.reserve(n);
  for (std::uint32_t i = 0; i < n; ++i) addrs.push_back(*pool_->get());
  // Descriptor writes: one contiguous run in the ring, split where it wraps.
  const std::uint64_t ring = ctx.ring_capacity;
  const std::uint64_t first = desc_index_ % ring;
  const std::uint64_t run1 = std::min<std::uint64_t>(n, ring - first);
  t = mem_.cpu_store(t, desc_base_ + first * cfg_.descriptor_bytes, run1 * cfg_.descriptor_bytes);
  if (run1 < n) t = mem_.cpu_store(t, desc_base_, (n - run1) * cfg_.descriptor_bytes);
  desc_index_ += n;
  t += cycles(cfg_.refill_cycles_per_buf * n);
  in_flight_posts_ += n;
  ++stats_.refills;
  stats_.buffers_posted += n;
  sim_.schedule_at(t, [this, addrs = std::move(addrs), n]() mutable {
    nic_.pcie().mmio_write(8, [this, addrs = std::move(addrs), n] {
      in_flight_posts_ -= n;
      nic_.post_buffers(queue_, addrs);
    });
  });
  return t;
}

MemoryHog::MemoryHog(Simulator& sim, hw::MemorySystem& mem, std::uint64_t base, std::uint64_t bytes,
                     double gbytes_per_s, double write_fraction, Rng rng)
    : sim_(sim),
      mem_(mem),
      base_(base),
      lines_(bytes / 64),
      write_fraction_(write_fraction),
      rng_(rng),
      period_(gbytes_per_s > 0 ? transfer_time(kLinesPerTick * 64ULL, gbytes_per_s * 8.0) : 0) {
  if (lines_ == 0) throw std::invalid_argument("MemoryHog: region too small");
}

void MemoryHog::start() {
  if (period_ > 0) sim_.schedule_in(0, [this] { tick(); });
}

void MemoryHog::tick() {
  for (std::uint32_t i = 0; i < kLinesPerTick; ++i) {
    const std::uint64_t addr = base_ + rng_.uniform_int(0, lines_ - 1) * 64;
    if (rng_.bernoulli(write_fraction_))
      mem_.cpu_write(sim_.now(), addr, 64);
    else
      mem_.cpu_read(sim_.now(), addr, 64);
  }
  sim_.schedule_in(period_, [this] { tick(); });
}

}  // namespace ceio::host
