#include "native_projection_transition_internal.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include <manyforge_core/types/native_projection_capability.hpp>

namespace manyforge::component_manager::detail {
namespace {

using core::CapabilityEndpointSourceKind;
using core::CapabilityProviderBindingDTO;
using core::CapabilityProviderBindingId;
using core::CapabilityProviderBindingRevision;
using core::ProjectionNativeRuntimeBindingDTO;
using core::RetainedBundleDTO;
using core::RuntimeEpoch;
using core::RuntimeUnitId;

struct ProviderRequirement final {
  CapabilityProviderBindingId provider_binding_id;
  CapabilityProviderBindingRevision provider_binding_revision;
  RuntimeUnitId runtime_unit_id;
};

[[noreturn]] void Fail(const std::string &detail) {
  throw std::invalid_argument("native projection transition: " + detail);
}

[[nodiscard]] bool
IsNativeRequirement(const CapabilityProviderBindingDTO &binding) noexcept {
  return core::RequiresNativeSceneProjection(
      binding.capability_id, binding.capability_definition_revision);
}

[[nodiscard]] std::vector<ProviderRequirement>
Requirements(const RetainedBundleDTO &bundle) {
  std::set<RuntimeUnitId> declared_runtime_units;
  for (const auto &runtime_unit : bundle.cell_graph.runtime_units) {
    if (!declared_runtime_units.insert(runtime_unit.runtime_unit_id).second) {
      Fail("bundle repeats a runtime-unit identity");
    }
  }

  std::vector<ProviderRequirement> result;
  for (const auto &binding : bundle.capability_provider_bindings) {
    if (!IsNativeRequirement(binding)) {
      continue;
    }
    std::set<RuntimeUnitId> runtime_sources;
    for (const auto &endpoint : binding.endpoint_bindings) {
      if (endpoint.source_kind != CapabilityEndpointSourceKind::kRuntimeUnit) {
        continue;
      }
      const auto *runtime_unit =
          std::get_if<RuntimeUnitId>(&endpoint.source_id);
      if (runtime_unit == nullptr || runtime_unit->empty()) {
        Fail("runtime endpoint has a mismatched source union");
      }
      runtime_sources.insert(*runtime_unit);
    }
    if (runtime_sources.size() != 1U) {
      Fail("each native provider binding must resolve one exact runtime "
           "unit through its compiled endpoint closure");
    }
    const auto runtime_unit_id = *runtime_sources.begin();
    if (!declared_runtime_units.contains(runtime_unit_id)) {
      Fail("native provider endpoint names an undeclared runtime unit");
    }
    result.push_back({binding.capability_provider_binding_id,
                      binding.capability_provider_binding_revision,
                      runtime_unit_id});
  }
  std::sort(
      result.begin(), result.end(), [](const auto &left, const auto &right) {
        return std::tie(left.provider_binding_id,
                        left.provider_binding_revision, left.runtime_unit_id) <
               std::tie(right.provider_binding_id,
                        right.provider_binding_revision, right.runtime_unit_id);
      });
  if (std::adjacent_find(result.begin(), result.end(),
                         [](const auto &left, const auto &right) {
                           return left.provider_binding_id ==
                                      right.provider_binding_id &&
                                  left.provider_binding_revision ==
                                      right.provider_binding_revision;
                         }) != result.end()) {
    Fail("bundle repeats a native provider-binding identity");
  }
  return result;
}

[[nodiscard]] std::vector<ProjectionNativeRuntimeBindingDTO>
CandidateBindings(const std::vector<ProviderRequirement> &requirements,
                  const ManagerApplyRuntimeStagedReadyDTO &staged_ready) {
  if (!staged_ready.valid()) {
    Fail("candidate staged-ready evidence is invalid");
  }
  std::vector<ProjectionNativeRuntimeBindingDTO> result;
  result.reserve(requirements.size());
  for (const auto &requirement : requirements) {
    const auto evidence = std::lower_bound(
        staged_ready.runtime_units.begin(), staged_ready.runtime_units.end(),
        requirement.runtime_unit_id,
        [](const auto &row, const auto &runtime_unit_id) {
          return row.runtime_unit_id < runtime_unit_id;
        });
    if (evidence == staged_ready.runtime_units.end() ||
        evidence->runtime_unit_id != requirement.runtime_unit_id ||
        !evidence->valid()) {
      Fail("candidate staged-ready evidence omits a native provider runtime");
    }
    result.push_back({requirement.provider_binding_id,
                      requirement.provider_binding_revision,
                      requirement.runtime_unit_id, evidence->runtime_epoch});
  }
  std::sort(result.begin(), result.end(),
            core::CanonicalProjectionNativeBindingLess);
  return result;
}

} // namespace

bool HasNativeProjectionRequirements(const RetainedBundleDTO &bundle) noexcept {
  return std::any_of(bundle.capability_provider_bindings.begin(),
                     bundle.capability_provider_bindings.end(),
                     IsNativeRequirement);
}

std::vector<ProjectionNativeRuntimeBindingDTO>
DeriveCandidateNativeProjectionBindings(
    const RetainedBundleDTO &candidate,
    const ManagerApplyRuntimeStagedReadyDTO &candidate_staged_ready) {
  return CandidateBindings(Requirements(candidate), candidate_staged_ready);
}

} // namespace manyforge::component_manager::detail
