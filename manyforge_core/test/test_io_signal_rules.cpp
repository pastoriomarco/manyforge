#include <gtest/gtest.h>

#include <latch>
#include <manyforge_core/io_condition_evaluator.hpp>
#include <manyforge_core/io_signal_ingress.hpp>
#include <manyforge_core/io_signal_subsystem.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <thread>

namespace manyforge::core {
namespace {

const InterruptSourceId kA("source.a"), kB("source.b");
const IoSourceIncarnation kIncarnation("source.1");

IoSignalId id(std::string name = "a") {
  return {ControllableComponentId("io"), std::move(name)};
}

IoSignalDescriptorDTO descriptor(std::string name, InterruptSourceId source) {
  return {id(std::move(name)),
          std::move(source),
          IoSignalValueType::kBoolean,
          IoSignalDirection::kInput,
          IoSignalUpdateMode::kSampled,
          IoTransitionCapability::kSampledOnly,
          {IoFreshnessMode::kSampleAge, 10},
          std::nullopt, IoSignalLivenessMode::kPeriodic};
}

std::unique_ptr<IoSignalSubsystem> signals(FakeClock& clock, bool same_source = false,
                                           std::size_t registrations = 8, std::size_t records = 8) {
  auto created = IoSignalSubsystem::Create(
      {RequestOwnerEpoch("host"), 1, {descriptor("a", kA), descriptor("b", same_source ? kA : kB)}},
      {2, 2, records, registrations, 128, 1024}, clock);
  if (!created.ok()) return {};
  auto service = std::move(created.value());
  if (!service->InstallSource(kA, kIncarnation, std::nullopt).ok()) return {};
  if (!same_source && !service->InstallSource(kB, kIncarnation, std::nullopt).ok()) return {};
  return service;
}

IoSourceAcquisitionDTO frame(std::uint64_t sequence, TimePoint received, bool value,
                             InterruptSourceId source = kA, std::string name = "a") {
  return {std::move(source),
          kIncarnation,
          sequence,
          received,
          {{id(std::move(name)), IoSampleValidity::kValid, value, std::nullopt}}};
}

IoConditionNodeDTO comparison(std::size_t input) {
  IoConditionCompareDTO node{IoConditionComparison::kEqual, IoConditionInputReferenceDTO{input},
                             IoConditionValueDTO{true}};
  return node;
}

std::shared_ptr<const CheckedIoCondition> condition(bool compound = false, bool any = false) {
  IoConditionDefinitionDTO definition;
  definition.inputs.push_back({id(), IoConditionValueKind::kBoolean});
  definition.nodes.push_back(comparison(0));
  if (compound) {
    definition.inputs.push_back({id("b"), IoConditionValueKind::kBoolean});
    definition.nodes.push_back(comparison(1));
    if (any)
      definition.nodes.emplace_back(IoConditionAnyDTO{{0, 1}});
    else
      definition.nodes.emplace_back(IoConditionAllDTO{{0, 1}});
  }
  auto checked = CheckIoCondition(std::move(definition), {2, 3, 2, 2, 128, 128});
  if (!checked.ok()) return {};
  return std::make_shared<const CheckedIoCondition>(std::move(checked.value()));
}

IoOperationRuleScopeDTO operation_scope() {
  return {OperationId("operation"),
          OperationRevision(1),
          OperationParticipantId("participant"),
          ControllableComponentId("io"),
          CommandTargetId("arm"),
          ControllerGoalId("goal"),
          ControllerGoalGeneration(1),
          7,
          ContentDigest("prepared.digest"),
          MotionInterruptScopeId("scope")};
}

IoActivationRuleScopeDTO activation_scope() {
  return {RetainedBundleId("bundle"), GraphRevision("graph")};
}

IoRuleArmRequestDTO request(
    MotionInterruptActivationMode mode = MotionInterruptActivationMode::kRequireInactiveAtArm) {
  return {InterruptPolicyId("rule"), operation_scope(), condition(), mode, {kA}};
}

TEST(IoSignalRuleTest, ConfiguredArmModesPreserveInactiveImmediateAndFreshObservedEdgeSemantics) {
  FakeClock clock;
  auto service = signals(clock);
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Publish(frame(1, 0, true)).ok());
  const auto inactive = service->ArmRule(request());
  ASSERT_FALSE(inactive.ok());
  EXPECT_EQ(inactive.error().code, kErrorMotionInterruptSourceActiveAtArm);
  const auto immediate =
      service->ArmRule(request(MotionInterruptActivationMode::kTripIfAlreadyActive));
  ASSERT_TRUE(immediate.ok());
  const auto immediate_evidence = service->ReadRuleEvidence(immediate.value());
  ASSERT_TRUE(immediate_evidence.ok());
  ASSERT_TRUE(immediate_evidence.value().trip);
  EXPECT_EQ(immediate_evidence.value().trip->registration, immediate.value());
  ASSERT_EQ(immediate_evidence.value().trip->matched_samples.size(), 1U);
  EXPECT_EQ(immediate_evidence.value().trip->matched_samples.front().publication.source_sequence,
            1U);
  const auto fresh = service->ArmRule(request(MotionInterruptActivationMode::kFreshEdgeAfterArm));
  ASSERT_TRUE(fresh.ok());
  EXPECT_FALSE(service->ReadRuleEvidence(fresh.value()).value().trip);
  ASSERT_TRUE(service->Publish(frame(2, 0, true)).ok());
  EXPECT_FALSE(service->ReadRuleEvidence(fresh.value()).value().trip);
  ASSERT_TRUE(service->Publish(frame(3, 0, false)).ok());
  ASSERT_TRUE(service->Publish(frame(4, 0, true)).ok());
  const auto edge = service->ReadRuleEvidence(fresh.value()).value();
  ASSERT_TRUE(edge.trip);
  EXPECT_EQ(edge.trip->matched_samples.front().publication.source_sequence, 4U);
  EXPECT_TRUE(
      edge.source_losses.empty());  // sampled observable transition needs no capture promise
}

TEST(IoSignalRuleTest, WholeSourceAcquisitionPreventsHalfMessageTripAndRetainsSupportingSamples) {
  FakeClock clock;
  auto service = signals(clock, true, 8, 1);
  ASSERT_TRUE(service);
  auto baseline = frame(1, 0, false);
  baseline.updates.push_back({id("b"), IoSampleValidity::kValid, true, std::nullopt});
  ASSERT_TRUE(service->Publish(baseline).ok());
  auto rule = request();
  rule.condition = condition(true);
  const auto registration = service->ArmRule(rule);
  ASSERT_TRUE(registration.ok());
  auto flipped = frame(2, 0, true);
  flipped.updates.push_back({id("b"), IoSampleValidity::kValid, false, std::nullopt});
  ASSERT_TRUE(service->Publish(flipped).ok());
  EXPECT_FALSE(service->ReadRuleEvidence(registration.value()).value().trip);
  flipped.source_sequence = 3;
  flipped.updates.back().value = true;
  ASSERT_TRUE(service->Publish(flipped).ok());
  auto ended = frame(4, 0, false);
  ended.updates.push_back({id("b"), IoSampleValidity::kValid, false, std::nullopt});
  ASSERT_TRUE(service->Publish(ended).ok());
  const auto evidence = service->ReadRuleEvidence(registration.value()).value();
  ASSERT_TRUE(evidence.trip);
  ASSERT_EQ(evidence.trip->matched_samples.size(), 2U);
  for (const auto& sample : evidence.trip->matched_samples) {
    EXPECT_EQ(sample.publication.source_sequence, 3U);
    EXPECT_EQ(sample.publication.value, true);
  }
  ASSERT_EQ(evidence.trip->source_cursors.size(), 1U);
  EXPECT_TRUE(evidence.source_losses.empty());
  EXPECT_FALSE(evidence.evaluation_error);
  EXPECT_EQ(service->ReadSignal(id()).value().latest_sample->publication.value, false);
}

TEST(IoSignalRuleTest, RequiredSourceLossSurvivesRecoveryAndCannotManufactureTrip) {
  FakeClock clock;
  auto service = signals(clock);
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
  ASSERT_TRUE(service->Publish(frame(1, 0, false, kB, "b")).ok());
  auto rule = request();
  rule.condition = condition(true, true);
  rule.required_sources = {kA, kB};
  const auto registration = service->ArmRule(rule);
  ASSERT_TRUE(registration.ok());
  const ErrorDTO disconnected{"offline", "source lost", {}};
  ASSERT_TRUE(service->MarkSourceUnavailable(kB, kIncarnation, 1, disconnected).ok());
  ASSERT_TRUE(service->Publish(frame(2, 0, true)).ok());
  ASSERT_TRUE(service->Publish(frame(2, 0, false, kB, "b")).ok());
  ASSERT_TRUE(service->Publish(frame(3, 0, false)).ok());
  ASSERT_TRUE(service->Publish(frame(4, 0, true)).ok());
  const auto evidence = service->ReadRuleEvidence(registration.value()).value();
  EXPECT_FALSE(evidence.trip);
  ASSERT_EQ(evidence.source_losses.size(), 1U);
  const auto& loss = evidence.source_losses.front();
  EXPECT_EQ(loss.registration, registration.value());
  EXPECT_EQ(loss.source_id, kB);
  EXPECT_EQ(loss.incarnation, kIncarnation);
  EXPECT_EQ(loss.kind, IoRuleSourceFailureKind::kContinuityLoss);
  ASSERT_TRUE(loss.source_loss);
  EXPECT_EQ(loss.source_loss->reason, IoSourceLossReason::kDisconnected);
  EXPECT_EQ(loss.source_loss->error, disconnected);
}

TEST(IoSignalRuleTest, LossCannotHideBehindTrueOrAndRemainsSeparateFromPriorTrip) {
  FakeClock clock;
  auto service = signals(clock);
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Publish(frame(1, 0, true)).ok());
  ASSERT_TRUE(service->Publish(frame(1, 0, false, kB, "b")).ok());
  auto rule = request(MotionInterruptActivationMode::kTripIfAlreadyActive);
  rule.condition = condition(true, true);
  rule.required_sources = {kA, kB};
  const auto registration = service->ArmRule(rule);
  ASSERT_TRUE(registration.ok());
  clock.advance(9);
  ASSERT_TRUE(service->Publish(frame(2, 9, true)).ok());
  clock.advance(2);
  ASSERT_TRUE(service->EvaluateRules().ok());
  const auto evidence = service->ReadRuleEvidence(registration.value()).value();
  ASSERT_TRUE(evidence.trip);
  EXPECT_EQ(evidence.trip->matched_samples.front().publication.source_sequence, 1U);
  ASSERT_EQ(evidence.source_losses.size(), 1U);
  EXPECT_EQ(evidence.source_losses.front().source_id, kB);
  EXPECT_EQ(evidence.source_losses.front().kind, IoRuleSourceFailureKind::kStale);
  EXPECT_FALSE(evidence.source_losses.front().source_loss);
}

