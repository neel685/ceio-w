#include "ceio/hw/dram.hpp"

#include <algorithm>
#include <stdexcept>

#include "ceio/sim/rng.hpp"

namespace ceio::hw {

void DramConfig::validate() const {
  if (channels == 0) throw std::invalid_argument("DramConfig: channels must be > 0");
  if (!(channel_gbytes_per_s > 0.0))
    throw std::invalid_argument("DramConfig: channel bandwidth must be > 0");
  if (line_bytes == 0) throw std::invalid_argument("DramConfig: line_bytes must be > 0");
}

double DramStats::mean_read_latency_ns() const {
  return reads ? to_ns(total_read_latency) / static_cast<double>(reads) : 0.0;
}

Dram::Dram(const DramConfig& cfg) : cfg_(cfg) {
  cfg_.validate();
  // GB/s -> Gbps for transfer_time().
  service_ = transfer_time(cfg_.line_bytes, cfg_.channel_gbytes_per_s * 8.0);
  free_at_.assign(cfg_.channels, 0);
}

std::size_t Dram::channel_of(std::uint64_t line) const {
  return static_cast<std::size_t>(mix64(line ^ 0x5bd1e995ULL) % cfg_.channels);
}

Tick Dram::channel_free_at(std::uint64_t line) const { return free_at_[channel_of(line)]; }

DramAccess Dram::access(Tick now, std::uint64_t line) {
  Tick& free = free_at_[channel_of(line)];
  const Tick start = std::max(now, free);
  const Tick wait = start - now;
  const Tick slack = static_cast<Tick>(cfg_.write_queue_lines) * service_;
  free = start + service_;
  return {now + (wait > slack ? wait - slack : 0), free, free + cfg_.idle_latency};
}

DramAccess Dram::read(Tick now, std::uint64_t line) {
  const DramAccess a = access(now, line);
  ++stats_.reads;
  stats_.bytes_read += cfg_.line_bytes;
  stats_.total_read_latency += a.completed - now;
  return a;
}

DramAccess Dram::write(Tick now, std::uint64_t line) {
  const DramAccess a = access(now, line);
  ++stats_.writes;
  stats_.bytes_written += cfg_.line_bytes;
  return a;
}

}  // namespace ceio::hw
