#include "manyforge_headless/owner_publication.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/logical_scene_snapshot.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::headless {
namespace {

namespace component_manager = manyforge::component_manager;
namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;

[[noreturn]] void Fail(const std::string &detail) {
  throw std::invalid_argument("headless owner publication: " + detail);
}

void Require(const bool condition, const std::string &detail) {
  if (!condition) {
    Fail(detail);
  }
}

CanonicalValue Text(const std::string_view value,
                    const std::string_view label) {
  Require(!value.empty(), std::string(label) + " must be non-empty");
  return CanonicalValue(std::string(value));
}

CanonicalValue OptionalText(const std::optional<std::string> &value) {
  return value.has_value() ? CanonicalValue(*value) : CanonicalValue(nullptr);
}

CanonicalValue Unsigned(const std::uint64_t value, const std::string_view label,
                        const bool nonzero = false) {
  Require(value <= kMaximumSafeInteger && (!nonzero || value != 0U),
          std::string(label) + " must be an I-JSON safe integer");
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue Integer(const std::int64_t value, const std::string_view label) {
  Require(value >= -static_cast<std::int64_t>(kMaximumSafeInteger) &&
              value <= static_cast<std::int64_t>(kMaximumSafeInteger),
          std::string(label) + " must be an I-JSON safe integer");
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue OptionalUnsigned(const std::optional<std::uint64_t> value,
                                const std::string_view label) {
  return value.has_value() ? Unsigned(*value, label) : CanonicalValue(nullptr);
}

template <typename StrongId>
CanonicalValue StrongIdArray(const std::vector<StrongId> &values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto &value : values) {
    result.emplace_back(Text(value.value(), "strong identifier"));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue StringArray(const std::vector<std::string> &values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto &value : values) {
    result.emplace_back(Text(value, "string array entry"));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue RuntimeContractOfferDigest(const std::string &value) {
  return value.empty() ? CanonicalValue(nullptr) : CanonicalValue(value);
}

CanonicalValue ProjectionRevision(const core::ProjectionRevisionDTO &value) {
  Require(value.valid(), "projection revision is invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"graph_revision", CanonicalValue(value.graph_revision.value())},
      {"retained_bundle_id", CanonicalValue(value.retained_bundle_id.value())},
      {"scene_revision",
       CanonicalValue(CanonicalValue::Object{
           {"content_hash",
            CanonicalValue(value.scene_revision.content_hash.value())},
           {"revision_id", Unsigned(value.scene_revision.revision_id,
                                    "scene_revision.revision_id", true)},
           {"scene_id", CanonicalValue(value.scene_revision.scene_id.value())},
       })},
  });
}

CanonicalValue PublicationTransaction(
    const core::ActiveBundlePublicationTransactionRefDTO &value) {
  return std::visit(
      [](const auto &transaction) -> CanonicalValue {
        using T = std::decay_t<decltype(transaction)>;
        if constexpr (std::is_same_v<
                          T, core::NamedApplyPublicationTransactionDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"apply_generation",
               Unsigned(transaction.apply_generation.value(),
                        "apply_generation", true)},
              {"apply_id", CanonicalValue(transaction.apply_id.value())},
              {"kind", CanonicalValue("named_apply")},
              {"projection_stage_generation",
               Unsigned(transaction.projection_stage_generation.value(),
                        "projection_stage_generation", true)},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"bootstrap_generation",
               Unsigned(transaction.bootstrap_generation.value(),
                        "bootstrap_generation", true)},
              {"bootstrap_id",
               CanonicalValue(transaction.bootstrap_id.value())},
              {"kind", CanonicalValue("bootstrap")},
          });
        }
      },
      value);
}

CanonicalValue
ActivePublication(const core::ActiveBundlePublicationReceiptDTO &value) {
  Require(value.valid(), "active publication receipt is invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"durable_public_pointer_content_digest",
       CanonicalValue(value.durable_public_pointer_content_digest.value())},
      {"projection_revision",
       ProjectionRevision(value.request.candidate_projection_revision)},
      {"publication_generation", Unsigned(value.publication_generation.value(),
                                          "publication_generation", true)},
      {"transaction", PublicationTransaction(value.request.transaction_ref)},
  });
}

