#include "manyforge_deployment/apply_component_delta.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>

namespace manyforge::deployment {
namespace {

namespace core = manyforge::core;
using Array = CanonicalValue::Array;
using Object = CanonicalValue::Object;

constexpr std::string_view kDeltaApiVersion = "manyforge.apply-component-delta.v1";
constexpr std::string_view kDeltaIdentityDomain =
    "manyforge.apply-component-delta";
constexpr std::string_view kRuntimeIdentityApiVersion =
    "manyforge.apply-component-runtime-identity.v2";
constexpr std::string_view kRuntimeIdentityDomain =
    "manyforge.apply-component-runtime-identity";
constexpr std::string_view kFamilyApiVersion =
    "manyforge.apply-component-runtime-family.v1";

[[noreturn]] void Fail(const std::string &detail) {
  throw ApplyComponentDeltaError(detail);
}

[[nodiscard]] const std::set<std::string_view> &KnownReasons() {
  static const std::set<std::string_view> reasons{
      kApplyComponentReasonComponent,   kApplyComponentReasonModelInstances,
      kApplyComponentReasonBindings,    kApplyComponentReasonDomains,
      kApplyComponentReasonRuntimeUnits, kApplyComponentReasonCapabilityProviders,
      kApplyComponentReasonRetainedPolicies,
  };
  return reasons;
}

template <typename Value>
[[nodiscard]] bool SortedUnique(const std::vector<Value> &values) {
  return std::adjacent_find(values.begin(), values.end(),
                            [](const auto &lhs, const auto &rhs) {
                              return !(lhs < rhs);
                            }) == values.end();
}

// -- Canonical projection access --------------------------------------------

[[nodiscard]] const Array &EmptyArray() {
  static const Array empty;
  return empty;
}

[[nodiscard]] const Array &MemberArray(const CanonicalValue &object,
                                       std::string_view name) {
  if (!object.is_object()) Fail("retained projection member is not an object");
  const auto *member = object.find_object_member(name);
  if (member == nullptr || member->is_null()) return EmptyArray();
  if (!member->is_array()) {
    Fail("retained projection member " + std::string(name) + " is not an array");
  }
  return member->as_array();
}

[[nodiscard]] const CanonicalValue &MemberObject(const CanonicalValue &object,
                                                 std::string_view name) {
  if (!object.is_object()) Fail("retained projection member is not an object");
  const auto *member = object.find_object_member(name);
  if (member == nullptr || !member->is_object()) {
    Fail("retained projection lacks object member " + std::string(name));
  }
  return *member;
}

/// Elements of `array` whose `key` member is one of `ids`, in canonical order,
/// optionally without placement members that belong to shared Scene data.
[[nodiscard]] CanonicalValue Select(const Array &array, std::string_view key,
                                    const std::set<std::string> &ids,
                                    bool strip_placement = false) {
  Array selected;
  for (const auto &element : array) {
    if (!element.is_object()) Fail("retained projection record is not an object");
    const auto *identity = element.find_object_member(key);
    if (identity == nullptr || !identity->is_string()) {
      Fail("retained projection record lacks string " + std::string(key));
    }
    if (!ids.contains(identity->as_string())) continue;
    if (!strip_placement) {
      selected.push_back(element);
      continue;
    }
    Object stripped;
    for (const auto &[name, value] : element.as_object()) {
      if (name != "root_pose_in_cell") stripped.emplace_back(name, value);
    }
    selected.emplace_back(std::move(stripped));
  }
  return CanonicalValue(std::move(selected));
}

template <typename Id>
[[nodiscard]] std::set<std::string> Strings(const std::set<Id> &ids) {
  std::set<std::string> result;
  for (const auto &id : ids) result.insert(id.value());
  return result;
}

// -- Component relations from the typed bundle ------------------------------

struct ComponentRelations final {
  std::set<core::ModelInstanceId> model_instances;
  std::set<core::ControlBindingId> bindings;
  std::set<core::ControlDomainId> domains;
  std::set<core::RuntimeUnitId> runtime_units;
  std::set<core::CapabilityProviderBindingId> providers;
};

[[nodiscard]] const core::ControllableComponentDTO *
FindComponent(const core::RetainedBundleDTO &bundle,
              const core::ControllableComponentId &id) {
  const auto &components = bundle.cell_graph.controllable_components;
  const auto found = std::find_if(
      components.begin(), components.end(),
      [&](const auto &component) { return component.controllable_component_id == id; });
  return found == components.end() ? nullptr : &*found;
}

[[nodiscard]] ComponentRelations
Relations(const core::RetainedBundleDTO &bundle,
          const core::ControllableComponentDTO &component) {
  const auto &graph = bundle.cell_graph;
  ComponentRelations relations;
  for (const auto &role : component.model_role_bindings) {
    relations.model_instances.insert(role.model_instance_id);
  }
  relations.runtime_units.insert(component.runtime_unit_ids.begin(),
                                 component.runtime_unit_ids.end());
  for (const auto &binding : graph.control_bindings) {
    if (binding.controllable_component_id != component.controllable_component_id) {
      continue;
    }
    relations.bindings.insert(binding.control_binding_id);
    relations.domains.insert(binding.control_domain_id);
    relations.runtime_units.insert(binding.provider_runtime_unit_id);
  }
  for (const auto &domain : graph.control_domains) {
    if (relations.domains.contains(domain.control_domain_id)) {
      relations.runtime_units.insert(domain.runtime_unit_ids.begin(),
                                     domain.runtime_unit_ids.end());
    }
  }
  // A component depends on every runtime unit its units depend on.
  bool grew = true;
  while (grew) {
    grew = false;
    for (const auto &unit : graph.runtime_units) {
      if (!relations.runtime_units.contains(unit.runtime_unit_id)) continue;
      for (const auto &dependency : unit.dependency_runtime_unit_ids) {
        grew = relations.runtime_units.insert(dependency).second || grew;
      }
    }
  }
  for (const auto &provider : bundle.capability_provider_bindings) {
    const bool targets_component = std::visit(
        [&](const auto &target) {
          using Target = std::decay_t<decltype(target)>;
          if constexpr (std::is_same_v<Target,
                                       core::CellGlobalCapabilityProviderTargetDTO>) {
            return true;
          } else if constexpr (std::is_same_v<
                                   Target,
                                   core::TargetBindingCapabilityProviderTargetDTO>) {
            return relations.bindings.contains(target.control_binding_id);
          } else {
            static_assert(std::is_same_v<
                          Target, core::ControlDomainCapabilityProviderTargetDTO>);
            return relations.domains.contains(target.control_domain_id);
          }
        },
        provider.target);
    if (targets_component) {
      relations.providers.insert(provider.capability_provider_binding_id);
    }
  }
  return relations;
}

// -- Runtime identity --------------------------------------------------------

[[nodiscard]] CanonicalValue RuntimeUnitProjection(
    const RetainedBundle &bundle, const core::ControllableComponentId &component,
    const std::set<std::string> &units) {
  auto selected = Select(
      MemberArray(MemberObject(bundle.normalized_root(), "cell_graph"),
                  "runtime_units"),
      "runtime_unit_id", units);
  auto rows = selected.as_array();
  for (auto &row : rows) {
    const auto *profile = row.find_object_member("runtime_profile_id");
    if (profile == nullptr || !profile->is_string() ||
        (profile->as_string() != native_execution_runtime_contract::kRuntimeProfileId &&
         profile->as_string() != ros2_control_execution_runtime_contract::kRuntimeProfileId)) {
      continue;
    }
    const auto *id = row.find_object_member("runtime_unit_id");
    if (id == nullptr || !id->is_string()) {
      Fail("native runtime identity lacks its unit");
    }
    auto fields = row.as_object();
    for (auto &[name, value] : fields) {
      if (name == "configuration") {
        value = profile->as_string() == native_execution_runtime_contract::kRuntimeProfileId
            ? native_execution_runtime_contract::component_configuration_projection(
                  bundle, core::RuntimeUnitId(id->as_string()), component)
            : ros2_control_execution_runtime_contract::component_configuration_projection(
                  bundle, core::RuntimeUnitId(id->as_string()), component);
      }
    }
    row = CanonicalValue(std::move(fields));
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue RuntimeRequirementProjection(
    const RetainedBundle &bundle, const std::set<std::string> &units) {
  auto selected = units;
  for (const auto &unit : bundle.value().cell_graph.runtime_units) {
    if (unit.runtime_profile_id.value() ==
        native_execution_runtime_contract::kRuntimeProfileId ||
        unit.runtime_profile_id.value() ==
            ros2_control_execution_runtime_contract::kRuntimeProfileId) {
      // The host's requirement is the union of all installed components.
      // The verified host offer covers that union; each component's actual
      // requirements are already bound by its own bindings/providers below.
      // Hashing the derived union again would couple unrelated controllers.
      selected.erase(unit.runtime_unit_id.value());
    }
  }
  return Select(MemberArray(bundle.normalized_root(),
                            "runtime_contract_requirements"),
                "runtime_unit_id", selected);
}

/// The canonical fact families one component's runtime depends on, keyed by
/// the replacement reason each family reports.
[[nodiscard]] std::map<std::string_view, CanonicalValue>
RuntimeFamilies(const RetainedBundle &bundle,
                const core::ControllableComponentDTO &component) {
  const auto &root = bundle.normalized_root();
  const auto &graph = MemberObject(root, "cell_graph");
  const auto &products = MemberObject(root, "compiled_products");
  const auto relations = Relations(bundle.value(), component);
  const auto bindings = Strings(relations.bindings);
  const auto domains = Strings(relations.domains);
  const auto units = Strings(relations.runtime_units);

  std::map<std::string_view, CanonicalValue> families;
  families.emplace(kApplyComponentReasonComponent,
                   Select(MemberArray(graph, "controllable_components"),
                          "controllable_component_id",
                          {component.controllable_component_id.value()}, true));
  families.emplace(kApplyComponentReasonModelInstances,
                   Select(MemberArray(graph, "model_instances"), "model_instance_id",
                          Strings(relations.model_instances), true));
  families.emplace(
      kApplyComponentReasonBindings,
      CanonicalValue(Object{
          {"control_bindings",
           Select(MemberArray(graph, "control_bindings"), "control_binding_id", bindings)},
      }));
  families.emplace(
      kApplyComponentReasonDomains,
      CanonicalValue(Object{
          {"control_domains",
           Select(MemberArray(graph, "control_domains"), "control_domain_id", domains)},
          {"desired_control_configurations",
           Select(MemberArray(root, "desired_control_configurations"), "control_domain_id",
                  domains)},
          {"control_domain_products",
           Select(MemberArray(products, "control_domain_products"), "control_domain_id",
                  domains)},
      }));
  families.emplace(
      kApplyComponentReasonRuntimeUnits,
      CanonicalValue(Object{
          {"runtime_units",
           RuntimeUnitProjection(bundle, component.controllable_component_id, units)},
          {"runtime_unit_products",
           Select(MemberArray(products, "runtime_unit_products"), "runtime_unit_id", units)},
          {"runtime_contract_requirements",
           RuntimeRequirementProjection(bundle, units)},
      }));
  families.emplace(kApplyComponentReasonCapabilityProviders,
                   Select(MemberArray(root, "capability_provider_bindings"),
                          "capability_provider_binding_id",
                          Strings(relations.providers)));
  // Retained policy records are cell-wide catalog references; a change is an
  // explicit, reported dependency of every component rather than a hidden
  // cell-wide fallback.
  families.emplace(kApplyComponentReasonRetainedPolicies,
                   CanonicalValue(MemberArray(root, "retained_policy_records")));
  return families;
}

[[nodiscard]] core::ContentDigest
FamiliesIdentity(const core::ControllableComponentId &id,
                 const std::map<std::string_view, CanonicalValue> &families) {
  Object payload{{"controllable_component_id", CanonicalValue(id.value())}};
  for (const auto &[family, value] : families) {
    payload.emplace_back(std::string(family), value);
  }
  return core::ContentDigest(content_identity::framed_sha256(
      kRuntimeIdentityDomain, kRuntimeIdentityApiVersion,
      CanonicalValue(std::move(payload))));
}

[[nodiscard]] std::string FamilyDigest(const CanonicalValue &value) {
  return content_identity::framed_sha256(kRuntimeIdentityDomain, kFamilyApiVersion, value);
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
  for (const auto &[id, edge] : old_edges) {
    const auto found = new_edges.find(id);
    if (found == new_edges.end() || *found->second != *edge) {
      mark(*edge);
      if (found != new_edges.end()) mark(*found->second);
    }
  }
  for (const auto &[id, edge] : new_edges) {
    if (!old_edges.contains(id)) mark(*edge);
  }
  return {changed.begin(), changed.end()};
}

[[nodiscard]] std::vector<core::ControlDomainId>
SortedDomains(const std::set<core::ControlDomainId> &domains) {
  return {domains.begin(), domains.end()};
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
                   [](const auto &reason) { return KnownReasons().contains(reason); })) {
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
    const RetainedBundle &bundle, const core::ControllableComponentId &controllable_component_id) {
  const auto *component = FindComponent(bundle.value(), controllable_component_id);
  if (component == nullptr) return std::nullopt;
  return FamiliesIdentity(controllable_component_id, RuntimeFamilies(bundle, *component));
}

ApplyComponentDeltaDTO derive_apply_component_delta(const RetainedBundle &predecessor,
                                                    const RetainedBundle &candidate) {
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
    const auto *old_component = FindComponent(old_bundle, id);
    const auto *new_component = FindComponent(new_bundle, id);
    ApplyComponentChangeDTO change{
        .controllable_component_id = id,
        .disposition = ApplyComponentDisposition::kRetain,
        .predecessor_runtime_identity = std::nullopt,
        .candidate_runtime_identity = std::nullopt,
        .predecessor_domains = {},
        .candidate_domains = {},
        .replacement_reasons = {}};
    std::map<std::string_view, CanonicalValue> old_families;
    std::map<std::string_view, CanonicalValue> new_families;
    if (old_component != nullptr) {
      old_families = RuntimeFamilies(predecessor, *old_component);
      change.predecessor_runtime_identity = FamiliesIdentity(id, old_families);
      change.predecessor_domains = SortedDomains(Relations(old_bundle, *old_component).domains);
    }
    if (new_component != nullptr) {
      new_families = RuntimeFamilies(candidate, *new_component);
      change.candidate_runtime_identity = FamiliesIdentity(id, new_families);
      change.candidate_domains = SortedDomains(Relations(new_bundle, *new_component).domains);
    }
    if (old_component == nullptr) {
      change.disposition = ApplyComponentDisposition::kAdd;
    } else if (new_component == nullptr) {
      change.disposition = ApplyComponentDisposition::kRemove;
    } else if (*change.predecessor_runtime_identity == *change.candidate_runtime_identity) {
      change.disposition = ApplyComponentDisposition::kRetain;
    } else {
      change.disposition = ApplyComponentDisposition::kReplace;
      for (const auto &[family, value] : old_families) {
        const auto found = new_families.find(family);
        if (found == new_families.end() || FamilyDigest(found->second) != FamilyDigest(value)) {
          change.replacement_reasons.emplace_back(family);
        }
      }
      std::sort(change.replacement_reasons.begin(), change.replacement_reasons.end());
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
