#include <gtest/gtest.h>

#include <manyforge_core/io_condition_evaluator.hpp>
#include <manyforge_core/io_signal_subsystem.hpp>
#include <manyforge_core/time/fake_clock.hpp>

namespace manyforge::core {
namespace {
using Kind = IoConditionValueKind;
const InterruptSourceId kBlackboard("program.bb");

IoProgramBlackboardSourceBindingDTO bb_binding() {
  return {{RequestOwnerEpoch("host"), RequestOwnerEpoch("runner"), 1U, std::nullopt, std::nullopt,
           ProgramBindingId("binding"), ActiveBundlePublicationGeneration(1U)},
          kBlackboard,
          IoSourceIncarnation("bb.1"),
          {{"a", Kind::kBoolean}, {"b", Kind::kBoolean}, {"text", Kind::kString}},
          8192U};
}
std::unique_ptr<IoSignalSubsystem> bb_service(FakeClock& clock, std::size_t maximum_keys = 3U) {
  auto result = IoSignalSubsystem::Create({RequestOwnerEpoch("host"), 1U, {}},
                                          {1U, maximum_keys, 8U, 8U, 128U, 1024U}, clock);
  if (!result.ok()) return {};
  return std::move(result.value());
}
IoProgramBlackboardPublicationDTO bb_baseline(const IoProgramBlackboardSourceBindingDTO& binding,
                                              bool a = false, bool b = false) {
  return {binding.scope,
          IoBlackboardPublicationKind::kBaseline,
          1U,
          0U,
          {{"a", Kind::kBoolean, IoConditionValueDTO{a}},
           {"b", Kind::kBoolean, IoConditionValueDTO{b}},
           {"text", Kind::kString, IoConditionValueDTO{std::string("waiting")}}},
          0};
}
IoProgramBlackboardPublicationDTO bb_update(const IoProgramBlackboardSourceBindingDTO& binding,
                                            std::uint64_t sequence,
                                            std::vector<IoBlackboardValueDTO> values) {
  return {binding.scope,     IoBlackboardPublicationKind::kUpdate,
          sequence,          sequence - 1U,
          std::move(values), 0};
}
std::shared_ptr<const CheckedIoCondition> bb_condition(bool two_keys = false, bool text = false) {
  IoConditionDefinitionDTO definition;
  definition.inputs.push_back(
      {IoConditionBlackboardKeyDTO{text ? "text" : "a"}, text ? Kind::kString : Kind::kBoolean});
  if (two_keys) definition.inputs.push_back({IoConditionBlackboardKeyDTO{"b"}, Kind::kBoolean});
  IoConditionOperandDTO right = IoConditionValueDTO{true};
  if (text) right = IoConditionValueDTO{std::string("done")};
  if (two_keys) right = IoConditionInputReferenceDTO{1U};
  definition.nodes.push_back(IoConditionCompareDTO{
      two_keys ? IoConditionComparison::kNotEqual : IoConditionComparison::kEqual,
      IoConditionInputReferenceDTO{0U}, std::move(right)});
  auto checked = CheckIoCondition(std::move(definition), {2U, 1U, 1U, 1U, 128U, 8192U});
  if (!checked.ok()) return {};
  return std::make_shared<const CheckedIoCondition>(std::move(checked.value()));
}
IoRuleArmRequestDTO bb_rule(bool two_keys = false, bool text = false) {
  return {InterruptPolicyId("rule"),
          IoActivationRuleScopeDTO{RetainedBundleId("bundle"), GraphRevision("graph")},
          bb_condition(two_keys, text),
          MotionInterruptActivationMode::kRequireInactiveAtArm,
          {kBlackboard}};
}

TEST(IoProgramBlackboardTest, ActualBaselineAcknowledgesInstalledIdentityWithoutRunOrAgeExpiry) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto binding = bb_binding();
  ASSERT_TRUE(service->InstallProgramBlackboardSource(binding, std::nullopt).ok());
  EXPECT_FALSE(service->ArmRule(bb_rule()).ok());
  auto baseline = bb_baseline(binding);
  baseline.values[0].value = IoConditionValueDTO{true};
  const auto admitted = service->PublishProgramBlackboard(baseline);
  ASSERT_TRUE(admitted.ok());
  EXPECT_EQ(admitted.value().source_epoch, binding.incarnation);
  EXPECT_EQ(admitted.value().active_publication_generation,
            binding.scope.active_publication_generation);
  EXPECT_EQ(admitted.value().accepted_sequence, 1U);
  EXPECT_FALSE(admitted.value().program_run_id);
  clock.set(1'000'000'000);
  auto request = bb_rule();
  request.activation_mode = MotionInterruptActivationMode::kTripIfAlreadyActive;
  const auto rule = service->ArmRule(request);
  ASSERT_TRUE(rule.ok());
  const auto evidence = service->ReadRuleEvidence(rule.value()).value();
  ASSERT_TRUE(evidence.trip);
  EXPECT_TRUE(evidence.trip->matched_samples.empty());
  EXPECT_FALSE(evidence.trip->triggering_acquisition);
  ASSERT_TRUE(evidence.trip->blackboard);
  EXPECT_EQ(evidence.trip->blackboard->message_sequence, 1U);
  ASSERT_EQ(evidence.trip->blackboard->values.size(), 1U);
  EXPECT_EQ(evidence.trip->blackboard->values[0]->name, "a");
  EXPECT_FALSE(service->NextRuleExpiry().value());
  EXPECT_TRUE(service->IngressConfiguration().value().sources.empty());
}

TEST(IoProgramBlackboardTest, WholeTypedFramePreventsHalfUpdateTripAndRetainsActualCause) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto binding = bb_binding();
  ASSERT_TRUE(service->InstallProgramBlackboardSource(binding, std::nullopt).ok());
  ASSERT_TRUE(service->PublishProgramBlackboard(bb_baseline(binding)).ok());
  const auto rule = service->ArmRule(bb_rule(true));
  ASSERT_TRUE(rule.ok());
  ASSERT_TRUE(
      service
          ->PublishProgramBlackboard(bb_update(binding, 2U,
                                               {{"a", Kind::kBoolean, IoConditionValueDTO{true}},
                                                {"b", Kind::kBoolean, IoConditionValueDTO{true}}}))
          .ok());
  EXPECT_FALSE(service->ReadRuleEvidence(rule.value()).value().trip);
  ASSERT_TRUE(service
                  ->PublishProgramBlackboard(
                      bb_update(binding, 3U, {{"b", Kind::kBoolean, IoConditionValueDTO{false}}}))
                  .ok());
  const auto matched = service->ReadRuleEvidence(rule.value()).value().trip;
  ASSERT_TRUE(matched);
  ASSERT_TRUE(matched->triggering_acquisition);
  EXPECT_EQ(matched->triggering_acquisition->source_sequence, 3U);
  ASSERT_TRUE(matched->blackboard);
  ASSERT_EQ(matched->blackboard->values.size(), 2U);
  const auto matched_value = matched->blackboard->values[0];
  ASSERT_TRUE(service
                  ->PublishProgramBlackboard(
                      bb_update(binding, 4U, {{"a", Kind::kBoolean, IoConditionValueDTO{false}}}))
                  .ok());
  const auto final = service->RetireRule(rule.value()).value();
  EXPECT_EQ(final.trip->blackboard->values[0], matched_value);
  EXPECT_EQ(final.trip->blackboard->message_sequence, 3U);
  EXPECT_EQ(std::get<bool>(*matched_value->value), true);
  const auto records = service->ReadRecords({RequestOwnerEpoch("host"), 1U, kBlackboard, 0U}, 8U);
  ASSERT_TRUE(records.ok());
  ASSERT_EQ(records.value().records.size(), 5U);
  ASSERT_TRUE(records.value().records[3].blackboard_snapshot);
  EXPECT_EQ(&records.value().records[3].blackboard_snapshot->values[0], matched_value.get());
}

TEST(IoProgramBlackboardTest, UnknownKeyIsAdmittedAndDoesNotInvalidateQualifiedSibling) {
  FakeClock clock;
  auto service = bb_service(clock);
  const auto binding = bb_binding();
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->InstallProgramBlackboardSource(binding, std::nullopt).ok());
  auto baseline = bb_baseline(binding);
  baseline.values[1].value.reset();
  ASSERT_TRUE(service->PublishProgramBlackboard(baseline).ok());
  EXPECT_TRUE(service->ArmRule(bb_rule()).ok());
  EXPECT_FALSE(service->ArmRule(bb_rule(true)).ok());
  const auto text_rule = service->ArmRule(bb_rule(false, true));
  ASSERT_TRUE(text_rule.ok());
  ASSERT_TRUE(service
                  ->PublishProgramBlackboard(
                      bb_update(binding, 2U, {{"text", Kind::kString, std::nullopt}}))
                  .ok());
  const auto loss = service->ReadRuleEvidence(text_rule.value()).value();
  ASSERT_EQ(loss.source_losses.size(), 1U);
  ASSERT_TRUE(loss.source_losses.front().blackboard);
  EXPECT_EQ(loss.source_losses.front().blackboard->unavailable_key, "text");
  EXPECT_EQ(loss.source_losses.front().blackboard->last_accepted_message_sequence, 2U);
  ASSERT_TRUE(
      service
          ->PublishProgramBlackboard(bb_update(
              binding, 3U, {{"text", Kind::kString, IoConditionValueDTO{std::string("done")}}}))
          .ok());
  const auto recovered = service->ReadRuleEvidence(text_rule.value()).value();
  EXPECT_FALSE(recovered.trip);
  EXPECT_EQ(recovered.source_losses, loss.source_losses);
  EXPECT_TRUE(service->ArmRule(bb_rule()).ok());
}

