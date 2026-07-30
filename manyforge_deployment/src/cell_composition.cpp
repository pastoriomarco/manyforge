#include "manyforge_deployment/cell_composition.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "mf1_name_internal.hpp"

namespace manyforge::deployment {
namespace {

using AssetKey = std::pair<std::string, std::string>;
using SymbolTuple =
    std::tuple<std::string, std::uint8_t, std::string, std::string>;

[[noreturn]] void fail(std::string message) {
  throw CellCompositionError(std::move(message));
}

void require_room(std::size_t current, std::size_t added,
                  std::size_t maximum, std::string_view family) {
  if (added > maximum || current > maximum - added) {
    fail("cell composition " + std::string(family) +
         " exceeds its aggregate bound");
  }
}

[[nodiscard]] AssetKey asset_key(const core::ModelAssetId& id,
                                 const core::ModelRevision& revision) {
  return {id.value(), revision.value()};
}

[[nodiscard]] AssetKey asset_key(
    const StructuralModelInstanceResolution& model) {
  return asset_key(model.model_asset_id, model.model_revision);
}

[[nodiscard]] std::string_view symbol_tag(OccurrenceSymbolKind kind) {
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
      return "group";
    case OccurrenceSymbolKind::kSrdfGroupState:
      return "state";
    case OccurrenceSymbolKind::kSrdfEndEffector:
      return "eef";
  }
  fail("cell composition encountered an unknown source symbol kind");
}

[[nodiscard]] SymbolTuple symbol_tuple(const OccurrenceSymbolKey& key) {
  return {key.model_instance_id.value(),
          static_cast<std::uint8_t>(key.kind), key.scope_name,
          key.source_name};
}

[[nodiscard]] bool symbol_key_less(const OccurrenceSymbolKey& left,
                                   const OccurrenceSymbolKey& right) {
  return symbol_tuple(left) < symbol_tuple(right);
}

class SymbolRegistry final {
 public:
  SymbolRegistry(core::CellId cell_id, std::set<std::string>& all_names)
      : cell_id_(std::move(cell_id)), all_names_(all_names) {}

  [[nodiscard]] std::string add(
      const StructuralModelInstanceResolution& model,
      OccurrenceSymbolKind kind, std::string scope_name,
      std::string source_name) {
    OccurrenceSymbolKey key{model.model_instance_id, kind,
                            std::move(scope_name), std::move(source_name)};
    const auto tuple = symbol_tuple(key);
    if (const auto existing = forward_.find(tuple);
        existing != forward_.end()) {
      return existing->second;
    }
    require_room(forward_.size(), 1U, kMaxCompositionSymbols, "symbol table");
    const auto tag = symbol_tag(kind);
    const auto compiled = detail::make_mf1_generated_name(
        tag,
        {{"cell_id", cell_id_.value()},
         {"model_instance_id", model.model_instance_id.value()},
         {"model_asset_id", model.model_asset_id.value()},
         {"model_revision", model.model_revision.value()},
         {"kind", tag},
         {"scope", key.scope_name},
         {"source", key.source_name}});
    if (!all_names_.insert(compiled).second ||
        !reverse_.emplace(compiled, key).second) {
      fail("generated occurrence-name collision for '" + compiled + "'");
    }
    forward_.emplace(tuple, compiled);
    rewrites_.push_back({std::move(key), compiled});
    return compiled;
  }

  [[nodiscard]] const std::string& require(
      const core::ModelInstanceId& model_instance_id,
      OccurrenceSymbolKind kind, std::string_view scope,
      std::string_view source) const {
    const SymbolTuple key{model_instance_id.value(),
                          static_cast<std::uint8_t>(kind),
                          std::string(scope), std::string(source)};
    const auto found = forward_.find(key);
    if (found == forward_.end()) {
      fail("internal occurrence rewrite is missing a source symbol");
    }
    return found->second;
  }

  [[nodiscard]] std::vector<OccurrenceSymbolRewrite> take_forward() {
    std::sort(rewrites_.begin(), rewrites_.end(),
              [](const OccurrenceSymbolRewrite& left,
                 const OccurrenceSymbolRewrite& right) {
                return symbol_key_less(left.source, right.source);
              });
    return std::move(rewrites_);
  }

  [[nodiscard]] std::vector<OccurrenceReverseSymbolRewrite> reverse() const {
    std::vector<OccurrenceReverseSymbolRewrite> result;
    result.reserve(reverse_.size());
    for (const auto& [compiled, source] : reverse_) {
      result.push_back({compiled, source});
    }
    return result;
  }

