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
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
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

// The second call captures its report, then waits for explicit release.
// Overlapping transport reads are recorded; barriers require no sleeps.
class StagedSupervisedSession final : public ILite6BrandLifecycleTransport {
public:
  explicit StagedSupervisedSession(std::vector<XArmObservedState> reports)
      : reports_(std::move(reports)) {}

  [[nodiscard]] XArmObservedState observe(std::int64_t now_ns) const override {
    XArmObservedState result;
    std::size_t call;
    {
      std::lock_guard lock(mutex_);
      call = observe_calls_++;
      result = reports_.at(call);
      ++active_observe_calls_;
      concurrent_observe_detected_ = concurrent_observe_detected_ ||
                                     active_observe_calls_ > 1U;
    }
    result.source_timestamp_ns = now_ns - 3;
    result.transport_owner_received_timestamp_ns = now_ns - 2;
    result.fresh_until_ns = now_ns + 1'000'000'000;
    if (call == 1U) {
      captured_.count_down();
      release_.wait();
    }
    {
      std::lock_guard lock(mutex_);
      --active_observe_calls_;
    }
    return result;
  }

  XArmLifecycleCommandResult execute(XArmLifecycleCommand, std::string_view,
                                     std::uint64_t expected_generation,
                                     std::int64_t, std::stop_token) override {
    return {XArmLifecycleResultState::kResponseReceived,
            true,
            true,
            0,
            "accepted",
            expected_generation,
            false};
  }

