#include <manyforge_lite6_product/lifecycle_profile.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::lite6_product {
namespace {

using core::ComponentLifecycleIntentKindDTO;
using core::ComponentLifecycleStateDTO;
using core::LifecycleFactId;
using core::LifecycleFactPredicateDTO;
using core::LifecycleFactValueDTO;
using core::LifecycleFactValueKindDTO;
using core::LifecycleIntentPlanDTO;
using core::LifecyclePredicateDTO;
using core::LifecyclePrimitiveId;

constexpr std::int64_t kPlanTimeoutNs = 30'000'000'000LL;

[[nodiscard]] LifecycleFactValueDTO boolean_value(const bool value) {
  return {LifecycleFactValueKindDTO::kBoolean, value, std::nullopt,
          std::nullopt};
}

[[nodiscard]] LifecycleFactValueDTO token_value(std::string value) {
  return {LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt,
          std::move(value)};
}

[[nodiscard]] LifecycleFactPredicateDTO boolean_fact(const std::string_view id,
                                                     const bool value) {
  return {LifecycleFactId(std::string(id)), boolean_value(value)};
}

[[nodiscard]] LifecycleFactPredicateDTO token_fact(const std::string_view id,
                                                   std::string value) {
  return {LifecycleFactId(std::string(id)), token_value(std::move(value))};
}

[[nodiscard]] LifecyclePredicateDTO
predicate(std::vector<ComponentLifecycleStateDTO> states,
          std::vector<LifecycleFactPredicateDTO> facts) {
  std::sort(states.begin(), states.end());
  std::sort(facts.begin(), facts.end(),
            [](const auto &left, const auto &right) {
              return left.lifecycle_fact_id < right.lifecycle_fact_id;
            });
  return {std::move(states), std::move(facts)};
}

[[nodiscard]] LifecyclePredicateDTO prepare_requested() {
  return predicate(
      {ComponentLifecycleStateDTO::kPrepared},
      {
          boolean_fact(kMotionEnabledFactId, true),
          boolean_fact(kPositionValidFactId, true),
          boolean_fact(kJointServoModeSelectedFactId, true),
          boolean_fact(kVendorControlReadyFactId, false),
          boolean_fact(kVendorStoppedFactId, false),
          boolean_fact(kFaultPresentFactId, false),
          boolean_fact(kRecoveryRequiredFactId, false),
          boolean_fact(kWarningPresentFactId, false),
          token_fact(kVendorFeedbackClassFactId, "mode_changed"),
      });
}

[[nodiscard]] LifecyclePredicateDTO drive_enabled_stopped_clean() {
  return predicate({ComponentLifecycleStateDTO::kDegraded},
                   {
                       boolean_fact(kMotionEnabledFactId, true),
                       boolean_fact(kPositionValidFactId, true),
                       boolean_fact(kVendorStoppedFactId, true),
                       boolean_fact(kFaultPresentFactId, false),
                       boolean_fact(kRecoveryRequiredFactId, false),
                       boolean_fact(kWarningPresentFactId, false),
                       token_fact(kVendorFeedbackClassFactId, "stopped"),
                   });
}

[[nodiscard]] LifecyclePredicateDTO active_requested() {
  return predicate({ComponentLifecycleStateDTO::kActive},
                   {
                       boolean_fact(kMotionEnabledFactId, true),
                       boolean_fact(kPositionValidFactId, true),
                       boolean_fact(kJointServoModeSelectedFactId, true),
                       boolean_fact(kVendorControlReadyFactId, true),
                       boolean_fact(kVendorStoppedFactId, false),
                       boolean_fact(kFaultPresentFactId, false),
                       boolean_fact(kRecoveryRequiredFactId, false),
                       boolean_fact(kWarningPresentFactId, false),
                       token_fact(kVendorFeedbackClassFactId, "ready"),
                   });
}

[[nodiscard]] LifecyclePredicateDTO disabled_requested() {
  return predicate({ComponentLifecycleStateDTO::kDisabled},
                   {
                       boolean_fact(kMotionEnabledFactId, false),
                       boolean_fact(kVendorControlReadyFactId, false),
                       boolean_fact(kVendorStoppedFactId, true),
                       token_fact(kVendorFeedbackClassFactId, "stopped"),
                   });
}

[[nodiscard]] LifecyclePredicateDTO recover_requested() {
  return predicate({ComponentLifecycleStateDTO::kDisabled},
                   {
                       boolean_fact(kMotionEnabledFactId, false),
                       boolean_fact(kVendorStoppedFactId, true),
                       boolean_fact(kFaultPresentFactId, false),
                       boolean_fact(kRecoveryRequiredFactId, false),
                       boolean_fact(kWarningPresentFactId, false),
                       token_fact(kVendorFeedbackClassFactId, "stopped"),
                   });
}

[[nodiscard]] LifecycleIntentPlanDTO prepare_plan() {
  const auto prepared = prepare_requested();
  const auto enabled_stopped = drive_enabled_stopped_clean();
  return {ComponentLifecycleIntentKindDTO::kPrepare,
          ComponentLifecycleStateDTO::kPrepared,
          std::nullopt,
          prepared,
          kPlanTimeoutNs,
          {{LifecyclePrimitiveId(std::string(kEnableMotionPrimitiveId)),
            predicate({ComponentLifecycleStateDTO::kDeclared,
                       ComponentLifecycleStateDTO::kDisabled},
                      {boolean_fact(kMotionEnabledFactId, false),
                       boolean_fact(kVendorStoppedFactId, true),
                       boolean_fact(kFaultPresentFactId, false),
                       boolean_fact(kRecoveryRequiredFactId, false),
                       boolean_fact(kWarningPresentFactId, false),
                       token_fact(kVendorFeedbackClassFactId, "stopped")}),
            enabled_stopped},
           {LifecyclePrimitiveId(
                std::string(kSelectJointServoModePrimitiveId)),
            enabled_stopped, prepared}}};
}

[[nodiscard]] LifecycleIntentPlanDTO enable_plan() {
  auto eligibility = prepare_requested();
  eligibility.fact_predicates.push_back(
      boolean_fact(kReadyStateRequestEligibleFactId, true));
  std::sort(eligibility.fact_predicates.begin(),
            eligibility.fact_predicates.end(),
            [](const auto &left, const auto &right) {
              return left.lifecycle_fact_id < right.lifecycle_fact_id;
            });
  return {ComponentLifecycleIntentKindDTO::kEnable,
          ComponentLifecycleStateDTO::kActive,
          std::nullopt,
          active_requested(),
          kPlanTimeoutNs,
          {{LifecyclePrimitiveId(std::string(kSetReadyStatePrimitiveId)),
            std::move(eligibility), active_requested()}}};
}

[[nodiscard]] LifecycleIntentPlanDTO disable_plan() {
  const auto stopped_enabled = predicate(
      {ComponentLifecycleStateDTO::kDegraded,
       ComponentLifecycleStateDTO::kFaulted},
      {boolean_fact(kMotionEnabledFactId, true),
       boolean_fact(kVendorControlReadyFactId, false),
       boolean_fact(kVendorStoppedFactId, true),
       token_fact(kVendorFeedbackClassFactId, "stopped")});
  return {ComponentLifecycleIntentKindDTO::kDisable,
          ComponentLifecycleStateDTO::kDisabled,
          std::nullopt,
          disabled_requested(),
          kPlanTimeoutNs,
          {{LifecyclePrimitiveId(std::string(kSetStoppedStatePrimitiveId)),
            predicate({ComponentLifecycleStateDTO::kPrepared,
                       ComponentLifecycleStateDTO::kActive,
                       ComponentLifecycleStateDTO::kDegraded,
                       ComponentLifecycleStateDTO::kFaulted},
                      {boolean_fact(kMotionEnabledFactId, true),
                       boolean_fact(kVendorStoppedFactId, false)}),
            stopped_enabled},
           {LifecyclePrimitiveId(std::string(kDisableMotionPrimitiveId)),
            stopped_enabled, disabled_requested()}}};
}

[[nodiscard]] LifecycleIntentPlanDTO recover_plan() {
  const auto target_eligibility = predicate(
      {ComponentLifecycleStateDTO::kDisabled},
      {boolean_fact(kMotionEnabledFactId, false),
       boolean_fact(kVendorStoppedFactId, true),
       token_fact(kVendorFeedbackClassFactId, "stopped")});
  const auto recovered = recover_requested();
  return {ComponentLifecycleIntentKindDTO::kRecover,
          ComponentLifecycleStateDTO::kDisabled,
          target_eligibility,
          recovered,
          kPlanTimeoutNs,
          {{LifecyclePrimitiveId(std::string(kClearErrorPrimitiveId)),
            predicate({ComponentLifecycleStateDTO::kDisabled},
                      {boolean_fact(kMotionEnabledFactId, false),
                       boolean_fact(kVendorStoppedFactId, true),
                       boolean_fact(kFaultPresentFactId, true),
                       token_fact(kVendorFeedbackClassFactId, "stopped")}),
            predicate({ComponentLifecycleStateDTO::kDisabled},
                      {boolean_fact(kMotionEnabledFactId, false),
                       boolean_fact(kVendorStoppedFactId, true),
                       boolean_fact(kFaultPresentFactId, false),
                       boolean_fact(kRecoveryRequiredFactId, false),
                       token_fact(kVendorFeedbackClassFactId, "stopped")})},
           {LifecyclePrimitiveId(std::string(kClearWarningPrimitiveId)),
            predicate({ComponentLifecycleStateDTO::kDisabled},
                      {boolean_fact(kMotionEnabledFactId, false),
                       boolean_fact(kVendorStoppedFactId, true),
                       boolean_fact(kFaultPresentFactId, false),
                       boolean_fact(kRecoveryRequiredFactId, false),
                       boolean_fact(kWarningPresentFactId, true),
                       token_fact(kVendorFeedbackClassFactId, "stopped")}),
            recovered}}};
}

[[nodiscard]] LifecyclePredicateDTO gripper_predicate(
    const core::ComponentLifecycleStateDTO state, const bool enabled,
    const bool fault_present) {
  return predicate(
      {state},
      {boolean_fact("native.gripper.command_admission_enabled", enabled),
       boolean_fact("native.gripper.fault_present", fault_present)});
}

[[nodiscard]] std::vector<LifecycleIntentPlanDTO> gripper_plans() {
  const auto declared = gripper_predicate(
      ComponentLifecycleStateDTO::kDeclared, false, false);
  const auto prepared = gripper_predicate(
      ComponentLifecycleStateDTO::kPrepared, false, false);
  const auto active = gripper_predicate(
      ComponentLifecycleStateDTO::kActive, true, false);
  const auto disabled = gripper_predicate(
      ComponentLifecycleStateDTO::kDisabled, false, false);
  return {
      {ComponentLifecycleIntentKindDTO::kPrepare,
       ComponentLifecycleStateDTO::kPrepared,
       std::nullopt,
       prepared,
       kPlanTimeoutNs,
       {{LifecyclePrimitiveId("native.gripper.prepare"), declared,
         prepared}}},
      {ComponentLifecycleIntentKindDTO::kEnable,
       ComponentLifecycleStateDTO::kActive,
       std::nullopt,
       active,
       kPlanTimeoutNs,
       {{LifecyclePrimitiveId("native.gripper.enable"), prepared, active}}},
      {ComponentLifecycleIntentKindDTO::kDisable,
       ComponentLifecycleStateDTO::kDisabled,
       std::nullopt,
       disabled,
       kPlanTimeoutNs,
       {{LifecyclePrimitiveId("native.gripper.disable"),
         predicate({ComponentLifecycleStateDTO::kPrepared,
                    ComponentLifecycleStateDTO::kActive,
                    ComponentLifecycleStateDTO::kDegraded,
                    ComponentLifecycleStateDTO::kFaulted},
                   {}),
         disabled}}},
      {ComponentLifecycleIntentKindDTO::kRecover,
       ComponentLifecycleStateDTO::kDisabled,
       gripper_predicate(ComponentLifecycleStateDTO::kDisabled, false, true),
       disabled,
       kPlanTimeoutNs,
       {{LifecyclePrimitiveId("native.gripper.clear_fault"),
         gripper_predicate(ComponentLifecycleStateDTO::kDisabled, false, true),
         disabled}}},
  };
}

} // namespace

