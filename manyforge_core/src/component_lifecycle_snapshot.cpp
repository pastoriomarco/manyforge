#include <cstdint>
#include <exception>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <string>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumIJsonInteger = 9'007'199'254'740'991ULL;

[[nodiscard]] CanonicalValue safe_unsigned(const std::uint64_t value) {
  if (value > kMaximumIJsonInteger) {
    throw content_identity::CanonicalValueError(
        "component-lifecycle snapshot counter exceeds the I-JSON safe range");
  }
  return CanonicalValue(static_cast<double>(value));
}

template <typename StrongId>
[[nodiscard]] CanonicalValue identity(const StrongId& value) {
  return CanonicalValue(value.value());
}

template <typename StrongCounter>
[[nodiscard]] CanonicalValue counter(const StrongCounter& value) {
  return safe_unsigned(value.value());
}

[[nodiscard]] CanonicalValue scene_revision_value(const SceneRevision& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"content_hash", identity(value.content_hash)},
      {"revision_id", safe_unsigned(value.revision_id)},
      {"scene_id", identity(value.scene_id)},
  });
}

[[nodiscard]] CanonicalValue projection_revision_value(const ProjectionRevisionDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"graph_revision", identity(value.graph_revision)},
      {"retained_bundle_id", identity(value.retained_bundle_id)},
      {"scene_revision", scene_revision_value(value.scene_revision)},
  });
}

[[nodiscard]] CanonicalValue publication_transaction_value(
    const ActiveBundlePublicationTransactionRefDTO& value) {
  return std::visit(
      [](const auto& selected) -> CanonicalValue {
        using T = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<T, NamedApplyPublicationTransactionDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"apply_generation", counter(selected.apply_generation)},
              {"apply_id", identity(selected.apply_id)},
              {"kind", CanonicalValue("named_apply")},
              {"projection_stage_generation", counter(selected.projection_stage_generation)},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"bootstrap_generation", counter(selected.bootstrap_generation)},
              {"bootstrap_id", identity(selected.bootstrap_id)},
              {"kind", CanonicalValue("bootstrap")},
          });
        }
      },
      value);
}

[[nodiscard]] CanonicalValue publication_request_value(
    const ActiveBundlePublicationRequestDTO& value) {
  CanonicalValue::Object members{
      {"candidate_graph_revision", identity(value.candidate_graph_revision)},
      {"candidate_projection_revision",
       projection_revision_value(value.candidate_projection_revision)},
      {"candidate_retained_bundle_id", identity(value.candidate_retained_bundle_id)},
      {"obsolete_managed_command_surfaces_retired_evidence_digest",
       identity(value.obsolete_managed_command_surfaces_retired_evidence_digest)},
      {"private_materialization_content_digest",
       identity(value.private_materialization_content_digest)},
      {"transaction_ref", publication_transaction_value(value.transaction_ref)},
  };
  if (value.expected_previous_publication_generation.has_value()) {
    members.emplace_back("expected_previous_publication_generation",
                         counter(*value.expected_previous_publication_generation));
  }
  return CanonicalValue(std::move(members));
}

[[nodiscard]] CanonicalValue publication_receipt_value(
    const ActiveBundlePublicationReceiptDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"durable_public_pointer_content_digest",
       identity(value.durable_public_pointer_content_digest)},
      {"publication_generation", counter(value.publication_generation)},
      {"request", publication_request_value(value.request)},
  });
}

[[nodiscard]] CanonicalValue active_binding_route_value(
    const ActiveControlConfigurationBindingRouteDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"control_binding_id", identity(value.control_binding_id)},
      {"control_binding_revision", identity(value.control_binding_revision)},
      {"control_route_id", identity(value.control_route_id)},
  });
}

[[nodiscard]] CanonicalValue kinematic_binding_value(
    const KinematicControlBindingSourceDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"control_binding_id", identity(value.control_binding_id)},
      {"control_binding_revision", identity(value.control_binding_revision)},
  });
}

