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

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_xarm_control/generated/lite6_brand_lifecycle_product_identity.hpp>
#include <manyforge_xarm_control/lite6_brand_lifecycle_catalog.hpp>
#include <manyforge_xarm_control/lite6_brand_lifecycle_port.hpp>

namespace manyforge::xarm_control {
namespace {

constexpr std::string_view kManifestRevision =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

#ifndef MANYFORGE_LITE6_LIFECYCLE_PRODUCT_ROOT
#error "the Lite6 lifecycle product root must be build-owned"
#endif

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

class FakeSupervisedSession final : public ILite6BrandLifecycleTransport {
public:
  [[nodiscard]] XArmObservedState observe(std::int64_t now_ns) const override {
    ++observe_calls;
    auto result = observation;
    result.source_timestamp_ns = now_ns - 3;
    result.transport_owner_received_timestamp_ns = now_ns - 2;
    result.fresh_until_ns = now_ns + 1'000'000'000;
    return result;
  }

  XArmLifecycleCommandResult execute(XArmLifecycleCommand command,
                                     std::string_view request_id,
                                     std::uint64_t expected_generation,
                                     std::int64_t remaining_duration_ns,
                                     std::stop_token cancellation) override {
    remaining_durations.push_back(remaining_duration_ns);
    cancellations.push_back(cancellation.stop_requested());
    commands.push_back(command);
    request_ids.emplace_back(request_id);
    expected_generations.push_back(expected_generation);
    auto result = next_result;
    if (result.observed_generation == 0U) {
      result.observed_generation = expected_generation;
    }
    return result;
  }

