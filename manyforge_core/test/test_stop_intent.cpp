#include <manyforge_core/testing/request_key_fixture.hpp>
#include <gtest/gtest.h>

#include <manyforge_core/types/operation_runtime.hpp>
#include <string>
#include <variant>
#include <vector>

namespace manyforge::core {
namespace {

[[nodiscard]] PublicRequestContextDTO request_context() {
  return PublicRequestContextDTO{TestRequestKey("request.stop.1"),
                                 "test-client"};
}

[[nodiscard]] StopIntentDTO operation_stop() {
  return StopIntentDTO{request_context(),
                       std::vector<StopOperationTargetDTO>{
                           {OperationNumber(1U), OperationRevision(3U)}},
                       std::string("operator requested Stop"),
                       RequestAttributionDTO{RequestTransportDTO::kHttp,
                                             RequestAssuranceDTO::kExternallyAuthenticated,
                                             std::string("operator.1")}};
}

TEST(StopIntentTest, AcceptsEveryClosedSelectorAlternative) {
  static_assert(std::variant_size_v<StopSelectorDTO> == 3U);

  const auto operation = operation_stop();
  EXPECT_FALSE(stop_intent_field_error(operation).has_value());
  ASSERT_NE(std::get_if<std::vector<StopOperationTargetDTO>>(&operation.selector), nullptr);

  const StopIntentDTO component{
      request_context(),
      std::vector<StopComponentTargetDTO>{{ControllableComponentId("robot.a"),
                                           GraphRevision("sha256:graph-a"),
                                           ComponentLifecycleGeneration(4U)}},
      std::nullopt,
      {}};
  EXPECT_FALSE(stop_intent_field_error(component).has_value());
  ASSERT_NE(std::get_if<std::vector<StopComponentTargetDTO>>(&component.selector), nullptr);

  const StopIntentDTO resource{
      request_context(),
      std::vector<StopResourceTargetDTO>{
          {ResourceId("robot.a.joint.1"), GraphRevision("sha256:graph-a"), 0U}},
      std::nullopt,
      {}};
  EXPECT_FALSE(stop_intent_field_error(resource).has_value());
  ASSERT_NE(std::get_if<std::vector<StopResourceTargetDTO>>(&resource.selector), nullptr);
}

// The program-run selector alternative was deleted; a run is now ended by its
// own intent (`end_program_run`), validated here.
TEST(StopIntentTest, AcceptsProgramRunEndIntent) {
  const ProgramRunEndIntentDTO program_run{request_context(), ProgramRunNumber(101U), {}};
  EXPECT_FALSE(program_run_end_intent_field_error(program_run).has_value());
}

TEST(StopIntentTest, RejectsEmptyUnsortedAndDuplicateSelectors) {
  auto empty = operation_stop();
  empty.selector = std::vector<StopOperationTargetDTO>{};
  EXPECT_EQ(stop_intent_field_error(empty), "stop_operation_targets_not_canonical");

  auto unsorted = operation_stop();
  unsorted.selector = std::vector<StopOperationTargetDTO>{
      {OperationNumber(2U), OperationRevision(1U)},
      {OperationNumber(1U), OperationRevision(1U)}};
  EXPECT_EQ(stop_intent_field_error(unsorted), "stop_operation_targets_not_canonical");

  auto duplicate = operation_stop();
  duplicate.selector = std::vector<StopComponentTargetDTO>{
      {ControllableComponentId("robot.a"), GraphRevision("sha256:graph-a"),
       ComponentLifecycleGeneration(1U)},
      {ControllableComponentId("robot.a"), GraphRevision("sha256:graph-a"),
       ComponentLifecycleGeneration(1U)}};
  EXPECT_EQ(stop_intent_field_error(duplicate), "stop_component_targets_not_canonical");
}

TEST(StopIntentTest, EnforcesSelectorCardinalityAndTypedExpectations) {
  auto too_many = operation_stop();
  std::vector<StopResourceTargetDTO> targets;
  targets.reserve(kMaximumStopSelectorTargets + 1U);
  for (std::size_t index = 0U; index <= kMaximumStopSelectorTargets; ++index) {
    targets.push_back(StopResourceTargetDTO{ResourceId("resource." + std::to_string(index)),
                                            GraphRevision("sha256:graph-a"), 0U});
  }
  too_many.selector = std::move(targets);
  EXPECT_EQ(stop_intent_field_error(too_many), "stop_resource_targets_not_canonical");

  auto zero_revision = operation_stop();
  zero_revision.selector = std::vector<StopOperationTargetDTO>{
      {OperationNumber(1U), OperationRevision(0U)}};
  EXPECT_EQ(stop_intent_field_error(zero_revision), "stop_operation_target_invalid");

  auto zero_lifecycle_generation = operation_stop();
  zero_lifecycle_generation.selector = std::vector<StopComponentTargetDTO>{
      {ControllableComponentId("robot.a"), GraphRevision("sha256:graph-a"),
       ComponentLifecycleGeneration(0U)}};
  EXPECT_EQ(stop_intent_field_error(zero_lifecycle_generation), "stop_component_target_invalid");
}

TEST(StopIntentTest, EnforcesIdentityContextAttributionAndReasonBounds) {
  auto zero_operation = operation_stop();
  zero_operation.selector = std::vector<StopOperationTargetDTO>{
      {OperationNumber(0U), OperationRevision(1U)}};
  EXPECT_EQ(stop_intent_field_error(zero_operation), "stop_operation_target_invalid");

  auto oversized_source = operation_stop();
  oversized_source.request_context.source_label =
      std::string(kMaximumStopRequestLabelBytes + 1U, 'a');
  EXPECT_EQ(stop_intent_field_error(oversized_source), "stop_request_context_invalid");

  auto invalid_attribution = operation_stop();
  invalid_attribution.attribution = RequestAttributionDTO{
      RequestTransportDTO::kHttp, RequestAssuranceDTO::kUnverified, std::string("self-asserted")};
  EXPECT_EQ(stop_intent_field_error(invalid_attribution), "stop_request_attribution_invalid");

  auto oversized_reason = operation_stop();
  oversized_reason.reason = std::string(kMaximumStopReasonBytes + 1U, 'a');
  EXPECT_EQ(stop_intent_field_error(oversized_reason), "stop_reason_invalid");

  auto embedded_nul = operation_stop();
  embedded_nul.reason = std::string("bad\0reason", 10U);
  EXPECT_EQ(stop_intent_field_error(embedded_nul), "stop_reason_invalid");

  auto non_nfc_reason = operation_stop();
  non_nfc_reason.reason = std::string("e\xCC\x81");
  EXPECT_EQ(stop_intent_field_error(non_nfc_reason), "stop_reason_invalid");

  auto invalid_utf8_source = operation_stop();
  invalid_utf8_source.request_context.source_label = std::string("\xFF", 1U);
  EXPECT_EQ(stop_intent_field_error(invalid_utf8_source), "stop_request_context_invalid");
}

TEST(StopIntentTest, ValidatesDistinctFullStopIntent) {
  const FullStopIntentDTO valid{request_context(), {}};
  EXPECT_FALSE(full_stop_intent_field_error(valid).has_value());

  auto invalid = valid;
  invalid.request_context.source_label.clear();
  EXPECT_EQ(full_stop_intent_field_error(invalid), "full_stop_request_context_invalid");

  invalid = valid;
  invalid.attribution = RequestAttributionDTO{RequestTransportDTO::kInternal,
                                              RequestAssuranceDTO::kExternallyAuthenticated,
                                              std::string(kMaximumStopRequestLabelBytes + 1U, 'p')};
  EXPECT_EQ(full_stop_intent_field_error(invalid), "full_stop_request_attribution_invalid");
}

TEST(StopIntentTest, RecognizedStopIngressAcceptsItsThreeReasonsOnly) {
  RecognizedStopIngressDTO ingress{
      TestRequestKey("request.stop.ingress"),
      RequestAttributionDTO{RequestTransportDTO::kHttp, RequestAssuranceDTO::kUnverified,
                            std::nullopt},
      "/api/execution/stops", "recognized_stop_payload_invalid"};
  for (const char* reason : {"recognized_stop_payload_invalid", "recognized_stop_body_oversized",
                             "recognized_stop_start_not_current"}) {
    ingress.reason = reason;
    EXPECT_FALSE(recognized_stop_ingress_field_error(ingress).has_value()) << reason;
  }
  ingress.reason = "stale_application_start_id";
  EXPECT_EQ(recognized_stop_ingress_field_error(ingress), "recognized_stop_ingress_reason_invalid");
}

TEST(StopIntentTest, RecognizedStopIngressLabelIsOptionalAndBoundedLikeAStopContext) {
  RecognizedStopIngressDTO ingress{
      TestRequestKey("request.stop.ingress"),
      RequestAttributionDTO{RequestTransportDTO::kHttp, RequestAssuranceDTO::kUnverified,
                            std::nullopt},
      "/api/execution/stops", "recognized_stop_start_not_current"};
  EXPECT_FALSE(recognized_stop_ingress_field_error(ingress).has_value());
  ingress.source_label = std::string("operator_ui");
  EXPECT_FALSE(recognized_stop_ingress_field_error(ingress).has_value());
  for (const auto& invalid : {std::string(), std::string(kMaximumStopRequestLabelBytes + 1U, 'l'),
                              std::string("\xFF", 1U), std::string("e\xCC\x81")}) {
    ingress.source_label = invalid;
    EXPECT_EQ(recognized_stop_ingress_field_error(ingress),
              "recognized_stop_ingress_label_invalid");
    // The same rule as the label of an ordinary Stop's request context.
    auto ordinary = operation_stop();
    ordinary.request_context.source_label = invalid;
    EXPECT_EQ(stop_intent_field_error(ordinary), "stop_request_context_invalid");
  }
}

}  // namespace
}  // namespace manyforge::core
