#pragma once

// Only the separately compiled collision-retirement test enables this latch.
#ifdef MANYFORGE_CORE_LOOKAHEAD_TIMING
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
namespace manyforge::core::detail {
inline std::atomic<bool> pre_execute_enabled{false};
inline std::atomic<bool> pre_execute_entered{false};
inline std::atomic<bool> pre_execute_release{false};
inline std::atomic<bool> pre_execute_timeout{false};
inline void HoldPreExecuteForTest() {
  if (!pre_execute_enabled.load(std::memory_order_acquire)) return;
  pre_execute_entered.store(true, std::memory_order_release);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!pre_execute_release.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  if (!pre_execute_release.load(std::memory_order_acquire))
    pre_execute_timeout.store(true, std::memory_order_release);
}
inline std::atomic<bool> replacement_join_enabled{false};
inline std::atomic<bool> replacement_join_entered{false};
inline std::atomic<bool> replacement_join_release{false};
inline std::atomic<bool> replacement_join_timeout{false};
inline void HoldReplacementJoinForTest() {
  if (!replacement_join_enabled.load(std::memory_order_acquire)) return;
  replacement_join_entered.store(true, std::memory_order_release);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!replacement_join_release.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  if (!replacement_join_release.load(std::memory_order_acquire))
    replacement_join_timeout.store(true, std::memory_order_release);
}
inline std::atomic<unsigned> lookahead_retirement_entries{0};
inline std::array<std::atomic<bool>, 2> lookahead_retirement_release{};
inline std::atomic<bool> lookahead_retirement_latch_timeout{false};
inline void HoldLookaheadRetirementForTest() {
  const auto ticket = lookahead_retirement_entries.fetch_add(1, std::memory_order_acq_rel);
  if (ticket >= lookahead_retirement_release.size()) return;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!lookahead_retirement_release[ticket].load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  if (!lookahead_retirement_release[ticket].load(std::memory_order_acquire))
    lookahead_retirement_latch_timeout.store(true, std::memory_order_release);
}
} // namespace manyforge::core::detail
#define MANYFORGE_LOOKAHEAD_RETIREMENT_TEST_POINT() (::manyforge::core::detail::HoldLookaheadRetirementForTest())
#define MANYFORGE_PRE_EXECUTE_TEST_POINT() (::manyforge::core::detail::HoldPreExecuteForTest())
#define MANYFORGE_REPLACEMENT_JOIN_TEST_POINT() (::manyforge::core::detail::HoldReplacementJoinForTest())
#else
#define MANYFORGE_LOOKAHEAD_RETIREMENT_TEST_POINT() ((void)0)
#define MANYFORGE_PRE_EXECUTE_TEST_POINT() ((void)0)
#define MANYFORGE_REPLACEMENT_JOIN_TEST_POINT() ((void)0)
#endif
