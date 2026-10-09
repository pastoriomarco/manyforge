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

#ifndef CONTROLLER_MECHANISM_HPP_
#define CONTROLLER_MECHANISM_HPP_

#include <span>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "hardware_interface/loaned_command_interface.hpp"
#include "manyforge_core/ports/iexecution_mechanism.hpp"

namespace manyforge_motion_controller
{
namespace core = manyforge::core;

// One producer and one consumer own distinct cells. NRT channel mutexes below
// serialize concurrent port callers before they become the single producer.
// RT never takes a mutex, retries an exchange, or allocates a cell.
template<class T>
class LatestValue final
{
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(std::atomic<unsigned>::is_always_lock_free);

public:
  void publish(const T & value) noexcept
  {
    cells_[back_] = value;
    back_ = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel) & kIndex;
  }
  bool consume(T & value) noexcept
  {
    if ((middle_.load(std::memory_order_acquire) & kDirty) == 0U) {
      return false;
    }
    front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndex;
    value = cells_[front_];
    return true;
  }

private:
  static constexpr unsigned kDirty = 4U;
  static constexpr unsigned kIndex = 3U;
  std::array<T, 3> cells_{};
  unsigned back_{2U};
  unsigned front_{0U};
  std::atomic<unsigned> middle_{1U};
};

// No deployment defaults: binding, resource order, initial accepted scale and
// receiver-local clock must be supplied by the eventual host wiring.
struct ReceiverProfile final
{
  using LocalClock = std::int64_t(void * context) noexcept;
  core::ExecutionBindingIdentity binding;
  std::vector<std::string> joints;
  double initial_scale;
  core::RuntimeScaleGeneration initial_scale_generation;
  core::CommandRealizationContract realization;
  LocalClock * local_now_ns;
  void * clock_context;
};

class ControllerMechanism final : public core::IExecutionMechanism
{
public:
  // Four outstanding prepared borrows; records are reusable after RT release.
  // Key ownership stays NRT. Every fixed RT payload carries its record version.
  static constexpr std::size_t kCapacity = 4U;
  explicit ControllerMechanism(ReceiverProfile profile);
  core::ExecutionMechanismCallResult Submit(
    const core::ExecutionGoalKey &, const core::PreparedMotionSlot &) noexcept override;
  core::ExecutionMechanismCallResult Replace(
    const core::ExecutionGoalKey &, const core::ExecutionGoalKey &,
    const core::PreparedMotionSlot &) noexcept override;
  core::ExecutionMechanismCallResult Release(const core::ExecutionGoalKey &) noexcept override;
  core::ExecutionMechanismCallResult PostScale(
    const core::ExecutionGoalKey &, const core::ScaleTarget &) noexcept override;
  core::ExecutionMechanismCallResult PostStop(
    const core::ExecutionGoalKey &, const core::StopTransition &) noexcept override;
  core::ExecutionMechanismCallResult Hold(const core::ExecutionGoalKey &) noexcept override;
  core::ExecutionMechanismCallResult Cancel(const core::ExecutionGoalKey &) noexcept override;
  core::ExecutionMechanismCallResult Fence(const core::ExecutionGoalKey &) noexcept override;
  core::ExecutionMechanismCallResult RenewHostLease(std::int64_t) noexcept override;
  core::ExecutionMechanismStatus ReadMechanismStatus() const override;