TEST(IoProgramBlackboardTest, InvalidTypedFrameRefusesAtomicallyAndSameSequenceCanBeCorrected) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto binding = bb_binding();
  ASSERT_TRUE(service->InstallProgramBlackboardSource(binding, std::nullopt).ok());
  ASSERT_TRUE(service->PublishProgramBlackboard(bb_baseline(binding)).ok());
  const auto rule = service->ArmRule(bb_rule());
  ASSERT_TRUE(rule.ok());
  auto update = bb_update(binding, 2U,
                          {{"a", Kind::kBoolean, IoConditionValueDTO{true}},
                           {"b", Kind::kBoolean, IoConditionValueDTO{std::int64_t(9)}}});
  EXPECT_FALSE(service->PublishProgramBlackboard(update).ok());
  const auto refused = service->ReadRuleEvidence(rule.value()).value();
  EXPECT_FALSE(refused.trip);
  EXPECT_TRUE(refused.source_losses.empty());
  update.values[1].value = IoConditionValueDTO{false};
  ASSERT_TRUE(service->PublishProgramBlackboard(update).ok());
  EXPECT_TRUE(service->ReadRuleEvidence(rule.value()).value().trip);
}

TEST(IoProgramBlackboardTest, ExactSenderGapRetainsLossAndDoesNotResumeFromPartialUpdates) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto binding = bb_binding();
  ASSERT_TRUE(service->InstallProgramBlackboardSource(binding, std::nullopt).ok());
  ASSERT_TRUE(service->PublishProgramBlackboard(bb_baseline(binding)).ok());
  const auto rule = service->ArmRule(bb_rule());
  ASSERT_TRUE(rule.ok());
  const auto gap = service->PublishProgramBlackboard(
      bb_update(binding, 3U, {{"a", Kind::kBoolean, IoConditionValueDTO{true}}}));
  ASSERT_FALSE(gap.ok());
  EXPECT_EQ(gap.error().details.at("reason"), "program_bb_sequence_gap");
  const auto evidence = service->ReadRuleEvidence(rule.value()).value();
  ASSERT_EQ(evidence.source_losses.size(), 1U);
  ASSERT_TRUE(evidence.source_losses[0].source_loss);
  EXPECT_EQ(evidence.source_losses[0].source_loss->reason, IoSourceLossReason::kSequenceGap);
  ASSERT_TRUE(evidence.source_losses[0].blackboard);
  EXPECT_EQ(evidence.source_losses[0].blackboard->last_accepted_message_sequence, 1U);
  EXPECT_FALSE(service
                   ->PublishProgramBlackboard(
                       bb_update(binding, 2U, {{"a", Kind::kBoolean, IoConditionValueDTO{true}}}))
                   .ok());
  EXPECT_FALSE(service->ArmRule(bb_rule()).ok());
  ASSERT_TRUE(service->InstallProgramBlackboardSource(binding, binding.incarnation).ok());
  EXPECT_FALSE(service->ArmRule(bb_rule()).ok());  // inert reinstall cannot bless lost continuity
}

