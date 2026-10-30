#include "composed_model_symbol_role_map_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "manyforge_deployment/cell_compiler.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

using SymbolKey =
    std::tuple<std::string, std::uint8_t, std::string, std::string>;

[[nodiscard]] std::string_view symbol_kind_name(
    OccurrenceSymbolKind kind) {
  switch (kind) {
    case OccurrenceSymbolKind::kModel:
      return "model";
    case OccurrenceSymbolKind::kRobot:
      return "robot";
    case OccurrenceSymbolKind::kMaterial:
      return "material";
    case OccurrenceSymbolKind::kLink:
      return "link";
    case OccurrenceSymbolKind::kJoint:
      return "joint";
    case OccurrenceSymbolKind::kVisual:
      return "visual";
    case OccurrenceSymbolKind::kCollision:
      return "collision";
    case OccurrenceSymbolKind::kSrdfGroup:
      return "srdf_group";
    case OccurrenceSymbolKind::kSrdfGroupState:
      return "srdf_group_state";
    case OccurrenceSymbolKind::kSrdfEndEffector:
      return "srdf_end_effector";
  }
  throw std::logic_error("unknown occurrence symbol kind");
}

[[nodiscard]] std::string_view reference_kind_name(
    OccurrenceReferenceKind kind) {
  switch (kind) {
    case OccurrenceReferenceKind::kJointParentLink:
      return "joint_parent_link";
    case OccurrenceReferenceKind::kJointChildLink:
      return "joint_child_link";
    case OccurrenceReferenceKind::kJointMimic:
      return "joint_mimic";
    case OccurrenceReferenceKind::kVisualMaterial:
      return "visual_material";
    case OccurrenceReferenceKind::kSrdfGroupLink:
      return "srdf_group_link";
    case OccurrenceReferenceKind::kSrdfGroupJoint:
      return "srdf_group_joint";
    case OccurrenceReferenceKind::kSrdfChainBaseLink:
      return "srdf_chain_base_link";
    case OccurrenceReferenceKind::kSrdfChainTipLink:
      return "srdf_chain_tip_link";
    case OccurrenceReferenceKind::kSrdfSubgroup:
      return "srdf_subgroup";
    case OccurrenceReferenceKind::kSrdfGroupStateGroup:
      return "srdf_group_state_group";
    case OccurrenceReferenceKind::kSrdfGroupStateJoint:
      return "srdf_group_state_joint";
    case OccurrenceReferenceKind::kSrdfEndEffectorParentLink:
      return "srdf_end_effector_parent_link";
    case OccurrenceReferenceKind::kSrdfEndEffectorGroup:
      return "srdf_end_effector_group";
    case OccurrenceReferenceKind::kSrdfEndEffectorParentGroup:
      return "srdf_end_effector_parent_group";
    case OccurrenceReferenceKind::kSrdfPassiveJoint:
      return "srdf_passive_joint";
    case OccurrenceReferenceKind::kSrdfDisabledCollisionFirstLink:
      return "srdf_disabled_collision_first_link";
    case OccurrenceReferenceKind::kSrdfDisabledCollisionSecondLink:
      return "srdf_disabled_collision_second_link";
  }
  throw std::logic_error("unknown occurrence reference kind");
}

[[nodiscard]] std::string_view joint_type_name(UrdfJointType kind) {
  switch (kind) {
    case UrdfJointType::kRevolute:
      return "revolute";
    case UrdfJointType::kContinuous:
      return "continuous";
    case UrdfJointType::kPrismatic:
      return "prismatic";
    case UrdfJointType::kFixed:
      return "fixed";
    case UrdfJointType::kFloating:
      return "floating";
    case UrdfJointType::kPlanar:
      return "planar";
  }
  throw std::logic_error("unknown URDF joint type");
}

