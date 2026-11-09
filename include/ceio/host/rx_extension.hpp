#pragma once

#include <cstddef>

#include "ceio/nic/nic.hpp"

namespace ceio::host {

class CpuCore;

/// Driver-level policy plugged into a CpuCore (e.g. the CEIO library: credit
/// release and slow-path fetching behind recv()/async_recv()).
class RxExtension {
 public:
  enum class PollAction {
    kNone,  ///< nothing pending beyond the completions already visible
    kWait,  ///< data is being fetched for this core: keep busy-waiting for it
  };
  virtual ~RxExtension() = default;

  /// Called on every poll with the number of completions already visible.
  /// May start fetching slow-path data for this core.
  virtual PollAction on_poll(CpuCore& core, std::size_t fast_ready) = 0;
  /// Called after the application finished one completion.
  virtual void on_processed(CpuCore& core, const nic::RxCompletion& c) = 0;
  /// Called after a batch, before the driver refills the ring (e.g. head-pointer update).
  virtual void on_batch_end(CpuCore& core) = 0;
};

}  // namespace ceio::host