TEST(IoProgramBlackboardTest, ReplacementFencesObsoleteFramesAndOldRulesBeforeNewBaseline) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto old = bb_binding();
  ASSERT_TRUE(service->InstallProgramBlackboardSource(old, std::nullopt).ok());
  ASSERT_TRUE(service->PublishProgramBlackboard(bb_baseline(old)).ok());
  const auto rule = service->ArmRule(bb_rule());
  ASSERT_TRUE(rule.ok());
  auto replacement = old;
  replacement.incarnation = IoSourceIncarnation("bb.2");
  replacement.scope.active_publication_generation = ActiveBundlePublicationGeneration(2U);
  EXPECT_FALSE(service->InstallProgramBlackboardSource(replacement, std::nullopt).ok());
  ASSERT_TRUE(service->InstallProgramBlackboardSource(replacement, old.incarnation).ok());
  auto obsolete = bb_update(old, 99U, {{"a", Kind::kBoolean, IoConditionValueDTO{true}}});
  EXPECT_FALSE(service->PublishProgramBlackboard(obsolete).ok());
  auto baseline = bb_baseline(replacement, true);
  ASSERT_TRUE(service->PublishProgramBlackboard(baseline).ok());
  const auto evidence = service->ReadRuleEvidence(rule.value()).value();
  EXPECT_FALSE(evidence.trip);
  ASSERT_EQ(evidence.source_losses.size(), 1U);
  EXPECT_EQ(evidence.source_losses[0].incarnation, old.incarnation);
  EXPECT_EQ(evidence.source_losses[0].source_loss->reason, IoSourceLossReason::kIncarnationChanged);
  auto current = bb_rule();
  current.activation_mode = MotionInterruptActivationMode::kTripIfAlreadyActive;
  const auto current_rule = service->ArmRule(current);
  ASSERT_TRUE(current_rule.ok());
  EXPECT_TRUE(service->ReadRuleEvidence(current_rule.value()).value().source_losses.empty());
  EXPECT_TRUE(service->ReadRuleEvidence(current_rule.value()).value().trip);
}

