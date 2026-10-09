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

#include "controller_mechanism.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace manyforge_motion_controller
{
namespace
{
using Refusal = core::ExecutionMechanismRefusal;
using Phase = core::ExecutionCommandPhase;
using Effect = core::ComponentLifecycleDomainEffectDTO;
using Result = core::ExecutionMechanismCallResult;
Result Refused(Refusal refusal) noexcept
{
  return {core::StopStageAttemptDispositionDTO::kNotDispatched, refusal};
}
Result Submitted() noexcept
{
  return {core::StopStageAttemptDispositionDTO::kDispatched, Refusal::kNone};
}
}  // namespace

ControllerMechanism::ControllerMechanism(ReceiverProfile profile)
: profile_(std::move(profile))
{
  if (!profile_.local_now_ns || profile_.joints.empty() ||
    profile_.joints.size() > core::kMaxMotionJoints ||
    !profile_.binding.active_control_configuration_generation.nonzero() ||
    profile_.binding.provider_runtime_epoch.empty() ||
    !std::isfinite(profile_.initial_scale) || profile_.initial_scale < 0.0 ||
    profile_.initial_scale > 1.0 || !profile_.initial_scale_generation.nonzero())
  {
    throw std::invalid_argument("Invalid receiver profile");
  }
  for (std::size_t i = 0; i < profile_.joints.size(); ++i) {
    if (profile_.joints[i].empty()) {
      throw std::invalid_argument("Empty resource name");
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (profile_.joints[i] == profile_.joints[j]) {
        throw std::invalid_argument("Duplicate resource name");
      }
    }
  }
}

std::int64_t ControllerMechanism::LocalNow() const noexcept
{
  return profile_.local_now_ns(profile_.clock_context);
}

Refusal ControllerMechanism::Find(
  const core::ExecutionGoalKey & key,
  std::size_t & index) const noexcept
{
  if (!(key.binding == profile_.binding)) {
    return Refusal::kStaleKey;
  }
  for (std::size_t i = 0; i < kCapacity; ++i) {
    std::lock_guard<std::mutex> lock(records_[i].publication_mutex);
    if (records_[i].metadata && records_[i].metadata->key == key) {
      index = i;
      return Refusal::kNone;
    }
  }
  return Refusal::kUnknownKey;
}

bool ControllerMechanism::Enqueue(const Event & event) noexcept
{
  const auto tail = tail_.load(std::memory_order_relaxed);
  if (tail - head_.load(std::memory_order_acquire) == kCapacity) {
    return false;
  }
  events_[tail % kCapacity] = event;
  tail_.store(tail + 1U, std::memory_order_release);
  return true;
}

bool ControllerMechanism::Dequeue(Event & event) noexcept
{
  const auto head = head_.load(std::memory_order_relaxed);
  if (head == tail_.load(std::memory_order_acquire)) {
    return false;
  }
  event = events_[head % kCapacity];
  head_.store(head + 1U, std::memory_order_release);
  return true;
}

Result ControllerMechanism::Register(
  const core::ExecutionGoalKey & key, const core::PreparedMotionSlot & slot,
  EventKind kind, std::size_t previous, std::uint64_t previous_version) noexcept
{
  // ordinary_mutex_ serializes ordinary producers. Allocate metadata before
  // taking a per-record publication lock; Stop never waits behind allocation.
  if (!(key.binding == profile_.binding)) {
    return Refused(Refusal::kStaleKey);
  }
  const auto & motion = slot.motion;
  if (!(motion.key == key) ||
    slot.sealed.participant_digest != motion.prepared_contract_digest)
  {
    return Refused(Refusal::kSlotKeyMismatch);
  }
  if (!key.fence().nonzero() || key.operation_id.empty() || key.controller_goal_id.empty() ||
    motion.joint_count != profile_.joints.size() ||
    motion.trajectory.joint_names != profile_.joints ||
    !(motion.command_realization == profile_.realization))
  {
    return Refused(Refusal::kInvalidInput);
  }
  std::size_t found{};
  if (Find(key, found) == Refusal::kNone) {
    return Refused(Refusal::kInvalidInput);
  }
  if (tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_acquire) == kCapacity) {
    return Refused(Refusal::kChannelFull);
  }
  try {
    auto metadata = std::make_shared<Metadata>();
    metadata->key = key;
    for (std::size_t i = 0; i < kCapacity; ++i) {
      auto & record = records_[i];
      std::shared_ptr<const Metadata> retired;
      {
        std::lock_guard<std::mutex> lock(record.publication_mutex);
        if (record.borrowed_version.load(std::memory_order_acquire) != 0U ||
          record.next_version == std::numeric_limits<std::uint64_t>::max() ||
          (record.metadata && record.release_observed_version != record.metadata->version))
        {
          continue;
        }
        metadata->version = ++record.next_version;
        retired = std::move(record.metadata);
        record.metadata = metadata;
        record.posted_stop = core::StopGeneration{0U};
        record.posted_scale = core::RuntimeScaleGeneration{0U};
        record.borrowed_version.store(metadata->version, std::memory_order_release);
      }
      // Retired strings/shared storage are destroyed outside the Stop lock.
      static_cast<void>(Enqueue(
        {kind, i, metadata->version, &motion, key.fence(), previous, previous_version}));
      return Submitted();
    }
  } catch (...) {
    return Refused(Refusal::kChannelFull);
  }
  return Refused(Refusal::kChannelFull);
}

