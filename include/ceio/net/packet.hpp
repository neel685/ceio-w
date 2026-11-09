#pragma once

#include <cstdint>

#include "ceio/sim/time.hpp"

namespace ceio::net {

using FlowId = std::uint32_t;

/// One Ethernet frame as seen by the receiver.
struct Packet {
  std::uint64_t id = 0;        ///< globally unique, increasing in send order
  FlowId flow = 0;
  std::uint32_t bytes = 0;     ///< frame size written to host memory
  std::uint64_t seq = 0;       ///< per-flow sequence number
  std::uint64_t msg_id = 0;    ///< per-flow message number
  std::uint32_t msg_pkts = 1;  ///< packets in this message
  std::uint32_t msg_index = 0; ///< position within the message (0 = first)
  bool msg_last = true;        ///< last packet of its message
  bool ecn = false;            ///< CE-marked by the switch
  bool slow = false;           ///< took CEIO's slow path (buffered in on-NIC memory)
  Tick sent = 0;               ///< when the sender emitted it
  Tick nic_arrival = 0;        ///< when the last bit reached the NIC
};

/// Bytes of preamble + SFD + inter-frame gap occupying the wire per frame.
inline constexpr std::uint32_t kWireOverhead = 20;

}  // namespace ceio::net
