#include "native_projection_transition_internal.hpp"

#include <manyforge_component_manager/active_native_projection_runtime_set.hpp>

#include <algorithm>
#include <map>
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
using core::ProjectionHealthDTO;
using core::ProjectionNativeRuntimeBindingDTO;
using core::ProjectionRevisionDTO;
using core::ProjectionTransitionDisposition;
using core::ProjectionTransitionRowDTO;
using core::RetainedBundleDTO;
using core::RuntimeEpoch;
using core::RuntimeUnitId;

struct ProviderRequirement final {
  CapabilityProviderBindingId provider_binding_id;
  CapabilityProviderBindingRevision provider_binding_revision;
  RuntimeUnitId runtime_unit_id;
};

using ProviderKey =
    std::pair<CapabilityProviderBindingId, CapabilityProviderBindingRevision>;

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

[[nodiscard]] std::vector<ProjectionNativeRuntimeBindingDTO>
PredecessorBindings(const std::vector<ProviderRequirement> &requirements,
                    const ProjectionRevisionDTO &predecessor_revision,
                    const std::span<const ProjectionHealthDTO> health) {
  if (requirements.empty()) {
    if (!health.empty()) {
      Fail("zero-native predecessor has unexpected live projection health");
    }
    return {};
  }
  if (health.size() != requirements.size()) {
    Fail("predecessor health does not cover every native provider binding");
  }
  std::map<ProviderKey, const ProviderRequirement *> expected;
  for (const auto &requirement : requirements) {
    expected.emplace(ProviderKey{requirement.provider_binding_id,
                                 requirement.provider_binding_revision},
                     &requirement);
  }
  std::vector<ProjectionNativeRuntimeBindingDTO> result;
  result.reserve(health.size());
  for (const auto &observation : health) {
    if (!observation.healthy_for_target(predecessor_revision)) {
      Fail("predecessor native projection is not healthy at the current "
           "active revision");
    }
    const auto &binding = observation.native_runtime_binding;
    const auto found = expected.find(
        {binding.provider_binding_id, binding.provider_binding_revision});
    if (found == expected.end() ||
        found->second->runtime_unit_id != binding.runtime_unit_id) {
      Fail("predecessor live projection differs from the compiled provider "
           "and runtime mapping");
    }
    result.push_back(binding);
    expected.erase(found);
  }
  if (!expected.empty()) {
    Fail("predecessor live projection set is incomplete");
  }
  std::sort(result.begin(), result.end(),
            core::CanonicalProjectionNativeBindingLess);
  if (std::adjacent_find(result.begin(), result.end()) != result.end()) {
    Fail("predecessor live projection set repeats a native instance");
  }
  return result;
}

} // namespace

bool HasNativeProjectionRequirements(const RetainedBundleDTO &bundle) noexcept {
  return std::any_of(bundle.capability_provider_bindings.begin(),
                     bundle.capability_provider_bindings.end(),
                     IsNativeRequirement);
}

} // namespace manyforge::component_manager::detail

