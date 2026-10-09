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

#include <gtest/gtest.h>

#include <semaphore>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <shared_mutex>
#include <thread>
#include <utility>
#include <vector>

#include "controller_mechanism.hpp"
#include "manyforge_motion_controller/motion_controller.hpp"
#include "controller_interface/controller_interface_params.hpp"
#include "hardware_interface/handle.hpp"
#include "rclcpp/rclcpp.hpp"

namespace
{
thread_local bool count_allocations = false;
thread_local std::size_t allocation_count = 0U;
void * Allocate(std::size_t size)
{
  if (count_allocations) {
    ++allocation_count;
  }
  if (auto * memory = std::malloc(size == 0U ? 1U : size)) {
    return memory;
  }
  throw std::bad_alloc();
}
}  // namespace
void * operator new(std::size_t size) {return Allocate(size);}
void * operator new[](std::size_t size) {return Allocate(size);}
void operator delete(void * memory) noexcept {std::free(memory);}
void operator delete[](void * memory) noexcept {std::free(memory);}
void operator delete(void * memory, std::size_t) noexcept {std::free(memory);}
void operator delete[](void * memory, std::size_t) noexcept {std::free(memory);}

namespace manyforge_motion_controller
{
class MotionControllerTestPeer
{
public:
  static void Attach(MotionController & controller, std::unique_ptr<ControllerMechanism> receiver)
  {
    controller.mechanism_ = std::move(receiver);
  }
};
namespace
{
using Refusal = core::ExecutionMechanismRefusal;
using Boundary = core::CommandRealizationBoundary;
using Phase = core::ExecutionCommandPhase;
using Effect = core::ComponentLifecycleDomainEffectDTO;
constexpr std::int64_t kPeriod = 1000000;

class LockableCommand final : public hardware_interface::CommandInterface
{
public:
  explicit LockableCommand(const std::string & joint)
  : hardware_interface::CommandInterface(joint, "position", "double", "9") {}
  std::unique_lock<std::shared_mutex> Lock() {return std::unique_lock(handle_mutex_);}
};

class MechanismTest : public ::testing::Test
{
protected:
  static void SetUpTestSuite() {rclcpp::init(0, nullptr);}
  static void TearDownTestSuite() {rclcpp::shutdown();}
  static std::int64_t Now(void * context) noexcept
  {
    return static_cast<std::atomic<std::int64_t> *>(context)->load();
  }
  ReceiverProfile Profile(double scale = 1.0, Boundary boundary = Boundary::kLoopInterfaceWrite)
  {
    return {{core::ActiveControlConfigurationGeneration{1U}, core::RuntimeEpoch{"epoch"}},
      {"j1", "j2"}, scale, core::RuntimeScaleGeneration{1U}, {boundary, false, false}, &Now, &now_};
  }
  core::PreparedMotionSlot Prepared(std::uint64_t generation = 1U, double start = 0.0)
  {
    core::PreparedMotionSlot slot;
    auto & motion = slot.motion;
    motion.key = {core::OperationId{"op"}, core::ControllerGoalId{"goal"},
      core::ControllerGoalGeneration{generation}, core::PreparedExecutionId{generation},
      Profile().binding};
    motion.prepared_contract_digest = core::ContentDigest{"retained-participant"};
    slot.sealed.participant_digest = motion.prepared_contract_digest;
    motion.joint_count = 2U;
    motion.interpolation = core::MotionInterpolation::kLinear;
    motion.trajectory.joint_names = {"j1", "j2"};
    motion.trajectory.waypoints = {{0.0, {start, start}, {}, {}},
      {1.0, {start + 1.0, start + 1.0}, {}, {}}};
    motion.governor_bounds = {2.0, 8.0};
    motion.stop_braking_bounds = {4.0, 16.0};
    motion.runtime_scale_bounds = {2.0, 8.0, 1.5, 1.0};
    motion.tick_envelope = {kPeriod, kPeriod};
    motion.continuity_tolerance = {1e-9, 1e-9, 1e-9};
    return slot;
  }
  core::StopTransition Stop(const core::PreparedMotionSlot & slot, std::uint64_t generation = 1U)
  {
    return {slot.motion.key.fence(), core::StopGeneration{generation}, 0U,
      slot.motion.stop_braking_bounds, 1000000000};
  }
  core::MotionStepOutput StopEntry(
    const core::PreparedMotionSlot & slot, const core::StopTransition & stop,
    double scale = 1.0)
  {
    core::MotionStepState state;
    core::MotionStepOutput output;
    EXPECT_TRUE(core::InstallMotionStep(
      slot.motion, 0, scale, core::RuntimeScaleGeneration{1U}, state));
    EXPECT_NE(core::StepMotion(slot.motion, state, {0, nullptr, nullptr, {}}, output),
      core::MotionStepStatus::kCannotProceed);
    EXPECT_EQ(core::StepMotion(slot.motion, state, {kPeriod, &stop, nullptr, {}}, output),
      core::MotionStepStatus::kBraking);
    return output;
  }
  core::PreparedMotionSlot StopReplacement(const core::MotionStepOutput & entry)
  {
    auto slot = Prepared(2U);
    auto & motion = slot.motion;
    motion.interpolation = core::MotionInterpolation::kQuintic;
    motion.trajectory.waypoints = {
      {0.0, {entry.command.position_rad[0], entry.command.position_rad[1]},
        {entry.command.velocity_rad_s[0], entry.command.velocity_rad_s[1]},
        {entry.command.acceleration_rad_s2[0], entry.command.acceleration_rad_s2[1]}},
      {0.02, {entry.command.position_rad[0], entry.command.position_rad[1]},
        {0.0, 0.0}, {0.0, 0.0}}};
    return slot;
  }
  void SetUp() override
  {
    for (const auto & joint : {"j1", "j2"}) {
      handles_.push_back(std::make_shared<LockableCommand>(joint));
      commands_.emplace_back(handles_.back(), nullptr);
    }
  }
  void Start(ControllerMechanism & receiver, const core::PreparedMotionSlot & motion)
  {
    ASSERT_EQ(receiver.RenewHostLease(10000000000).refusal, Refusal::kNone);
    ASSERT_EQ(receiver.Submit(motion.motion.key, motion).refusal, Refusal::kNone);
    ASSERT_TRUE(receiver.Tick(0, commands_));
  }
  std::atomic<std::int64_t> now_{0};
  std::vector<std::shared_ptr<LockableCommand>> handles_;
  std::vector<hardware_interface::LoanedCommandInterface> commands_;
};

TEST_F(MechanismTest, UsesRealStepAndSeparatesConsumptionRealizationAndDelivery)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  Start(receiver, motion);
  core::MotionStepState state;
  ASSERT_TRUE(core::InstallMotionStep(motion.motion, 0, 1.0, core::RuntimeScaleGeneration{1U},
        state));
  core::MotionStepOutput expected;
  for (std::int64_t tick = 1; tick <= 8; ++tick) {
    ASSERT_NE(core::StepMotion(motion.motion, state, {tick * kPeriod, nullptr, nullptr, {}},
          expected),
      core::MotionStepStatus::kCannotProceed);
    ASSERT_TRUE(receiver.Tick(tick * kPeriod, commands_));
    EXPECT_DOUBLE_EQ(*handles_[0]->get_optional<double>(), expected.command.position_rad[0]);
  }
  const auto status = receiver.ReadMechanismStatus();
  ASSERT_EQ(status.keys.size(), 1U);
  EXPECT_EQ(status.keys[0].dispatch.phase, Phase::kRealized);
  EXPECT_EQ(status.keys[0].realization.last_computed_sequence, 9U);
  EXPECT_EQ(status.keys[0].realization.last_realized_sequence, 9U);
  EXPECT_FALSE(status.keys[0].realization.last_delivered_sequence.has_value());
  EXPECT_FALSE(status.keys[0].realization.final_command_realized);
  EXPECT_EQ(status.loop.stall_watchdog_checked_local_ns, 0);
}

TEST_F(MechanismTest, InterfaceWriteCannotClaimTransportPublication)
{
  auto motion = Prepared();
  motion.motion.command_realization.boundary = Boundary::kTransportPublication;
  ControllerMechanism receiver(Profile(1.0, Boundary::kTransportPublication));
  Start(receiver, motion);
  const auto key = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_GT(key.realization.last_computed_sequence, 0U);
  EXPECT_EQ(key.realization.last_realized_sequence, 0U);
  EXPECT_EQ(key.dispatch.phase, Phase::kConsumed);
  EXPECT_EQ(key.dispatch.effect, Effect::kEffectUncertain);
}

TEST_F(MechanismTest, ReservedStopPrecedesReleaseEvenWithFullOrdinaryQueue)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  Start(receiver, motion);
  for (std::size_t i = 0; i < ControllerMechanism::kCapacity; ++i) {
    ASSERT_EQ(receiver.Release(motion.motion.key).refusal, Refusal::kNone);
  }
  EXPECT_EQ(receiver.Release(motion.motion.key).refusal, Refusal::kChannelFull);
  ASSERT_EQ(receiver.PostScale(motion.motion.key,
    {motion.motion.key.fence(), core::RuntimeScaleGeneration{2U}, -1.0}).refusal, Refusal::kNone);
  ASSERT_EQ(receiver.PostStop(motion.motion.key, Stop(motion)).refusal, Refusal::kNone);
  EXPECT_FALSE(receiver.Tick(kPeriod, commands_));
  const auto key = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_EQ(key.last_consumed_stop_generation.value(), 1U);
  EXPECT_EQ(key.last_step_status, core::MotionStepStatus::kBraking);
  EXPECT_FALSE(key.slot_referenced);
  EXPECT_EQ(key.downstream, core::DownstreamFenceState::kFenceRequested);
}