  void wait_until_captured() const { captured_.wait(); }
  void release() const { release_.count_down(); }
  [[nodiscard]] bool concurrent_observe_detected() const {
    std::lock_guard lock(mutex_);
    return concurrent_observe_detected_;
  }

private:
  std::vector<XArmObservedState> reports_;
  mutable std::mutex mutex_;
  mutable std::size_t observe_calls_ = 0U;
  mutable std::size_t active_observe_calls_ = 0U;
  mutable bool concurrent_observe_detected_ = false;
  mutable std::latch captured_{1};
  mutable std::latch release_{1};
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
  result.error_code = 0;
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
  // Catalog facts are normalized, never raw vendor numbers; raw mode/state
  // appear only as diagnostic vendor-code facts.
  EXPECT_TRUE(std::all_of(
      disabled.value().facts.begin(), disabled.value().facts.end(),
      [](const auto &fact) {
        return fact.lifecycle_fact_id.value().rfind("xarm.", 0U) != 0U ||
               fact.value.kind != core::LifecycleFactValueKindDTO::kInteger;
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
  EXPECT_EQ(observation.value().observed_state,
            core::ComponentLifecycleStateDTO::kDisabled);
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

[[nodiscard]] std::string
cause_class(const core::BrandLifecycleObservationDTO &observation) {
  const auto *fact =
      find_fact(observation, core::kLifecycleBlockingCauseFactId);
  if (fact == nullptr ||
      fact->value.kind != core::LifecycleFactValueKindDTO::kToken ||
      !fact->value.token_value.has_value()) {
    return {};
  }
  return *fact->value.token_value;
}

TEST(Lite6BrandLifecyclePort, FaultCauseClassPrecedesDisabledAndIsSticky) {
  core::FakeClock clock;
  clock.set(10'000);
  auto transport = std::make_unique<FakeSupervisedSession>();
  auto *fake = transport.get();
  fake->observation = disabled_observation();
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));

  auto observation = port.Observe(observation_query());
  ASSERT_TRUE(observation.ok()) << observation.error().message;
  EXPECT_EQ(observation.value().observed_state,
            core::ComponentLifecycleStateDTO::kDisabled);
  EXPECT_EQ(cause_class(observation.value()), "none");

  // A stopped drive-disabled arm with a vendor error keeps lifecycle state
  // Disabled (Recover requires it) but its cause class is fault. A positive
  // error code without a more specific documented meaning is a fault.
  fake->observation.fault_present = FactState::kTrue;
  fake->observation.recovery_required = FactState::kTrue;
  fake->observation.error_code = 22;
  ++fake->observation.generation;
  observation = port.Observe(observation_query());
  ASSERT_TRUE(observation.ok()) << observation.error().message;
  EXPECT_EQ(observation.value().observed_state,
            core::ComponentLifecycleStateDTO::kDisabled);
  EXPECT_EQ(cause_class(observation.value()), "fault");

  // A connected owner without a vendor report cannot clear the fault.
  auto unreported = disabled_observation();
  unreported.generation = fake->observation.generation + 1U;
  unreported.motion_enabled = FactState::kUnknown;
  unreported.fault_present = FactState::kUnknown;
  unreported.recovery_required = FactState::kUnknown;
  unreported.vendor_control_ready = FactState::kUnknown;
  unreported.blocking_reason_codes = {"xarm_report_unavailable"};
  fake->observation = unreported;
  observation = port.Observe(observation_query());
  ASSERT_TRUE(observation.ok()) << observation.error().message;
  EXPECT_EQ(observation.value().observed_state,
            core::ComponentLifecycleStateDTO::kDegraded);
  EXPECT_EQ(cause_class(observation.value()), "fault");

  // Fresh clean vendor status clears it; a later report loss is availability.
  fake->observation = disabled_observation();
  fake->observation.generation = unreported.generation + 1U;
  observation = port.Observe(observation_query());
  ASSERT_TRUE(observation.ok()) << observation.error().message;
  EXPECT_EQ(cause_class(observation.value()), "none");
  unreported.generation = fake->observation.generation + 1U;
  fake->observation = unreported;
  observation = port.Observe(observation_query());
  ASSERT_TRUE(observation.ok()) << observation.error().message;
  EXPECT_EQ(cause_class(observation.value()), "availability");

  // A vendor warning is not classified without qualification.
  fake->observation = disabled_observation();
  fake->observation.generation = unreported.generation + 1U;
  fake->observation.warning_code = 17;
  observation = port.Observe(observation_query());
  ASSERT_TRUE(observation.ok()) << observation.error().message;
  EXPECT_EQ(observation.value().observed_state,
            core::ComponentLifecycleStateDTO::kDisabled);
  EXPECT_EQ(cause_class(observation.value()), "unknown");
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


[[nodiscard]] XArmObservedState active_observation() {
  auto result = disabled_observation();
  result.motion_enabled = FactState::kTrue;
  result.vendor_control_ready = FactState::kTrue;
  result.mode = 1;
  result.state = 2;
  return result;
}

// Token value, or empty when the fact is absent or not a token.
[[nodiscard]] std::string
token_fact(const core::BrandLifecycleObservationDTO &observation,
           std::string_view id) {
  const auto *fact = find_fact(observation, id);
  if (fact == nullptr ||
      fact->value.kind != core::LifecycleFactValueKindDTO::kToken) {
    return {};
  }
  return fact->value.token_value.value_or(std::string());
}

// Integer value, or -1 when the fact is absent or not an integer.
[[nodiscard]] std::int64_t
integer_fact(const core::BrandLifecycleObservationDTO &observation,
             std::string_view id) {
  const auto *fact = find_fact(observation, id);
  if (fact == nullptr ||
      fact->value.kind != core::LifecycleFactValueKindDTO::kInteger) {
    return -1;
  }
  return fact->value.integer_value.value_or(-1);
}

// Observes the fake's current report after bumping its generation.
[[nodiscard]] core::BrandLifecycleObservationDTO
observe_changed(Lite6BrandLifecyclePort &port, FakeSupervisedSession &fake) {
  ++fake.observation.generation;
  auto observation = port.Observe(observation_query());
  EXPECT_TRUE(observation.ok()) << observation.error().message;
  EXPECT_TRUE(observation.value().locally_valid());
  return observation.value();
}

[[nodiscard]] core::BrandLifecyclePrimitiveResultDTO
dispatch(Lite6BrandLifecyclePort &port, std::string_view primitive) {
  return port.Dispatch(primitive_intent(std::string(primitive)),
                       std::stop_token{});
}

TEST(XArmBrandLifecyclePort, ClassifiesUFactoryControllerErrorCodes) {
  core::FakeClock clock;
  clock.set(10'000);
  auto transport = std::make_unique<FakeSupervisedSession>();
  auto *fake = transport.get();
  fake->observation = disabled_observation();
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));

  auto observation = observe_changed(port, *fake);
  EXPECT_EQ(cause_class(observation), "none");
  EXPECT_EQ(integer_fact(observation, kVendorCodeModeFactId), 1);
  EXPECT_EQ(integer_fact(observation, kVendorCodeStateFactId), 4);
  EXPECT_EQ(find_fact(observation, kVendorCodeErrorFactId), nullptr);
  EXPECT_EQ(find_fact(observation, kVendorCodeWarningFactId), nullptr);

  struct Row {
    int error_code;
    std::string_view cause;
    bool power_cycle;
  };
  for (const Row row : {Row{1, "emergency_stop", false},
                        Row{2, "emergency_stop", false},
                        Row{3, "emergency_stop", false},
                        Row{31, "collision", false},
                        Row{10, "fault", true},
                        Row{12, "fault", true},
                        Row{17, "fault", true},
                        Row{9, "fault", false},
                        Row{18, "fault", false},
                        Row{22, "fault", false}}) {
    SCOPED_TRACE(row.error_code);
    fake->observation = disabled_observation();
    fake->observation.generation = observation.observation_generation;
    fake->observation.mode = 0;
    fake->observation.fault_present = FactState::kTrue;
    fake->observation.recovery_required = FactState::kTrue;
    fake->observation.error_code = row.error_code;
    observation = observe_changed(port, *fake);
    // Fault precedence: the drive-disabled stopped arm stays Disabled.
    EXPECT_EQ(observation.observed_state,
              core::ComponentLifecycleStateDTO::kDisabled);
    EXPECT_EQ(cause_class(observation), row.cause);
    EXPECT_EQ(token_fact(observation, kVendorCodeErrorFactId),
              "C" + std::to_string(row.error_code));
    EXPECT_EQ(integer_fact(observation, kVendorCodeModeFactId), 0);
    EXPECT_EQ(integer_fact(observation, kVendorCodeStateFactId), 4);
    if (row.power_cycle) {
      EXPECT_EQ(token_fact(observation, kVendorCodePowerCycleFactId),
                std::string(kPowerCycleRequiredToken));
    } else {
      EXPECT_EQ(find_fact(observation, kVendorCodePowerCycleFactId), nullptr);
    }
  }

  // A real Lite 6 (firmware 2.7.0) reported C2 with fault flags; the error
  // code alone already establishes the class.
  fake->observation = disabled_observation();
  fake->observation.generation = observation.observation_generation;
  fake->observation.error_code = 2;
  observation = observe_changed(port, *fake);
  EXPECT_EQ(cause_class(observation), "emergency_stop");

  // Emergency stop survives report loss; vendor codes are not invented.
  fake->observation.motion_enabled = FactState::kUnknown;
  fake->observation.fault_present = FactState::kUnknown;
  fake->observation.recovery_required = FactState::kUnknown;
  fake->observation.error_code = -1;
  fake->observation.warning_code = -1;
  fake->observation.mode = -1;
  fake->observation.state = -1;
  fake->observation.blocking_reason_codes = {"xarm_report_unavailable"};
  observation = observe_changed(port, *fake);
  EXPECT_EQ(cause_class(observation), "emergency_stop");
  EXPECT_EQ(find_fact(observation, kVendorCodeErrorFactId), nullptr);
  EXPECT_EQ(find_fact(observation, kVendorCodeModeFactId), nullptr);

  // Warning only: not classified (no documented protective-stop code).
  fake->observation = disabled_observation();
  fake->observation.generation = observation.observation_generation;
  fake->observation.warning_code = 23;
  observation = observe_changed(port, *fake);
  EXPECT_EQ(cause_class(observation), "unknown");
  EXPECT_EQ(integer_fact(observation, kVendorCodeWarningFactId), 23);
}

TEST(XArmBrandLifecyclePort, RefusesErrorClearingBeforeServoFaultPowerCycle) {
  core::FakeClock clock;
  clock.set(10'000);
  auto transport = std::make_unique<FakeSupervisedSession>();
  auto *fake = transport.get();
  fake->observation = disabled_observation();
  fake->observation.fault_present = FactState::kTrue;
  fake->observation.recovery_required = FactState::kTrue;
  fake->observation.error_code = 14;
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));
  static_cast<void>(observe_changed(port, *fake));

  const auto refused = dispatch(port, kXArmClearErrorPrimitiveId);
  EXPECT_EQ(refused.dispatch_state,
            core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->details.at("reason"), "power_cycle_required");
  EXPECT_TRUE(fake->commands.empty());

  // Another error code may be cleared.
  fake->observation.error_code = 22;
  static_cast<void>(observe_changed(port, *fake));
  const auto cleared = dispatch(port, kXArmClearErrorPrimitiveId);
  EXPECT_EQ(cleared.known_result,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(fake->commands.size(), 1U);
}

TEST(XArmBrandLifecyclePort,
     ConcurrentReadsPreserveOwnModeChangeAndAllowDispatchDuringCapture) {
  core::FakeClock clock;
  clock.set(10'000);
  auto baseline = disabled_observation();
  baseline.mode = 0;
  auto older = baseline;
  ++older.generation;
  ++older.transport_owner_progress_sequence;
  auto newer = older;
  ++newer.generation;
  ++newer.transport_owner_progress_sequence;
  newer.mode = 1;
  newer.state = 5;
  auto next = newer;
  ++next.generation;
  ++next.transport_owner_progress_sequence;
  auto transport = std::make_unique<StagedSupervisedSession>(
      std::vector{baseline, older, newer, next});
  auto *fake = transport.get();
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));
  ASSERT_TRUE(port.Observe(observation_query()).ok());

  std::optional<core::Result<core::BrandLifecycleObservationDTO>> delayed;
  std::jthread observer([&] { delayed = port.Observe(observation_query()); });
  fake->wait_until_captured();
  std::latch concurrent_started{1};
  std::optional<core::Result<core::BrandLifecycleObservationDTO>> latest;
  std::jthread concurrent_observer([&] {
    concurrent_started.count_down();
    latest = port.Observe(observation_query());
  });
  concurrent_started.wait();
  // Dispatch must complete while the observation cache copy is held. The
  // separate classification lock remains available to this own primitive.
  const auto stopped = dispatch(port, kXArmSetStoppedStatePrimitiveId);
  const auto requested = dispatch(port, kXArmSelectJointServoModePrimitiveId);
  fake->release();
  observer.join();
  concurrent_observer.join();

  ASSERT_EQ(stopped.known_result,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(requested.known_result,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(latest.has_value());
  ASSERT_TRUE(latest->ok()) << latest->error().message;
  EXPECT_EQ(cause_class(latest->value()), "none");
  ASSERT_TRUE(delayed.has_value());
  ASSERT_TRUE(delayed->ok()) << delayed->error().message;
  EXPECT_EQ(cause_class(delayed->value()), "none");
  EXPECT_FALSE(fake->concurrent_observe_detected());
  const auto following = port.Observe(observation_query());
  ASSERT_TRUE(following.ok()) << following.error().message;
  EXPECT_EQ(cause_class(following.value()), "none");
}

TEST(XArmBrandLifecyclePort,
     ConcurrentReadsPreserveFaultAndPowerCycleRequirement) {
  core::FakeClock clock;
  clock.set(10'000);
  auto baseline = disabled_observation();
  auto older = baseline;
  ++older.generation;
  ++older.transport_owner_progress_sequence;
  auto fault = older;
  ++fault.generation;
  ++fault.transport_owner_progress_sequence;
  fault.error_code = 14;
  fault.fault_present = FactState::kTrue;
  fault.recovery_required = FactState::kTrue;
  auto unavailable = fault;
  ++unavailable.generation;
  ++unavailable.transport_owner_progress_sequence;
  unavailable.blocking_reason_codes = {"xarm_report_unavailable"};
  unavailable.error_code = -1;
  unavailable.mode = -1;
  unavailable.state = -1;
  unavailable.motion_enabled = FactState::kUnknown;
  unavailable.fault_present = FactState::kUnknown;
  unavailable.recovery_required = FactState::kUnknown;
  unavailable.vendor_control_ready = FactState::kUnknown;
  auto transport = std::make_unique<StagedSupervisedSession>(
      std::vector{baseline, older, fault, unavailable});
  auto *fake = transport.get();
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));
  ASSERT_TRUE(port.Observe(observation_query()).ok());

  std::optional<core::Result<core::BrandLifecycleObservationDTO>> delayed;
  std::jthread observer([&] { delayed = port.Observe(observation_query()); });
  fake->wait_until_captured();
  std::latch concurrent_started{1};
  std::optional<core::Result<core::BrandLifecycleObservationDTO>> latest;
  std::jthread concurrent_observer([&] {
    concurrent_started.count_down();
    latest = port.Observe(observation_query());
  });
  concurrent_started.wait();
  fake->release();
  observer.join();
  concurrent_observer.join();

  ASSERT_TRUE(latest.has_value());
  ASSERT_TRUE(latest->ok()) << latest->error().message;
  EXPECT_EQ(cause_class(latest->value()), "fault");
  ASSERT_TRUE(delayed.has_value());
  ASSERT_TRUE(delayed->ok()) << delayed->error().message;
  EXPECT_EQ(cause_class(delayed->value()), "none");
  EXPECT_FALSE(fake->concurrent_observe_detected());
  const auto following = port.Observe(observation_query());
  ASSERT_TRUE(following.ok()) << following.error().message;
  EXPECT_EQ(cause_class(following.value()), "fault");
  const auto refused = dispatch(port, kXArmClearErrorPrimitiveId);
  EXPECT_EQ(refused.dispatch_state,
            core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->details.at("reason"), "power_cycle_required");
}

TEST(XArmBrandLifecyclePort,
     RejectsRegressedOwnerPublicationWithoutRejectingSemanticRefreshes) {
  core::FakeClock clock;
  clock.set(10'000);
  auto transport = std::make_unique<FakeSupervisedSession>();
  auto *fake = transport.get();
  fake->observation = disabled_observation();
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));
  ASSERT_TRUE(port.Observe(observation_query()).ok());
  // Re-reading the same publication and a newer heartbeat are both valid;
  // neither requires a new semantic generation.
  clock.advance(100);
  ASSERT_TRUE(port.Observe(observation_query()).ok());
  ++fake->observation.transport_owner_progress_sequence;
  ASSERT_TRUE(port.Observe(observation_query()).ok());
  --fake->observation.transport_owner_progress_sequence;
  const auto regressed = port.Observe(observation_query());
  ASSERT_FALSE(regressed.ok());
  EXPECT_EQ(regressed.error().details.at("reason"),
            "owner_publication_sequence_regressed");
  ++fake->observation.transport_owner_progress_sequence;
  // An invalid future-epoch sample must not advance either watermark.
  const auto valid = fake->observation;
  fake->observation.transport_owner_component_epoch = "unselected.owner";
  ++fake->observation.generation;
  ++fake->observation.transport_owner_progress_sequence;
  EXPECT_FALSE(port.Observe(observation_query()).ok());
  fake->observation = valid;
  EXPECT_TRUE(port.Observe(observation_query()).ok());
}

TEST(XArmBrandLifecyclePort,
     RegressedSourceCannotClearVendorFaultHistory) {
  core::FakeClock clock;
  clock.set(10'000);
  auto transport = std::make_unique<FakeSupervisedSession>();
  auto *fake = transport.get();
  auto fault = disabled_observation();
  ++fault.generation;
  fault.error_code = 14;
  fault.fault_present = FactState::kTrue;
  fault.recovery_required = FactState::kTrue;
  fake->observation = fault;
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));
  ASSERT_TRUE(port.Observe(observation_query()).ok());

  fake->observation = disabled_observation();
  ++fake->observation.transport_owner_progress_sequence;
  const auto regressed = port.Observe(observation_query());
  EXPECT_FALSE(regressed.ok());
  if (!regressed.ok()) {
    EXPECT_EQ(regressed.error().details.at("reason"),
              "observation_generation_regressed");
  }

  fake->observation.generation = fault.generation + 1U;
  ++fake->observation.transport_owner_progress_sequence;
  fake->observation.blocking_reason_codes = {"xarm_report_unavailable"};
  fake->observation.error_code = -1;
  fake->observation.fault_present = FactState::kUnknown;
  fake->observation.recovery_required = FactState::kUnknown;
  const auto unavailable = port.Observe(observation_query());
  ASSERT_TRUE(unavailable.ok()) << unavailable.error().message;
  EXPECT_EQ(cause_class(unavailable.value()), "fault");
  const auto refused = dispatch(port, kXArmClearErrorPrimitiveId);
  EXPECT_EQ(refused.dispatch_state,
            core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  EXPECT_TRUE(fake->commands.empty());
}

TEST(XArmBrandLifecyclePort,
     InitialReportAvailabilityDoesNotMaskGenuineStartupFaults) {
  core::FakeClock clock;
  clock.set(10'000);
  auto transport = std::make_unique<FakeSupervisedSession>();
  auto *fake = transport.get();
  fake->observation = disabled_observation();
  fake->observation.blocking_reason_codes = {"xarm_report_unavailable"};
  fake->observation.motion_enabled = FactState::kUnknown;
  fake->observation.fault_present = FactState::kUnknown;
  fake->observation.recovery_required = FactState::kUnknown;
  fake->observation.vendor_control_ready = FactState::kUnknown;
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));
  auto initial = port.Observe(observation_query());
  ASSERT_TRUE(initial.ok()) << initial.error().message;
  EXPECT_EQ(cause_class(initial.value()), "availability");

  fake->observation = disabled_observation();
  fake->observation.generation = initial.value().observation_generation + 1U;
  fake->observation.error_code = 22;
  fake->observation.fault_present = FactState::kTrue;
  fake->observation.recovery_required = FactState::kTrue;
  auto fault = port.Observe(observation_query());
  ASSERT_TRUE(fault.ok()) << fault.error().message;
  EXPECT_EQ(cause_class(fault.value()), "fault");
}

TEST(XArmBrandLifecyclePort, OwnPrimitiveEffectsAreNeverExternalControl) {
  core::FakeClock clock;
  clock.set(10'000);
  auto transport = std::make_unique<FakeSupervisedSession>();
  auto *fake = transport.get();
  fake->observation = disabled_observation();
  fake->observation.mode = 0;
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "none");

  // Prepare: enable drives (state stays 4), then select joint-servo mode
  // with the vendor's intermediate mode-1/state-4 report.
  ASSERT_EQ(dispatch(port, kXArmEnableMotionPrimitiveId).known_result,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  fake->observation.motion_enabled = FactState::kTrue;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "none");
  ASSERT_EQ(dispatch(port, kXArmSelectJointServoModePrimitiveId).known_result,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  fake->observation.mode = 1;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "none");
  fake->observation.state = 5;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "none");