CanonicalValue RuntimePlanRow(
    const component_manager::RuntimeUnitConvergencePlanRowDTO &value) {
  Require(value.valid(), "runtime plan row is invalid");
  CanonicalValue::Array dependencies;
  dependencies.reserve(value.dependency_runtime_unit_ids.size());
  for (const auto &id : value.dependency_runtime_unit_ids) {
    dependencies.emplace_back(CanonicalValue(id.value()));
  }
  CanonicalValue::Array roles;
  roles.reserve(value.required_runtime_role_ids.size());
  for (const auto &id : value.required_runtime_role_ids) {
    roles.emplace_back(CanonicalValue(id.value()));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"candidate_disposition",
       CanonicalValue(std::string(
           component_manager::ToString(value.candidate_disposition)))},
      {"configuration_identity",
       CanonicalValue(value.configuration_identity.value())},
      {"dependency_runtime_unit_ids", CanonicalValue(std::move(dependencies))},
      {"lifecycle_control",
       CanonicalValue(std::string(core::to_string(value.lifecycle_control)))},
      {"product_contract_id",
       CanonicalValue(value.product_contract_id.value())},
      {"product_contract_revision",
       CanonicalValue(value.product_contract_revision.value())},
      {"product_id", CanonicalValue(value.product_id.value())},
      {"product_identity", CanonicalValue(value.product_identity.value())},
      {"required_runtime_role_ids", CanonicalValue(std::move(roles))},
      {"runtime_profile_id", CanonicalValue(value.runtime_profile_id.value())},
      {"runtime_profile_revision",
       CanonicalValue(value.runtime_profile_revision.value())},
      {"selected_runtime_contract_offer_digest",
       CanonicalValue(value.runtime_contract_offer_digest.value())},
      {"runtime_unit_id", CanonicalValue(value.runtime_unit_id.value())},
      {"status_endpoint_contract_id",
       CanonicalValue(value.status_endpoint_contract_id)},
      {"status_endpoint_contract_revision",
       CanonicalValue(value.status_endpoint_contract_revision)},
      {"status_endpoint_role_id",
       CanonicalValue(value.status_endpoint_role_id.value())},
      {"status_transport", CanonicalValue(value.status_transport)},
  });
}

CanonicalValue
RuntimeObservation(const component_manager::RuntimeUnitObservation &value) {
  CanonicalValue::Object dependency_epochs;
  dependency_epochs.reserve(value.dependency_runtime_epochs().size());
  for (const auto &[runtime_unit_id, runtime_epoch] :
       value.dependency_runtime_epochs()) {
    dependency_epochs.emplace_back(runtime_unit_id,
                                   CanonicalValue(runtime_epoch));
  }
  const auto optional_phase =
      value.source_phase().has_value()
          ? CanonicalValue(
                std::string(component_manager::ToString(*value.source_phase())))
          : CanonicalValue(nullptr);
  const auto optional_health =
      value.source_health().has_value()
          ? CanonicalValue(std::string(
                component_manager::ToString(*value.source_health())))
          : CanonicalValue(nullptr);
  const auto optional_surface =
      value.command_surface_state().has_value()
          ? CanonicalValue(std::string(
                component_manager::ToString(*value.command_surface_state())))
          : CanonicalValue(nullptr);
  return CanonicalValue(CanonicalValue::Object{
      {"command_surface_state", optional_surface},
      {"converged", CanonicalValue(value.converged())},
      {"critical_progress_sequence",
       OptionalUnsigned(value.critical_progress_sequence(),
                        "critical_progress_sequence")},
      {"dependency_runtime_epochs",
       CanonicalValue(std::move(dependency_epochs))},
      {"derived_state", CanonicalValue(std::string(component_manager::ToString(
                            value.derived_state())))},
      {"lifecycle_control",
       CanonicalValue(std::string(
           component_manager::ToString(value.lifecycle_control())))},
      {"observed_configuration_identity",
       CanonicalValue(value.observed_configuration_identity())},
      {"observed_product_identity",
       CanonicalValue(value.observed_product_identity())},
      {"process_observation_sequence",
       OptionalUnsigned(value.process_observation_sequence(),
                        "process_observation_sequence")},
      {"process_observer_instance_id",
       CanonicalValue(value.process_observer_instance_id())},
      {"process_presence",
       CanonicalValue(
           std::string(component_manager::ToString(value.process_presence())))},
      {"ready_runtime_role_ids", StringArray(value.ready_runtime_role_ids())},
      {"reason_codes", StringArray(value.reason_codes())},
      {"runtime_epoch", CanonicalValue(value.runtime_epoch())},
      {"runtime_unit_id", CanonicalValue(value.runtime_unit_id())},
      {"source_health", optional_health},
      {"source_phase", optional_phase},
      {"source_reason_codes", StringArray(value.source_reason_codes())},
      {"source_report_sequence",
       OptionalUnsigned(value.source_report_sequence(),
                        "source_report_sequence")},
      {"observed_runtime_contract_offer_digest",
       RuntimeContractOfferDigest(
           value.observed_runtime_contract_offer_digest())},
  });
}

