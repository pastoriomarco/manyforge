#include <algorithm>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/io_provider_lifecycle_catalog.hpp>

namespace manyforge::core {
namespace {
LifecyclePredicateDTO predicate(std::vector<ComponentLifecycleStateDTO> states,
                                std::initializer_list<std::pair<const char*, bool>> facts) {
  std::sort(states.begin(), states.end());
  std::vector<LifecycleFactPredicateDTO> values;
  for (const auto& [id, value] : facts)
    values.push_back({LifecycleFactId(id),
                      {LifecycleFactValueKindDTO::kBoolean, value, std::nullopt, std::nullopt}});
  std::sort(values.begin(), values.end(),
            [](const auto& a, const auto& b) { return a.lifecycle_fact_id < b.lifecycle_fact_id; });
  return {std::move(states), std::move(values)};
}
}  // namespace

Result<std::vector<LifecycleIntentPlanDTO>> MakeIoProviderLifecyclePlans() {
  using Reply = Result<std::vector<LifecycleIntentPlanDTO>>;
  constexpr auto timeout = DeviceConfiguredLifecycleLimitDTO{};
  using State = ComponentLifecycleStateDTO;
  using Intent = ComponentLifecycleIntentKindDTO;
  constexpr auto resources = "io.driver.resources_ready";
  constexpr auto enabled = "io.driver.enabled";
  constexpr auto fault = "io.driver.fault_present";
  constexpr auto transport = "io.driver.transport_ready";
  const auto prepared = predicate({State::kPrepared, State::kActive},
                                  {{resources, true}, {fault, false}, {transport, true}});
  const auto active = predicate(
      {State::kActive}, {{resources, true}, {enabled, true}, {fault, false}, {transport, true}});
  const auto disabled =
      predicate({State::kDisabled}, {{resources, true}, {enabled, false}, {fault, false}});
  return Reply::Ok(
      {{Intent::kPrepare,
        State::kPrepared,
        std::nullopt,
        prepared,
        timeout,
        {{LifecyclePrimitiveId("io.provider.prepare"),
          predicate({State::kDeclared, State::kDisabled}, {{fault, false}, {transport, true}}),
          prepared}}},
       {Intent::kEnable,
        State::kActive,
        std::nullopt,
        active,
        timeout,
        {{LifecyclePrimitiveId("io.provider.enable"),
          predicate({State::kPrepared},
                    {{resources, true}, {enabled, false}, {fault, false}, {transport, true}}),
          active}}},
       {Intent::kDisable,
        State::kDisabled,
        std::nullopt,
        disabled,
        timeout,
        {{LifecyclePrimitiveId("io.provider.disable"),
          predicate({State::kPrepared, State::kActive, State::kDegraded}, {{resources, true}}),
          disabled}}},
       {Intent::kRecover,
        State::kPrepared,
        predicate({State::kPrepared, State::kActive, State::kDegraded, State::kFaulted},
                  {{transport, true}}),
        prepared,
        timeout,
        {{LifecyclePrimitiveId("io.provider.recover"),
          predicate({State::kDegraded, State::kFaulted}, {{transport, true}}), prepared}}}});
}

Result<BrandLifecyclePlanCatalogDTO> MakeIoProviderLifecycleCatalog(
    const ProviderManifestDTO& manifest) {
  using Reply = Result<BrandLifecyclePlanCatalogDTO>;
  if (manifest.provider_manifest_id.value() != kIoProviderLifecycleManifestId &&
      manifest.provider_manifest_id.value() != kPoseSourceLifecycleManifestId)
    return Reply::Err({"io_provider_lifecycle_manifest_invalid",
                       "I/O driver catalog requires its exact maintained provider manifest",
                       {}});
  auto plans = MakeIoProviderLifecyclePlans();
  if (!plans.ok()) return Reply::Err(plans.error());
  auto catalog = MakeBrandLifecyclePlanCatalog(
      manifest.provider_manifest_id, manifest.provider_manifest_revision, std::move(plans.value()));
  if (!catalog.ok()) return catalog;
  const auto anchored = VerifyBrandLifecyclePlanCatalogManifestAnchor(manifest, catalog.value());
  if (!anchored.ok()) return Reply::Err(anchored.error());
  return catalog;
}

}  // namespace manyforge::core
