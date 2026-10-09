// Copyright 2026 Flexin Group SRL
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TRANSPORT_HPP_
#define TRANSPORT_HPP_

#include <span>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace manyforge_isaac_hardware::detail
{

constexpr std::size_t kJointCount = 6;
constexpr std::size_t kFeedbackCapacity = 8;
using Positions = std::array<double, kJointCount>;

// Exactly one producer and one consumer. Neither side may clear/reset the
// queue while the other runs. Ownership transfers at release/acquire; a slot
// is never overwritten until its consumer has finished copying it.
template<typename T, std::size_t Capacity>
class SpscQueue
{
  static_assert(Capacity > 0);
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(std::atomic<std::size_t>::is_always_lock_free);

public:
  bool push(const T & value) noexcept
  {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto next = (head + 1) % (Capacity + 1);
    if (next == tail_.load(std::memory_order_acquire)) {
      return false;  // Drop the new item; the producer cannot edit consumer slots.
    }
    slots_[head] = value;
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool pop(T & value) noexcept
  {
    const auto tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) {
      return false;
    }
    value = slots_[tail];
    tail_.store((tail + 1) % (Capacity + 1), std::memory_order_release);
    return true;
  }

  // Bounded even if the producer keeps filling the queue during this call.
  std::size_t pop_latest(T & value) noexcept
  {
    std::size_t count = 0;
    while (count < Capacity && pop(value)) {
      ++count;
    }
    return count;
  }

private:
  std::array<T, Capacity + 1> slots_{};
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

enum class FeedbackValidity : std::uint8_t
{
  kUnavailable,
  kValid,
  kIncomplete,
  kNonFinite,
  kUnknownJoint,
  kDuplicateJoint,
  kMissingSourceTime,
  kSourceClockUnavailable,
  kSourceTimeReordered,
  kClockReversed,
  kStale,
  kOverflow,
};

struct FeedbackSnapshot
{
  Positions position{};
  Positions velocity{};
  std::int64_t source_ns{0};
  std::int64_t received_ns{0};
  std::int64_t source_age_ns{0};
  FeedbackValidity validity{FeedbackValidity::kUnavailable};
};

struct FeedbackLimits
{
  std::int64_t source_max_age_ns{0};
  std::int64_t receive_max_age_ns{0};
};

enum class FeedbackSourceClock : std::uint8_t {kSystem, kRosSimulation};

// ROS-free view of the unmodified source sample. Source and source_now must
// share a clock domain. Receipt and read times use a local monotonic clock.
struct RawFeedback
{
  std::span<const std::string> names;
  std::span<const double> position;
  std::span<const double> velocity;
  std::int64_t source_ns;
};

class FeedbackValidator
{
public:
  FeedbackValidator(
    std::array<std::string, kJointCount> names, bool require_velocity,
    FeedbackLimits limits, FeedbackSourceClock source_clock = FeedbackSourceClock::kSystem)
  : names_(std::move(names)), require_velocity_(require_velocity), limits_(limits),
    source_clock_(source_clock) {}

  void invalidate_clock() noexcept {clock_failed_ = true;}

  FeedbackSnapshot accept(
    const RawFeedback & raw, std::int64_t source_now_ns, std::int64_t received_ns,
    bool source_clock_initialized = true) noexcept
  {
    FeedbackSnapshot sample;
    sample.source_ns = raw.source_ns;
    sample.received_ns = received_ns;
    if (!source_clock_initialized) {
      sample.validity = FeedbackValidity::kSourceClockUnavailable;
      return sample;
    }
    if (received_ns < 0 || source_now_ns < 0 ||
      (have_clock_ && (source_now_ns < last_source_now_ns_ || received_ns < last_received_ns_)))
    {
      sample.validity = FeedbackValidity::kClockReversed;
      clock_failed_ = true;
      return sample;
    }
    have_clock_ = true;
    last_source_now_ns_ = source_now_ns;
    last_received_ns_ = received_ns;
    if (clock_failed_) {
      sample.validity = FeedbackValidity::kClockReversed;
      return sample;
    }
    if (raw.names.size() != kJointCount || raw.position.size() != kJointCount ||
      (require_velocity_ && raw.velocity.size() != kJointCount) ||
      (!raw.velocity.empty() && raw.velocity.size() != kJointCount))
    {
      sample.validity = FeedbackValidity::kIncomplete;
      return sample;
    }
    std::array<bool, kJointCount> seen{};
    for (std::size_t i = 0; i < kJointCount; ++i) {
      std::size_t joint = 0;
      while (joint < kJointCount && names_[joint] != raw.names[i]) {
        ++joint;
      }
      if (joint == kJointCount) {
        sample.validity = FeedbackValidity::kUnknownJoint;
        return sample;
      }
      if (seen[joint]) {
        sample.validity = FeedbackValidity::kDuplicateJoint;
        return sample;
      }
      seen[joint] = true;
      if (!std::isfinite(raw.position[i]) ||
        (!raw.velocity.empty() && !std::isfinite(raw.velocity[i])))
      {
        sample.validity = FeedbackValidity::kNonFinite;
        return sample;
      }
      sample.position[joint] = raw.position[i];
      if (!raw.velocity.empty()) {
        sample.velocity[joint] = raw.velocity[i];
      }
    }
    if (raw.source_ns < 0 ||
      (raw.source_ns == 0 && source_clock_ != FeedbackSourceClock::kRosSimulation))
    {
      sample.validity = FeedbackValidity::kMissingSourceTime;
      return sample;
    }
    if (have_source_ && raw.source_ns <= last_source_ns_) {
      sample.validity = FeedbackValidity::kSourceTimeReordered;
      return sample;
    }
    if (raw.source_ns > source_now_ns) {
      sample.validity = FeedbackValidity::kClockReversed;
      return sample;
    }
    sample.source_age_ns = source_now_ns - raw.source_ns;
    if (limits_.source_max_age_ns <= 0 || limits_.receive_max_age_ns <= 0 ||
      sample.source_age_ns > limits_.source_max_age_ns)
    {
      sample.validity = FeedbackValidity::kStale;
      return sample;
    }
    last_source_ns_ = raw.source_ns;
    have_source_ = true;
    sample.validity = FeedbackValidity::kValid;
    return sample;
  }

private:
  std::array<std::string, kJointCount> names_;
  bool require_velocity_;
  FeedbackLimits limits_;
  FeedbackSourceClock source_clock_;
  std::int64_t last_source_ns_{0};
  std::int64_t last_source_now_ns_{0};
  std::int64_t last_received_ns_{0};
  bool have_clock_{false};
  bool have_source_{false};
  bool clock_failed_{false};
};

inline FeedbackValidity freshness(
  const FeedbackSnapshot & sample, FeedbackLimits limits, std::int64_t now_ns) noexcept
{
  if (sample.validity != FeedbackValidity::kValid) {
    return sample.validity;
  }
  if (now_ns < sample.received_ns) {
    // The producer can enqueue after read captured its clock and before it
    // drains the queue. This is an ineligible snapshot, not a reversed clock.
    return FeedbackValidity::kUnavailable;
  }
  const auto elapsed = now_ns - sample.received_ns;
  // Source age is checked in its domain at receipt. Continuing eligibility is
  // bounded by local receipt age; simulation and steady rates may differ.
  if (limits.receive_max_age_ns <= 0 || limits.source_max_age_ns <= 0 ||
    elapsed > limits.receive_max_age_ns || sample.source_age_ns < 0 ||
    sample.source_age_ns > limits.source_max_age_ns)
  {
    return FeedbackValidity::kStale;
  }
  return FeedbackValidity::kValid;
}

// Callback owns validator and is the queue's only producer. read() owns the
// consumer/cache. Overflow invalidates the cached sample and drains the bounded
// backlog: an invalid dropped sample cannot leave old feedback eligible.
class FeedbackHandoff
{
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
  FeedbackHandoff(
    std::array<std::string, kJointCount> names, bool require_velocity,
    FeedbackLimits limits, FeedbackSourceClock source_clock = FeedbackSourceClock::kSystem)
  : validator_(std::move(names), require_velocity, limits, source_clock), limits_(limits) {}

  void receive(
    const RawFeedback & raw, std::int64_t source_now_ns, std::int64_t received_ns,
    bool source_clock_initialized = true) noexcept
  {
    push(validator_.accept(raw, source_now_ns, received_ns, source_clock_initialized));
  }

  // Called by the same NRT executor as receive. A clock reset invalidates
  // cached feedback even if physics produces no subsequent joint sample.
  void invalidate_source_clock(std::int64_t received_ns) noexcept
  {
    validator_.invalidate_clock();
    FeedbackSnapshot sample;
    sample.received_ns = received_ns;
    sample.validity = FeedbackValidity::kClockReversed;
    push(sample);
  }

  FeedbackSnapshot read(std::int64_t now_ns) noexcept
  {
    const auto dropped = dropped_.load(std::memory_order_acquire);
    queue_.pop_latest(current_);
    if (dropped != observed_dropped_) {
      current_.validity = FeedbackValidity::kOverflow;
      observed_dropped_ = dropped;
    }
    auto result = current_;
    if (now_ns < 0 ||
      (have_read_clock_ && now_ns < last_read_ns_))
    {
      read_clock_failed_ = true;
    }
    have_read_clock_ = true;
    last_read_ns_ = now_ns;
    result.validity = read_clock_failed_ ? FeedbackValidity::kClockReversed :
      freshness(current_, limits_, now_ns);
    return result;
  }

private:
  void push(const FeedbackSnapshot & sample) noexcept
  {
    if (!queue_.push(sample)) {
      dropped_.fetch_add(1, std::memory_order_release);
    }
  }

  FeedbackValidator validator_;
  FeedbackLimits limits_;
  SpscQueue<FeedbackSnapshot, kFeedbackCapacity> queue_;
  std::atomic<std::uint64_t> dropped_{0};
  std::uint64_t observed_dropped_{0};
  FeedbackSnapshot current_;
  std::int64_t last_read_ns_{0};
  bool have_read_clock_{false};
  bool read_clock_failed_{false};
};

enum class WriteDisposition : std::uint8_t {kSuppressedInactive};

// These are local transport facts, not an execution status/wire contract. No
// publisher or accepted command path exists in this inactive slice.
struct TransportFacts
{
  WriteDisposition disposition{WriteDisposition::kSuppressedInactive};
  bool future_publication_fenced{true};
  bool command_submitted{false};
  bool command_realized{false};
  bool downstream_delivery_known{false};
  bool downstream_drained{false};
};

}  // namespace manyforge_isaac_hardware::detail

#endif  // TRANSPORT_HPP_