 private:
  core::CellId cell_id_;
  std::set<std::string>& all_names_;
  std::map<SymbolTuple, std::string> forward_;
  std::map<std::string, OccurrenceSymbolKey, std::less<>> reverse_;
  std::vector<OccurrenceSymbolRewrite> rewrites_;
};

void checked_add(std::uint64_t& value, std::uint64_t added,
                 std::string_view family) {
  if (value > std::numeric_limits<std::uint64_t>::max() - added) {
    fail("cell composition " + std::string(family) +
         " overflows its checked aggregate");
  }
  value += added;
}

void checked_add_scaled(std::uint64_t& value, std::uint64_t count,
                        std::uint64_t scale, std::string_view family) {
  if (count != 0U &&
      scale > std::numeric_limits<std::uint64_t>::max() / count) {
    fail("cell composition " + std::string(family) +
         " overflows its checked aggregate");
  }
  checked_add(value, count * scale, family);
}

struct CompositionPreflight final {
  std::uint64_t symbols = 0U;
  std::uint64_t references = 0U;
  std::uint64_t flattened_rows = 1U;  // semantic cell frame
  std::uint64_t coordinates = 0U;
  std::uint64_t portable_frames = 0U;
  std::uint64_t estimated_flattened_bytes = 1024U;
};

[[nodiscard]] CompositionPreflight preflight_composition(
    const StructurallyQualifiedCellGraph& structure,
    const std::map<AssetKey, const SourceVerifiedModelAsset*>& assets) {
  CompositionPreflight result;
  for (const auto& model : structure.model_instances()) {
    const auto asset = assets.find(asset_key(model));
    if (asset == assets.end()) {
      fail("composition preflight lost a source-verified asset");
    }
    const auto& source = asset->second->source_ir();
    std::uint64_t symbols = 2U;  // model occurrence and source robot
    std::uint64_t references = 0U;
    checked_add(symbols, source.urdf().materials.size(),
                "symbol preflight");
    checked_add(symbols, source.urdf().links.size(), "symbol preflight");
    checked_add(symbols, source.urdf().joints.size(), "symbol preflight");
    for (const auto& link : source.urdf().links) {
      for (const auto& visual : link.visuals) {
        if (visual.name.has_value()) {
          checked_add(symbols, 1U, "symbol preflight");
        }
        if (visual.material.has_value() &&
            visual.material->name.has_value() &&
            !visual.material->resolved_reference.has_value()) {
          checked_add(symbols, 1U, "symbol preflight");
        }
        if (visual.material.has_value() &&
            visual.material->resolved_reference.has_value()) {
          checked_add(references, 1U, "reference preflight");
        }
      }
      for (const auto& collision : link.collisions) {
        if (collision.name.has_value()) {
          checked_add(symbols, 1U, "symbol preflight");
        }
      }
    }
    for (const auto& joint : source.urdf().joints) {
      checked_add(references, joint.mimic.has_value() ? 3U : 2U,
                  "reference preflight");
    }

    std::uint64_t flattened_rows =
        source.urdf().materials.size() + source.urdf().links.size() +
        source.urdf().joints.size();
    if (source.srdf().has_value()) {
      const auto& srdf = *source.srdf();
      checked_add(symbols, srdf.groups.size(), "symbol preflight");
      checked_add(symbols, srdf.group_states.size(), "symbol preflight");
      checked_add(symbols, srdf.end_effectors.size(), "symbol preflight");
      checked_add(flattened_rows, srdf.groups.size(),
                  "flattened-row preflight");
      checked_add(flattened_rows, srdf.group_states.size(),
                  "flattened-row preflight");
      checked_add(flattened_rows, srdf.end_effectors.size(),
                  "flattened-row preflight");
      checked_add(flattened_rows, srdf.passive_joint_names.size(),
                  "flattened-row preflight");
      checked_add(flattened_rows, srdf.disabled_collisions.size(),
                  "flattened-row preflight");
      for (const auto& group : srdf.groups) {
        for (const auto& member : group.members) {
          checked_add(
              references,
              std::holds_alternative<SrdfChainGroupMemberIr>(member) ? 2U
                                                                    : 1U,
              "reference preflight");
        }
      }
      for (const auto& state : srdf.group_states) {
        checked_add(references, 1U + state.joints.size(),
                    "reference preflight");
      }
      for (const auto& eef : srdf.end_effectors) {
        checked_add(references,
                    eef.parent_group_name.has_value() ? 3U : 2U,
                    "reference preflight");
      }
      checked_add(references, srdf.passive_joint_names.size(),
                  "reference preflight");
      checked_add_scaled(references, srdf.disabled_collisions.size(), 2U,
                         "reference preflight");
    }
    checked_add(result.symbols, symbols, "symbol preflight");
    checked_add(result.references, references, "reference preflight");
    checked_add(result.flattened_rows, flattened_rows,
                "flattened-row preflight");
    checked_add(result.coordinates, source.urdf().joints.size(),
                "coordinate preflight");
    checked_add(
        result.portable_frames,
        asset->second->value().frame_role_bindings.size(),
        "portable-frame preflight");
    const auto urdf_bytes = emit_canonical_urdf(source).size();
    checked_add(result.estimated_flattened_bytes, urdf_bytes,
                "flattened-byte preflight");
    if (const auto srdf_bytes = emit_canonical_srdf(source);
        srdf_bytes.has_value()) {
      checked_add(result.estimated_flattened_bytes, srdf_bytes->size(),
                  "flattened-byte preflight");
    }
    // A compiled name is at most 78 bytes today. Ninety-six is a deliberate
    // version-local conservative allowance for XML quoting and separators.
    checked_add_scaled(result.estimated_flattened_bytes, symbols, 96U,
                       "flattened-byte preflight");
    checked_add_scaled(result.estimated_flattened_bytes, references, 96U,
                       "flattened-byte preflight");
  }
  checked_add(result.flattened_rows, structure.model_instances().size(),
              "flattened-row preflight");
  checked_add_scaled(result.estimated_flattened_bytes,
                     structure.model_instances().size(), 1024U,
                     "flattened-byte preflight");
  if (result.symbols > kMaxCompositionSymbols ||
      result.references > kMaxCompositionReferences ||
      result.flattened_rows > kMaxCompositionSymbols ||
      result.coordinates > kMaxCompositionSymbols ||
      result.portable_frames > kMaxCompositionFrameResolutions) {
    fail("cell composition preflight exceeds a retained row bound");
  }
  if (result.estimated_flattened_bytes >
      kMaxCompositionFlattenedXmlBytes) {
    fail("cell composition preflight exceeds the flattened XML byte bound");
  }
  return result;
}

[[nodiscard]] std::string indexed_scope(std::string_view parent,
                                        std::string_view kind,
                                        std::size_t index) {
  return std::string(parent) + "/" + std::string(kind) + "/" +
         std::to_string(index);
}

void register_source_symbols(const StructuralModelInstanceResolution& model,
                             const ModelSourceIr& source,
                             SymbolRegistry& symbols) {
  (void)symbols.add(model, OccurrenceSymbolKind::kModel, {},
                    model.model_instance_id.value());
  (void)symbols.add(model, OccurrenceSymbolKind::kRobot, {},
                    source.urdf().robot_name);
  for (const auto& material : source.urdf().materials) {
    if (!material.name.has_value()) {
      fail("source-qualified top-level material lost its required name");
    }
    (void)symbols.add(model, OccurrenceSymbolKind::kMaterial, {},
                      *material.name);
  }
  for (const auto& link : source.urdf().links) {
    (void)symbols.add(model, OccurrenceSymbolKind::kLink, {}, link.name);
    for (std::size_t index = 0; index < link.visuals.size(); ++index) {
      const auto& visual = link.visuals[index];
      if (visual.name.has_value()) {
        (void)symbols.add(model, OccurrenceSymbolKind::kVisual,
                          indexed_scope(link.name, "visual", index),
                          *visual.name);
      }
      if (visual.material.has_value() &&
          visual.material->name.has_value() &&
          !visual.material->resolved_reference.has_value()) {
        (void)symbols.add(
            model, OccurrenceSymbolKind::kMaterial,
            indexed_scope(link.name, "visual", index),
            *visual.material->name);
      }
    }
    for (std::size_t index = 0; index < link.collisions.size(); ++index) {
      const auto& collision = link.collisions[index];
      if (collision.name.has_value()) {
        (void)symbols.add(model, OccurrenceSymbolKind::kCollision,
                          indexed_scope(link.name, "collision", index),
                          *collision.name);
      }
    }
  }
  for (const auto& joint : source.urdf().joints) {
    (void)symbols.add(model, OccurrenceSymbolKind::kJoint, {}, joint.name);
  }
  if (!source.srdf().has_value()) {
    return;
  }
  for (const auto& group : source.srdf()->groups) {
    (void)symbols.add(model, OccurrenceSymbolKind::kSrdfGroup, {},
                      group.name);
  }
  for (const auto& state : source.srdf()->group_states) {
    (void)symbols.add(model, OccurrenceSymbolKind::kSrdfGroupState,
                      state.group_name, state.name);
  }
  for (const auto& end_effector : source.srdf()->end_effectors) {
    (void)symbols.add(model, OccurrenceSymbolKind::kSrdfEndEffector, {},
                      end_effector.name);
  }
}

void add_reference(
    std::vector<OccurrenceReferenceRewrite>& references,
    const core::ModelInstanceId& model_instance_id,
    OccurrenceReferenceKind kind, std::string owner_scope_name,
    std::string owner_source_name, std::size_t ordinal,
    OccurrenceSymbolKey source_target, const std::string& compiled_target) {
  require_room(references.size(), 1U, kMaxCompositionReferences,
               "reference table");
  references.push_back(
      {model_instance_id, kind, std::move(owner_scope_name),
       std::move(owner_source_name), ordinal, std::move(source_target),
       compiled_target});
}

[[nodiscard]] UrdfMaterialIr rewrite_material(
    const StructuralModelInstanceResolution& model,
    const UrdfMaterialIr& source, SymbolRegistry& symbols,
    std::vector<OccurrenceReferenceRewrite>& references,
    std::string_view definition_scope,
    std::string_view owner_scope, std::string_view owner_name,
    std::size_t ordinal) {
  auto result = source;
  if (result.name.has_value()) {
    const auto source_name = *result.name;
    const auto scope =
        result.resolved_reference.has_value() ? std::string_view{}
                                              : definition_scope;
    result.name = symbols.require(model.model_instance_id,
                                  OccurrenceSymbolKind::kMaterial, scope,
                                  source_name);
  }
  if (result.resolved_reference.has_value()) {
    const auto source_name = result.resolved_reference->definition_name;
    const auto& compiled = symbols.require(
        model.model_instance_id, OccurrenceSymbolKind::kMaterial, {},
        source_name);
    add_reference(
        references, model.model_instance_id,
        OccurrenceReferenceKind::kVisualMaterial, std::string(owner_scope),
        std::string(owner_name), ordinal,
        {model.model_instance_id, OccurrenceSymbolKind::kMaterial, {},
         source_name},
        compiled);
    result.resolved_reference->definition_name = compiled;
  }
  return result;
}

[[nodiscard]] UrdfDocumentIr rewrite_urdf_occurrence(
    const StructuralModelInstanceResolution& model,
    const UrdfDocumentIr& source, SymbolRegistry& symbols,
    std::vector<OccurrenceReferenceRewrite>& references) {
  UrdfDocumentIr result;
  result.robot_name = symbols.require(
      model.model_instance_id, OccurrenceSymbolKind::kRobot, {},
      source.robot_name);
  result.materials.reserve(source.materials.size());
  for (std::size_t index = 0; index < source.materials.size(); ++index) {
    result.materials.push_back(rewrite_material(
        model, source.materials[index], symbols, references, {}, "robot",
        source.robot_name, index));
  }

  result.links.reserve(source.links.size());
  for (const auto& source_link : source.links) {
    auto link = source_link;
    const auto original_link_name = source_link.name;
    link.name = symbols.require(model.model_instance_id,
                                OccurrenceSymbolKind::kLink, {},
                                original_link_name);
    for (std::size_t index = 0; index < link.visuals.size(); ++index) {
      auto& visual = link.visuals[index];
      const auto original_visual_name =
          source_link.visuals[index].name.value_or("");
      const auto visual_scope =
          indexed_scope(original_link_name, "visual", index);
      if (visual.name.has_value()) {
        visual.name = symbols.require(
            model.model_instance_id, OccurrenceSymbolKind::kVisual,
            visual_scope,
            *visual.name);
      }
      if (visual.material.has_value()) {
        visual.material = rewrite_material(
            model, *visual.material, symbols, references, visual_scope,
            visual_scope, original_visual_name, index);
      }
    }
    for (std::size_t index = 0; index < link.collisions.size(); ++index) {
      auto& collision = link.collisions[index];
      if (collision.name.has_value()) {
        collision.name = symbols.require(
            model.model_instance_id, OccurrenceSymbolKind::kCollision,
            indexed_scope(original_link_name, "collision", index),
            *collision.name);
      }
    }
    result.links.push_back(std::move(link));
  }

  result.joints.reserve(source.joints.size());
  for (const auto& source_joint : source.joints) {
    auto joint = source_joint;
    joint.name = symbols.require(model.model_instance_id,
                                 OccurrenceSymbolKind::kJoint, {},
                                 source_joint.name);
    joint.parent_link_name = symbols.require(
        model.model_instance_id, OccurrenceSymbolKind::kLink, {},
        source_joint.parent_link_name);
    add_reference(
        references, model.model_instance_id,
        OccurrenceReferenceKind::kJointParentLink, {}, source_joint.name, 0U,
        {model.model_instance_id, OccurrenceSymbolKind::kLink, {},
         source_joint.parent_link_name},
        joint.parent_link_name);
    joint.child_link_name = symbols.require(
        model.model_instance_id, OccurrenceSymbolKind::kLink, {},
        source_joint.child_link_name);
    add_reference(
        references, model.model_instance_id,
        OccurrenceReferenceKind::kJointChildLink, {}, source_joint.name, 0U,
        {model.model_instance_id, OccurrenceSymbolKind::kLink, {},
         source_joint.child_link_name},
        joint.child_link_name);
    if (joint.mimic.has_value()) {
      const auto source_name = joint.mimic->joint_name;
      const auto& compiled = symbols.require(
          model.model_instance_id, OccurrenceSymbolKind::kJoint, {},
          source_name);
      add_reference(
          references, model.model_instance_id,
          OccurrenceReferenceKind::kJointMimic, {}, source_joint.name, 0U,
          {model.model_instance_id, OccurrenceSymbolKind::kJoint, {},
           source_name},
          compiled);
      joint.mimic->joint_name = compiled;
    }
    result.joints.push_back(std::move(joint));
  }
  return result;
}

[[nodiscard]] SrdfDocumentIr rewrite_srdf_occurrence(
    const StructuralModelInstanceResolution& model,
    const SrdfDocumentIr& source, SymbolRegistry& symbols,
    std::vector<OccurrenceReferenceRewrite>& references) {
  SrdfDocumentIr result;
  result.robot_name = symbols.require(
      model.model_instance_id, OccurrenceSymbolKind::kRobot, {},
      source.robot_name);
  result.groups.reserve(source.groups.size());
  for (const auto& source_group : source.groups) {
    SrdfGroupIr group{
        symbols.require(model.model_instance_id,
                        OccurrenceSymbolKind::kSrdfGroup, {},
                        source_group.name),
        {}};
    group.members.reserve(source_group.members.size());
    for (std::size_t index = 0; index < source_group.members.size(); ++index) {
      const auto& member = source_group.members[index];
      std::visit(
          [&](const auto& selected) {
            using T = std::decay_t<decltype(selected)>;
            if constexpr (std::is_same_v<T, SrdfLinkGroupMemberIr>) {
              const auto& compiled = symbols.require(
                  model.model_instance_id, OccurrenceSymbolKind::kLink, {},
                  selected.link_name);
              add_reference(
                  references, model.model_instance_id,
                  OccurrenceReferenceKind::kSrdfGroupLink, {},
                  source_group.name, index,
                  {model.model_instance_id, OccurrenceSymbolKind::kLink, {},
                   selected.link_name},
                  compiled);
              group.members.emplace_back(
                  SrdfLinkGroupMemberIr{compiled});
            } else if constexpr (
                std::is_same_v<T, SrdfJointGroupMemberIr>) {
              const auto& compiled = symbols.require(
                  model.model_instance_id, OccurrenceSymbolKind::kJoint, {},
                  selected.joint_name);
              add_reference(
                  references, model.model_instance_id,
                  OccurrenceReferenceKind::kSrdfGroupJoint, {},
                  source_group.name, index,
                  {model.model_instance_id, OccurrenceSymbolKind::kJoint, {},
                   selected.joint_name},
                  compiled);
              group.members.emplace_back(
                  SrdfJointGroupMemberIr{compiled});
            } else if constexpr (
                std::is_same_v<T, SrdfChainGroupMemberIr>) {
              const auto& compiled_base = symbols.require(
                  model.model_instance_id, OccurrenceSymbolKind::kLink, {},
                  selected.base_link_name);
              const auto& compiled_tip = symbols.require(
                  model.model_instance_id, OccurrenceSymbolKind::kLink, {},
                  selected.tip_link_name);
              add_reference(
                  references, model.model_instance_id,
                  OccurrenceReferenceKind::kSrdfChainBaseLink, {},
                  source_group.name, index,
                  {model.model_instance_id, OccurrenceSymbolKind::kLink, {},
                   selected.base_link_name},
                  compiled_base);
              add_reference(
                  references, model.model_instance_id,
                  OccurrenceReferenceKind::kSrdfChainTipLink, {},
                  source_group.name, index,
                  {model.model_instance_id, OccurrenceSymbolKind::kLink, {},
                   selected.tip_link_name},
                  compiled_tip);
              group.members.emplace_back(SrdfChainGroupMemberIr{
                  compiled_base, compiled_tip});
            } else {
              const auto& compiled = symbols.require(
                  model.model_instance_id,
                  OccurrenceSymbolKind::kSrdfGroup, {},
                  selected.group_name);
              add_reference(
                  references, model.model_instance_id,
                  OccurrenceReferenceKind::kSrdfSubgroup, {},
                  source_group.name, index,
                  {model.model_instance_id,
                   OccurrenceSymbolKind::kSrdfGroup, {},
                   selected.group_name},
                  compiled);
              group.members.emplace_back(
                  SrdfSubgroupMemberIr{compiled});
            }
          },
          member);
    }
    result.groups.push_back(std::move(group));
  }

  result.group_states.reserve(source.group_states.size());
  for (const auto& source_state : source.group_states) {
    SrdfGroupStateIr state{
        symbols.require(model.model_instance_id,
                        OccurrenceSymbolKind::kSrdfGroupState,
                        source_state.group_name, source_state.name),
        symbols.require(model.model_instance_id,
                        OccurrenceSymbolKind::kSrdfGroup, {},
                        source_state.group_name),
        {}};
    add_reference(
        references, model.model_instance_id,
        OccurrenceReferenceKind::kSrdfGroupStateGroup, source_state.group_name,
        source_state.name, 0U,
        {model.model_instance_id, OccurrenceSymbolKind::kSrdfGroup, {},
         source_state.group_name},
        state.group_name);
    state.joints.reserve(source_state.joints.size());
    for (std::size_t index = 0; index < source_state.joints.size(); ++index) {
      const auto& source_joint = source_state.joints[index];
      const auto& compiled = symbols.require(
          model.model_instance_id, OccurrenceSymbolKind::kJoint, {},
          source_joint.joint_name);
      add_reference(
          references, model.model_instance_id,
          OccurrenceReferenceKind::kSrdfGroupStateJoint,
          source_state.group_name, source_state.name, index,
          {model.model_instance_id, OccurrenceSymbolKind::kJoint, {},
           source_joint.joint_name},
          compiled);
      state.joints.push_back({compiled, source_joint.value});
    }
    result.group_states.push_back(std::move(state));
  }

  result.end_effectors.reserve(source.end_effectors.size());
  for (const auto& source_eef : source.end_effectors) {
    SrdfEndEffectorIr eef{
        symbols.require(model.model_instance_id,
                        OccurrenceSymbolKind::kSrdfEndEffector, {},
                        source_eef.name),
        symbols.require(model.model_instance_id,
                        OccurrenceSymbolKind::kLink, {},
                        source_eef.parent_link_name),
        symbols.require(model.model_instance_id,
                        OccurrenceSymbolKind::kSrdfGroup, {},
                        source_eef.group_name),
        std::nullopt};
    add_reference(
        references, model.model_instance_id,
        OccurrenceReferenceKind::kSrdfEndEffectorParentLink, {},
        source_eef.name, 0U,
        {model.model_instance_id, OccurrenceSymbolKind::kLink, {},
         source_eef.parent_link_name},
        eef.parent_link_name);
    add_reference(
        references, model.model_instance_id,
        OccurrenceReferenceKind::kSrdfEndEffectorGroup, {}, source_eef.name,
        0U,
        {model.model_instance_id, OccurrenceSymbolKind::kSrdfGroup, {},
         source_eef.group_name},
        eef.group_name);
    if (source_eef.parent_group_name.has_value()) {
      eef.parent_group_name = symbols.require(
          model.model_instance_id, OccurrenceSymbolKind::kSrdfGroup, {},
          *source_eef.parent_group_name);
      add_reference(
          references, model.model_instance_id,
          OccurrenceReferenceKind::kSrdfEndEffectorParentGroup, {},
          source_eef.name, 0U,
          {model.model_instance_id, OccurrenceSymbolKind::kSrdfGroup, {},
           *source_eef.parent_group_name},
          *eef.parent_group_name);
    }
    result.end_effectors.push_back(std::move(eef));
  }

  result.passive_joint_names.reserve(source.passive_joint_names.size());
  for (std::size_t index = 0; index < source.passive_joint_names.size();
       ++index) {
    const auto& source_name = source.passive_joint_names[index];
    const auto& compiled = symbols.require(
        model.model_instance_id, OccurrenceSymbolKind::kJoint, {},
        source_name);
    add_reference(
        references, model.model_instance_id,
        OccurrenceReferenceKind::kSrdfPassiveJoint, {}, "robot", index,
        {model.model_instance_id, OccurrenceSymbolKind::kJoint, {},
         source_name},
        compiled);
    result.passive_joint_names.push_back(compiled);
  }

  result.disabled_collisions.reserve(source.disabled_collisions.size());
  for (std::size_t index = 0; index < source.disabled_collisions.size();
       ++index) {
    const auto& source_pair = source.disabled_collisions[index];
    SrdfDisabledCollisionIr pair{
        symbols.require(model.model_instance_id,
                        OccurrenceSymbolKind::kLink, {},
                        source_pair.first_link_name),
        symbols.require(model.model_instance_id,
                        OccurrenceSymbolKind::kLink, {},
                        source_pair.second_link_name),
        source_pair.reason};
    add_reference(
        references, model.model_instance_id,
        OccurrenceReferenceKind::kSrdfDisabledCollisionFirstLink, {},
        "robot", index,
        {model.model_instance_id, OccurrenceSymbolKind::kLink, {},
         source_pair.first_link_name},
        pair.first_link_name);
    add_reference(
        references, model.model_instance_id,
        OccurrenceReferenceKind::kSrdfDisabledCollisionSecondLink, {},
        "robot", index,
        {model.model_instance_id, OccurrenceSymbolKind::kLink, {},
         source_pair.second_link_name},
        pair.second_link_name);
    result.disabled_collisions.push_back(std::move(pair));
  }
  return result;
}

[[nodiscard]] CompositionCoordinateUnit coordinate_unit(
    UrdfCoordinateKind kind) {
  switch (kind) {
    case UrdfCoordinateKind::kNone:
      return CompositionCoordinateUnit::kNone;
    case UrdfCoordinateKind::kAngular:
      return CompositionCoordinateUnit::kRadians;
    case UrdfCoordinateKind::kLinear:
      return CompositionCoordinateUnit::kMeters;
    case UrdfCoordinateKind::kPlanar:
      return CompositionCoordinateUnit::kPlanarMetersRadians;
    case UrdfCoordinateKind::kFloating:
      return CompositionCoordinateUnit::kFloatingMetersRadians;
  }
  fail("cell composition encountered an unknown coordinate kind");
}

[[nodiscard]] UrdfOriginIr origin_from_pose(
    const core::PlacementPoseDTO& pose) {
  const double x = pose.orientation_xyzw[0];
  const double y = pose.orientation_xyzw[1];
  const double z = pose.orientation_xyzw[2];
  const double w = pose.orientation_xyzw[3];
  const double sinr_cosp = 2.0 * (w * x + y * z);
  const double cosr_cosp = 1.0 - 2.0 * (x * x + y * y);
  const double roll = std::atan2(sinr_cosp, cosr_cosp);
  const double sinp = 2.0 * (w * y - z * x);
  const double pitch =
      std::abs(sinp) >= 1.0
          ? std::copysign(std::numbers::pi_v<double> / 2.0, sinp)
          : std::asin(sinp);
  const double siny_cosp = 2.0 * (w * z + x * y);
  const double cosy_cosp = 1.0 - 2.0 * (y * y + z * z);
  const double yaw = std::atan2(siny_cosp, cosy_cosp);

  UrdfOriginIr result;
  result.element_authored = true;
  result.authored_xyz_m = pose.position_m;
  result.authored_rpy_rad = ModelSourceVector3{roll, pitch, yaw};
  result.xyz_m = pose.position_m;
  result.rpy_rad = *result.authored_rpy_rad;
  return result;
}

[[nodiscard]] UrdfJointIr fixed_joint(
    const CompositionFixedConnection& connection) {
  return {connection.compiled_joint_name,
          UrdfJointType::kFixed,
          UrdfCoordinateKind::kNone,
          connection.compiled_parent_link_name,
          connection.compiled_child_link_name,
          origin_from_pose(connection.child_pose_in_parent),
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt};
}

[[nodiscard]] const std::string& compiled_link(
    const StructuralFrameResolution& frame, const SymbolRegistry& symbols) {
  return symbols.require(frame.model.model_instance_id,
                         OccurrenceSymbolKind::kLink, {},
                         frame.source_link_name);
}

[[nodiscard]] const std::string& compiled_root(
    const StructuralModelInstanceResolution& model,
    const SymbolRegistry& symbols) {
  return symbols.require(model.model_instance_id,
                         OccurrenceSymbolKind::kLink, {},
                         model.source_root_link_name);
}

[[nodiscard]] std::string compiled_frame_name(
    const CompositionResolvedFrame& frame) {
  return std::visit(
      [](const auto& selected) { return selected.compiled_frame_name; },
      frame);
}

[[nodiscard]] auto frame_sort_key(
    const CompositionResolvedFrame& frame) {
  using Key =
      std::tuple<std::uint8_t, std::string, std::string, std::string,
                 std::string, std::string>;
  return std::visit(
      [](const auto& selected) -> Key {
        using T = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<T, CompositionCellFrame>) {
          return {0U, selected.cell_id.value(), {}, {}, {},
                  selected.compiled_frame_name};
        } else if constexpr (
            std::is_same_v<T, CompositionComponentModelFrame>) {
          return {1U,
                  selected.controllable_component_id.value(),
                  selected.model_instance_id.value(),
                  selected.model_revision.value(),
                  selected.frame_role_id.value(),
                  selected.compiled_frame_name};
        } else {
          return {2U,
                  {},
                  selected.model_instance_id.value(),
                  selected.model_revision.value(),
                  selected.frame_role_id.value(),
                  selected.compiled_frame_name};
        }
      },
      frame);
}

