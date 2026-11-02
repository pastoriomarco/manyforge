#include "manyforge_headless/source_model_projection.hpp"

#include <algorithm>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::headless {
namespace {

namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

using content_identity::CanonicalValue;

[[noreturn]] void Fail(const std::string& detail) {
  throw SourceModelProjectionError("source model projection: " + detail);
}

void Require(const bool condition, const std::string& detail) {
  if (!condition) {
    Fail(detail);
  }
}

CanonicalValue TextArray(const std::vector<std::string>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

std::string JointType(const deployment::UrdfJointType value) {
  switch (value) {
    case deployment::UrdfJointType::kRevolute:
      return "revolute";
    case deployment::UrdfJointType::kContinuous:
      return "continuous";
    case deployment::UrdfJointType::kPrismatic:
      return "prismatic";
    case deployment::UrdfJointType::kFixed:
      return "fixed";
    case deployment::UrdfJointType::kFloating:
      return "floating";
    case deployment::UrdfJointType::kPlanar:
      return "planar";
  }
  Fail("verified coordinate has an unknown URDF joint type");
}

std::string CoordinateKind(const deployment::UrdfCoordinateKind value) {
  switch (value) {
    case deployment::UrdfCoordinateKind::kNone:
      return "none";
    case deployment::UrdfCoordinateKind::kAngular:
      return "angular";
    case deployment::UrdfCoordinateKind::kLinear:
      return "linear";
    case deployment::UrdfCoordinateKind::kPlanar:
      return "planar";
    case deployment::UrdfCoordinateKind::kFloating:
      return "floating";
  }
  Fail("verified coordinate has an unknown coordinate kind");
}

std::string CoordinateUnit(const deployment::CompositionCoordinateUnit value) {
  switch (value) {
    case deployment::CompositionCoordinateUnit::kNone:
      return "none";
    case deployment::CompositionCoordinateUnit::kRadians:
      return "radians";
    case deployment::CompositionCoordinateUnit::kMeters:
      return "meters";
    case deployment::CompositionCoordinateUnit::kPlanarMetersRadians:
      return "planar_meters_radians";
    case deployment::CompositionCoordinateUnit::kFloatingMetersRadians:
      return "floating_meters_radians";
  }
  Fail("verified coordinate has an unknown unit");
}

const deployment::OccurrenceSymbolRewrite& RequireCompiledSymbol(
    const deployment::VerifiedRetainedModelMap& model_map,
    const core::ModelInstanceId& model_instance_id, const deployment::OccurrenceSymbolKind kind,
    const std::string& compiled_name) {
  const deployment::OccurrenceSymbolRewrite* match = nullptr;
  for (const auto& symbol : model_map.symbols()) {
    if (symbol.source.model_instance_id == model_instance_id && symbol.source.kind == kind &&
        symbol.compiled_name == compiled_name) {
      Require(match == nullptr, "compiled symbol is ambiguous");
      match = &symbol;
    }
  }
  Require(match != nullptr, "compiled symbol has no source-local referent");
  return *match;
}

const deployment::OccurrenceSymbolRewrite& RequireOccurrenceSymbol(
    const deployment::VerifiedRetainedModelMap& model_map,
    const core::ModelInstanceId& model_instance_id, const deployment::OccurrenceSymbolKind kind) {
  const deployment::OccurrenceSymbolRewrite* match = nullptr;
  for (const auto& symbol : model_map.symbols()) {
    if (symbol.source.model_instance_id == model_instance_id && symbol.source.kind == kind) {
      Require(match == nullptr, "model occurrence has duplicate root symbols");
      match = &symbol;
    }
  }
  Require(match != nullptr, "model occurrence has no source root symbol");
  return *match;
}

std::vector<const deployment::OccurrenceReferenceRewrite*> OwnerReferences(
    const deployment::VerifiedRetainedModelMap& model_map,
    const core::ModelInstanceId& model_instance_id, const deployment::OccurrenceReferenceKind kind,
    const std::string_view owner_scope_name, const std::string_view owner_source_name) {
  std::vector<const deployment::OccurrenceReferenceRewrite*> result;
  for (const auto& reference : model_map.references()) {
    if (reference.model_instance_id == model_instance_id && reference.kind == kind &&
        reference.owner_scope_name == owner_scope_name &&
        reference.owner_source_name == owner_source_name) {
      result.push_back(&reference);
    }
  }
  std::sort(result.begin(), result.end(), [](const auto* left, const auto* right) {
    return left->reference_ordinal < right->reference_ordinal;
  });
  for (std::size_t index = 1U; index < result.size(); ++index) {
    Require(result[index - 1U]->reference_ordinal < result[index]->reference_ordinal,
            "source reference ordinals are duplicated");
  }
  return result;
}

const deployment::OccurrenceReferenceRewrite& RequireOwnerReference(
    const deployment::VerifiedRetainedModelMap& model_map,
    const core::ModelInstanceId& model_instance_id, const deployment::OccurrenceReferenceKind kind,
    const std::string_view owner_scope_name, const std::string_view owner_source_name) {
  const auto matches =
      OwnerReferences(model_map, model_instance_id, kind, owner_scope_name, owner_source_name);
  Require(matches.size() == 1U, "source owner reference is not singular");
  return *matches.front();
}

std::vector<std::string> SourceReferenceNames(const deployment::VerifiedRetainedModelMap& model_map,
                                              const core::ModelInstanceId& model_instance_id,
                                              const deployment::OccurrenceReferenceKind kind,
                                              const std::string_view owner_scope_name,
                                              const std::string_view owner_source_name) {
  std::vector<std::string> result;
  const auto references =
      OwnerReferences(model_map, model_instance_id, kind, owner_scope_name, owner_source_name);
  result.reserve(references.size());
  for (const auto* reference : references) {
    result.push_back(reference->source_target.source_name);
  }
  return result;
}

CanonicalValue CoordinateValue(const deployment::VerifiedRetainedModelMap& model_map,
                               const deployment::CompositionModelCoordinate& coordinate) {
  const auto& parent = RequireOwnerReference(model_map, coordinate.model_instance_id,
                                             deployment::OccurrenceReferenceKind::kJointParentLink,
                                             {}, coordinate.source_joint_name);
  const auto& child = RequireOwnerReference(model_map, coordinate.model_instance_id,
                                            deployment::OccurrenceReferenceKind::kJointChildLink,
                                            {}, coordinate.source_joint_name);
  const auto mimic_references = OwnerReferences(model_map, coordinate.model_instance_id,
                                                deployment::OccurrenceReferenceKind::kJointMimic,
                                                {}, coordinate.source_joint_name);
  CanonicalValue mimic(nullptr);
  if (coordinate.mimic_source_compiled_joint_name.has_value()) {
    Require(mimic_references.size() == 1U && mimic_references.front()->compiled_target_name ==
                                                 *coordinate.mimic_source_compiled_joint_name,
            "joint mimic reference disagrees with its coordinate");
    mimic = CanonicalValue(CanonicalValue::Object{
        {"multiplier", CanonicalValue(coordinate.mimic_multiplier)},
        {"offset", CanonicalValue(coordinate.mimic_offset)},
        {"source_joint_name", CanonicalValue(mimic_references.front()->source_target.source_name)},
    });
  } else {
    Require(mimic_references.empty(), "non-mimic coordinate carries a mimic reference");
  }
  return CanonicalValue(CanonicalValue::Object{
      {"coordinate_kind", CanonicalValue(CoordinateKind(coordinate.coordinate_kind))},
      {"joint_type", CanonicalValue(JointType(coordinate.joint_type))},
      {"mimic", std::move(mimic)},
      {"source_child_link_name", CanonicalValue(child.source_target.source_name)},
      {"source_joint_index", CanonicalValue(static_cast<double>(coordinate.source_joint_index))},
      {"source_joint_name", CanonicalValue(coordinate.source_joint_name)},
      {"source_parent_link_name", CanonicalValue(parent.source_target.source_name)},
      {"unit", CanonicalValue(CoordinateUnit(coordinate.unit))},
      {"viewer_child_link_name", CanonicalValue(child.compiled_target_name)},
      {"viewer_joint_name", CanonicalValue(coordinate.compiled_joint_name)},
      {"viewer_parent_link_name", CanonicalValue(parent.compiled_target_name)},
  });
}

CanonicalValue SrdfGroupValue(const deployment::VerifiedRetainedModelMap& model_map,
                              const deployment::OccurrenceSymbolRewrite& symbol) {
  const auto& model_instance_id = symbol.source.model_instance_id;
  const auto& group_name = symbol.source.source_name;
  const auto bases =
      OwnerReferences(model_map, model_instance_id,
                      deployment::OccurrenceReferenceKind::kSrdfChainBaseLink, {}, group_name);
  const auto tips =
      OwnerReferences(model_map, model_instance_id,
                      deployment::OccurrenceReferenceKind::kSrdfChainTipLink, {}, group_name);
  Require(bases.size() == tips.size(), "SRDF group chain bases and tips disagree");
  CanonicalValue::Array chains;
  chains.reserve(bases.size());
  for (std::size_t index = 0U; index < bases.size(); ++index) {
    Require(bases[index]->reference_ordinal == tips[index]->reference_ordinal,
            "SRDF group chain ordinals disagree");
    chains.emplace_back(CanonicalValue::Object{
        {"source_base_link_name", CanonicalValue(bases[index]->source_target.source_name)},
        {"source_tip_link_name", CanonicalValue(tips[index]->source_target.source_name)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"chains", CanonicalValue(std::move(chains))},
      {"source_group_name", CanonicalValue(group_name)},
      {"source_joint_names",
       TextArray(SourceReferenceNames(model_map, model_instance_id,
                                      deployment::OccurrenceReferenceKind::kSrdfGroupJoint, {},
                                      group_name))},
      {"source_link_names",
       TextArray(SourceReferenceNames(model_map, model_instance_id,
                                      deployment::OccurrenceReferenceKind::kSrdfGroupLink, {},
                                      group_name))},
      {"source_subgroup_names",
       TextArray(SourceReferenceNames(model_map, model_instance_id,
                                      deployment::OccurrenceReferenceKind::kSrdfSubgroup, {},
                                      group_name))},
  });
}

CanonicalValue SrdfGroupStateValue(const deployment::VerifiedRetainedModelMap& model_map,
                                   const deployment::OccurrenceSymbolRewrite& symbol) {
  const auto& group =
      RequireOwnerReference(model_map, symbol.source.model_instance_id,
                            deployment::OccurrenceReferenceKind::kSrdfGroupStateGroup,
                            symbol.source.scope_name, symbol.source.source_name);
  Require(group.source_target.source_name == symbol.source.scope_name,
          "SRDF group-state scope disagrees with its group reference");
  return CanonicalValue(CanonicalValue::Object{
      {"source_group_name", CanonicalValue(symbol.source.scope_name)},
      {"source_group_state_name", CanonicalValue(symbol.source.source_name)},
      {"source_joint_names",
       TextArray(SourceReferenceNames(model_map, symbol.source.model_instance_id,
                                      deployment::OccurrenceReferenceKind::kSrdfGroupStateJoint,
                                      symbol.source.scope_name, symbol.source.source_name))},
  });
}

CanonicalValue SrdfEndEffectorValue(const deployment::VerifiedRetainedModelMap& model_map,
                                    const deployment::OccurrenceSymbolRewrite& symbol) {
  const auto& parent_link =
      RequireOwnerReference(model_map, symbol.source.model_instance_id,
                            deployment::OccurrenceReferenceKind::kSrdfEndEffectorParentLink, {},
                            symbol.source.source_name);
  const auto& group = RequireOwnerReference(
      model_map, symbol.source.model_instance_id,
      deployment::OccurrenceReferenceKind::kSrdfEndEffectorGroup, {}, symbol.source.source_name);
  const auto parent_groups =
      OwnerReferences(model_map, symbol.source.model_instance_id,
                      deployment::OccurrenceReferenceKind::kSrdfEndEffectorParentGroup, {},
                      symbol.source.source_name);
  Require(parent_groups.size() <= 1U, "SRDF end-effector has duplicate parent-group references");
  return CanonicalValue(CanonicalValue::Object{
      {"source_end_effector_name", CanonicalValue(symbol.source.source_name)},
      {"source_group_name", CanonicalValue(group.source_target.source_name)},
      {"source_parent_group_name",
       parent_groups.empty() ? CanonicalValue(nullptr)
                             : CanonicalValue(parent_groups.front()->source_target.source_name)},
      {"source_parent_link_name", CanonicalValue(parent_link.source_target.source_name)},
  });
}

CanonicalValue OccurrenceValue(const deployment::VerifiedRetainedModelMap& model_map,
                               const deployment::CompositionModelOccurrence& occurrence) {
  const auto& source_model = RequireOccurrenceSymbol(model_map, occurrence.model_instance_id,
                                                     deployment::OccurrenceSymbolKind::kModel);
  const auto& source_robot = RequireOccurrenceSymbol(model_map, occurrence.model_instance_id,
                                                     deployment::OccurrenceSymbolKind::kRobot);

  CanonicalValue::Array coordinates;
  for (const auto& coordinate : model_map.coordinates()) {
    if (coordinate.model_instance_id == occurrence.model_instance_id) {
      coordinates.emplace_back(CoordinateValue(model_map, coordinate));
    }
  }

  CanonicalValue::Array groups;
  CanonicalValue::Array group_states;
  CanonicalValue::Array end_effectors;
  std::vector<std::pair<std::string, std::string>> viewer_links;
  for (const auto& symbol : model_map.symbols()) {
    if (symbol.source.model_instance_id != occurrence.model_instance_id) {
      continue;
    }
    if (symbol.source.kind == deployment::OccurrenceSymbolKind::kLink) {
      viewer_links.emplace_back(symbol.source.source_name, symbol.compiled_name);
    } else if (symbol.source.kind == deployment::OccurrenceSymbolKind::kSrdfGroup) {
      groups.emplace_back(SrdfGroupValue(model_map, symbol));
    } else if (symbol.source.kind == deployment::OccurrenceSymbolKind::kSrdfGroupState) {
      group_states.emplace_back(SrdfGroupStateValue(model_map, symbol));
    } else if (symbol.source.kind == deployment::OccurrenceSymbolKind::kSrdfEndEffector) {
      end_effectors.emplace_back(SrdfEndEffectorValue(model_map, symbol));
    }
  }
  std::sort(viewer_links.begin(), viewer_links.end());
  CanonicalValue::Array links;
  links.reserve(viewer_links.size());
  for (const auto& [source_name, viewer_name] : viewer_links) {
    links.emplace_back(CanonicalValue::Object{
        {"source_link_name", CanonicalValue(source_name)},
        {"viewer_link_name", CanonicalValue(viewer_name)},
    });
  }

  return CanonicalValue(CanonicalValue::Object{
      {"component_id", occurrence.owner_component_id.has_value()
                           ? CanonicalValue(occurrence.owner_component_id->value())
                           : CanonicalValue(nullptr)},
      {"coordinates", CanonicalValue(std::move(coordinates))},
      {"model_asset_id", CanonicalValue(occurrence.model_asset_id.value())},
      {"model_instance_id", CanonicalValue(occurrence.model_instance_id.value())},
      {"model_revision", CanonicalValue(occurrence.model_revision.value())},
      {"model_role_id", occurrence.owner_model_role_id.has_value()
                            ? CanonicalValue(occurrence.owner_model_role_id->value())
                            : CanonicalValue(nullptr)},
      {"source_model_name", CanonicalValue(source_model.source.source_name)},
      {"source_robot_name", CanonicalValue(source_robot.source.source_name)},
      {"links", CanonicalValue(std::move(links))},
      {"srdf_end_effectors", CanonicalValue(std::move(end_effectors))},
      {"srdf_group_states", CanonicalValue(std::move(group_states))},
      {"srdf_groups", CanonicalValue(std::move(groups))},
  });
}

std::set<std::pair<std::string, std::string>> CommandableCoordinates(
    const deployment::VerifiedRetainedModelMap& model_map,
    const deployment::CompositionTargetResolution& target) {
  std::set<std::pair<std::string, std::string>> result;
  for (const auto& binding : model_map.control_bindings()) {
    if (binding.controllable_component_id != target.controllable_component_id ||
        binding.command_target_id != target.command_target_id) {
      continue;
    }
    for (const auto& resource : binding.resources) {
      for (const auto& coordinate : resource.coordinates) {
        if (!coordinate.derived_mimic) {
          result.emplace(coordinate.model_instance_id.value(),
                         coordinate.source_joint_name);
        }
      }
    }
  }
  return result;
}

CanonicalValue TargetGroupValue(const deployment::VerifiedRetainedModelMap& model_map,
                                const deployment::CompositionTargetGroupResolution& group,
                                const std::set<std::pair<std::string, std::string>>& commandable) {
  const auto& group_symbol = RequireCompiledSymbol(model_map, group.model_instance_id,
                                                   deployment::OccurrenceSymbolKind::kSrdfGroup,
                                                   group.compiled_group_name);

  std::vector<std::string> source_links;
  source_links.reserve(group.expanded_compiled_link_names.size());
  for (const auto& compiled_link : group.expanded_compiled_link_names) {
    source_links.push_back(RequireCompiledSymbol(model_map, group.model_instance_id,
                                                 deployment::OccurrenceSymbolKind::kLink,
                                                 compiled_link)
                               .source.source_name);
  }

  std::vector<std::string> source_joints;
  std::vector<std::string> commandable_joints;
  source_joints.reserve(group.expanded_compiled_joint_names.size());
  commandable_joints.reserve(group.expanded_compiled_joint_names.size());
  for (const auto& compiled_joint : group.expanded_compiled_joint_names) {
    const auto source_joint =
        RequireCompiledSymbol(model_map, group.model_instance_id,
                              deployment::OccurrenceSymbolKind::kJoint, compiled_joint)
            .source.source_name;
    source_joints.push_back(source_joint);
    if (commandable.contains({group.model_instance_id.value(), source_joint})) {
      commandable_joints.push_back(source_joint);
    }
  }

  return CanonicalValue(CanonicalValue::Object{
      {"commandable_source_joint_names", TextArray(commandable_joints)},
      {"model_group_role_id", CanonicalValue(group.model_group_role_id.value())},
      {"model_instance_id", CanonicalValue(group.model_instance_id.value())},
      {"source_group_name", CanonicalValue(group_symbol.source.source_name)},
      {"source_joint_names", TextArray(source_joints)},
      {"source_link_names", TextArray(source_links)},
  });
}

CanonicalValue TargetFrameValue(const deployment::VerifiedRetainedModelMap& model_map,
                                const deployment::CompositionTargetFrameResolution& frame) {
  CanonicalValue::Object result{
      {"frame_role_id", CanonicalValue(frame.frame_role_id.value())},
  };
  std::visit(
      [&model_map, &result](const auto& value) {
        using Frame = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Frame, deployment::CompositionCellFrame>) {
          result.emplace_back("frame_kind", CanonicalValue("cell_frame"));
          result.emplace_back("model_instance_id", CanonicalValue(nullptr));
          result.emplace_back("source_link_name", CanonicalValue(nullptr));
        } else {
          result.emplace_back(
              "frame_kind",
              CanonicalValue(std::is_same_v<Frame, deployment::CompositionComponentModelFrame>
                                 ? "component_model_frame"
                                 : "passive_model_frame"));
          result.emplace_back("model_instance_id", CanonicalValue(value.model_instance_id.value()));
          result.emplace_back(
              "source_link_name",
              CanonicalValue(RequireCompiledSymbol(model_map, value.model_instance_id,
                                                   deployment::OccurrenceSymbolKind::kLink,
                                                   value.compiled_frame_name)
                                 .source.source_name));
        }
      },
      frame.frame);
  return CanonicalValue(std::move(result));
}

CanonicalValue TargetCollisionValue(
    const deployment::VerifiedRetainedModelMap& model_map,
    const deployment::CompositionTargetCollisionResolution& collision) {
  std::vector<std::string> source_links;
  source_links.reserve(collision.compiled_link_names.size());
  for (const auto& compiled_link : collision.compiled_link_names) {
    source_links.push_back(RequireCompiledSymbol(model_map, collision.model_instance_id,
                                                 deployment::OccurrenceSymbolKind::kLink,
                                                 compiled_link)
                               .source.source_name);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"collision_body_role_id", CanonicalValue(collision.collision_body_role_id.value())},
      {"model_instance_id", CanonicalValue(collision.model_instance_id.value())},
      {"source_link_names", TextArray(source_links)},
  });
}

