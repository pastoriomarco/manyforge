#include <algorithm>
#include <cmath>
#include <iterator>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/kinematic_state_view_provider.hpp>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;
using SourceKey = std::pair<ControlBindingId, ControlBindingRevision>;
using ModelKey = std::tuple<ControllableComponentId, ModelInstanceId, ModelRevision>;

[[nodiscard]] ErrorDTO state_view_error(std::string reason, std::string message) {
  return ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                  std::move(message),
                  {{"reason", std::move(reason)}}};
}

[[nodiscard]] bool finite_pose(const PlacementPoseDTO& pose) noexcept {
  double norm_squared = 0.0;
  for (const double value : pose.position_m) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  for (const double value : pose.orientation_xyzw) {
    if (!std::isfinite(value)) {
      return false;
    }
    norm_squared += value * value;
  }
  return std::isfinite(norm_squared) && std::abs(norm_squared - 1.0) <= 1.0e-6;
}

[[nodiscard]] const std::string& segment_parent_link(
    const KinematicFramePathSegmentDTO& segment) noexcept {
  return std::visit(
      [](const auto& value) -> const std::string& { return value.compiled_parent_link_name; },
      segment);
}

[[nodiscard]] const std::string& segment_child_link(
    const KinematicFramePathSegmentDTO& segment) noexcept {
  return std::visit(
      [](const auto& value) -> const std::string& { return value.compiled_child_link_name; },
      segment);
}

[[nodiscard]] bool valid_fixed_segment(const KinematicFramePathSegmentDTO& segment) noexcept {
  return std::visit(
      [](const auto& value) {
        using Segment = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Segment, KinematicModelJointPathSegmentDTO>) {
          return true;
        } else if constexpr (std::is_same_v<Segment, KinematicDirectModelAnchorPathSegmentDTO>) {
          return !value.model_instance_id.empty() && !value.model_revision.empty() &&
                 !value.compiled_joint_name.empty() && !value.compiled_parent_link_name.empty() &&
                 !value.compiled_child_link_name.empty() && finite_pose(value.child_pose_in_parent);
        } else if constexpr (std::is_same_v<Segment,
                                            KinematicDirectComponentAnchorPathSegmentDTO>) {
          return !value.controllable_component_id.empty() &&
                 !value.root_model_instance_id.empty() && !value.root_model_revision.empty() &&
                 !value.compiled_joint_name.empty() && !value.compiled_parent_link_name.empty() &&
                 !value.compiled_child_link_name.empty() && finite_pose(value.child_pose_in_parent);
        } else if constexpr (std::is_same_v<Segment, KinematicModelMountPathSegmentDTO>) {
          return !value.controllable_component_id.empty() &&
                 !value.parent_model_instance_id.empty() && !value.parent_model_revision.empty() &&
                 !value.child_model_instance_id.empty() && !value.child_model_revision.empty() &&
                 value.parent_model_instance_id != value.child_model_instance_id &&
                 !value.compiled_joint_name.empty() && !value.compiled_parent_link_name.empty() &&
                 !value.compiled_child_link_name.empty() && finite_pose(value.child_pose_in_parent);
        } else {
          return !value.component_attachment_id.empty() && !value.parent_component_id.empty() &&
                 !value.child_component_id.empty() &&
                 value.parent_component_id != value.child_component_id &&
                 !value.compiled_joint_name.empty() && !value.compiled_parent_link_name.empty() &&
                 !value.compiled_child_link_name.empty() && finite_pose(value.child_pose_in_parent);
        }
      },
      segment);
}

[[nodiscard]] bool valid_terminal_frame(const ResolvedFrameDTO& frame) noexcept {
  return std::visit(
      [](const auto& value) {
        using Frame = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Frame, ResolvedCellFrameDTO>) {
          return !value.cell_id.empty() && !value.compiled_frame_name.empty();
        } else if constexpr (std::is_same_v<Frame, ResolvedComponentModelFrameDTO>) {
          return !value.controllable_component_id.empty() && !value.model_instance_id.empty() &&
                 !value.frame_role_id.empty() && !value.compiled_frame_name.empty() &&
                 !value.model_revision.empty();
        } else {
          return !value.model_instance_id.empty() && !value.frame_role_id.empty() &&
                 !value.compiled_frame_name.empty() && !value.model_revision.empty();
        }
      },
      frame);
}