TEST(IoProgramBlackboardTest, RetireKeepsTruthfulLossAndCannotReviveRetiredEpoch) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto old = bb_binding();
  ASSERT_TRUE(service->InstallProgramBlackboardSource(old, std::nullopt).ok());
  ASSERT_TRUE(service->PublishProgramBlackboard(bb_baseline(old)).ok());
  const auto rule = service->ArmRule(bb_rule());
  ASSERT_TRUE(rule.ok());
  ASSERT_TRUE(service->RetireProgramBlackboardSource(old).ok());
  EXPECT_FALSE(service->InstallProgramBlackboardSource(old, old.incarnation).ok());
  EXPECT_FALSE(service->PublishProgramBlackboardBaseline(old, old.incarnation,
                                                        bb_baseline(old)).ok());
  EXPECT_FALSE(service->PublishProgramBlackboard(bb_baseline(old)).ok());
  const auto evidence = service->RetireRule(rule.value()).value();
  ASSERT_EQ(evidence.source_losses.size(), 1U);
  EXPECT_EQ(evidence.source_losses[0].source_loss->reason, IoSourceLossReason::kDisconnected);
  EXPECT_FALSE(evidence.trip);
  auto replacement = old;
  replacement.incarnation = IoSourceIncarnation("bb.reset");
  replacement.scope.cycle_generation = 2U;
  ASSERT_TRUE(service->InstallProgramBlackboardSource(replacement, old.incarnation).ok());
  auto unknown = bb_baseline(replacement);
  unknown.values[0].value.reset();
  ASSERT_TRUE(service->PublishProgramBlackboard(unknown).ok());
  EXPECT_FALSE(service->ArmRule(bb_rule()).ok());
}

TEST(IoProgramBlackboardTest, TrustedReceiverStampRefusesFutureAndRegressedFramesWithoutMutation) {
  FakeClock clock;
  clock.set(10);
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto binding = bb_binding();
  ASSERT_TRUE(service->InstallProgramBlackboardSource(binding, std::nullopt).ok());
  auto baseline = bb_baseline(binding);
  baseline.trusted_received_at_ns = 10;
  ASSERT_TRUE(service->PublishProgramBlackboard(baseline).ok());
  auto update = bb_update(binding, 2U, {{"a", Kind::kBoolean, IoConditionValueDTO{true}}});
  update.trusted_received_at_ns = 11;
  EXPECT_FALSE(service->PublishProgramBlackboard(update).ok());
  update.trusted_received_at_ns = 9;
  EXPECT_FALSE(service->PublishProgramBlackboard(update).ok());
  update.trusted_received_at_ns = 10;
  ASSERT_TRUE(service->PublishProgramBlackboard(update).ok());
}

TEST(IoProgramBlackboardTest, SignalIngressCannotPublishOrInstallBlackboardSource) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto binding = bb_binding();
  ASSERT_TRUE(service->InstallProgramBlackboardSource(binding, std::nullopt).ok());
  EXPECT_FALSE(
      service->InstallSource(kBlackboard, IoSourceIncarnation("foreign"), binding.incarnation)
          .ok());
  IoSourceAcquisitionDTO acquisition{
      kBlackboard,
      binding.incarnation,
      1U,
      0,
      {{{ControllableComponentId("io"), "a"}, IoSampleValidity::kValid, true, std::nullopt}}};
  EXPECT_FALSE(service->ValidateAcquisition(acquisition).ok());
  EXPECT_FALSE(service->Publish(acquisition).ok());
  EXPECT_TRUE(service->PublishProgramBlackboard(bb_baseline(binding)).ok());
}

TEST(IoProgramBlackboardTest, LossBeforeFirstBaselineCannotBeRequalifiedByDelayedSender) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto binding = bb_binding();
  ASSERT_TRUE(service->InstallProgramBlackboardSource(binding, std::nullopt).ok());
  ASSERT_TRUE(service
                  ->ReportIngressOverflow(binding.source_id, binding.incarnation,
                                          {"io_loss", "Lost sender frame", {}})
                  .ok());
  EXPECT_FALSE(service->PublishProgramBlackboard(bb_baseline(binding)).ok());
  auto replacement = binding;
  replacement.incarnation = IoSourceIncarnation("bb.new");
  replacement.scope.cycle_generation = 2U;
  ASSERT_TRUE(service->InstallProgramBlackboardSource(replacement, binding.incarnation).ok());
  EXPECT_TRUE(service->PublishProgramBlackboard(bb_baseline(replacement)).ok());
}

TEST(IoProgramBlackboardTest, AllReferencedKeysQualifyIndependentlyOfBooleanShortCircuit) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto binding = bb_binding();
  ASSERT_TRUE(service->InstallProgramBlackboardSource(binding, std::nullopt).ok());
  ASSERT_TRUE(service->PublishProgramBlackboard(bb_baseline(binding)).ok());
  IoConditionDefinitionDTO definition;
  definition.inputs = {{IoConditionBlackboardKeyDTO{"a"}, Kind::kBoolean},
                       {IoConditionBlackboardKeyDTO{"b"}, Kind::kBoolean}};
  definition.nodes.push_back(IoConditionCompareDTO{
      IoConditionComparison::kEqual, IoConditionInputReferenceDTO{0U}, IoConditionValueDTO{true}});
  definition.nodes.push_back(IoConditionCompareDTO{
      IoConditionComparison::kEqual, IoConditionInputReferenceDTO{1U}, IoConditionValueDTO{true}});
  definition.nodes.push_back(IoConditionAnyDTO{{0U, 1U}});
  auto checked = CheckIoCondition(std::move(definition), {2U, 3U, 2U, 2U, 128U, 128U});
  ASSERT_TRUE(checked.ok());
  auto request = bb_rule();
  request.condition = std::make_shared<const CheckedIoCondition>(std::move(checked.value()));
  const auto rule = service->ArmRule(request);
  ASSERT_TRUE(rule.ok());
  ASSERT_TRUE(
      service
          ->PublishProgramBlackboard(bb_update(binding, 2U,
                                               {{"a", Kind::kBoolean, IoConditionValueDTO{true}},
                                                {"b", Kind::kBoolean, std::nullopt}}))
          .ok());
  const auto evidence = service->ReadRuleEvidence(rule.value()).value();
  EXPECT_FALSE(evidence.trip);
  ASSERT_EQ(evidence.source_losses.size(), 1U);
  ASSERT_TRUE(evidence.source_losses[0].blackboard);
  EXPECT_EQ(evidence.source_losses[0].blackboard->unavailable_key, "b");
}