CanonicalValue TargetValue(const deployment::VerifiedRetainedModelMap& model_map,
                           const deployment::CompositionTargetResolution& target) {
  const auto commandable = CommandableCoordinates(model_map, target);
  CanonicalValue::Array groups;
  groups.reserve(target.model_groups.size());
  for (const auto& group : target.model_groups) {
    groups.emplace_back(TargetGroupValue(model_map, group, commandable));
  }
  CanonicalValue::Array frames;
  frames.reserve(target.frames.size());
  for (const auto& frame : target.frames) {
    frames.emplace_back(TargetFrameValue(model_map, frame));
  }
  CanonicalValue::Array collision_bodies;
  collision_bodies.reserve(target.collision_bodies.size());
  for (const auto& collision : target.collision_bodies) {
    collision_bodies.emplace_back(TargetCollisionValue(model_map, collision));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"collision_bodies", CanonicalValue(std::move(collision_bodies))},
      {"component_id", CanonicalValue(target.controllable_component_id.value())},
      {"frames", CanonicalValue(std::move(frames))},
      {"model_groups", CanonicalValue(std::move(groups))},
      {"target_id", CanonicalValue(target.command_target_id.value())},
  });
}

}  // namespace

CanonicalValue SourceModelComponents(const core::CellGraphDTO& graph) {
  std::map<std::string, CanonicalValue> mounted_on;
  for (const auto& component : graph.controllable_components) {
    mounted_on.emplace(component.controllable_component_id.value(), CanonicalValue(nullptr));
  }
  for (const auto& edge : graph.component_attachments) {
    mounted_on.at(edge.child.controllable_component_id.value()) =
        CanonicalValue(CanonicalValue::Object{
            {"component_id", CanonicalValue(edge.parent.controllable_component_id.value())},
            {"interface_id", CanonicalValue(edge.parent.attachment_interface_id.value())},
        });
  }
  CanonicalValue::Array rows;
  rows.reserve(mounted_on.size());
  for (auto& [component_id, parent] : mounted_on) {
    rows.emplace_back(CanonicalValue::Object{
        {"component_id", CanonicalValue(component_id)},
        {"mounted_on", std::move(parent)},
    });
  }
  return CanonicalValue(std::move(rows));
}

