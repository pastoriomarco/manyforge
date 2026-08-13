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

#include <manyforge_xarm_control/lite6_brand_lifecycle_catalog.hpp>

#include <manyforge_xarm_control/generated/lite6_brand_lifecycle_product_identity.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <manyforge_core/types/capability_definition.hpp>

namespace manyforge::xarm_control {
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
using core::LifecyclePrimitiveTransitionDTO;

constexpr std::int64_t kPlanTimeoutNs = 30'000'000'000LL;

[[nodiscard]] LifecycleFactValueDTO boolean_value(bool value) {
  return {LifecycleFactValueKindDTO::kBoolean, value, std::nullopt,
          std::nullopt};
}

[[nodiscard]] LifecycleFactValueDTO token_value(std::string value) {
  return {LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt,
          std::move(value)};
}

[[nodiscard]] LifecycleFactPredicateDTO boolean_fact(std::string_view id,
                                                     bool value) {
  return {LifecycleFactId(std::string(id)), boolean_value(value)};
}

[[nodiscard]] LifecycleFactPredicateDTO token_fact(std::string_view id,
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
          boolean_fact(kXArmMotionEnabledFactId, true),
          boolean_fact(kXArmPositionValidFactId, true),
          boolean_fact(kXArmJointServoModeSelectedFactId, true),
          boolean_fact(kXArmVendorControlReadyFactId, false),
          boolean_fact(kXArmVendorStoppedFactId, false),
          boolean_fact(kXArmFaultPresentFactId, false),
          boolean_fact(kXArmRecoveryRequiredFactId, false),
          boolean_fact(kXArmWarningPresentFactId, false),
          token_fact(kXArmVendorFeedbackClassFactId, "mode_changed"),
      });
}

[[nodiscard]] LifecyclePredicateDTO drive_enabled_stopped_clean() {
  return predicate({ComponentLifecycleStateDTO::kDegraded},
                   {
                       boolean_fact(kXArmMotionEnabledFactId, true),
                       boolean_fact(kXArmPositionValidFactId, true),
                       boolean_fact(kXArmVendorStoppedFactId, true),
                       boolean_fact(kXArmFaultPresentFactId, false),
                       boolean_fact(kXArmRecoveryRequiredFactId, false),
                       boolean_fact(kXArmWarningPresentFactId, false),
                       token_fact(kXArmVendorFeedbackClassFactId, "stopped"),
                   });
}

[[nodiscard]] LifecyclePredicateDTO active_requested() {
  return predicate({ComponentLifecycleStateDTO::kActive},
                   {
                       boolean_fact(kXArmMotionEnabledFactId, true),
                       boolean_fact(kXArmPositionValidFactId, true),
                       boolean_fact(kXArmJointServoModeSelectedFactId, true),
                       boolean_fact(kXArmVendorControlReadyFactId, true),
                       boolean_fact(kXArmVendorStoppedFactId, false),
                       boolean_fact(kXArmFaultPresentFactId, false),
                       boolean_fact(kXArmRecoveryRequiredFactId, false),
                       boolean_fact(kXArmWarningPresentFactId, false),
                       token_fact(kXArmVendorFeedbackClassFactId, "ready"),
                   });
}

[[nodiscard]] LifecyclePredicateDTO disabled_requested() {
  return predicate({ComponentLifecycleStateDTO::kDisabled},
                   {
                       boolean_fact(kXArmMotionEnabledFactId, false),
                       boolean_fact(kXArmVendorControlReadyFactId, false),
                       boolean_fact(kXArmVendorStoppedFactId, true),
                       token_fact(kXArmVendorFeedbackClassFactId, "stopped"),
                   });
}

[[nodiscard]] LifecyclePredicateDTO recover_requested() {
  return predicate({ComponentLifecycleStateDTO::kDisabled},
                   {
                       boolean_fact(kXArmMotionEnabledFactId, false),
                       boolean_fact(kXArmVendorStoppedFactId, true),
                       boolean_fact(kXArmFaultPresentFactId, false),
                       boolean_fact(kXArmRecoveryRequiredFactId, false),
                       boolean_fact(kXArmWarningPresentFactId, false),
                       token_fact(kXArmVendorFeedbackClassFactId, "stopped"),
                   });
}