TEST(IoSignalRuleTest, AgeOnlyFailureAndPreCommitExpiryBothSurviveFreshRecovery) {
  for (const bool explicit_check : {false, true}) {
    FakeClock clock;
    auto service = signals(clock);
    ASSERT_TRUE(service);
    ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
    const auto registration = service->ArmRule(request());
    ASSERT_TRUE(registration.ok());
    clock.advance(11);
    if (explicit_check) {
      ASSERT_TRUE(service->EvaluateRules().ok());
      EXPECT_EQ(service->ReadRuleEvidence(registration.value()).value().source_losses.size(), 1U);
    }
    ASSERT_TRUE(service->Publish(frame(2, 11, true)).ok());
    const auto evidence = service->ReadRuleEvidence(registration.value()).value();
    EXPECT_FALSE(evidence.trip);
    ASSERT_EQ(evidence.source_losses.size(), 1U);
    EXPECT_EQ(evidence.source_losses.front().kind, IoRuleSourceFailureKind::kStale);
    ASSERT_TRUE(evidence.source_losses.front().sample);
    EXPECT_EQ(evidence.source_losses.front().sample->publication.source_sequence, 1U);
    EXPECT_EQ(evidence.source_losses.front().evaluated_at_ns, 11);
  }
}

TEST(IoSignalRuleTest, SequenceGapLatchesBeforeSameEnvelopeRecoveryAndHistoryOverwrite) {
  FakeClock clock;
  auto service = signals(clock, false, 8, 1);
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
  const auto registration = service->ArmRule(request());
  ASSERT_TRUE(registration.ok());
  ASSERT_TRUE(service->Publish(frame(3, 0, true)).ok());
  ASSERT_TRUE(service->ReportIngressOverflow(kA, kIncarnation, {"overflow", "", {}}).ok());
  ASSERT_TRUE(service->Publish(frame(4, 0, false)).ok());
  const auto evidence = service->ReadRuleEvidence(registration.value()).value();
  EXPECT_FALSE(evidence.trip);
  ASSERT_EQ(evidence.source_losses.size(), 1U);
  ASSERT_TRUE(evidence.source_losses.front().source_loss);
  const auto& loss = *evidence.source_losses.front().source_loss;
  EXPECT_EQ(loss.reason, IoSourceLossReason::kSequenceGap);
  EXPECT_EQ(loss.first_lost_source_sequence, 2U);
  EXPECT_EQ(loss.last_lost_source_sequence, 2U);
}

