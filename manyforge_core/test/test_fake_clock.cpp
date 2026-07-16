#include <gtest/gtest.h>

#include <manyforge_core/time/fake_clock.hpp>
#include <thread>

namespace manyforge::core {

TEST(FakeClockTest, StartsAtZero) {
  FakeClock clock;
  EXPECT_EQ(clock.now(), 0);
}

TEST(FakeClockTest, AdvanceAddsDuration) {
  FakeClock clock;
  EXPECT_EQ(clock.now(), 0);

  clock.advance(100);
  EXPECT_EQ(clock.now(), 100);

  clock.advance(50);
  EXPECT_EQ(clock.now(), 150);
}

TEST(FakeClockTest, AdvanceWithNegativeDuration) {
  FakeClock clock;
  clock.set(100);
  clock.advance(-30);
  EXPECT_EQ(clock.now(), 70);
}

TEST(FakeClockTest, SetToAbsoluteTime) {
  FakeClock clock;
  EXPECT_EQ(clock.now(), 0);

  clock.set(1000);
  EXPECT_EQ(clock.now(), 1000);

  clock.set(500);
  EXPECT_EQ(clock.now(), 500);
}

TEST(FakeClockTest, AdvanceThenSet) {
  FakeClock clock;
  clock.advance(100);
  EXPECT_EQ(clock.now(), 100);

  clock.set(50);
  EXPECT_EQ(clock.now(), 50);

  clock.advance(25);
  EXPECT_EQ(clock.now(), 75);
}

TEST(FakeClockTest, SetThenAdvance) {
  FakeClock clock;
  clock.set(1000);
  EXPECT_EQ(clock.now(), 1000);

  clock.advance(200);
  EXPECT_EQ(clock.now(), 1200);
}

TEST(FakeClockTest, MultipleAdvances) {
  FakeClock clock;
  for (int i = 0; i < 10; ++i) {
    clock.advance(10);
  }
  EXPECT_EQ(clock.now(), 100);
}

TEST(FakeClockTest, LargeDurations) {
  FakeClock clock;
  clock.advance(1'000'000'000'000LL);  // 1 trillion nanoseconds
  EXPECT_EQ(clock.now(), 1'000'000'000'000LL);

  clock.advance(-500'000'000'000LL);
  EXPECT_EQ(clock.now(), 500'000'000'000LL);
}

TEST(FakeClockTest, ConcurrentAdvancesRemainRaceFreeAndExact) {
  FakeClock clock;
  std::thread first([&clock]() {
    for (int i = 0; i < 10'000; ++i) {
      clock.advance(1);
    }
  });
  std::thread second([&clock]() {
    for (int i = 0; i < 10'000; ++i) {
      clock.advance(1);
    }
  });

  first.join();
  second.join();
  EXPECT_EQ(clock.now(), 20'000);
}

}  // namespace manyforge::core