[[nodiscard]] std::optional<ErrorDTO> validate_query_shape(
    const KinematicStatePathQueryDTO& query) {
  if (query.retained_bundle_id.empty() || query.graph_revision.empty() ||
      query.maximum_receiver_time_skew_ns < 0 ||
      !valid_terminal_frame(query.frame_path.terminal_frame)) {
    return state_view_error("kinematic_query_identity_invalid",
                            "Kinematic state query identity is incomplete");
  }

  if (query.frame_path.segments.empty()) {
    if (!std::holds_alternative<ResolvedCellFrameDTO>(query.frame_path.terminal_frame)) {
      return state_view_error("kinematic_path_disconnected",
                              "Only the cell frame may have an empty path");
    }
  } else {
    if (!std::holds_alternative<KinematicDirectModelAnchorPathSegmentDTO>(
            query.frame_path.segments.front()) &&
        !std::holds_alternative<KinematicDirectComponentAnchorPathSegmentDTO>(
            query.frame_path.segments.front())) {
      return state_view_error("kinematic_path_not_cell_rooted",
                              "Kinematic path does not begin at a cell anchor");
    }
    std::string current = segment_parent_link(query.frame_path.segments.front());
    for (const auto& segment : query.frame_path.segments) {
      if (!valid_fixed_segment(segment) || segment_parent_link(segment) != current ||
          segment_parent_link(segment) == segment_child_link(segment)) {
        return state_view_error("kinematic_path_disconnected",
                                "Kinematic path is incomplete or malformed");
      }
      current = segment_child_link(segment);
      if (const auto* joint = std::get_if<KinematicModelJointPathSegmentDTO>(&segment)) {
        if (joint->model_instance_id.empty() || joint->model_revision.empty() ||
            joint->source_joint_name.empty() || joint->compiled_joint_name.empty() ||
            joint->compiled_parent_link_name.empty() || joint->compiled_child_link_name.empty()) {
          return state_view_error("kinematic_joint_identity_invalid",
                                  "Kinematic joint segment is incomplete");
        }
        std::optional<std::tuple<ControlDomainId, ControlDomainRevision, ControlBindingId,
                                 ControlBindingRevision, ResourceId>>
            previous;
        for (const auto& dependency : joint->eligible_resource_dependencies) {
          const auto key =
              std::tuple{dependency.control_domain_id, dependency.control_domain_revision,
                         dependency.control_binding_id, dependency.control_binding_revision,
                         dependency.resource_id};
          if (dependency.control_domain_id.empty() || dependency.control_domain_revision.empty() ||
              dependency.control_binding_id.empty() ||
              dependency.control_binding_revision.empty() || dependency.resource_id.empty() ||
              !std::isfinite(dependency.multiplier) || !std::isfinite(dependency.offset) ||
              (previous.has_value() && *previous >= key)) {
            return state_view_error("kinematic_dependency_invalid",
                                    "Kinematic coordinate dependencies are not canonical");
          }
          previous = key;
        }
        const bool dynamic = joint->coordinate_kind == KinematicJointCoordinateKind::kAngular ||
                             joint->coordinate_kind == KinematicJointCoordinateKind::kLinear;
        if ((dynamic && joint->eligible_resource_dependencies.empty()) ||
            (!dynamic && !joint->eligible_resource_dependencies.empty())) {
          return state_view_error("kinematic_dependency_shape_invalid",
                                  "Kinematic joint dependency shape disagrees with its kind");
        }
      }
    }
    if (current != compiled_frame_name(query.frame_path.terminal_frame)) {
      return state_view_error("kinematic_path_wrong_terminal",
                              "Kinematic path ends at another frame");
    }
  }

  std::optional<ControlDomainId> previous_domain;
  for (const auto& expected : query.expected_active_control_configurations) {
    if (expected.control_domain_id.empty() || expected.control_domain_revision.empty() ||
        expected.active_control_configuration_id.empty() ||
        !expected.active_control_configuration_generation.nonzero() ||
        (previous_domain.has_value() && *previous_domain >= expected.control_domain_id)) {
      return state_view_error("active_configuration_expectation_invalid",
                              "Active-control expectations are incomplete or non-canonical");
    }
    previous_domain = expected.control_domain_id;
  }

  std::optional<ResourceId> previous_resource;
  for (const auto& source : query.source_projections) {
    if (source.resource_id.empty() || source.provider_instance_id.empty() ||
        source.provider_manifest_id.empty() || source.provider_manifest_revision.empty() ||
        source.state_authority_id.empty() || source.contributing_control_bindings.empty() ||
        (previous_resource.has_value() && *previous_resource >= source.resource_id)) {
      return state_view_error("kinematic_source_projection_invalid",
                              "Kinematic source projections are incomplete or non-canonical");
    }
    previous_resource = source.resource_id;
    std::optional<SourceKey> previous_binding;
    for (const auto& binding : source.contributing_control_bindings) {
      const SourceKey key{binding.control_binding_id, binding.control_binding_revision};
      if (binding.control_binding_id.empty() || binding.control_binding_revision.empty() ||
          (previous_binding.has_value() && *previous_binding >= key)) {
        return state_view_error("kinematic_source_binding_invalid",
                                "Kinematic source bindings are incomplete or non-canonical");
      }
      previous_binding = key;
    }
  }
  return std::nullopt;
}

[[nodiscard]] const KinematicActiveControlConfigurationExpectationDTO* find_expected_configuration(
    const KinematicStatePathQueryDTO& query, const ObservedStatePublicationDTO& publication) {
  const auto found = std::lower_bound(
      query.expected_active_control_configurations.begin(),
      query.expected_active_control_configurations.end(), publication.control_domain_id,
      [](const KinematicActiveControlConfigurationExpectationDTO& candidate,
         const ControlDomainId& domain) { return candidate.control_domain_id < domain; });
  if (found == query.expected_active_control_configurations.end() ||
      found->control_domain_id != publication.control_domain_id ||
      found->control_domain_revision != publication.control_domain_revision ||
      found->active_control_configuration_id != publication.active_control_configuration_id ||
      found->active_control_configuration_generation !=
          publication.active_control_configuration_generation) {
    return nullptr;
  }
  return &*found;
}

