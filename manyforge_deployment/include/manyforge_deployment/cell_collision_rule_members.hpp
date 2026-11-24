#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/collision_rule.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>

namespace manyforge::deployment {

class RetainedBundle;
class VerifiedRetainedModelMap;

/// What a cell rule's fixed selections resolve against, per component of one
/// cell: the compiled links that carry collision bodies, each collision body
/// role's links and each SRDF group's links (by the group's source name, as
/// the component's command targets use it). Built from the cell's retained
/// model map, the same facts the execution host resolves cell rules from.
struct CellCollisionComponentFacts final {
  std::set<std::string> links;
  std::map<std::string, std::set<std::string>> collision_bodies;
  std::map<std::string, std::set<std::string>> model_groups;

  friend bool operator==(const CellCollisionComponentFacts &,
                         const CellCollisionComponentFacts &) = default;
};

struct CellCollisionFacts final {
  std::map<std::string, CellCollisionComponentFacts> components;

  friend bool operator==(const CellCollisionFacts &, const CellCollisionFacts &) = default;
};

[[nodiscard]] CellCollisionFacts cell_collision_facts(const VerifiedRetainedModelMap &model_map);

/// The compiled link names of SRDF group `group` (its source name) of
/// `component_id`, as one of the component's command targets uses it. Empty
/// when the component has no such group.
[[nodiscard]] std::vector<std::string> model_group_compiled_links(
    const VerifiedRetainedModelMap &model_map, const core::ControllableComponentId &component_id,
    std::string_view group);

/// One member of a rule side at Apply: a link of a component (`kLink`), the
/// objects a component carries (`kAttachedObject`, known only per move) or
/// every detached Scene object (`kSceneObjects`, known only per move).
struct CollisionRuleMember final {
  enum class Kind : std::uint8_t { kLink, kAttachedObject, kSceneObjects };
  Kind kind = Kind::kLink;
  std::string component_id;
  std::string link;

  friend auto operator<=>(const CollisionRuleMember &, const CollisionRuleMember &) = default;
  friend bool operator==(const CollisionRuleMember &, const CollisionRuleMember &) = default;
};

/// The members of one rule side, sorted and unique. A component selects the
/// links of its collision bodies, a collision body its links, a model group
/// the links of the group that carry collision bodies, a link itself.
[[nodiscard]] std::vector<CollisionRuleMember> collision_rule_side_members(
    const std::vector<core::CollisionRuleSelectionDTO> &side, const CellCollisionFacts &facts);

/// The first pair (one member of `a`, one of `b`) that two cell rules with
/// selections both hold, as the error admission gives for it
/// (`invalid_collision_rule`, reason `collision_rules_overlap`, with both rule
/// ids and the pair). The objects a component carries and the detached Scene
/// objects are compared as symbols. nullopt when no pair is held twice.
[[nodiscard]] std::optional<core::ErrorDTO> cell_collision_rules_overlap(
    const std::vector<core::CollisionRuleDTO> &rules, const CellCollisionFacts &facts);

/// The cell rule changes of an Apply, for its preview: one row per rule that
/// is added, removed or changed (its authored row or its resolved members),
/// in the candidate's rule order, then the removed rules:
/// `{rule_id, change: added|removed|changed, now, was}`, where `now` and `was`
/// are `{rule, a, b}` (the authored row and each side's members as
/// `{kind: link, component_id, link}`, `{kind: attached_object, component_id}`
/// or `{kind: scene_objects}`) or null.
[[nodiscard]] CanonicalValue cell_collision_rule_changes(
    const std::vector<core::CollisionRuleDTO> &current, const CellCollisionFacts &current_facts,
    const std::vector<core::CollisionRuleDTO> &candidate,
    const CellCollisionFacts &candidate_facts);

/// The candidate's triangle meshes that are not closed surfaces (OD-363),
/// each with the URI of such a mesh: by compiled link (its collision
/// geometry) and by Scene object.
struct CellOpenMeshes final {
  std::map<std::string, std::string> links;
  std::map<std::string, std::string> scene_objects;
};

/// The Apply preview's mesh-depth report (OD-363): a depth between two
/// meshes needs closed meshes, so each cell rule with a depth (a contact rule
/// with `max_penetration_m` or `max_compression_step_m`, a release rule with
/// signed evidence) and each geometric-contact input with `trip_depth_m`
/// that selects a mesh in `open` gives one row per such mesh:
/// `{code: mesh_not_closed, collision_rule_id | signal_id, member, mesh_uri}`,
/// `member` being `{kind: link, component_id, link}` or `{kind: scene_object,
/// scene_resource_id}`. A report, not a refusal. Carried objects and TCP
/// contact bodies exist per move and are not known here.
[[nodiscard]] CanonicalValue cell_mesh_depth_reports(
    const std::vector<core::CollisionRuleDTO> &rules,
    const std::vector<RetainedIoSignalConfiguration> &io_signals,
    const CellCollisionFacts &facts, const CellOpenMeshes &open);

/// The cell rules of a bundle's execution host; empty without one.
[[nodiscard]] std::vector<core::CollisionRuleDTO> bundle_cell_collision_rules(
    const RetainedBundle &bundle);

}  // namespace manyforge::deployment