[[nodiscard]] std::string_view coordinate_kind_name(
    UrdfCoordinateKind kind) {
  switch (kind) {
    case UrdfCoordinateKind::kNone:
      return "none";
    case UrdfCoordinateKind::kAngular:
      return "angular";
    case UrdfCoordinateKind::kLinear:
      return "linear";
    case UrdfCoordinateKind::kPlanar:
      return "planar";
    case UrdfCoordinateKind::kFloating:
      return "floating";
  }
  throw std::logic_error("unknown URDF coordinate kind");
}

[[nodiscard]] std::string_view coordinate_unit_name(
    CompositionCoordinateUnit unit) {
  switch (unit) {
    case CompositionCoordinateUnit::kNone:
      return "none";
    case CompositionCoordinateUnit::kRadians:
      return "radians";
    case CompositionCoordinateUnit::kMeters:
      return "meters";
    case CompositionCoordinateUnit::kPlanarMetersRadians:
      return "planar_meters_radians";
    case CompositionCoordinateUnit::kFloatingMetersRadians:
      return "floating_meters_radians";
  }
  throw std::logic_error("unknown composition coordinate unit");
}

[[nodiscard]] SymbolKey symbol_key(const OccurrenceSymbolKey& source) {
  return {source.model_instance_id.value(),
          static_cast<std::uint8_t>(source.kind), source.scope_name,
          source.source_name};
}

using SymbolIndex = std::map<SymbolKey, std::string>;

[[nodiscard]] SymbolIndex make_symbol_index(
    const CompositionQualifiedCellIr& composition) {
  SymbolIndex index;
  for (const auto& row : composition.symbol_rewrites()) {
    if (!index.emplace(symbol_key(row.source), row.compiled_name).second) {
      throw std::logic_error(
          "compiler-qualified symbol map contains a duplicate source key");
    }
  }
  return index;
}

[[nodiscard]] const std::string& require_compiled(
    const SymbolIndex& index, const core::ModelInstanceId& model_instance_id,
    OccurrenceSymbolKind kind, std::string_view scope,
    std::string_view source) {
  const auto found = index.find(
      {model_instance_id.value(), static_cast<std::uint8_t>(kind),
       std::string(scope), std::string(source)});
  if (found == index.end()) {
    throw std::logic_error(
        "compiler-qualified role map references an absent source symbol");
  }
  return found->second;
}

[[nodiscard]] CanonicalValue optional_identity(
    const std::optional<core::ControllableComponentId>& value) {
  return value.has_value() ? CanonicalValue(value->value())
                           : CanonicalValue(nullptr);
}

[[nodiscard]] CanonicalValue optional_model_role(
    const std::optional<core::ModelRoleId>& value) {
  return value.has_value() ? CanonicalValue(value->value())
                           : CanonicalValue(nullptr);
}

[[nodiscard]] CanonicalValue text_array(
    std::span<const std::string> values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue resolved_frame_projection(
    const CompositionResolvedFrame& frame) {
  return std::visit(
      [](const auto& value) -> CanonicalValue {
        using Frame = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Frame, CompositionCellFrame>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("cell")},
              {"cell_id", CanonicalValue(value.cell_id.value())},
              {"compiled_frame_name",
               CanonicalValue(value.compiled_frame_name)},
          });
        } else if constexpr (std::is_same_v<
                                 Frame, CompositionComponentModelFrame>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("component_model")},
              {"controllable_component_id",
               CanonicalValue(value.controllable_component_id.value())},
              {"model_instance_id",
               CanonicalValue(value.model_instance_id.value())},
              {"model_revision",
               CanonicalValue(value.model_revision.value())},
              {"frame_role_id", CanonicalValue(value.frame_role_id.value())},
              {"compiled_frame_name",
               CanonicalValue(value.compiled_frame_name)},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("passive_model")},
              {"model_instance_id",
               CanonicalValue(value.model_instance_id.value())},
              {"model_revision",
               CanonicalValue(value.model_revision.value())},
              {"frame_role_id", CanonicalValue(value.frame_role_id.value())},
              {"compiled_frame_name",
               CanonicalValue(value.compiled_frame_name)},
          });
        }
      },
      frame);
}

