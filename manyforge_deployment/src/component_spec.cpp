#include "manyforge_deployment/component_spec.hpp"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/deployment_applicability_codec.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>

namespace manyforge::deployment {
namespace {

namespace core = manyforge::core;
namespace host_contract = ros2_control_execution_runtime_contract;
using Array = CanonicalValue::Array;
using Object = CanonicalValue::Object;

constexpr std::string_view kSpecIdentityDomain = "manyforge.component-spec";
constexpr std::string_view kSpecApiVersion = "manyforge.component-spec.v1";

[[noreturn]] void Fail(const std::string &detail) {
  throw ComponentSpecError("component spec: " + detail);
}

// -- Execution-host configuration tiers -------------------------------------
// Every field of the execution host's configuration has exactly one tier
// (execution_host_field_has_tier; a test over every example holds it).

/// One component's rows, which the host launches it with.
constexpr std::array<std::string_view, 6> kHostLaunchRows{
    "controller_profiles", "default_fault_stop_profiles",
    "gripper_target_configurations", "io_signals", "machines",
    "motion_target_limits"};
/// One component's adapter configuration rows.
constexpr std::array<std::string_view, 6> kHostAdapterRows{
    "ai_models", "component_streams", "io_provider_devices",
    "lifecycle_command_timeouts", "pose_sources", "sensors"};
/// Bound when the host constructs a motion component's execution kernel
/// (ExecutionKernelConfig, its runtime collision monitor): a change needs that
/// component rebuilt, so they belong to the spec of every motion component.
constexpr std::array<std::string_view, 10> kHostKernelScalars{
    "collision_monitor_period_ms",
    "execution_tick_hz",
    "runtime_collision_immediate_query_budget_ms",
    "runtime_collision_lookahead_cycle_budget_ms",
    "runtime_collision_lookahead_max_samples",
    "runtime_collision_lookahead_query_budget_ms",
    "runtime_collision_lookahead_s",
    "runtime_collision_lookahead_workers",
    "runtime_dynamic_body_max_age_cap_ms",
    "validation_collision_workers"};
/// Planner settings: CellContext.
constexpr std::array<std::string_view, 10> kHostPlannerScalars{
    "cartesian_timing",  "collision_check_fresh_plans", "collision_check_step_rad",
    "collision_clearance_m", "motion_timing",           "planner_max_nodes",
    "planner_n",         "planner_optimize",            "planner_rng_seed",
    "planner_search"};
/// The component's slice of the cell's initial joint state (launch).
constexpr std::string_view kHostInitialPositions = "initial_joint_positions_rad";

template <std::size_t N>
[[nodiscard]] bool Contains(const std::array<std::string_view, N> &names,
                            std::string_view name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

// -- Canonical projection access --------------------------------------------

[[nodiscard]] const Array &EmptyArray() {
  static const Array empty;
  return empty;
}

[[nodiscard]] const Array &MemberArray(const CanonicalValue &object,
                                       std::string_view name) {
  if (!object.is_object()) Fail("projection member is not an object");
  const auto *member = object.find_object_member(name);
  if (member == nullptr || member->is_null()) return EmptyArray();
  if (!member->is_array()) Fail(std::string(name) + " is not an array");
  return member->as_array();
}

[[nodiscard]] const CanonicalValue &MemberObject(const CanonicalValue &object,
                                                 std::string_view name) {
  const auto *member = object.is_object() ? object.find_object_member(name) : nullptr;
  if (member == nullptr || !member->is_object()) Fail("projection lacks object " + std::string(name));
  return *member;
}

[[nodiscard]] CanonicalValue Optional(const CanonicalValue &object, std::string_view name) {
  const auto *member = object.find_object_member(name);
  return member == nullptr ? CanonicalValue(nullptr) : *member;
}

[[nodiscard]] const std::string *StringMember(const CanonicalValue &row,
                                              std::string_view name) {
  const auto *member = row.is_object() ? row.find_object_member(name) : nullptr;
  return member != nullptr && member->is_string() ? &member->as_string() : nullptr;
}

[[nodiscard]] CanonicalValue Without(const CanonicalValue &row,
                                     std::initializer_list<std::string_view> names) {
  Object kept;
  for (const auto &[name, value] : row.as_object())
    if (std::find(names.begin(), names.end(), name) == names.end())
      kept.emplace_back(name, value);
  return CanonicalValue(std::move(kept));
}

/// Elements of `array` whose `key` is one of `ids`, in canonical order.
[[nodiscard]] CanonicalValue Select(const Array &array, std::string_view key,
                                    const std::set<std::string> &ids) {
  Array selected;
  for (const auto &element : array) {
    const auto *identity = StringMember(element, key);
    if (identity == nullptr) Fail("record lacks string " + std::string(key));
    if (ids.contains(*identity)) selected.push_back(element);
  }
  return CanonicalValue(std::move(selected));
}

template <typename Id>
[[nodiscard]] std::set<std::string> Strings(const std::set<Id> &ids) {
  std::set<std::string> result;
  for (const auto &id : ids) result.insert(id.value());
  return result;
}

[[nodiscard]] const core::ControllableComponentDTO *
FindComponent(const core::RetainedBundleDTO &bundle, const core::ControllableComponentId &id) {
  const auto &components = bundle.cell_graph.controllable_components;
  const auto found = std::find_if(components.begin(), components.end(), [&](const auto &row) {
    return row.controllable_component_id == id;
  });
  return found == components.end() ? nullptr : &*found;
}

// -- Relations ---------------------------------------------------------------

struct Relations final {
  std::set<core::ControlBindingId> bindings;
  std::set<core::ControlDomainId> domains;
  std::set<core::RuntimeUnitId> runtime_units;
  std::set<core::CapabilityProviderBindingId> providers;
};

[[nodiscard]] Relations RelationsOf(const core::RetainedBundleDTO &bundle,
                                    const core::ControllableComponentDTO &component) {
  const auto &graph = bundle.cell_graph;
  Relations relations;
  relations.runtime_units.insert(component.runtime_unit_ids.begin(),
                                 component.runtime_unit_ids.end());
  std::set<core::ProviderInstanceId> cited_instances;
  for (const auto &binding : graph.control_bindings) {
    if (binding.controllable_component_id != component.controllable_component_id) continue;
    relations.bindings.insert(binding.control_binding_id);
    relations.domains.insert(binding.control_domain_id);
    relations.runtime_units.insert(binding.provider_runtime_unit_id);
    cited_instances.insert(binding.provider_instance_id);
  }
  // The domains it provides (a generic I/O provider) are its own too.
  for (const auto &domain : graph.control_domains)
    if (const auto *provider = domain.provider();
        provider != nullptr &&
        provider->controllable_component_id == component.controllable_component_id)
      relations.domains.insert(domain.control_domain_id);
  for (const auto &domain : graph.control_domains)
    if (relations.domains.contains(domain.control_domain_id))
      relations.runtime_units.insert(domain.runtime_unit_ids.begin(), domain.runtime_unit_ids.end());
  // A component depends on every runtime unit its units depend on.
  for (bool grew = true; grew;) {
    grew = false;
    for (const auto &unit : graph.runtime_units) {
      if (!relations.runtime_units.contains(unit.runtime_unit_id)) continue;
      for (const auto &dependency : unit.dependency_runtime_unit_ids)
        grew = relations.runtime_units.insert(dependency).second || grew;
    }
  }
  for (const auto &provider : bundle.capability_provider_bindings) {
    const bool cited = std::visit(
        [&](const auto &target) {
          using Target = std::decay_t<decltype(target)>;
          if constexpr (std::is_same_v<Target, core::CellGlobalCapabilityProviderTargetDTO>) {
            return cited_instances.contains(provider.provider_instance_id);
          } else if constexpr (std::is_same_v<Target,
                                              core::ComponentTargetCapabilityProviderTargetDTO>) {
            return target.controllable_component_id == component.controllable_component_id;
          } else if constexpr (std::is_same_v<Target,
                                              core::TargetBindingCapabilityProviderTargetDTO>) {
            return relations.bindings.contains(target.control_binding_id);
          } else {
            static_assert(std::is_same_v<Target, core::ControlDomainCapabilityProviderTargetDTO>);
            return relations.domains.contains(target.control_domain_id);
          }
        },
        provider.target);
    if (cited) relations.providers.insert(provider.capability_provider_binding_id);
  }
  return relations;
}

// -- Execution host rows -----------------------------------------------------

[[nodiscard]] bool IsHost(const CanonicalValue &unit) {
  const auto *profile = StringMember(unit, "runtime_profile_id");
  return profile != nullptr && *profile == host_contract::kRuntimeProfileId;
}

/// The rows of one table, of every execution host, whose component is `id`.
[[nodiscard]] std::vector<CanonicalValue> HostRows(const CanonicalValue &graph,
                                                   std::string_view table,
                                                   std::string_view id) {
  std::vector<CanonicalValue> rows;
  for (const auto &unit : MemberArray(graph, "runtime_units")) {
    if (!IsHost(unit)) continue;
    for (const auto &row : MemberArray(MemberObject(unit, "configuration"), table)) {
      const auto *component = StringMember(row, "controllable_component_id");
      if (component != nullptr && *component == id) rows.push_back(row);
    }
  }
  return rows;
}

/// CONTRACTS §16: a member's owner, from its controller profile or I/O
/// device row (both name the owner and the member's role).
struct Membership final {
  std::string owner;
  std::string role;
};
[[nodiscard]] std::optional<Membership> MembershipOf(const CanonicalValue &graph,
                                                     std::string_view id) {
  for (const std::string_view table : {"controller_profiles", "io_provider_devices"})
    for (const auto &row : HostRows(graph, table, id)) {
      const auto *owner = StringMember(row, "owner_component_id");
      const auto *role = StringMember(row, "member_role");
      if (owner != nullptr && role != nullptr) return Membership{*owner, *role};
    }
  return std::nullopt;
}

/// The owner's arm member (the arm whose description its process launches).
[[nodiscard]] std::optional<std::string> ArmMemberOf(const core::RetainedBundleDTO &bundle,
                                                     const CanonicalValue &graph,
                                                     std::string_view owner) {
  for (const auto &component : bundle.cell_graph.controllable_components) {
    const auto membership = MembershipOf(graph, component.controllable_component_id.value());
    if (membership && membership->owner == owner && membership->role == "arm")
      return component.controllable_component_id.value();
  }
  return std::nullopt;
}

/// Whether a component runs no process of its own: no motion
/// target, and every gripper row is no-output (pneumatic or output none).
[[nodiscard]] bool RunsNoProcess(const CanonicalValue &graph, std::string_view id) {
  if (!HostRows(graph, "motion_target_limits", id).empty()) return false;
  const auto grippers = HostRows(graph, "gripper_target_configurations", id);
  return !grippers.empty() &&
         std::all_of(grippers.begin(), grippers.end(), [](const auto &row) {
           const auto *mode = StringMember(row, "output_mode");
           const auto *pneumatic = row.find_object_member("pneumatic_io");
           return (mode != nullptr && *mode == "none") ||
                  (pneumatic != nullptr && !pneumatic->is_null());
         });
}

// -- Sections ----------------------------------------------------------------

[[nodiscard]] CanonicalValue ModelRow(const CanonicalValue &graph, const std::string &model,
                                      bool with_custom_tcps) {
  for (const auto &row : MemberArray(graph, "model_instances")) {
    const auto *id = StringMember(row, "model_instance_id");
    if (id == nullptr || *id != model) continue;
    return with_custom_tcps ? Without(row, {"root_pose_in_cell"})
                            : Without(row, {"root_pose_in_cell", "custom_tcps"});
  }
  Fail("model instance " + model + " is absent");
}

[[nodiscard]] CanonicalValue Description(const RetainedBundle &bundle,
                                         const core::ControllableComponentDTO &component,
                                         bool with_arm_member) {
  const auto &graph = MemberObject(bundle.normalized_root(), "cell_graph");
  const auto &id = component.controllable_component_id.value();
  CanonicalValue row(nullptr);
  for (const auto &candidate : MemberArray(graph, "controllable_components")) {
    const auto *candidate_id = StringMember(candidate, "controllable_component_id");
    if (candidate_id != nullptr && *candidate_id == id)
      row = Without(candidate, {"root_pose_in_cell", "default_tcp"});
  }
  Array models;
  for (const auto &role : component.model_role_bindings)
    models.push_back(ModelRow(graph, role.model_instance_id.value(), true));
  // Each mount is a fixed joint of its launched description: the child's root
  // link (from its model) at the attachment pose. A child that runs no process
  // of its own has its fixed links and offset frames published there too.
  Array mounts;
  for (const auto &attachment : MemberArray(graph, "component_attachments")) {
    const auto *parent = attachment.find_object_member("parent");
    const auto *parent_id = parent ? StringMember(*parent, "controllable_component_id") : nullptr;
    if (parent_id == nullptr || *parent_id != id) continue;
    const auto *child = attachment.find_object_member("child");
    const auto *child_id = child ? StringMember(*child, "controllable_component_id") : nullptr;
    if (child_id == nullptr) Fail("attachment lacks its child");
    const auto *child_component =
        FindComponent(bundle.value(), core::ControllableComponentId(*child_id));
    Array child_models;
    if (child_component != nullptr) {
      const bool process_less = RunsNoProcess(graph, *child_id);
      for (const auto &role : child_component->model_role_bindings)
        child_models.push_back(ModelRow(graph, role.model_instance_id.value(), process_less));
    }
    mounts.emplace_back(Object{
        {"attachment", Without(attachment, {"component_attachment_revision",
                                            "collision_exclusions"})},
        {"child_models", CanonicalValue(std::move(child_models))},
        {"child_runs_no_process", CanonicalValue(RunsNoProcess(graph, *child_id))},
    });
  }
  Object description{
      {"cell_id", CanonicalValue(bundle.value().cell_graph.cell_id.value())},
      {"component", std::move(row)},
      {"models", CanonicalValue(std::move(models))},
      {"mounts", CanonicalValue(std::move(mounts))},
  };
  // CONTRACTS §16: a machine owner's process launches its arm member's
  // description (the arm's models, custom TCPs and the mounts on it).
  if (with_arm_member) {
    if (const auto arm = ArmMemberOf(bundle.value(), graph, id)) {
      const auto *member = FindComponent(bundle.value(), core::ControllableComponentId(*arm));
      if (member == nullptr) Fail("machine " + id + " lost its arm member");
      description.emplace_back("arm", Description(bundle, *member, false));
    }
  }
  return CanonicalValue(std::move(description));
}

struct HostSections final {
  Object launch;
  Object adapter;
};

/// The component's share of one execution host's configuration, split by tier.
[[nodiscard]] HostSections HostSectionsOf(const RetainedBundle &bundle,
                                          const core::RuntimeUnitId &unit,
                                          const core::ControllableComponentId &id) {
  const auto projection = host_contract::component_configuration_projection(bundle, unit, id);
  const auto *limits = projection.find_object_member("motion_target_limits");
  const bool motion = limits != nullptr && limits->is_array() && !limits->as_array().empty();
  HostSections sections;
  for (const auto &[name, value] : projection.as_object()) {
    const bool rows = value.is_array();
    if (Contains(kHostLaunchRows, name)) {
      if (rows && !value.as_array().empty()) sections.launch.emplace_back(name, value);
    } else if (Contains(kHostAdapterRows, name)) {
      if (rows && !value.as_array().empty()) sections.adapter.emplace_back(name, value);
    } else if (name == kHostInitialPositions) {
      if (rows && !value.as_array().empty()) sections.launch.emplace_back(name, value);
    } else if (Contains(kHostKernelScalars, name)) {
      if (motion) sections.launch.emplace_back(name, value);
    }
  }
  auto overrides = construction_stream_overrides(bundle, id);
  if (!overrides.as_array().empty())
    sections.adapter.emplace_back("stream_overrides", std::move(overrides));
  return sections;
}

/// Whether the one ROS host `signals` run on is simulated: no component it
/// hosts has measured state authority. Absent without signals or when they
/// span hosts (no simulated PLC then).
[[nodiscard]] std::optional<bool>
IoHostSimulated(const core::CellGraphDTO &graph,
                const std::vector<RetainedIoSignalConfiguration> &signals) {
  if (signals.empty()) return std::nullopt;
  const auto &unit_id = signals.front().runtime_unit_id;
  if (std::any_of(signals.begin(), signals.end(),
                  [&](const auto &row) { return row.runtime_unit_id != unit_id; }))
    return std::nullopt;
  const auto unit = std::find_if(graph.runtime_units.begin(), graph.runtime_units.end(),
                                 [&](const auto &row) { return row.runtime_unit_id == unit_id; });
  return unit != graph.runtime_units.end() &&
         unit->runtime_profile_id.value() == host_contract::kRuntimeProfileId &&
         std::none_of(graph.control_bindings.begin(), graph.control_bindings.end(),
                      [&](const auto &binding) {
                        return binding.provider_runtime_unit_id == unit_id &&
                               binding.state_authority_class ==
                                   core::StateAuthorityClass::kMeasured;
                      });
}

/// The component's share of one execution host's configuration, typed: the
/// kernel scalars only on a motion component, never the planner settings
/// (CellContext); as HostSectionsOf hashes them.
[[nodiscard]] host_contract::Configuration
TypedHostShare(const RetainedBundle &bundle, const core::RuntimeUnitId &unit,
               const core::ControllableComponentId &id) {
  auto share = host_contract::require_component_configuration(bundle, unit, id);
  share.planner_rng_seed = 0U;
  share.collision_clearance_m.reset();
  share.collision_check_step_rad.reset();
  share.collision_check_fresh_plans.reset();
  if (share.motion_target_limits.empty()) {
    const host_contract::Configuration none;
    share.execution_tick_hz = none.execution_tick_hz;
    share.collision_monitor_period_ms = none.collision_monitor_period_ms;
    share.runtime_collision_immediate_query_budget_ms.reset();
    share.runtime_collision_lookahead_query_budget_ms.reset();
    share.runtime_collision_lookahead_cycle_budget_ms.reset();
    share.runtime_collision_lookahead_s.reset();
    share.runtime_collision_lookahead_max_samples.reset();
    share.runtime_collision_lookahead_workers.reset();
    share.runtime_dynamic_body_max_age_cap_ms.reset();
    share.validation_collision_workers.reset();
  }
  return share;
}

[[nodiscard]] core::ModelInstanceDTO TypedModel(const core::CellGraphDTO &graph,
                                                const core::ModelInstanceId &id,
                                                bool with_custom_tcps) {
  const auto found = std::find_if(graph.model_instances.begin(), graph.model_instances.end(),
                                  [&](const auto &row) { return row.model_instance_id == id; });
  if (found == graph.model_instances.end()) Fail("model instance " + id.value() + " is absent");
  auto model = *found;
  model.root_pose_in_cell.reset();
  if (!with_custom_tcps) model.custom_tcps.clear();
  return model;
}

[[nodiscard]] CanonicalValue Address(const CanonicalValue &graph, std::string_view id,
                                     const SiteFacts &site_facts) {
  std::set<std::string> references;
  for (const std::string_view table : {"controller_profiles", "machines", "io_provider_devices"})
    for (const auto &row : HostRows(graph, table, id))
      if (const auto *reference = StringMember(row, "device_address_ref"))
        references.insert(*reference);
  if (references.empty()) return CanonicalValue(nullptr);
  Object addresses;
  for (const auto &reference : references) {
    const auto found = site_facts.device_addresses.find(reference);
    addresses.emplace_back(reference, found == site_facts.device_addresses.end()
                                          ? CanonicalValue(nullptr)
                                          : CanonicalValue(found->second));
  }
  return CanonicalValue(std::move(addresses));
}

[[nodiscard]] CanonicalValue HostScalars(const CanonicalValue &graph,
                                         const decltype(kHostPlannerScalars) &names) {
  Object hosts;
  for (const auto &unit : MemberArray(graph, "runtime_units")) {
    if (!IsHost(unit)) continue;
    const auto &configuration = MemberObject(unit, "configuration");
    Object values;
    for (const auto name : names)
      if (const auto *value = configuration.find_object_member(name))
        values.emplace_back(std::string(name), *value);
    hosts.emplace_back(*StringMember(unit, "runtime_unit_id"), CanonicalValue(std::move(values)));
  }
  return CanonicalValue(std::move(hosts));
}

} // namespace

ComponentDescriptionFacts component_description_facts(const core::CellGraphDTO &typed,
                                                      const CanonicalValue &graph,
                                                      const core::ControllableComponentId &id) {
  const auto find = [&](const core::ControllableComponentId &wanted) {
    const auto found = std::find_if(
        typed.controllable_components.begin(), typed.controllable_components.end(),
        [&](const auto &row) { return row.controllable_component_id == wanted; });
    return found == typed.controllable_components.end() ? nullptr : &*found;
  };
  const auto *component = find(id);
  if (component == nullptr) Fail("the cell has no component " + id.value());
  ComponentDescriptionFacts facts;
  for (const auto &role : component->model_role_bindings)
    facts.models.push_back(TypedModel(typed, role.model_instance_id, true));
  for (const auto &attachment : typed.component_attachments) {
    if (attachment.parent.controllable_component_id != id) continue;
    const auto &child = attachment.child.controllable_component_id;
    ComponentSpecMount mount{.child = child,
                             .child_models = {},
                             .child_runs_no_process = RunsNoProcess(graph, child.value())};
    if (const auto *child_component = find(child))
      for (const auto &role : child_component->model_role_bindings)
        mount.child_models.push_back(
            TypedModel(typed, role.model_instance_id, mount.child_runs_no_process));
    facts.mounts.push_back(std::move(mount));
  }
  return facts;
}

bool execution_host_field_has_tier(const std::string_view name) noexcept {
  return Contains(kHostLaunchRows, name) || Contains(kHostAdapterRows, name) ||
         name == kHostInitialPositions || Contains(kHostKernelScalars, name) ||
         Contains(kHostPlannerScalars, name);
}

const CanonicalValue &ComponentSpec::section(const std::string_view name) const {
  if (name == kComponentSpecAdapter) return adapter;
  if (name == kComponentSpecAddress) return address;
  if (name == kComponentSpecBindings) return bindings;
  if (name == kComponentSpecControllers) return controllers;
  if (name == kComponentSpecDescription) return description;
  if (name == kComponentSpecLaunch) return launch;
  if (name == kComponentSpecOwner) return owner;
  if (name == kComponentSpecProviders) return providers;
  Fail("unknown section " + std::string(name));
}

CanonicalValue ComponentSpec::value() const {
  Object value{{"controllable_component_id", CanonicalValue(controllable_component_id.value())}};
  for (const auto name : kComponentSpecSections)
    value.emplace_back(std::string(name), section(name));
  return CanonicalValue(std::move(value));
}

CanonicalValue CellContext::value() const {
  return CanonicalValue(Object{
      {"assistant", assistant},       {"default_tcps", default_tcps},
      {"planner", planner},           {"policies", policies},
      {"pose_sources", pose_sources}, {"rules", rules},
      {"scene", scene},               {"takeover_budgets", takeover_budgets},
  });
}

namespace {

/// One component's spec; `with_owner` false leaves its owner section null
/// (an owner hashing its arm member's sections, which never include the
/// owner's own fingerprint).
[[nodiscard]] ComponentSpec
BuildSpec(const RetainedBundle &bundle, const SiteFacts &site_facts,
          const core::ControllableComponentId &controllable_component_id, bool with_owner) {
  const auto *component = FindComponent(bundle.value(), controllable_component_id);
  if (component == nullptr)
    Fail("the bundle has no component " + controllable_component_id.value());
  const auto &root = bundle.normalized_root();
  const auto &graph = MemberObject(root, "cell_graph");
  const auto &products = MemberObject(root, "compiled_products");
  const auto relations = RelationsOf(bundle.value(), *component);
  const auto domains = Strings(relations.domains);
  const auto &id = controllable_component_id.value();

  Array units;
  Object launch;
  Object adapter;
  std::set<std::string> requirement_units;
  for (const auto &unit : MemberArray(graph, "runtime_units")) {
    const auto *unit_id = StringMember(unit, "runtime_unit_id");
    if (unit_id == nullptr) Fail("runtime unit lacks its id");
    if (!relations.runtime_units.contains(core::RuntimeUnitId(*unit_id))) continue;
    if (!IsHost(unit)) {
      units.push_back(unit);
      requirement_units.insert(*unit_id);
      continue;
    }
    // The shared host: its own row and this component's share of its
    // configuration. Its runtime-contract requirement is the union of every
    // hosted component's, already bound by each one's bindings and providers.
    units.push_back(Without(unit, {"configuration"}));
    auto host = HostSectionsOf(bundle, core::RuntimeUnitId(*unit_id), controllable_component_id);
    launch.emplace_back(*unit_id, CanonicalValue(std::move(host.launch)));
    adapter.emplace_back(*unit_id, CanonicalValue(std::move(host.adapter)));
  }
  const auto unit_ids = Strings(relations.runtime_units);

  ComponentSpec spec{.controllable_component_id = controllable_component_id};
  spec.description = Description(bundle, *component, true);
  spec.controllers = CanonicalValue(Object{
      {"control_domains", Select(MemberArray(graph, "control_domains"), "control_domain_id", domains)},
      {"desired_control_configurations",
       Select(MemberArray(root, "desired_control_configurations"), "control_domain_id", domains)},
      {"control_domain_products",
       Select(MemberArray(products, "control_domain_products"), "control_domain_id", domains)},
  });
  spec.launch = CanonicalValue(Object{
      {"runtime_units", CanonicalValue(std::move(units))},
      {"runtime_unit_products",
       Select(MemberArray(products, "runtime_unit_products"), "runtime_unit_id", unit_ids)},
      {"runtime_contract_requirements",
       Select(MemberArray(root, "runtime_contract_requirements"), "runtime_unit_id",
              requirement_units)},
      {"host", CanonicalValue(std::move(launch))},
  });
  // CONTRACTS §16: a machine owner's process is launched with its arm
  // member (its controllers, host rows, adapter and bindings): the host
  // rebuilds the owner with that arm, so a change to it replaces the owner.
  // The arm's description is already in the owner's description.
  if (const auto arm = ArmMemberOf(bundle.value(), graph, id)) {
    const auto member =
        BuildSpec(bundle, site_facts, core::ControllableComponentId(*arm), false);
    auto launch_rows = spec.launch.as_object();
    launch_rows.emplace_back("arm", CanonicalValue(Object{
                                        {"adapter", member.adapter},
                                        {"address", member.address},
                                        {"bindings", member.bindings},
                                        {"controllers", member.controllers},
                                        {"launch", member.launch},
                                        {"providers", member.providers},
                                    }));
    spec.launch = CanonicalValue(std::move(launch_rows));
  }
  spec.adapter = CanonicalValue(std::move(adapter));
  spec.bindings = Select(MemberArray(graph, "control_bindings"), "control_binding_id",
                         Strings(relations.bindings));
  spec.providers = Select(MemberArray(root, "capability_provider_bindings"),
                          "capability_provider_binding_id", Strings(relations.providers));
  spec.address = Address(graph, id, site_facts);
  spec.owner = CanonicalValue(nullptr);
  if (const auto membership = with_owner ? MembershipOf(graph, id) : std::nullopt) {
    const core::ControllableComponentId owner(membership->owner);
    if (FindComponent(bundle.value(), owner) == nullptr) Fail("machine member " + id + " lost its owner");
    spec.owner = CanonicalValue(
        component_spec_fingerprint(component_spec(bundle, site_facts, owner)).value());
  }
  spec.control_domains.assign(relations.domains.begin(), relations.domains.end());

  // Typed facts, from exactly the rows the sections hash.
  const auto &typed = bundle.value().cell_graph;
  spec.cell_id = typed.cell_id;
  auto facts = component_description_facts(typed, graph, controllable_component_id);
  spec.models = std::move(facts.models);
  spec.mounts = std::move(facts.mounts);
  for (const auto &unit : typed.runtime_units) {
    if (!relations.runtime_units.contains(unit.runtime_unit_id) ||
        unit.runtime_profile_id.value() != host_contract::kRuntimeProfileId)
      continue;
    spec.execution_host = unit.runtime_unit_id;
    spec.host_configuration = TypedHostShare(bundle, unit.runtime_unit_id, controllable_component_id);
    spec.stream_overrides = decode_component_stream_overrides(
        construction_stream_overrides(bundle, controllable_component_id));
  }
  for (const auto &domain : typed.control_domains)
    if (const auto *provider = domain.provider();
        provider != nullptr && provider->controllable_component_id == controllable_component_id)
      spec.provider_domains.push_back(domain);
  const auto budgets = host_contract::component_composition_budgets_ns(typed);
  spec.lifecycle_command_timeout_ns = budgets.at(id);
  const auto devices = ResolveRetainedIoProviderDevices(typed);
  if (!devices.ok()) Fail("I/O devices are not qualified: " + devices.error().message);
  for (const auto &device : devices.value())
    if (device.controllable_component_id == controllable_component_id) spec.io_device = device;
  const auto signals = ResolveRetainedIoSignalCatalog(typed);
  if (!signals.ok()) Fail("I/O signals are not qualified: " + signals.error().message);
  for (const auto &signal : signals.value())
    if (signal.descriptor.signal_id.controllable_component_id == controllable_component_id)
      spec.io_signals.push_back(signal);
  spec.io_host_simulated = IoHostSimulated(typed, spec.io_signals);
  if (spec.io_host_simulated) {
    auto launch_rows = spec.launch.as_object();
    launch_rows.emplace_back("io_host_simulated", CanonicalValue(*spec.io_host_simulated));
    spec.launch = CanonicalValue(std::move(launch_rows));
  }
  if (spec.address.is_object())
    for (const auto &[reference, address] : spec.address.as_object())
      spec.device_addresses.emplace(reference, address.is_string()
                                                   ? std::optional(address.as_string())
                                                   : std::nullopt);
  spec.site_configured = site_facts.configured;
  return spec;
}

} // namespace

ComponentSpec component_spec(const RetainedBundle &bundle, const SiteFacts &site_facts,
                             const core::ControllableComponentId &controllable_component_id) {
  return BuildSpec(bundle, site_facts, controllable_component_id, true);
}

CanonicalValue construction_stream_overrides(const RetainedBundle &bundle,
                                             const core::ControllableComponentId &id) {
  const auto &units = bundle.value().cell_graph.runtime_units;
  const bool motion = std::any_of(units.begin(), units.end(), [&](const auto &unit) {
    if (unit.runtime_profile_id.value() != host_contract::kRuntimeProfileId) return false;
    const auto limits =
        host_contract::require_configuration(bundle, unit.runtime_unit_id).motion_target_limits;
    return std::any_of(limits.begin(), limits.end(), [&](const auto &row) {
      return row.controllable_component_id == id;
    });
  });
  Array own;
  if (motion)
    for (const auto &row : MemberArray(bundle.normalized_root(), "component_streams"))
      if (*StringMember(row, "controllable_component_id") == id.value() &&
          *StringMember(row, "stream_id") == host_contract::kMeasuredJointsStreamId)
        own.push_back(row);
  return CanonicalValue(std::move(own));
}

CellContext cell_context(const RetainedBundle &bundle) {
  const auto &root = bundle.normalized_root();
  const auto &graph = MemberObject(root, "cell_graph");
  const auto &products = MemberObject(root, "compiled_products");
  CellContext context;
  context.policies = CanonicalValue(Object{
      {"retained_policy_records", CanonicalValue(MemberArray(root, "retained_policy_records"))},
      {"component_streams", Optional(root, "component_streams")},
  });
  context.takeover_budgets = CanonicalValue(Object{
      {"motion_takeover_preparation_budget_s",
       Optional(root, "motion_takeover_preparation_budget_s")},
      {"motion_takeover_planning_budget_s", Optional(root, "motion_takeover_planning_budget_s")},
  });
  Array placements;
  for (const auto &component : MemberArray(graph, "controllable_components"))
    placements.emplace_back(Object{
        {"controllable_component_id", Optional(component, "controllable_component_id")},
        {"root_pose_in_cell", Optional(component, "root_pose_in_cell")},
    });
  context.scene = CanonicalValue(Object{
      {"planning_collision_model", Optional(products, "planning_collision_model")},
      {"viewer_model", Optional(products, "viewer_model")},
      {"symbol_role_maps", Optional(products, "symbol_role_maps")},
      {"model_instances", CanonicalValue(MemberArray(graph, "model_instances"))},
      {"component_placements", CanonicalValue(std::move(placements))},
      {"component_attachments", CanonicalValue(MemberArray(graph, "component_attachments"))},
  });
  context.pose_sources = Optional(root, "scene_pose_sources");
  Array default_tcps;
  for (const auto &component : MemberArray(graph, "controllable_components"))
    if (const auto *tcp = component.find_object_member("default_tcp"))
      default_tcps.emplace_back(Object{
          {"controllable_component_id", Optional(component, "controllable_component_id")},
          {"default_tcp", *tcp},
      });
  context.default_tcps = CanonicalValue(std::move(default_tcps));
  context.rules = Optional(root, "interrupt_policy_configuration");
  context.planner = HostScalars(graph, kHostPlannerScalars);
  context.assistant = CanonicalValue(Object{
      {"assistant", Optional(root, "assistant")},
      {"external_resources", Optional(root, "external_resources")},
      {"site_overlay_ref", Optional(root, "site_overlay_ref")},
  });
  return context;
}

PlannerSettings planner_settings(const CellContext &context,
                                 const core::RuntimeUnitId &runtime_unit_id) {
  const auto *host = context.planner.find_object_member(runtime_unit_id.value());
  if (host == nullptr) Fail("the cell has no execution host " + runtime_unit_id.value());
  PlannerSettings settings;
  if (const auto *seed = host->find_object_member("planner_rng_seed"))
    settings.rng_seed = static_cast<std::uint32_t>(seed->as_number());
  if (const auto *step = host->find_object_member("collision_check_step_rad"))
    settings.collision_check_step_rad = step->as_number();
  if (const auto *clearance = host->find_object_member("collision_clearance_m"))
    settings.collision_clearance_m = clearance->as_number();
  if (const auto *fresh = host->find_object_member("collision_check_fresh_plans"))
    settings.collision_check_fresh_plans = fresh->as_bool();
  if (const auto *search = host->find_object_member("planner_search"))
    settings.search = core::planner_search_from_string(search->as_string()).value();
  if (const auto *optimize = host->find_object_member("planner_optimize"))
    settings.optimize = core::planner_optimize_from_string(optimize->as_string()).value();
  if (const auto *timing = host->find_object_member("cartesian_timing"))
    settings.cartesian_timing = core::parse_cartesian_speed_mode(timing->as_string()).value();
  if (const auto *timing = host->find_object_member("motion_timing"))
    settings.motion_timing = core::parse_motion_timing(timing->as_string()).value();
  if (const auto *n = host->find_object_member("planner_n"))
    settings.n = static_cast<std::uint32_t>(n->as_number());
  if (const auto *max_nodes = host->find_object_member("planner_max_nodes"))
    settings.max_nodes = static_cast<std::uint32_t>(max_nodes->as_number());
  return settings;
}

core::ContentDigest component_spec_fingerprint(const ComponentSpec &spec) {
  return core::ContentDigest(
      content_identity::framed_sha256(kSpecIdentityDomain, kSpecApiVersion, spec.value()));
}

} // namespace manyforge::deployment
