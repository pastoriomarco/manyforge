// Compile-only checkpoint for the shared execution interfaces. StepMotion is
// referenced only in unevaluated contexts; its behavior is in test_motion_step.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <manyforge_core/execution/execution_goal_key.hpp>
#include <manyforge_core/execution/execution_knowledge.hpp>
#include <manyforge_core/execution/motion_step.hpp>
#include <manyforge_core/execution/prepared_motion.hpp>
#include <manyforge_core/execution/prepared_motion_slot.hpp>
#include <manyforge_core/ports/iexecution_mechanism.hpp>
#include <optional>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

// ---- RT values: trivially copyable, fixed size, no owned storage. ----
template <typename T>
constexpr bool kRtValue =
    std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T> &&
    std::is_nothrow_default_constructible_v<T> && std::is_standard_layout_v<T>;

static_assert(kRtValue<ExecutionGoalFence>);
static_assert(kRtValue<StopTransition>);
static_assert(kRtValue<ScaleTarget>);
static_assert(kRtValue<DeviceScalingFacts>);
static_assert(kRtValue<MotionStepInputs>);
static_assert(kRtValue<MotionJointCommand>);
static_assert(kRtValue<MotionStepOutput>);
static_assert(kRtValue<CommandRealizationContract>);
static_assert(kRtValue<MotionTickEnvelope>);
static_assert(kRtValue<MotionContinuityTolerance>);
static_assert(kRtValue<ExecutionCommandKnowledge>);
static_assert(kRtValue<ExecutionMechanismCallResult>);
// optional<> is not standard-layout-guaranteed; the state needs only the copy
// and destruction guarantees for a fixed loop-owned buffer.
static_assert(std::is_trivially_copyable_v<MotionStepState>);
static_assert(std::is_trivially_destructible_v<MotionStepState>);
static_assert(std::is_nothrow_default_constructible_v<MotionStepState>);
static_assert(std::is_trivially_copyable_v<detail::RuntimeScaleGovernor>);

static_assert(kMaxMotionJoints == 16U);
static_assert(sizeof(MotionJointCommand::position_rad) == kMaxMotionJoints * sizeof(double));
static_assert(sizeof(MotionStepOutput) < 640U);
static_assert(sizeof(MotionStepState) <= 320U);

// Sealed transport inputs and the retained slot are NRT values owned with
// the motion; the port borrows the slot by reference.
static_assert(!std::is_trivially_copyable_v<SealedPreparedMotionInputs>);
static_assert(std::is_same_v<decltype(PreparedMotionSlot::motion), PreparedMotion>);
static_assert(std::is_same_v<decltype(PreparedMotionSlot::sealed), SealedPreparedMotionInputs>);
static_assert(sizeof(ExecutionGoalFence) == 2U * sizeof(std::uint64_t));

// Prepared motion is cold and immutable, NRT-owned: copyable for building,
// but its per-joint RT reads are fixed-capacity.
static_assert(std::tuple_size_v<decltype(PreparedMotion::effective_limits)> == kMaxMotionJoints);
static_assert(std::tuple_size_v<decltype(PreparedMotion::final_position_tolerance_rad)> ==
              kMaxMotionJoints);

// ---- StepMotion: exact signature, noexcept, nodiscard result. ----
using StepMotionFn = MotionStepStatus (*)(const PreparedMotion&, MotionStepState&,
                                          const MotionStepInputs&, MotionStepOutput&) noexcept;
static_assert(std::is_same_v<decltype(&StepMotion), StepMotionFn>);

// ---- Replacement installation: exact signature, noexcept, fixed result. ----
using InstallReplacementFn = ReplacementInstallResult (*)(
    const PreparedMotion&, const MotionStepState&, const MotionStepOutput&, const PreparedMotion&,
    std::int64_t, MotionStepState&, MotionStepOutput&) noexcept;