[[nodiscard]] CanonicalValue kinematic_projection_value(
    const KinematicResourceSourceProjectionDTO& value) {
  CanonicalValue::Array bindings;
  bindings.reserve(value.contributing_control_bindings.size());
  for (const auto& binding : value.contributing_control_bindings) {
    bindings.emplace_back(kinematic_binding_value(binding));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"contributing_control_bindings", CanonicalValue(std::move(bindings))},
      {"provider_instance_id", identity(value.provider_instance_id)},
      {"provider_manifest_id", identity(value.provider_manifest_id)},
      {"provider_manifest_revision", identity(value.provider_manifest_revision)},
      {"publication_source", CanonicalValue(std::string(to_string(value.publication_source)))},
      {"resource_id", identity(value.resource_id)},
      {"state_authority_class",
       CanonicalValue(std::string(to_string(value.state_authority_class)))},
      {"state_authority_id", identity(value.state_authority_id)},
  });
}

[[nodiscard]] CanonicalValue active_configuration_value(
    const ActiveControlConfigurationDTO& value) {
  CanonicalValue::Array bindings;
  bindings.reserve(value.binding_routes.size());
  for (const auto& binding : value.binding_routes) {
    bindings.emplace_back(active_binding_route_value(binding));
  }
  CanonicalValue::Array projections;
  projections.reserve(value.kinematic_resource_source_projections.size());
  for (const auto& projection : value.kinematic_resource_source_projections) {
    projections.emplace_back(kinematic_projection_value(projection));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"activation_set_id", identity(value.activation_set_id)},
      {"active_control_configuration_generation",
       counter(value.active_control_configuration_generation)},
      {"active_control_configuration_id", identity(value.active_control_configuration_id)},
      {"binding_routes", CanonicalValue(std::move(bindings))},
      {"control_domain_id", identity(value.control_domain_id)},
      {"control_domain_revision", identity(value.control_domain_revision)},
      {"controller_topology_id", identity(value.controller_topology_id)},
      {"kinematic_resource_source_projections", CanonicalValue(std::move(projections))},
      {"source_runtime_epoch", identity(value.source_runtime_epoch)},
      {"source_runtime_unit_id", identity(value.source_runtime_unit_id)},
      {"source_sequence", safe_unsigned(value.source_sequence)},
      {"state", CanonicalValue(std::string(to_string(value.state)))},
      {"topology_revision", identity(value.topology_revision)},
  });
}

[[nodiscard]] std::string disable_request_kind_token(
    const DisableHandoffScopeRequestKindDTO value) {
  switch (value) {
    case DisableHandoffScopeRequestKindDTO::kResolvedComponentScope:
      return "resolved_component_scope";
    case DisableHandoffScopeRequestKindDTO::kFullStopRequired:
      return "full_stop_required";
  }
  throw content_identity::CanonicalValueError("unknown Disable request kind");
}

[[nodiscard]] std::string disable_resolution_token(const DisableHandoffScopeResolutionDTO value) {
  switch (value) {
    case DisableHandoffScopeResolutionDTO::kExact:
      return "exact";
    case DisableHandoffScopeResolutionDTO::kStaleButUnambiguouslyResolved:
      return "stale_but_unambiguously_resolved";
    case DisableHandoffScopeResolutionDTO::kPromotedToFull:
      return "promoted_to_full";
  }
  throw content_identity::CanonicalValueError("unknown Disable scope resolution");
}

[[nodiscard]] std::string expected_configuration_presence_token(
    const ExpectedActiveControlConfigurationPresenceDTO value) {
  switch (value) {
    case ExpectedActiveControlConfigurationPresenceDTO::kAbsent:
      return "absent";
    case ExpectedActiveControlConfigurationPresenceDTO::kPresent:
      return "present";
  }
  throw content_identity::CanonicalValueError("unknown active-configuration presence");
}