Result ControllerMechanism::Submit(
  const core::ExecutionGoalKey & key, const core::PreparedMotionSlot & slot) noexcept
{
  std::lock_guard<std::mutex> lock(ordinary_mutex_);
  return Register(key, slot, EventKind::kSubmit, kCapacity, 0U);
}

Result ControllerMechanism::Replace(
  const core::ExecutionGoalKey & key, const core::ExecutionGoalKey & next,
  const core::PreparedMotionSlot & slot) noexcept
{
  std::lock_guard<std::mutex> lock(ordinary_mutex_);
  std::size_t index{};
  const auto refusal = Find(key, index);
  if (refusal != Refusal::kNone) {
    return Refused(refusal);
  }
  if (next.controller_goal_generation <= key.controller_goal_generation ||
    next.operation_id != key.operation_id || records_[index].borrowed_version.load() == 0U)
  {
    return Refused(Refusal::kInvalidInput);
  }
  return Register(next, slot, EventKind::kReplace, index, records_[index].borrowed_version.load());
}

Result ControllerMechanism::Release(const core::ExecutionGoalKey & key) noexcept
{
  std::lock_guard<std::mutex> lock(ordinary_mutex_);
  std::size_t index{};
  const auto refusal = Find(key, index);
  if (refusal != Refusal::kNone) {
    return Refused(refusal);
  }
  return Enqueue({EventKind::kRelease, index, records_[index].borrowed_version.load(),
             nullptr, key.fence(), kCapacity, 0U}) ?
         Submitted() : Refused(Refusal::kChannelFull);
}

Result ControllerMechanism::PostScale(
  const core::ExecutionGoalKey & key, const core::ScaleTarget & target) noexcept
{
  std::size_t index{};
  const auto refusal = Find(key, index);
  if (refusal != Refusal::kNone) {
    return Refused(refusal);
  }
  if (!(target.goal == key.fence()) || !target.generation.nonzero()) {
    return Refused(Refusal::kInvalidInput);
  }
  auto & record = records_[index];
  std::lock_guard<std::mutex> lock(record.publication_mutex);
  if (!record.metadata || record.metadata->key != key ||
    record.borrowed_version.load() != record.metadata->version ||
    target.generation <= record.posted_scale)
  {
    return Refused(Refusal::kStaleKey);
  }
  record.posted_scale = target.generation;
  record.scales.publish({record.metadata->version, target});
  return Submitted();
}

Result ControllerMechanism::PostStop(
  const core::ExecutionGoalKey & key, const core::StopTransition & transition) noexcept
{
  std::size_t index{};
  const auto refusal = Find(key, index);
  if (refusal != Refusal::kNone) {
    return Refused(refusal);
  }
  if (!(transition.goal == key.fence()) || !transition.stop_generation.nonzero()) {
    return Refused(Refusal::kInvalidInput);
  }
  auto & record = records_[index];
  std::lock_guard<std::mutex> lock(record.publication_mutex);
  if (!record.metadata || record.metadata->key != key ||
    record.borrowed_version.load() != record.metadata->version ||
    transition.stop_generation <= record.posted_stop)
  {
    return Refused(Refusal::kStaleKey);
  }
  record.posted_stop = transition.stop_generation;
  record.stops.publish({record.metadata->version, transition});
  return Submitted();
}

Result ControllerMechanism::RequestFence(const core::ExecutionGoalKey & key) noexcept
{
  std::size_t index{};
  const auto refusal = Find(key, index);
  if (refusal != Refusal::kNone) {
    return Refused(refusal);
  }
  auto & record = records_[index];
  std::lock_guard<std::mutex> lock(record.publication_mutex);
  if (!record.metadata || record.metadata->key != key) {
    return Refused(Refusal::kStaleKey);
  }
  record.fence_version.store(record.metadata->version, std::memory_order_release);
  return Submitted();
}