TEST_F(MechanismTest, ReusesRecordsAcrossThirtyMovesAfterObservedBorrowRelease)
{
  ControllerMechanism receiver(Profile());
  ASSERT_EQ(receiver.RenewHostLease(10000000000).refusal, Refusal::kNone);
  for (std::uint64_t generation = 1U; generation <= 30U; ++generation) {
    const auto motion = Prepared(generation);
    ASSERT_EQ(receiver.Submit(motion.motion.key, motion).refusal, Refusal::kNone);
    ASSERT_TRUE(receiver.ReadMechanismStatus().keys.at(0).slot_referenced);
    ASSERT_TRUE(receiver.Tick(static_cast<std::int64_t>(generation) * kPeriod, commands_));
    ASSERT_EQ(receiver.Release(motion.motion.key).refusal, Refusal::kNone);
    ASSERT_FALSE(receiver.Tick(static_cast<std::int64_t>(generation) * kPeriod, commands_));
    const auto key = receiver.ReadMechanismStatus().keys.at(0);
    ASSERT_EQ(key.key, motion.motion.key);
    ASSERT_FALSE(key.slot_referenced);
  }
}

TEST_F(MechanismTest, DelayedOldReleaseAndStopCannotClearOrAffectReusedRecord)
{
  const auto old = Prepared();
  const auto next = Prepared(2U);
  ControllerMechanism receiver(Profile());
  Start(receiver, old);
  for (std::size_t i = 0; i < ControllerMechanism::kCapacity; ++i) {
    ASSERT_EQ(receiver.Release(old.motion.key).refusal, Refusal::kNone);
  }
  ASSERT_EQ(receiver.PostStop(old.motion.key, Stop(old)).refusal, Refusal::kNone);
  EXPECT_FALSE(receiver.Tick(kPeriod, commands_));
  EXPECT_FALSE(receiver.ReadMechanismStatus().keys.at(0).slot_referenced);
  ASSERT_EQ(receiver.Submit(next.motion.key, next).refusal, Refusal::kNone);
  EXPECT_EQ(receiver.PostStop(old.motion.key, Stop(old, 2U)).refusal, Refusal::kUnknownKey);
  for (std::size_t i = 0; i < 3U; ++i) {
    EXPECT_FALSE(receiver.Tick(2 * kPeriod, commands_));
    const auto key = receiver.ReadMechanismStatus().keys.at(0);
    EXPECT_EQ(key.key, next.motion.key);
    EXPECT_TRUE(key.slot_referenced);
  }
  EXPECT_TRUE(receiver.Tick(2 * kPeriod, commands_));
  const auto key = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_EQ(key.last_consumed_stop_generation.value(), 0U);
  EXPECT_EQ(key.last_step_status, core::MotionStepStatus::kRunning);
}