[[nodiscard]] bool publication_is_self_consistent(const ObservedStatePublicationDTO& publication,
                                                  std::int64_t now_ns) {
  const auto& snapshot = publication.snapshot;
  return publication.controllable_component_id == snapshot.controllable_component_id &&
         publication.command_target_id == snapshot.command_target_id &&
         publication.control_domain_id == snapshot.control_domain_id &&
         publication.control_domain_revision == snapshot.control_domain_revision &&
         publication.control_binding_id == snapshot.control_binding_id &&
         publication.control_binding_revision == snapshot.control_binding_revision &&
         publication.controller_topology_id == snapshot.controller_topology_id &&
         publication.topology_revision == snapshot.topology_revision &&
         publication.control_route_id == snapshot.control_route_id &&
         publication.active_control_configuration_id == snapshot.active_control_configuration_id &&
         publication.active_control_configuration_generation ==
             snapshot.active_control_configuration_generation &&
         publication.provider_instance_id == snapshot.provider_instance_id &&
         publication.provider_manifest_id == snapshot.provider_manifest_id &&
         publication.provider_manifest_revision == snapshot.provider_manifest_revision &&
         publication.state_authority_id == snapshot.state_authority_id &&
         publication.state_authority_class == snapshot.state_authority_class &&
         publication.runtime_epoch == snapshot.source_epoch &&
         publication.authority_generation == snapshot.authority_generation &&
         publication.publication_sequence == snapshot.sequence &&
         publication.received_timestamp_ns == snapshot.received_timestamp_ns &&
         validate_observed_state_snapshot(snapshot, now_ns).ok();
}

[[nodiscard]] const CoordinatePositionDTO* find_resource_position(
    const ObservedStatePublicationDTO& publication, const ResourceId& resource_id) {
  const auto& resources = publication.snapshot.resources;
  const auto found = std::find(resources.begin(), resources.end(), resource_id);
  if (found == resources.end()) {
    return nullptr;
  }
  const std::size_t index = static_cast<std::size_t>(std::distance(resources.begin(), found));
  if (std::find(std::next(found), resources.end(), resource_id) != resources.end() ||
      index >= publication.snapshot.positions.size()) {
    return nullptr;
  }
  return &publication.snapshot.positions[index];
}

[[nodiscard]] double coordinate_position_value(const CoordinatePositionDTO& position) noexcept {
  if (const auto* angular = std::get_if<AngularPositionDTO>(&position)) {
    return angular->value_rad;
  }
  return std::get<LinearPositionDTO>(position).value_m;
}

[[nodiscard]] CoordinatePositionDTO tagged_position(CoordinateKind kind, double value) {
  return kind == CoordinateKind::kAngular ? CoordinatePositionDTO{AngularPositionDTO{value}}
                                          : CoordinatePositionDTO{LinearPositionDTO{value}};
}

[[nodiscard]] bool same_collapsed_publication(
    const ObservedStatePublicationDTO& left, const CoordinatePositionDTO& left_position,
    const ObservedStatePublicationDTO& right,
    const CoordinatePositionDTO& right_position) noexcept {
  return left.control_domain_id == right.control_domain_id &&
         left.control_domain_revision == right.control_domain_revision &&
         left.active_control_configuration_id == right.active_control_configuration_id &&
         left.active_control_configuration_generation ==
             right.active_control_configuration_generation &&
         left.runtime_epoch == right.runtime_epoch &&
         left.authority_generation == right.authority_generation &&
         left.publication_sequence == right.publication_sequence &&
         left.snapshot.source_id == right.snapshot.source_id &&
         left.snapshot.source_kind == right.snapshot.source_kind &&
         left.snapshot.source_timestamp_ns == right.snapshot.source_timestamp_ns &&
         left.snapshot.received_timestamp_ns == right.snapshot.received_timestamp_ns &&
         left.received_timestamp_ns == right.received_timestamp_ns &&
         left.snapshot.freshness_budget_ns == right.snapshot.freshness_budget_ns &&
         left_position == right_position;
}

[[nodiscard]] double normalized_number(double value) noexcept {
  return value == 0.0 ? 0.0 : value;
}

[[nodiscard]] CanonicalValue text(std::string_view value) {
  return CanonicalValue(std::string(value));
}

template <typename Id>
[[nodiscard]] CanonicalValue identity(const Id& value) {
  return text(value.value());
}

[[nodiscard]] CanonicalValue integer_string(std::uint64_t value) {
  return text(std::to_string(value));
}

[[nodiscard]] CanonicalValue signed_integer_string(std::int64_t value) {
  return text(std::to_string(value));
}

[[nodiscard]] CanonicalValue pose_value(const PlacementPoseDTO& pose) {
  CanonicalValue::Array position;
  for (const double value : pose.position_m) {
    position.emplace_back(normalized_number(value));
  }
  CanonicalValue::Array orientation;
  for (const double value : pose.orientation_xyzw) {
    orientation.emplace_back(normalized_number(value));
  }
  return CanonicalValue(
      CanonicalValue::Object{{"position_m", CanonicalValue(std::move(position))},
                             {"orientation_xyzw", CanonicalValue(std::move(orientation))}});
}