  // Called by the production update method; one RT caller, no ROS time input.
  bool Tick(
    std::int64_t now_ns,
    std::span<hardware_interface::LoanedCommandInterface> commands) noexcept;
  std::int64_t LocalNow() const noexcept;

private:
  struct Facts final
  {
    std::uint64_t version{};
    core::ExecutionCommandKnowledge dispatch;
    core::ExecutionCommandKnowledge scale;
    core::ExecutionCommandKnowledge stop;
    core::ControllerGoalGeneration goal_generation{0U};
    core::RuntimeScaleGeneration scale_generation{0U};
    core::StopGeneration stop_generation{0U};
    core::MotionStepStatus step{core::MotionStepStatus::kCannotProceed};
    core::CommandRealizationFacts realization;
    bool scale_command_active{};
    bool stop_command_active{};
    double scale_target{};
    core::DownstreamFenceState downstream{core::DownstreamFenceState::kOpen};
    core::ExecutionMechanismFactStamp stamp;
  };
  struct Metadata final
  {
    core::ExecutionGoalKey key;
    std::uint64_t version{};
  };
  struct StopMessage final { std::uint64_t version{}; core::StopTransition value; };
  struct ScaleMessage final { std::uint64_t version{}; core::ScaleTarget value; };
  struct Record final
  {
    // Never held across transport, ordinary queue work or RT. Copying the
    // immutable shared pointer is the only key ownership operation under it.
    mutable std::mutex publication_mutex;
    std::shared_ptr<const Metadata> metadata;
    std::uint64_t next_version{};
    mutable std::uint64_t release_observed_version{};
    std::atomic<std::uint64_t> borrowed_version{0U};
    std::atomic<std::uint64_t> fence_version{0U};
    core::StopGeneration posted_stop{0U};
    core::RuntimeScaleGeneration posted_scale{0U};
    LatestValue<StopMessage> stops;
    LatestValue<ScaleMessage> scales;
  };
  enum class EventKind { kSubmit, kReplace, kRelease };
  struct Event final
  {
    EventKind kind{};
    std::size_t index{};
    std::uint64_t version{};
    const core::PreparedMotion * slot{};
    core::ExecutionGoalFence goal;
    std::size_t previous{kCapacity};
    std::uint64_t previous_version{};
  };
  struct Snapshot final
  {
    std::array<Facts, kCapacity> facts;
    core::ExecutionLoopHealth health;
  };
  core::ExecutionMechanismCallResult Register(
    const core::ExecutionGoalKey &, const core::PreparedMotionSlot &, EventKind,
    std::size_t previous, std::uint64_t previous_version) noexcept;
  core::ExecutionMechanismCallResult RequestFence(const core::ExecutionGoalKey &) noexcept;
  core::ExecutionMechanismRefusal Find(
    const core::ExecutionGoalKey &, std::size_t &) const noexcept;
  bool Enqueue(const Event &) noexcept;
  bool Dequeue(Event &) noexcept;
  void Unreference(std::size_t, std::uint64_t) noexcept;
  bool Install(const Event &, std::int64_t, bool replacement) noexcept;
  void Publish(std::int64_t) noexcept;
  void Advance(std::int64_t, StopMessage &, ScaleMessage &) noexcept;

  const ReceiverProfile profile_;
  std::array<Record, kCapacity> records_;
  // Only NRT registration, ordinary calls and status reads share this mutex.
  // Stop, fence primitives, scale and lease never acquire it.
  mutable std::mutex ordinary_mutex_;
  std::mutex lease_mutex_;
  LatestValue<std::int64_t> lease_;
  std::array<Event, kCapacity> events_{};
  std::atomic<std::uint64_t> head_{0U};
  std::atomic<std::uint64_t> tail_{0U};
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  std::array<StopMessage, kCapacity> pending_stops_{};
  std::array<ScaleMessage, kCapacity> pending_scales_{};
  std::array<Facts, kCapacity> facts_{};
  core::ExecutionLoopHealth health_{};
  mutable LatestValue<Snapshot> status_;
  mutable Snapshot observed_{};
  core::MotionStepState step_;
  core::MotionStepOutput output_;
  std::size_t active_{kCapacity};
  std::uint64_t active_version_{};
  const core::PreparedMotion * active_slot_{};
  bool lease_received_{};
  bool lease_containment_{};
};
}  // namespace manyforge_motion_controller
#endif  // CONTROLLER_MECHANISM_HPP_