[[nodiscard]] CanonicalValue coordinate_projection(
    const CompositionCoordinateProjection& coordinate) {
  return CanonicalValue(CanonicalValue::Object{
      {"model_instance_id",
       CanonicalValue(coordinate.model_instance_id.value())},
      {"source_joint_name", CanonicalValue(coordinate.source_joint_name)},
      {"compiled_joint_name",
       CanonicalValue(coordinate.compiled_joint_name)},
      {"coordinate_kind",
       CanonicalValue(std::string(
           coordinate_kind_name(coordinate.coordinate_kind)))},
      {"unit",
       CanonicalValue(std::string(coordinate_unit_name(coordinate.unit)))},
      {"multiplier", CanonicalValue(coordinate.multiplier)},
      {"offset", CanonicalValue(coordinate.offset)},
      {"derived_mimic", CanonicalValue(coordinate.derived_mimic)},
  });
}

[[nodiscard]] CanonicalValue placement_pose_projection(
    const core::PlacementPoseDTO& pose) {
  CanonicalValue::Array position;
  position.reserve(pose.position_m.size());
  for (const double value : pose.position_m) {
    position.emplace_back(value);
  }
  CanonicalValue::Array orientation;
  orientation.reserve(pose.orientation_xyzw.size());
  for (const double value : pose.orientation_xyzw) {
    orientation.emplace_back(value);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"position_m", CanonicalValue(std::move(position))},
      {"orientation_xyzw", CanonicalValue(std::move(orientation))},
  });
}

[[nodiscard]] CanonicalValue connection_source_projection(
    const CompositionConnectionSource& source) {
  return std::visit(
      [](const auto& value) -> CanonicalValue {
        using Source = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Source,
                                     CompositionDirectModelAnchor>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("direct_model_anchor")},
              {"model_instance_id",
               CanonicalValue(value.model_instance_id.value())},
          });
        } else if constexpr (std::is_same_v<
                                 Source, CompositionDirectComponentAnchor>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("direct_component_anchor")},
              {"controllable_component_id",
               CanonicalValue(value.controllable_component_id.value())},
              {"root_model_instance_id",
               CanonicalValue(value.root_model_instance_id.value())},
          });
        } else if constexpr (std::is_same_v<Source,
                                            CompositionModelMount>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("model_mount")},
              {"controllable_component_id",
               CanonicalValue(value.controllable_component_id.value())},
              {"parent_model_instance_id",
               CanonicalValue(value.parent_model_instance_id.value())},
              {"child_model_instance_id",
               CanonicalValue(value.child_model_instance_id.value())},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("component_attachment")},
              {"component_attachment_id",
               CanonicalValue(value.component_attachment_id.value())},
              {"parent_component_id",
               CanonicalValue(value.parent_component_id.value())},
              {"child_component_id",
               CanonicalValue(value.child_component_id.value())},
          });
        }
      },
      source);
}

[[nodiscard]] CanonicalValue fixed_connection_projection(
    const CompositionFixedConnection& connection) {
  return CanonicalValue(CanonicalValue::Object{
      {"source", connection_source_projection(connection.source)},
      {"compiled_joint_name",
       CanonicalValue(connection.compiled_joint_name)},
      {"compiled_parent_link_name",
       CanonicalValue(connection.compiled_parent_link_name)},
      {"compiled_child_link_name",
       CanonicalValue(connection.compiled_child_link_name)},
      {"child_pose_in_parent",
       placement_pose_projection(connection.child_pose_in_parent)},
  });
}