TEST(IoProgramBlackboardTest, MixedTypedKeysUseExactComparisonAndProjectOnlyCheckedInputs) {
  FakeClock clock;
  auto service = bb_service(clock, 5U);
  ASSERT_TRUE(service);
  auto binding = bb_binding();
  binding.keys.push_back({"count", Kind::kInt64});
  binding.keys.push_back({"ratio", Kind::kFloat64});
  ASSERT_TRUE(service->InstallProgramBlackboardSource(binding, std::nullopt).ok());
  auto baseline = bb_baseline(binding);
  baseline.values.push_back(
      {"count", Kind::kInt64, IoConditionValueDTO{std::int64_t(9007199254740993LL)}});
  baseline.values.push_back({"ratio", Kind::kFloat64, IoConditionValueDTO{9007199254740992.0}});
  ASSERT_TRUE(service->PublishProgramBlackboard(baseline).ok());
  IoConditionDefinitionDTO definition;
  definition.inputs = {{IoConditionBlackboardKeyDTO{"count"}, Kind::kInt64},
                       {IoConditionBlackboardKeyDTO{"ratio"}, Kind::kFloat64},
                       {IoConditionBlackboardKeyDTO{"text"}, Kind::kString}};
  definition.nodes.push_back(IoConditionCompareDTO{IoConditionComparison::kGreater,
                                                   IoConditionInputReferenceDTO{0U},
                                                   IoConditionInputReferenceDTO{1U}});
  definition.nodes.push_back(IoConditionCompareDTO{IoConditionComparison::kEqual,
                                                   IoConditionInputReferenceDTO{2U},
                                                   IoConditionValueDTO{std::string("done")}});
  definition.nodes.push_back(IoConditionAllDTO{{0U, 1U}});
  auto checked = CheckIoCondition(std::move(definition), {3U, 3U, 2U, 2U, 128U, 8192U});
  ASSERT_TRUE(checked.ok());
  auto request = bb_rule();
  request.condition = std::make_shared<const CheckedIoCondition>(std::move(checked.value()));
  const auto rule = service->ArmRule(request);
  ASSERT_TRUE(rule.ok());
  ASSERT_TRUE(
      service
          ->PublishProgramBlackboard(bb_update(
              binding, 2U, {{"text", Kind::kString, IoConditionValueDTO{std::string("done")}}}))
          .ok());
  const auto evidence = service->ReadRuleEvidence(rule.value()).value();
  ASSERT_TRUE(evidence.trip);
  ASSERT_TRUE(evidence.trip->blackboard);
  ASSERT_EQ(evidence.trip->blackboard->values.size(), 3U);
  EXPECT_EQ(evidence.trip->blackboard->values[0]->name, "count");
  EXPECT_EQ(evidence.trip->blackboard->values[1]->name, "ratio");
  EXPECT_EQ(evidence.trip->blackboard->values[2]->name, "text");
  const auto records = service->ReadRecords({RequestOwnerEpoch("host"), 1U, kBlackboard, 0U}, 8U);
  ASSERT_TRUE(records.ok());
  ASSERT_TRUE(records.value().records.back().blackboard_snapshot);
  EXPECT_EQ(records.value().records.back().blackboard_snapshot->values.size(), 5U);
  EXPECT_EQ(evidence.trip->triggering_acquisition->source_sequence, 2U);
}

TEST(IoProgramBlackboardTest,
     SupersededProofRequiresExactRetirementAndAcceptedReplacementBaseline) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto old = bb_binding();
  ASSERT_TRUE(service->InstallProgramBlackboardSource(old, std::nullopt).ok());
  ASSERT_TRUE(service->PublishProgramBlackboard(bb_baseline(old)).ok());
  EXPECT_FALSE(service->WasProgramBlackboardScopeRetired(old.scope).value());
  ASSERT_TRUE(service->RetireProgramBlackboardSource(old).ok());
  EXPECT_FALSE(service->WasProgramBlackboardScopeRetired(old.scope).value());
  auto replacement = old;
  replacement.incarnation = IoSourceIncarnation("bb.2");
  replacement.scope.cycle_generation = 2U;
  ASSERT_TRUE(service->InstallProgramBlackboardSource(replacement, old.incarnation).ok());
  EXPECT_FALSE(service->WasProgramBlackboardScopeRetired(old.scope).value());
  ASSERT_TRUE(service->PublishProgramBlackboard(bb_baseline(replacement)).ok());
  EXPECT_TRUE(service->WasProgramBlackboardScopeRetired(old.scope).value());
  auto invented = old.scope;
  invented.program_binding_id = ProgramBindingId("never.installed");
  EXPECT_FALSE(service->WasProgramBlackboardScopeRetired(invented).value());
  invented = old.scope;
  invented.program_runner_owner_epoch = RequestOwnerEpoch("unknown.runner");
  EXPECT_FALSE(service->WasProgramBlackboardScopeRetired(invented).value());
  EXPECT_FALSE(service->WasProgramBlackboardScopeRetired(replacement.scope).value());
}