  // Enable: ready state; then own ros2_control motion alternates 1 and 2.
  ASSERT_EQ(dispatch(port, kXArmSetReadyStatePrimitiveId).known_result,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  fake->observation.state = 2;
  fake->observation.vendor_control_ready = FactState::kTrue;
  auto observation = observe_changed(port, *fake);
  EXPECT_EQ(observation.observed_state,
            core::ComponentLifecycleStateDTO::kActive);
  EXPECT_EQ(cause_class(observation), "none");
  clock.set(clock.now() + 60'000'000'000LL);
  fake->observation.state = 1;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "none");
  fake->observation.state = 2;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "none");

  // Disable: stopped state through deceleration, then drives off.
  ASSERT_EQ(dispatch(port, kXArmSetStoppedStatePrimitiveId).known_result,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  fake->observation.state = 6;
  fake->observation.vendor_control_ready = FactState::kFalse;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "none");
  fake->observation.state = 4;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "none");
  ASSERT_EQ(dispatch(port, kXArmDisableMotionPrimitiveId).known_result,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  fake->observation.motion_enabled = FactState::kFalse;
  fake->observation.mode = 0;
  observation = observe_changed(port, *fake);
  EXPECT_EQ(observation.observed_state,
            core::ComponentLifecycleStateDTO::kDisabled);
  EXPECT_EQ(cause_class(observation), "none");

  // An own primitive whose outcome is unknown still explains its effect.
  fake->next_result = {XArmLifecycleResultState::kOutcomeUnknownAfterDispatch,
                       true,
                       true,
                       -1,
                       "lost_response",
                       0U,
                       false};
  ASSERT_EQ(dispatch(port, kXArmEnableMotionPrimitiveId).dispatch_state,
            core::LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain);
  fake->observation.motion_enabled = FactState::kTrue;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "none");
}