CanonicalValue
OptionalStrongId(const std::optional<core::ControlDomainRevision> &value) {
  return value.has_value() ? CanonicalValue(value->value())
                           : CanonicalValue(nullptr);
}

CanonicalValue
OptionalStrongId(const std::optional<core::ControlBindingRevision> &value) {
  return value.has_value() ? CanonicalValue(value->value())
                           : CanonicalValue(nullptr);
}

CanonicalValue FenceScope(const core::ConfigurationFenceScopeDTO &scope) {
  Require(scope.valid(), "Apply fence scope is invalid");
  CanonicalValue::Array domains;
  domains.reserve(scope.domains.size());
  for (const auto &domain : scope.domains) {
    domains.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"candidate_control_domain_revision",
         OptionalStrongId(domain.candidate_control_domain_revision)},
        {"control_domain_id", CanonicalValue(domain.control_domain_id.value())},
        {"predecessor_control_domain_revision",
         OptionalStrongId(domain.predecessor_control_domain_revision)},
    }));
  }
  CanonicalValue::Array bindings;
  bindings.reserve(scope.bindings.size());
  for (const auto &binding : scope.bindings) {
    bindings.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"candidate_control_binding_revision",
         OptionalStrongId(binding.candidate_control_binding_revision)},
        {"control_binding_id",
         CanonicalValue(binding.control_binding_id.value())},
        {"predecessor_control_binding_revision",
         OptionalStrongId(binding.predecessor_control_binding_revision)},
    }));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"bindings", CanonicalValue(std::move(bindings))},
      {"controllable_component_ids",
       StrongIdArray(scope.controllable_component_ids)},
      {"domains", CanonicalValue(std::move(domains))},
      {"resources", StrongIdArray(scope.resources)},
  });
}

std::string_view ApplyPhase(
    const component_manager::PersistedManagerApplyStatus &value) noexcept {
  if (value.terminal_outcome.has_value()) {
    return "terminal";
  }
  if (value.fence_released.has_value()) {
    return "fence_released";
  }
  if (value.fence_release_intent.has_value()) {
    return "fence_release_pending";
  }
  if (value.scene_finalized.has_value()) {
    return "scene_finalized";
  }
  if (value.publication_receipt.has_value()) {
    return "active_bundle_published";
  }
  if (value.retired_command_surfaces.has_value()) {
    return "command_surfaces_retired";
  }
  if (value.private_materialization.has_value()) {
    return "private_materialized";
  }
  if (value.scene_activated.has_value()) {
    return "scene_activated";
  }
  if (value.activation_selection.has_value()) {
    return "activation_selected";
  }
  if (value.scene_staged.has_value()) {
    return "scene_staged";
  }
  if (value.scene_stage_intent.has_value()) {
    return "scene_stage_pending";
  }
  if (value.latest_runtime_staged_ready.has_value()) {
    return "runtime_staged_ready";
  }
  if (value.runtime_convergence_intent.has_value()) {
    return "runtime_convergence_pending";
  }
  if (value.fence_acceptance.has_value()) {
    return "fence_held";
  }
  if (value.fence_attempt.has_value()) {
    return "fence_acquire_pending";
  }
  if (value.affected_scope.has_value()) {
    return "scope_persisted";
  }
  return "intent_persisted";
}