[[nodiscard]] CanonicalValue resolved_frame_value(const ResolvedFrameDTO& frame) {
  return std::visit(
      [](const auto& value) -> CanonicalValue {
        using Frame = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Frame, ResolvedCellFrameDTO>) {
          return CanonicalValue(
              CanonicalValue::Object{{"kind", text("cell")},
                                     {"cell_id", identity(value.cell_id)},
                                     {"compiled_frame_name", text(value.compiled_frame_name)}});
        } else if constexpr (std::is_same_v<Frame, ResolvedComponentModelFrameDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", text("component_model")},
              {"controllable_component_id", identity(value.controllable_component_id)},
              {"model_instance_id", identity(value.model_instance_id)},
              {"model_revision", identity(value.model_revision)},
              {"frame_role_id", identity(value.frame_role_id)},
              {"compiled_frame_name", text(value.compiled_frame_name)}});
        } else {
          return CanonicalValue(
              CanonicalValue::Object{{"kind", text("passive_model")},
                                     {"model_instance_id", identity(value.model_instance_id)},
                                     {"model_revision", identity(value.model_revision)},
                                     {"frame_role_id", identity(value.frame_role_id)},
                                     {"compiled_frame_name", text(value.compiled_frame_name)}});
        }
      },
      frame);
}

[[nodiscard]] CanonicalValue dependency_value(
    const KinematicResourceCoordinateDependencyDTO& dependency) {
  return CanonicalValue(CanonicalValue::Object{
      {"control_domain_id", identity(dependency.control_domain_id)},
      {"control_domain_revision", identity(dependency.control_domain_revision)},
      {"control_binding_id", identity(dependency.control_binding_id)},
      {"control_binding_revision", identity(dependency.control_binding_revision)},
      {"resource_id", identity(dependency.resource_id)},
      {"coordinate_kind", text(to_string(dependency.coordinate_kind))},
      {"multiplier", CanonicalValue(normalized_number(dependency.multiplier))},
      {"offset", CanonicalValue(normalized_number(dependency.offset))},
      {"derived_mimic", CanonicalValue(dependency.derived_mimic)}});
}

[[nodiscard]] std::string_view joint_kind_name(KinematicJointCoordinateKind kind) noexcept {
  switch (kind) {
    case KinematicJointCoordinateKind::kFixed:
      return "fixed";
    case KinematicJointCoordinateKind::kAngular:
      return "angular";
    case KinematicJointCoordinateKind::kLinear:
      return "linear";
    case KinematicJointCoordinateKind::kPlanar:
      return "planar";
    case KinematicJointCoordinateKind::kFloating:
      return "floating";
  }
  return {};
}

template <typename Segment>
[[nodiscard]] CanonicalValue fixed_segment_common(std::string_view kind, const Segment& value,
                                                  CanonicalValue::Object identities) {
  identities.emplace_back("kind", text(kind));
  identities.emplace_back("compiled_joint_name", text(value.compiled_joint_name));
  identities.emplace_back("compiled_parent_link_name", text(value.compiled_parent_link_name));
  identities.emplace_back("compiled_child_link_name", text(value.compiled_child_link_name));
  identities.emplace_back("child_pose_in_parent", pose_value(value.child_pose_in_parent));
  return CanonicalValue(std::move(identities));
}

[[nodiscard]] CanonicalValue path_segment_value(const KinematicFramePathSegmentDTO& segment) {
  return std::visit(
      [](const auto& value) -> CanonicalValue {
        using Segment = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Segment, KinematicDirectModelAnchorPathSegmentDTO>) {
          return fixed_segment_common("direct_model_anchor", value,
                                      {{"model_instance_id", identity(value.model_instance_id)},
                                       {"model_revision", identity(value.model_revision)}});
        } else if constexpr (std::is_same_v<Segment,
                                            KinematicDirectComponentAnchorPathSegmentDTO>) {
          return fixed_segment_common(
              "direct_component_anchor", value,
              {{"controllable_component_id", identity(value.controllable_component_id)},
               {"root_model_instance_id", identity(value.root_model_instance_id)},
               {"root_model_revision", identity(value.root_model_revision)}});
        } else if constexpr (std::is_same_v<Segment, KinematicModelMountPathSegmentDTO>) {
          return fixed_segment_common(
              "model_mount", value,
              {{"controllable_component_id", identity(value.controllable_component_id)},
               {"parent_model_instance_id", identity(value.parent_model_instance_id)},
               {"parent_model_revision", identity(value.parent_model_revision)},
               {"child_model_instance_id", identity(value.child_model_instance_id)},
               {"child_model_revision", identity(value.child_model_revision)}});
        } else if constexpr (std::is_same_v<Segment, KinematicComponentAttachmentPathSegmentDTO>) {
          return fixed_segment_common(
              "component_attachment", value,
              {{"component_attachment_id", identity(value.component_attachment_id)},
               {"parent_component_id", identity(value.parent_component_id)},
               {"child_component_id", identity(value.child_component_id)}});
        } else {
          CanonicalValue::Array dependencies;
          for (const auto& dependency : value.eligible_resource_dependencies) {
            dependencies.push_back(dependency_value(dependency));
          }
          return CanonicalValue(CanonicalValue::Object{
              {"kind", text("model_joint")},
              {"controllable_component_id", value.controllable_component_id.has_value()
                                                ? identity(*value.controllable_component_id)
                                                : CanonicalValue(nullptr)},
              {"model_instance_id", identity(value.model_instance_id)},
              {"model_revision", identity(value.model_revision)},
              {"composition_coordinate_index", integer_string(value.composition_coordinate_index)},
              {"source_joint_name", text(value.source_joint_name)},
              {"compiled_joint_name", text(value.compiled_joint_name)},
              {"compiled_parent_link_name", text(value.compiled_parent_link_name)},
              {"compiled_child_link_name", text(value.compiled_child_link_name)},
              {"coordinate_kind", text(joint_kind_name(value.coordinate_kind))},
              {"eligible_resource_dependencies", CanonicalValue(std::move(dependencies))}});
        }
      },
      segment);
}