TEST_F(MechanismTest, SerializesConcurrentNrtPublishersWithoutRegressingGenerations)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  Start(receiver, motion);
  std::atomic<bool> invalid{false};
  std::vector<std::jthread> callers;
  for (std::uint64_t generation = 2U; generation <= 32U; ++generation) {
    callers.emplace_back([&, generation]() {
        const auto scale = receiver.PostScale(motion.motion.key,
        {motion.motion.key.fence(), core::RuntimeScaleGeneration{generation}, 0.5});
        const auto stop = receiver.PostStop(motion.motion.key, Stop(motion, generation));
        if ((scale.refusal != Refusal::kNone && scale.refusal != Refusal::kStaleKey) ||
        (stop.refusal != Refusal::kNone && stop.refusal != Refusal::kStaleKey))
        {
          invalid.store(true);
        }
    });
  }
  callers.clear();
  EXPECT_FALSE(invalid.load());
  EXPECT_TRUE(receiver.Tick(kPeriod, commands_));
  EXPECT_EQ(receiver.ReadMechanismStatus().keys.at(0).last_consumed_stop_generation.value(), 32U);
}

TEST_F(MechanismTest, InvalidCurrentStopContainsWithoutWriting)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  Start(receiver, motion);
  const double previous = *handles_[0]->get_optional<double>();
  auto stop = Stop(motion);
  stop.braking_bounds.max_rate_per_s = -1.0;
  ASSERT_EQ(receiver.PostStop(motion.motion.key, stop).refusal, Refusal::kNone);
  EXPECT_FALSE(receiver.Tick(kPeriod, commands_));
  EXPECT_DOUBLE_EQ(*handles_[0]->get_optional<double>(), previous);
  const auto key = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_EQ(key.last_step_status, core::MotionStepStatus::kCannotProceed);
  EXPECT_EQ(key.downstream, core::DownstreamFenceState::kFenceRequested);
}

TEST_F(MechanismTest, DeadlineUsesActualLocalTime)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  Start(receiver, motion);
  auto stop = Stop(motion);
  stop.braking_deadline_ns = kPeriod;
  ASSERT_EQ(receiver.PostStop(motion.motion.key, stop).refusal, Refusal::kNone);
  EXPECT_FALSE(receiver.Tick(100 * kPeriod, commands_));
  EXPECT_EQ(receiver.ReadMechanismStatus().keys.at(0).downstream,
    core::DownstreamFenceState::kFenceRequested);
}

TEST_F(MechanismTest, LeaseUsesReceiverReceiptAndRenewalDoesNotResumeAtScaleZero)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile(0.0));
  ASSERT_EQ(receiver.RenewHostLease(kPeriod).refusal, Refusal::kNone);
  ASSERT_EQ(receiver.Submit(motion.motion.key, motion).refusal, Refusal::kNone);
  ASSERT_TRUE(receiver.Tick(0, commands_));
  EXPECT_EQ(receiver.ReadMechanismStatus().loop.host_lease_expiry_local_ns, kPeriod);
  EXPECT_FALSE(receiver.Tick(kPeriod, commands_));
  now_.store(kPeriod);
  ASSERT_EQ(receiver.RenewHostLease(100 * kPeriod).refusal, Refusal::kNone);
  EXPECT_FALSE(receiver.Tick(2 * kPeriod, commands_));
  const auto status = receiver.ReadMechanismStatus();
  EXPECT_TRUE(status.loop.host_lease_valid);
  EXPECT_EQ(status.keys.at(0).downstream, core::DownstreamFenceState::kFenceRequested);
  EXPECT_EQ(status.loop.stall_watchdog_checked_local_ns, 0);
}

TEST_F(MechanismTest, RejectsStaleBindingMismatchedSlotAndResourceOrder)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  auto key = motion.motion.key;
  key.binding.provider_runtime_epoch = core::RuntimeEpoch{"stale"};
  EXPECT_EQ(receiver.Submit(key, motion).refusal, Refusal::kStaleKey);
  key = motion.motion.key;
  key.prepared_execution_id = core::PreparedExecutionId{99U};
  EXPECT_EQ(receiver.Submit(key, motion).refusal, Refusal::kSlotKeyMismatch);
  auto reversed = motion;
  reversed.motion.trajectory.joint_names = {"j2", "j1"};
  EXPECT_EQ(receiver.Submit(reversed.motion.key, reversed).refusal, Refusal::kInvalidInput);
  EXPECT_EQ(receiver.RenewHostLease(0).refusal, Refusal::kInvalidInput);
  EXPECT_EQ(receiver.RenewHostLease(-1).refusal, Refusal::kInvalidInput);
}