[[nodiscard]] CompositionResolvedFrame resolve_portable_frame(
    const StructuralModelInstanceResolution& model,
    const core::FrameRoleId& frame_role_id,
    const std::string& compiled_name) {
  if (model.owner_component_id.has_value()) {
    return CompositionComponentModelFrame{
        *model.owner_component_id, model.model_instance_id,
        model.model_revision, frame_role_id, compiled_name};
  }
  return CompositionPassiveModelFrame{
      model.model_instance_id, model.model_revision, frame_role_id,
      compiled_name};
}

struct MimicProjection final {
  std::string ultimate_source_joint_name;
  double multiplier = 1.0;
  double offset = 0.0;
  bool derived = false;
};

[[nodiscard]] std::map<std::string, MimicProjection, std::less<>>
build_mimic_projections(const UrdfDocumentIr& source) {
  std::map<std::string, const UrdfJointIr*, std::less<>> joints;
  for (const auto& joint : source.joints) {
    joints.emplace(joint.name, &joint);
  }
  std::map<std::string, MimicProjection, std::less<>> result;
  for (const auto& joint : source.joints) {
    if (result.contains(joint.name)) {
      continue;
    }
    std::vector<const UrdfJointIr*> unresolved;
    std::set<std::string, std::less<>> path;
    const UrdfJointIr* current = &joint;
    while (!result.contains(current->name) &&
           current->mimic.has_value()) {
      if (!path.insert(current->name).second) {
        fail("source-qualified mimic relation became cyclic during "
             "composition");
      }
      unresolved.push_back(current);
      const auto found = joints.find(current->mimic->joint_name);
      if (found == joints.end()) {
        fail("source-qualified mimic relation lost its referent during "
             "composition");
      }
      current = found->second;
    }
    if (!result.contains(current->name)) {
      result.emplace(current->name,
                     MimicProjection{current->name, 1.0, 0.0, false});
    }
    for (auto iterator = unresolved.rbegin();
         iterator != unresolved.rend(); ++iterator) {
      const auto* selected = *iterator;
      const auto& mimic = *selected->mimic;
      const auto& target = result.at(mimic.joint_name);
      const double multiplier = mimic.multiplier * target.multiplier;
      const double offset =
          mimic.multiplier * target.offset + mimic.offset;
      if (!std::isfinite(multiplier) || !std::isfinite(offset)) {
        fail("transitive mimic transform is not finite");
      }
      result.emplace(
          selected->name,
          MimicProjection{target.ultimate_source_joint_name, multiplier,
                          offset, true});
    }
  }
  return result;
}