[[nodiscard]] LifecycleIntentPlanDTO prepare_plan() {
  const auto prepared = prepare_requested();
  const auto enabled_stopped = drive_enabled_stopped_clean();
  return {
      ComponentLifecycleIntentKindDTO::kPrepare,
      ComponentLifecycleStateDTO::kPrepared,
      std::nullopt,
      prepared,
      kPlanTimeoutNs,
      {
          {
              LifecyclePrimitiveId(std::string(kXArmEnableMotionPrimitiveId)),
              predicate(
                  {
                      ComponentLifecycleStateDTO::kDeclared,
                      ComponentLifecycleStateDTO::kDisabled,
                  },
                  {
                      boolean_fact(kXArmMotionEnabledFactId, false),
                      boolean_fact(kXArmVendorStoppedFactId, true),
                      boolean_fact(kXArmFaultPresentFactId, false),
                      boolean_fact(kXArmRecoveryRequiredFactId, false),
                      boolean_fact(kXArmWarningPresentFactId, false),
                      token_fact(kXArmVendorFeedbackClassFactId, "stopped"),
                  }),
              enabled_stopped,
          },
          {
              LifecyclePrimitiveId(
                  std::string(kXArmSelectJointServoModePrimitiveId)),
              enabled_stopped,
              prepared,
          },
      }};
}

[[nodiscard]] LifecycleIntentPlanDTO enable_plan() {
  auto eligibility = prepare_requested();
  eligibility.fact_predicates.push_back(
      boolean_fact(kXArmReadyStateRequestEligibleFactId, true));
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
          {{
              LifecyclePrimitiveId(std::string(kXArmSetReadyStatePrimitiveId)),
              std::move(eligibility),
              active_requested(),
          }}};
}

[[nodiscard]] LifecycleIntentPlanDTO disable_plan() {
  const auto stopped_enabled = predicate(
      {
          ComponentLifecycleStateDTO::kDegraded,
          ComponentLifecycleStateDTO::kFaulted,
      },
      {
          boolean_fact(kXArmMotionEnabledFactId, true),
          boolean_fact(kXArmVendorControlReadyFactId, false),
          boolean_fact(kXArmVendorStoppedFactId, true),
          token_fact(kXArmVendorFeedbackClassFactId, "stopped"),
      });
  return {
      ComponentLifecycleIntentKindDTO::kDisable,
      ComponentLifecycleStateDTO::kDisabled,
      std::nullopt,
      disabled_requested(),
      kPlanTimeoutNs,
      {
          {
              LifecyclePrimitiveId(
                  std::string(kXArmSetStoppedStatePrimitiveId)),
              predicate(
                  {
                      ComponentLifecycleStateDTO::kPrepared,
                      ComponentLifecycleStateDTO::kActive,
                      ComponentLifecycleStateDTO::kDegraded,
                      ComponentLifecycleStateDTO::kFaulted,
                  },
                  {
                      boolean_fact(kXArmMotionEnabledFactId, true),
                      boolean_fact(kXArmVendorStoppedFactId, false),
                  }),
              stopped_enabled,
          },
          {
              LifecyclePrimitiveId(std::string(kXArmDisableMotionPrimitiveId)),
              stopped_enabled,
              disabled_requested(),
          },
      }};
}

