#include "ceio/exp/scenario.hpp"

#include <stdexcept>
#include <string>
#include <unordered_set>

namespace ceio::exp {

void Scenario::validate() const {
  auto fail = [this](const std::string& msg) {
    throw std::invalid_argument("Scenario '" + name + "': " + msg);
  };
  if (queues.empty()) fail("needs at least one queue");
  if (measure == 0) fail("measure window must be > 0");
  cache.validate();
  dram.validate();
  pcie.validate();
  for (std::size_t i = 0; i < queues.size(); ++i) {
    const auto& q = queues[i];
    const std::string tag = "queue " + std::to_string(i) + ": ";
    if (q.kind == nic::QueueKind::kDescriptor) {
      if (q.ring_size == 0 || q.pool_buffers == 0) fail(tag + "ring and pool must be non-empty");
      if (q.buffer_bytes < 64) fail(tag + "buffer_bytes must be >= 64");
    } else if (q.mr_bytes == 0) {
      fail(tag + "RDMA queue needs mr_bytes > 0");
    }
  }
  std::unordered_set<net::FlowId> ids;
  for (const auto& f : flows) {
    const std::string tag = "flow " + std::to_string(f.spec.id) + ": ";
    if (!ids.insert(f.spec.id).second) fail(tag + "duplicate flow id");
    if (f.queue >= queues.size()) fail(tag + "queue index out of range");
    const auto& q = queues[f.queue];
    if (q.kind == nic::QueueKind::kDescriptor && f.spec.packet_bytes > q.buffer_bytes)
      fail(tag + "packet larger than the queue's buffers");
    if (q.kind == nic::QueueKind::kRdma && f.spec.packet_bytes > q.mr_bytes)
      fail(tag + "packet larger than the RDMA region");
  }
  if (pressure.gbytes_per_s < 0 || pressure.write_fraction < 0 || pressure.write_fraction > 1)
    fail("invalid memory-pressure settings");
}

Scenario ceio_testbed() {
  Scenario s;
  s.name = "ceio-testbed";
  s.cache.size_bytes = 12ULL << 20;
  s.cache.ways = 12;
  s.cache.ddio_ways = 6;
  s.cache.slices = 8;
  s.dram.channels = 8;
  s.dram.channel_gbytes_per_s = 19.2;  // DDR4-3200 at 75 % efficiency
  s.pcie.link_gbps = 504.0;            // Gen5 x16
  s.link.rate_gbps = 200.0;
  return s;
}

Scenario toasty_testbed() {
  Scenario s;
  s.name = "toasty-testbed";
  s.cache.size_bytes = 45ULL << 20;
  s.cache.ways = 15;
  s.cache.ddio_ways = 2;
  s.cache.slices = 24;
  s.dram.channels = 8;
  s.dram.channel_gbytes_per_s = 26.4;  // DDR5-4400 at 75 % efficiency
  s.pcie.link_gbps = 252.0;            // Gen4 x16 (E810)
  s.link.rate_gbps = 100.0;
  return s;
}

}  // namespace ceio::exp