TEST(IoProgramBlackboardTest, EvictedRetirementCannotAuthorizeSupersededReply) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto old = bb_binding();
  ASSERT_TRUE(service->InstallProgramBlackboardSource(old, std::nullopt).ok());
  auto replacement = old;
  replacement.incarnation = IoSourceIncarnation("bb.next");
  replacement.scope.active_publication_generation = ActiveBundlePublicationGeneration(2U);
  ASSERT_TRUE(service->InstallProgramBlackboardSource(replacement, old.incarnation).ok());
  ASSERT_TRUE(service->PublishProgramBlackboard(bb_baseline(replacement)).ok());
  EXPECT_TRUE(service->WasProgramBlackboardScopeRetired(old.scope).value());
  for (std::uint64_t sequence = 2U; sequence <= 61U; ++sequence)
    ASSERT_TRUE(service
                    ->PublishProgramBlackboard(bb_update(
                        replacement, sequence, {{"a", Kind::kBoolean, IoConditionValueDTO{false}}}))
                    .ok());
  EXPECT_TRUE(service->WasProgramBlackboardScopeRetired(old.scope).value());
  for (std::uint64_t sequence = 62U; sequence <= 63U; ++sequence)
    ASSERT_TRUE(service
                    ->PublishProgramBlackboard(bb_update(
                        replacement, sequence, {{"a", Kind::kBoolean, IoConditionValueDTO{false}}}))
                    .ok());
  EXPECT_FALSE(service->WasProgramBlackboardScopeRetired(old.scope).value());
  const auto records = service->ReadRecords({RequestOwnerEpoch("host"), 1U, kBlackboard, 0U}, 64U);
  ASSERT_TRUE(records.ok());
  EXPECT_TRUE(records.value().retention_gap);
  EXPECT_EQ(records.value().records.size(), 64U);
}

TEST(IoProgramBlackboardTest, CompactEvidenceAliasesOnlyUsedKeysAndOwnsNoFabricatedFullSnapshot) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto binding = bb_binding();
  ASSERT_TRUE(service->InstallProgramBlackboardSource(binding, std::nullopt).ok());
  ASSERT_TRUE(service->PublishProgramBlackboard(bb_baseline(binding)).ok());
  const auto rule = service->ArmRule(bb_rule());
  ASSERT_TRUE(rule.ok());
  ASSERT_TRUE(service
                  ->PublishProgramBlackboard(
                      bb_update(binding, 2U, {{"a", Kind::kBoolean, IoConditionValueDTO{true}}}))
                  .ok());
  const auto acquired = service->ReadRuleEvidence(rule.value()).value();
  ASSERT_TRUE(acquired.trip);
  ASSERT_TRUE(acquired.trip->blackboard);
  ASSERT_EQ(acquired.trip->blackboard->values.size(), 1U);
  const auto records = service->ReadRecords({RequestOwnerEpoch("host"), 1U, kBlackboard, 0U}, 8U);
  ASSERT_TRUE(records.ok());
  const auto& full = records.value().records.back().blackboard_snapshot;
  ASSERT_TRUE(full);
  EXPECT_EQ(acquired.trip->blackboard->values[0].get(), &full->values[0]);
  auto decoded = *acquired.trip;
  decoded.blackboard->values[0] = std::make_shared<const IoBlackboardValueDTO>(full->values[0]);
  EXPECT_EQ(decoded, *acquired.trip);
  decoded.blackboard->message_sequence = 1U;
  EXPECT_NE(decoded, *acquired.trip);
}