[[nodiscard]] CanonicalValue frame_path_segment_projection(
    const CompositionFramePathSegment& segment) {
  return std::visit(
      [](const auto& value) -> CanonicalValue {
        using Segment = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Segment,
                                     CompositionFixedPathSegment>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("fixed_connection")},
              {"fixed_connection_index",
               CanonicalValue(
                   static_cast<double>(value.fixed_connection_index))},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("model_joint")},
              {"model_instance_id",
               CanonicalValue(value.model_instance_id.value())},
              {"composition_coordinate_index",
               CanonicalValue(static_cast<double>(
                   value.composition_coordinate_index))},
              {"source_joint_name",
               CanonicalValue(value.source_joint_name)},
              {"compiled_joint_name",
               CanonicalValue(value.compiled_joint_name)},
              {"compiled_parent_link_name",
               CanonicalValue(value.compiled_parent_link_name)},
              {"compiled_child_link_name",
               CanonicalValue(value.compiled_child_link_name)},
          });
        }
      },
      segment);
}

[[nodiscard]] CanonicalValue frame_path_projection(
    const CompositionFramePath& path) {
  CanonicalValue::Array segments;
  segments.reserve(path.segments.size());
  for (const auto& segment : path.segments) {
    segments.push_back(frame_path_segment_projection(segment));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"terminal_frame", resolved_frame_projection(path.terminal_frame)},
      {"segments", CanonicalValue(std::move(segments))},
  });
}

[[nodiscard]] CanonicalValue occurrence_projection(
    const CompositionModelOccurrence& occurrence,
    const core::ModelAssetDTO& asset, const SymbolIndex& symbols) {
  CanonicalValue::Array frame_roles;
  frame_roles.reserve(asset.frame_role_bindings.size());
  for (const auto& role : asset.frame_role_bindings) {
    frame_roles.emplace_back(CanonicalValue::Object{
        {"frame_role_id", CanonicalValue(role.frame_role_id.value())},
        {"source_link_name",
         CanonicalValue(core::frame_role_link_name(role))},
        {"compiled_link_name",
         CanonicalValue(require_compiled(
             symbols, occurrence.model_instance_id,
             OccurrenceSymbolKind::kLink, {},
             core::frame_role_link_name(role)))},
    });
  }

  CanonicalValue::Array group_roles;
  group_roles.reserve(asset.model_group_role_bindings.size());
  for (const auto& role : asset.model_group_role_bindings) {
    group_roles.emplace_back(CanonicalValue::Object{
        {"model_group_role_id",
         CanonicalValue(role.model_group_role_id.value())},
        {"source_group_name", CanonicalValue(role.source_group_name)},
        {"compiled_group_name",
         CanonicalValue(require_compiled(
             symbols, occurrence.model_instance_id,
             OccurrenceSymbolKind::kSrdfGroup, {},
             role.source_group_name))},
    });
  }

  CanonicalValue::Array resource_roles;
  resource_roles.reserve(asset.resource_role_bindings.size());
  for (const auto& role : asset.resource_role_bindings) {
    std::vector<std::string> compiled;
    compiled.reserve(role.source_joint_names.size());
    for (const auto& source_joint : role.source_joint_names) {
      compiled.push_back(require_compiled(
          symbols, occurrence.model_instance_id,
          OccurrenceSymbolKind::kJoint, {}, source_joint));
    }
    resource_roles.emplace_back(CanonicalValue::Object{
        {"resource_role_id", CanonicalValue(role.resource_role_id.value())},
        {"source_joint_names", text_array(role.source_joint_names)},
        {"compiled_joint_names", text_array(compiled)},
    });
  }

  CanonicalValue::Array collision_roles;
  collision_roles.reserve(asset.collision_body_role_bindings.size());
  for (const auto& role : asset.collision_body_role_bindings) {
    std::vector<std::string> compiled;
    compiled.reserve(role.source_link_names.size());
    for (const auto& source_link : role.source_link_names) {
      compiled.push_back(require_compiled(
          symbols, occurrence.model_instance_id,
          OccurrenceSymbolKind::kLink, {}, source_link));
    }
    collision_roles.emplace_back(CanonicalValue::Object{
        {"collision_body_role_id",
         CanonicalValue(role.collision_body_role_id.value())},
        {"source_link_names", text_array(role.source_link_names)},
        {"compiled_link_names", text_array(compiled)},
    });
  }

  return CanonicalValue(CanonicalValue::Object{
      {"model_instance_id",
       CanonicalValue(occurrence.model_instance_id.value())},
      {"model_asset_id", CanonicalValue(occurrence.model_asset_id.value())},
      {"model_revision", CanonicalValue(occurrence.model_revision.value())},
      {"owner_component_id", optional_identity(occurrence.owner_component_id)},
      {"owner_model_role_id", optional_model_role(occurrence.owner_model_role_id)},
      {"compiled_model_name",
       CanonicalValue(occurrence.compiled_model_name)},
      {"compiled_robot_name",
       CanonicalValue(occurrence.compiled_robot_name)},
      {"compiled_root_link_name",
       CanonicalValue(occurrence.compiled_root_link_name)},
      {"frame_roles", CanonicalValue(std::move(frame_roles))},
      {"model_group_roles", CanonicalValue(std::move(group_roles))},
      {"resource_roles", CanonicalValue(std::move(resource_roles))},
      {"collision_body_roles", CanonicalValue(std::move(collision_roles))},
  });
}