static_assert(std::is_same_v<decltype(&InstallReplacementMotionStep), InstallReplacementFn>);
static_assert(kRtValue<ReplacementInstallResult>);
static_assert(noexcept(StepMotion(std::declval<const PreparedMotion&>(),
                                  std::declval<MotionStepState&>(),
                                  std::declval<const MotionStepInputs&>(),
                                  std::declval<MotionStepOutput&>())));

// ---- Mechanism port: mutating calls are noexcept. ----
static_assert(noexcept(std::declval<IExecutionMechanism&>().PostStop(
    std::declval<const ExecutionGoalKey&>(), std::declval<const StopTransition&>())));
static_assert(noexcept(std::declval<IExecutionMechanism&>().Submit(
    std::declval<const ExecutionGoalKey&>(), std::declval<const PreparedMotionSlot&>())));
static_assert(noexcept(std::declval<IExecutionMechanism&>().Replace(
    std::declval<const ExecutionGoalKey&>(), std::declval<const ExecutionGoalKey&>(),
    std::declval<const PreparedMotionSlot&>())));
static_assert(noexcept(std::declval<IExecutionMechanism&>().Fence(
    std::declval<const ExecutionGoalKey&>())));
static_assert(noexcept(std::declval<IExecutionMechanism&>().RenewHostLease(std::int64_t{})));
static_assert(std::has_virtual_destructor_v<IExecutionMechanism>);

// Minimal single-key fake proving the port is implementable without policy.
class FakeExecutionMechanism final : public IExecutionMechanism {
 public:
  ExecutionMechanismCallResult Submit(const ExecutionGoalKey& key,
                                      const PreparedMotionSlot& slot) noexcept override {
    if (!SlotMatches(slot, key)) {
      return Refuse(ExecutionMechanismRefusal::kSlotKeyMismatch);
    }
    if (active_.has_value()) {
      return Refuse(ExecutionMechanismRefusal::kChannelFull);
    }
    active_ = ExecutionMechanismKeyStatus{};
    active_->key = key;
    active_->dispatch.phase = ExecutionCommandPhase::kSubmitted;
    active_->slot_referenced = true;
    return Submitted();
  }

  ExecutionMechanismCallResult Replace(const ExecutionGoalKey& key,
                                       const ExecutionGoalKey& next_key,
                                       const PreparedMotionSlot& slot) noexcept override {
    if (!Matches(key)) {
      return Refuse(ExecutionMechanismRefusal::kStaleKey);
    }
    if (!SlotMatches(slot, next_key)) {
      return Refuse(ExecutionMechanismRefusal::kSlotKeyMismatch);
    }
    if (!(next_key.controller_goal_generation > key.controller_goal_generation)) {
      return Refuse(ExecutionMechanismRefusal::kInvalidInput);
    }
    active_->key = next_key;
    active_->dispatch = ExecutionCommandKnowledge{ExecutionCommandPhase::kSubmitted,
                                                  ComponentLifecycleDomainEffectDTO::kNone};
    return Submitted();
  }

  ExecutionMechanismCallResult Release(const ExecutionGoalKey& key) noexcept override {
    if (!Matches(key)) {
      return Refuse(ExecutionMechanismRefusal::kUnknownKey);
    }
    active_->slot_referenced = false;
    return Submitted();
  }

  ExecutionMechanismCallResult PostScale(const ExecutionGoalKey& key,
                                         const ScaleTarget& target) noexcept override {
    if (!Matches(key) || !(target.goal == key.fence())) {
      return Refuse(ExecutionMechanismRefusal::kStaleKey);
    }
    pending_scale_ = target;
    active_->scale.phase = ExecutionCommandPhase::kSubmitted;
    return Submitted();
  }

  ExecutionMechanismCallResult PostStop(const ExecutionGoalKey& key,
                                        const StopTransition& transition) noexcept override {
    if (!Matches(key) || !(transition.goal == key.fence())) {
      return Refuse(ExecutionMechanismRefusal::kStaleKey);
    }
    pending_stop_ = transition;
    active_->stop.phase = ExecutionCommandPhase::kSubmitted;
    return Submitted();
  }