std::vector<core::LifecycleIntentPlanDTO> make_lite6_lifecycle_plans() {
  return {prepare_plan(), enable_plan(), disable_plan(), recover_plan()};
}

core::Result<core::ContentDigest>
compute_lite6_lifecycle_catalog_semantic_digest() {
  return core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
      core::ProviderManifestId(std::string(kLifecycleProviderManifestId)),
      make_lite6_lifecycle_plans());
}

core::Result<core::BrandLifecyclePlanCatalogDTO>
make_lite6_lifecycle_catalog(
    const core::ProviderManifestDTO &provider_manifest) {
  const core::ProviderManifestId expected_id{
      std::string(kLifecycleProviderManifestId)};
  if (provider_manifest.provider_manifest_id != expected_id) {
    return core::Result<core::BrandLifecyclePlanCatalogDTO>::Err(
        {"lite6_lifecycle_provider_manifest_mismatch",
         "the qualified provider manifest is not the Lite6 lifecycle owner",
         {{"reason", "provider_manifest_id_mismatch"}}});
  }
  auto catalog = core::MakeBrandLifecyclePlanCatalog(
      expected_id, provider_manifest.provider_manifest_revision,
      make_lite6_lifecycle_plans());
  if (!catalog.ok()) {
    return catalog;
  }
  const auto anchored = core::VerifyBrandLifecyclePlanCatalogManifestAnchor(
      provider_manifest, catalog.value());
  if (!anchored.ok()) {
    return core::Result<core::BrandLifecyclePlanCatalogDTO>::Err(
        anchored.error());
  }
  return catalog;
}