[[nodiscard]] const core::ModelAssetDTO& require_asset(
    const CompositionModelOccurrence& occurrence,
    const CompositionQualifiedCellIr& composition) {
  const auto found = std::find_if(
      composition.model_assets().begin(), composition.model_assets().end(),
      [&occurrence](const SourceVerifiedModelAsset& asset) {
        return asset.value().model_asset_id == occurrence.model_asset_id &&
               asset.value().model_revision == occurrence.model_revision;
      });
  if (found == composition.model_assets().end()) {
    throw std::logic_error(
        "compiler-qualified occurrence has no exact model-asset closure");
  }
  return found->value();
}

[[nodiscard]] CanonicalValue map_projection(
    const CompilerQualifiedCellIr& compiler_ir) {
  const auto& composition = compiler_ir.applicability().composition();
  const SymbolIndex symbols = make_symbol_index(composition);

  CanonicalValue::Array occurrences;
  occurrences.reserve(composition.model_occurrences().size());
  for (const auto& occurrence : composition.model_occurrences()) {
    occurrences.push_back(occurrence_projection(
        occurrence, require_asset(occurrence, composition), symbols));
  }

  CanonicalValue::Array symbol_rows;
  symbol_rows.reserve(composition.symbol_rewrites().size());
  for (const auto& rewrite : composition.symbol_rewrites()) {
    symbol_rows.emplace_back(CanonicalValue::Object{
        {"model_instance_id",
         CanonicalValue(rewrite.source.model_instance_id.value())},
        {"kind",
         CanonicalValue(std::string(symbol_kind_name(rewrite.source.kind)))},
        {"scope_name", CanonicalValue(rewrite.source.scope_name)},
        {"source_name", CanonicalValue(rewrite.source.source_name)},
        {"compiled_name", CanonicalValue(rewrite.compiled_name)},
    });
  }

  CanonicalValue::Array reference_rows;
  reference_rows.reserve(composition.reference_rewrites().size());
  for (const auto& rewrite : composition.reference_rewrites()) {
    reference_rows.emplace_back(CanonicalValue::Object{
        {"model_instance_id",
         CanonicalValue(rewrite.model_instance_id.value())},
        {"kind",
         CanonicalValue(std::string(reference_kind_name(rewrite.kind)))},
        {"owner_scope_name", CanonicalValue(rewrite.owner_scope_name)},
        {"owner_source_name", CanonicalValue(rewrite.owner_source_name)},
        {"reference_ordinal",
         CanonicalValue(static_cast<double>(rewrite.reference_ordinal))},
        {"target_model_instance_id",
         CanonicalValue(rewrite.source_target.model_instance_id.value())},
        {"target_kind",
         CanonicalValue(
             std::string(symbol_kind_name(rewrite.source_target.kind)))},
        {"target_scope_name",
         CanonicalValue(rewrite.source_target.scope_name)},
        {"target_source_name",
         CanonicalValue(rewrite.source_target.source_name)},
        {"compiled_target_name",
         CanonicalValue(rewrite.compiled_target_name)},
    });
  }

  CanonicalValue::Array coordinate_rows;
  coordinate_rows.reserve(composition.coordinates().size());
  for (const auto& coordinate : composition.coordinates()) {
    coordinate_rows.emplace_back(CanonicalValue::Object{
        {"model_instance_id",
         CanonicalValue(coordinate.model_instance_id.value())},
        {"owner_component_id",
         optional_identity(coordinate.owner_component_id)},
        {"source_joint_index",
         CanonicalValue(static_cast<double>(coordinate.source_joint_index))},
        {"source_joint_name", CanonicalValue(coordinate.source_joint_name)},
        {"compiled_joint_name",
         CanonicalValue(coordinate.compiled_joint_name)},
        {"joint_type",
         CanonicalValue(std::string(joint_type_name(coordinate.joint_type)))},
        {"coordinate_kind",
         CanonicalValue(std::string(
             coordinate_kind_name(coordinate.coordinate_kind)))},
        {"unit",
         CanonicalValue(std::string(coordinate_unit_name(coordinate.unit)))},
        {"mimic_source_compiled_joint_name",
         coordinate.mimic_source_compiled_joint_name.has_value()
             ? CanonicalValue(*coordinate.mimic_source_compiled_joint_name)
             : CanonicalValue(nullptr)},
        {"mimic_multiplier", CanonicalValue(coordinate.mimic_multiplier)},
        {"mimic_offset", CanonicalValue(coordinate.mimic_offset)},
    });
  }

  CanonicalValue::Array fixed_connections;
  fixed_connections.reserve(composition.fixed_connections().size());
  for (const auto& connection : composition.fixed_connections()) {
    fixed_connections.push_back(fixed_connection_projection(connection));
  }

  CanonicalValue::Array frame_paths;
  frame_paths.reserve(composition.frame_paths().size());
  for (const auto& path : composition.frame_paths()) {
    frame_paths.push_back(frame_path_projection(path));
  }

  CanonicalValue::Array binding_maps;
  binding_maps.reserve(composition.control_binding_coordinate_maps().size());
  for (const auto& binding : composition.control_binding_coordinate_maps()) {
    CanonicalValue::Array resources;
    resources.reserve(binding.resources.size());
    for (const auto& resource : binding.resources) {
      CanonicalValue::Array coordinates;
      coordinates.reserve(resource.coordinates.size());
      for (const auto& coordinate : resource.coordinates) {
        coordinates.push_back(coordinate_projection(coordinate));
      }
      resources.emplace_back(CanonicalValue::Object{
          {"resource_role_id",
           CanonicalValue(resource.resource_role_id.value())},
          {"resource_id", CanonicalValue(resource.resource_id.value())},
          {"component_local_coordinate_kind",
           resource.component_local_coordinate_kind.has_value()
               ? CanonicalValue(std::string(core::to_string(
                     *resource.component_local_coordinate_kind)))
               : CanonicalValue(nullptr)},
          {"coordinates", CanonicalValue(std::move(coordinates))},
      });
    }
    binding_maps.emplace_back(CanonicalValue::Object{
        {"control_binding_id",
         CanonicalValue(binding.control_binding_id.value())},
        {"controllable_component_id",
         CanonicalValue(binding.controllable_component_id.value())},
        {"command_target_id",
         CanonicalValue(binding.command_target_id.value())},
        {"resources", CanonicalValue(std::move(resources))},
    });
  }

  CanonicalValue::Array targets;
  targets.reserve(composition.targets().size());
  for (const auto& target : composition.targets()) {
    CanonicalValue::Array groups;
    groups.reserve(target.model_groups.size());
    for (const auto& group : target.model_groups) {
      groups.emplace_back(CanonicalValue::Object{
          {"model_group_role_id",
           CanonicalValue(group.model_group_role_id.value())},
          {"model_instance_id",
           CanonicalValue(group.model_instance_id.value())},
          {"compiled_group_name",
           CanonicalValue(group.compiled_group_name)},
          {"expanded_compiled_link_names",
           text_array(group.expanded_compiled_link_names)},
          {"expanded_compiled_joint_names",
           text_array(group.expanded_compiled_joint_names)},
      });
    }
    CanonicalValue::Array frames;
    frames.reserve(target.frames.size());
    for (const auto& frame : target.frames) {
      frames.emplace_back(CanonicalValue::Object{
          {"frame_role_id", CanonicalValue(frame.frame_role_id.value())},
          {"frame", resolved_frame_projection(frame.frame)},
      });
    }
    CanonicalValue::Array collisions;
    collisions.reserve(target.collision_bodies.size());
    for (const auto& collision : target.collision_bodies) {
      collisions.emplace_back(CanonicalValue::Object{
          {"collision_body_role_id",
           CanonicalValue(collision.collision_body_role_id.value())},
          {"model_instance_id",
           CanonicalValue(collision.model_instance_id.value())},
          {"compiled_link_names",
           text_array(collision.compiled_link_names)},
      });
    }
    targets.emplace_back(CanonicalValue::Object{
        {"controllable_component_id",
         CanonicalValue(target.controllable_component_id.value())},
        {"command_target_id",
         CanonicalValue(target.command_target_id.value())},
        {"model_groups", CanonicalValue(std::move(groups))},
        {"frames", CanonicalValue(std::move(frames))},
        {"collision_bodies", CanonicalValue(std::move(collisions))},
    });
  }

  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kComposedModelSymbolRoleMapApiVersion))},
      {"cell_id", CanonicalValue(composition.graph().cell_id.value())},
      {"cell_frame",
       CanonicalValue(CanonicalValue::Object{
           {"compiled_frame_name",
            CanonicalValue(composition.cell_frame().compiled_frame_name)},
       })},
      {"model_occurrences", CanonicalValue(std::move(occurrences))},
      {"symbol_rows", CanonicalValue(std::move(symbol_rows))},
      {"reference_rows", CanonicalValue(std::move(reference_rows))},
      {"coordinate_rows", CanonicalValue(std::move(coordinate_rows))},
      {"fixed_connections", CanonicalValue(std::move(fixed_connections))},
      {"frame_paths", CanonicalValue(std::move(frame_paths))},
      {"control_binding_coordinate_maps",
       CanonicalValue(std::move(binding_maps))},
      {"target_role_maps", CanonicalValue(std::move(targets))},
  });
}

}  // namespace

