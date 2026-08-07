#include <algorithm>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/external_observed_state_provider.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

constexpr std::size_t kMaximumExternalModelInstances = 256U;
constexpr std::size_t kMaximumExternalResources = 256U;

[[nodiscard]] ErrorDTO source_error(std::string reason, std::string message) {
  return ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                  std::move(message),
                  {{"reason", std::move(reason)}}};
}

[[nodiscard]] ErrorDTO source_unavailable(std::string reason) {
  return ErrorDTO{std::string(to_string(ErrorCode::kMissingReference)),
                  "external observed-state source is unavailable",
                  {{"reason", std::move(reason)}}};
}

template <typename StrongId>
[[nodiscard]] CanonicalValue identity(const StrongId& value) {
  return CanonicalValue(value.value());
}

[[nodiscard]] CanonicalValue text(const std::string_view value) {
  return CanonicalValue(std::string(value));
}

[[nodiscard]] CanonicalValue string_array(const std::vector<std::string>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

template <typename StrongId>
[[nodiscard]] CanonicalValue identity_array(const std::vector<StrongId>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.push_back(identity(value));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue coordinate_kind_array(const std::vector<CoordinateKind>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto value : values) {
    result.push_back(text(to_string(value)));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] bool source_matches_authority(const StateAuthorityClass authority,
                                            const ObservedStateSourceKind source) noexcept {
  switch (authority) {
    case StateAuthorityClass::kMeasured:
      return source == ObservedStateSourceKind::kActionFeedback ||
             source == ObservedStateSourceKind::kControllerState ||
             source == ObservedStateSourceKind::kJointState;
    case StateAuthorityClass::kCommanded:
      return source == ObservedStateSourceKind::kCommandedState;
    case StateAuthorityClass::kPredicted:
      return source == ObservedStateSourceKind::kPredictedState;
  }
  return false;
}

[[nodiscard]] bool context_matches_publication(
    const ExternalObservedStateSourceContractDTO& contract,
    const ObservedStatePublicationDTO& publication) {
  const auto& context = contract.query_context;
  const auto& snapshot = publication.snapshot;
  if (publication.controllable_component_id != context.controllable_component_id ||
      publication.command_target_id != context.command_target_id ||
      publication.control_domain_id != context.control_domain_id ||
      publication.control_domain_revision != context.control_domain_revision ||
      publication.control_binding_id != context.control_binding_id ||
      publication.control_binding_revision != context.control_binding_revision ||
      publication.controller_topology_id != context.controller_topology_id ||
      publication.topology_revision != context.topology_revision ||
      publication.control_route_id != context.control_route_id ||
      publication.active_control_configuration_id != context.active_control_configuration_id ||
      publication.active_control_configuration_generation !=
          context.active_control_configuration_generation ||
      publication.provider_instance_id != context.provider_instance_id ||
      publication.provider_manifest_id != context.provider_manifest_id ||
      publication.provider_manifest_revision != context.provider_manifest_revision ||
      publication.state_authority_id != context.state_authority_id ||
      publication.state_authority_class != context.state_authority_class ||
      publication.runtime_epoch != context.runtime_epoch ||
      snapshot.resources != context.resources || snapshot.joint_names != context.joint_names ||
      snapshot.source_id != contract.source_id ||
      snapshot.source_kind != contract.publication_source ||
      snapshot.freshness_budget_ns != context.freshness_budget_ns ||
      snapshot.positions.size() != context.coordinate_kinds.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < snapshot.positions.size(); ++index) {
    if (coordinate_kind(snapshot.positions[index]) != context.coordinate_kinds[index]) {
      return false;
    }
  }
  return true;
}

}  // namespace

Result<void> validate_external_observed_state_source_contract(
    const ExternalObservedStateSourceContractDTO& contract) {
  const auto& context = contract.query_context;
  if (contract.cell_id.empty() || contract.retained_bundle_id.empty() ||
      contract.graph_revision.empty() || !contract.projection_stage_generation.nonzero() ||
      contract.receiver_runtime_unit_id.empty() || contract.receiver_runtime_epoch.empty() ||
      contract.source_runtime_unit_id.empty() || contract.source_id.empty() ||
      context.controllable_component_id.empty() || context.command_target_id.empty() ||
      context.capability_id.empty() || context.control_domain_id.empty() ||
      context.control_domain_revision.empty() || context.control_binding_id.empty() ||
      context.control_binding_revision.empty() || context.controller_topology_id.empty() ||
      context.topology_revision.empty() || context.control_route_id.empty() ||
      context.control_interface_contract_id.empty() || context.control_interface_revision.empty() ||
      context.active_control_configuration_id.empty() ||
      !context.active_control_configuration_generation.nonzero() ||
      context.provider_instance_id.empty() || context.provider_manifest_id.empty() ||
      context.provider_manifest_revision.empty() || context.runtime_epoch.empty() ||
      context.state_authority_id.empty()) {
    return Result<void>::Err(source_error("external_source_identity_incomplete",
                                          "external observed-state source identity is incomplete"));
  }
  if (contract.model_instance_ids.empty() ||
      contract.model_instance_ids.size() > kMaximumExternalModelInstances ||
      !std::is_sorted(contract.model_instance_ids.begin(), contract.model_instance_ids.end()) ||
      std::adjacent_find(contract.model_instance_ids.begin(), contract.model_instance_ids.end()) !=
          contract.model_instance_ids.end() ||
      std::any_of(contract.model_instance_ids.begin(), contract.model_instance_ids.end(),
                  [](const ModelInstanceId& value) { return value.empty(); })) {
    return Result<void>::Err(source_error(
        "external_source_models_noncanonical",
        "external source model instances must be non-empty, bounded, sorted, and unique"));
  }
  if (context.resources.empty() || context.resources.size() > kMaximumExternalResources ||
      context.resources.size() != context.joint_names.size() ||
      context.resources.size() != context.coordinate_kinds.size()) {
    return Result<void>::Err(
        source_error("external_source_resource_shape_invalid",
                     "external source resources, names, and coordinate kinds must align"));
  }
  std::set<ResourceId> resources;
  std::set<std::string> names;
  for (std::size_t index = 0U; index < context.resources.size(); ++index) {
    if (context.resources[index].empty() || context.joint_names[index].empty() ||
        !resources.insert(context.resources[index]).second ||
        !names.insert(context.joint_names[index]).second ||
        to_string(context.coordinate_kinds[index]).empty()) {
      return Result<void>::Err(
          source_error("external_source_resource_identity_invalid",
                       "external source resources and names must be non-empty and unique"));
    }
  }
  if (context.freshness_budget_ns < 0 ||
      (context.state_authority_class == StateAuthorityClass::kMeasured &&
       context.freshness_budget_ns <= 0) ||
      !source_matches_authority(context.state_authority_class, contract.publication_source)) {
    return Result<void>::Err(source_error(
        "external_source_authority_invalid",
        "external source authority, publication kind, and freshness are incompatible"));
  }
  return Result<void>::Ok();
}

Result<ContentDigest> external_observed_state_source_contract_digest(
    const ExternalObservedStateSourceContractDTO& contract) {
  auto validation = validate_external_observed_state_source_contract(contract);
  if (!validation.ok()) {
    return Result<ContentDigest>::Err(validation.error());
  }
  const auto& context = contract.query_context;
  try {
    const CanonicalValue payload(CanonicalValue::Object{
        {"cell_id", identity(contract.cell_id)},
        {"retained_bundle_id", identity(contract.retained_bundle_id)},
        {"graph_revision", identity(contract.graph_revision)},
        {"projection_stage_generation",
         text(std::to_string(contract.projection_stage_generation.value()))},
        {"receiver_runtime_unit_id", identity(contract.receiver_runtime_unit_id)},
        {"receiver_runtime_epoch", identity(contract.receiver_runtime_epoch)},
        {"source_runtime_unit_id", identity(contract.source_runtime_unit_id)},
        {"model_instance_ids", identity_array(contract.model_instance_ids)},
        {"controllable_component_id", identity(context.controllable_component_id)},
        {"command_target_id", identity(context.command_target_id)},
        {"capability_id", identity(context.capability_id)},
        {"control_domain_id", identity(context.control_domain_id)},
        {"control_domain_revision", identity(context.control_domain_revision)},
        {"control_binding_id", identity(context.control_binding_id)},
        {"control_binding_revision", identity(context.control_binding_revision)},
        {"controller_topology_id", identity(context.controller_topology_id)},
        {"topology_revision", identity(context.topology_revision)},
        {"control_route_id", identity(context.control_route_id)},
        {"resources", identity_array(context.resources)},
        {"joint_names", string_array(context.joint_names)},
        {"coordinate_kinds", coordinate_kind_array(context.coordinate_kinds)},
        {"control_interface_contract_id", identity(context.control_interface_contract_id)},
        {"control_interface_revision", identity(context.control_interface_revision)},
        {"active_control_configuration_id", identity(context.active_control_configuration_id)},
        {"active_control_configuration_generation",
         text(std::to_string(context.active_control_configuration_generation.value()))},
        {"provider_instance_id", identity(context.provider_instance_id)},
        {"provider_manifest_id", identity(context.provider_manifest_id)},
        {"provider_manifest_revision", identity(context.provider_manifest_revision)},
        {"runtime_epoch", identity(context.runtime_epoch)},
        {"state_authority_id", identity(context.state_authority_id)},
        {"state_authority_class", text(to_string(context.state_authority_class))},
        {"freshness_budget_ns", text(std::to_string(context.freshness_budget_ns))},
        {"source_id", CanonicalValue(contract.source_id)},
        {"publication_source", text(to_string(contract.publication_source))}});
    return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
        "manyforge.external_observed_state_source_contract",
        "manyforge.external_observed_state_source_contract.v1", payload)));
  } catch (const std::exception&) {
    return Result<ContentDigest>::Err(
        source_error("external_source_contract_not_canonical",
                     "external observed-state source contract is not canonical"));
  }
}