TEST(IoSignalRuleTest, SharedRegistrationBoundsAndExactRuleLifetimesDoNotFollowRunWatchTeardown) {
  FakeClock clock;
  auto service = signals(clock, false, 3);
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
  const IoWatchScopeDTO scope{
      RequestOwnerEpoch("runner"), ProgramRunId("run"), {ProgramNodeInstanceId("wait"), "attempt"}};
  const auto wait = service->RegisterLevelWatch({scope, id(), true});
  ASSERT_TRUE(wait.ok());
  auto activation_request = request();
  activation_request.scope = activation_scope();
  const auto activation = service->ArmRule(activation_request);
  const auto operation = service->ArmRule(request());
  ASSERT_TRUE(activation.ok());
  ASSERT_TRUE(operation.ok());
  EXPECT_FALSE(service->ArmRule(request()).ok());
  ASSERT_TRUE(
      service->ReleaseRunWatches(scope.program_runner_owner_epoch, scope.program_run_id).ok());
  EXPECT_EQ(service->ReadWatch(wait.value()).value().state, IoWatchState::kReleased);
  EXPECT_EQ(service->ReadRuleEvidence(activation.value()).value().state,
            IoRuleRegistrationState::kArmed);
  EXPECT_EQ(service->ReadRuleEvidence(operation.value()).value().state,
            IoRuleRegistrationState::kArmed);
  auto wrong = operation.value();
  std::get<IoOperationRuleScopeDTO>(wrong.scope).operation_revision = OperationRevision(2);
  EXPECT_FALSE(service->RetireRule(wrong).ok());
  EXPECT_FALSE(service->ReadRuleEvidence(wrong).ok());
  IoWatchRegistrationDTO wrong_kind{operation.value().owner_epoch,
                                    operation.value().catalog_generation,
                                    operation.value().registration_id, scope};
  EXPECT_FALSE(service->ReleaseWatch(wrong_kind).ok());
  ASSERT_TRUE(service->RetireRule(operation.value()).ok());
  ASSERT_TRUE(service->RetireRule(operation.value()).ok());
  ASSERT_TRUE(service->Publish(frame(2, 0, true)).ok());
  EXPECT_EQ(service->ReadRuleEvidence(operation.value()).value().state,
            IoRuleRegistrationState::kRetired);
  EXPECT_TRUE(service->ReadRuleEvidence(activation.value()).value().trip);
  ASSERT_TRUE(service->RetireRule(activation.value()).ok());
  auto rearmed = service->ArmRule(request(MotionInterruptActivationMode::kTripIfAlreadyActive));
  ASSERT_TRUE(rearmed.ok());
  EXPECT_GT(rearmed.value().registration_id, operation.value().registration_id);
  auto old_owner = rearmed.value();
  old_owner.owner_epoch = RequestOwnerEpoch("old");
  EXPECT_FALSE(service->ReadRuleEvidence(old_owner).ok());
  auto old_generation = rearmed.value();
  ++old_generation.catalog_generation;
  EXPECT_FALSE(service->RetireRule(old_generation).ok());
}