core::Result<std::vector<core::ProviderCapabilityOfferDTO>>
make_lite6_lifecycle_capability_offers() {
  try {
    std::vector<core::ProviderCapabilityOfferDTO> capabilities;
    for (const std::string_view capability_id : {
             "component.lifecycle.disable",
             "component.lifecycle.enable",
             "component.lifecycle.observe",
             "component.lifecycle.prepare",
             "component.lifecycle.recover",
         }) {
      const auto *definition =
          core::find_builtin_capability_definition(capability_id);
      if (definition == nullptr) {
        throw std::logic_error("missing built-in Lite6 lifecycle capability");
      }
      capabilities.push_back(
          {core::CapabilityId(std::string(capability_id)),
           core::CapabilityDefinitionRevision(
               std::string(definition->capability_definition_revision)),
           {}});
    }
    return core::Result<std::vector<core::ProviderCapabilityOfferDTO>>::Ok(
        std::move(capabilities));
  } catch (const std::exception &exception) {
    return core::Result<std::vector<core::ProviderCapabilityOfferDTO>>::Err(
        {"lite6_lifecycle_capability_catalog_invalid",
         "the Lite6 lifecycle capability catalog is unavailable",
         {{"reason", exception.what()}}});
  }
}

std::vector<core::LifecycleIntentPlanDTO>
make_lite6_gripper_lifecycle_plans() {
  return gripper_plans();
}