TEST(IoProgramBlackboardTest, QualifiedResetAndRunBaselinesKeepGlobalTokenAndEvaluateActualValues) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto initial = bb_binding();
  ASSERT_TRUE(service->PublishProgramBlackboardBaseline(initial, {}, bb_baseline(initial)).ok());
  const auto guard = service->ArmRule(bb_rule());
  ASSERT_TRUE(guard.ok());
  auto reset = initial;
  reset.scope.cycle_generation = 2U;
  reset.incarnation = IoSourceIncarnation("bb.reset.2");
  auto invalid = bb_baseline(reset);
  invalid.values.front().value = IoConditionValueDTO{std::string("wrong")};
  EXPECT_FALSE(service->PublishProgramBlackboardBaseline(reset, initial.incarnation, invalid).ok());
  EXPECT_FALSE(service->WasProgramBlackboardScopeRetired(initial.scope).value());
  EXPECT_EQ(service->ReadRuleConditionSnapshot(guard.value()).value().value, std::optional(false));
  const auto accepted = service->PublishProgramBlackboardBaseline(
      reset, initial.incarnation, bb_baseline(reset));
  ASSERT_TRUE(accepted.ok());
  EXPECT_EQ(accepted.value().source_epoch, reset.incarnation);
  EXPECT_TRUE(service->WasProgramBlackboardScopeRetired(initial.scope).value());
  const auto retained = service->ReadRuleEvidence(guard.value()).value();
  EXPECT_EQ(retained.state, IoRuleRegistrationState::kArmed);
  EXPECT_FALSE(retained.trip);
  EXPECT_TRUE(retained.source_losses.empty());
  EXPECT_EQ(service->ReadRuleConditionSnapshot(guard.value()).value().value, std::optional(false));
  EXPECT_FALSE(service->PublishProgramBlackboard(bb_update(initial, 2U,
      {{"a", Kind::kBoolean, IoConditionValueDTO{true}}})).ok());
  EXPECT_FALSE(service->ReadRuleEvidence(guard.value()).value().trip);
  auto run = reset;
  run.scope.program_run_id = ProgramRunId("run.new");
  run.scope.claim_generation = CellAdmissionClaimGeneration(7U);
  run.incarnation = IoSourceIncarnation("bb.run.new");
  ASSERT_TRUE(service->PublishProgramBlackboardBaseline(run, reset.incarnation,
                                                        bb_baseline(run, true)).ok());
  const auto matched = service->ReadRuleEvidence(guard.value()).value();
  EXPECT_TRUE(matched.source_losses.empty());
  ASSERT_TRUE(matched.trip);
  EXPECT_EQ(matched.trip->registration, guard.value());
  ASSERT_TRUE(matched.trip->blackboard);
  EXPECT_EQ(matched.trip->blackboard->scope, run.scope);
  EXPECT_EQ(matched.trip->blackboard->incarnation, run.incarnation);
  ASSERT_TRUE(matched.trip->triggering_acquisition);
  EXPECT_EQ(matched.trip->triggering_acquisition->incarnation, run.incarnation);
  const auto records = service->ReadRecords({RequestOwnerEpoch("host"), 1U, kBlackboard, 0U}, 8U);
  ASSERT_TRUE(records.ok());
  ASSERT_EQ(records.value().records.size(), 3U);
  for (const auto& record : records.value().records) EXPECT_FALSE(record.loss);
  ASSERT_TRUE(records.value().records.back().retired_blackboard_scope);
  EXPECT_EQ(*records.value().records.back().retired_blackboard_scope, reset.scope);
}

TEST(IoProgramBlackboardTest, FreshBaselineRestoresSourceWithoutErasingEarlierGapOrOverflow) {
  for (const bool overflow : {false, true}) {
    SCOPED_TRACE(overflow);
    FakeClock clock;
    auto service = bb_service(clock);
    ASSERT_TRUE(service);
    const auto initial = bb_binding();
    ASSERT_TRUE(service->PublishProgramBlackboardBaseline(initial, {}, bb_baseline(initial)).ok());
    const auto guard = service->ArmRule(bb_rule());
    ASSERT_TRUE(guard.ok());
    if (overflow) {
      ASSERT_TRUE(service->ReportIngressOverflow(initial.source_id, initial.incarnation,
          {"ingress_overflow", "actual source overflow", {}}).ok());
    } else {
      EXPECT_FALSE(service->PublishProgramBlackboard(bb_update(initial, 3U,
          {{"a", Kind::kBoolean, IoConditionValueDTO{true}}})).ok());
    }
    const auto before = service->ReadRuleEvidence(guard.value()).value();
    ASSERT_FALSE(before.source_losses.empty());
    auto replacement = initial;
    replacement.scope.cycle_generation = 2U;
    replacement.incarnation = IoSourceIncarnation("bb.reset.2");
    const auto accepted = service->PublishProgramBlackboardBaseline(
        replacement, initial.incarnation, bb_baseline(replacement));
    ASSERT_TRUE(accepted.ok());
    EXPECT_EQ(accepted.value().source_epoch, replacement.incarnation);
    EXPECT_EQ(service->ReadRuleEvidence(guard.value()).value().source_losses, before.source_losses);
    EXPECT_FALSE(service->ReadRuleEvidence(guard.value()).value().trip);
    EXPECT_FALSE(service->ReadRuleConditionSnapshot(guard.value()).value().value);
    EXPECT_TRUE(service->WasProgramBlackboardScopeRetired(initial.scope).value());
    const auto fresh = service->ArmRule(bb_rule());
    ASSERT_TRUE(fresh.ok());
    EXPECT_NE(fresh.value().registration_id, guard.value().registration_id);
    EXPECT_EQ(service->ReadRuleConditionSnapshot(fresh.value()).value().value, std::optional(false));
    EXPECT_TRUE(service->ReadRuleEvidence(fresh.value()).value().source_losses.empty());
    const auto records = service->ReadRecords({RequestOwnerEpoch("host"), 1U, kBlackboard, 0U}, 8U);
    ASSERT_TRUE(records.ok());
    ASSERT_EQ(records.value().records.size(), 3U);
    EXPECT_TRUE(records.value().records[1].loss);
    EXPECT_FALSE(records.value().records.back().loss);
    ASSERT_TRUE(service->PublishProgramBlackboard(bb_update(replacement, 2U,
        {{"a", Kind::kBoolean, IoConditionValueDTO{true}}})).ok());
    const auto old_after_true = service->ReadRuleEvidence(guard.value()).value();
    EXPECT_EQ(old_after_true.source_losses, before.source_losses);
    EXPECT_FALSE(old_after_true.trip);
    EXPECT_FALSE(service->ReadRuleConditionSnapshot(guard.value()).value().value);
    const auto fresh_after_true = service->ReadRuleEvidence(fresh.value()).value();
    ASSERT_TRUE(fresh_after_true.trip);
    ASSERT_TRUE(fresh_after_true.trip->blackboard);
    EXPECT_EQ(fresh_after_true.trip->blackboard->scope, replacement.scope);
    EXPECT_EQ(fresh_after_true.trip->blackboard->incarnation, replacement.incarnation);
  }
}

