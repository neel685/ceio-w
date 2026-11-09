#include "ceio/policy/credit_manager.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

#include "ceio/sim/rng.hpp"

namespace ceio::policy {

TEST(CreditManager, FirstFlowsSplitTheTotal) {
  CreditManager cm(3000);
  cm.add_flows({1, 2, 3});
  EXPECT_EQ(cm.credits(1), 1000);
  EXPECT_EQ(cm.credits(2), 1000);
  EXPECT_EQ(cm.credits(3), 1000);
  EXPECT_EQ(cm.free_pool(), 0);
  EXPECT_TRUE(cm.invariant_ok());
}

TEST(CreditManager, RemaindersGoToLowestIds) {
  CreditManager cm(10);
  cm.add_flows({5, 7, 9});
  EXPECT_EQ(cm.credits(5), 4);
  EXPECT_EQ(cm.credits(7), 3);
  EXPECT_EQ(cm.credits(9), 3);
}

TEST(CreditManager, NewFlowGetsEqualShareFromRichFlows) {
  // [CEIO] Sec. 4.1 example: C_total = 3000; one flow, then a second joins.
  CreditManager cm(3000);
  cm.add_flows({1});
  EXPECT_EQ(cm.credits(1), 3000);
  cm.add_flows({2});
  EXPECT_EQ(cm.credits(1), 1500);
  EXPECT_EQ(cm.credits(2), 1500);
  cm.add_flows({3, 4});  // n = 2, m = 2: C_flow = 750, share = 375 per pair
  for (net::FlowId f : {1u, 2u, 3u, 4u}) EXPECT_EQ(cm.credits(f), 750) << f;
  EXPECT_TRUE(cm.invariant_ok());
}

TEST(CreditManager, ShortFlowGivesWhatItHasAndOwesTheRest) {
  CreditManager cm(1000);
  cm.add_flows({1, 2});
  cm.consume(1, 450);  // flow 1 has 50 left, needs to give 2 * (333/2) = 332
  cm.add_flows({3, 4});
  // C_flow = 250, share = 125. Flow 2 (500) pays 125 to each newcomer.
  EXPECT_EQ(cm.credits(2), 250);
  // Flow 1 splits its 50 (25 each) and owes 100 to each newcomer.
  EXPECT_EQ(cm.credits(1), 0);
  EXPECT_EQ(cm.credits(3), 150);
  EXPECT_EQ(cm.credits(4), 150);
  EXPECT_EQ(cm.debt_of(1), 200);
  EXPECT_EQ(cm.owed_to(3), 100);
  EXPECT_TRUE(cm.invariant_ok());

  // Releases repay the debt first (min(o_ij, gamma/m) per creditor), then refill flow 1.
  cm.release(1, 100);
  EXPECT_EQ(cm.credits(3), 200);
  EXPECT_EQ(cm.credits(4), 200);
  EXPECT_EQ(cm.debt_of(1), 100);
  EXPECT_EQ(cm.credits(1), 0);
  cm.release(1, 300);
  EXPECT_EQ(cm.debt_of(1), 0);
  EXPECT_EQ(cm.credits(1), 200);
  EXPECT_EQ(cm.outstanding(1), 50);
  EXPECT_TRUE(cm.invariant_ok());
}

TEST(CreditManager, LiteralSplitDecaysGeometrically) {
  CreditManager uniform(1000), literal(1000, true);
  for (CreditManager* cm : {&uniform, &literal}) {
    cm->add_flows({1, 2});
    cm->consume(1, 400);  // 100 left
    cm->add_flows({3, 4});
  }
  // Flow 2 gives each newcomer 125. Flow 1 (100 left) gives 50 + 50 uniformly,
  // or 100/2 then 50/2 when line 12 is applied literally.
  EXPECT_EQ(uniform.credits(3), 175);
  EXPECT_EQ(uniform.credits(4), 175);
  EXPECT_EQ(literal.credits(3), 175);
  EXPECT_EQ(literal.credits(4), 150);
  EXPECT_EQ(literal.debt_of(1), 75 + 100);
  EXPECT_TRUE(literal.invariant_ok());
}

TEST(CreditManager, ConsumeAndReleaseRoundTrip) {
  CreditManager cm(100);
  cm.add_flows({1});
  cm.consume(1, 120);  // late detection: balance goes negative
  EXPECT_EQ(cm.credits(1), -20);
  EXPECT_EQ(cm.outstanding(1), 120);
  EXPECT_TRUE(cm.invariant_ok());
  cm.release(1, 120);
  EXPECT_EQ(cm.credits(1), 100);
  EXPECT_EQ(cm.outstanding(1), 0);
}

TEST(CreditManager, DeactivationRecyclesCreditsAndOutstandingReleases) {
  CreditManager cm(900);
  cm.add_flows({1, 2, 3});
  cm.consume(1, 100);
  cm.deactivate(1);  // its 200 remaining credits go to flows 2 and 3
  EXPECT_FALSE(cm.active(1));
  EXPECT_EQ(cm.credits(2), 400);
  EXPECT_EQ(cm.credits(3), 400);
  cm.release(1, 100);  // in-flight packets of the inactive flow finish later
  EXPECT_EQ(cm.credits(2), 450);
  EXPECT_EQ(cm.credits(3), 450);
  EXPECT_EQ(cm.credits(1), 0);
  EXPECT_TRUE(cm.invariant_ok());
  cm.add_flows({1});  // re-activation through Algorithm 1
  EXPECT_EQ(cm.credits(1), 300);
  EXPECT_EQ(cm.active_count(), 3u);
  EXPECT_TRUE(cm.invariant_ok());
}

TEST(CreditManager, LastFlowLeavingParksCreditsInFreePool) {
  CreditManager cm(64);
  cm.add_flows({1});
  cm.deactivate(1);
  EXPECT_EQ(cm.free_pool(), 64);
  cm.add_flows({2});
  EXPECT_EQ(cm.credits(2), 64);
  EXPECT_EQ(cm.free_pool(), 0);
}

TEST(CreditManager, MisuseThrows) {
  EXPECT_THROW(CreditManager(0), std::invalid_argument);
  CreditManager cm(10);
  cm.add_flows({1});
  EXPECT_THROW(cm.add_flows({1}), std::logic_error);
  EXPECT_THROW(cm.consume(9), std::out_of_range);
}

TEST(CreditManager, RandomOperationsPreserveInvariant) {
  // Property test: any interleaving of joins, leaves, consumes and releases
  // conserves credits and never leaves debt with an inactive party.
  Rng rng(2024);
  CreditManager cm(3000);
  std::map<net::FlowId, std::int64_t> out;  // outstanding we may release
  for (int step = 0; step < 20000; ++step) {
    const auto f = static_cast<net::FlowId>(rng.uniform_int(1, 40));
    switch (rng.uniform_int(0, 3)) {
      case 0:
        if (!cm.active(f)) cm.add_flows({f});
        break;
      case 1:
        if (cm.active(f)) cm.deactivate(f);
        break;
      case 2:
        if (cm.known(f)) {
          const auto n = static_cast<std::int64_t>(rng.uniform_int(1, 50));
          cm.consume(f, n);
          out[f] += n;
        }
        break;
      case 3:
        if (out[f] > 0) {
          const auto g = static_cast<std::int64_t>(rng.uniform_int(1, static_cast<std::uint64_t>(out[f])));
          cm.release(f, g);
          out[f] -= g;
        }
        break;
    }
    ASSERT_TRUE(cm.invariant_ok()) << "step " << step;
    ASSERT_EQ(cm.outstanding(f), out[f]);
    if (!cm.active(f)) {
      ASSERT_EQ(cm.debt_of(f), 0);
    }
  }
}

}  // namespace ceio::policy