[[nodiscard]] const StructuralComponentResolution& require_component(
    const std::map<std::string, const StructuralComponentResolution*,
                   std::less<>>& components,
    const core::ControllableComponentId& component_id) {
  const auto found = components.find(component_id.value());
  if (found == components.end()) {
    fail("control binding selects a missing structurally qualified component");
  }
  return *found->second;
}

[[nodiscard]] const StructuralCommandTargetResolution& require_target(
    const StructuralComponentResolution& component,
    const core::CommandTargetId& target_id) {
  const auto found = std::lower_bound(
      component.command_targets.begin(), component.command_targets.end(),
      target_id,
      [](const StructuralCommandTargetResolution& target,
         const core::CommandTargetId& id) {
        return target.command_target_id.value() < id.value();
      });
  if (found == component.command_targets.end() ||
      found->command_target_id != target_id) {
    fail("control binding selects a missing structurally qualified command "
         "target");
  }
  return *found;
}

[[nodiscard]] core::ResourceRoleId structural_resource_role(
    const StructuralResourceResolution& resource) {
  return std::visit(
      [](const auto& selected) { return selected.resource_role_id; },
      resource);
}

[[nodiscard]] const StructuralModelInstanceResolution& require_model(
    const std::map<std::string,
                   const StructuralModelInstanceResolution*, std::less<>>&
        models,
    const core::ModelInstanceId& model_id) {
  const auto found = models.find(model_id.value());
  if (found == models.end()) {
    fail("internal composition lost a structurally qualified model");
  }
  return *found->second;
}