[[nodiscard]] CanonicalValue source_projection_value(
    const KinematicResourceSourceProjectionDTO& source) {
  CanonicalValue::Array bindings;
  for (const auto& binding : source.contributing_control_bindings) {
    bindings.emplace_back(CanonicalValue::Object{
        {"control_binding_id", identity(binding.control_binding_id)},
        {"control_binding_revision", identity(binding.control_binding_revision)}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"resource_id", identity(source.resource_id)},
      {"contributing_control_bindings", CanonicalValue(std::move(bindings))},
      {"provider_instance_id", identity(source.provider_instance_id)},
      {"provider_manifest_id", identity(source.provider_manifest_id)},
      {"provider_manifest_revision", identity(source.provider_manifest_revision)},
      {"state_authority_id", identity(source.state_authority_id)},
      {"state_authority_class", text(to_string(source.state_authority_class))},
      {"publication_source", text(to_string(source.publication_source))}});
}

[[nodiscard]] CanonicalValue query_value(const KinematicStatePathQueryDTO& query) {
  CanonicalValue::Array segments;
  for (const auto& segment : query.frame_path.segments) {
    segments.push_back(path_segment_value(segment));
  }
  CanonicalValue::Array configurations;
  for (const auto& configuration : query.expected_active_control_configurations) {
    configurations.emplace_back(CanonicalValue::Object{
        {"control_domain_id", identity(configuration.control_domain_id)},
        {"control_domain_revision", identity(configuration.control_domain_revision)},
        {"active_control_configuration_id",
         identity(configuration.active_control_configuration_id)},
        {"active_control_configuration_generation",
         integer_string(configuration.active_control_configuration_generation.value())}});
  }
  CanonicalValue::Array sources;
  for (const auto& source : query.source_projections) {
    sources.push_back(source_projection_value(source));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"retained_bundle_id", identity(query.retained_bundle_id)},
      {"graph_revision", identity(query.graph_revision)},
      {"frame_path", CanonicalValue(CanonicalValue::Object{
                         {"terminal_frame", resolved_frame_value(query.frame_path.terminal_frame)},
                         {"segments", CanonicalValue(std::move(segments))}})},
      {"expected_active_control_configurations", CanonicalValue(std::move(configurations))},
      {"maximum_receiver_time_skew_ns", signed_integer_string(query.maximum_receiver_time_skew_ns)},
      {"source_projections", CanonicalValue(std::move(sources))}});
}

[[nodiscard]] CanonicalValue coordinate_position_identity(const CoordinatePositionDTO& position) {
  if (const auto* angular = std::get_if<AngularPositionDTO>(&position)) {
    return CanonicalValue(CanonicalValue::Object{
        {"kind", text("angular")},
        {"value_si", CanonicalValue(normalized_number(angular->value_rad))}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"kind", text("linear")},
      {"value_si",
       CanonicalValue(normalized_number(std::get<LinearPositionDTO>(position).value_m))}});
}

[[nodiscard]] CanonicalValue resource_state_value(const KinematicResourceStateRowDTO& row) {
  return CanonicalValue(CanonicalValue::Object{
      {"resource_id", identity(row.resource_id)},
      {"position", coordinate_position_identity(row.position)},
      {"source_projection", source_projection_value(row.source_projection)},
      {"active_control_configuration_id", identity(row.active_control_configuration_id)},
      {"active_control_configuration_generation",
       integer_string(row.active_control_configuration_generation.value())},
      {"runtime_epoch", identity(row.runtime_epoch)},
      {"authority_generation", integer_string(row.authority_generation)},
      {"publication_sequence", integer_string(row.publication_sequence)},
      {"source_timestamp_ns", signed_integer_string(row.source_timestamp_ns)},
      {"snapshot_received_timestamp_ns", signed_integer_string(row.snapshot_received_timestamp_ns)},
      {"publication_received_timestamp_ns",
       signed_integer_string(row.publication_received_timestamp_ns)},
      {"freshness_budget_ns", signed_integer_string(row.freshness_budget_ns)}});
}

[[nodiscard]] CanonicalValue model_state_value(const ComponentModelStateViewDTO& state) {
  CanonicalValue::Array projection;
  for (const auto& row : state.coordinate_projection) {
    projection.emplace_back(CanonicalValue::Object{
        {"composition_coordinate_index", integer_string(row.composition_coordinate_index)},
        {"resource_id", identity(row.resource_id)},
        {"coordinate_kind", text(to_string(row.coordinate_kind))},
        {"multiplier", CanonicalValue(normalized_number(row.multiplier))},
        {"offset", CanonicalValue(normalized_number(row.offset))},
        {"derived_mimic", CanonicalValue(row.derived_mimic)}});
  }
  CanonicalValue::Array positions;
  for (const auto& position : state.positions) {
    positions.push_back(coordinate_position_identity(position));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"controllable_component_id", identity(state.controllable_component_id)},
      {"model_instance_id", identity(state.model_instance_id)},
      {"model_revision", identity(state.model_revision)},
      {"coordinate_projection", CanonicalValue(std::move(projection))},
      {"positions", CanonicalValue(std::move(positions))}});
}