TEST(IoProgramBlackboardTest, BaselineHandoffRejectsDifferentAuthorityAndPreservesPriorTrip) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto initial = bb_binding();
  ASSERT_TRUE(service->PublishProgramBlackboardBaseline(initial, {}, bb_baseline(initial)).ok());
  const auto guard = service->ArmRule(bb_rule());
  ASSERT_TRUE(guard.ok());
  ASSERT_TRUE(service->PublishProgramBlackboard(bb_update(initial, 2U,
      {{"a", Kind::kBoolean, IoConditionValueDTO{true}}})).ok());
  const auto trip = service->ReadRuleEvidence(guard.value()).value().trip;
  ASSERT_TRUE(trip);
  auto reset = initial;
  reset.scope.cycle_generation = 2U;
  reset.incarnation = IoSourceIncarnation("bb.reset.2");
  for (int wrong = 0; wrong < 4; ++wrong) {
    auto foreign = reset;
    if (wrong == 0) foreign.scope.program_runner_owner_epoch = RequestOwnerEpoch("other.runner");
    if (wrong == 1) foreign.scope.program_binding_id = ProgramBindingId("other.binding");
    if (wrong == 2) foreign.scope.active_publication_generation = ActiveBundlePublicationGeneration(2U);
    const auto expected = wrong == 3 ? std::optional(IoSourceIncarnation("foreign.epoch"))
                                     : std::optional(initial.incarnation);
    EXPECT_FALSE(service->PublishProgramBlackboardBaseline(foreign, expected,
                                                          bb_baseline(foreign)).ok());
    EXPECT_EQ(service->ReadRuleEvidence(guard.value()).value().trip, trip);
  }
  ASSERT_TRUE(service->PublishProgramBlackboardBaseline(reset, initial.incarnation,
                                                        bb_baseline(reset)).ok());
  const auto after = service->ReadRuleEvidence(guard.value()).value();
  EXPECT_EQ(after.trip, trip);
  EXPECT_TRUE(after.source_losses.empty());
  EXPECT_EQ(service->ReadRuleConditionSnapshot(guard.value()).value().value, std::optional(false));
}


TEST(IoProgramBlackboardTest, BaselineAfterActualTeardownKeepsOldGuardUnqualified) {
  FakeClock clock;
  auto service = bb_service(clock);
  ASSERT_TRUE(service);
  const auto initial = bb_binding();
  ASSERT_TRUE(service->PublishProgramBlackboardBaseline(initial, {}, bb_baseline(initial)).ok());
  const auto guard = service->ArmRule(bb_rule());
  ASSERT_TRUE(guard.ok());
  ASSERT_TRUE(service->RetireProgramBlackboardSource(initial).ok());
  const auto lost = service->ReadRuleEvidence(guard.value()).value();
  ASSERT_FALSE(lost.source_losses.empty());
  auto replacement = initial;
  replacement.scope.cycle_generation = 2U;
  replacement.incarnation = IoSourceIncarnation("bb.after.actual.teardown");
  ASSERT_TRUE(service->PublishProgramBlackboardBaseline(replacement, initial.incarnation,
                                                        bb_baseline(replacement)).ok());
  EXPECT_EQ(service->ReadRuleEvidence(guard.value()).value().source_losses, lost.source_losses);
  EXPECT_FALSE(service->ReadRuleEvidence(guard.value()).value().trip);
  EXPECT_FALSE(service->ReadRuleConditionSnapshot(guard.value()).value().value);
  const auto fresh = service->ArmRule(bb_rule());
  ASSERT_TRUE(fresh.ok());
  EXPECT_TRUE(service->ReadRuleEvidence(fresh.value()).value().source_losses.empty());
  EXPECT_EQ(service->ReadRuleConditionSnapshot(fresh.value()).value().value, std::optional(false));
  ASSERT_TRUE(service->PublishProgramBlackboard(bb_update(replacement, 2U,
      {{"a", Kind::kBoolean, IoConditionValueDTO{true}}})).ok());
  EXPECT_EQ(service->ReadRuleEvidence(guard.value()).value().source_losses, lost.source_losses);
  EXPECT_FALSE(service->ReadRuleEvidence(guard.value()).value().trip);
  EXPECT_FALSE(service->ReadRuleConditionSnapshot(guard.value()).value().value);
  const auto fresh_after_true = service->ReadRuleEvidence(fresh.value()).value();
  ASSERT_TRUE(fresh_after_true.trip);
  ASSERT_TRUE(fresh_after_true.trip->blackboard);
  EXPECT_EQ(fresh_after_true.trip->blackboard->scope, replacement.scope);
  EXPECT_EQ(fresh_after_true.trip->blackboard->incarnation, replacement.incarnation);
}

}  // namespace
}  // namespace manyforge::core