TEST(IoSignalRuleTest, UnsupportedModesInputsUnknownBaselineAndRequiredSourceMismatchRefuseArm) {
  FakeClock clock;
  auto service = signals(clock);
  ASSERT_TRUE(service);
  EXPECT_FALSE(service->ArmRule(request()).ok());
  ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
  auto unsupported = request(MotionInterruptActivationMode::kMaintainLevelWithHysteresis);
  EXPECT_EQ(service->ArmRule(unsupported).error().code, "unsupported_capability");
  unsupported.activation_mode = static_cast<MotionInterruptActivationMode>(255);
  EXPECT_EQ(service->ArmRule(unsupported).error().code, "unsupported_capability");
  auto missing_source = request();
  missing_source.required_sources.clear();
  EXPECT_FALSE(service->ArmRule(missing_source).ok());
  missing_source.required_sources = {kA, kA};
  EXPECT_FALSE(service->ArmRule(missing_source).ok());
  IoConditionDefinitionDTO bb_definition;
  bb_definition.inputs.push_back(
      {IoConditionBlackboardKeyDTO{"key"}, IoConditionValueKind::kBoolean});
  bb_definition.nodes.push_back(comparison(0));
  auto bb = CheckIoCondition(std::move(bb_definition), {1, 1, 1, 1, 128, 128});
  ASSERT_TRUE(bb.ok());
  auto bb_request = request();
  bb_request.condition = std::make_shared<const CheckedIoCondition>(std::move(bb.value()));
  const auto missing_baseline = service->ArmRule(bb_request);
  ASSERT_FALSE(missing_baseline.ok());
  EXPECT_EQ(missing_baseline.error().code, kErrorMotionInterruptArmFailed);
  EXPECT_EQ(missing_baseline.error().details.at("reason"), "io_rule_baseline_unavailable");
  const auto accepted = service->ArmRule(request());
  ASSERT_TRUE(accepted.ok());
  EXPECT_EQ(accepted.value().registration_id, 1U);  // refusals consume no registration
}