CanonicalValue
ApplyFence(const component_manager::PersistedManagerApplyStatus &value) {
  std::string_view state = "not_requested";
  if (value.fence_released.has_value()) {
    state = "released";
  } else if (value.fence_release_intent.has_value()) {
    state = "release_pending";
  } else if (value.fence_acceptance.has_value()) {
    state = "held";
  } else if (value.fence_attempt.has_value()) {
    state = "acquire_pending";
  } else if (value.affected_scope.has_value()) {
    state = "scope_persisted";
  }

  const auto scope =
      value.affected_scope.has_value()
          ? FenceScope(value.affected_scope->affected_scope.scope)
          : CanonicalValue(nullptr);
  const auto fence_id =
      value.fence_acceptance.has_value()
          ? CanonicalValue(value.fence_acceptance->accepted_fence
                               .configuration_fence_id.value())
          : CanonicalValue(nullptr);
  const auto fence_generation =
      value.fence_acceptance.has_value()
          ? Unsigned(value.fence_acceptance->accepted_fence
                         .configuration_fence_generation.value(),
                     "configuration_fence_generation", true)
          : CanonicalValue(nullptr);
  return CanonicalValue(CanonicalValue::Object{
      {"acceptance_digest",
       value.fence_acceptance.has_value()
           ? CanonicalValue(value.fence_acceptance->acceptance_digest.value())
           : CanonicalValue(nullptr)},
      {"configuration_fence_generation", fence_generation},
      {"configuration_fence_id", fence_id},
      {"fence_attempt_digest",
       value.fence_attempt.has_value()
           ? CanonicalValue(value.fence_attempt->fence_attempt_digest.value())
           : CanonicalValue(nullptr)},
      {"scope", scope},
      {"state", CanonicalValue(std::string(state))},
  });
}

CanonicalValue
ApplyStatus(const component_manager::PersistedManagerApplyStatus &value) {
  const auto &intent = value.intent.intent;
  const auto terminal_state =
      value.terminal_outcome.has_value()
          ? CanonicalValue(std::string(component_manager::ToString(
                value.terminal_outcome->terminal_outcome.state)))
          : CanonicalValue(nullptr);
  return CanonicalValue(CanonicalValue::Object{
      {"apply_generation",
       Unsigned(intent.apply_generation.value(), "apply_generation", true)},
      {"apply_id", CanonicalValue(intent.apply_id.value())},
      {"candidate_graph_revision",
       CanonicalValue(intent.candidate_graph_revision.value())},
      {"candidate_retained_bundle_id",
       CanonicalValue(intent.candidate_retained_bundle_id.value())},
      {"committed", CanonicalValue(value.publication_receipt.has_value())},
      {"expected_active_projection_revision",
       ProjectionRevision(intent.expected_active_projection_revision)},
      {"fence", ApplyFence(value)},
      {"incomplete", CanonicalValue(!value.terminal_outcome.has_value())},
      {"intent_digest", CanonicalValue(value.intent.intent_digest.value())},
      {"phase", CanonicalValue(std::string(ApplyPhase(value)))},
      {"request",
       CanonicalValue(CanonicalValue::Object{
           {"correlation_id",
            OptionalText(intent.request_context.correlation_id)},
           {"request_id",
            CanonicalValue(intent.request_context.request_id.value())},
           {"request_owner_epoch",
            CanonicalValue(intent.request_context.request_owner_epoch.value())},
           {"source_label",
            CanonicalValue(intent.request_context.source_label)},
       })},
      {"terminal_state", terminal_state},
  });
}

std::string_view ContactMode(const core::CollisionContactPolicyMode value) {
  switch (value) {
  case core::CollisionContactPolicyMode::kContactRelease:
    return "contact_release";
  case core::CollisionContactPolicyMode::kBoundedPenetration:
    return "bounded_penetration";
  case core::CollisionContactPolicyMode::kContactProcess:
    return "contact_process";
  }
  return {};
}

std::string_view ReleaseEvidence(const core::ContactReleaseEvidenceMode value) {
  switch (value) {
  case core::ContactReleaseEvidenceMode::kSignedPenetration:
    return "signed_penetration";
  case core::ContactReleaseEvidenceMode::kBinaryContact:
    return "binary_contact";
  }
  return {};
}

CanonicalValue Pair(const core::CollisionPairId &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"body_a", Text(value.body_a, "collision pair body_a")},
      {"body_b", Text(value.body_b, "collision pair body_b")},
  });
}