TEST_F(MechanismTest, MissingLeaseRefusesInstallationWithKnownNoEffect)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  ASSERT_EQ(receiver.Submit(motion.motion.key, motion).refusal, Refusal::kNone);
  EXPECT_FALSE(receiver.Tick(0, commands_));
  const auto key = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_FALSE(key.slot_referenced);
  EXPECT_EQ(key.dispatch.phase, Phase::kConsumed);
  EXPECT_EQ(key.dispatch.effect, Effect::kKnownNoEffect);
  EXPECT_DOUBLE_EQ(*handles_[0]->get_optional<double>(), 9.0);
}

TEST_F(MechanismTest, LocalPrimitivesFenceFutureWritesWithoutClaimingDownstreamDrain)
{
  for (const auto primitive : {&ControllerMechanism::Hold, &ControllerMechanism::Cancel,
      &ControllerMechanism::Fence})
  {
    const auto motion = Prepared();
    ControllerMechanism receiver(Profile());
    Start(receiver, motion);
    const double previous = *handles_[0]->get_optional<double>();
    ASSERT_EQ((receiver.*primitive)(motion.motion.key).refusal, Refusal::kNone);
    EXPECT_FALSE(receiver.Tick(kPeriod, commands_));
    const auto key = receiver.ReadMechanismStatus().keys.at(0);
    EXPECT_TRUE(key.slot_referenced);
    EXPECT_EQ(key.stop.phase, Phase::kAccepted);
    EXPECT_EQ(key.last_consumed_stop_generation.value(), 0U);
    EXPECT_EQ(key.downstream, core::DownstreamFenceState::kFenceRequested);
    EXPECT_DOUBLE_EQ(*handles_[0]->get_optional<double>(), previous);
  }
}

TEST_F(MechanismTest, ReplacementChecksActualTickContinuityAndReleasesOldBorrow)
{
  const auto motion = Prepared();
  const auto next = Prepared(2U, 0.001);
  ControllerMechanism receiver(Profile());
  Start(receiver, motion);
  ASSERT_EQ(receiver.Replace(motion.motion.key, next.motion.key, next).refusal, Refusal::kNone);
  EXPECT_TRUE(receiver.Tick(kPeriod, commands_));
  const auto status = receiver.ReadMechanismStatus();
  ASSERT_EQ(status.keys.size(), 2U);
  EXPECT_FALSE(status.keys[0].slot_referenced);
  EXPECT_TRUE(status.keys[1].slot_referenced);
  EXPECT_DOUBLE_EQ(*handles_[0]->get_optional<double>(), 0.001);
}

TEST_F(MechanismTest, DiscontinuousReplacementHasNoEffectAndDoesNotLoseOldBorrow)
{
  const auto motion = Prepared();
  const auto next = Prepared(2U, 5.0);
  ControllerMechanism receiver(Profile());
  Start(receiver, motion);
  ASSERT_EQ(receiver.Replace(motion.motion.key, next.motion.key, next).refusal, Refusal::kNone);
  EXPECT_TRUE(receiver.Tick(kPeriod, commands_));
  const auto status = receiver.ReadMechanismStatus();
  EXPECT_TRUE(status.keys[0].slot_referenced);
  EXPECT_FALSE(status.keys[1].slot_referenced);
  EXPECT_EQ(status.keys[1].dispatch.effect, Effect::kKnownNoEffect);
  EXPECT_DOUBLE_EQ(*handles_[0]->get_optional<double>(), 0.001);
}

TEST_F(MechanismTest, SealedDigestMismatchRefusesSubmitAndReplaceBeforeBorrow)
{
  const auto current = Prepared();
  auto next = Prepared(2U, 0.001);
  next.sealed.participant_digest = core::ContentDigest{"different-participant"};
  ControllerMechanism receiver(Profile());
  const auto submit = receiver.Submit(next.motion.key, next);
  EXPECT_EQ(submit.refusal, Refusal::kSlotKeyMismatch);
  EXPECT_EQ(submit.disposition, core::StopStageAttemptDispositionDTO::kNotDispatched);
  EXPECT_TRUE(receiver.ReadMechanismStatus().keys.empty());
  Start(receiver, current);
  const auto replace = receiver.Replace(current.motion.key, next.motion.key, next);
  EXPECT_EQ(replace.refusal, Refusal::kSlotKeyMismatch);
  EXPECT_EQ(replace.disposition, core::StopStageAttemptDispositionDTO::kNotDispatched);
  EXPECT_EQ(receiver.ReadMechanismStatus().keys.size(), 1U);
}