[[nodiscard]] ContentDigest compute_view_digest(const KinematicStatePathViewDTO& view) {
  CanonicalValue::Array resources;
  for (const auto& row : view.resource_states) {
    resources.push_back(resource_state_value(row));
  }
  CanonicalValue::Array models;
  for (const auto& state : view.model_states) {
    models.push_back(model_state_value(state));
  }
  const auto optional_time = [](const std::optional<std::int64_t>& value) {
    return value.has_value() ? signed_integer_string(*value) : CanonicalValue(nullptr);
  };
  const CanonicalValue payload(CanonicalValue::Object{
      {"query", query_value(view.query)},
      {"resource_states", CanonicalValue(std::move(resources))},
      {"model_states", CanonicalValue(std::move(models))},
      {"evaluated_at_ns", signed_integer_string(view.evaluated_at_ns)},
      {"earliest_received_timestamp_ns", optional_time(view.earliest_received_timestamp_ns)},
      {"latest_received_timestamp_ns", optional_time(view.latest_received_timestamp_ns)},
      {"actual_receiver_time_skew_ns", signed_integer_string(view.actual_receiver_time_skew_ns)},
      {"accepted_receiver_time_skew_ns",
       signed_integer_string(view.accepted_receiver_time_skew_ns)}});
  return ContentDigest(content_identity::framed_sha256(
      "manyforge.kinematic_state_path_view", "manyforge.kinematic_state_path_view.v1", payload));
}

}  // namespace

ImmutableKinematicStateViewProvider::ImmutableKinematicStateViewProvider(
    std::vector<KinematicObservedStateSourceBinding> sources)
    : sources_(std::move(sources)) {
  std::sort(sources_.begin(), sources_.end(), [](const auto& left, const auto& right) {
    return std::tie(left.control_binding_id, left.control_binding_revision) <
           std::tie(right.control_binding_id, right.control_binding_revision);
  });
  std::optional<SourceKey> previous;
  for (const auto& source : sources_) {
    const SourceKey key{source.control_binding_id, source.control_binding_revision};
    if (source.control_binding_id.empty() || source.control_binding_revision.empty() ||
        !source.provider || (previous.has_value() && *previous == key)) {
      throw std::invalid_argument("kinematic state-view sources must be exact and unique");
    }
    previous = key;
  }
}