CanonicalValue PairArray(const std::vector<core::CollisionPairId> &values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto &value : values) {
    result.emplace_back(Pair(value));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue CollisionPolicy(const core::CollisionPolicySnapshot &value) {
  CanonicalValue::Array support_contacts;
  support_contacts.reserve(value.support_contacts.size());
  for (const auto &contact : value.support_contacts) {
    support_contacts.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"allow_recontact", CanonicalValue(contact.allow_recontact)},
        {"enforce_max_start_penetration",
         CanonicalValue(contact.enforce_max_start_penetration)},
        {"max_contact_penetration_m",
         CanonicalValue(contact.max_contact_penetration_m)},
        {"max_start_penetration_m",
         CanonicalValue(contact.max_start_penetration_m)},
        {"min_release_progress_m",
         CanonicalValue(contact.min_release_progress_m)},
        {"mode", CanonicalValue(std::string(ContactMode(contact.mode)))},
        {"pair", Pair(contact.pair)},
        {"release_evidence_mode", CanonicalValue(std::string(ReleaseEvidence(
                                      contact.release_evidence_mode)))},
        {"release_tolerance_m", CanonicalValue(contact.release_tolerance_m)},
        {"require_separation_by_end",
         CanonicalValue(contact.require_separation_by_end)},
        {"require_strictly_decreasing_penetration",
         CanonicalValue(contact.require_strictly_decreasing_penetration)},
    }));
  }
  CanonicalValue::Array grasps;
  grasps.reserve(value.contact_grasps.size());
  for (const auto &grasp : value.contact_grasps) {
    grasps.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"allowed_robot_links", StringArray(grasp.allowed_robot_links)},
        {"max_penetration_m", CanonicalValue(grasp.max_penetration_m)},
        {"policy_id", Text(grasp.policy_id, "contact grasp policy_id")},
        {"selected_object_id",
         Text(grasp.selected_object_id, "contact grasp selected_object_id")},
        {"terminal_window_start_fraction",
         CanonicalValue(grasp.terminal_window_start_fraction)},
    }));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"allowed_static_static_pairs",
       PairArray(value.allowed_static_static_pairs)},
      {"allowed_touch_link_pairs", PairArray(value.allowed_touch_link_pairs)},
      {"contact_grasps", CanonicalValue(std::move(grasps))},
      {"policy_id", Text(value.policy_id, "collision policy_id")},
      {"require_full_diagnostics",
       CanonicalValue(value.require_full_diagnostics)},
      {"revision",
       CanonicalValue(CanonicalValue::Object{
           {"content_hash",
            Text(value.revision.content_hash, "collision policy content_hash")},
           {"revision_id", Unsigned(value.revision.revision_id,
                                    "collision policy revision_id", true)},
       })},
      {"support_contacts", CanonicalValue(std::move(support_contacts))},
  });
}

CanonicalValue SupportDeclaration(const core::SupportContactDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"allow_recontact", CanonicalValue(value.allow_recontact)},
      {"enforce_max_start_penetration",
       CanonicalValue(value.enforce_max_start_penetration)},
      {"max_contact_penetration_m",
       CanonicalValue(value.max_contact_penetration_m)},
      {"max_start_penetration_m",
       CanonicalValue(value.max_start_penetration_m)},
      {"min_release_progress_m", CanonicalValue(value.min_release_progress_m)},
      {"mode", CanonicalValue(std::string(ContactMode(value.mode)))},
      {"release_evidence_mode", CanonicalValue(std::string(ReleaseEvidence(
                                    value.release_evidence_mode)))},
      {"release_tolerance_m", CanonicalValue(value.release_tolerance_m)},
      {"require_separation_by_end",
       CanonicalValue(value.require_separation_by_end)},
      {"require_strictly_decreasing_penetration",
       CanonicalValue(value.require_strictly_decreasing_penetration)},
      {"support_object_id", CanonicalValue(value.support_object_id)},
  });
}

CanonicalValue Vector(const core::Vector3DTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"x", CanonicalValue(value.x_m)},
      {"y", CanonicalValue(value.y_m)},
      {"z", CanonicalValue(value.z_m)},
  });
}

CanonicalValue Pose(const core::Pose3DTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"orientation_xyzw", CanonicalValue(CanonicalValue::Array{
                               CanonicalValue(value.orientation.x),
                               CanonicalValue(value.orientation.y),
                               CanonicalValue(value.orientation.z),
                               CanonicalValue(value.orientation.w),
                           })},
      {"position_m", Vector(value.position_m)},
  });
}