[[nodiscard]] CanonicalValue fence_scope_value(const ConfigurationFenceScopeDTO& value) {
  CanonicalValue::Array components;
  components.reserve(value.controllable_component_ids.size());
  for (const auto& component : value.controllable_component_ids) {
    components.emplace_back(identity(component));
  }
  CanonicalValue::Array domains;
  domains.reserve(value.domains.size());
  for (const auto& domain : value.domains) {
    CanonicalValue::Object row{{"control_domain_id", identity(domain.control_domain_id)}};
    if (domain.candidate_control_domain_revision.has_value()) {
      row.emplace_back("candidate_control_domain_revision",
                       identity(*domain.candidate_control_domain_revision));
    }
    if (domain.predecessor_control_domain_revision.has_value()) {
      row.emplace_back("predecessor_control_domain_revision",
                       identity(*domain.predecessor_control_domain_revision));
    }
    domains.emplace_back(CanonicalValue(std::move(row)));
  }
  CanonicalValue::Array bindings;
  bindings.reserve(value.bindings.size());
  for (const auto& binding : value.bindings) {
    CanonicalValue::Object row{{"control_binding_id", identity(binding.control_binding_id)}};
    if (binding.candidate_control_binding_revision.has_value()) {
      row.emplace_back("candidate_control_binding_revision",
                       identity(*binding.candidate_control_binding_revision));
    }
    if (binding.predecessor_control_binding_revision.has_value()) {
      row.emplace_back("predecessor_control_binding_revision",
                       identity(*binding.predecessor_control_binding_revision));
    }
    bindings.emplace_back(CanonicalValue(std::move(row)));
  }
  CanonicalValue::Array resources;
  resources.reserve(value.resources.size());
  for (const auto& resource : value.resources) {
    resources.emplace_back(identity(resource));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"bindings", CanonicalValue(std::move(bindings))},
      {"controllable_component_ids", CanonicalValue(std::move(components))},
      {"domains", CanonicalValue(std::move(domains))},
      {"resources", CanonicalValue(std::move(resources))},
  });
}

[[nodiscard]] CanonicalValue expected_active_configuration_value(
    const ExpectedActiveControlConfigurationDTO& value) {
  CanonicalValue::Object members{
      {"presence", CanonicalValue(expected_configuration_presence_token(value.presence))}};
  if (value.active_control_configuration_generation.has_value()) {
    members.emplace_back("active_control_configuration_generation",
                         counter(*value.active_control_configuration_generation));
  }
  if (value.active_control_configuration_id.has_value()) {
    members.emplace_back("active_control_configuration_id",
                         identity(*value.active_control_configuration_id));
  }
  return CanonicalValue(std::move(members));
}

[[nodiscard]] CanonicalValue current_active_configurations_value(
    const std::vector<CurrentDomainActiveControlConfigurationDTO>& values) {
  CanonicalValue::Array rows;
  rows.reserve(values.size());
  for (const auto& value : values) {
    rows.emplace_back(CanonicalValue::Object{
        {"active_control_configuration",
         expected_active_configuration_value(value.active_control_configuration)},
        {"control_domain_id", identity(value.control_domain_id)},
    });
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue disable_component_expectation_value(
    const DisableHandoffComponentScopeExpectationDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"active_public_pointer_digest", identity(value.active_public_pointer_digest)},
      {"active_publication_generation", counter(value.active_publication_generation)},
      {"component_lifecycle_generation", counter(value.component_lifecycle_generation)},
      {"component_scope", fence_scope_value(value.component_scope)},
      {"component_snapshot_digest", identity(value.component_snapshot_digest)},
      {"current_active_control_configurations",
       current_active_configurations_value(value.current_active_control_configurations)},
      {"graph_revision", identity(value.graph_revision)},
      {"retained_bundle_id", identity(value.retained_bundle_id)},
  });
}

[[nodiscard]] CanonicalValue disable_query_value(const DisableHandoffAdmissionQueryDTO& value) {
  CanonicalValue::Object members{
      {"controllable_component_id", identity(value.controllable_component_id)},
      {"request_kind", CanonicalValue(disable_request_kind_token(value.request_kind))},
  };
  if (value.component_expectation.has_value()) {
    members.emplace_back("component_expectation",
                         disable_component_expectation_value(*value.component_expectation));
  }
  return CanonicalValue(std::move(members));
}

