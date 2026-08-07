#include <manyforge_core/types/observed_state.hpp>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] ErrorDTO invalid_publication(std::string reason) {
  return ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                  "observed-state publication is invalid",
                  {{"reason", std::move(reason)}}};
}

}  // namespace

Result<void> validate_observed_state_publication(const ObservedStatePublicationDTO& publication,
                                                 const std::int64_t now_ns) {
  const auto& snapshot = publication.snapshot;
  if (publication.controllable_component_id != snapshot.controllable_component_id ||
      publication.command_target_id != snapshot.command_target_id ||
      publication.control_domain_id != snapshot.control_domain_id ||
      publication.control_domain_revision != snapshot.control_domain_revision ||
      publication.control_binding_id != snapshot.control_binding_id ||
      publication.control_binding_revision != snapshot.control_binding_revision ||
      publication.controller_topology_id != snapshot.controller_topology_id ||
      publication.topology_revision != snapshot.topology_revision ||
      publication.control_route_id != snapshot.control_route_id ||
      publication.active_control_configuration_id != snapshot.active_control_configuration_id ||
      publication.active_control_configuration_generation !=
          snapshot.active_control_configuration_generation ||
      publication.provider_instance_id != snapshot.provider_instance_id ||
      publication.provider_manifest_id != snapshot.provider_manifest_id ||
      publication.provider_manifest_revision != snapshot.provider_manifest_revision ||
      publication.state_authority_id != snapshot.state_authority_id ||
      publication.state_authority_class != snapshot.state_authority_class ||
      publication.runtime_epoch != snapshot.source_epoch ||
      publication.authority_generation != snapshot.authority_generation ||
      publication.publication_sequence != snapshot.sequence ||
      publication.received_timestamp_ns != snapshot.received_timestamp_ns) {
    return Result<void>::Err(invalid_publication("publication_snapshot_identity_mismatch"));
  }
  return validate_observed_state_snapshot(snapshot, now_ns);
}

}  // namespace manyforge::core