struct ExternalObservedStateProvider::LatestPublication final {
  ObservedStatePublicationDTO publication;
};

ExternalObservedStateProvider::ExternalObservedStateProvider(
    ExternalObservedStateSourceContractDTO contract)
    : contract_(std::move(contract)), contract_digest_(std::string{}), latest_(nullptr) {
  auto digest = external_observed_state_source_contract_digest(contract_);
  if (!digest.ok()) {
    throw std::invalid_argument(digest.error().message);
  }
  contract_digest_ = std::move(digest.value());
}

ExternalObservedStateProvider::~ExternalObservedStateProvider() = default;

Result<ExternalObservedStateIngestDisposition> ExternalObservedStateProvider::ingest(
    ExternalObservedStatePublicationDTO received) {
  if (!content_identity::digest_equal(received.source_contract_digest.value(),
                                      contract_digest_.value())) {
    return Result<ExternalObservedStateIngestDisposition>::Err(
        source_error("external_source_contract_mismatch",
                     "external observed-state sample belongs to another source contract"));
  }
  if (received.receiver_received_timestamp_ns < 0) {
    return Result<ExternalObservedStateIngestDisposition>::Err(
        source_error("external_source_receipt_time_invalid",
                     "external observed-state receipt time must be non-negative"));
  }

  auto candidate = std::move(received.publication);
  candidate.received_timestamp_ns = received.receiver_received_timestamp_ns;
  candidate.snapshot.received_timestamp_ns = received.receiver_received_timestamp_ns;
  if (!context_matches_publication(contract_, candidate)) {
    return Result<ExternalObservedStateIngestDisposition>::Err(source_error(
        "external_source_publication_context_mismatch",
        "external observed-state publication differs from its exact instance contract"));
  }
  auto valid =
      validate_observed_state_publication(candidate, received.receiver_received_timestamp_ns);
  if (!valid.ok()) {
    return Result<ExternalObservedStateIngestDisposition>::Err(valid.error());
  }

  const std::lock_guard lock(ingress_mutex_);
  const auto previous = latest_.load(std::memory_order_acquire);
  if (previous) {
    const auto& current = previous->publication;
    if (candidate.publication_sequence < current.publication_sequence ||
        candidate.authority_generation < current.authority_generation ||
        candidate.received_timestamp_ns < current.received_timestamp_ns) {
      return Result<ExternalObservedStateIngestDisposition>::Err(source_error(
          "external_source_publication_stale", "external observed-state publication regressed"));
    }
    if (candidate.publication_sequence == current.publication_sequence) {
      candidate.received_timestamp_ns = current.received_timestamp_ns;
      candidate.snapshot.received_timestamp_ns = current.snapshot.received_timestamp_ns;
      if (candidate == current) {
        return Result<ExternalObservedStateIngestDisposition>::Ok(
            ExternalObservedStateIngestDisposition::kDuplicate);
      }
      return Result<ExternalObservedStateIngestDisposition>::Err(
          source_error("external_source_publication_sequence_conflict",
                       "one external source sequence names different state"));
    }
  }

  latest_.store(std::make_shared<const LatestPublication>(LatestPublication{std::move(candidate)}),
                std::memory_order_release);
  return Result<ExternalObservedStateIngestDisposition>::Ok(
      ExternalObservedStateIngestDisposition::kAccepted);
}