CanonicalValue Shape(const core::CollisionShapeDTO &value) {
  return std::visit(
      [](const auto &shape) -> CanonicalValue {
        using T = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<T, core::BoxShapeDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"dimensions_m", Vector(shape.dimensions_m)},
              {"type", CanonicalValue("box")},
          });
        } else if constexpr (std::is_same_v<T, core::SphereShapeDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"radius_m", CanonicalValue(shape.radius_m)},
              {"type", CanonicalValue("sphere")},
          });
        } else if constexpr (std::is_same_v<T, core::CylinderShapeDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"height_m", CanonicalValue(shape.height_m)},
              {"radius_m", CanonicalValue(shape.radius_m)},
              {"type", CanonicalValue("cylinder")},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"resource_uri", Text(shape.resource_uri, "mesh resource_uri")},
              {"scale", Vector(shape.scale)},
              {"type", CanonicalValue("mesh")},
          });
        }
      },
      value);
}

CanonicalValue SceneFrame(const core::SceneFrameRefDTO &value) {
  return std::visit(
      [](const auto &frame) -> CanonicalValue {
        using T = std::decay_t<decltype(frame)>;
        if constexpr (std::is_same_v<T, core::CellSceneFrameRefDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"cell_id", CanonicalValue(frame.cell_id.value())},
              {"kind", CanonicalValue("cell")},
          });
        } else if constexpr (std::is_same_v<T,
                                            core::ComponentSceneFrameRefDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"controllable_component_id",
               CanonicalValue(frame.controllable_component_id.value())},
              {"frame_role_id", CanonicalValue(frame.frame_role_id.value())},
              {"kind", CanonicalValue("component")},
              {"model_instance_id",
               CanonicalValue(frame.model_instance_id.value())},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"frame_role_id", CanonicalValue(frame.frame_role_id.value())},
              {"kind", CanonicalValue("passive_model")},
              {"model_instance_id",
               CanonicalValue(frame.model_instance_id.value())},
          });
        }
      },
      value);
}

CanonicalValue
ProviderEvidence(const core::SceneObjectProviderEvidenceDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"provider_instance_id",
       CanonicalValue(value.provider_instance_id.value())},
      {"provider_manifest_id",
       CanonicalValue(value.provider_manifest_id.value())},
      {"provider_manifest_revision",
       CanonicalValue(value.provider_manifest_revision.value())},
      {"provider_source_epoch", CanonicalValue(value.provider_source_epoch)},
      {"provider_source_sequence",
       Unsigned(value.provider_source_sequence, "provider_source_sequence")},
      {"received_at_ns", content_identity::nonnegative_int64_value(value.received_at_ns)},
      {"source_freshness_budget_ns",
       value.source_freshness_budget_ns.has_value()
           ? Integer(*value.source_freshness_budget_ns,
                     "source_freshness_budget_ns")
           : CanonicalValue(nullptr)},
      {"source_timestamp_ns",
       value.source_timestamp_ns.has_value()
           ? content_identity::nonnegative_int64_value(*value.source_timestamp_ns)
           : CanonicalValue(nullptr)},
  });
}

CanonicalValue SceneObject(const core::SceneObjectSnapshotDTO &value) {
  CanonicalValue::Array shapes;
  shapes.reserve(value.collision_shapes.size());
  for (const auto &shape : value.collision_shapes) {
    shapes.emplace_back(Shape(shape));
  }
  CanonicalValue::Array support_contacts;
  support_contacts.reserve(value.support_contacts.size());
  for (const auto &contact : value.support_contacts) {
    support_contacts.emplace_back(SupportDeclaration(contact));
  }
  const auto placement = std::visit(
      [](const auto &entry) -> CanonicalValue {
        using T = std::decay_t<decltype(entry)>;
        if constexpr (std::is_same_v<T,
                                     core::CellFixedSceneObjectPlacementDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"cell_id", CanonicalValue(entry.cell_id.value())},
              {"kind", CanonicalValue("cell_fixed")},
              {"pose_in_cell", Pose(entry.pose_in_cell)},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"carried_object_attachment_id",
               CanonicalValue(entry.carried_object_attachment_id.value())},
              {"carried_object_attachment_revision",
               CanonicalValue(
                   entry.carried_object_attachment_revision.value())},
              {"kind", CanonicalValue("carried")},
          });
        }
      },
      value.placement);
  return CanonicalValue(CanonicalValue::Object{
      {"collision_shapes", CanonicalValue(std::move(shapes))},
      {"placement", placement},
      {"provider_evidence", value.provider_evidence.has_value()
                                ? ProviderEvidence(*value.provider_evidence)
                                : CanonicalValue(nullptr)},
      {"scene_resource_id", CanonicalValue(value.scene_resource_id.value())},
      {"support_contacts", CanonicalValue(std::move(support_contacts))},
  });
}