  ExecutionMechanismCallResult Hold(const ExecutionGoalKey& key) noexcept override {
    return Matches(key) ? Submitted() : Refuse(ExecutionMechanismRefusal::kStaleKey);
  }

  ExecutionMechanismCallResult Cancel(const ExecutionGoalKey& key) noexcept override {
    if (!Matches(key)) {
      return Refuse(ExecutionMechanismRefusal::kStaleKey);
    }
    // A lost device acknowledgement: phase stays submitted, effect unknown.
    active_->stop.effect = ComponentLifecycleDomainEffectDTO::kEffectUncertain;
    return ExecutionMechanismCallResult{StopStageAttemptDispositionDTO::kEffectUncertain,
                                        ExecutionMechanismRefusal::kNone};
  }

  ExecutionMechanismCallResult Fence(const ExecutionGoalKey& key) noexcept override {
    if (!Matches(key)) {
      return Refuse(ExecutionMechanismRefusal::kStaleKey);
    }
    active_->downstream = DownstreamFenceState::kFenced;
    return Submitted();
  }

  ExecutionMechanismCallResult RenewHostLease(std::int64_t lease_duration_ns) noexcept override {
    if (lease_duration_ns <= 0) {
      return Refuse(ExecutionMechanismRefusal::kInvalidInput);
    }
    lease_duration_ns_ = lease_duration_ns;
    return Submitted();
  }

  [[nodiscard]] std::int64_t lease_duration_ns() const noexcept { return lease_duration_ns_; }

  ExecutionMechanismStatus ReadMechanismStatus() const override {
    ExecutionMechanismStatus status;
    if (active_.has_value()) {
      status.keys.push_back(*active_);
    }
    return status;
  }

  [[nodiscard]] const std::optional<StopTransition>& pending_stop() const noexcept {
    return pending_stop_;
  }

 private:
  // The borrowed slot must carry the key and the digest it was sealed under.
  [[nodiscard]] static bool SlotMatches(const PreparedMotionSlot& slot,
                                        const ExecutionGoalKey& key) noexcept {
    return slot.motion.key == key &&
           slot.sealed.participant_digest == slot.motion.prepared_contract_digest;
  }
  [[nodiscard]] bool Matches(const ExecutionGoalKey& key) const noexcept {
    return active_.has_value() && active_->key == key;
  }
  static ExecutionMechanismCallResult Submitted() noexcept {
    return {StopStageAttemptDispositionDTO::kDispatched, ExecutionMechanismRefusal::kNone};
  }
  static ExecutionMechanismCallResult Refuse(ExecutionMechanismRefusal refusal) noexcept {
    return {StopStageAttemptDispositionDTO::kNotDispatched, refusal};
  }

  std::optional<ExecutionMechanismKeyStatus> active_;
  std::optional<ScaleTarget> pending_scale_;
  std::optional<StopTransition> pending_stop_;
  std::int64_t lease_duration_ns_{};
};

ExecutionGoalKey MakeKey(std::uint64_t goal_generation) {
  ExecutionGoalKey key;
  key.operation_id = OperationId{"op-1"};
  key.controller_goal_id = ControllerGoalId{"goal-1"};
  key.controller_goal_generation = ControllerGoalGeneration{goal_generation};
  key.prepared_execution_id = PreparedExecutionId{7U};
  key.binding.active_control_configuration_generation = ActiveControlConfigurationGeneration{3U};
  key.binding.provider_runtime_epoch = RuntimeEpoch{"epoch-a"};
  return key;
}

PreparedMotionSlot MakeSlot(std::uint64_t goal_generation) {
  PreparedMotionSlot slot;
  slot.motion.key = MakeKey(goal_generation);
  slot.motion.prepared_contract_digest = ContentDigest{"sha256:participant"};
  slot.sealed.participant_digest = slot.motion.prepared_contract_digest;
  return slot;
}