[[nodiscard]] LifecycleIntentPlanDTO recover_plan() {
  const auto target_eligibility =
      predicate({ComponentLifecycleStateDTO::kDisabled},
                {
                    boolean_fact(kXArmMotionEnabledFactId, false),
                    boolean_fact(kXArmVendorStoppedFactId, true),
                    token_fact(kXArmVendorFeedbackClassFactId, "stopped"),
                });
  const auto recovered = recover_requested();
  return {
      ComponentLifecycleIntentKindDTO::kRecover,
      ComponentLifecycleStateDTO::kDisabled,
      target_eligibility,
      recovered,
      kPlanTimeoutNs,
      {
          {
              LifecyclePrimitiveId(std::string(kXArmClearErrorPrimitiveId)),
              predicate(
                  {ComponentLifecycleStateDTO::kDisabled},
                  {
                      boolean_fact(kXArmMotionEnabledFactId, false),
                      boolean_fact(kXArmVendorStoppedFactId, true),
                      boolean_fact(kXArmFaultPresentFactId, true),
                      token_fact(kXArmVendorFeedbackClassFactId, "stopped"),
                  }),
              predicate(
                  {ComponentLifecycleStateDTO::kDisabled},
                  {
                      boolean_fact(kXArmMotionEnabledFactId, false),
                      boolean_fact(kXArmVendorStoppedFactId, true),
                      boolean_fact(kXArmFaultPresentFactId, false),
                      boolean_fact(kXArmRecoveryRequiredFactId, false),
                      token_fact(kXArmVendorFeedbackClassFactId, "stopped"),
                  }),
          },
          {
              LifecyclePrimitiveId(std::string(kXArmClearWarningPrimitiveId)),
              predicate(
                  {ComponentLifecycleStateDTO::kDisabled},
                  {
                      boolean_fact(kXArmMotionEnabledFactId, false),
                      boolean_fact(kXArmVendorStoppedFactId, true),
                      boolean_fact(kXArmFaultPresentFactId, false),
                      boolean_fact(kXArmRecoveryRequiredFactId, false),
                      boolean_fact(kXArmWarningPresentFactId, true),
                      token_fact(kXArmVendorFeedbackClassFactId, "stopped"),
                  }),
              recovered,
          },
      }};
}

[[nodiscard]] std::vector<LifecycleIntentPlanDTO> lite6_plans() {
  return {prepare_plan(), enable_plan(), disable_plan(), recover_plan()};
}

[[nodiscard]] core::ProviderCapabilityOfferDTO
lifecycle_capability(std::string_view capability_id) {
  const auto *definition =
      core::find_builtin_capability_definition(capability_id);
  if (definition == nullptr) {
    throw std::logic_error("missing built-in Lite6 lifecycle capability");
  }
  return {core::CapabilityId(std::string(capability_id)),
          core::CapabilityDefinitionRevision(
              std::string(definition->capability_definition_revision)),
          {}};
}

} // namespace

core::Result<core::ContentDigest>
compute_lite6_brand_lifecycle_catalog_semantic_digest() {
  return core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
      core::ProviderManifestId(std::string(kLite6LifecycleProviderManifestId)),
      lite6_plans());
}

core::Result<core::ProviderManifestDTO>
make_build_owned_lite6_lifecycle_provider_manifest() {
  const auto semantic = compute_lite6_brand_lifecycle_catalog_semantic_digest();
  if (!semantic.ok()) {
    return core::Result<core::ProviderManifestDTO>::Err(semantic.error());
  }
  if (semantic.value().value() !=
      generated::kLite6LifecycleCatalogSemanticDigest) {
    return core::Result<core::ProviderManifestDTO>::Err(
        {"lite6_lifecycle_product_identity_stale",
         "the generated Lite6 lifecycle semantic identity is stale",
         {{"reason", "semantic_digest_mismatch"}}});
  }
  try {
    std::vector<core::ProviderCapabilityOfferDTO> capabilities;
    for (const std::string_view capability_id : {
             "component.lifecycle.disable",
             "component.lifecycle.enable",
             "component.lifecycle.observe",
             "component.lifecycle.prepare",
             "component.lifecycle.recover",
         }) {
      capabilities.push_back(lifecycle_capability(capability_id));
    }
    return core::Result<core::ProviderManifestDTO>::Ok(
        {core::ProviderManifestId(
             std::string(kLite6LifecycleProviderManifestId)),
         core::ProviderManifestRevision(
             std::string(generated::kLite6LifecycleProviderManifestRevision)),
         std::move(capabilities),
         {},
         core::ProviderLifecyclePlanCatalogDeclarationDTO{
             core::SchemaId(
                 std::string(core::kBrandLifecyclePlanCatalogApiVersion)),
             semantic.value()}});
  } catch (const std::exception &exception) {
    return core::Result<core::ProviderManifestDTO>::Err(
        {"lite6_lifecycle_product_manifest_invalid",
         "the generated Lite6 lifecycle provider manifest is invalid",
         {{"reason", exception.what()}}});
  }
}

