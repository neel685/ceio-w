#pragma once

#include <cstdint>
#include <string>

namespace ceio::host {

/// Per-packet (or per-message) work an application does on received data.
///
/// The cost model is: read `read_bytes` of the packet through the cache, do
/// `table_lookups` random line reads in a private table, burn
/// `cycles_per_packet + cycles_per_byte * bytes_read` cycles, modify
/// `write_bytes` in place, then optionally transmit a reply. Values are
/// calibration parameters (docs/spec.md Sec. 3.4).
struct AppProfile {
  enum class Reply {
    kNone,       ///< consume locally; buffer recycled immediately
    kNewBuffer,  ///< build `reply_bytes` in a fresh pool buffer and transmit it (RPC, echo)
    kInPlace,    ///< transmit the received buffer itself (forwarding NFs)
  };

  std::string name = "custom";
  double cycles_per_packet = 200.0;
  double cycles_per_byte = 0.0;
  std::uint32_t read_bytes = 64;     ///< bytes of the packet read (clamped to its size)
  std::uint32_t write_bytes = 0;     ///< bytes modified in place
  std::uint32_t table_lookups = 0;
  std::uint64_t table_bytes = 0;     ///< size of the private lookup table
  Reply reply = Reply::kNone;
  std::uint32_t reply_bytes = 64;
  /// Work happens once per RDMA message completion rather than per packet.
  bool per_message = false;

  // Presets. Workloads from [CEIO] Sec. 6.1 and [Toasty] Sec. 2.
  static AppProfile echo();       ///< dperf-style echo, 64 B acknowledgement
  static AppProfile kv();         ///< eRPC key-value server, 1000 entries, 64 B values
  static AppProfile nat();        ///< header rewrite + flow-table lookup, forward
  static AppProfile ids();        ///< scans the whole payload
  static AppProfile l2fwd();      ///< MAC swap and forward
  static AppProfile rdma_sink();  ///< LineFS-style consumer of RDMA writes (CPU-bypass)
  static AppProfile by_name(const std::string& name);
};

}  // namespace ceio::host
