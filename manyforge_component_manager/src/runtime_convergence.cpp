#include "manyforge_component_manager/runtime_convergence.hpp"

#include <algorithm>
#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <set>
#include <stdexcept>

namespace manyforge::component_manager {
namespace {

template <typename Value>
[[nodiscard]] bool CanonicalStrongIds(const std::vector<Value> &values,
                                      const bool allow_empty) noexcept {
  if ((!allow_empty && values.empty()) ||
      std::any_of(values.begin(), values.end(),
                  [](const auto &value) { return value.empty(); })) {
    return false;
  }
  return std::adjacent_find(values.begin(), values.end(),
                            [](const auto &left, const auto &right) {
                              return !(left < right);
                            }) == values.end();
}

[[nodiscard]] bool PrintableIdentifier(const std::string_view value) noexcept {
  return !value.empty() && value.size() <= kMaximumIdentifierBytes &&
         std::all_of(value.begin(), value.end(), [](const char character) {
           const auto byte = static_cast<unsigned char>(character);
           return byte >= 0x21U && byte <= 0x7EU;
         });
}

[[nodiscard]] bool
Digest(const manyforge::core::ContentDigest &value) noexcept {
  const auto &text = value.value();
  return text.size() == 71U && text.starts_with("sha256:") &&
         std::all_of(text.begin() + 7, text.end(), [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] bool DigestText(const std::string_view text) noexcept {
  return text.size() == 71U && text.starts_with("sha256:") &&
         std::all_of(text.begin() + 7, text.end(), [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

} // namespace

void VerifyRuntimeContractAdmission(
    const RuntimeUnitConvergencePlanRowDTO &plan) {
  if (plan.runtime_contract_offer_digest.empty() ||
      plan.runtime_contract_requirement.requirement_digest.empty()) {
    throw std::invalid_argument(
        "runtime convergence plan has no sealed contract-manifest admission");
  }
  const auto *contract =
      manyforge::deployment::build_qualified_runtime_contract_registry().find(
          plan.runtime_profile_id, plan.runtime_profile_revision,
          plan.product_contract_id, plan.product_contract_revision);
  if (contract == nullptr ||
      contract->offer().offer_digest != plan.runtime_contract_offer_digest) {
    throw std::invalid_argument(
        "runtime convergence plan selected offer differs from the immutable "
        "build-qualified registry");
  }
  const auto compatible = manyforge::core::CheckRuntimeContractCompatibility(
      contract->offer(), plan.runtime_contract_requirement);
  if (!compatible.ok()) {
    throw std::invalid_argument(
        "runtime convergence plan requirement is incompatible with its "
        "selected offer");
  }
}

bool RuntimeUnitConvergencePlanRowDTO::valid() const noexcept {
  const bool contract_fields_valid =
      (DigestText(runtime_contract_offer_digest.value()) &&
       manyforge::core::VerifyRuntimeContractRequirement(
           runtime_contract_requirement)
           .ok());
  const bool base =
      !runtime_unit_id.empty() && !runtime_profile_id.empty() &&
      !runtime_profile_revision.empty() && !product_id.empty() &&
      !product_contract_id.empty() && !product_contract_revision.empty() &&
      Digest(product_identity) && Digest(configuration_identity) &&
      dependency_runtime_unit_ids.size() <= kMaximumDependencies &&
      CanonicalStrongIds(dependency_runtime_unit_ids, true) &&
      !std::binary_search(dependency_runtime_unit_ids.begin(),
                          dependency_runtime_unit_ids.end(), runtime_unit_id) &&
      required_runtime_role_ids.size() <= kMaximumRuntimeRoles &&
      CanonicalStrongIds(required_runtime_role_ids, false) &&
      !status_endpoint_role_id.empty() &&
      PrintableIdentifier(status_endpoint_contract_id) &&
      PrintableIdentifier(status_endpoint_contract_revision) &&
      PrintableIdentifier(status_transport) &&
      PrintableIdentifier(process_adapter_contract_id) &&
      PrintableIdentifier(process_adapter_contract_revision) &&
      PrintableIdentifier(operating_system) &&
      PrintableIdentifier(shell_contract) &&
      !ToString(candidate_disposition).empty() &&
      !manyforge::core::to_string(lifecycle_control).empty();
  if (!base || !contract_fields_valid) {
    return false;
  }
  if (lifecycle_control ==
      manyforge::core::RuntimeLifecycleControl::kManagerManaged) {
    return PrintableIdentifier(retrieval_contract_id) &&
           PrintableIdentifier(retrieval_contract_revision) &&
           launchable_content_digest.has_value() &&
           Digest(*launchable_content_digest) && launchable_byte_size > 0U;
  }
  return retrieval_contract_id.empty() && retrieval_contract_revision.empty() &&
         !launchable_content_digest.has_value() && launchable_byte_size == 0U;
}

bool ManagerApplyRuntimeConvergenceIntentDTO::valid() const noexcept {
  return !apply_id.empty() && apply_generation.nonzero() &&
         !candidate_retained_bundle_id.empty() &&
         !candidate_graph_revision.empty() && Digest(fence_acceptance_digest) &&
         predecessor_publication_generation.nonzero() &&
         Digest(predecessor_public_pointer_digest) &&
         runtime_units.size() <= kMaximumRuntimeUnits &&
         std::all_of(runtime_units.begin(), runtime_units.end(),
                     [](const auto &row) { return row.valid(); }) &&
         std::adjacent_find(runtime_units.begin(), runtime_units.end(),
                            [](const auto &left, const auto &right) {
                              return !(left.runtime_unit_id <
                                       right.runtime_unit_id);
                            }) == runtime_units.end();
}

bool RuntimeUnitConvergenceQueryDTO::valid() const noexcept {
  return std::visit([](const auto &value) { return value.valid(); }, owner) &&
         !candidate_retained_bundle_id.empty() &&
         !candidate_graph_revision.empty() &&
         Digest(runtime_convergence_intent_digest) && plan.valid();
}

bool RuntimeUnitStagedReadyEvidenceDTO::valid() const noexcept {
  return !runtime_unit_id.empty() && !runtime_epoch.empty() &&
         PrintableIdentifier(process_observer_instance_id) &&
         process_observation_sequence > 0U && source_report_sequence > 0U &&
         Digest(observed_product_identity) &&
         Digest(observed_configuration_identity) &&
         source_phase == RuntimeSourcePhase::kServing &&
         source_health == RuntimeSourceHealth::kHealthy &&
         ready_runtime_role_ids.size() <= kMaximumRuntimeRoles &&
         CanonicalStrongIds(ready_runtime_role_ids, false) &&
         command_surface_state ==
             ManagedCommandSurfaceState::kClosedNoActiveEffect &&
         dependency_runtime_epochs.size() <= kMaximumDependencies &&
         std::all_of(dependency_runtime_epochs.begin(),
                     dependency_runtime_epochs.end(),
                     [](const auto &row) { return row.valid(); }) &&
         std::adjacent_find(
             dependency_runtime_epochs.begin(), dependency_runtime_epochs.end(),
             [](const auto &left, const auto &right) {
               return !(left.runtime_unit_id < right.runtime_unit_id);
             }) == dependency_runtime_epochs.end() &&
         DigestText(observed_runtime_contract_offer_digest.value()) &&
         !ToString(candidate_disposition).empty() &&
         !manyforge::core::to_string(lifecycle_control).empty();
}

bool ManagerApplyRuntimeStagedReadyDTO::valid() const noexcept {
  return !apply_id.empty() && apply_generation.nonzero() &&
         staged_ready_generation > 0U &&
         Digest(runtime_convergence_intent_digest) &&
         predecessor_publication_generation.nonzero() &&
         Digest(predecessor_public_pointer_digest) && evaluated_at_ns > 0U &&
         runtime_units.size() <= kMaximumRuntimeUnits &&
         std::all_of(runtime_units.begin(), runtime_units.end(),
                     [](const auto &row) { return row.valid(); }) &&
         std::adjacent_find(runtime_units.begin(), runtime_units.end(),
                            [](const auto &left, const auto &right) {
                              return !(left.runtime_unit_id <
                                       right.runtime_unit_id);
                            }) == runtime_units.end();
}

std::string_view ToString(const RuntimeUnitCandidateDisposition value) {
  switch (value) {
  case RuntimeUnitCandidateDisposition::kAdded:
    return "added";
  case RuntimeUnitCandidateDisposition::kRetainedExact:
    return "retained_exact";
  }
  return {};
}

} // namespace manyforge::component_manager