core::Result<core::BrandLifecyclePlanCatalogDTO>
make_lite6_brand_lifecycle_plan_catalog(
    const core::ProviderManifestDTO &provider_manifest) {
  if (provider_manifest.provider_manifest_id !=
      core::ProviderManifestId(
          std::string(kLite6LifecycleProviderManifestId))) {
    return core::Result<core::BrandLifecyclePlanCatalogDTO>::Err(
        {"lite6_lifecycle_provider_manifest_mismatch",
         "the qualified provider manifest is not the Lite6 lifecycle owner",
         {{"reason", "provider_manifest_id_mismatch"}}});
  }
  auto catalog = core::MakeBrandLifecyclePlanCatalog(
      core::ProviderManifestId(std::string(kLite6LifecycleProviderManifestId)),
      provider_manifest.provider_manifest_revision, lite6_plans());
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

Lite6BrandLifecyclePlanCatalogSource::Lite6BrandLifecyclePlanCatalogSource(
    core::BrandLifecyclePlanCatalogDTO catalog,
    core::BuildOwnedLifecyclePlanCatalogSource source)
    : catalog_(std::move(catalog)), source_(std::move(source)) {}

core::Result<Lite6BrandLifecyclePlanCatalogSource>
Lite6BrandLifecyclePlanCatalogSource::make_build_owned() {
  auto manifest = make_build_owned_lite6_lifecycle_provider_manifest();
  if (!manifest.ok()) {
    return core::Result<Lite6BrandLifecyclePlanCatalogSource>::Err(
        manifest.error());
  }
  auto source = make(std::move(manifest.value()));
  if (!source.ok()) {
    return source;
  }
  if (source.value().catalog().catalog_digest.value() !=
      generated::kLite6LifecycleCatalogDigest) {
    return core::Result<Lite6BrandLifecyclePlanCatalogSource>::Err(
        {"lite6_lifecycle_product_identity_stale",
         "the generated Lite6 lifecycle catalog identity is stale",
         {{"reason", "catalog_digest_mismatch"}}});
  }
  return source;
}

core::Result<Lite6BrandLifecyclePlanCatalogSource>
Lite6BrandLifecyclePlanCatalogSource::make(
    core::ProviderManifestDTO provider_manifest) {
  auto catalog = make_lite6_brand_lifecycle_plan_catalog(provider_manifest);
  if (!catalog.ok()) {
    return core::Result<Lite6BrandLifecyclePlanCatalogSource>::Err(
        catalog.error());
  }
  auto source = core::BuildOwnedLifecyclePlanCatalogSource::Make(
      {{std::move(provider_manifest), catalog.value()}});
  if (!source.ok()) {
    return core::Result<Lite6BrandLifecyclePlanCatalogSource>::Err(
        source.error());
  }
  return core::Result<Lite6BrandLifecyclePlanCatalogSource>::Ok(
      Lite6BrandLifecyclePlanCatalogSource(std::move(catalog.value()),
                                           std::move(source.value())));
}

core::Result<core::BrandLifecyclePlanCatalogDTO>
Lite6BrandLifecyclePlanCatalogSource::LoadExactLifecyclePlanCatalog(
    const core::ProviderManifestId &provider_manifest_id,
    const core::ProviderManifestRevision &provider_manifest_revision,
    const core::ContentDigest &catalog_digest) const {
  return source_.LoadExactLifecyclePlanCatalog(
      provider_manifest_id, provider_manifest_revision, catalog_digest);
}

const core::BrandLifecyclePlanCatalogDTO &
Lite6BrandLifecyclePlanCatalogSource::catalog() const noexcept {
  return catalog_;
}

} // namespace manyforge::xarm_control