[[nodiscard]] const SourceVerifiedModelAsset& require_asset(
    const std::map<AssetKey, const SourceVerifiedModelAsset*>& assets,
    const StructuralModelInstanceResolution& model) {
  const auto found = assets.find(asset_key(model));
  if (found == assets.end()) {
    fail("internal composition lost a source-verified model asset");
  }
  return *found->second;
}

[[nodiscard]] std::vector<CompositionControlBindingCoordinateMap>
build_control_binding_maps(
    const StructurallyQualifiedCellGraph& structure,
    const std::map<std::string, const StructuralComponentResolution*,
                   std::less<>>& components,
    const std::map<std::string,
                   const StructuralModelInstanceResolution*, std::less<>>&
        models,
    const std::map<AssetKey, const SourceVerifiedModelAsset*>& assets,
    const std::vector<CompositionModelCoordinate>& coordinates) {
  using CoordinateKey = std::pair<std::string, std::string>;
  std::map<CoordinateKey, const CompositionModelCoordinate*>
      coordinate_index;
  for (const auto& coordinate : coordinates) {
    coordinate_index.emplace(
        CoordinateKey{coordinate.model_instance_id.value(),
                      coordinate.source_joint_name},
        &coordinate);
  }
  std::map<std::string,
           std::map<std::string, MimicProjection, std::less<>>,
           std::less<>>
      mimic_by_model;
  for (const auto& [model_id, model] : models) {
    mimic_by_model.emplace(
        model_id,
        build_mimic_projections(require_asset(assets, *model)
                                    .source_ir()
                                    .urdf()));
  }

  std::vector<const core::ControlBindingDTO*> bindings;
  bindings.reserve(structure.graph().control_bindings.size());
  for (const auto& binding : structure.graph().control_bindings) {
    bindings.push_back(&binding);
  }
  std::sort(bindings.begin(), bindings.end(),
            [](const core::ControlBindingDTO* left,
               const core::ControlBindingDTO* right) {
              return left->control_binding_id < right->control_binding_id;
            });

  std::vector<CompositionControlBindingCoordinateMap> result;
  result.reserve(bindings.size());
  std::size_t projection_count = 0U;
  std::map<std::string, std::vector<CompositionCoordinateProjection>,
           std::less<>>
      coordinates_by_resource_id;
  for (const auto* binding : bindings) {
    const auto& component =
        require_component(components, binding->controllable_component_id);
    const auto& target = require_target(component, binding->command_target_id);

    CompositionControlBindingCoordinateMap map{
        binding->control_binding_id, binding->controllable_component_id,
        binding->command_target_id, {}};
    map.resources.reserve(binding->resource_bindings.size());
    using DirectKey = std::pair<std::string, std::string>;
    std::set<DirectKey> selected_coordinates;
    std::size_t binding_cursor = 0U;
    for (std::size_t resource_index = 0;
         resource_index < target.resources.size(); ++resource_index) {
      const auto expected_role =
          structural_resource_role(target.resources[resource_index]);
      const auto group_begin = binding_cursor;
      while (binding_cursor < binding->resource_bindings.size() &&
             binding->resource_bindings[binding_cursor].resource_role_id ==
                 expected_role) {
        ++binding_cursor;
      }
      const auto group_size = binding_cursor - group_begin;
      if (group_size == 0U) {
        fail("control binding '" + binding->control_binding_id.value() +
             "' resource roles are missing, extra, or reordered");
      }
      if (const auto* model_resource =
              std::get_if<StructuralModelResourceResolution>(
                  &target.resources[resource_index]);
          model_resource != nullptr) {
        if (group_size != model_resource->source_joint_names.size()) {
          fail("control binding '" + binding->control_binding_id.value() +
               "' resource group cardinality does not match the exact "
               "source-coordinate order");
        }
        const auto& model = require_model(models, model_resource->model
                                                      .model_instance_id);
        const auto& source = require_asset(assets, model).source_ir().urdf();
        const auto& mimic = mimic_by_model.at(model.model_instance_id.value());

        std::map<std::string, std::size_t, std::less<>> direct_rows;
        const auto output_group_begin = map.resources.size();
        for (std::size_t coordinate_slot = 0;
             coordinate_slot < model_resource->source_joint_names.size();
             ++coordinate_slot) {
          const auto& joint_name =
              model_resource->source_joint_names[coordinate_slot];
          const auto coordinate = coordinate_index.find(
              {model.model_instance_id.value(), joint_name});
          if (coordinate == coordinate_index.end()) {
            fail("target resource selects a missing model coordinate");
          }
          if (coordinate->second->coordinate_kind ==
              UrdfCoordinateKind::kNone) {
            fail("target resource attempts to bind a fixed model coordinate");
          }
          const auto mimic_row = mimic.find(joint_name);
          if (mimic_row == mimic.end() || mimic_row->second.derived) {
            fail("target resource must bind an independent source coordinate, "
                 "not a mimic coordinate");
          }
          if (!selected_coordinates
                   .emplace(model.model_instance_id.value(), joint_name)
                   .second) {
            fail("two target resources bind the same model coordinate");
          }
          direct_rows.emplace(joint_name,
                              output_group_begin + coordinate_slot);
          const auto& graph_resource =
              binding->resource_bindings[group_begin + coordinate_slot];
          map.resources.push_back(
              {expected_role, graph_resource.resource_id, {}});
          require_room(projection_count, 1U,
                       kMaxCompositionCoordinateProjections,
                       "coordinate projection");
          ++projection_count;
          map.resources.back().coordinates.push_back(
              {model.model_instance_id,
               joint_name,
               coordinate->second->compiled_joint_name,
               coordinate->second->coordinate_kind,
               coordinate->second->unit,
               1.0,
               0.0,
               false});
        }

        for (const auto& source_joint : source.joints) {
          const auto projection = mimic.find(source_joint.name);
          if (projection == mimic.end()) {
            fail("internal composition lost a model mimic projection");
          }
          if (!projection->second.derived) {
            continue;
          }
          const auto direct_row =
              direct_rows.find(projection->second.ultimate_source_joint_name);
          if (direct_row == direct_rows.end()) {
            continue;
          }
          const auto coordinate = coordinate_index.find(
              {model.model_instance_id.value(), source_joint.name});
          if (coordinate == coordinate_index.end()) {
            fail("internal composition lost a model coordinate");
          }
          require_room(projection_count, 1U,
                       kMaxCompositionCoordinateProjections,
                       "coordinate projection");
          ++projection_count;
          map.resources[direct_row->second].coordinates.push_back(
              {model.model_instance_id,
               source_joint.name,
               coordinate->second->compiled_joint_name,
               source_joint.coordinate_kind,
               coordinate->second->unit,
               projection->second.multiplier,
               projection->second.offset,
               projection->second.derived});
        }
      } else {
        for (std::size_t index = group_begin; index < binding_cursor;
             ++index) {
          map.resources.push_back(
              {expected_role,
               binding->resource_bindings[index].resource_id,
               {}});
        }
      }
    }
    if (binding_cursor != binding->resource_bindings.size()) {
      fail("control binding '" + binding->control_binding_id.value() +
           "' resource roles are missing, extra, or noncontiguous");
    }
    std::set<std::string, std::less<>> binding_resource_ids;
    for (const auto& resource : map.resources) {
      if (!binding_resource_ids.insert(resource.resource_id.value()).second) {
        fail("control binding '" + binding->control_binding_id.value() +
             "' contains a duplicate ResourceId");
      }
      const auto [existing, inserted] = coordinates_by_resource_id.emplace(
          resource.resource_id.value(), resource.coordinates);
      if (!inserted && existing->second != resource.coordinates) {
        fail("ResourceId '" + resource.resource_id.value() +
             "' maps to conflicting model coordinates across control "
             "bindings");
      }
    }
    result.push_back(std::move(map));
  }
  return result;
}

