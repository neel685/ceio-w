#include "ceio/sim/simulator.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace ceio {

void Simulator::schedule_at(Tick t, Callback cb) {
  if (t < now_) {
    throw std::invalid_argument("Simulator::schedule_at: time " + std::to_string(t) +
                                " is in the past (now=" + std::to_string(now_) + ")");
  }
  heap_.push_back(Event{t, next_seq_++, std::move(cb)});
  std::push_heap(heap_.begin(), heap_.end(), Later{});
}

EventHandle Simulator::schedule_cancellable(Tick t, Callback cb) {
  auto alive = std::make_shared<bool>(true);
  schedule_at(t, [alive, inner = std::move(cb)]() {
    if (!*alive) return;
    *alive = false;
    inner();
  });
  return EventHandle(std::move(alive));
}

Simulator::Event Simulator::pop() {
  std::pop_heap(heap_.begin(), heap_.end(), Later{});
  Event ev = std::move(heap_.back());
  heap_.pop_back();
  return ev;
}

void Simulator::run() {
  stopped_ = false;
  while (!heap_.empty() && !stopped_) {
    Event ev = pop();
    now_ = ev.time;
    ++processed_;
    ev.cb();
  }
}

void Simulator::run_until(Tick t) {
  stopped_ = false;
  while (!heap_.empty() && !stopped_ && heap_.front().time <= t) {
    Event ev = pop();
    now_ = ev.time;
    ++processed_;
    ev.cb();
  }
  if (!stopped_ && now_ < t) now_ = t;
}

}  // namespace ceio