TEST(ExecutionInterfacesCompile, FakeMechanismHonorsKeyFencingAndSeparateFacts) {
  FakeExecutionMechanism mechanism;
  IExecutionMechanism& port = mechanism;

  const PreparedMotionSlot slot = MakeSlot(1U);

  EXPECT_EQ(port.Submit(MakeKey(1U), slot).disposition,
            StopStageAttemptDispositionDTO::kDispatched);

  // Binding identity fences the key even when goal identity matches.
  ExecutionGoalKey other_epoch = MakeKey(1U);
  other_epoch.binding.provider_runtime_epoch = RuntimeEpoch{"epoch-b"};
  const auto stale = port.Hold(other_epoch);
  EXPECT_EQ(stale.disposition, StopStageAttemptDispositionDTO::kNotDispatched);
  EXPECT_EQ(stale.refusal, ExecutionMechanismRefusal::kStaleKey);

  StopTransition stop;
  stop.goal = slot.motion.key.fence();
  stop.stop_generation = StopGeneration{1U};
  EXPECT_EQ(port.PostStop(slot.motion.key, stop).disposition,
            StopStageAttemptDispositionDTO::kDispatched);
  ASSERT_TRUE(mechanism.pending_stop().has_value());

  EXPECT_EQ(port.Cancel(slot.motion.key).disposition, StopStageAttemptDispositionDTO::kEffectUncertain);

  // Retirement: slot release and downstream fence are separate facts.
  EXPECT_EQ(port.Release(slot.motion.key).disposition, StopStageAttemptDispositionDTO::kDispatched);
  auto status = port.ReadMechanismStatus();
  ASSERT_EQ(status.keys.size(), 1U);
  EXPECT_FALSE(status.keys.front().slot_referenced);
  EXPECT_EQ(status.keys.front().downstream, DownstreamFenceState::kOpen);
  EXPECT_EQ(status.keys.front().stop.phase, ExecutionCommandPhase::kSubmitted);
  EXPECT_EQ(status.keys.front().stop.effect, ComponentLifecycleDomainEffectDTO::kEffectUncertain);

  EXPECT_EQ(port.Fence(slot.motion.key).disposition, StopStageAttemptDispositionDTO::kDispatched);
  status = port.ReadMechanismStatus();
  EXPECT_EQ(status.keys.front().downstream, DownstreamFenceState::kFenced);

  // The host lease is renewed with a duration, never an absolute time.
  EXPECT_EQ(port.RenewHostLease(0).refusal, ExecutionMechanismRefusal::kInvalidInput);
  EXPECT_EQ(port.RenewHostLease(250'000'000).disposition,
            StopStageAttemptDispositionDTO::kDispatched);
  EXPECT_EQ(mechanism.lease_duration_ns(), 250'000'000);
}

TEST(ExecutionInterfacesCompile, ReplacementRequiresNewerGoalGeneration) {
  FakeExecutionMechanism mechanism;
  const PreparedMotionSlot first = MakeSlot(1U);
  ASSERT_EQ(mechanism.Submit(first.motion.key, first).disposition,
            StopStageAttemptDispositionDTO::kDispatched);

  const PreparedMotionSlot same_generation = MakeSlot(1U);
  EXPECT_EQ(
      mechanism.Replace(first.motion.key, same_generation.motion.key, same_generation).refusal,
            ExecutionMechanismRefusal::kInvalidInput);

  // A slot whose sealed inputs name another participant is refused.
  PreparedMotionSlot foreign = MakeSlot(2U);
  foreign.sealed.participant_digest = ContentDigest{"sha256:other"};
  EXPECT_EQ(mechanism.Replace(first.motion.key, foreign.motion.key, foreign).refusal,
            ExecutionMechanismRefusal::kSlotKeyMismatch);

  const PreparedMotionSlot replacement = MakeSlot(2U);
  EXPECT_EQ(mechanism.Replace(first.motion.key, replacement.motion.key, replacement).disposition,
            StopStageAttemptDispositionDTO::kDispatched);
  EXPECT_EQ(mechanism.ReadMechanismStatus().keys.front().key.fence(),
            replacement.motion.key.fence());
}

}  // namespace
}  // namespace manyforge::core
