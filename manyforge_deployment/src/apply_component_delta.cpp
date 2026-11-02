#include "manyforge_deployment/apply_component_delta.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/component_spec.hpp>

namespace manyforge::deployment {
namespace {

namespace core = manyforge::core;
using Array = CanonicalValue::Array;
using Object = CanonicalValue::Object;

constexpr std::string_view kDeltaApiVersion = "manyforge.apply-component-delta.v1";
constexpr std::string_view kDeltaIdentityDomain =
    "manyforge.apply-component-delta";

[[noreturn]] void Fail(const std::string &detail) {
  throw ApplyComponentDeltaError(detail);
}

[[nodiscard]] bool KnownReason(const std::string_view reason) {
  return std::find(kComponentSpecSections.begin(), kComponentSpecSections.end(), reason) !=
         kComponentSpecSections.end();
}

template <typename Value>
[[nodiscard]] bool SortedUnique(const std::vector<Value> &values) {
  return std::adjacent_find(values.begin(), values.end(),
                            [](const auto &lhs, const auto &rhs) {
                              return !(lhs < rhs);
                            }) == values.end();
}

[[nodiscard]] bool HasComponent(const core::RetainedBundleDTO &bundle,
                                const core::ControllableComponentId &id) {
  const auto &components = bundle.cell_graph.controllable_components;
  return std::any_of(components.begin(), components.end(), [&](const auto &component) {
    return component.controllable_component_id == id;
  });
}

// -- Shared Scene projection ------------------------------------------------

[[nodiscard]] bool SharedSceneProjectionChanged(const core::RetainedBundleDTO &old_bundle,
                                                const core::RetainedBundleDTO &new_bundle) {
  const auto &old_products = old_bundle.compiled_products;
  const auto &new_products = new_bundle.compiled_products;
  if (old_products.planning_collision_model != new_products.planning_collision_model ||
      old_products.viewer_model != new_products.viewer_model ||
      old_products.symbol_role_maps != new_products.symbol_role_maps ||
      old_bundle.cell_graph.component_attachments !=
          new_bundle.cell_graph.component_attachments) {
    return true;
  }
  const auto placements = [](const core::RetainedBundleDTO &bundle) {
    std::map<std::string, std::optional<core::PlacementPoseDTO>> result;
    for (const auto &model : bundle.cell_graph.model_instances) {
      result.emplace("model:" + model.model_instance_id.value(), model.root_pose_in_cell);
    }
    for (const auto &component : bundle.cell_graph.controllable_components) {
      result.emplace("component:" + component.controllable_component_id.value(),
                     component.root_pose_in_cell);
    }
    return result;
  };
  return placements(old_bundle) != placements(new_bundle);
}

[[nodiscard]] std::vector<core::ControllableComponentId>
AttachmentChangedComponents(const core::CellGraphDTO &old_graph,
                            const core::CellGraphDTO &new_graph) {
  std::map<core::ComponentAttachmentId, const core::ComponentAttachmentEdgeDTO *> old_edges;
  std::map<core::ComponentAttachmentId, const core::ComponentAttachmentEdgeDTO *> new_edges;
  for (const auto &edge : old_graph.component_attachments) {
    old_edges.emplace(edge.component_attachment_id, &edge);
  }
  for (const auto &edge : new_graph.component_attachments) {
    new_edges.emplace(edge.component_attachment_id, &edge);
  }
  std::set<core::ControllableComponentId> changed;
  const auto mark = [&](const core::ComponentAttachmentEdgeDTO &edge) {
    changed.insert(edge.parent.controllable_component_id);
    changed.insert(edge.child.controllable_component_id);
  };
  // The mount itself; its collision exclusions (and the revision that hashes
  // them) only change the arm's planning model, which is cell context.
  const auto same_mount = [](const core::ComponentAttachmentEdgeDTO &left,
                             const core::ComponentAttachmentEdgeDTO &right) {
    return left.parent == right.parent && left.child == right.child &&
           left.child_interface_pose_in_parent_interface ==
               right.child_interface_pose_in_parent_interface &&
           left.primary == right.primary;
  };
  for (const auto &[id, edge] : old_edges) {
    const auto found = new_edges.find(id);
    if (found == new_edges.end() || !same_mount(*found->second, *edge)) {
      mark(*edge);
      if (found != new_edges.end()) mark(*found->second);
    }
  }
  for (const auto &[id, edge] : new_edges) {
    if (!old_edges.contains(id)) mark(*edge);
  }
  return {changed.begin(), changed.end()};
}

// -- Codec -------------------------------------------------------------------

[[nodiscard]] CanonicalValue OptionalDigest(const std::optional<core::ContentDigest> &value) {
  return value.has_value() ? CanonicalValue(value->value()) : CanonicalValue(nullptr);
}

template <typename Id>
[[nodiscard]] CanonicalValue IdArray(const std::vector<Id> &ids) {
  Array result;
  result.reserve(ids.size());
  for (const auto &id : ids) result.emplace_back(id.value());
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue DeltaValue(const ApplyComponentDeltaDTO &delta) {
  Array components;
  components.reserve(delta.components.size());
  for (const auto &change : delta.components) {
    Array reasons;
    for (const auto &reason : change.replacement_reasons) reasons.emplace_back(reason);
    components.emplace_back(Object{
        {"candidate_domains", IdArray(change.candidate_domains)},
        {"candidate_runtime_identity", OptionalDigest(change.candidate_runtime_identity)},
        {"controllable_component_id", CanonicalValue(change.controllable_component_id.value())},
        {"disposition", CanonicalValue(std::string(to_string(change.disposition)))},
        {"predecessor_domains", IdArray(change.predecessor_domains)},
        {"predecessor_runtime_identity",
         OptionalDigest(change.predecessor_runtime_identity)},
        {"replacement_reasons", CanonicalValue(std::move(reasons))},
    });
  }
  return CanonicalValue(Object{
      {"api_version", CanonicalValue(std::string(kDeltaApiVersion))},
      {"attachment_changed_components", IdArray(delta.attachment_changed_components)},
      {"components", CanonicalValue(std::move(components))},
      {"shared_scene_projection_changed", CanonicalValue(delta.shared_scene_projection_changed)},
  });
}

[[nodiscard]] const CanonicalValue &Required(const CanonicalValue &object,
                                             std::string_view name) {
  const auto *member = object.is_object() ? object.find_object_member(name) : nullptr;
  if (member == nullptr) Fail("component delta lacks member " + std::string(name));
  return *member;
}

[[nodiscard]] std::optional<core::ContentDigest> DecodeOptionalDigest(const CanonicalValue &value) {
  if (value.is_null()) return std::nullopt;
  if (!value.is_string()) Fail("component delta identity is not a string or null");
  return core::ContentDigest(value.as_string());
}

template <typename Id>
[[nodiscard]] std::vector<Id> DecodeIds(const CanonicalValue &value) {
  if (!value.is_array()) Fail("component delta identity list is not an array");
  std::vector<Id> ids;
  for (const auto &element : value.as_array()) {
    if (!element.is_string()) Fail("component delta identity is not a string");
    ids.emplace_back(element.as_string());
  }
  return ids;
}

[[nodiscard]] ApplyComponentDisposition DecodeDisposition(const CanonicalValue &value) {
  if (!value.is_string()) Fail("component delta disposition is not a string");
  for (const auto disposition :
       {ApplyComponentDisposition::kRetain, ApplyComponentDisposition::kAdd,
        ApplyComponentDisposition::kRemove, ApplyComponentDisposition::kReplace}) {
    if (to_string(disposition) == value.as_string()) return disposition;
  }
  Fail("component delta disposition is unknown");
}

} // namespace

std::string_view to_string(const ApplyComponentDisposition disposition) noexcept {
  switch (disposition) {
    case ApplyComponentDisposition::kRetain:
      return "retain";
    case ApplyComponentDisposition::kAdd:
      return "add";
    case ApplyComponentDisposition::kRemove:
      return "remove";
    case ApplyComponentDisposition::kReplace:
      return "replace";
  }
  return "";
}

bool ApplyComponentChangeDTO::valid() const noexcept {
  const auto digest_valid = [](const std::optional<core::ContentDigest> &digest) {
    return !digest.has_value() || content_identity::is_sha256_digest(digest->value());
  };
  if (controllable_component_id.empty() || !digest_valid(predecessor_runtime_identity) ||
      !digest_valid(candidate_runtime_identity) || !SortedUnique(predecessor_domains) ||
      !SortedUnique(candidate_domains) ||
      predecessor_domains.size() > kMaximumApplyComponentDeltaDomains ||
      candidate_domains.size() > kMaximumApplyComponentDeltaDomains ||
      !SortedUnique(replacement_reasons) ||
      !std::all_of(replacement_reasons.begin(), replacement_reasons.end(),
                   [](const auto &reason) { return KnownReason(reason); })) {
    return false;
  }
  switch (disposition) {
    case ApplyComponentDisposition::kAdd:
      return !predecessor_runtime_identity && candidate_runtime_identity &&
             predecessor_domains.empty() && replacement_reasons.empty();
    case ApplyComponentDisposition::kRemove:
      return predecessor_runtime_identity && !candidate_runtime_identity &&
             candidate_domains.empty() && replacement_reasons.empty();
    case ApplyComponentDisposition::kRetain:
      return predecessor_runtime_identity && candidate_runtime_identity &&
             *predecessor_runtime_identity == *candidate_runtime_identity &&
             predecessor_domains == candidate_domains && replacement_reasons.empty();
    case ApplyComponentDisposition::kReplace:
      return predecessor_runtime_identity && candidate_runtime_identity &&
             *predecessor_runtime_identity != *candidate_runtime_identity &&
             !replacement_reasons.empty();
  }
  return false;
}

bool ApplyComponentDeltaDTO::valid() const noexcept {
  return components.size() <= kMaximumApplyComponentDeltaRows &&
         std::adjacent_find(components.begin(), components.end(),
                            [](const auto &lhs, const auto &rhs) {
                              return !(lhs.controllable_component_id <
                                       rhs.controllable_component_id);
                            }) == components.end() &&
         std::all_of(components.begin(), components.end(),
                     [](const auto &change) { return change.valid(); }) &&
         SortedUnique(attachment_changed_components) &&
         attachment_changed_components.size() <= kMaximumApplyComponentDeltaRows;
}

std::string EncodeApplyComponentDelta(const ApplyComponentDeltaDTO &delta) {
  if (!delta.valid()) Fail("component delta is invalid");
  return canonical_json(DeltaValue(delta));
}

ApplyComponentDeltaDTO DecodeApplyComponentDelta(const std::string_view encoded) {
  try {
    const auto root = content_identity::parse_canonical_json(encoded);
    if (!root.is_object() || root.as_object().size() != 4U ||
        !Required(root, "api_version").is_string() ||
        Required(root, "api_version").as_string() != kDeltaApiVersion ||
        !Required(root, "shared_scene_projection_changed").is_bool() ||
        !Required(root, "components").is_array()) {
      Fail("component delta envelope is not exact");
    }
    ApplyComponentDeltaDTO delta;
    delta.shared_scene_projection_changed =
        Required(root, "shared_scene_projection_changed").as_bool();
    delta.attachment_changed_components =
        DecodeIds<core::ControllableComponentId>(Required(root, "attachment_changed_components"));
    for (const auto &row : Required(root, "components").as_array()) {
      if (!row.is_object() || row.as_object().size() != 7U) {
        Fail("component delta row is not exact");
      }
      const auto &reasons = Required(row, "replacement_reasons");
      if (!reasons.is_array()) Fail("component delta reasons are not an array");
      ApplyComponentChangeDTO change{
          .controllable_component_id = core::ControllableComponentId(
              Required(row, "controllable_component_id").is_string()
                  ? Required(row, "controllable_component_id").as_string()
                  : std::string{}),
          .disposition = ApplyComponentDisposition::kRetain,
          .predecessor_runtime_identity = std::nullopt,
          .candidate_runtime_identity = std::nullopt,
          .predecessor_domains = {},
          .candidate_domains = {},
          .replacement_reasons = {}};
      change.disposition = DecodeDisposition(Required(row, "disposition"));
      change.predecessor_runtime_identity =
          DecodeOptionalDigest(Required(row, "predecessor_runtime_identity"));
      change.candidate_runtime_identity =
          DecodeOptionalDigest(Required(row, "candidate_runtime_identity"));
      change.predecessor_domains =
          DecodeIds<core::ControlDomainId>(Required(row, "predecessor_domains"));
      change.candidate_domains = DecodeIds<core::ControlDomainId>(Required(row, "candidate_domains"));
      for (const auto &reason : reasons.as_array()) {
        if (!reason.is_string()) Fail("component delta reason is not a string");
        change.replacement_reasons.push_back(reason.as_string());
      }
      delta.components.push_back(std::move(change));
    }
    if (!delta.valid() || canonical_json(DeltaValue(delta)) != encoded) {
      Fail("component delta is not canonical or valid");
    }
    return delta;
  } catch (const ApplyComponentDeltaError &) {
    throw;
  } catch (const std::exception &error) {
    Fail(std::string("component delta cannot be decoded: ") + error.what());
  }
}

core::ContentDigest DigestApplyComponentDelta(const ApplyComponentDeltaDTO &delta) {
  if (!delta.valid()) Fail("component delta is invalid");
  return core::ContentDigest(
      content_identity::framed_sha256(kDeltaIdentityDomain, kDeltaApiVersion, DeltaValue(delta)));
}

std::vector<core::ControllableComponentId>
detached_attachment_children(const core::CellGraphDTO &predecessor,
                             const core::CellGraphDTO &candidate) {
  std::set<core::ControllableComponentId> old_children;
  for (const auto &edge : predecessor.component_attachments) {
    old_children.insert(edge.child.controllable_component_id);
  }
  std::set<core::ControllableComponentId> new_children;
  for (const auto &edge : candidate.component_attachments) {
    new_children.insert(edge.child.controllable_component_id);
  }
  std::vector<core::ControllableComponentId> detached;
  for (const auto &component : candidate.controllable_components) {
    const auto &id = component.controllable_component_id;
    if (!old_children.contains(id) || new_children.contains(id)) continue;
    if (!component.model_role_bindings.empty() &&
        !component.root_pose_in_cell.has_value()) {
      Fail("detached attachment child " + id.value() +
           " has no verified root pose in the candidate");
    }
    detached.push_back(id);
  }
  std::sort(detached.begin(), detached.end());
  return detached;
}

bool apply_component_delta_is_fenced(const ApplyComponentDeltaDTO &delta,
                                     const core::ConfigurationFenceScopeDTO &scope) noexcept {
  const auto &fenced = scope.controllable_component_ids;
  return delta.valid() &&
         std::all_of(delta.components.begin(), delta.components.end(), [&](const auto &change) {
           return change.disposition == ApplyComponentDisposition::kRetain ||
                  (change.predecessor_domains.empty() && change.candidate_domains.empty()) ||
                  std::find(fenced.begin(), fenced.end(), change.controllable_component_id) !=
                      fenced.end();
         });
}

std::optional<core::ContentDigest> derive_component_runtime_identity(
    const RetainedBundle &bundle, const SiteFacts &site_facts,
    const core::ControllableComponentId &controllable_component_id) {
  if (!HasComponent(bundle.value(), controllable_component_id)) return std::nullopt;
  return component_spec_fingerprint(
      component_spec(bundle, site_facts, controllable_component_id));
}

ApplyComponentDeltaDTO derive_apply_component_delta(const RetainedBundle &predecessor,
                                                    const RetainedBundle &candidate,
                                                    const SiteFacts &site_facts) {
  if (!verify_retained_bundle_identity(predecessor) ||
      !verify_retained_bundle_identity(candidate)) {
    Fail("component delta input failed retained-bundle identity proof");
  }
  const auto &old_bundle = predecessor.value();
  const auto &new_bundle = candidate.value();
  if (old_bundle.cell_graph.cell_id != new_bundle.cell_graph.cell_id) {
    Fail("component delta bundles belong to different cells");
  }

  std::set<core::ControllableComponentId> ids;
  for (const auto *bundle : {&old_bundle, &new_bundle}) {
    for (const auto &component : bundle->cell_graph.controllable_components) {
      ids.insert(component.controllable_component_id);
    }
  }
  if (ids.size() > kMaximumApplyComponentDeltaRows) {
    Fail("component delta exceeds its component bound");
  }

  ApplyComponentDeltaDTO delta;
  delta.components.reserve(ids.size());
  for (const auto &id : ids) {
    ApplyComponentChangeDTO change{
        .controllable_component_id = id,
        .disposition = ApplyComponentDisposition::kRetain,
        .predecessor_runtime_identity = std::nullopt,
        .candidate_runtime_identity = std::nullopt,
        .predecessor_domains = {},
        .candidate_domains = {},
        .replacement_reasons = {}};
    std::optional<ComponentSpec> old_spec;
    std::optional<ComponentSpec> new_spec;
    if (HasComponent(old_bundle, id)) {
      old_spec = component_spec(predecessor, site_facts, id);
      change.predecessor_runtime_identity = component_spec_fingerprint(*old_spec);
      change.predecessor_domains = old_spec->control_domains;
    }
    if (HasComponent(new_bundle, id)) {
      new_spec = component_spec(candidate, site_facts, id);
      change.candidate_runtime_identity = component_spec_fingerprint(*new_spec);
      change.candidate_domains = new_spec->control_domains;
    }
    if (!old_spec) {
      change.disposition = ApplyComponentDisposition::kAdd;
    } else if (!new_spec) {
      change.disposition = ApplyComponentDisposition::kRemove;
    } else if (*change.predecessor_runtime_identity == *change.candidate_runtime_identity) {
      change.disposition = ApplyComponentDisposition::kRetain;
    } else {
      change.disposition = ApplyComponentDisposition::kReplace;
      // Reasons: the spec sections that differ (kComponentSpecSections is sorted).
      for (const auto section : kComponentSpecSections)
        if (old_spec->section(section) != new_spec->section(section))
          change.replacement_reasons.emplace_back(section);
    }
    delta.components.push_back(std::move(change));
  }
  delta.shared_scene_projection_changed = SharedSceneProjectionChanged(old_bundle, new_bundle);
  delta.attachment_changed_components =
      AttachmentChangedComponents(old_bundle.cell_graph, new_bundle.cell_graph);
  if (!delta.valid()) Fail("derived component delta is not canonical");
  return delta;
}

} // namespace manyforge::deployment