Result<KinematicStatePathViewDTO> ImmutableKinematicStateViewProvider::current_kinematic_state_path(
    const KinematicStatePathQueryDTO& query, std::int64_t now_ns) const {
  if (now_ns < 0) {
    return Result<KinematicStatePathViewDTO>::Err(
        state_view_error("kinematic_evaluation_time_invalid",
                         "Kinematic state evaluation time must be non-negative"));
  }
  if (auto error = validate_query_shape(query); error.has_value()) {
    return Result<KinematicStatePathViewDTO>::Err(std::move(*error));
  }

  const auto find_source = [&](const SourceKey& key) -> const KinematicObservedStateSourceBinding* {
    const auto found = std::lower_bound(
        sources_.begin(), sources_.end(), key,
        [](const KinematicObservedStateSourceBinding& candidate, const SourceKey& selected) {
          return SourceKey{candidate.control_binding_id, candidate.control_binding_revision} <
                 selected;
        });
    if (found == sources_.end() || found->control_binding_id != key.first ||
        found->control_binding_revision != key.second) {
      return nullptr;
    }
    return &*found;
  };

  std::map<SourceKey, ObservedStatePublicationDTO> publications;
  std::set<ControlDomainId> used_domains;
  using PathSourceKey = std::pair<ResourceId, SourceKey>;
  using PathDomain = std::pair<ControlDomainId, ControlDomainRevision>;
  std::map<PathSourceKey, PathDomain> path_source_candidates;
  for (const auto& segment : query.frame_path.segments) {
    const auto* joint = std::get_if<KinematicModelJointPathSegmentDTO>(&segment);
    if (joint == nullptr) {
      continue;
    }
    for (const auto& dependency : joint->eligible_resource_dependencies) {
      const PathSourceKey key{
          dependency.resource_id,
          SourceKey{dependency.control_binding_id, dependency.control_binding_revision}};
      const PathDomain domain{dependency.control_domain_id, dependency.control_domain_revision};
      const auto [found, inserted] = path_source_candidates.emplace(key, domain);
      if (!inserted && found->second != domain) {
        return Result<KinematicStatePathViewDTO>::Err(
            state_view_error("kinematic_dependency_domain_conflict",
                             "One path resource binding maps to multiple control domains"));
      }
    }
  }

  std::vector<KinematicResourceStateRowDTO> resource_states;
  resource_states.reserve(query.source_projections.size());
  for (const auto& source_projection : query.source_projections) {
    const ObservedStatePublicationDTO* first_publication = nullptr;
    const CoordinatePositionDTO* first_position = nullptr;
    for (const auto& binding : source_projection.contributing_control_bindings) {
      const SourceKey key{binding.control_binding_id, binding.control_binding_revision};
      const auto path_source = path_source_candidates.find({source_projection.resource_id, key});
      if (path_source == path_source_candidates.end()) {
        return Result<KinematicStatePathViewDTO>::Err(
            state_view_error("kinematic_source_not_required_by_path",
                             "State source is not a compiler-derived path dependency"));
      }
      auto publication = publications.find(key);
      if (publication == publications.end()) {
        const auto* registered = find_source(key);
        if (registered == nullptr) {
          return Result<KinematicStatePathViewDTO>::Err(
              state_view_error("kinematic_state_source_unavailable",
                               "Required observed-state source is unavailable"));
        }
        auto read = registered->provider->publication(key.first, key.second);
        if (!read.ok()) {
          return Result<KinematicStatePathViewDTO>::Err(read.error());
        }
        publication = publications.emplace(key, std::move(read.value())).first;
      }
      const auto& value = publication->second;
      const auto* configuration = find_expected_configuration(query, value);
      const CoordinatePositionDTO* position =
          find_resource_position(value, source_projection.resource_id);
      if (!publication_is_self_consistent(value, now_ns) || configuration == nullptr ||
          position == nullptr || value.control_domain_id != path_source->second.first ||
          value.control_domain_revision != path_source->second.second ||
          value.provider_instance_id != source_projection.provider_instance_id ||
          value.provider_manifest_id != source_projection.provider_manifest_id ||
          value.provider_manifest_revision != source_projection.provider_manifest_revision ||
          value.state_authority_id != source_projection.state_authority_id ||
          value.state_authority_class != source_projection.state_authority_class ||
          value.snapshot.source_kind != source_projection.publication_source) {
        return Result<KinematicStatePathViewDTO>::Err(
            state_view_error("kinematic_state_publication_mismatch",
                             "Observed state differs from the retained path expectation"));
      }
      used_domains.insert(configuration->control_domain_id);
      if (first_publication == nullptr) {
        first_publication = &value;
        first_position = position;
      } else if (!same_collapsed_publication(*first_publication, *first_position, value,
                                             *position)) {
        return Result<KinematicStatePathViewDTO>::Err(
            state_view_error("kinematic_collapsed_source_conflict",
                             "Bindings collapsed to one resource published different state"));
      }
    }
    if (first_publication == nullptr || first_position == nullptr) {
      return Result<KinematicStatePathViewDTO>::Err(state_view_error(
          "kinematic_state_resource_missing", "Kinematic resource has no authoritative state"));
    }
    resource_states.push_back(KinematicResourceStateRowDTO{
        source_projection.resource_id, *first_position, source_projection,
        first_publication->active_control_configuration_id,
        first_publication->active_control_configuration_generation,
        first_publication->runtime_epoch, first_publication->authority_generation,
        first_publication->publication_sequence, first_publication->snapshot.source_timestamp_ns,
        first_publication->snapshot.received_timestamp_ns, first_publication->received_timestamp_ns,
        first_publication->snapshot.freshness_budget_ns});
  }

  if (used_domains.size() != query.expected_active_control_configurations.size()) {
    return Result<KinematicStatePathViewDTO>::Err(
        state_view_error("kinematic_active_configuration_scope_mismatch",
                         "Active-control expectations do not exactly cover the path"));
  }

  const auto find_resource_state =
      [&](const ResourceId& resource_id) -> const KinematicResourceStateRowDTO* {
    const auto found = std::lower_bound(
        resource_states.begin(), resource_states.end(), resource_id,
        [](const KinematicResourceStateRowDTO& candidate, const ResourceId& selected) {
          return candidate.resource_id < selected;
        });
    return found == resource_states.end() || found->resource_id != resource_id ? nullptr : &*found;
  };

  struct ModelStateAccumulator final {
    ControllableComponentId controllable_component_id;
    ModelInstanceId model_instance_id;
    ModelRevision model_revision;
    std::map<std::size_t, std::pair<KinematicModelCoordinateProjectionDTO, CoordinatePositionDTO>>
        coordinates;
  };
  std::map<ModelKey, ModelStateAccumulator> model_states_by_key;
  std::set<ResourceId> used_resources;
  for (const auto& segment : query.frame_path.segments) {
    const auto* joint = std::get_if<KinematicModelJointPathSegmentDTO>(&segment);
    if (joint == nullptr || joint->coordinate_kind == KinematicJointCoordinateKind::kFixed) {
      continue;
    }
    if (joint->coordinate_kind == KinematicJointCoordinateKind::kPlanar ||
        joint->coordinate_kind == KinematicJointCoordinateKind::kFloating) {
      return Result<KinematicStatePathViewDTO>::Err(
          state_view_error("kinematic_coordinate_kind_unsupported",
                           "Planar or floating frame-state projection is unavailable"));
    }
    if (!joint->controllable_component_id.has_value()) {
      return Result<KinematicStatePathViewDTO>::Err(
          state_view_error("kinematic_dynamic_passive_model_unavailable",
                           "Dynamic passive model has no selected state authority"));
    }

    const KinematicResourceCoordinateDependencyDTO* selected = nullptr;
    for (const auto& dependency : joint->eligible_resource_dependencies) {
      const auto* resource = find_resource_state(dependency.resource_id);
      if (resource == nullptr) {
        continue;
      }
      const SourceKey dependency_key{dependency.control_binding_id,
                                     dependency.control_binding_revision};
      const bool binding_selected =
          std::any_of(resource->source_projection.contributing_control_bindings.begin(),
                      resource->source_projection.contributing_control_bindings.end(),
                      [&](const KinematicControlBindingSourceDTO& binding) {
                        return SourceKey{binding.control_binding_id,
                                         binding.control_binding_revision} == dependency_key;
                      });
      if (!binding_selected) {
        continue;
      }
      if (selected != nullptr &&
          (selected->resource_id != dependency.resource_id ||
           selected->coordinate_kind != dependency.coordinate_kind ||
           selected->multiplier != dependency.multiplier || selected->offset != dependency.offset ||
           selected->derived_mimic != dependency.derived_mimic)) {
        return Result<KinematicStatePathViewDTO>::Err(
            state_view_error("kinematic_coordinate_source_ambiguous",
                             "Active sources map one coordinate inconsistently"));
      }
      selected = &dependency;
    }
    if (selected == nullptr) {
      return Result<KinematicStatePathViewDTO>::Err(
          state_view_error("kinematic_coordinate_state_missing",
                           "Path coordinate has no selected authoritative resource"));
    }
    const auto* resource = find_resource_state(selected->resource_id);
    if (resource == nullptr || coordinate_kind(resource->position) != selected->coordinate_kind) {
      return Result<KinematicStatePathViewDTO>::Err(state_view_error(
          "kinematic_coordinate_unit_mismatch", "Resource and model coordinate units differ"));
    }
    const double projected =
        coordinate_position_value(resource->position) * selected->multiplier + selected->offset;
    if (!std::isfinite(projected)) {
      return Result<KinematicStatePathViewDTO>::Err(
          state_view_error("kinematic_coordinate_projection_nonfinite",
                           "Model coordinate projection is not finite"));
    }
    used_resources.insert(selected->resource_id);
    const ModelKey model_key{*joint->controllable_component_id, joint->model_instance_id,
                             joint->model_revision};
    auto [model, inserted] = model_states_by_key.try_emplace(
        model_key, ModelStateAccumulator{*joint->controllable_component_id,
                                         joint->model_instance_id,
                                         joint->model_revision,
                                         {}});
    (void)inserted;
    if (!model->second.coordinates
             .emplace(joint->composition_coordinate_index,
                      std::pair{KinematicModelCoordinateProjectionDTO{
                                    joint->composition_coordinate_index, selected->resource_id,
                                    selected->coordinate_kind, selected->multiplier,
                                    selected->offset, selected->derived_mimic},
                                tagged_position(selected->coordinate_kind, projected)})
             .second) {
      return Result<KinematicStatePathViewDTO>::Err(
          state_view_error("kinematic_model_coordinate_duplicate",
                           "A model coordinate is repeated on the frame path"));
    }
  }

  if (used_resources.size() != resource_states.size()) {
    return Result<KinematicStatePathViewDTO>::Err(
        state_view_error("kinematic_resource_scope_mismatch",
                         "State source projections do not exactly cover dynamic path resources"));
  }

  std::vector<ComponentModelStateViewDTO> model_states;
  model_states.reserve(model_states_by_key.size());
  for (auto& [unused, accumulated] : model_states_by_key) {
    (void)unused;
    ComponentModelStateViewDTO state{accumulated.controllable_component_id,
                                     accumulated.model_instance_id,
                                     accumulated.model_revision,
                                     {},
                                     {}};
    state.coordinate_projection.reserve(accumulated.coordinates.size());
    state.positions.reserve(accumulated.coordinates.size());
    for (auto& [unused_index, coordinate] : accumulated.coordinates) {
      (void)unused_index;
      state.coordinate_projection.push_back(std::move(coordinate.first));
      state.positions.push_back(std::move(coordinate.second));
    }
    model_states.push_back(std::move(state));
  }

  std::optional<std::int64_t> earliest;
  std::optional<std::int64_t> latest;
  for (const auto& state : resource_states) {
    earliest = !earliest.has_value() ? state.publication_received_timestamp_ns
                                     : std::min(*earliest, state.publication_received_timestamp_ns);
    latest = !latest.has_value() ? state.publication_received_timestamp_ns
                                 : std::max(*latest, state.publication_received_timestamp_ns);
  }
  const std::int64_t actual_skew = earliest.has_value() ? *latest - *earliest : 0;
  if (actual_skew > query.maximum_receiver_time_skew_ns) {
    return Result<KinematicStatePathViewDTO>::Err(
        state_view_error("kinematic_receiver_time_skew_exceeded",
                         "Observed-state sources exceed the retained coherence budget"));
  }

  KinematicStatePathViewDTO result{query,
                                   std::move(resource_states),
                                   std::move(model_states),
                                   now_ns,
                                   earliest,
                                   latest,
                                   actual_skew,
                                   query.maximum_receiver_time_skew_ns,
                                   ContentDigest(std::string{})};
  try {
    result.view_digest = compute_view_digest(result);
  } catch (const std::exception&) {
    return Result<KinematicStatePathViewDTO>::Err(
        state_view_error("kinematic_view_identity_invalid",
                         "Kinematic state view cannot be assigned a canonical identity"));
  }
  return Result<KinematicStatePathViewDTO>::Ok(std::move(result));
}

}  // namespace manyforge::core