  mutable std::size_t observe_calls = 0U;
  XArmObservedState observation;
  XArmLifecycleCommandResult next_result{
      XArmLifecycleResultState::kResponseReceived,
      true,
      true,
      0,
      "accepted",
      0U,
      false};
  std::vector<XArmLifecycleCommand> commands;
  std::vector<std::string> request_ids;
  std::vector<std::uint64_t> expected_generations;
  std::vector<std::int64_t> remaining_durations;
  std::vector<bool> cancellations;
};

[[nodiscard]] Lite6BrandLifecyclePortConfig port_config() {
  return {core::ControllableComponentId("lite6.component"),
          core::ControlDomainId("lite6.domain"),
          core::ControlDomainRevision("domain-revision.1"),
          core::ProviderInstanceId("lite6.provider"),
          core::ProviderManifestRevision(std::string(kManifestRevision)),
          core::RuntimeUnitId("lite6.runtime"),
          core::RuntimeEpoch("lite6.runtime.epoch"),
          "xarm.owner.session"};
}

[[nodiscard]] core::ProviderManifestDTO provider_manifest() {
  const auto semantic = compute_lite6_brand_lifecycle_catalog_semantic_digest();
  EXPECT_TRUE(semantic.ok());
  std::vector<core::ProviderCapabilityOfferDTO> lifecycle_capabilities;
  for (const std::string_view capability_id : {
           "component.lifecycle.disable",
           "component.lifecycle.enable",
           "component.lifecycle.observe",
           "component.lifecycle.prepare",
           "component.lifecycle.recover",
       }) {
    const auto *definition =
        core::find_builtin_capability_definition(capability_id);
    EXPECT_NE(definition, nullptr);
    lifecycle_capabilities.push_back(
        {core::CapabilityId(std::string(capability_id)),
         core::CapabilityDefinitionRevision(
             std::string(definition->capability_definition_revision)),
         {}});
  }
  return {
      core::ProviderManifestId(std::string(kLite6LifecycleProviderManifestId)),
      core::ProviderManifestRevision(std::string(kManifestRevision)),
      std::move(lifecycle_capabilities),
      {},
      core::ProviderLifecyclePlanCatalogDeclarationDTO{
          core::SchemaId(
              std::string(core::kBrandLifecyclePlanCatalogApiVersion)),
          semantic.value()},
  };
}

[[nodiscard]] core::BrandLifecycleObservationQueryDTO observation_query() {
  const auto config = port_config();
  return {
      core::LifecycleTransactionId("lifecycle.transaction"),
      core::LifecycleTransactionGeneration(7U),
      config.controllable_component_id,
      config.control_domain_id,
      config.control_domain_revision,
      config.provider_instance_id,
      core::ProviderManifestId(std::string(kLite6LifecycleProviderManifestId)),
      config.provider_manifest_revision,
      config.provider_runtime_unit_id,
      config.provider_runtime_epoch};
}

[[nodiscard]] core::BrandLifecyclePrimitiveIntentDTO
primitive_intent(std::string primitive_id) {
  const auto query = observation_query();
  return {query.lifecycle_transaction_id,
          query.lifecycle_transaction_generation,
          query.controllable_component_id,
          query.control_domain_id,
          query.control_domain_revision,
          query.provider_instance_id,
          query.provider_manifest_id,
          query.provider_manifest_revision,
          query.provider_runtime_unit_id,
          query.provider_runtime_epoch,
          core::LifecyclePrimitiveId(std::move(primitive_id)),
          core::LifecyclePrimitiveAttemptId("primitive.attempt"),
          11U,
          5'000'000'000LL};
}

[[nodiscard]] XArmObservedState disabled_observation() {
  XArmObservedState result;
  result.transport_owner_component_epoch = "xarm.owner.session";
  result.transport_owner_progress_sequence = 19U;
  result.transport_owner_progress_healthy = true;
  result.generation = 11U;
  result.transport_connected = FactState::kTrue;
  result.identity_verified = FactState::kTrue;
  result.position_valid = FactState::kTrue;
  result.motion_enabled = FactState::kFalse;
  result.vendor_control_ready = FactState::kFalse;
  result.fault_present = FactState::kFalse;
  result.recovery_required = FactState::kFalse;
  result.mode = 1;
  result.state = 4;
  result.warning_code = 0;
  return result;
}

[[nodiscard]] const core::LifecycleFactDTO *
find_fact(const core::BrandLifecycleObservationDTO &observation,
          std::string_view id) {
  const auto found =
      std::lower_bound(observation.facts.begin(), observation.facts.end(),
                       core::LifecycleFactId(std::string(id)),
                       [](const auto &candidate, const auto &expected_id) {
                         return candidate.lifecycle_fact_id < expected_id;
                       });
  return found != observation.facts.end() &&
                 found->lifecycle_fact_id ==
                     core::LifecycleFactId(std::string(id))
             ? &*found
             : nullptr;
}

[[nodiscard]] std::string
selected_primitive(core::ComponentLifecycleIntentKindDTO intent,
                   core::ComponentLifecycleStateDTO target,
                   const core::BrandLifecyclePlanCatalogDTO &catalog,
                   const core::BrandLifecycleObservationDTO &observation) {
  const auto decision =
      core::EvaluateLifecycleDomainPlan(intent, target, catalog, observation);
  if (!decision.ok() || !decision.value().selected_transition.has_value()) {
    return {};
  }
  return decision.value().selected_transition->lifecycle_primitive_id.value();
}

TEST(Lite6BrandLifecycleCatalog, DerivesAndVerifiesOneExactBuildOwnedRow) {
  const auto manifest = provider_manifest();
  auto source = Lite6BrandLifecyclePlanCatalogSource::make(manifest);
  ASSERT_TRUE(source.ok()) << source.error().message;
  const auto &catalog = source.value().catalog();
  ASSERT_TRUE(core::VerifyBrandLifecyclePlanCatalog(catalog).ok());
  EXPECT_EQ(
      catalog.provider_manifest_id,
      core::ProviderManifestId(std::string(kLite6LifecycleProviderManifestId)));
  ASSERT_EQ(catalog.plans.size(), 4U);

  const auto encoded = core::EncodeBrandLifecyclePlanCatalog(catalog);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  EXPECT_NE(encoded.value().find(catalog.catalog_digest.value()),
            std::string::npos);
  EXPECT_EQ(encoded.value().find("shutdown"), std::string::npos);
  EXPECT_EQ(encoded.value().find("controller"), std::string::npos);
  EXPECT_EQ(encoded.value().find("command_gate"), std::string::npos);

  const auto loaded = source.value().LoadExactLifecyclePlanCatalog(
      catalog.provider_manifest_id, catalog.provider_manifest_revision,
      catalog.catalog_digest);
  ASSERT_TRUE(loaded.ok()) << loaded.error().message;
  EXPECT_EQ(loaded.value(), catalog);

  const auto wrong_digest = source.value().LoadExactLifecyclePlanCatalog(
      catalog.provider_manifest_id, catalog.provider_manifest_revision,
      core::ContentDigest(
          "sha256:"
          "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
  ASSERT_FALSE(wrong_digest.ok());
  EXPECT_EQ(wrong_digest.error().code,
            "brand_lifecycle_plan_catalog_digest_mismatch");

  auto wrong_anchor = manifest;
  wrong_anchor.lifecycle_plan_catalog->semantic_digest = core::ContentDigest(
      "sha256:"
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
  const auto mismatched_source =
      Lite6BrandLifecyclePlanCatalogSource::make(std::move(wrong_anchor));
  ASSERT_FALSE(mismatched_source.ok());
  EXPECT_EQ(mismatched_source.error().code,
            "brand_lifecycle_plan_catalog_semantic_digest_mismatch");

  auto invalid_manifest = manifest;
  invalid_manifest.provider_manifest_revision =
      core::ProviderManifestRevision("manifest-revision-not-a-digest");
  const auto invalid_revision =
      make_lite6_brand_lifecycle_plan_catalog(invalid_manifest);
  ASSERT_FALSE(invalid_revision.ok());
}

TEST(Lite6BrandLifecycleCatalog,
     ProductionManifestCatalogAndRegistryRowReadBackExactly) {
  const auto manifest = make_build_owned_lite6_lifecycle_provider_manifest();
  ASSERT_TRUE(manifest.ok()) << manifest.error().message;
  EXPECT_EQ(manifest.value().provider_manifest_revision.value(),
            generated::kLite6LifecycleProviderManifestRevision);
  ASSERT_TRUE(manifest.value().lifecycle_plan_catalog.has_value());
  EXPECT_EQ(manifest.value().lifecycle_plan_catalog->semantic_digest.value(),
            generated::kLite6LifecycleCatalogSemanticDigest);
  ASSERT_EQ(manifest.value().offered_capabilities.size(), 5U);

  auto source = Lite6BrandLifecyclePlanCatalogSource::make_build_owned();
  ASSERT_TRUE(source.ok()) << source.error().message;
  EXPECT_EQ(source.value().catalog().catalog_digest.value(),
            generated::kLite6LifecycleCatalogDigest);

  const auto encoded =
      core::EncodeBrandLifecyclePlanCatalog(source.value().catalog());
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  const std::filesystem::path root(MANYFORGE_LITE6_LIFECYCLE_PRODUCT_ROOT);
  EXPECT_EQ(read_file(root / "manyforge_xarm_control/resources/lifecycle/"
                             "lite6_arm_provider.lifecycle_plan_catalog.json"),
            encoded.value());

  const auto manifest_bytes =
      read_file(root / "manyforge_xarm_control/resources/lifecycle/"
                       "lite6_arm_provider.json");
  ASSERT_FALSE(manifest_bytes.empty());
  EXPECT_NE(manifest_bytes.find(std::string(
                generated::kLite6LifecycleProviderManifestRevision)),
            std::string::npos);
  EXPECT_NE(manifest_bytes.find(
                std::string(generated::kLite6LifecycleCatalogSemanticDigest)),
            std::string::npos);
}

TEST(Lite6BrandLifecycleCatalog,
     MaintainedNativeGripperManifestUsesTheProductLifecycleProfile) {
  const auto semantic =
      lite6_product::compute_lite6_gripper_lifecycle_catalog_semantic_digest();
  ASSERT_TRUE(semantic.ok()) << semantic.error().message;
  const std::filesystem::path root(MANYFORGE_LITE6_LIFECYCLE_PRODUCT_ROOT);
  const auto manifest_bytes = read_file(
      root / "manyforge_deployment/resources/maintained/catalog/"
           "provider_manifest/lite6_gripper_provider.json");
  ASSERT_FALSE(manifest_bytes.empty());
  EXPECT_NE(manifest_bytes.find(semantic.value().value()), std::string::npos);
}

TEST(Lite6BrandLifecyclePort, NormalizesExactStatesAndRejectsCrossEpoch) {
  core::FakeClock clock;
  clock.set(10'000);
  auto transport = std::make_unique<FakeSupervisedSession>();
  auto *fake = transport.get();
  fake->observation = disabled_observation();
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));

  const auto disabled = port.Observe(observation_query());
  ASSERT_TRUE(disabled.ok()) << disabled.error().message;
  EXPECT_EQ(disabled.value().observed_state,
            core::ComponentLifecycleStateDTO::kDisabled);
  const auto *feedback =
      find_fact(disabled.value(), kXArmVendorFeedbackClassFactId);
  ASSERT_NE(feedback, nullptr);
  ASSERT_TRUE(feedback->value.token_value.has_value());
  EXPECT_EQ(*feedback->value.token_value, "stopped");
  EXPECT_TRUE(std::all_of(disabled.value().facts.begin(),
                          disabled.value().facts.end(), [](const auto &fact) {
                            return fact.value.kind !=
                                   core::LifecycleFactValueKindDTO::kInteger;
                          }));

  fake->observation.motion_enabled = FactState::kTrue;
  fake->observation.state = 5;
  fake->observation.vendor_control_ready = FactState::kFalse;
  ++fake->observation.generation;
  const auto prepared = port.Observe(observation_query());
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().observed_state,
            core::ComponentLifecycleStateDTO::kPrepared);

  fake->observation.state = 2;
  fake->observation.vendor_control_ready = FactState::kTrue;
  ++fake->observation.generation;
  const auto active = port.Observe(observation_query());
  ASSERT_TRUE(active.ok()) << active.error().message;
  EXPECT_EQ(active.value().observed_state,
            core::ComponentLifecycleStateDTO::kActive);

  fake->observation.transport_owner_component_epoch = "replacement.epoch";
  const auto cross_epoch = port.Observe(observation_query());
  ASSERT_FALSE(cross_epoch.ok());
  EXPECT_EQ(cross_epoch.error().code, "xarm_lifecycle_observation_unavailable");

  auto wrong_query = observation_query();
  wrong_query.provider_instance_id = core::ProviderInstanceId("other.provider");
  const auto calls_before = fake->observe_calls;
  const auto wrong_instance = port.Observe(wrong_query);
  ASSERT_FALSE(wrong_instance.ok());
  EXPECT_EQ(fake->observe_calls, calls_before);
}

TEST(Lite6BrandLifecyclePort, CatalogSelectsTheFrozenBrandOnlySequences) {
  core::FakeClock clock;
  clock.set(10'000);
  auto catalog = make_lite6_brand_lifecycle_plan_catalog(provider_manifest());
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;
  auto transport = std::make_unique<FakeSupervisedSession>();
  auto *fake = transport.get();
  fake->observation = disabled_observation();
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));

  auto observation = port.Observe(observation_query());
  ASSERT_TRUE(observation.ok()) << observation.error().message;
  EXPECT_EQ(selected_primitive(core::ComponentLifecycleIntentKindDTO::kPrepare,
                               core::ComponentLifecycleStateDTO::kPrepared,
                               catalog.value(), observation.value()),
            kXArmEnableMotionPrimitiveId);

  fake->observation.motion_enabled = FactState::kTrue;
  fake->observation.mode = 0;
  ++fake->observation.generation;
  observation = port.Observe(observation_query());
  ASSERT_TRUE(observation.ok()) << observation.error().message;
  EXPECT_EQ(selected_primitive(core::ComponentLifecycleIntentKindDTO::kPrepare,
                               core::ComponentLifecycleStateDTO::kPrepared,
                               catalog.value(), observation.value()),
            kXArmSelectJointServoModePrimitiveId);

  fake->observation.mode = 1;
  fake->observation.state = 5;
  ++fake->observation.generation;
  observation = port.Observe(observation_query());
  ASSERT_TRUE(observation.ok()) << observation.error().message;
  EXPECT_EQ(observation.value().observed_state,
            core::ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(selected_primitive(core::ComponentLifecycleIntentKindDTO::kEnable,
                               core::ComponentLifecycleStateDTO::kActive,
                               catalog.value(), observation.value()),
            kXArmSetReadyStatePrimitiveId);

  fake->observation.state = 2;
  fake->observation.vendor_control_ready = FactState::kTrue;
  ++fake->observation.generation;
  observation = port.Observe(observation_query());
  ASSERT_TRUE(observation.ok()) << observation.error().message;
  EXPECT_EQ(selected_primitive(core::ComponentLifecycleIntentKindDTO::kDisable,
                               core::ComponentLifecycleStateDTO::kDisabled,
                               catalog.value(), observation.value()),
            kXArmSetStoppedStatePrimitiveId);

  fake->observation.state = 4;
  fake->observation.vendor_control_ready = FactState::kFalse;
  ++fake->observation.generation;
  observation = port.Observe(observation_query());
  ASSERT_TRUE(observation.ok()) << observation.error().message;
  EXPECT_EQ(selected_primitive(core::ComponentLifecycleIntentKindDTO::kDisable,
                               core::ComponentLifecycleStateDTO::kDisabled,
                               catalog.value(), observation.value()),
            kXArmDisableMotionPrimitiveId);

  fake->observation.motion_enabled = FactState::kFalse;
  fake->observation.fault_present = FactState::kTrue;
  fake->observation.recovery_required = FactState::kTrue;
  fake->observation.warning_code = 17;
  ++fake->observation.generation;
  observation = port.Observe(observation_query());
  ASSERT_TRUE(observation.ok()) << observation.error().message;
  EXPECT_EQ(selected_primitive(core::ComponentLifecycleIntentKindDTO::kRecover,
                               core::ComponentLifecycleStateDTO::kDisabled,
                               catalog.value(), observation.value()),
            kXArmClearErrorPrimitiveId);

  fake->observation.fault_present = FactState::kFalse;
  fake->observation.recovery_required = FactState::kFalse;
  ++fake->observation.generation;
  observation = port.Observe(observation_query());
  ASSERT_TRUE(observation.ok()) << observation.error().message;
  EXPECT_EQ(selected_primitive(core::ComponentLifecycleIntentKindDTO::kRecover,
                               core::ComponentLifecycleStateDTO::kDisabled,
                               catalog.value(), observation.value()),
            kXArmClearWarningPrimitiveId);
}

TEST(Lite6BrandLifecyclePort, DispatchesOnlyCatalogPrimitivesWithExactTruth) {
  core::FakeClock clock;
  clock.set(10'000);
  auto transport = std::make_unique<FakeSupervisedSession>();
  auto *fake = transport.get();
  fake->observation = disabled_observation();
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));

  // The restricted owner reports its post-call observation generation. It
  // normally advances after an effect; Core correlation remains the intent's
  // expected pre-call generation.
  fake->next_result.observed_generation = 12U;

  const std::vector<std::pair<std::string_view, XArmLifecycleCommand>>
      supported{
          {kXArmEnableMotionPrimitiveId, XArmLifecycleCommand::kEnableMotion},
          {kXArmSelectJointServoModePrimitiveId,
           XArmLifecycleCommand::kSelectJointServoMode},
          {kXArmSetReadyStatePrimitiveId, XArmLifecycleCommand::kSetReadyState},
          {kXArmSetStoppedStatePrimitiveId,
           XArmLifecycleCommand::kSetStoppedState},
          {kXArmDisableMotionPrimitiveId, XArmLifecycleCommand::kDisableMotion},
          {kXArmClearErrorPrimitiveId, XArmLifecycleCommand::kClearError},
          {kXArmClearWarningPrimitiveId, XArmLifecycleCommand::kClearWarning},
      };
  for (const auto &[primitive, expected_command] : supported) {
    auto result = port.Dispatch(primitive_intent(std::string(primitive)),
                                std::stop_token{});
    ASSERT_TRUE(result.locally_valid());
    EXPECT_EQ(result.dispatch_state,
              core::LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown);
    EXPECT_EQ(result.known_result,
              core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
    ASSERT_FALSE(fake->commands.empty());
    EXPECT_EQ(fake->commands.back(), expected_command);
  }
  const auto calls_before = fake->commands.size();
  auto forbidden = port.Dispatch(primitive_intent("xarm.shutdown_controller"),
                                 std::stop_token{});
  EXPECT_EQ(forbidden.dispatch_state,
            core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  EXPECT_EQ(fake->commands.size(), calls_before);

  std::stop_source cancelled;
  cancelled.request_stop();
  auto cancelled_result =
      port.Dispatch(primitive_intent(std::string(kXArmEnableMotionPrimitiveId)),
                    cancelled.get_token());
  EXPECT_EQ(cancelled_result.dispatch_state,
            core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  EXPECT_EQ(fake->commands.size(), calls_before);

  fake->next_result = {XArmLifecycleResultState::kOutcomeUnknownAfterDispatch,
                       true,
                       true,
                       -1,
                       "lost_response",
                       11U,
                       false};
  const auto uncertain =
      port.Dispatch(primitive_intent(std::string(kXArmEnableMotionPrimitiveId)),
                    std::stop_token{});
  EXPECT_EQ(uncertain.dispatch_state,
            core::LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain);
  EXPECT_EQ(uncertain.effect, core::LifecyclePrimitiveEffectDTO::kPossible);
  EXPECT_FALSE(uncertain.known_result.has_value());
}

} // namespace
} // namespace manyforge::xarm_control