TEST(IoSignalRuleTest, SourceReplacementProducesLossWithoutTransitionAcrossIncarnations) {
  FakeClock clock;
  auto service = signals(clock);
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
  const auto registration =
      service->ArmRule(request(MotionInterruptActivationMode::kFreshEdgeAfterArm));
  ASSERT_TRUE(registration.ok());
  const IoSourceIncarnation replacement("source.2");
  ASSERT_TRUE(service->InstallSource(kA, replacement, kIncarnation).ok());
  auto next = frame(1, 0, true);
  next.incarnation = replacement;
  ASSERT_TRUE(service->Publish(next).ok());
  const auto evidence = service->ReadRuleEvidence(registration.value()).value();
  EXPECT_FALSE(evidence.trip);
  ASSERT_EQ(evidence.source_losses.size(), 1U);
  EXPECT_EQ(evidence.source_losses.front().incarnation, replacement);
  EXPECT_EQ(evidence.source_losses.front().source_loss->reason,
            IoSourceLossReason::kIncarnationChanged);
}

TEST(IoSignalRuleTest, InvalidAndUnavailableSamplesAreFailuresNeverFalseOrSignalTrips) {
  for (const auto validity : {IoSampleValidity::kInvalid, IoSampleValidity::kUnavailable}) {
    FakeClock clock;
    auto service = signals(clock);
    ASSERT_TRUE(service);
    ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
    const auto registration = service->ArmRule(request());
    ASSERT_TRUE(registration.ok());
    auto failed = frame(2, 0, true);
    failed.updates.front().validity = validity;
    failed.updates.front().value.reset();
    ASSERT_TRUE(service->Publish(failed).ok());
    ASSERT_TRUE(service->Publish(frame(3, 0, true)).ok());
    const auto evidence = service->ReadRuleEvidence(registration.value()).value();
    EXPECT_FALSE(evidence.trip);
    ASSERT_EQ(evidence.source_losses.size(), 1U);
    const auto expected = validity == IoSampleValidity::kInvalid
                              ? IoRuleSourceFailureKind::kInvalid
                              : IoRuleSourceFailureKind::kUnavailable;
    EXPECT_EQ(evidence.source_losses.front().kind, expected);
    ASSERT_TRUE(evidence.source_losses.front().sample);
    EXPECT_FALSE(evidence.source_losses.front().sample->publication.value);
  }
}

TEST(IoSignalRuleTest, LastDroppedIngressAcquisitionDeliversGuardLossWithoutLaterData) {
  FakeClock clock;
  std::shared_ptr<IoSignalSubsystem> service(signals(clock, false, 8, 1));
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
  const auto registration = service->ArmRule(request());
  ASSERT_TRUE(registration.ok());
  auto created = IoSignalIngress::Create(service);
  ASSERT_TRUE(created.ok());
  auto ingress = std::move(created.value());
  ASSERT_TRUE(ingress->TryEnqueue(frame(2, 0, false)));
  EXPECT_FALSE(ingress->TryEnqueue(frame(3, 0, true)));
  const auto drained = ingress->Drain(2);
  ASSERT_TRUE(drained.ok());
  EXPECT_EQ(drained.value().applied, 2U);
  const auto evidence = service->ReadRuleEvidence(registration.value()).value();
  EXPECT_FALSE(evidence.trip);
  ASSERT_EQ(evidence.source_losses.size(), 1U);
  ASSERT_TRUE(evidence.source_losses.front().source_loss);
  ASSERT_TRUE(evidence.source_losses.front().source_loss->ingress_barrier);
  EXPECT_TRUE(evidence.source_losses.front().source_loss->ingress_barrier->host_ingress_rejected);
  EXPECT_EQ(evidence.source_losses.front().source_loss->last_accepted_source_sequence, 2U);
}

TEST(IoSignalRuleTest, AtomicRetirementCapturesPreRetirementPublicationAndFencesLaterWrites) {
  FakeClock clock;
  auto service = signals(clock);
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
  const auto armed = service->ArmRule(request());
  ASSERT_TRUE(armed.ok());
  EXPECT_FALSE(service->ReadRuleEvidence(armed.value()).value().trip);
  std::latch published(1), retired(1);
  std::thread source([&] {
    EXPECT_TRUE(service->Publish(frame(2, 0, true)).ok());
    published.count_down();
    retired.wait();
    EXPECT_TRUE(service->Publish(frame(3, 0, false)).ok());
    EXPECT_TRUE(service->Publish(frame(4, 0, true)).ok());
  });
  published.wait();
  const auto final = service->RetireRule(armed.value());
  retired.count_down();
  source.join();
  ASSERT_TRUE(final.ok());
  EXPECT_EQ(final.value().state, IoRuleRegistrationState::kRetired);
  ASSERT_TRUE(final.value().trip);
  EXPECT_EQ(final.value().trip->matched_samples.front().publication.source_sequence, 2U);
  const auto after = service->ReadRuleEvidence(armed.value());
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after.value().state, IoRuleRegistrationState::kRetired);
  EXPECT_FALSE(after.value().trip);
  EXPECT_TRUE(service->RetireRule(armed.value()).ok());
}