TEST(XArmBrandLifecyclePort, UnrequestedModeOrStateChangeIsStickyExternalControl) {
  core::FakeClock clock;
  clock.set(10'000);
  auto transport = std::make_unique<FakeSupervisedSession>();
  auto *fake = transport.get();
  fake->observation = active_observation();
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "none");

  // A vendor-UI jog leaves position mode / mode-changed state without a
  // ManyForge request.
  fake->observation.mode = 0;
  fake->observation.state = 5;
  fake->observation.vendor_control_ready = FactState::kFalse;
  auto observation = observe_changed(port, *fake);
  EXPECT_EQ(cause_class(observation), "external_control");
  EXPECT_EQ(integer_fact(observation, kVendorCodeModeFactId), 0);
  EXPECT_EQ(integer_fact(observation, kVendorCodeStateFactId), 5);

  // Sticky across report loss and while the status stays unexplained,
  // and even when the robot is returned to the explained pair.
  auto unreported = fake->observation;
  unreported.motion_enabled = FactState::kUnknown;
  unreported.fault_present = FactState::kUnknown;
  unreported.recovery_required = FactState::kUnknown;
  unreported.mode = -1;
  unreported.state = -1;
  unreported.blocking_reason_codes = {"xarm_report_unavailable"};
  const auto reported = fake->observation;
  fake->observation = unreported;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "external_control");
  fake->observation = reported;
  fake->observation.generation = unreported.generation;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "external_control");
  fake->observation.mode = 1;
  fake->observation.state = 2;
  fake->observation.vendor_control_ready = FactState::kTrue;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "external_control");
  fake->observation.mode = 0;
  fake->observation.state = 5;
  fake->observation.vendor_control_ready = FactState::kFalse;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "external_control");

  // A requested Disable (stop direction) does not acknowledge it.
  ASSERT_EQ(dispatch(port, kXArmSetStoppedStatePrimitiveId).known_result,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  fake->observation.state = 4;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "external_control");

  // The next non-stop primitive (dispatched by CCLC only after the
  // operator's acknowledgement) re-baselines from the fresh report.
  ASSERT_EQ(dispatch(port, kXArmSelectJointServoModePrimitiveId).known_result,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  fake->observation.mode = 1;
  fake->observation.state = 5;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "none");
  ASSERT_EQ(dispatch(port, kXArmSetReadyStatePrimitiveId).known_result,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  fake->observation.state = 2;
  fake->observation.vendor_control_ready = FactState::kTrue;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "none");

  // A stop ManyForge did not request, after the own primitive settled.
  clock.set(clock.now() + 6'000'000'000LL);
  fake->observation.state = 4;
  fake->observation.vendor_control_ready = FactState::kFalse;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "external_control");
}

