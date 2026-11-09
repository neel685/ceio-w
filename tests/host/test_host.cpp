#include <gtest/gtest.h>

#include <stdexcept>

#include "ceio/host/address_space.hpp"
#include "ceio/host/app.hpp"
#include "ceio/host/buffer_pool.hpp"

namespace ceio::host {

TEST(AddressSpace, AlignedAndDisjoint) {
  AddressSpace as(1000);
  const auto a = as.allocate("a", 100, 4096);
  const auto b = as.allocate("b", 5000, 2ULL << 20);
  EXPECT_EQ(a % 4096, 0u);
  EXPECT_EQ(b % (2ULL << 20), 0u);
  EXPECT_GE(b, a + 100);
  ASSERT_NE(as.find(b + 10), nullptr);
  EXPECT_EQ(as.find(b + 10)->name, "b");
  EXPECT_EQ(as.find(a + 200), nullptr);
  EXPECT_THROW(as.allocate("z", 0), std::invalid_argument);
  EXPECT_THROW(as.allocate("z", 10, 3), std::invalid_argument);
}

TEST(BufferPool, FifoReusesLeastRecentlyFreed) {
  BufferPool p(0x10000, 4, 2048, PoolOrder::kFifo);
  auto a = *p.get(), b = *p.get();
  EXPECT_EQ(a, 0x10000u);
  EXPECT_EQ(b, 0x10000u + 2048);
  p.put(a);
  // Buffers 2 and 3 were free before `a`, so they come first.
  EXPECT_EQ(*p.get(), 0x10000u + 2 * 2048);
  EXPECT_EQ(*p.get(), 0x10000u + 3 * 2048);
  EXPECT_EQ(*p.get(), a);
  EXPECT_FALSE(p.get().has_value());
}

TEST(BufferPool, LifoReusesMostRecentlyFreed) {
  BufferPool p(0, 4, 4096, PoolOrder::kLifo);
  auto a = *p.get();
  EXPECT_EQ(a, 0u);  // initial order matches FIFO
  auto b = *p.get();
  p.put(a);
  p.put(b);
  EXPECT_EQ(*p.get(), b);  // warmest first
  EXPECT_EQ(*p.get(), a);
}

TEST(BufferPool, LifoKeepsWorkingSetSmall) {
  // [Toasty] Fig. 4: with a steady hand-out/return cycle, LIFO touches only as many
  // buffers as are outstanding, while FIFO cycles through the whole pool.
  for (PoolOrder order : {PoolOrder::kFifo, PoolOrder::kLifo}) {
    BufferPool p(0, 1024, 2048, order);
    std::vector<std::uint64_t> held;
    for (int i = 0; i < 100'000; ++i) {
      held.push_back(*p.get());
      if (held.size() > 8) {
        p.put(held.front());
        held.erase(held.begin());
      }
    }
    if (order == PoolOrder::kFifo)
      EXPECT_EQ(p.distinct_used(), 1024u);
    else
      EXPECT_LE(p.distinct_used(), 9u);
  }
}

TEST(BufferPool, RejectsForeignAndDoubleFree) {
  BufferPool p(4096, 2, 1024, PoolOrder::kFifo);
  auto a = *p.get();
  EXPECT_THROW(p.put(a + 1), std::invalid_argument);
  EXPECT_THROW(p.put(0), std::invalid_argument);
  p.put(a);
  EXPECT_THROW(p.put(a), std::logic_error);
  EXPECT_EQ(p.index_of(4096 + 1024), 1u);
}

TEST(AppProfile, PresetsResolveByName) {
  for (const char* n : {"echo", "kv", "nat", "ids", "l2fwd", "rdma_sink"})
    EXPECT_EQ(AppProfile::by_name(n).name, n);
  EXPECT_THROW(AppProfile::by_name("nope"), std::invalid_argument);
  EXPECT_TRUE(AppProfile::rdma_sink().per_message);
}

}  // namespace ceio::host