Result ControllerMechanism::Hold(const core::ExecutionGoalKey & key) noexcept
{
  return RequestFence(key);
}
Result ControllerMechanism::Cancel(const core::ExecutionGoalKey & key) noexcept
{
  return RequestFence(key);
}
Result ControllerMechanism::Fence(const core::ExecutionGoalKey & key) noexcept
{
  return RequestFence(key);
}

Result ControllerMechanism::RenewHostLease(std::int64_t duration) noexcept
{
  // Receipt is on this receiver's clock, not the delayed consumption tick.
  std::lock_guard<std::mutex> lock(lease_mutex_);
  const auto now = LocalNow();
  if (duration <= 0 || now < 0 || duration > std::numeric_limits<std::int64_t>::max() - now) {
    return Refused(Refusal::kInvalidInput);
  }
  lease_.publish(now + duration);
  return Submitted();
}

void ControllerMechanism::Unreference(std::size_t index, std::uint64_t version) noexcept
{
  if (active_ == index && active_version_ == version) {
    active_ = kCapacity;
    active_slot_ = nullptr;
    facts_[index].downstream = core::DownstreamFenceState::kFenceRequested;
  }
  auto expected = version;
  static_cast<void>(records_[index].borrowed_version.compare_exchange_strong(
      expected, 0U, std::memory_order_release, std::memory_order_relaxed));
}

bool ControllerMechanism::Install(const Event & event, std::int64_t now, bool replacement) noexcept
{
  const auto index = event.index;
  auto & facts = facts_[index];
  facts = {};
  facts.version = event.version;
  facts.dispatch = {Phase::kConsumed, Effect::kKnownNoEffect};
  facts.goal_generation = event.goal.controller_goal_generation;
  if (replacement) {
    // The old goal has already consumed Stop and stepped at this exact tick.
    // Core owns continuity, the replacement's time law and the retained Stop.
    // Its alias-safe refusal leaves both authoritative values unchanged.
    const auto result = core::InstallReplacementMotionStep(
      *active_slot_, step_, output_, *event.slot, now, step_, output_);
    if (!result.installed()) {
      Unreference(index, event.version);
      return false;
    }
    Unreference(active_, active_version_);
  } else {
    core::MotionStepState candidate;
    core::MotionStepOutput initial;
    if (!core::InstallMotionStep(
        *event.slot, now, profile_.initial_scale, profile_.initial_scale_generation, candidate) ||
      core::StepMotion(*event.slot, candidate, {now, nullptr, nullptr, {}}, initial) ==
      core::MotionStepStatus::kCannotProceed)
    {
      Unreference(index, event.version);
      return false;
    }
    step_ = candidate;
    output_ = initial;
  }
  facts.step = output_.status;
  facts.scale_generation = output_.consumed_scale_generation;
  facts.stop_generation = output_.consumed_stop_generation;
  if (step_.active_stop) {
    facts.stop = {Phase::kConsumed, Effect::kNone};
    facts.stop_command_active = true;
  }
  active_ = index;
  active_version_ = event.version;
  active_slot_ = event.slot;
  return true;
}

void ControllerMechanism::Publish(std::int64_t now) noexcept
{
  ++health_.loop_tick_count;
  health_.last_tick_local_ns = now;
  health_.stamp = {health_.loop_tick_count, now, now};
  for (auto & facts : facts_) {
    facts.stamp = health_.stamp;
  }
  // No independent watchdog is implemented. Its checked timestamp stays zero;
  // this observation is explicitly absent, never a healthy readiness claim.
  status_.publish({facts_, health_});
}