TEST(XArmBrandLifecyclePort, ModesManyForgeNeverSelectsAreExternalControl) {
  for (const int mode : {2, 4}) {
    SCOPED_TRACE(mode);
    core::FakeClock clock;
    clock.set(10'000);
    auto transport = std::make_unique<FakeSupervisedSession>();
    auto *fake = transport.get();
    fake->observation = disabled_observation();
    fake->observation.mode = mode;
    Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));
    // Even on the first report, and even while an own primitive is in
    // flight or settling.
    EXPECT_EQ(cause_class(observe_changed(port, *fake)), "external_control");
    ASSERT_EQ(dispatch(port, kXArmEnableMotionPrimitiveId).known_result,
              core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
    EXPECT_EQ(cause_class(observe_changed(port, *fake)), "external_control");
  }
}

TEST(XArmBrandLifecyclePort, ErrorClassPrecedesExternalControl) {
  core::FakeClock clock;
  clock.set(10'000);
  auto transport = std::make_unique<FakeSupervisedSession>();
  auto *fake = transport.get();
  fake->observation = active_observation();
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "none");

  // An emergency stop reports mode 0 / state 4: the error explains it.
  fake->observation.mode = 0;
  fake->observation.state = 4;
  fake->observation.motion_enabled = FactState::kFalse;
  fake->observation.vendor_control_ready = FactState::kFalse;
  fake->observation.fault_present = FactState::kTrue;
  fake->observation.recovery_required = FactState::kTrue;
  fake->observation.error_code = 2;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "emergency_stop");

  // Recover clears the error from the explained stopped pair.
  ASSERT_EQ(dispatch(port, kXArmClearErrorPrimitiveId).known_result,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  fake->observation.fault_present = FactState::kFalse;
  fake->observation.recovery_required = FactState::kFalse;
  fake->observation.error_code = 0;
  EXPECT_EQ(cause_class(observe_changed(port, *fake)), "none");
}

