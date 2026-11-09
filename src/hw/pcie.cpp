#include "ceio/hw/pcie.hpp"

#include <algorithm>
#include <stdexcept>

namespace ceio::hw {

void PcieConfig::validate() const {
  if (!(link_gbps > 0.0)) throw std::invalid_argument("PcieConfig: link_gbps must be > 0");
  if (max_payload == 0 || (max_payload & (max_payload - 1)) != 0)
    throw std::invalid_argument("PcieConfig: max_payload must be a power of two");
  if (iio_credit_bytes < max_payload)
    throw std::invalid_argument("PcieConfig: iio_credit_bytes must hold at least one max-size TLP");
}

PcieHost::PcieHost(Simulator& sim, MemorySystem& mem, const PcieConfig& cfg)
    : sim_(sim), mem_(mem), cfg_(cfg), credits_free_(cfg.iio_credit_bytes), iio_occ_(sim.now()) {
  cfg_.validate();
}

Tick PcieHost::send(Tick& link_free, std::uint32_t wire_bytes) {
  const Tick start = std::max(sim_.now(), link_free);
  link_free = start + transfer_time(wire_bytes, cfg_.link_gbps);
  return link_free + cfg_.one_way_latency;
}

void PcieHost::dma_write(std::uint64_t addr, std::uint32_t bytes, Callback done) {
  if (bytes == 0) throw std::invalid_argument("PcieHost::dma_write: zero-length write");
  ++stats_.dma_writes;
  stats_.dma_write_bytes += bytes;
  queued_bytes_ += bytes;
  pending_.push_back(WriteReq{addr, bytes, 0, std::move(done)});
  pump();
}

void PcieHost::pump() {
  while (!pending_.empty()) {
    WriteReq& req = pending_.front();
    const std::uint64_t cur = req.addr + req.sent;
    const std::uint64_t to_boundary = cfg_.max_payload - (cur % cfg_.max_payload);
    const auto payload =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(to_boundary, req.bytes - req.sent));
    if (credits_free_ < payload) {
      if (!stalled_) ++stats_.credit_stalls;
      stalled_ = true;
      return;
    }
    stalled_ = false;
    credits_free_ -= payload;
    queued_bytes_ -= payload;
    req.sent += payload;
    ++stats_.write_tlps;

    Tlp tlp{cur, payload, nullptr};
    if (req.sent == req.bytes) {
      tlp.done = std::move(req.done);
      pending_.pop_front();
    }
    const Tick arrival = send(up_free_, payload + cfg_.tlp_overhead);
    sim_.schedule_at(arrival, [this, t = std::move(tlp)]() mutable { iio_arrive(std::move(t)); });
  }
}

void PcieHost::iio_arrive(Tlp tlp) {
  iio_bytes_ += tlp.bytes;
  iio_occ_.set(sim_.now(), static_cast<double>(iio_bytes_));
  iio_queue_.push_back(std::move(tlp));
  if (!iio_busy_) iio_next();
}

void PcieHost::iio_next() {
  if (iio_queue_.empty()) {
    iio_busy_ = false;
    return;
  }
  iio_busy_ = true;
  Tlp tlp = std::move(iio_queue_.front());
  iio_queue_.pop_front();

  const LineRange r = lines_of(tlp.addr, tlp.bytes, mem_.line_bytes());
  Tick t = sim_.now();
  for (std::uint64_t i = 0; i < r.count; ++i) t = mem_.io_write_line(t, r.first + i);

  sim_.schedule_at(t, [this, t2 = std::move(tlp)]() mutable {
    iio_bytes_ -= t2.bytes;
    iio_occ_.set(sim_.now(), static_cast<double>(iio_bytes_));
    const std::uint32_t freed = t2.bytes;
    sim_.schedule_in(cfg_.credit_return_latency, [this, freed]() {
      credits_free_ += freed;
      pump();
    });
    if (t2.done) t2.done();
    iio_next();
  });
}

void PcieHost::dma_read(std::uint64_t addr, std::uint32_t bytes, Callback done) {
  if (bytes == 0) throw std::invalid_argument("PcieHost::dma_read: zero-length read");
  ++stats_.dma_reads;
  stats_.dma_read_bytes += bytes;
  // Read request TLP travels upstream (header only).
  const Tick at_host = send(up_free_, cfg_.tlp_overhead);
  sim_.schedule_at(at_host, [this, addr, bytes, cb = std::move(done)]() mutable {
    const LineRange r = lines_of(addr, bytes, mem_.line_bytes());
    Tick ready = sim_.now();
    for (std::uint64_t i = 0; i < r.count; ++i)
      ready = std::max(ready, mem_.io_read_line(sim_.now(), r.first + i));
    sim_.schedule_at(ready, [this, bytes, cb2 = std::move(cb)]() mutable {
      // Completions travel downstream in MPS-sized TLPs.
      Tick arrival = sim_.now();
      std::uint32_t left = bytes;
      while (left > 0) {
        const std::uint32_t chunk = std::min(left, cfg_.max_payload);
        arrival = send(down_free_, chunk + cfg_.tlp_overhead);
        left -= chunk;
      }
      sim_.schedule_at(arrival, std::move(cb2));
    });
  });
}

void PcieHost::mmio_write(std::uint32_t bytes, Callback done) {
  sim_.schedule_at(send(down_free_, bytes + cfg_.tlp_overhead), std::move(done));
}

Tick PcieHost::mmio_read_latency() const {
  return 2 * cfg_.one_way_latency + 2 * transfer_time(cfg_.tlp_overhead + 8, cfg_.link_gbps);
}

}  // namespace ceio::hw