void ControllerMechanism::Advance(
  std::int64_t now, StopMessage & stop, ScaleMessage & scale) noexcept
{
  auto & facts = facts_[active_];
  if (!health_.host_lease_valid || lease_containment_ ||
    facts.downstream != core::DownstreamFenceState::kOpen)
  {
    return;
  }
  const bool has_stop = stop.version == active_version_;
  const bool has_scale = scale.version == active_version_;
  facts.step = core::StepMotion(
    *active_slot_, step_,
    {now, has_stop ? &stop.value : nullptr, has_scale ? &scale.value : nullptr, {}}, output_);
  facts.scale_generation = output_.consumed_scale_generation;
  facts.stop_generation = output_.consumed_stop_generation;
  if (has_scale) {
    if (output_.scale_validation == core::MotionInputValidation::kAccepted &&
      output_.consumed_scale_generation == scale.value.generation)
    {
      facts.scale = {Phase::kConsumed, Effect::kNone};
      facts.scale_command_active = true;
      facts.scale_target = scale.value.target_scale;
    }
    // Invalid, stale or Stop-ignored input cannot change knowledge for the
    // last accepted generation, nor erase its previously observed effect.
    scale.version = 0U;
  }
  if (has_stop) {
    if (output_.stop_validation == core::MotionInputValidation::kAccepted &&
      output_.consumed_stop_generation == stop.value.stop_generation)
    {
      facts.stop = {Phase::kConsumed, Effect::kNone};
      facts.stop_command_active = true;
      // Stop governs subsequent frames; its braking cannot realize an
      // earlier ordinary scale target by coincidence. Retain prior facts.
      facts.scale_command_active = false;
    }
    stop.version = 0U;
  }
  if (facts.step == core::MotionStepStatus::kCannotProceed ||
    (step_.active_stop && now >= step_.active_stop->braking_deadline_ns &&
    facts.step != core::MotionStepStatus::kStopped))
  {
    records_[active_].fence_version.store(active_version_, std::memory_order_release);
    facts.downstream = core::DownstreamFenceState::kFenceRequested;
    return;
  }
}

bool ControllerMechanism::Tick(
  std::int64_t now, std::span<hardware_interface::LoanedCommandInterface> commands) noexcept
{
  std::int64_t expiry{};
  if (lease_.consume(expiry)) {
    health_.host_lease_expiry_local_ns = expiry;
    lease_received_ = true;
  }
  if (now < 0 || (health_.loop_tick_count != 0U && now < health_.last_tick_local_ns)) {
    lease_containment_ = true;
  }
  health_.host_lease_valid = lease_received_ && now >= 0 &&
    now < health_.host_lease_expiry_local_ns;
  if (lease_received_ && !health_.host_lease_valid) {
    lease_containment_ = true;
  }
  for (std::size_t i = 0; i < kCapacity; ++i) {
    static_cast<void>(records_[i].stops.consume(pending_stops_[i]));
    static_cast<void>(records_[i].scales.consume(pending_scales_[i]));
    if (facts_[i].version != 0U &&
      (records_[i].fence_version.load(std::memory_order_acquire) == facts_[i].version ||
      lease_containment_))
    {
      facts_[i].downstream = core::DownstreamFenceState::kFenceRequested;
    }
  }
  // Stop is consumed and the old goal is stepped before any ordinary event,
  // including Release. No ordinary traffic can erase an unconsumed Stop.
  if (active_ != kCapacity) {
    Advance(now, pending_stops_[active_], pending_scales_[active_]);
  }
  Event event;
  if (Dequeue(event)) {
    if (event.kind == EventKind::kRelease) {
      if (facts_[event.index].version == event.version) {
        facts_[event.index].downstream = core::DownstreamFenceState::kFenceRequested;
      }
      Unreference(event.index, event.version);
    } else {
      const bool replacing = event.kind == EventKind::kReplace;
      const bool eligible = health_.host_lease_valid && !lease_containment_ &&
        records_[event.index].fence_version.load(std::memory_order_acquire) != event.version &&
        (replacing ? (active_ == event.previous && active_version_ == event.previous_version &&
        facts_[active_].downstream == core::DownstreamFenceState::kOpen) :
        active_ == kCapacity);
      if (eligible && Install(event, now, replacing)) {
        // Installation already produced this tick's command. Only a pending
        // input for the new key needs a zero-elapsed step before the one write.
        if (pending_stops_[active_].version == active_version_ ||
          pending_scales_[active_].version == active_version_)
        {
          Advance(now, pending_stops_[active_], pending_scales_[active_]);
        }
      } else {
        // Installation refusal is consumption with known no command effect.
        // Preserve any more specific continuity/step diagnostic from Install.
        if (facts_[event.index].version != event.version) {
          facts_[event.index] = {};
          facts_[event.index].version = event.version;
          facts_[event.index].goal_generation = event.goal.controller_goal_generation;
        }
        facts_[event.index].dispatch = {Phase::kConsumed, Effect::kKnownNoEffect};
        if (lease_containment_ ||
          records_[event.index].fence_version.load(std::memory_order_acquire) == event.version)
        {
          facts_[event.index].downstream = core::DownstreamFenceState::kFenceRequested;
        }
        Unreference(event.index, event.version);
      }
    }
  }
  bool wrote = false;
  if (active_ != kCapacity && health_.host_lease_valid && !lease_containment_) {
    auto & current = facts_[active_];
    if (current.downstream == core::DownstreamFenceState::kOpen) {
      ++current.realization.last_computed_sequence;
      current.realization.stamp = {current.realization.last_computed_sequence, now, now};
      std::size_t writes = 0U;
      bool matching = output_.command.joint_count == commands.size() &&
        commands.size() == profile_.joints.size();
      for (std::size_t j = 0; matching && j < commands.size(); ++j) {
        matching = commands[j].get_prefix_name() == profile_.joints[j] &&
          commands[j].get_interface_name() == "position" &&
          commands[j].get_data_type() == hardware_interface::HandleDataType::DOUBLE;
      }
      if (matching) {
        for (std::size_t j = 0; j < commands.size(); ++j) {
          // One attempt: failure returns without the ROS handle's retry/yield.
          if (!commands[j].set_value(output_.command.position_rad[j], 1U)) {
            break;
          }
          ++writes;
        }
      }
      wrote = matching && writes == commands.size();
      const bool loop_write = profile_.realization.boundary ==
        core::CommandRealizationBoundary::kLoopInterfaceWrite;
      // Attribute only accepted generations governing this output frame.
      // A rejected/ignored target never inherits dispatch write success.
      if (current.scale_command_active) {
        if (wrote && loop_write) {
          current.scale.effect = Effect::kEffectObserved;
          if (output_.applied_scale == current.scale_target && output_.scale_rate_per_s == 0.0) {
            current.scale.phase = Phase::kRealized;
          }
        } else if (writes != 0U) {
          current.scale.effect = wrote ? Effect::kEffectUncertain : Effect::kPartialEffectObserved;
        }
      }
      if (current.stop_command_active) {
        if (wrote && loop_write) {
          current.stop = {Phase::kRealized, Effect::kEffectObserved};
        } else if (writes != 0U) {
          current.stop.effect = wrote ? Effect::kEffectUncertain : Effect::kPartialEffectObserved;
        }
      }
      if (!wrote) {
        // A failed later frame cannot erase effects already observed for
        // this goal. No writes is known no effect for that frame only.
        if (writes != 0U) {
          current.dispatch.effect = Effect::kPartialEffectObserved;
        }
        current.downstream = core::DownstreamFenceState::kFenceRequested;
        records_[active_].fence_version.store(active_version_, std::memory_order_release);
      } else if (loop_write) {
        current.realization.last_realized_sequence = current.realization.last_computed_sequence;
        // Core clamps path time at the endpoint. A Stop replacement reports
        // kStopped there; ordinary braking can stop before that endpoint.
        current.realization.final_command_realized =
          output_.path_time_s >= active_slot_->trajectory.waypoints.back().time_from_start_s;
        current.dispatch = {Phase::kRealized, Effect::kEffectObserved};
      } else {
        // Local handles changed, but their declared transport/session
        // boundary has no acknowledgement. Never report known no effect.
        current.dispatch.effect = Effect::kEffectUncertain;
      }
    }
  }
  Publish(now);
  return wrote;
}