TEST(IoSignalRuleTest, RetirementBeforePublicationCannotAcquireRetroactiveTrip) {
  FakeClock clock;
  auto service = signals(clock);
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
  const auto armed = service->ArmRule(request());
  ASSERT_TRUE(armed.ok());
  std::latch retired(1);
  std::thread source([&] {
    retired.wait();
    EXPECT_TRUE(service->Publish(frame(2, 0, true)).ok());
  });
  const auto final = service->RetireRule(armed.value());
  retired.count_down();
  source.join();
  ASSERT_TRUE(final.ok());
  EXPECT_FALSE(final.value().trip);
  EXPECT_EQ(service->ReadRuleEvidence(armed.value()).value().state,
            IoRuleRegistrationState::kRetired);
}

TEST(IoSignalRuleTest,
     NextExpiryUsesInclusiveSampleAgeAndSkipsRetainedFailureAndSyntheticValidity) {
  FakeClock clock;
  auto service = signals(clock);
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
  const auto armed = service->ArmRule(request());
  ASSERT_TRUE(armed.ok());
  EXPECT_EQ(service->NextRuleExpiry().value(), 11);
  clock.set(10);
  EXPECT_EQ(service->NextRuleExpiry().value(), 11);
  clock.set(11);
  EXPECT_EQ(service->NextRuleExpiry().value(), 11);
  ASSERT_TRUE(service->EvaluateRules().ok());
  EXPECT_FALSE(service->NextRuleExpiry().value());
  EXPECT_FALSE(service->RetireRule(armed.value()).value().source_losses.empty());

  auto synthetic = descriptor("a", kA);
  synthetic.update_mode = IoSignalUpdateMode::kChangeOnly;
  synthetic.freshness = {IoFreshnessMode::kProviderValidity, std::nullopt};
  synthetic.liveness = IoSignalLivenessMode::kInProcess;
  auto created = IoSignalSubsystem::Create({RequestOwnerEpoch("host"), 2, {synthetic}, {kA}},
                                           {1, 1, 8, 8, 128, 1024}, clock);
  ASSERT_TRUE(created.ok());
  auto stable = std::move(created.value());
  ASSERT_TRUE(stable->InstallSource(kA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(stable->Publish(frame(1, 11, false)).ok());
  const auto stable_arm = stable->ArmRule(request());
  ASSERT_TRUE(stable_arm.ok());
  clock.advance(1000000000000LL);
  ASSERT_TRUE(stable->EvaluateRules().ok());
  EXPECT_FALSE(stable->NextRuleExpiry().value());
  EXPECT_TRUE(stable->ReadRuleEvidence(stable_arm.value()).value().source_losses.empty());
  ASSERT_TRUE(stable->Publish(frame(2, clock.now(), true)).ok());
  EXPECT_TRUE(stable->RetireRule(stable_arm.value()).value().trip);
}

TEST(IoSignalRuleTest, FinalRetirementDoesNotInventLaterAgeFailureAfterEligibilityEnded) {
  FakeClock clock;
  auto service = signals(clock);
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
  const auto armed = service->ArmRule(request());
  ASSERT_TRUE(armed.ok());
  // The owner has received ordinary completion. Its teardown captures facts
  // already recorded; source-age evaluation belongs to the host's earlier wake.
  clock.set(11);
  const auto final = service->RetireRule(armed.value());
  ASSERT_TRUE(final.ok());
  EXPECT_FALSE(final.value().trip);
  EXPECT_TRUE(final.value().source_losses.empty());
  ASSERT_TRUE(service->EvaluateRules().ok());
  EXPECT_EQ(service->ReadRuleEvidence(armed.value()).value().state,
            IoRuleRegistrationState::kRetired);
}

TEST(IoSignalRuleTest,
     CompoundTripRetainsActualCausingAcquisitionSeparatelyFromSupportingSnapshot) {
  FakeClock clock;
  auto service = signals(clock);
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
  ASSERT_TRUE(service->Publish(frame(1, 0, false, kB, "b")).ok());
  auto compound = request();
  compound.condition = condition(true);
  compound.required_sources = {kA, kB};
  const auto armed = service->ArmRule(compound);
  ASSERT_TRUE(armed.ok());
  ASSERT_TRUE(service->Publish(frame(2, 0, true)).ok());
  EXPECT_FALSE(service->ReadRuleEvidence(armed.value()).value().trip);
  const auto caused = service->Publish(frame(2, 0, true, kB, "b"));
  ASSERT_TRUE(caused.ok());
  const auto evidence = service->ReadRuleEvidence(armed.value());
  ASSERT_TRUE(evidence.ok());
  ASSERT_TRUE(evidence.value().trip);
  const auto& trip = *evidence.value().trip;
  ASSERT_EQ(trip.matched_samples.size(), 2U);
  EXPECT_EQ(trip.matched_samples.front().publication.source_id, kA);
  ASSERT_TRUE(trip.triggering_acquisition);
  EXPECT_EQ(trip.triggering_acquisition->source_id, kB);
  EXPECT_EQ(trip.triggering_acquisition->incarnation, kIncarnation);
  EXPECT_EQ(trip.triggering_acquisition->source_sequence, 2U);
  EXPECT_EQ(trip.triggering_acquisition->source_cursor, caused.value());
  ASSERT_TRUE(service->Publish(frame(3, 0, false)).ok());
  EXPECT_EQ(service->RetireRule(armed.value()).value().trip, evidence.value().trip);

  auto baseline = compound;
  baseline.policy_id = InterruptPolicyId("baseline");
  baseline.activation_mode = MotionInterruptActivationMode::kTripIfAlreadyActive;
  ASSERT_TRUE(service->Publish(frame(4, 0, true)).ok());
  const auto already = service->ArmRule(baseline);
  ASSERT_TRUE(already.ok());
  const auto captured = service->RetireRule(already.value());
  ASSERT_TRUE(captured.ok());
  ASSERT_TRUE(captured.value().trip);
  EXPECT_FALSE(captured.value().trip->triggering_acquisition);
  EXPECT_EQ(captured.value().trip->matched_samples.front().publication.source_sequence, 4U);
}

TEST(IoSignalRuleTest, ImmutableRuleServiceConfigurationNeedsNoSampleOrRegistration) {
  FakeClock clock;
  auto service = signals(clock, false, 3U);
  ASSERT_TRUE(service);
  const auto before = service->RuleServiceConfiguration();
  ASSERT_TRUE(before.ok());
  EXPECT_EQ(before.value().owner_epoch, RequestOwnerEpoch("host"));
  EXPECT_EQ(before.value().catalog_generation, 1U);
  EXPECT_EQ(before.value().maximum_registrations, 3U);
  ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
  const auto armed = service->ArmRule(request());
  ASSERT_TRUE(armed.ok());
  const auto after = service->RuleServiceConfiguration();
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after.value().maximum_registrations, before.value().maximum_registrations);
  EXPECT_EQ(armed.value().catalog_generation, before.value().catalog_generation);
}

TEST(IoSignalRuleTest, CurrentConditionCanReleaseWhileFirstAcquiredTripRemainsRetained) {
  FakeClock clock;
  auto service = signals(clock);
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
  auto arm = request();
  arm.scope = activation_scope();
  const auto armed = service->ArmRule(arm);
  ASSERT_TRUE(armed.ok());
  ASSERT_TRUE(service->Publish(frame(2, 0, true)).ok());
  const auto tripped = service->ReadRuleEvidence(armed.value());
  ASSERT_TRUE(tripped.ok());
  ASSERT_TRUE(tripped.value().trip);
  const auto active = service->ReadRuleConditionSnapshot(armed.value());
  ASSERT_TRUE(active.ok());
  ASSERT_EQ(active.value().value, true);
  clock.set(1);
  ASSERT_TRUE(service->Publish(frame(3, 1, false)).ok());
  const auto released = service->ReadRuleConditionSnapshot(armed.value());
  ASSERT_TRUE(released.ok());
  ASSERT_TRUE(released.value().value.has_value());
  EXPECT_FALSE(*released.value().value);
  EXPECT_EQ(released.value().evaluated_at_ns, 1);
  EXPECT_EQ(service->ReadRuleEvidence(armed.value()).value().trip, tripped.value().trip);
}

TEST(IoSignalRuleTest, CurrentConditionBecomesUnknownOnLossAndCannotRecoverRetainedGuard) {
  FakeClock clock;
  auto service = signals(clock);
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Publish(frame(1, 0, false)).ok());
  const auto armed = service->ArmRule(request());
  ASSERT_TRUE(armed.ok());
  clock.set(11);
  ASSERT_TRUE(service->EvaluateRules().ok());
  const auto unavailable = service->ReadRuleConditionSnapshot(armed.value());
  ASSERT_TRUE(unavailable.ok());
  EXPECT_FALSE(unavailable.value().value);
  const auto loss = service->ReadRuleEvidence(armed.value());
  ASSERT_TRUE(loss.ok());
  ASSERT_EQ(loss.value().source_losses.size(), 1U);
  ASSERT_TRUE(service->Publish(frame(2, 11, false)).ok());
  const auto after_recovery = service->ReadRuleConditionSnapshot(armed.value());
  ASSERT_TRUE(after_recovery.ok());
  EXPECT_FALSE(after_recovery.value().value);
  EXPECT_EQ(service->ReadRuleEvidence(armed.value()).value().source_losses,
            loss.value().source_losses);
  ASSERT_TRUE(service->RetireRule(armed.value()).ok());
  EXPECT_FALSE(service->ReadRuleConditionSnapshot(armed.value()).ok());
}


