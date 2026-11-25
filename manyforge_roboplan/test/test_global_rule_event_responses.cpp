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
  definition.nodes.emplace_back(std::in_place_type<core::IoConditionCompareDTO>,
      core::IoConditionComparison::kEqual,
      core::IoConditionOperandDTO{core::IoConditionInputReferenceDTO{0U}},
      core::IoConditionOperandDTO{core::IoConditionValueDTO{true}});
  const auto checked = core::CheckIoCondition(std::move(definition),
                                              {8U, 8U, 8U, 8U, 128U, 1024U});
  if (!checked.ok())
    throw std::runtime_error("projection condition fixture refused");
  auto catalog = std::make_shared<core::IoActivationRuleCatalogDTO>(
      core::IoActivationRuleCatalogDTO{
          {UINT64_MAX,
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
       1,
       std::nullopt});
  catalog->referenced_program_blackboard_keys = {"permit"};
  return catalog;
}
std::shared_ptr<core::IoRuleWaitingForProgramCatalogDTO> WaitingCatalog() {
  return std::make_shared<core::IoRuleWaitingForProgramCatalogDTO>(
      core::IoRuleWaitingForProgramCatalogDTO{
          {core::ActiveBundlePublicationGeneration(3U),
           core::RetainedBundleId("bundle.actual"),
           core::GraphRevision("graph.actual")},
          {{core::InterruptPolicyId("global.waiting"), nullptr, {},
            core::IoActivationAllMotionScopeDTO{}, 1, std::nullopt,
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
          catalog});
  EXPECT_EQ(fields.size(), 2U);
  const auto &metadata = Field(fields, "interrupt_rule_catalog");
  EXPECT_EQ(metadata.as_object().size(), 6U);
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
     ReadyCarriesTheStatesWithoutACutAndPreservesActualCausalCursor) {
  const auto catalog = Catalog();
  const core::InterruptSourceId source("program.blackboard");
  core::IoRuleStateBaselineDTO baseline{
      catalog->context,
      {{catalog->rules.front().policy_id, core::IoGlobalRuleState::kTripped,
        core::IoRuleAcquisitionKeyDTO{
            source,
            core::IoSourceIncarnation("bb.epoch"),
            UINT64_MAX,
            {UINT64_MAX, source, UINT64_MAX}}}}};
  const auto fields = InterruptRuleEventMetadataFields(
      core::IoRuleStateProjectionDTO{
          core::IoRuleStateProjectionStatusDTO::kReady, baseline, catalog});
  const auto &ready = Field(fields, "interrupt_rule_state_baseline");
  EXPECT_EQ(ready.find_object_member("status")->as_string(), "ready");
  const auto &snapshot = *ready.find_object_member("snapshot");
  EXPECT_EQ(snapshot.as_object().size(), 5U);
  EXPECT_EQ(snapshot.find_object_member("through_sequence"), nullptr);
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
      core::ActiveBundlePublicationGeneration(9'007'199'254'740'991ULL),
      core::RetainedBundleId("bundle.actual"),
      core::GraphRevision("graph.actual")};
  core::IoRuleStateProjectionDTO empty{
      core::IoRuleStateProjectionStatusDTO::kReady, std::nullopt, {}, context};
  const auto fields =
      InterruptRuleEventMetadataFields(empty);
  const auto &metadata = Field(fields, "interrupt_rule_catalog");
  ASSERT_EQ(metadata.as_object().size(), 4U);
  EXPECT_EQ(metadata.find_object_member("status")->as_string(),
            "verified_empty");
  EXPECT_EQ(
      metadata.find_object_member("active_publication_generation")->as_number(),
      9'007'199'254'740'991.0);
  EXPECT_EQ(metadata.find_object_member("bundle")->as_string(),
            "bundle.actual");
  EXPECT_EQ(metadata.find_object_member("graph")->as_string(), "graph.actual");
  EXPECT_TRUE(Field(fields, "interrupt_rule_state_baseline").is_null());
  EXPECT_EQ(metadata.find_object_member("catalog_generation"), nullptr);
  EXPECT_EQ(metadata.find_object_member("rule_ids"), nullptr);
  // A verified-empty product is never pending.
  empty.status = core::IoRuleStateProjectionStatusDTO::kPending;
  EXPECT_THROW((void)InterruptRuleEventMetadataFields(empty), std::exception);
  empty.status = static_cast<core::IoRuleStateProjectionStatusDTO>(255);
  EXPECT_THROW((void)InterruptRuleEventMetadataFields(empty),
               std::exception);
  empty.status = core::IoRuleStateProjectionStatusDTO::kReady;
  empty.catalog = Catalog();
  EXPECT_THROW(
      (void)InterruptRuleEventMetadataFields(empty),
      std::exception);
  empty.catalog.reset();
  empty.baseline = core::IoRuleStateBaselineDTO{Catalog()->context, {}};
  EXPECT_THROW(
      (void)InterruptRuleEventMetadataFields(empty),
      std::exception);
  empty.baseline.reset();
  empty.verified_empty->active_publication_generation =
      core::ActiveBundlePublicationGeneration(0U);
  EXPECT_THROW(
      (void)InterruptRuleEventMetadataFields(empty),
      std::exception);
  empty.verified_empty->active_publication_generation =
      core::ActiveBundlePublicationGeneration(9'007'199'254'740'992ULL);
  EXPECT_THROW(
      (void)InterruptRuleEventMetadataFields(empty),
      std::exception);
  empty.verified_empty = context;
  empty.verified_empty->retained_bundle_id = core::RetainedBundleId("");
  EXPECT_THROW(
      (void)InterruptRuleEventMetadataFields(empty),
      std::exception);
}
TEST(GlobalRuleEventResponseTest,
     NullMeansOnlyAbsentCatalogAndMixedSnapshotsRefuse) {
  const auto catalog = Catalog();
  const auto absent = InterruptRuleEventMetadataFields(
      std::nullopt);
  EXPECT_TRUE(Field(absent, "interrupt_rule_catalog").is_null());
  EXPECT_TRUE(Field(absent, "interrupt_rule_state_baseline").is_null());
  core::IoRuleStateProjectionDTO pending{
      core::IoRuleStateProjectionStatusDTO::kPending, std::nullopt, {}};
  EXPECT_THROW((void)InterruptRuleEventMetadataFields(
                   pending),
               std::exception);
  core::IoRuleStateBaselineDTO baseline{
      catalog->context,
      {{catalog->rules.front().policy_id, core::IoGlobalRuleState::kClear,
        std::nullopt}}};
  baseline.context.active_publication_generation =
      core::ActiveBundlePublicationGeneration(4U);
  EXPECT_THROW(
      (void)InterruptRuleEventMetadataFields(
          core::IoRuleStateProjectionDTO{
              core::IoRuleStateProjectionStatusDTO::kReady, baseline, catalog}),
      std::exception);
  baseline.context = catalog->context;
  baseline.states.clear();
  EXPECT_THROW(
      (void)InterruptRuleEventMetadataFields(
          core::IoRuleStateProjectionDTO{
              core::IoRuleStateProjectionStatusDTO::kReady, baseline, catalog}),
      std::exception);
}
TEST(GlobalRuleEventResponseTest,
     NoServiceWaitingUsesAppliedMetadataAndPendingWithoutCatalogGeneration) {
  const auto waiting = WaitingCatalog();
  const auto fields = InterruptRuleEventMetadataFields(
      core::IoRuleStateProjectionDTO{
          core::IoRuleStateProjectionStatusDTO::kPending, std::nullopt, {},
          std::nullopt, waiting});
  ASSERT_EQ(fields.size(), 2U);
  const auto &metadata = Field(fields, "interrupt_rule_catalog");
  ASSERT_EQ(metadata.as_object().size(), 6U);
  EXPECT_EQ(metadata.find_object_member("status")->as_string(),
            "waiting_for_program");
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
                     projection),
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
  projection.baseline = core::IoRuleStateBaselineDTO{Catalog()->context, {}};
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
}

TEST(GlobalRuleEventResponseTest,
     MixedActualServiceCatalogKeepsItsContextAndOnlyAnnotatesWaitingRows) {
  const auto catalog = Catalog();
  core::IoConditionDefinitionDTO signal;
  signal.inputs.push_back(
      {core::IoSignalId{core::ControllableComponentId("device.io"), "ready"},
       core::IoConditionValueKind::kBoolean});
  signal.nodes.emplace_back(std::in_place_type<core::IoConditionCompareDTO>,
      core::IoConditionComparison::kEqual,
      core::IoConditionOperandDTO{core::IoConditionInputReferenceDTO{0U}},
      core::IoConditionOperandDTO{core::IoConditionValueDTO{false}});
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
      projection);
  const auto &metadata = Field(fields, "interrupt_rule_catalog");
  ASSERT_EQ(metadata.as_object().size(), 7U);
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
      catalog->context,
      {{catalog->rules[0].policy_id, core::IoGlobalRuleState::kClear, std::nullopt},
       {catalog->rules[1].policy_id, core::IoGlobalRuleState::kClear, std::nullopt}}};
  EXPECT_THROW((void)InterruptRuleEventMetadataFields(
                   projection),
               std::exception);
}

TEST(GlobalRuleEventResponseTest, NotifyUnknownIncludesTypedReasonAndCatalogIdentity) {
  const auto catalog = Catalog();
  catalog->rules.front().on_trip = core::IoRuleOnTripDTO::kNotify;
  core::IoRuleStateBaselineDTO baseline{
      catalog->context,
      {{catalog->rules.front().policy_id, core::IoGlobalRuleState::kUnknown,
        std::nullopt, core::ErrorDTO{"io_watch_bound_exceeded", "watch unavailable", {{"limit", "4"}}}}}};
  const auto fields = InterruptRuleEventMetadataFields(
      core::IoRuleStateProjectionDTO{core::IoRuleStateProjectionStatusDTO::kReady, baseline, catalog});
  const auto &row = Field(fields, "interrupt_rule_state_baseline")
                        .find_object_member("snapshot")->find_object_member("states")->as_array().front();
  EXPECT_EQ(row.find_object_member("state")->as_string(), "unknown");
  const auto &reason = *row.find_object_member("unavailable_reason");
  EXPECT_EQ(reason.find_object_member("code")->as_string(), "io_watch_bound_exceeded");
  EXPECT_EQ(reason.find_object_member("details")->find_object_member("limit")->as_string(), "4");
}

// A member's state names its owner and role; an
// owner's fault lists the members it concerns; both are absent otherwise.
TEST(ComponentStateResponseTest, CarriesOwnerRoleAndFaultConcerns) {
  core::ComponentState member;
  member.component_id = "lite6_robot";
  member.owner_id = "lite6";
  member.role = "arm";
  member.lifecycle = core::ComponentLifecycle::kOffline;
  member.reason = core::ComponentStateReason{"owner_unavailable", "through lite6: rebooting"};
  const auto value = ComponentStateValue(member);
  EXPECT_EQ(value.find_object_member("owner_id")->as_string(), "lite6");
  EXPECT_EQ(value.find_object_member("role")->as_string(), "arm");
  EXPECT_EQ(value.find_object_member("reason")->find_object_member("code")->as_string(),
            "owner_unavailable");

  core::ComponentState owner;
  owner.component_id = "lite6";
  owner.lifecycle = core::ComponentLifecycle::kFault;
  owner.fault = core::Fault{core::FaultCode::kVendorFault, "C33", true,
                            core::FaultOperatorAction::kRecover, "C33", "I/O module",
                            {"lite6_robot_controller_io"}, 0};
  const auto owned = ComponentStateValue(owner);
  EXPECT_EQ(owned.find_object_member("owner_id"), nullptr);
  EXPECT_EQ(owned.find_object_member("role"), nullptr);
  const auto *concerns = owned.find_object_member("fault")->find_object_member("concerns");
  ASSERT_NE(concerns, nullptr);
  ASSERT_EQ(concerns->as_array().size(), 1U);
  EXPECT_EQ(concerns->as_array()[0].as_string(), "lite6_robot_controller_io");

  owner.fault->concerns.clear();
  EXPECT_EQ(ComponentStateValue(owner).find_object_member("fault")->find_object_member("concerns"),
            nullptr);
  EXPECT_EQ(owned.find_object_member("fault")->find_object_member("details"), nullptr);
}

// A fault and a stop carry the UTC time of `since_ns` (the int64 encoding:
// decimal text above 2^53 - 1); a launch fault carries its output tail.
TEST(ComponentStateResponseTest, CarriesSinceUtcAndFaultDetails) {
  core::ComponentState state;
  state.component_id = "lite6_robot";
  state.lifecycle = core::ComponentLifecycle::kFault;
  state.fault = core::Fault{core::FaultCode::kProcessExited, "controller launch exited", true,
                            core::FaultOperatorAction::kRecover, std::nullopt, std::nullopt,
                            {}, 4};
  state.fault->since_utc_ns = 1'795'122'990'477'663'500;
  state.fault->details = {{"output_tail", "[ros2_control_node-2]: driver failed"}};
  state.stop = core::ComponentStop{core::StopCode::kEstop, "E-stop", "C2", std::nullopt, 5};
  state.stop->since_utc_ns = 7;
  const auto value = ComponentStateValue(state);
  const auto *fault = value.find_object_member("fault");
  EXPECT_EQ(fault->find_object_member("since_utc_ns")->as_string(), "1795122990477663500");
  EXPECT_EQ(fault->find_object_member("details")->find_object_member("output_tail")->as_string(),
            "[ros2_control_node-2]: driver failed");
  EXPECT_EQ(value.find_object_member("stop")->find_object_member("since_utc_ns")->as_number(), 7.0);
}

TEST(ComponentStateResponseTest, CarriesTypedStopOnOwnerAndLatchCause) {
  core::ComponentState owner;
  owner.component_id = "lite6";
  owner.lifecycle = core::ComponentLifecycle::kPrepared;
  owner.activity = core::ComponentActivity::kIdle;
  owner.stop = core::ComponentStop{core::StopCode::kEstop, "E-stop not cleared",
                                    "C2", "Emergency stop", 42};
  const auto state = ComponentStateValue(owner);
  const auto *stop = state.find_object_member("stop");
  ASSERT_NE(stop, nullptr);
  ASSERT_NE(stop->find_object_member("code"), nullptr);
  EXPECT_EQ(stop->find_object_member("code")->as_string(), "estop");
  EXPECT_EQ(stop->find_object_member("message")->as_string(), "E-stop not cleared");
  EXPECT_EQ(stop->find_object_member("vendor_code")->as_string(), "C2");
  EXPECT_EQ(stop->find_object_member("vendor_text")->as_string(), "Emergency stop");
  EXPECT_NE(stop->find_object_member("since_ns"), nullptr);
  EXPECT_EQ(state.find_object_member("fault"), nullptr);

  const core::CellLatch latch{true, core::cell_latch_causes({owner})};
  const auto wire = CellLatchValue(latch);
  const auto *causes = wire.find_object_member("causes");
  ASSERT_NE(causes, nullptr);
  ASSERT_EQ(causes->as_array().size(), 1U);
  const auto &cause = causes->as_array().front();
  EXPECT_EQ(cause.find_object_member("kind")->as_string(), "stop");
  EXPECT_EQ(cause.find_object_member("component_id")->as_string(), "lite6");
  ASSERT_NE(cause.find_object_member("stop"), nullptr);
  EXPECT_EQ(cause.find_object_member("stop")->find_object_member("code")->as_string(), "estop");
  EXPECT_EQ(cause.find_object_member("fault"), nullptr);

  owner.stop->vendor_code.reset();
  owner.stop->vendor_text.reset();
  const auto without_vendor = ComponentStateValue(owner);
  ASSERT_NE(without_vendor.find_object_member("stop"), nullptr);
  EXPECT_EQ(without_vendor.find_object_member("stop")->find_object_member("vendor_code"), nullptr);
  EXPECT_EQ(without_vendor.find_object_member("stop")->find_object_member("vendor_text"), nullptr);
  owner.stop.reset();
  EXPECT_EQ(ComponentStateValue(owner).find_object_member("stop"), nullptr);
}

TEST(ComponentStateResponseTest, CarriesAdapterDetailsInOrderOnlyWhenPresent) {
  core::ComponentState state;
  state.component_id = "lite6";
  state.lifecycle = core::ComponentLifecycle::kPrepared;
  EXPECT_EQ(ComponentStateValue(state).find_object_member("details"), nullptr);

  state.details = {{"mode", "position"}, {"state", "ready"}};
  const auto value = ComponentStateValue(state);
  const auto *details = value.find_object_member("details");
  ASSERT_NE(details, nullptr);
  ASSERT_EQ(details->as_array().size(), 2U);
  EXPECT_EQ(details->as_array()[0].find_object_member("name")->as_string(), "mode");
  EXPECT_EQ(details->as_array()[0].find_object_member("value")->as_string(), "position");
  EXPECT_EQ(details->as_array()[1].find_object_member("name")->as_string(), "state");
  EXPECT_EQ(details->as_array()[1].find_object_member("value")->as_string(), "ready");
}

TEST(ComponentStateResponseTest, CarriesEndpointsOnlyWhenPresent) {
  core::ComponentState state;
  state.component_id = "assistant";
  state.lifecycle = core::ComponentLifecycle::kPrepared;
  EXPECT_EQ(ComponentStateValue(state).find_object_member("endpoints"), nullptr);

  state.lifecycle = core::ComponentLifecycle::kEnabled;
  state.endpoints = {{"assistant_request", "http://127.0.0.1:40123"}};
  const auto value = ComponentStateValue(state);
  const auto *endpoints = value.find_object_member("endpoints");
  ASSERT_NE(endpoints, nullptr);
  ASSERT_EQ(endpoints->as_array().size(), 1U);
  EXPECT_EQ(endpoints->as_array()[0].find_object_member("role_id")->as_string(),
            "assistant_request");
  EXPECT_EQ(endpoints->as_array()[0].find_object_member("url")->as_string(),
            "http://127.0.0.1:40123");
}

} // namespace
} // namespace manyforge::roboplan_backend::host_detail