core::ExecutionMechanismStatus ControllerMechanism::ReadMechanismStatus() const
{
  std::lock_guard<std::mutex> lock(ordinary_mutex_);
  static_cast<void>(status_.consume(observed_));
  core::ExecutionMechanismStatus status;
  status.source_epoch = profile_.binding.provider_runtime_epoch;
  status.loop = observed_.health;
  for (std::size_t i = 0; i < kCapacity; ++i) {
    const auto & record = records_[i];
    std::shared_ptr<const Metadata> metadata;
    {
      std::lock_guard<std::mutex> publication(record.publication_mutex);
      metadata = record.metadata;
    }
    if (!metadata) {
      continue;
    }
    const auto facts = observed_.facts[i].version == metadata->version ?
      observed_.facts[i] : Facts{};
    core::ExecutionMechanismKeyStatus key;
    key.key = metadata->key;
    key.last_consumed_goal_generation = facts.goal_generation;
    key.last_consumed_scale_generation = facts.scale_generation;
    key.last_consumed_stop_generation = facts.stop_generation;
    key.dispatch = facts.dispatch;
    if (!facts.goal_generation.nonzero()) {
      key.dispatch.phase = Phase::kSubmitted;
    }
    key.scale = facts.scale;
    key.stop = facts.stop;
    key.last_step_status = facts.step;
    key.realization = facts.realization;
    key.slot_referenced = record.borrowed_version.load(std::memory_order_acquire) ==
      metadata->version;
    if (!key.slot_referenced) {
      record.release_observed_version = metadata->version;
    }
    key.downstream = facts.downstream;
    key.stamp = facts.stamp;
    status.keys.push_back(std::move(key));
  }
  return status;
}
}  // namespace manyforge_motion_controller