core::Result<core::ContentDigest>
compute_lite6_gripper_lifecycle_catalog_semantic_digest() {
  return core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
      core::ProviderManifestId(
          std::string(kGripperLifecycleProviderManifestId)),
      make_lite6_gripper_lifecycle_plans());
}

core::Result<core::BrandLifecyclePlanCatalogDTO>
make_lite6_gripper_lifecycle_catalog(
    const core::ProviderManifestDTO &provider_manifest) {
  const core::ProviderManifestId expected_id{
      std::string(kGripperLifecycleProviderManifestId)};
  if (provider_manifest.provider_manifest_id != expected_id) {
    return core::Result<core::BrandLifecyclePlanCatalogDTO>::Err(
        {"lite6_gripper_lifecycle_provider_manifest_mismatch",
         "the qualified provider manifest is not the Lite6 gripper owner",
         {{"reason", "provider_manifest_id_mismatch"}}});
  }
  auto catalog = core::MakeBrandLifecyclePlanCatalog(
      expected_id, provider_manifest.provider_manifest_revision,
      make_lite6_gripper_lifecycle_plans());
  if (!catalog.ok()) {
    return catalog;
  }
  const auto anchored = core::VerifyBrandLifecyclePlanCatalogManifestAnchor(
      provider_manifest, catalog.value());
  if (!anchored.ok()) {
    return core::Result<core::BrandLifecyclePlanCatalogDTO>::Err(
        anchored.error());
  }
  return catalog;
}

} // namespace manyforge::lite6_product