CanonicalValue SceneSnapshot(const core::LogicalSceneSnapshotDTO &value) {
  Require(value.locally_valid(), "logical scene snapshot is invalid");
  CanonicalValue::Array objects;
  objects.reserve(value.objects.size());
  for (const auto &object : value.objects) {
    objects.emplace_back(SceneObject(object));
  }
  CanonicalValue::Array attachments;
  attachments.reserve(value.carried_object_attachments.size());
  for (const auto &attachment : value.carried_object_attachments) {
    attachments.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"carried_object_attachment_id",
         CanonicalValue(attachment.carried_object_attachment_id.value())},
        {"carried_object_attachment_revision",
         CanonicalValue(attachment.carried_object_attachment_revision.value())},
        {"carrier_frame_ref", SceneFrame(attachment.carrier_frame_ref)},
        {"command_target_id",
         CanonicalValue(attachment.command_target_id.value())},
        {"controllable_component_id",
         CanonicalValue(attachment.controllable_component_id.value())},
        {"pose_in_carrier_frame", Pose(attachment.pose_in_carrier_frame)},
        {"scene_resource_id",
         CanonicalValue(attachment.scene_resource_id.value())},
        {"touch_body_roles", StrongIdArray(attachment.touch_body_roles)},
    }));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"carried_object_attachments", CanonicalValue(std::move(attachments))},
      {"cell_id", CanonicalValue(value.cell_id.value())},
      {"configured_collision_policy",
       CollisionPolicy(value.configured_collision_policy)},
      {"objects", CanonicalValue(std::move(objects))},
      {"policy_epoch", Unsigned(value.policy_epoch, "policy_epoch")},
      {"revision",
       CanonicalValue(CanonicalValue::Object{
           {"content_hash",
            CanonicalValue(value.revision.content_hash.value())},
           {"revision_id",
            Unsigned(value.revision.revision_id, "scene revision_id", true)},
           {"scene_id", CanonicalValue(value.revision.scene_id.value())},
       })},
      {"topology_epoch", Unsigned(value.topology_epoch, "topology_epoch")},
      {"transaction_sequence",
       Unsigned(value.transaction_sequence, "transaction_sequence")},
  });
}

} // namespace

std::string_view
ToString(const ManagerApplyMutationAvailability availability) noexcept {
  switch (availability) {
  case ManagerApplyMutationAvailability::kAvailable:
    return "available";
  case ManagerApplyMutationAvailability::kKernelOwnerUnavailable:
    return "unavailable";
  case ManagerApplyMutationAvailability::kFullStackRestartRequired:
    return "full_stack_restart_required";
  }
  return {};
}