[[nodiscard]] std::vector<CompositionTargetResolution> build_targets(
    const StructurallyQualifiedCellGraph& structure,
    const std::map<std::string,
                   const StructuralModelInstanceResolution*, std::less<>>&
        models,
    const std::map<AssetKey, const SourceVerifiedModelAsset*>& assets,
    const SymbolRegistry& symbols,
    const std::map<std::tuple<std::string, std::string>,
                   CompositionResolvedFrame>& frame_index) {
  std::map<std::pair<std::string, std::string>,
           const ExpandedSrdfGroupIr*>
      expanded_group_index;
  for (const auto& [model_id, model] : models) {
    const auto& groups =
        require_asset(assets, *model)
            .source_ir()
            .semantic_index()
            .expanded_groups;
    for (const auto& group : groups) {
      if (!expanded_group_index
               .emplace(std::pair{model_id, group.group_name}, &group)
               .second) {
        fail("source-qualified expanded SRDF group identity is duplicated");
      }
    }
  }
  std::uint64_t target_count = 0U;
  std::uint64_t target_projection_rows = 0U;
  for (const auto& component : structure.components()) {
    for (const auto& target : component.command_targets) {
      checked_add(target_count, 1U, "target preflight");
      checked_add(target_projection_rows, 1U, "target preflight");
      checked_add(target_projection_rows, target.frames.size(),
                  "target preflight");
      for (const auto& group : target.model_groups) {
        const auto expanded = expanded_group_index.find(
            {group.model.model_instance_id.value(),
             group.source_group_name});
        if (expanded == expanded_group_index.end()) {
          fail("target preflight lost a source-qualified expanded SRDF "
               "group");
        }
        checked_add(target_projection_rows, 1U, "target preflight");
        checked_add(target_projection_rows,
                    expanded->second->links.size(),
                    "target preflight");
        checked_add(target_projection_rows,
                    expanded->second->joints.size(),
                    "target preflight");
      }
      for (const auto& body : target.collision_bodies) {
        checked_add(target_projection_rows, 1U, "target preflight");
        checked_add(target_projection_rows,
                    body.source_link_names.size(),
                    "target preflight");
      }
    }
  }
  if (target_count > kMaxCompositionTargetProjectionRows ||
      target_projection_rows >
          kMaxCompositionTargetProjectionRows) {
    fail("cell composition target preflight exceeds its retained row "
         "bound");
  }
  std::vector<CompositionTargetResolution> result;
  result.reserve(static_cast<std::size_t>(target_count));
  std::size_t retained_semantic_rows = 0U;
  for (const auto& component : structure.components()) {
    for (const auto& target : component.command_targets) {
      require_room(retained_semantic_rows, 1U,
                   kMaxCompositionTargetProjectionRows,
                   "target semantic projection");
      ++retained_semantic_rows;
      CompositionTargetResolution resolved{
          component.controllable_component_id, target.command_target_id,
          {}, {}, {}};
      resolved.model_groups.reserve(target.model_groups.size());
      for (const auto& group : target.model_groups) {
        const auto& model = require_model(models, group.model.model_instance_id);
        const auto expanded_found = expanded_group_index.find(
            {model.model_instance_id.value(), group.source_group_name});
        if (expanded_found == expanded_group_index.end()) {
          fail("internal composition lost a source-qualified expanded SRDF "
               "group");
        }
        const auto& expanded = *expanded_found->second;
        require_room(retained_semantic_rows,
                     1U + expanded.links.size() +
                         expanded.joints.size(),
                     kMaxCompositionTargetProjectionRows,
                     "target semantic projection");
        retained_semantic_rows +=
            1U + expanded.links.size() + expanded.joints.size();
        CompositionTargetGroupResolution target_group{
            group.model_group_role_id,
            model.model_instance_id,
            symbols.require(model.model_instance_id,
                            OccurrenceSymbolKind::kSrdfGroup, {},
                            group.source_group_name),
            {},
            {}};
        target_group.expanded_compiled_link_names.reserve(
            expanded.links.size());
        for (const auto& link : expanded.links) {
          target_group.expanded_compiled_link_names.push_back(
              symbols.require(model.model_instance_id,
                              OccurrenceSymbolKind::kLink, {}, link.name));
        }
        target_group.expanded_compiled_joint_names.reserve(
            expanded.joints.size());
        for (const auto& joint : expanded.joints) {
          target_group.expanded_compiled_joint_names.push_back(
              symbols.require(model.model_instance_id,
                              OccurrenceSymbolKind::kJoint, {}, joint.name));
        }
        resolved.model_groups.push_back(std::move(target_group));
      }
      resolved.frames.reserve(target.frames.size());
      for (const auto& frame : target.frames) {
        require_room(retained_semantic_rows, 1U,
                     kMaxCompositionTargetProjectionRows,
                     "target semantic projection");
        ++retained_semantic_rows;
        const auto key = std::tuple{
            frame.source.model.model_instance_id.value(),
            frame.source.frame_role_id.value()};
        const auto found = frame_index.find(key);
        if (found == frame_index.end()) {
          fail("internal composition lost a portable target frame");
        }
        resolved.frames.push_back({frame.frame_role_id, found->second});
      }
      resolved.collision_bodies.reserve(target.collision_bodies.size());
      for (const auto& body : target.collision_bodies) {
        require_room(retained_semantic_rows,
                     1U + body.source_link_names.size(),
                     kMaxCompositionTargetProjectionRows,
                     "target semantic projection");
        retained_semantic_rows +=
            1U + body.source_link_names.size();
        CompositionTargetCollisionResolution collision{
            body.collision_body_role_id, body.model.model_instance_id, {}};
        collision.compiled_link_names.reserve(body.source_link_names.size());
        for (const auto& link : body.source_link_names) {
          collision.compiled_link_names.push_back(symbols.require(
              body.model.model_instance_id, OccurrenceSymbolKind::kLink, {},
              link));
        }
        resolved.collision_bodies.push_back(std::move(collision));
      }
      result.push_back(std::move(resolved));
    }
  }
  std::sort(result.begin(), result.end(),
            [](const CompositionTargetResolution& left,
               const CompositionTargetResolution& right) {
              return std::tie(left.controllable_component_id,
                              left.command_target_id) <
                     std::tie(right.controllable_component_id,
                              right.command_target_id);
            });
  return result;
}

[[nodiscard]] std::vector<CompositionFramePath> build_frame_paths(
    const CompositionCellFrame& cell_frame,
    const UrdfDocumentIr& flattened_urdf,
    const std::vector<CompositionFrameResolution>& frames,
    const std::vector<CompositionFixedConnection>& connections,
    const std::vector<CompositionModelCoordinate>& coordinates) {
  using Segment = CompositionFramePathSegment;
  std::map<std::string, Segment, std::less<>> parent_segments;
  for (std::size_t index = 0; index < connections.size(); ++index) {
    const auto& connection = connections[index];
    if (!parent_segments
             .emplace(connection.compiled_child_link_name,
                      CompositionFixedPathSegment{index})
             .second) {
      fail("flattened cell assigns more than one parent to a link");
    }
  }

  std::map<std::string,
           const CompositionModelCoordinate*, std::less<>>
      coordinate_by_joint;
  for (const auto& coordinate : coordinates) {
    coordinate_by_joint.emplace(coordinate.compiled_joint_name, &coordinate);
  }
  for (const auto& joint : flattened_urdf.joints) {
    if (coordinate_by_joint.contains(joint.name)) {
      const auto* coordinate = coordinate_by_joint.at(joint.name);
      const auto coordinate_index =
          static_cast<std::size_t>(coordinate - coordinates.data());
      if (!parent_segments
               .emplace(
                   joint.child_link_name,
                   CompositionModelJointPathSegment{
                       coordinate->model_instance_id, coordinate_index,
                       coordinate->source_joint_name, joint.name,
                       joint.parent_link_name, joint.child_link_name})
               .second) {
        fail("flattened cell assigns more than one parent to a link");
      }
    }
  }

  std::set<std::string, std::less<>> link_names;
  for (const auto& link : flattened_urdf.links) {
    link_names.insert(link.name);
  }
  if (!link_names.contains(cell_frame.compiled_frame_name)) {
    fail("flattened cell lost its semantic cell frame");
  }

  auto parent_link = [&](const Segment& segment) {
    return std::visit(
        [&](const auto& selected) -> std::string {
          using T = std::decay_t<decltype(selected)>;
          if constexpr (std::is_same_v<T,
                                       CompositionFixedPathSegment>) {
            return connections[selected.fixed_connection_index]
                .compiled_parent_link_name;
          } else {
            return selected.compiled_parent_link_name;
          }
        },
        segment);
  };

  // Prove the complete flattened link graph is one cell-rooted tree in
  // linear graph work. Do not materialize a root path for every unexported
  // link: a deep chain would otherwise create quadratic validation work.
  if (parent_segments.size() + 1U != link_names.size()) {
    fail("flattened cell link-parent cardinality is not a tree");
  }
  enum class Visit : std::uint8_t { kUnseen = 0, kVisiting, kDone };
  std::map<std::string, Visit, std::less<>> visit;
  visit.emplace(cell_frame.compiled_frame_name, Visit::kDone);
  for (const auto& link : link_names) {
    if (visit[link] == Visit::kDone) {
      continue;
    }
    std::vector<std::string> chain;
    std::string current = link;
    while (visit[current] != Visit::kDone) {
      if (visit[current] == Visit::kVisiting) {
        fail("flattened cell contains a parent cycle");
      }
      visit[current] = Visit::kVisiting;
      chain.push_back(current);
      const auto segment = parent_segments.find(current);
      if (segment == parent_segments.end()) {
        fail("flattened cell contains a link disconnected from the cell "
             "frame");
      }
      current = parent_link(segment->second);
      if (!link_names.contains(current)) {
        fail("flattened cell joint references an absent parent link");
      }
    }
    for (const auto& visited_link : chain) {
      visit[visited_link] = Visit::kDone;
    }
  }

  auto path_to = [&](std::string terminal, std::size_t remaining_budget) {
    std::vector<Segment> reversed;
    while (terminal != cell_frame.compiled_frame_name) {
      const auto segment = parent_segments.find(terminal);
      if (segment == parent_segments.end()) {
        fail("flattened cell contains a link disconnected from the cell "
             "frame");
      }
      if (reversed.size() >= remaining_budget) {
        fail("cell composition frame path exceeds its aggregate bound");
      }
      reversed.push_back(segment->second);
      terminal = parent_link(segment->second);
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
  };

  std::vector<CompositionFramePath> result;
  result.reserve(frames.size() + 1U);
  result.push_back(
      {CompositionResolvedFrame{cell_frame}, {}});
  std::size_t aggregate_segments = 0U;
  for (const auto& frame : frames) {
    auto segments = path_to(
        compiled_frame_name(frame.frame),
        kMaxCompositionPathSegments - aggregate_segments);
    require_room(aggregate_segments, segments.size(),
                 kMaxCompositionPathSegments, "frame path");
    aggregate_segments += segments.size();
    result.push_back({frame.frame, std::move(segments)});
  }
  return result;
}

}  // namespace