std::string emit_composed_model_symbol_role_map(
    const CompilerQualifiedCellIr& compiler_ir,
    const bool allow_empty_model_map) {
  const CanonicalValue projection = map_projection(compiler_ir);
  const std::string bytes = canonical_json(projection);
  if (bytes.size() > kMaxComposedModelSymbolRoleMapBytes) {
    fail_retained_bundle(
        "composed model symbol/role map exceeds its retained artifact bound");
  }
  const CanonicalValue parsed = parse_and_verify_composed_model_symbol_role_map(
      bytes, allow_empty_model_map);
  if (parsed != projection) {
    throw std::logic_error(
        "composed model symbol/role map verifier changed compiler output");
  }
  return bytes;
}

void require_composed_model_symbol_role_map_matches(
    std::string_view canonical_bytes,
    const CompilerQualifiedCellIr& compiler_ir,
    const bool allow_empty_model_map) {
  const CanonicalValue parsed = parse_and_verify_composed_model_symbol_role_map(
      canonical_bytes, allow_empty_model_map);
  const CanonicalValue expected = map_projection(compiler_ir);
  if (parsed != expected) {
    fail_retained_bundle(
        "composed model symbol/role map differs from CompilerQualifiedCellIr");
  }
}

}  // namespace manyforge::deployment::detail
