#include "execution_host_responses.hpp"
#include <gtest/gtest.h>
#include <manyforge_core/io_condition_evaluator.hpp>

namespace manyforge::roboplan_backend::host_detail {
namespace {
// Typed projection inputs only; this test does not install a Kernel catalog,
// grant source qualification, publish state, or substitute a Composition.
std::shared_ptr<core::IoActivationRuleCatalogDTO> Catalog() {
  core::IoConditionDefinitionDTO definition;
  definition.inputs.push_back({core::IoConditionBlackboardKeyDTO{"permit"},
                               core::IoConditionValueKind::kBoolean});
  definition.nodes.push_back(core::IoConditionCompareDTO{
      core::IoConditionComparison::kEqual,
      core::IoConditionInputReferenceDTO{0U}, core::IoConditionValueDTO{true}});
  const auto checked = core::CheckIoCondition(std::move(definition),
                                              {8U, 8U, 8U, 8U, 128U, 1024U});
  if (!checked.ok())
    throw std::runtime_error("projection condition fixture refused");
  auto catalog = std::make_shared<core::IoActivationRuleCatalogDTO>(
      core::IoActivationRuleCatalogDTO{
          {core::RequestOwnerEpoch("kernel.owner"), UINT64_MAX,
           core::ActiveBundlePublicationGeneration(3U),
           core::RetainedBundleId("bundle.actual"),
           core::GraphRevision("graph.actual")},
          {},
          std::nullopt,
          {}});
  catalog->rules.push_back(
      {core::InterruptPolicyId("global.door"),
       std::make_shared<const core::CheckedIoCondition>(checked.value()),
       {core::InterruptSourceId("program.blackboard")},
       core::IoActivationAllMotionScopeDTO{},
       std::nullopt,
       1,
       std::nullopt});
  catalog->referenced_program_blackboard_keys = {"permit"};
  return catalog;
}
std::shared_ptr<core::IoRuleWaitingForProgramCatalogDTO> WaitingCatalog() {
  return std::make_shared<core::IoRuleWaitingForProgramCatalogDTO>(
      core::IoRuleWaitingForProgramCatalogDTO{
          {core::RequestOwnerEpoch("kernel.owner"),
           core::ActiveBundlePublicationGeneration(3U),
           core::RetainedBundleId("bundle.actual"),
           core::GraphRevision("graph.actual")},
          {{core::InterruptPolicyId("global.waiting"), nullptr, {},
            core::IoActivationAllMotionScopeDTO{}, std::nullopt, 1,
            std::nullopt,
            core::InterruptPostLatchDisposition::kAwaitOperatorAck,
            {"permit", "ready"}}}});
}

const CanonicalValue &Field(const CanonicalValue::Object &object,
                            std::string_view name) {
  for (const auto &[key, value] : object)
    if (key == name)
      return value;
  throw std::runtime_error("projection field missing");
}
TEST(GlobalRuleEventResponseTest,
     PendingKeepsActualMetadataWithoutInventingClearBaseline) {
  const auto catalog = Catalog();
  const auto fields = InterruptRuleEventMetadataFields(
      core::IoRuleStateProjectionDTO{
          core::IoRuleStateProjectionStatusDTO::kPending, std::nullopt,
          catalog},
      catalog->context.owner_epoch);
  EXPECT_EQ(fields.size(), 2U);
  const auto &metadata = Field(fields, "interrupt_rule_catalog");
  EXPECT_EQ(metadata.as_object().size(), 7U);
  EXPECT_EQ(metadata.find_object_member("catalog_generation")->as_string(),
            "18446744073709551615");
  EXPECT_EQ(
      metadata.find_object_member("active_publication_generation")->as_number(),
      3.0);
  EXPECT_EQ(
      metadata.find_object_member("rule_ids")->as_array().front().as_string(),
      "global.door");
  EXPECT_EQ(metadata.find_object_member("referenced_program_blackboard_keys")
                ->as_array()
                .front()
                .as_string(),
            "permit");
  const auto &pending = Field(fields, "interrupt_rule_state_baseline");
  ASSERT_EQ(pending.as_object().size(), 1U);
  EXPECT_EQ(pending.find_object_member("status")->as_string(),
            "projection_pending");
  EXPECT_EQ(pending.find_object_member("snapshot"), nullptr);
}
TEST(GlobalRuleEventResponseTest,
     ReadyUsesExactCutAndPreservesActualCausalCursor) {
  const auto catalog = Catalog();
  const core::InterruptSourceId source("program.blackboard");
  core::IoRuleStateBaselineDTO baseline{
      catalog->context,
      9U,
      {{catalog->rules.front().policy_id, core::IoGlobalRuleState::kTripped,
        core::IoRuleAcquisitionKeyDTO{
            source,
            core::IoSourceIncarnation("bb.epoch"),
            UINT64_MAX,
            {catalog->context.owner_epoch, UINT64_MAX, source, UINT64_MAX}}}}};
  const auto fields = InterruptRuleEventMetadataFields(
      core::IoRuleStateProjectionDTO{
          core::IoRuleStateProjectionStatusDTO::kReady, baseline, catalog},
      catalog->context.owner_epoch);
  const auto &ready = Field(fields, "interrupt_rule_state_baseline");
  EXPECT_EQ(ready.find_object_member("status")->as_string(), "ready");
  const auto &snapshot = *ready.find_object_member("snapshot");
  EXPECT_EQ(snapshot.as_object().size(), 7U);
  EXPECT_EQ(snapshot.find_object_member("through_sequence")->as_number(), 9.0);
  const auto &row = snapshot.find_object_member("states")->as_array().front();
  EXPECT_EQ(row.find_object_member("state")->as_string(), "tripped");
  const auto &acquired = *row.find_object_member("triggering_acquisition");
  EXPECT_EQ(acquired.find_object_member("source_sequence")->as_string(),
            "18446744073709551615");
  EXPECT_EQ(acquired.find_object_member("source_cursor")
                ->find_object_member("record_sequence")
                ->as_string(),
            "18446744073709551615");
}
TEST(GlobalRuleEventResponseTest,
     VerifiedEmptyUsesOnlyActualAppliedContextWithoutCatalogOrBaseline) {
  const core::IoRuleAppliedContextDTO context{
      core::RequestOwnerEpoch("kernel.owner"),
      core::ActiveBundlePublicationGeneration(9'007'199'254'740'991ULL),
      core::RetainedBundleId("bundle.actual"),
      core::GraphRevision("graph.actual")};
  core::IoRuleStateProjectionDTO empty{
      core::IoRuleStateProjectionStatusDTO::kReady, std::nullopt, {}, context};
  const auto fields =
      InterruptRuleEventMetadataFields(empty, context.owner_epoch);
  const auto &metadata = Field(fields, "interrupt_rule_catalog");
  ASSERT_EQ(metadata.as_object().size(), 5U);
  EXPECT_EQ(metadata.find_object_member("status")->as_string(),
            "verified_empty");
  EXPECT_EQ(metadata.find_object_member("owner_epoch")->as_string(),
            "kernel.owner");
  EXPECT_EQ(
      metadata.find_object_member("active_publication_generation")->as_number(),
      9'007'199'254'740'991.0);
  EXPECT_EQ(metadata.find_object_member("bundle")->as_string(),
            "bundle.actual");
  EXPECT_EQ(metadata.find_object_member("graph")->as_string(), "graph.actual");
  EXPECT_TRUE(Field(fields, "interrupt_rule_state_baseline").is_null());
  EXPECT_EQ(metadata.find_object_member("catalog_generation"), nullptr);
  EXPECT_EQ(metadata.find_object_member("rule_ids"), nullptr);
  EXPECT_THROW((void)InterruptRuleEventMetadataFields(
                   empty, core::RequestOwnerEpoch("foreign")),
               std::exception);
  empty.status = core::IoRuleStateProjectionStatusDTO::kPending;
  const auto pending = InterruptRuleEventMetadataFields(empty, context.owner_epoch);
  EXPECT_EQ(Field(pending, "interrupt_rule_catalog"), metadata);
  const auto &pending_baseline = Field(pending, "interrupt_rule_state_baseline");
  ASSERT_EQ(pending_baseline.as_object().size(), 1U);
  EXPECT_EQ(pending_baseline.find_object_member("status")->as_string(),
            "projection_pending");
  EXPECT_EQ(pending_baseline.find_object_member("snapshot"), nullptr);
  empty.status = static_cast<core::IoRuleStateProjectionStatusDTO>(255);
  EXPECT_THROW((void)InterruptRuleEventMetadataFields(empty, context.owner_epoch),
               std::exception);
  empty.status = core::IoRuleStateProjectionStatusDTO::kReady;
  empty.catalog = Catalog();
  EXPECT_THROW(
      (void)InterruptRuleEventMetadataFields(empty, context.owner_epoch),
      std::exception);
  empty.catalog.reset();
  empty.baseline = core::IoRuleStateBaselineDTO{Catalog()->context, 0U, {}};
  EXPECT_THROW(
      (void)InterruptRuleEventMetadataFields(empty, context.owner_epoch),
      std::exception);
  empty.baseline.reset();
  empty.verified_empty->active_publication_generation =
      core::ActiveBundlePublicationGeneration(0U);
  EXPECT_THROW(
      (void)InterruptRuleEventMetadataFields(empty, context.owner_epoch),
      std::exception);
  empty.verified_empty->active_publication_generation =
      core::ActiveBundlePublicationGeneration(9'007'199'254'740'992ULL);
  EXPECT_THROW(
      (void)InterruptRuleEventMetadataFields(empty, context.owner_epoch),
      std::exception);
  empty.verified_empty = context;
  empty.verified_empty->retained_bundle_id = core::RetainedBundleId("");
  EXPECT_THROW(
      (void)InterruptRuleEventMetadataFields(empty, context.owner_epoch),
      std::exception);
}
TEST(GlobalRuleEventResponseTest,
     NullMeansOnlyAbsentCatalogAndMixedSnapshotsRefuse) {
  const auto catalog = Catalog();
  const auto absent = InterruptRuleEventMetadataFields(
      std::nullopt, catalog->context.owner_epoch);
  EXPECT_TRUE(Field(absent, "interrupt_rule_catalog").is_null());
  EXPECT_TRUE(Field(absent, "interrupt_rule_state_baseline").is_null());
  core::IoRuleStateProjectionDTO pending{
      core::IoRuleStateProjectionStatusDTO::kPending, std::nullopt, {}};
  EXPECT_THROW((void)InterruptRuleEventMetadataFields(
                   pending, catalog->context.owner_epoch),
               std::exception);
  pending.catalog = catalog;
  EXPECT_THROW((void)InterruptRuleEventMetadataFields(
                   pending, core::RequestOwnerEpoch("foreign")),
               std::exception);
  core::IoRuleStateBaselineDTO baseline{
      catalog->context,
      0U,
      {{catalog->rules.front().policy_id, core::IoGlobalRuleState::kClear,
        std::nullopt}}};
  baseline.context.active_publication_generation =
      core::ActiveBundlePublicationGeneration(4U);
  EXPECT_THROW(
      (void)InterruptRuleEventMetadataFields(
          core::IoRuleStateProjectionDTO{
              core::IoRuleStateProjectionStatusDTO::kReady, baseline, catalog},
          catalog->context.owner_epoch),
      std::exception);
  baseline.context = catalog->context;
  baseline.states.clear();
  EXPECT_THROW(
      (void)InterruptRuleEventMetadataFields(
          core::IoRuleStateProjectionDTO{
              core::IoRuleStateProjectionStatusDTO::kReady, baseline, catalog},
          catalog->context.owner_epoch),
      std::exception);
}
TEST(GlobalRuleEventResponseTest,
     NoServiceWaitingUsesAppliedMetadataAndPendingWithoutCatalogGeneration) {
  const auto waiting = WaitingCatalog();
  const auto fields = InterruptRuleEventMetadataFields(
      core::IoRuleStateProjectionDTO{
          core::IoRuleStateProjectionStatusDTO::kPending, std::nullopt, {},
          std::nullopt, waiting},
      waiting->context.owner_epoch);
  ASSERT_EQ(fields.size(), 2U);
  const auto &metadata = Field(fields, "interrupt_rule_catalog");
  ASSERT_EQ(metadata.as_object().size(), 7U);
  EXPECT_EQ(metadata.find_object_member("status")->as_string(),
            "waiting_for_program");
  EXPECT_EQ(metadata.find_object_member("owner_epoch")->as_string(),
            waiting->context.owner_epoch.value());
  EXPECT_EQ(metadata.find_object_member("active_publication_generation")
                ->as_number(),
            3.0);
  EXPECT_EQ(metadata.find_object_member("bundle")->as_string(),
            waiting->context.retained_bundle_id.value());
  EXPECT_EQ(metadata.find_object_member("graph")->as_string(),
            waiting->context.graph_revision.value());
  const auto &ids = metadata.find_object_member("rule_ids")->as_array();
  ASSERT_EQ(ids.size(), 1U);
  EXPECT_EQ(ids.front().as_string(), "global.waiting");
  const auto &rows =
      metadata.find_object_member("waiting_for_program")->as_array();
  ASSERT_EQ(rows.size(), 1U);
  ASSERT_EQ(rows.front().as_object().size(), 2U);
  EXPECT_EQ(rows.front().find_object_member("rule_id")->as_string(),
            "global.waiting");
  const auto &keys = rows.front().find_object_member("keys")->as_array();
  ASSERT_EQ(keys.size(), 2U);
  EXPECT_EQ(keys[0].as_string(), "permit");
  EXPECT_EQ(keys[1].as_string(), "ready");
  for (const auto *absent : {"catalog_generation", "program_binding_id",
                             "referenced_program_blackboard_keys", "states",
                             "source_id"})
    EXPECT_EQ(metadata.find_object_member(absent), nullptr);
  const auto &baseline = Field(fields, "interrupt_rule_state_baseline");
  ASSERT_EQ(baseline.as_object().size(), 1U);
  EXPECT_EQ(baseline.find_object_member("status")->as_string(),
            "projection_pending");
  EXPECT_EQ(baseline.find_object_member("snapshot"), nullptr);
}

TEST(GlobalRuleEventResponseTest,
     WaitingProjectionRejectsMixedAuthorityAndNoncanonicalMetadata) {
  const auto waiting = WaitingCatalog();
  core::IoRuleStateProjectionDTO projection{
      core::IoRuleStateProjectionStatusDTO::kPending, std::nullopt, {},
      std::nullopt, waiting};
  const auto refused = [&] {
    EXPECT_THROW((void)InterruptRuleEventMetadataFields(
                     projection, waiting->context.owner_epoch),
                 std::exception);
  };
  projection.status = core::IoRuleStateProjectionStatusDTO::kReady;
  refused();
  projection.status = core::IoRuleStateProjectionStatusDTO::kPending;
  projection.catalog = Catalog();
  refused();
  projection.catalog.reset();
  projection.verified_empty = waiting->context;
  refused();
  projection.verified_empty.reset();
  projection.baseline = core::IoRuleStateBaselineDTO{Catalog()->context, 0U, {}};
  refused();
  projection.baseline.reset();
  waiting->rules.front().waiting_for_program_keys = {"ready", "permit"};
  refused();
  waiting->rules.front().waiting_for_program_keys = {"permit", "ready"};
  waiting->rules.push_back(waiting->rules.front());
  refused();
  waiting->rules.pop_back();
  waiting->rules.front().condition = Catalog()->rules.front().condition;
  refused();
  waiting->rules.front().condition.reset();
  waiting->rules.front().required_signal_sources = {
      core::InterruptSourceId("source.requires.service")};
  refused();
  waiting->rules.front().required_signal_sources.clear();
  EXPECT_THROW((void)InterruptRuleEventMetadataFields(
                   projection, core::RequestOwnerEpoch("foreign.owner")),
               std::exception);
}

TEST(GlobalRuleEventResponseTest,
     MixedActualServiceCatalogKeepsItsContextAndOnlyAnnotatesWaitingRows) {
  const auto catalog = Catalog();
  core::IoConditionDefinitionDTO signal;
  signal.inputs.push_back(
      {core::IoSignalId{core::ControllableComponentId("device.io"), "ready"},
       core::IoConditionValueKind::kBoolean});
  signal.nodes.push_back(core::IoConditionCompareDTO{
      core::IoConditionComparison::kEqual,
      core::IoConditionInputReferenceDTO{0U}, core::IoConditionValueDTO{false}});
  const auto checked = core::CheckIoCondition(
      std::move(signal), {8U, 8U, 8U, 8U, 128U, 1024U});
  ASSERT_TRUE(checked.ok()) << checked.error().message;
  catalog->rules.front().condition =
      std::make_shared<const core::CheckedIoCondition>(checked.value());
  catalog->rules.front().required_signal_sources = {
      core::InterruptSourceId("source.io")};
  catalog->referenced_program_blackboard_keys.clear();
  auto waiting = WaitingCatalog()->rules.front();
  waiting.required_signal_sources = {core::InterruptSourceId("source.io")};
  catalog->rules.push_back(std::move(waiting));
  core::IoRuleStateProjectionDTO projection{
      core::IoRuleStateProjectionStatusDTO::kPending, std::nullopt, catalog};
  const auto fields = InterruptRuleEventMetadataFields(
      projection, catalog->context.owner_epoch);
  const auto &metadata = Field(fields, "interrupt_rule_catalog");
  ASSERT_EQ(metadata.as_object().size(), 8U);
  EXPECT_EQ(metadata.find_object_member("status"), nullptr);
  EXPECT_EQ(metadata.find_object_member("catalog_generation")->as_string(),
            "18446744073709551615");
  EXPECT_EQ(metadata.find_object_member("active_publication_generation")
                ->as_number(),
            3.0);
  const auto &ids = metadata.find_object_member("rule_ids")->as_array();
  ASSERT_EQ(ids.size(), 2U);
  EXPECT_EQ(ids[0].as_string(), "global.door");
  EXPECT_EQ(ids[1].as_string(), "global.waiting");
  const auto &rows =
      metadata.find_object_member("waiting_for_program")->as_array();
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().find_object_member("rule_id")->as_string(),
            "global.waiting");
  EXPECT_TRUE(metadata.find_object_member("referenced_program_blackboard_keys")
                  ->as_array().empty());
  EXPECT_EQ(Field(fields, "interrupt_rule_state_baseline")
                .find_object_member("status")->as_string(),
            "projection_pending");
  EXPECT_EQ(Field(fields, "interrupt_rule_state_baseline")
                .find_object_member("snapshot"), nullptr);
  // No waiting row may acquire a fabricated clear/ready baseline, even if
  // the separately checked signal sibling already has qualified state.
  projection.status = core::IoRuleStateProjectionStatusDTO::kReady;
  projection.baseline = core::IoRuleStateBaselineDTO{
      catalog->context, 0U,
      {{catalog->rules[0].policy_id, core::IoGlobalRuleState::kClear, std::nullopt},
       {catalog->rules[1].policy_id, core::IoGlobalRuleState::kClear, std::nullopt}}};
  EXPECT_THROW((void)InterruptRuleEventMetadataFields(
                   projection, catalog->context.owner_epoch),
               std::exception);
}

} // namespace
} // namespace manyforge::roboplan_backend::host_detail