namespace manyforge::component_manager {

std::vector<core::ProjectionNativeRuntimeBindingDTO>
DeriveActiveNativeProjectionRuntimeBindings(
    const core::RetainedBundleDTO &active_bundle,
    const RuntimeObservationSnapshot &observations) {
  std::vector<core::ProjectionNativeRuntimeBindingDTO> bindings;
  const auto requirements = detail::Requirements(active_bundle);
  bindings.reserve(requirements.size());
  for (const auto &requirement : requirements) {
    const auto *observation =
        observations.observation(requirement.runtime_unit_id.value());
    if (observation == nullptr || !observation->converged() ||
        observation->lifecycle_control() !=
            RuntimeLifecycleControl::kManagerManaged ||
        observation->process_presence() != ProcessPresenceState::kPresent ||
        observation->runtime_epoch().empty()) {
      detail::Fail(
          "active native provider has no exact converged Manager-observed "
          "runtime epoch");
    }
    bindings.push_back({requirement.provider_binding_id,
                        requirement.provider_binding_revision,
                        requirement.runtime_unit_id,
                        core::RuntimeEpoch(observation->runtime_epoch())});
  }
  std::sort(bindings.begin(), bindings.end(),
            core::CanonicalProjectionNativeBindingLess);
  return bindings;
}

core::ActiveProjectionRuntimeSetDTO DeriveActiveNativeProjectionRuntimeSet(
    const core::RetainedBundleDTO &active_bundle,
    const core::ProjectionRevisionDTO &active_projection_revision,
    const RuntimeObservationSnapshot &observations) {
  if (!active_projection_revision.valid() ||
      active_bundle.retained_bundle_id !=
          active_projection_revision.retained_bundle_id ||
      active_bundle.cell_graph.graph_revision !=
          active_projection_revision.graph_revision) {
    detail::Fail("active bundle differs from its selected projection revision");
  }
  core::ActiveProjectionRuntimeSetDTO result{
      active_projection_revision,
      DeriveActiveNativeProjectionRuntimeBindings(active_bundle, observations),
  };
  if (!result.valid()) {
    detail::Fail("active native runtime set is not exact and canonical");
  }
  return result;
}

} // namespace manyforge::component_manager

namespace manyforge::component_manager::detail {

std::vector<ProjectionTransitionRowDTO> DeriveNativeProjectionTransitionRows(
    const RetainedBundleDTO &predecessor, const RetainedBundleDTO &candidate,
    const ProjectionRevisionDTO &predecessor_revision,
    const std::span<const ProjectionHealthDTO> predecessor_health,
    const ManagerApplyRuntimeStagedReadyDTO &candidate_staged_ready) {
  if (!predecessor_revision.valid() ||
      predecessor.retained_bundle_id !=
          predecessor_revision.retained_bundle_id ||
      predecessor.cell_graph.graph_revision !=
          predecessor_revision.graph_revision) {
    Fail("predecessor bundle and active revision differ");
  }

  const auto predecessor_bindings = PredecessorBindings(
      Requirements(predecessor), predecessor_revision, predecessor_health);
  const auto candidate_bindings =
      CandidateBindings(Requirements(candidate), candidate_staged_ready);

  std::vector<ProjectionTransitionRowDTO> rows;
  rows.reserve(predecessor_bindings.size() + candidate_bindings.size());
  std::size_t predecessor_index = 0U;
  std::size_t candidate_index = 0U;
  while (predecessor_index < predecessor_bindings.size() ||
         candidate_index < candidate_bindings.size()) {
    if (predecessor_index == predecessor_bindings.size()) {
      rows.push_back({candidate_bindings[candidate_index++],
                      ProjectionTransitionDisposition::kAdded, std::nullopt});
      continue;
    }
    if (candidate_index == candidate_bindings.size()) {
      rows.push_back({predecessor_bindings[predecessor_index++],
                      ProjectionTransitionDisposition::kRemoved,
                      predecessor_revision});
      continue;
    }
    const auto &predecessor_binding = predecessor_bindings[predecessor_index];
    const auto &candidate_binding = candidate_bindings[candidate_index];
    if (predecessor_binding == candidate_binding) {
      rows.push_back({predecessor_binding,
                      ProjectionTransitionDisposition::kRetained,
                      predecessor_revision});
      ++predecessor_index;
      ++candidate_index;
    } else if (core::CanonicalProjectionNativeBindingLess(predecessor_binding,
                                                          candidate_binding)) {
      rows.push_back({predecessor_binding,
                      ProjectionTransitionDisposition::kRemoved,
                      predecessor_revision});
      ++predecessor_index;
    } else {
      rows.push_back({candidate_binding,
                      ProjectionTransitionDisposition::kAdded, std::nullopt});
      ++candidate_index;
    }
  }
  if (!core::CanonicalProjectionTransitionRows(rows)) {
    Fail("derived transition rows are not exact and canonical");
  }
  return rows;
}

} // namespace manyforge::component_manager::detail