CanonicalValue BuildManagerSourcePayload(
    const manyforge::deployment::RetainedBundle &active_bundle,
    const component_manager::ManagerActiveRuntimeConvergenceResult &runtime,
    const component_manager::SqliteManagerApplyJournal &apply_journal,
    const ManagerApplyMutationAvailability apply_availability) {
  Require(manyforge::deployment::verify_retained_bundle_identity(active_bundle),
          "active retained bundle failed identity verification");
  Require(runtime.plan.valid(), "active runtime plan is invalid");
  const auto &publication = runtime.plan.active_publication;
  const auto &active_revision =
      publication.request.candidate_projection_revision;
  Require(active_bundle.retained_bundle_id() ==
                  active_revision.retained_bundle_id &&
              active_bundle.value().cell_graph.graph_revision ==
                  active_revision.graph_revision,
          "active runtime plan does not select the supplied retained bundle");
  Require(apply_journal.cell_id() == active_bundle.value().cell_graph.cell_id,
          "Apply journal belongs to a different cell");
  const auto *graph =
      active_bundle.normalized_root().find_object_member("cell_graph");
  Require(graph != nullptr && graph->is_object(),
          "verified retained bundle has no normalized cell_graph");

  CanonicalValue::Array desired;
  desired.reserve(runtime.plan.runtime_units.size());
  for (const auto &row : runtime.plan.runtime_units) {
    desired.emplace_back(RuntimePlanRow(row));
  }
  CanonicalValue::Array observed;
  observed.reserve(runtime.observations.observations().size());
  for (const auto &row : runtime.observations.observations()) {
    observed.emplace_back(RuntimeObservation(row));
  }
  CanonicalValue::Array applies;
  const auto apply_inventory = apply_journal.ReadStatusInventory();
  applies.reserve(apply_inventory.size());
  for (const auto &apply : apply_inventory) {
    Require(apply.intent.intent.cell_id == apply_journal.cell_id(),
            "Apply status belongs to a different cell");
    applies.emplace_back(ApplyStatus(apply));
  }

  const bool apply_available =
      apply_availability == ManagerApplyMutationAvailability::kAvailable;
  const auto reason = apply_available
                          ? CanonicalValue(nullptr)
                          : CanonicalValue(std::string(
                                apply_availability ==
                                        ManagerApplyMutationAvailability::
                                            kFullStackRestartRequired
                                    ? "full_stack_restart_required"
                                    : kKernelOwnerUnavailableReasonCode));
  const bool runtime_converged =
      runtime.state ==
          component_manager::ManagerActiveRuntimeConvergenceState::kConverged &&
      apply_availability !=
          ManagerApplyMutationAvailability::kFullStackRestartRequired;
  return CanonicalValue(CanonicalValue::Object{
      {"active",
       CanonicalValue(CanonicalValue::Object{
           {"cell_graph", *graph},
           {"graph_revision",
            CanonicalValue(active_revision.graph_revision.value())},
           {"publication", ActivePublication(publication)},
           {"retained_bundle_id",
            CanonicalValue(active_revision.retained_bundle_id.value())},
       })},
      {"api_version",
       CanonicalValue(std::string(kManagerSourcePayloadApiVersion))},
      {"apply", CanonicalValue(CanonicalValue::Object{
                    {"mutation_available", CanonicalValue(apply_available)},
                    {"records", CanonicalValue(std::move(applies))},
                    {"unavailable_reason_code", reason},
                })},
      {"cell_id",
       CanonicalValue(active_bundle.value().cell_graph.cell_id.value())},
      {"manager_ready", CanonicalValue(runtime_converged)},
      {"runtime", CanonicalValue(CanonicalValue::Object{
                      {"converged", CanonicalValue(runtime_converged)},
                      {"desired_units", CanonicalValue(std::move(desired))},
                      {"observed_units", CanonicalValue(std::move(observed))},
                  })},
  });
}

CanonicalValue BuildLogicalSceneSourcePayload(
    const manyforge::scene::CanonicalEmptySceneAccessStatus &access,
    const std::optional<core::LogicalSceneSnapshotRoot> &snapshot) {
  Require(access.valid(), "logical-scene access status is invalid");
  Require(access.active_projection.has_value() == snapshot.has_value(),
          "logical-scene access and snapshot presence disagree");
  if (snapshot.has_value()) {
    const auto &value = *snapshot->shared();
    Require(access.active_projection->scene_revision == value.revision,
            "logical-scene access and snapshot revisions disagree");
  }
  return CanonicalValue(CanonicalValue::Object{
      {"access",
       CanonicalValue(CanonicalValue::Object{
           {"active_projection",
            access.active_projection.has_value()
                ? ProjectionRevision(*access.active_projection)
                : CanonicalValue(nullptr)},
           {"phase", CanonicalValue(std::string(
                         manyforge::scene::ToString(access.phase)))},
           {"projection_leases_open",
            CanonicalValue(access.projection_leases_open)},
           {"scene_writes_open", CanonicalValue(access.scene_writes_open)},
       })},
      {"api_version",
       CanonicalValue(std::string(kLogicalSceneSourcePayloadApiVersion))},
      {"snapshot", snapshot.has_value() ? SceneSnapshot(*snapshot->shared())
                                        : CanonicalValue(nullptr)},
  });
}

CanonicalValue
BuildSceneRuntimeSourcePayload(const std::string_view cell_id,
                               const SceneRuntimeStateSnapshot &snapshot) {
  Require(!cell_id.empty() && cell_id.size() <= 256U,
          "scene/runtime source cell_id is invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kSceneRuntimeSourcePayloadApiVersion))},
      {"cell_id", CanonicalValue(std::string(cell_id))},
      {"state", SceneRuntimeStateValue(snapshot)},
  });
}

} // namespace manyforge::headless