[[nodiscard]] CanonicalValue disable_admission_snapshot_value(
    const DisableHandoffAdmissionSnapshotDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"active_public_pointer_digest", identity(value.active_public_pointer_digest)},
      {"active_publication_generation", counter(value.active_publication_generation)},
      {"configuration_fence_generation", counter(value.configuration_fence_generation)},
      {"current_active_control_configurations",
       current_active_configurations_value(value.current_active_control_configurations)},
      {"current_scope", fence_scope_value(value.current_scope)},
      {"graph_revision", identity(value.graph_revision)},
      {"operation_resource_generation", safe_unsigned(value.operation_resource_generation)},
      {"query", disable_query_value(value.query)},
      {"request_owner_epoch", identity(value.request_owner_epoch)},
      {"retained_bundle_id", identity(value.retained_bundle_id)},
      {"scope_resolution", CanonicalValue(disable_resolution_token(value.scope_resolution))},
  });
}

[[nodiscard]] CanonicalValue current_binding_value(const ExpectedControlBindingRevisionDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"control_binding_id", identity(value.control_binding_id)},
      {"control_binding_revision", identity(value.control_binding_revision)},
      {"control_route_id", identity(value.control_route_id)},
  });
}

[[nodiscard]] CanonicalValue domain_value(const ResolvedLifecycleControlDomainScopeDTO& value) {
  CanonicalValue::Array bindings;
  bindings.reserve(value.current_control_bindings.size());
  for (const auto& binding : value.current_control_bindings) {
    bindings.emplace_back(current_binding_value(binding));
  }
  CanonicalValue::Array resources;
  resources.reserve(value.current_resources.size());
  for (const auto& resource : value.current_resources) {
    resources.emplace_back(identity(resource));
  }
  CanonicalValue::Object members{
      {"control_domain_id", identity(value.control_domain_id)},
      {"control_domain_revision", identity(value.control_domain_revision)},
      {"controller_topology_id", identity(value.controller_topology_id)},
      {"current_control_bindings", CanonicalValue(std::move(bindings))},
      {"current_resources", CanonicalValue(std::move(resources))},
      {"topology_revision", identity(value.topology_revision)},
  };
  if (value.active_control_configuration.has_value()) {
    members.emplace_back("active_control_configuration",
                         active_configuration_value(*value.active_control_configuration));
  }
  return CanonicalValue(std::move(members));
}

[[nodiscard]] CanonicalValue provider_value(const ResolvedLifecycleProviderOccurrenceDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"control_domain_id", identity(value.control_domain_id)},
      {"control_domain_revision", identity(value.control_domain_revision)},
      {"lifecycle_plan_catalog_digest", identity(value.lifecycle_plan_catalog_digest)},
      {"provider_instance_id", identity(value.provider_instance_id)},
      {"provider_manifest_id", identity(value.provider_manifest_id)},
      {"provider_manifest_revision", identity(value.provider_manifest_revision)},
      {"provider_runtime_epoch", identity(value.provider_runtime_epoch)},
      {"provider_runtime_unit_id", identity(value.provider_runtime_unit_id)},
  });
}

[[nodiscard]] CanonicalValue snapshot_value(const ComponentLifecycleCurrentSnapshotDTO& value) {
  CanonicalValue::Array domains;
  domains.reserve(value.domains.size());
  for (const auto& domain : value.domains) {
    domains.emplace_back(domain_value(domain));
  }
  CanonicalValue::Array providers;
  providers.reserve(value.provider_occurrences.size());
  for (const auto& provider : value.provider_occurrences) {
    providers.emplace_back(provider_value(provider));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"active_publication_receipt", publication_receipt_value(value.active_publication_receipt)},
      {"component_lifecycle_generation", counter(value.component_lifecycle_generation)},
      {"component_lifecycle_state",
       CanonicalValue(std::string(to_string(value.component_lifecycle_state)))},
      {"controllable_component_id", identity(value.controllable_component_id)},
      {"domains", CanonicalValue(std::move(domains))},
      {"provider_occurrences", CanonicalValue(std::move(providers))},
  });
}

[[nodiscard]] ErrorDTO snapshot_error(std::string reason) {
  return ErrorDTO{std::string(to_string(ErrorCode::kInvariantViolation)),
                  "component lifecycle current snapshot is not one canonical "
                  "verified value",
                  {{"reason", std::move(reason)}}};
}

