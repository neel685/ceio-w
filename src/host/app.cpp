#include "ceio/host/app.hpp"

#include <stdexcept>

namespace ceio::host {

AppProfile AppProfile::echo() {
  AppProfile a;
  a.name = "echo";
  a.cycles_per_packet = 120;
  a.read_bytes = 64;
  a.reply = Reply::kNewBuffer;
  a.reply_bytes = 64;
  return a;
}

AppProfile AppProfile::kv() {
  AppProfile a;
  a.name = "kv";
  a.cycles_per_packet = 350;
  a.read_bytes = UINT32_MAX;  // whole request
  a.table_lookups = 1;
  a.table_bytes = 1000 * 128;  // 1000 entries of key + 64 B value, padded
  a.reply = Reply::kNewBuffer;
  a.reply_bytes = 64;
  return a;
}

AppProfile AppProfile::nat() {
  AppProfile a;
  a.name = "nat";
  a.cycles_per_packet = 51;
  a.read_bytes = 64;
  a.write_bytes = 64;
  a.table_lookups = 1;
  a.table_bytes = 1ULL << 20;
  a.reply = Reply::kInPlace;
  return a;
}

AppProfile AppProfile::ids() {
  AppProfile a;
  a.name = "ids";
  a.cycles_per_packet = 100;
  a.cycles_per_byte = 0.25;
  a.read_bytes = UINT32_MAX;
  return a;
}

AppProfile AppProfile::l2fwd() {
  AppProfile a;
  a.name = "l2fwd";
  a.cycles_per_packet = 90;
  a.read_bytes = 64;
  a.write_bytes = 64;
  a.reply = Reply::kInPlace;
  return a;
}

AppProfile AppProfile::rdma_sink() {
  AppProfile a;
  a.name = "rdma_sink";
  a.cycles_per_packet = 2000;  // per message: log append + replication bookkeeping
  a.read_bytes = 0;            // data is not touched by the CPU
  a.per_message = true;
  return a;
}

AppProfile AppProfile::by_name(const std::string& name) {
  if (name == "echo") return echo();
  if (name == "kv") return kv();
  if (name == "nat") return nat();
  if (name == "ids") return ids();
  if (name == "l2fwd") return l2fwd();
  if (name == "rdma_sink") return rdma_sink();
  throw std::invalid_argument("unknown app profile '" + name + "'");
}

}  // namespace ceio::host