ActiveSourceModelProjection::ActiveSourceModelProjection(
    std::filesystem::path package_root,
    std::optional<std::filesystem::path> activation_root)
    : model_maps_(std::move(package_root),
                  std::move(activation_root)) {}

CanonicalValue ActiveSourceModelProjection::Project(
    const deployment::RetainedBundle& active_bundle) const {
  const auto& bundle = active_bundle.value();
  const auto& products = bundle.compiled_products.symbol_role_maps;
  Require(bundle.cell_graph.model_instances.empty() ? products.size() <= 1U
                                                    : products.size() == 1U,
          "retained model-map cardinality disagrees with the active graph");

  CanonicalValue::Array occurrences;
  CanonicalValue::Array targets;
  CanonicalValue map_contract(nullptr);
  CanonicalValue map_product_id(nullptr);
  if (!products.empty()) {
    const auto& product = products.front();
    std::optional<deployment::VerifiedRetainedModelMap> model_map;
    try {
      model_map = model_maps_.get_verified(deployment::RetainedModelMapQuery{
          active_bundle.retained_bundle_id(), product.product_id});
    } catch (const std::exception& error) {
      Fail("retained model map failed independent verification: " + std::string(error.what()));
    }
    Require(model_map.has_value(), "retained model map is absent");

    occurrences.reserve(model_map->model_occurrences().size());
    for (const auto& occurrence : model_map->model_occurrences()) {
      occurrences.emplace_back(OccurrenceValue(*model_map, occurrence));
    }
    targets.reserve(model_map->targets().size());
    for (const auto& target : model_map->targets()) {
      targets.emplace_back(TargetValue(*model_map, target));
    }
    map_product_id = CanonicalValue(product.product_id.value());
    map_contract = CanonicalValue(CanonicalValue::Object{
        {"contract_id", CanonicalValue(model_map->product_contract_id().value())},
        {"contract_revision", CanonicalValue(model_map->product_contract_revision().value())},
    });
  }

  CanonicalValue result(CanonicalValue::Object{
      {"cell_id", CanonicalValue(bundle.cell_graph.cell_id.value())},
      {"components", SourceModelComponents(bundle.cell_graph)},
      {"graph_revision", CanonicalValue(bundle.cell_graph.graph_revision.value())},
      {"model_map_contract", std::move(map_contract)},
      {"model_map_product_id", std::move(map_product_id)},
      {"occurrences", CanonicalValue(std::move(occurrences))},
      {"retained_bundle_id", CanonicalValue(active_bundle.retained_bundle_id().value())},
      {"schema_version", CanonicalValue(std::string(kSourceModelProjectionApiVersion))},
      {"targets", CanonicalValue(std::move(targets))},
  });
  Require(content_identity::canonical_json(result).size() <= kMaximumSourceModelProjectionBytes,
          "projection exceeds its public byte bound");
  return result;
}

}  // namespace manyforge::headless