[[nodiscard]] ComponentLifecycleCurrentSnapshotDTO semantic_shape_copy(
    const ComponentLifecycleCurrentSnapshotDTO& snapshot) {
  auto copy = snapshot;
  copy.snapshot_digest = ContentDigest(std::string("sha256:") + std::string(64U, '0'));
  return copy;
}

[[nodiscard]] DisableHandoffAdmissionSnapshotDTO semantic_shape_copy(
    const DisableHandoffAdmissionSnapshotDTO& snapshot) {
  auto copy = snapshot;
  copy.snapshot_digest = ContentDigest(std::string("sha256:") + std::string(64U, '0'));
  return copy;
}

[[nodiscard]] ErrorDTO disable_snapshot_error(std::string reason) {
  return ErrorDTO{std::string(to_string(ErrorCode::kInvariantViolation)),
                  "Kernel Disable admission snapshot is not one canonical verified value",
                  {{"reason", std::move(reason)}}};
}

}  // namespace

Result<ContentDigest> ComputeComponentLifecycleCurrentSnapshotDigest(
    const ComponentLifecycleCurrentSnapshotDTO& snapshot) {
  if (!semantic_shape_copy(snapshot).locally_valid()) {
    return Result<ContentDigest>::Err(snapshot_error("snapshot_shape_invalid"));
  }
  try {
    return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
        kComponentLifecycleCurrentSnapshotIdentityDomain,
        kComponentLifecycleCurrentSnapshotApiVersion, snapshot_value(snapshot))));
  } catch (const std::exception&) {
    return Result<ContentDigest>::Err(snapshot_error("snapshot_identity_not_canonical"));
  }
}

Result<void> VerifyComponentLifecycleCurrentSnapshot(
    const ComponentLifecycleCurrentSnapshotDTO& snapshot) {
  if (!snapshot.locally_valid()) {
    return Result<void>::Err(snapshot_error("snapshot_shape_invalid"));
  }
  const auto computed = ComputeComponentLifecycleCurrentSnapshotDigest(snapshot);
  if (!computed.ok()) {
    return Result<void>::Err(computed.error());
  }
  if (computed.value() != snapshot.snapshot_digest) {
    return Result<void>::Err(snapshot_error("snapshot_digest_mismatch"));
  }
  return Result<void>::Ok();
}

Result<ContentDigest> ComputeDisableHandoffAdmissionSnapshotDigest(
    const DisableHandoffAdmissionSnapshotDTO& snapshot) {
  if (!semantic_shape_copy(snapshot).locally_valid()) {
    return Result<ContentDigest>::Err(disable_snapshot_error("snapshot_shape_invalid"));
  }
  try {
    return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
        kDisableHandoffAdmissionSnapshotIdentityDomain, kDisableHandoffAdmissionSnapshotApiVersion,
        disable_admission_snapshot_value(snapshot))));
  } catch (const std::exception&) {
    return Result<ContentDigest>::Err(disable_snapshot_error("snapshot_identity_not_canonical"));
  }
}

Result<void> VerifyDisableHandoffAdmissionSnapshot(
    const DisableHandoffAdmissionSnapshotDTO& snapshot) {
  if (!snapshot.locally_valid()) {
    return Result<void>::Err(disable_snapshot_error("snapshot_shape_invalid"));
  }
  const auto computed = ComputeDisableHandoffAdmissionSnapshotDigest(snapshot);
  if (!computed.ok()) {
    return Result<void>::Err(computed.error());
  }
  if (computed.value() != snapshot.snapshot_digest) {
    return Result<void>::Err(disable_snapshot_error("snapshot_digest_mismatch"));
  }
  return Result<void>::Ok();
}

Result<ContentDigest> ComputeDisableHandoffScopeDigest(const ConfigurationFenceScopeDTO& scope) {
  if (!scope.valid()) {
    return Result<ContentDigest>::Err(disable_snapshot_error("scope_shape_invalid"));
  }
  try {
    return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
        kDisableHandoffScopeIdentityDomain, kDisableHandoffAdmissionSnapshotApiVersion,
        fence_scope_value(scope))));
  } catch (const std::exception&) {
    return Result<ContentDigest>::Err(disable_snapshot_error("scope_identity_not_canonical"));
  }
}

}  // namespace manyforge::core