TEST_F(MechanismTest, ReplacementPreservesStopAndUsesItsOwnLawWithOneTickWrite)
{
  const auto current = Prepared();
  const auto stop = Stop(current);
  const auto entry = StopEntry(current, stop, 0.5);
  const auto next = StopReplacement(entry);
  ControllerMechanism receiver(Profile(0.5));
  Start(receiver, current);
  ASSERT_EQ(receiver.PostStop(current.motion.key, stop).refusal, Refusal::kNone);
  ASSERT_EQ(receiver.Replace(current.motion.key, next.motion.key, next).refusal, Refusal::kNone);
  ASSERT_EQ(receiver.PostScale(next.motion.key,
    {next.motion.key.fence(), core::RuntimeScaleGeneration{2U}, 0.0}).refusal, Refusal::kNone);
  allocation_count = 0U;
  count_allocations = true;
  const bool wrote = receiver.Tick(kPeriod, commands_);
  count_allocations = false;
  ASSERT_TRUE(wrote);
  EXPECT_EQ(allocation_count, 0U);
  auto status = receiver.ReadMechanismStatus();
  ASSERT_EQ(status.keys.size(), 2U);
  EXPECT_FALSE(status.keys[0].slot_referenced);
  EXPECT_EQ(status.keys[0].stop.phase, Phase::kConsumed);
  EXPECT_EQ(status.keys[0].stop.effect, Effect::kNone);
  EXPECT_EQ(status.keys[0].realization.last_computed_sequence, 1U);
  const auto & installed = status.keys[1];
  EXPECT_TRUE(installed.slot_referenced);
  EXPECT_EQ(installed.last_consumed_stop_generation.value(), 1U);
  EXPECT_EQ(installed.last_consumed_scale_generation.value(), 1U);
  EXPECT_EQ(installed.stop.phase, Phase::kRealized);
  EXPECT_EQ(installed.stop.effect, Effect::kEffectObserved);
  EXPECT_EQ(installed.last_step_status, core::MotionStepStatus::kBraking);
  EXPECT_EQ(installed.realization.last_computed_sequence, 1U);
  EXPECT_EQ(installed.realization.last_realized_sequence, 1U);
  EXPECT_DOUBLE_EQ(*handles_[0]->get_optional<double>(), entry.command.position_rad[0]);

  // The same prepared replacement sampled at its own scale one is the
  // coordinate oracle; neither the old slider nor the new zero target applies.
  core::MotionStepState own_law;
  core::MotionStepOutput expected;
  ASSERT_TRUE(core::InstallMotionStep(
    next.motion, kPeriod, 1.0, core::RuntimeScaleGeneration{1U}, own_law));
  ASSERT_NE(core::StepMotion(
    next.motion, own_law, {2 * kPeriod, nullptr, nullptr, {}}, expected),
    core::MotionStepStatus::kCannotProceed);
  ASSERT_TRUE(receiver.Tick(2 * kPeriod, commands_));
  EXPECT_NEAR(*handles_[0]->get_optional<double>(), expected.command.position_rad[0], 1e-12);
  for (std::int64_t tick = 3; tick <= 30; ++tick) {
    ASSERT_TRUE(receiver.Tick(tick * kPeriod, commands_));
  }
  status = receiver.ReadMechanismStatus();
  EXPECT_EQ(status.keys[1].last_step_status, core::MotionStepStatus::kStopped);
  EXPECT_EQ(status.keys[1].last_consumed_scale_generation.value(), 1U);
  EXPECT_TRUE(status.keys[1].realization.final_command_realized);
}

TEST_F(MechanismTest, RefusedReplacementKeepsActualGoalBrakingAndEffectTruth)
{
  const auto current = Prepared();
  const auto stop = Stop(current);
  const auto entry = StopEntry(current, stop);
  auto next = StopReplacement(entry);
  next.motion.trajectory.waypoints.front().positions_rad[0] += 1.0;
  ControllerMechanism receiver(Profile());
  Start(receiver, current);
  ASSERT_EQ(receiver.PostStop(current.motion.key, stop).refusal, Refusal::kNone);
  ASSERT_EQ(receiver.Replace(current.motion.key, next.motion.key, next).refusal, Refusal::kNone);
  ASSERT_TRUE(receiver.Tick(kPeriod, commands_));
  const auto status = receiver.ReadMechanismStatus();
  EXPECT_TRUE(status.keys[0].slot_referenced);
  EXPECT_EQ(status.keys[0].last_step_status, core::MotionStepStatus::kBraking);
  EXPECT_EQ(status.keys[0].last_consumed_stop_generation.value(), 1U);
  EXPECT_EQ(status.keys[0].stop.phase, Phase::kRealized);
  EXPECT_EQ(status.keys[0].dispatch.effect, Effect::kEffectObserved);
  EXPECT_EQ(status.keys[0].realization.last_computed_sequence, 2U);
  EXPECT_FALSE(status.keys[1].slot_referenced);
  EXPECT_EQ(status.keys[1].dispatch.phase, Phase::kConsumed);
  EXPECT_EQ(status.keys[1].dispatch.effect, Effect::kKnownNoEffect);
  EXPECT_EQ(status.keys[1].realization.last_computed_sequence, 0U);
  EXPECT_DOUBLE_EQ(*handles_[0]->get_optional<double>(), entry.command.position_rad[0]);
}

TEST_F(MechanismTest, PendingNewKeyStopIsConsumedAfterReplacementWithoutAnotherWrite)
{
  const auto current = Prepared();
  const auto stop = Stop(current);
  const auto entry = StopEntry(current, stop);
  const auto next = StopReplacement(entry);
  ControllerMechanism receiver(Profile());
  Start(receiver, current);
  ASSERT_EQ(receiver.PostStop(current.motion.key, stop).refusal, Refusal::kNone);
  ASSERT_EQ(receiver.Replace(current.motion.key, next.motion.key, next).refusal, Refusal::kNone);
  auto stronger = Stop(next, 2U);
  stronger.stage_index = 1U;
  stronger.braking_bounds = {2.0, 8.0};
  ASSERT_EQ(receiver.PostStop(next.motion.key, stronger).refusal, Refusal::kNone);
  ASSERT_TRUE(receiver.Tick(kPeriod, commands_));
  const auto installed = receiver.ReadMechanismStatus().keys.at(1);
  EXPECT_EQ(installed.last_consumed_stop_generation.value(), 2U);
  EXPECT_EQ(installed.stop.phase, Phase::kRealized);
  EXPECT_EQ(installed.realization.last_computed_sequence, 1U);
  EXPECT_EQ(installed.realization.last_realized_sequence, 1U);
  EXPECT_DOUBLE_EQ(*handles_[0]->get_optional<double>(), entry.command.position_rad[0]);
}

