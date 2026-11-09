#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "ceio/sim/time.hpp"

namespace ceio {

/// Handle to an event scheduled with Simulator::schedule_cancellable().
/// Cancelling after the event fired, or cancelling twice, is a harmless no-op.
class EventHandle {
 public:
  EventHandle() = default;
  void cancel() {
    if (alive_) *alive_ = false;
  }
  bool pending() const { return alive_ && *alive_; }

 private:
  friend class Simulator;
  explicit EventHandle(std::shared_ptr<bool> alive) : alive_(std::move(alive)) {}
  std::shared_ptr<bool> alive_;
};

/// Single-threaded discrete-event kernel.
///
/// Events run in non-decreasing time order; events with equal time run in the
/// order they were scheduled. This makes every run bit-for-bit deterministic.
class Simulator {
 public:
  using Callback = std::function<void()>;

  Simulator() = default;
  Simulator(const Simulator&) = delete;
  Simulator& operator=(const Simulator&) = delete;

  Tick now() const { return now_; }

  /// Schedules `cb` at absolute time `t`. Throws std::invalid_argument if t < now().
  void schedule_at(Tick t, Callback cb);
  /// Schedules `cb` at now() + delay.
  void schedule_in(Tick delay, Callback cb) { schedule_at(now_ + delay, std::move(cb)); }

  /// Like schedule_at(), but returns a handle that can cancel the event.
  EventHandle schedule_cancellable(Tick t, Callback cb);

  /// Runs until no events remain or stop() is called.
  void run();
  /// Runs every event with time <= `t`, then advances now() to `t` (unless stopped).
  void run_until(Tick t);
  /// Makes the current run()/run_until() return after the current event.
  void stop() { stopped_ = true; }

  std::uint64_t events_processed() const { return processed_; }
  std::size_t pending_events() const { return heap_.size(); }

 private:
  struct Event {
    Tick time;
    std::uint64_t seq;
    Callback cb;
  };
  struct Later {
    bool operator()(const Event& a, const Event& b) const {
      return a.time != b.time ? a.time > b.time : a.seq > b.seq;
    }
  };

  Event pop();

  std::vector<Event> heap_;
  Tick now_ = 0;
  std::uint64_t next_seq_ = 0;
  std::uint64_t processed_ = 0;
  bool stopped_ = false;
};

}  // namespace ceio
