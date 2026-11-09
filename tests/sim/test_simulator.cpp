#include "ceio/sim/simulator.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

namespace ceio {

TEST(Simulator, RunsEventsInTimeOrder) {
  Simulator sim;
  std::vector<int> order;
  sim.schedule_at(30, [&] { order.push_back(3); });
  sim.schedule_at(10, [&] { order.push_back(1); });
  sim.schedule_at(20, [&] { order.push_back(2); });
  sim.run();
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
  EXPECT_EQ(sim.now(), 30u);
  EXPECT_EQ(sim.events_processed(), 3u);
  EXPECT_EQ(sim.pending_events(), 0u);
}

TEST(Simulator, EqualTimesRunInSchedulingOrder) {
  Simulator sim;
  std::vector<int> order;
  for (int i = 0; i < 100; ++i) sim.schedule_at(5, [&, i] { order.push_back(i); });
  sim.run();
  ASSERT_EQ(order.size(), 100u);
  for (int i = 0; i < 100; ++i) EXPECT_EQ(order[static_cast<std::size_t>(i)], i);
}

TEST(Simulator, CallbacksCanScheduleMoreEvents) {
  Simulator sim;
  int fired = 0;
  std::function<void()> tick = [&] {
    if (++fired < 10) sim.schedule_in(7, tick);
  };
  sim.schedule_at(0, tick);
  sim.run();
  EXPECT_EQ(fired, 10);
  EXPECT_EQ(sim.now(), 63u);
}

TEST(Simulator, ZeroDelayEventRunsAfterCurrentOnes) {
  Simulator sim;
  std::vector<int> order;
  sim.schedule_at(1, [&] {
    order.push_back(1);
    sim.schedule_in(0, [&] { order.push_back(3); });
  });
  sim.schedule_at(1, [&] { order.push_back(2); });
  sim.run();
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(Simulator, RunUntilIsInclusiveAndAdvancesClock) {
  Simulator sim;
  int fired = 0;
  sim.schedule_at(100, [&] { ++fired; });
  sim.schedule_at(101, [&] { ++fired; });
  sim.run_until(100);
  EXPECT_EQ(fired, 1);
  EXPECT_EQ(sim.now(), 100u);
  sim.run_until(500);
  EXPECT_EQ(fired, 2);
  EXPECT_EQ(sim.now(), 500u);
}

TEST(Simulator, SchedulingInThePastThrows) {
  Simulator sim;
  sim.run_until(50);
  EXPECT_THROW(sim.schedule_at(49, [] {}), std::invalid_argument);
  EXPECT_NO_THROW(sim.schedule_at(50, [] {}));
}

TEST(Simulator, CancelledEventDoesNotFire) {
  Simulator sim;
  int fired = 0;
  EventHandle h = sim.schedule_cancellable(10, [&] { ++fired; });
  EXPECT_TRUE(h.pending());
  h.cancel();
  EXPECT_FALSE(h.pending());
  sim.run();
  EXPECT_EQ(fired, 0);
}

TEST(Simulator, CancelAfterFireIsNoOp) {
  Simulator sim;
  int fired = 0;
  EventHandle h = sim.schedule_cancellable(10, [&] { ++fired; });
  sim.run();
  EXPECT_FALSE(h.pending());
  h.cancel();
  EXPECT_EQ(fired, 1);
  EventHandle empty;
  empty.cancel();  // default handle is safe
  EXPECT_FALSE(empty.pending());
}

TEST(Simulator, StopEndsRunEarly) {
  Simulator sim;
  int fired = 0;
  sim.schedule_at(1, [&] {
    ++fired;
    sim.stop();
  });
  sim.schedule_at(2, [&] { ++fired; });
  sim.run();
  EXPECT_EQ(fired, 1);
  EXPECT_EQ(sim.pending_events(), 1u);
  sim.run();  // resumes
  EXPECT_EQ(fired, 2);
}

TEST(Simulator, ExceptionsPropagateFromCallbacks) {
  Simulator sim;
  sim.schedule_at(1, [] { throw std::runtime_error("boom"); });
  EXPECT_THROW(sim.run(), std::runtime_error);
}

}  // namespace ceio