TEST(XArmBrandLifecyclePort, NonZeroVendorReturnIsAnUnknownOutcome) {
  core::FakeClock clock;
  clock.set(10'000);
  auto transport = std::make_unique<FakeSupervisedSession>();
  auto *fake = transport.get();
  fake->observation = disabled_observation();
  Lite6BrandLifecyclePort port(port_config(), clock, std::move(transport));

  fake->next_result = {XArmLifecycleResultState::kResponseReceived,
                       true,
                       true,
                       9,
                       "vendor_rejected",
                       0U,
                       false};
  const auto rejected = dispatch(port, kXArmEnableMotionPrimitiveId);
  ASSERT_TRUE(rejected.locally_valid());
  EXPECT_EQ(rejected.dispatch_state,
            core::LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain);
  EXPECT_EQ(rejected.effect, core::LifecyclePrimitiveEffectDTO::kPossible);
  EXPECT_FALSE(rejected.known_result.has_value());
  ASSERT_TRUE(rejected.error.has_value());
  EXPECT_EQ(rejected.error->code, "xarm_lifecycle_primitive_outcome_unknown");
  EXPECT_EQ(rejected.error->details.at("reason"),
            "vendor_return_code_9:vendor_rejected");
  EXPECT_EQ(fake->commands.size(), 1U);

  // Attempted but not permitted is equally unknown, never a known failure.
  fake->next_result.permitted = false;
  fake->next_result.return_code = 0;
  const auto unpermitted = dispatch(port, kXArmEnableMotionPrimitiveId);
  EXPECT_EQ(unpermitted.dispatch_state,
            core::LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain);

  // Not attempted by the owner remains not dispatched.
  fake->next_result.attempted = false;
  const auto refused = dispatch(port, kXArmEnableMotionPrimitiveId);
  EXPECT_EQ(refused.dispatch_state,
            core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
}

} // namespace
} // namespace manyforge::xarm_control