TEST_F(MechanismTest, ReplacementKeepsOriginalStopDeadline)
{
  const auto current = Prepared();
  auto stop = Stop(current);
  stop.braking_deadline_ns = 3 * kPeriod;
  const auto next = StopReplacement(StopEntry(current, stop));
  ControllerMechanism receiver(Profile());
  Start(receiver, current);
  ASSERT_EQ(receiver.PostStop(current.motion.key, stop).refusal, Refusal::kNone);
  ASSERT_EQ(receiver.Replace(current.motion.key, next.motion.key, next).refusal, Refusal::kNone);
  ASSERT_TRUE(receiver.Tick(kPeriod, commands_));
  ASSERT_TRUE(receiver.Tick(2 * kPeriod, commands_));
  const double previous = *handles_[0]->get_optional<double>();
  EXPECT_FALSE(receiver.Tick(3 * kPeriod, commands_));
  EXPECT_DOUBLE_EQ(*handles_[0]->get_optional<double>(), previous);
  const auto installed = receiver.ReadMechanismStatus().keys.at(1);
  EXPECT_TRUE(installed.slot_referenced);
  EXPECT_EQ(installed.last_consumed_stop_generation.value(), 1U);
  EXPECT_EQ(installed.downstream, core::DownstreamFenceState::kFenceRequested);
}

TEST_F(MechanismTest, FailedStopReplacementEndpointWriteDoesNotRealizeFinalCommand)
{
  for (std::size_t blocked = 0; blocked < handles_.size(); ++blocked) {
    const auto current = Prepared();
    const auto stop = Stop(current);
    const auto next = StopReplacement(StopEntry(current, stop));
    ControllerMechanism receiver(Profile());
    Start(receiver, current);
    ASSERT_EQ(receiver.PostStop(current.motion.key, stop).refusal, Refusal::kNone);
    ASSERT_EQ(receiver.Replace(current.motion.key, next.motion.key, next).refusal, Refusal::kNone);
    for (std::int64_t tick = 1; tick <= 20; ++tick) {
      ASSERT_TRUE(receiver.Tick(tick * kPeriod, commands_));
    }
    const auto previous = receiver.ReadMechanismStatus().keys.at(1);
    ASSERT_FALSE(previous.realization.final_command_realized);
    std::binary_semaphore locked{0};
    std::binary_semaphore release{0};
    std::jthread owner([&]() {
        auto lock = handles_[blocked]->Lock();
        locked.release();
        release.acquire();
      });
    locked.acquire();
    const bool wrote = receiver.Tick(21 * kPeriod, commands_);
    release.release();
    owner.join();
    EXPECT_FALSE(wrote);
    const auto endpoint = receiver.ReadMechanismStatus().keys.at(1);
    EXPECT_EQ(endpoint.last_step_status, core::MotionStepStatus::kStopped);
    EXPECT_FALSE(endpoint.realization.final_command_realized);
    EXPECT_EQ(endpoint.realization.last_realized_sequence,
      previous.realization.last_realized_sequence);
    EXPECT_EQ(endpoint.realization.last_computed_sequence,
      previous.realization.last_computed_sequence + 1U);
    EXPECT_EQ(endpoint.dispatch.effect,
      blocked == 0U ? Effect::kEffectObserved : Effect::kPartialEffectObserved);
    EXPECT_EQ(endpoint.downstream, core::DownstreamFenceState::kFenceRequested);
  }
}

TEST_F(MechanismTest, PartialInterfaceWriteContainsAndPreservesDeliveryUncertainty)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  Start(receiver, motion);
  std::binary_semaphore locked{0};
  std::binary_semaphore release{0};
  std::jthread owner([&]() {
      auto lock = handles_[1]->Lock();
      locked.release();
      release.acquire();
    });
  locked.acquire();
  const bool wrote = receiver.Tick(kPeriod, commands_);
  release.release();
  owner.join();
  EXPECT_FALSE(wrote);
  const auto key = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_EQ(key.dispatch.effect, Effect::kPartialEffectObserved);
  EXPECT_EQ(key.realization.last_realized_sequence, 1U);
  EXPECT_EQ(key.realization.last_computed_sequence, 2U);
  EXPECT_FALSE(key.realization.last_delivered_sequence.has_value());
  EXPECT_EQ(key.downstream, core::DownstreamFenceState::kFenceRequested);
}

TEST_F(MechanismTest, LaterZeroWriteFailureDoesNotErasePreviouslyObservedGoalEffect)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  Start(receiver, motion);
  EXPECT_FALSE(receiver.Tick(kPeriod, {}));
  const auto key = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_EQ(key.dispatch.phase, Phase::kRealized);
  EXPECT_EQ(key.dispatch.effect, Effect::kEffectObserved);
  EXPECT_EQ(key.realization.last_realized_sequence, 1U);
  EXPECT_EQ(key.realization.last_computed_sequence, 2U);
  EXPECT_EQ(key.downstream, core::DownstreamFenceState::kFenceRequested);
}

TEST_F(MechanismTest, TickDoesNotAllocateOnRunningStopOrInterfaceCountFailure)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  Start(receiver, motion);
  ASSERT_EQ(receiver.PostStop(motion.motion.key, Stop(motion)).refusal, Refusal::kNone);
  allocation_count = 0U;
  count_allocations = true;
  const bool wrote = receiver.Tick(kPeriod, commands_);
  const bool wrong_count = receiver.Tick(2 * kPeriod, {});
  count_allocations = false;
  EXPECT_TRUE(wrote);
  EXPECT_FALSE(wrong_count);
  EXPECT_EQ(allocation_count, 0U);
}