Result<ObservedStateSnapshotDTO> ExternalObservedStateProvider::current_state(
    const ObservedStateQueryContextDTO& context, const std::int64_t now_ns) const {
  if (context != contract_.query_context) {
    return Result<ObservedStateSnapshotDTO>::Err(
        source_error("external_source_query_context_mismatch",
                     "external observed-state query differs from its exact instance contract"));
  }
  const auto latest = latest_.load(std::memory_order_acquire);
  if (!latest) {
    return Result<ObservedStateSnapshotDTO>::Err(
        source_unavailable("external_source_not_published"));
  }
  auto valid = validate_observed_state_publication(latest->publication, now_ns);
  if (!valid.ok()) {
    return Result<ObservedStateSnapshotDTO>::Err(valid.error());
  }
  return Result<ObservedStateSnapshotDTO>::Ok(latest->publication.snapshot);
}

Result<ObservedStatePublicationDTO> ExternalObservedStateProvider::publication(
    const ControlBindingId& control_binding_id,
    const ControlBindingRevision& control_binding_revision) const {
  const auto& context = contract_.query_context;
  if (control_binding_id != context.control_binding_id ||
      control_binding_revision != context.control_binding_revision) {
    return Result<ObservedStatePublicationDTO>::Err(
        source_error("external_source_binding_mismatch",
                     "external observed-state binding differs from its exact source contract"));
  }
  const auto latest = latest_.load(std::memory_order_acquire);
  if (!latest) {
    return Result<ObservedStatePublicationDTO>::Err(
        source_unavailable("external_source_not_published"));
  }
  return Result<ObservedStatePublicationDTO>::Ok(latest->publication);
}

}  // namespace manyforge::core