TEST(IoSignalRuleTest, LargestProductGrammarConditionRetainsEveryAcquiredSourceSample) {
  constexpr std::size_t maximum_nodes = 64U;
  constexpr std::size_t maximum_inputs = maximum_nodes - 1U;
  FakeClock clock;
  std::vector<IoSignalDescriptorDTO> descriptors;
  std::vector<InterruptSourceId> sources;
  IoConditionDefinitionDTO definition;
  std::vector<std::size_t> children;
  for (std::size_t index = 0; index < maximum_inputs; ++index) {
    // Three decimal digits preserve canonical source order throughout.
    const auto suffix = std::to_string(100U + index);
    const InterruptSourceId source("source." + suffix);
    const auto signal = descriptor("input." + suffix, source);
    sources.push_back(source);
    descriptors.push_back(signal);
    definition.inputs.push_back({signal.signal_id, IoConditionValueKind::kBoolean});
    definition.nodes.emplace_back(std::in_place_type<IoConditionCompareDTO>,
        IoConditionComparison::kEqual, IoConditionOperandDTO{IoConditionInputReferenceDTO{index}},
        IoConditionOperandDTO{IoConditionValueDTO{true}});
    children.push_back(index);
  }
  definition.nodes.emplace_back(std::in_place_type<IoConditionAllDTO>, std::move(children));
  auto checked = CheckIoCondition(std::move(definition),
      {maximum_inputs, maximum_nodes, maximum_nodes - 1U, 8U, 128U, 1024U});
  ASSERT_TRUE(checked.ok()) << checked.error().message;
  auto created = IoSignalSubsystem::Create({RequestOwnerEpoch("host"), 1U, descriptors},
      {maximum_inputs, maximum_inputs, 4U, 1U, 128U, 1024U}, clock);
  ASSERT_TRUE(created.ok()) << created.error().message;
  auto service = std::move(created.value());
  for (std::size_t index = 0; index < maximum_inputs; ++index) {
    ASSERT_TRUE(service->InstallSource(sources[index], kIncarnation, std::nullopt).ok());
    ASSERT_TRUE(service->Publish({sources[index], kIncarnation, 1U, clock.now(),
        {{descriptors[index].signal_id, IoSampleValidity::kValid, false, std::nullopt}}}).ok());
  }
  const auto token = service->ArmRule({InterruptPolicyId("largest_product_condition"), activation_scope(),
      std::make_shared<const CheckedIoCondition>(std::move(checked.value())),
      MotionInterruptActivationMode::kRequireInactiveAtArm, sources});
  ASSERT_TRUE(token.ok()) << token.error().message;
  for (std::size_t index = 0; index + 1U < maximum_inputs; ++index) {
    ASSERT_TRUE(service->Publish({sources[index], kIncarnation, 2U, clock.now(),
        {{descriptors[index].signal_id, IoSampleValidity::kValid, true, std::nullopt}}}).ok());
  }
  const auto incomplete = service->ReadRuleEvidence(token.value());
  ASSERT_TRUE(incomplete.ok());
  EXPECT_FALSE(incomplete.value().trip);
  ASSERT_TRUE(service->Publish({sources.back(), kIncarnation, 2U, clock.now(),
      {{descriptors.back().signal_id, IoSampleValidity::kValid, true, std::nullopt}}}).ok());
  const auto evidence = service->ReadRuleEvidence(token.value());
  ASSERT_TRUE(evidence.ok());
  ASSERT_TRUE(evidence.value().trip);
  EXPECT_EQ(evidence.value().trip->registration, token.value());
  ASSERT_EQ(evidence.value().trip->matched_samples.size(), maximum_inputs);
  ASSERT_EQ(evidence.value().trip->source_cursors.size(), maximum_inputs);
  for (std::size_t index = 0; index < maximum_inputs; ++index) {
    const auto& sample = evidence.value().trip->matched_samples[index];
    EXPECT_EQ(sample.publication.signal_id, descriptors[index].signal_id);
    EXPECT_EQ(sample.publication.source_id, sources[index]);
    EXPECT_EQ(sample.publication.source_sequence, 2U);
    EXPECT_EQ(sample.publication.value, true);
    EXPECT_EQ(sample.received_at_ns, clock.now());
  }
  EXPECT_TRUE(evidence.value().source_losses.empty());
  EXPECT_FALSE(evidence.value().evaluation_error);
}

}  // namespace
}  // namespace manyforge::core