TEST_F(MechanismTest, RetainsReservedStopWhileNewGoalWaitsBehindOldReleaseEvents)
{
  const auto old = Prepared();
  const auto next = Prepared(2U);
  ControllerMechanism receiver(Profile());
  Start(receiver, old);
  for (std::size_t i = 0; i < ControllerMechanism::kCapacity; ++i) {
    ASSERT_EQ(receiver.Release(old.motion.key).refusal, Refusal::kNone);
  }
  EXPECT_FALSE(receiver.Tick(kPeriod, commands_));
  EXPECT_FALSE(receiver.ReadMechanismStatus().keys.at(0).slot_referenced);
  ASSERT_EQ(receiver.Submit(next.motion.key, next).refusal, Refusal::kNone);
  ASSERT_EQ(receiver.PostStop(next.motion.key, Stop(next)).refusal, Refusal::kNone);
  for (std::size_t i = 0; i < 3U; ++i) {
    EXPECT_FALSE(receiver.Tick(2 * kPeriod, commands_));
  }
  EXPECT_TRUE(receiver.Tick(2 * kPeriod, commands_));
  const auto key = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_EQ(key.last_consumed_stop_generation.value(), 1U);
  EXPECT_EQ(key.last_step_status, core::MotionStepStatus::kBraking);
}

TEST_F(MechanismTest, RejectsWrongLoanedResourceOrderBeforeAnyWrite)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  ASSERT_EQ(receiver.RenewHostLease(10000000000).refusal, Refusal::kNone);
  ASSERT_EQ(receiver.Submit(motion.motion.key, motion).refusal, Refusal::kNone);
  std::vector<hardware_interface::LoanedCommandInterface> reversed;
  reversed.emplace_back(handles_[1], nullptr);
  reversed.emplace_back(handles_[0], nullptr);
  EXPECT_FALSE(receiver.Tick(0, reversed));
  EXPECT_DOUBLE_EQ(*handles_[0]->get_optional<double>(), 9.0);
  EXPECT_DOUBLE_EQ(*handles_[1]->get_optional<double>(), 9.0);
  const auto key = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_EQ(key.realization.last_realized_sequence, 0U);
  EXPECT_EQ(key.dispatch.effect, Effect::kKnownNoEffect);
}

TEST_F(MechanismTest, CapacityRefusalDoesNotBorrowFifthPreparedSlot)
{
  ControllerMechanism receiver(Profile());
  std::vector<core::PreparedMotionSlot> motions;
  for (std::uint64_t generation = 1U; generation <= 5U; ++generation) {
    motions.push_back(Prepared(generation));
  }
  for (std::size_t i = 0; i < ControllerMechanism::kCapacity; ++i) {
    ASSERT_EQ(receiver.Submit(motions[i].motion.key, motions[i]).refusal, Refusal::kNone);
  }
  const auto result = receiver.Submit(motions.back().motion.key, motions.back());
  EXPECT_EQ(result.refusal, Refusal::kChannelFull);
  EXPECT_EQ(result.disposition, core::StopStageAttemptDispositionDTO::kNotDispatched);
  EXPECT_EQ(receiver.ReadMechanismStatus().keys.size(), ControllerMechanism::kCapacity);
}

TEST_F(MechanismTest, PreInstallFenceIsVisibleOnFirstConsumedStatus)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  ASSERT_EQ(receiver.RenewHostLease(10000000000).refusal, Refusal::kNone);
  ASSERT_EQ(receiver.Submit(motion.motion.key, motion).refusal, Refusal::kNone);
  ASSERT_EQ(receiver.Fence(motion.motion.key).refusal, Refusal::kNone);
  EXPECT_FALSE(receiver.Tick(0, commands_));
  const auto key = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_EQ(key.dispatch.phase, Phase::kConsumed);
  EXPECT_EQ(key.dispatch.effect, Effect::kKnownNoEffect);
  EXPECT_EQ(key.downstream, core::DownstreamFenceState::kFenceRequested);
  EXPECT_FALSE(key.slot_referenced);
  EXPECT_DOUBLE_EQ(*handles_[0]->get_optional<double>(), 9.0);
}

TEST_F(MechanismTest, WrongInterfaceTypeRefusesAllWritesWithoutAllocating)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  ASSERT_EQ(receiver.RenewHostLease(10000000000).refusal, Refusal::kNone);
  ASSERT_EQ(receiver.Submit(motion.motion.key, motion).refusal, Refusal::kNone);
  auto wrong = std::make_shared<hardware_interface::CommandInterface>(
    "j2", "position", "float32", "9");
  std::vector<hardware_interface::LoanedCommandInterface> interfaces;
  interfaces.emplace_back(handles_[0], nullptr);
  interfaces.emplace_back(wrong, nullptr);
  allocation_count = 0U;
  count_allocations = true;
  const bool wrote = receiver.Tick(0, interfaces);
  count_allocations = false;
  EXPECT_FALSE(wrote);
  EXPECT_EQ(allocation_count, 0U);
  EXPECT_DOUBLE_EQ(*handles_[0]->get_optional<double>(), 9.0);
  EXPECT_EQ(receiver.ReadMechanismStatus().keys.at(0).realization.last_realized_sequence, 0U);
}

TEST_F(MechanismTest, ScaleWriteEffectIsSeparateFromTargetAppliedAndInvalidInput)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  Start(receiver, motion);
  ASSERT_EQ(receiver.PostScale(motion.motion.key,
    {motion.motion.key.fence(), core::RuntimeScaleGeneration{2U}, 0.5}).refusal, Refusal::kNone);
  ASSERT_TRUE(receiver.Tick(kPeriod, commands_));
  auto key = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_EQ(key.scale.phase, Phase::kConsumed);
  EXPECT_EQ(key.scale.effect, Effect::kEffectObserved);
  ASSERT_EQ(receiver.PostScale(motion.motion.key,
    {motion.motion.key.fence(), core::RuntimeScaleGeneration{3U}, -1.0}).refusal, Refusal::kNone);
  ASSERT_TRUE(receiver.Tick(2 * kPeriod, commands_));
  key = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_EQ(key.last_consumed_scale_generation.value(), 2U);
  EXPECT_EQ(key.scale.effect, Effect::kEffectObserved);
  for (std::int64_t tick = 3; tick < 1000; ++tick) {
    ASSERT_TRUE(receiver.Tick(tick * kPeriod, commands_));
  }
  EXPECT_EQ(receiver.ReadMechanismStatus().keys.at(0).scale.phase, Phase::kRealized);
}