CompositionQualifiedCellIr compose_cell_occurrences(
    const StructurallyQualifiedCellGraph& structure) {
  const auto& graph = structure.graph();
  std::set<std::string> all_compiled_names;
  const auto cell_link_name = detail::make_mf1_generated_name(
      "cell", {{"cell_id", graph.cell_id.value()}, {"kind", "cell_frame"}});
  if (!all_compiled_names.insert(cell_link_name).second) {
    fail("generated semantic cell-frame name collided");
  }
  const CompositionCellFrame cell_frame{graph.cell_id, cell_link_name};
  const auto flattened_robot_name = detail::make_mf1_generated_name(
      "robot", {{"cell_id", graph.cell_id.value()}, {"kind", "cell_robot"}});
  if (!all_compiled_names.insert(flattened_robot_name).second) {
    fail("generated flattened robot name collided");
  }

  std::map<AssetKey, const SourceVerifiedModelAsset*> assets;
  for (const auto& asset : structure.model_assets()) {
    assets.emplace(asset_key(asset.value().model_asset_id,
                             asset.value().model_revision),
                   &asset);
  }
  std::vector<const StructuralModelInstanceResolution*> ordered_models;
  ordered_models.reserve(structure.model_instances().size());
  std::map<std::string,
           const StructuralModelInstanceResolution*, std::less<>>
      models;
  for (const auto& model : structure.model_instances()) {
    ordered_models.push_back(&model);
    models.emplace(model.model_instance_id.value(), &model);
  }
  std::sort(ordered_models.begin(), ordered_models.end(),
            [](const StructuralModelInstanceResolution* left,
               const StructuralModelInstanceResolution* right) {
              return left->model_instance_id < right->model_instance_id;
            });

  const auto preflight = preflight_composition(structure, assets);
  SymbolRegistry symbols(graph.cell_id, all_compiled_names);
  for (const auto* model : ordered_models) {
    register_source_symbols(*model, require_asset(assets, *model).source_ir(),
                            symbols);
  }

  UrdfDocumentIr flattened_urdf{flattened_robot_name, {},
                                {UrdfLinkIr{cell_link_name, std::nullopt, {},
                                            {}}},
                                {}};
  SrdfDocumentIr flattened_srdf{flattened_robot_name, {}, {}, {}, {}, {}};
  std::vector<OccurrenceReferenceRewrite> references;
  references.reserve(static_cast<std::size_t>(preflight.references));
  std::vector<CompositionModelOccurrence> occurrences;
  std::vector<CompositionModelCoordinate> coordinates;
  coordinates.reserve(static_cast<std::size_t>(preflight.coordinates));
  std::vector<CompositionFrameResolution> frames;
  frames.reserve(static_cast<std::size_t>(preflight.portable_frames));
  std::map<std::tuple<std::string, std::string>, CompositionResolvedFrame>
      frame_index;

  occurrences.reserve(ordered_models.size());
  for (const auto* model : ordered_models) {
    const auto& asset = require_asset(assets, *model);
    const auto& source = asset.source_ir();
    auto rewritten =
        rewrite_urdf_occurrence(*model, source.urdf(), symbols, references);
    require_room(flattened_urdf.materials.size(), rewritten.materials.size(),
                 kMaxCompositionSymbols, "flattened material");
    flattened_urdf.materials.insert(
        flattened_urdf.materials.end(),
        std::make_move_iterator(rewritten.materials.begin()),
        std::make_move_iterator(rewritten.materials.end()));
    require_room(flattened_urdf.links.size(), rewritten.links.size(),
                 kMaxCompositionSymbols, "flattened link");
    flattened_urdf.links.insert(
        flattened_urdf.links.end(),
        std::make_move_iterator(rewritten.links.begin()),
        std::make_move_iterator(rewritten.links.end()));
    require_room(flattened_urdf.joints.size(), rewritten.joints.size(),
                 kMaxCompositionSymbols, "flattened joint");
    flattened_urdf.joints.insert(
        flattened_urdf.joints.end(),
        std::make_move_iterator(rewritten.joints.begin()),
        std::make_move_iterator(rewritten.joints.end()));

    if (source.srdf().has_value()) {
      auto srdf = rewrite_srdf_occurrence(
          *model, *source.srdf(), symbols, references);
      require_room(flattened_srdf.groups.size(), srdf.groups.size(),
                   kMaxCompositionSymbols, "flattened SRDF group");
      flattened_srdf.groups.insert(
          flattened_srdf.groups.end(),
          std::make_move_iterator(srdf.groups.begin()),
          std::make_move_iterator(srdf.groups.end()));
      require_room(flattened_srdf.group_states.size(),
                   srdf.group_states.size(), kMaxCompositionSymbols,
                   "flattened SRDF group-state");
      flattened_srdf.group_states.insert(
          flattened_srdf.group_states.end(),
          std::make_move_iterator(srdf.group_states.begin()),
          std::make_move_iterator(srdf.group_states.end()));
      require_room(flattened_srdf.end_effectors.size(),
                   srdf.end_effectors.size(), kMaxCompositionSymbols,
                   "flattened SRDF end-effector");
      flattened_srdf.end_effectors.insert(
          flattened_srdf.end_effectors.end(),
          std::make_move_iterator(srdf.end_effectors.begin()),
          std::make_move_iterator(srdf.end_effectors.end()));
      require_room(flattened_srdf.passive_joint_names.size(),
                   srdf.passive_joint_names.size(), kMaxCompositionSymbols,
                   "flattened SRDF passive joint");
      flattened_srdf.passive_joint_names.insert(
          flattened_srdf.passive_joint_names.end(),
          std::make_move_iterator(srdf.passive_joint_names.begin()),
          std::make_move_iterator(srdf.passive_joint_names.end()));
      require_room(flattened_srdf.disabled_collisions.size(),
                   srdf.disabled_collisions.size(), kMaxCompositionSymbols,
                   "flattened SRDF disabled collision");
      flattened_srdf.disabled_collisions.insert(
          flattened_srdf.disabled_collisions.end(),
          std::make_move_iterator(srdf.disabled_collisions.begin()),
          std::make_move_iterator(srdf.disabled_collisions.end()));
    }

    occurrences.push_back(
        {model->model_instance_id,
         model->model_asset_id,
         model->model_revision,
         model->owner_component_id,
         model->owner_model_role_id,
         symbols.require(model->model_instance_id,
                         OccurrenceSymbolKind::kModel, {},
                         model->model_instance_id.value()),
         symbols.require(model->model_instance_id,
                         OccurrenceSymbolKind::kRobot, {},
                         source.urdf().robot_name),
         compiled_root(*model, symbols)});

    require_room(coordinates.size(), source.urdf().joints.size(),
                 kMaxCompositionSymbols, "model coordinate");
    for (std::size_t index = 0; index < source.urdf().joints.size(); ++index) {
      const auto& joint = source.urdf().joints[index];
      std::optional<std::string> mimic_compiled;
      double multiplier = 1.0;
      double offset = 0.0;
      if (joint.mimic.has_value()) {
        mimic_compiled = symbols.require(
            model->model_instance_id, OccurrenceSymbolKind::kJoint, {},
            joint.mimic->joint_name);
        multiplier = joint.mimic->multiplier;
        offset = joint.mimic->offset;
      }
      coordinates.push_back(
          {model->model_instance_id,
           model->owner_component_id,
           index,
           joint.name,
           symbols.require(model->model_instance_id,
                           OccurrenceSymbolKind::kJoint, {}, joint.name),
           joint.type,
           joint.coordinate_kind,
           coordinate_unit(joint.coordinate_kind),
           std::move(mimic_compiled),
           multiplier,
           offset});
    }

    for (const auto& frame_binding : asset.value().frame_role_bindings) {
      require_room(frames.size(), 1U,
                   kMaxCompositionFrameResolutions,
                   "portable frame resolution");
      const auto& compiled = symbols.require(
          model->model_instance_id, OccurrenceSymbolKind::kLink, {},
          frame_binding.source_link_name);
      auto resolved =
          resolve_portable_frame(*model, frame_binding.frame_role_id, compiled);
      if (!frame_index
               .emplace(std::tuple{model->model_instance_id.value(),
                                   frame_binding.frame_role_id.value()},
                        resolved)
               .second) {
        fail("portable frame resolution contains a duplicate exact identity");
      }
      frames.push_back({std::move(resolved),
                        frame_binding.source_link_name});
    }
  }

  std::map<std::string, const core::ModelInstanceDTO*, std::less<>>
      graph_models;
  for (const auto& model : graph.model_instances) {
    graph_models.emplace(model.model_instance_id.value(), &model);
  }
  std::map<std::string, const core::ControllableComponentDTO*, std::less<>>
      graph_components;
  for (const auto& component : graph.controllable_components) {
    graph_components.emplace(component.controllable_component_id.value(),
                             &component);
  }
  std::map<std::string, const StructuralComponentResolution*, std::less<>>
      components;
  using IndexedMount =
      std::pair<const StructuralComponentResolution*,
                const StructuralModelMountResolution*>;
  std::map<std::string, IndexedMount, std::less<>>
      mounts_by_child_model;
  for (const auto& component : structure.components()) {
    components.emplace(component.controllable_component_id.value(),
                       &component);
    for (const auto& mount : component.model_mounts) {
      if (!mounts_by_child_model
               .emplace(mount.child.model.model_instance_id.value(),
                        IndexedMount{&component, &mount})
               .second) {
        fail("structurally qualified model has more than one internal mount");
      }
    }
  }
  std::map<std::string, const StructuralAttachmentResolution*, std::less<>>
      attachments_by_child_component;
  for (const auto& attachment : structure.attachments()) {
    if (!attachments_by_child_component
             .emplace(attachment.child.controllable_component_id.value(),
                      &attachment)
             .second) {
      fail("structurally qualified component has more than one attachment "
           "parent");
    }
  }

  std::vector<CompositionFixedConnection> connections;
  connections.reserve(structure.model_instances().size());
  for (const auto* model : ordered_models) {
    CompositionConnectionSource source =
        CompositionDirectModelAnchor{
            model->model_instance_id,
            core::PlacementPoseDTO{}};
    std::string tag;
    std::string parent_link;
    std::string child_link = compiled_root(*model, symbols);
    core::PlacementPoseDTO pose;
    switch (model->placement_source) {
      case StructuralPlacementSource::kDirectModelPoseInCell: {
        const auto* graph_model = graph_models.at(
            model->model_instance_id.value());
        if (!graph_model->root_pose_in_cell.has_value()) {
          fail("passive model lost its graph-owned root pose");
        }
        pose = *graph_model->root_pose_in_cell;
        source = CompositionDirectModelAnchor{
            model->model_instance_id, pose};
        tag = "anchor";
        parent_link = cell_frame.compiled_frame_name;
        break;
      }
      case StructuralPlacementSource::kDirectComponentPoseInCell: {
        if (!model->owner_component_id.has_value()) {
          fail("component-root placement lost its owner");
        }
        const auto* graph_component = graph_components.at(
            model->owner_component_id->value());
        if (!graph_component->root_pose_in_cell.has_value()) {
          fail("unattached component lost its graph-owned root pose");
        }
        pose = *graph_component->root_pose_in_cell;
        source = CompositionDirectComponentAnchor{
            *model->owner_component_id, model->model_instance_id, pose};
        tag = "anchor";
        parent_link = cell_frame.compiled_frame_name;
        break;
      }
      case StructuralPlacementSource::kDerivedFromComponentModelMount: {
        if (!model->owner_component_id.has_value()) {
          fail("model mount lost its component owner");
        }
        const auto& component =
            *components.at(model->owner_component_id->value());
        const auto found = mounts_by_child_model.find(
            model->model_instance_id.value());
        if (found == mounts_by_child_model.end()) {
          fail("structurally qualified model mount lost its child relation");
        }
        if (found->second.first != &component) {
          fail("structurally qualified model mount changed component owner");
        }
        const auto& mount = *found->second.second;
        pose = mount.child_frame_pose_in_parent_frame;
        source = CompositionModelMount{
            component.controllable_component_id,
            mount.parent.model.model_instance_id,
            mount.child.model.model_instance_id,
            pose};
        tag = "mount";
        parent_link = compiled_link(mount.parent, symbols);
        child_link = compiled_link(mount.child, symbols);
        break;
      }
      case StructuralPlacementSource::kDerivedFromComponentAttachment: {
        if (!model->owner_component_id.has_value()) {
          fail("component attachment lost its child owner");
        }
        const auto found = attachments_by_child_component.find(
            model->owner_component_id->value());
        if (found == attachments_by_child_component.end()) {
          fail("structurally qualified component attachment lost its child "
               "relation");
        }
        const auto& attachment = *found->second;
        pose = attachment.child_interface_pose_in_parent_interface;
        source = CompositionComponentAttachment{
            attachment.component_attachment_id,
            attachment.parent.controllable_component_id,
            attachment.child.controllable_component_id,
            pose};
        tag = "attach";
        parent_link = compiled_link(attachment.parent.frame, symbols);
        child_link = compiled_link(attachment.child.frame, symbols);
        break;
      }
    }
    const auto joint_name = detail::make_mf1_generated_name(
        tag,
        {{"cell_id", graph.cell_id.value()},
         {"model_instance_id", model->model_instance_id.value()},
         {"parent_link", parent_link},
         {"child_link", child_link}});
    if (!all_compiled_names.insert(joint_name).second) {
      fail("generated fixed-connection name collided");
    }
    connections.push_back(
        {std::move(source), joint_name, std::move(parent_link),
         std::move(child_link), pose});
  }
  for (const auto& connection : connections) {
    require_room(flattened_urdf.joints.size(), 1U,
                 kMaxCompositionSymbols, "flattened joint");
    flattened_urdf.joints.push_back(fixed_joint(connection));
  }

  auto name_less = [](const auto& left, const auto& right) {
    return left.name < right.name;
  };
  std::sort(flattened_urdf.materials.begin(),
            flattened_urdf.materials.end(),
            [](const UrdfMaterialIr& left, const UrdfMaterialIr& right) {
              return left.name.value_or("") < right.name.value_or("");
            });
  std::sort(flattened_urdf.links.begin(), flattened_urdf.links.end(),
            name_less);
  std::sort(flattened_urdf.joints.begin(), flattened_urdf.joints.end(),
            name_less);
  std::sort(flattened_srdf.groups.begin(), flattened_srdf.groups.end(),
            name_less);
  std::sort(
      flattened_srdf.group_states.begin(),
      flattened_srdf.group_states.end(),
      [](const SrdfGroupStateIr& left, const SrdfGroupStateIr& right) {
        return std::tie(left.group_name, left.name) <
               std::tie(right.group_name, right.name);
      });
  std::sort(flattened_srdf.end_effectors.begin(),
            flattened_srdf.end_effectors.end(), name_less);
  std::sort(flattened_srdf.passive_joint_names.begin(),
            flattened_srdf.passive_joint_names.end());
  std::sort(
      flattened_srdf.disabled_collisions.begin(),
      flattened_srdf.disabled_collisions.end(),
      [](const SrdfDisabledCollisionIr& left,
         const SrdfDisabledCollisionIr& right) {
        return std::tie(left.first_link_name, left.second_link_name,
                        left.reason) <
               std::tie(right.first_link_name, right.second_link_name,
                        right.reason);
      });

  std::sort(frames.begin(), frames.end(),
            [](const CompositionFrameResolution& left,
               const CompositionFrameResolution& right) {
              return frame_sort_key(left.frame) <
                     frame_sort_key(right.frame);
            });
  std::sort(references.begin(), references.end(),
            [](const OccurrenceReferenceRewrite& left,
               const OccurrenceReferenceRewrite& right) {
              const auto left_owner =
                  std::tie(left.model_instance_id, left.kind,
                           left.owner_scope_name,
                           left.owner_source_name,
                           left.reference_ordinal);
              const auto right_owner =
                  std::tie(right.model_instance_id, right.kind,
                           right.owner_scope_name,
                           right.owner_source_name,
                           right.reference_ordinal);
              if (left_owner != right_owner) {
                return left_owner < right_owner;
              }
              const auto left_target = symbol_tuple(left.source_target);
              const auto right_target = symbol_tuple(right.source_target);
              if (left_target != right_target) {
                return left_target < right_target;
              }
              return left.compiled_target_name <
                     right.compiled_target_name;
            });

  const auto urdf_bytes =
      emit_canonical_urdf_document(flattened_urdf).size();
  const auto srdf_bytes =
      emit_canonical_srdf_document(flattened_srdf).size();
  if (urdf_bytes > kMaxCompositionFlattenedXmlBytes ||
      srdf_bytes > kMaxCompositionFlattenedXmlBytes - urdf_bytes) {
    fail("flattened cell XML exceeds its aggregate byte bound");
  }

  auto coordinate_maps = build_control_binding_maps(
      structure, components, models, assets, coordinates);
  auto targets = build_targets(structure, models, assets, symbols, frame_index);
  auto frame_paths = build_frame_paths(
      cell_frame, flattened_urdf, frames, connections, coordinates);
  auto reverse_rewrites = symbols.reverse();
  auto forward_rewrites = symbols.take_forward();

  return CompositionQualifiedCellIr(
      structure, cell_frame, std::move(flattened_urdf),
      std::move(flattened_srdf), std::move(occurrences),
      std::move(forward_rewrites), std::move(reverse_rewrites),
      std::move(references), std::move(frames), std::move(connections),
      std::move(frame_paths), std::move(coordinates),
      std::move(coordinate_maps), std::move(targets));
}

}  // namespace manyforge::deployment