TEST_F(MechanismTest, AcceptedStopWriteHasLocalEffectWithoutClaimingRestOrDelivery)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  Start(receiver, motion);
  ASSERT_EQ(receiver.PostScale(motion.motion.key,
    {motion.motion.key.fence(), core::RuntimeScaleGeneration{2U}, 0.5}).refusal, Refusal::kNone);
  ASSERT_EQ(receiver.PostStop(motion.motion.key, Stop(motion)).refusal, Refusal::kNone);
  ASSERT_TRUE(receiver.Tick(kPeriod, commands_));
  const auto key = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_EQ(key.stop.phase, Phase::kRealized);
  EXPECT_EQ(key.stop.effect, Effect::kEffectObserved);
  EXPECT_EQ(key.last_step_status, core::MotionStepStatus::kBraking);
  EXPECT_EQ(key.last_consumed_scale_generation.value(), 1U);
  EXPECT_EQ(key.scale.phase, Phase::kAccepted);
  EXPECT_EQ(key.scale.effect, Effect::kNone);
  EXPECT_FALSE(key.realization.last_delivered_sequence.has_value());
  ASSERT_EQ(receiver.Fence(motion.motion.key).refusal, Refusal::kNone);
  EXPECT_FALSE(receiver.Tick(2 * kPeriod, commands_));
  const auto fenced = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_EQ(fenced.stop.phase, Phase::kRealized);
  EXPECT_EQ(fenced.stop.effect, Effect::kEffectObserved);
  EXPECT_EQ(fenced.last_consumed_stop_generation.value(), 1U);
}

TEST_F(MechanismTest, StopCannotRealizeAnEarlierPendingScaleTargetByCoincidence)
{
  const auto motion = Prepared();
  ControllerMechanism receiver(Profile());
  Start(receiver, motion);
  ASSERT_EQ(receiver.PostScale(motion.motion.key,
    {motion.motion.key.fence(), core::RuntimeScaleGeneration{2U}, 0.0}).refusal, Refusal::kNone);
  ASSERT_TRUE(receiver.Tick(kPeriod, commands_));
  ASSERT_EQ(receiver.ReadMechanismStatus().keys.at(0).scale.phase, Phase::kConsumed);
  auto stop = Stop(motion);
  stop.braking_deadline_ns = 2000000000;
  ASSERT_EQ(receiver.PostStop(motion.motion.key, stop).refusal, Refusal::kNone);
  for (std::int64_t tick = 2; tick < 1000; ++tick) {
    ASSERT_TRUE(receiver.Tick(tick * kPeriod, commands_));
  }
  const auto key = receiver.ReadMechanismStatus().keys.at(0);
  EXPECT_EQ(key.last_step_status, core::MotionStepStatus::kStopped);
  EXPECT_LT(*handles_[0]->get_optional<double>(),
    motion.motion.trajectory.waypoints.back().positions_rad[0]);
  EXPECT_FALSE(key.realization.final_command_realized);
  EXPECT_EQ(key.scale.phase, Phase::kConsumed);
  EXPECT_EQ(key.scale.effect, Effect::kEffectObserved);
  EXPECT_EQ(key.stop.phase, Phase::kRealized);
}

TEST_F(MechanismTest, ActualPluginUpdateUsesReceiverClockAndLoanedCommandHandles)
{
  const auto motion = Prepared();
  auto receiver = std::make_unique<ControllerMechanism>(Profile());
  auto * port = receiver.get();
  ASSERT_EQ(port->RenewHostLease(10000000000).refusal, Refusal::kNone);
  ASSERT_EQ(port->Submit(motion.motion.key, motion).refusal, Refusal::kNone);
  MotionController controller;
  controller_interface::ControllerInterfaceParams params;
  params.controller_name = "real_step_tick_test";
  params.update_rate = 100U;
  params.controller_manager_update_rate = 100U;
  params.node_options.enable_rosout(false);
  params.node_options.start_parameter_services(false);
  params.node_options.enable_logger_service(false);
  params.node_options.parameter_overrides({rclcpp::Parameter("joints", Profile().joints)});
  ASSERT_EQ(controller.init(params), controller_interface::return_type::OK);
  static_cast<void>(controller.configure());
  controller.assign_interfaces(std::move(commands_), {});
  MotionControllerTestPeer::Attach(controller, std::move(receiver));
  EXPECT_EQ(controller.update(rclcpp::Time(999999999), rclcpp::Duration(7, 0)),
    controller_interface::return_type::OK);
  now_.store(kPeriod);
  EXPECT_EQ(controller.update(rclcpp::Time(0), rclcpp::Duration(0, 0)),
    controller_interface::return_type::OK);
  EXPECT_DOUBLE_EQ(*handles_[0]->get_optional<double>(), 0.001);
  EXPECT_EQ(port->ReadMechanismStatus().loop.last_tick_local_ns, kPeriod);
  EXPECT_EQ(controller.on_activate(rclcpp_lifecycle::State()),
    controller_interface::CallbackReturn::FAILURE);
}
}  // namespace
}  // namespace manyforge_motion_controller
