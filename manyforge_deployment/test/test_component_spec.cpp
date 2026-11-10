// Component build spec over every example Scenario.
//
// - Every execution-host configuration field has a tier.
// - Properties: a CellContext edit leaves every ComponentSpec byte-identical;
//   a spec input edit changes that component's fingerprint and only its own
//   (and its members').
#include <gtest/gtest.h>

#include <manyforge_deployment/apply_affected_scope.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/component_spec.hpp>
#include <manyforge_deployment/deployment_applicability_codec.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <manyforge_deployment/scene_pose_source_policy_codec.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <algorithm>
#include <cstdint>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>

#include "content_identity_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

namespace core = manyforge::core;
using Array = CanonicalValue::Array;
using Object = CanonicalValue::Object;

// ===========================================================================
// Example bundles and edits
// ===========================================================================

struct Example final {
  std::string name;
  std::shared_ptr<const RetainedBundle> bundle;
};

[[nodiscard]] std::shared_ptr<const RetainedBundle> Reload(const std::string &bytes) {
  // The fixture is RFC 8785 JSON from Python; normalize it through ours.
  return detail::RetainedBundleReloadAuthority::reload(
      canonical_json(parse_strict_json_model_yaml(bytes).root()));
}

[[nodiscard]] const std::vector<Example> &Examples() {
  static const std::vector<Example> examples = [] {
    std::vector<Example> result;
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(MANYFORGE_EXAMPLE_BUNDLE_ROOT))
      if (entry.path().extension() == ".json") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const auto &file : files) {
      std::ifstream stream(file, std::ios::binary);
      std::ostringstream bytes;
      bytes << stream.rdbuf();
      result.push_back({file.stem().string(), Reload(bytes.str())});
    }
    if (result.empty()) throw std::logic_error("no example bundle was produced");
    return result;
  }();
  return examples;
}

[[nodiscard]] CanonicalValue With(const CanonicalValue &object, std::string_view name, CanonicalValue value) {
  Object fields;
  bool set = false;
  for (const auto &[member, current] : object.as_object()) {
    if (member == name) {
      fields.emplace_back(member, value);
      set = true;
    } else {
      fields.emplace_back(member, current);
    }
  }
  if (!set) fields.emplace_back(std::string(name), std::move(value));
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue WithoutMember(const CanonicalValue &object, std::string_view name) {
  Object fields;
  for (const auto &[member, current] : object.as_object())
    if (member != name) fields.emplace_back(member, current);
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] const CanonicalValue &At(const CanonicalValue &object, std::string_view name) {
  const auto *member = object.find_object_member(name);
  if (member == nullptr) throw std::logic_error("missing member " + std::string(name));
  return *member;
}

[[nodiscard]] CanonicalValue MapArray(const CanonicalValue &array,
                                      const std::function<CanonicalValue(const CanonicalValue &)> &map) {
  Array rows;
  for (const auto &row : array.as_array()) rows.push_back(map(row));
  return CanonicalValue(std::move(rows));
}

/// `root` with its cell graph edited.
[[nodiscard]] CanonicalValue EditGraph(const CanonicalValue &root,
                                       const std::function<CanonicalValue(const CanonicalValue &)> &edit) {
  return With(root, "cell_graph", edit(At(root, "cell_graph")));
}

/// `graph` with every execution host's configuration edited.
[[nodiscard]] CanonicalValue EditHosts(const CanonicalValue &graph,
                                       const std::function<CanonicalValue(const CanonicalValue &)> &edit) {
  return With(graph, "runtime_units", MapArray(At(graph, "runtime_units"), [&](const auto &unit) {
                const auto &profile = At(unit, "runtime_profile_id").as_string();
                if (profile != ros2_control_execution_runtime_contract::kRuntimeProfileId) return unit;
                return With(unit, "configuration", edit(At(unit, "configuration")));
              }));
}

[[nodiscard]] std::vector<QualifiedRuntimeProfile> HostProfiles(const RetainedBundle &bundle) {
  std::vector<QualifiedRuntimeProfile> profiles;
  for (const auto &record : bundle.value().catalogs.records) {
    const auto *profile = std::get_if<core::RuntimeProfileCatalogRecordRefDTO>(&record);
    if (profile == nullptr) continue;
    if (profile->record_id.value() != detail::kRos2ControlExecutionRuntimeProfileId)
      throw std::logic_error("example bundle uses another runtime profile");
    profiles.push_back(resolve_and_qualify_runtime_profile(
        *profile, profile->content_addressed_store_key,
        detail::ros2_control_execution_runtime_profile_document()));
  }
  return profiles;
}

/// The bundle with its normalized root edited and sealed again: a changed
/// graph gets its nested and outer revisions recomputed, then the bundle id.
[[nodiscard]] std::shared_ptr<const RetainedBundle>
Reseal(const RetainedBundle &bundle, const std::function<CanonicalValue(const CanonicalValue &)> &edit) {
  auto root = edit(bundle.normalized_root());
  if (At(root, "cell_graph") != At(bundle.normalized_root(), "cell_graph")) {
    const auto typed = decode_cell_graph_candidate(
        parse_strict_json_model_yaml(canonical_json(At(root, "cell_graph"))));
    const auto profiles = HostProfiles(bundle);
    const auto ready = prepare_cell_graph_identity(validate_intrinsic_cell_graph_candidate(typed), profiles);
    root = With(root, "cell_graph", compute_graph_revision(ready).normalized_root());
  }
  const auto identity = detail::compute_content_identity(
      root, detail::ContentIdentitySpec{.identity_domain = "retained-bundle-id",
                                        .api_version = std::string(core::kRetainedBundleApiVersion),
                                        .own_field = "retained_bundle_id"});
  root = With(root, "retained_bundle_id", CanonicalValue(identity));
  return detail::RetainedBundleReloadAuthority::reload(canonical_json(root));
}

[[nodiscard]] CanonicalValue Pose(double z) {
  return CanonicalValue(Object{
      {"orientation_xyzw", CanonicalValue(Array{CanonicalValue(0.0), CanonicalValue(0.0), CanonicalValue(0.0),
                                                CanonicalValue(1.0)})},
      {"position_m", CanonicalValue(Array{CanonicalValue(0.0), CanonicalValue(0.0), CanonicalValue(z)})},
  });
}

// ===========================================================================
// Edits by kind
// ===========================================================================

enum class Kind {
  kTakeoverBudgets,  // CellContext
  kPoseSources,      // CellContext
  kRules,            // CellContext
  kPlanner,          // CellContext
  kDefaultTcp,       // CellContext
  kBasePose,         // CellContext
  kCollisionExclusions,  // CellContext
  kKernelScalar,     // spec of the host's motion components
  kChildCustomTcp,   // spec of the mounted child (and of a process-less child's parent)
  kInitialPosition,  // spec of the component owning the first initial joint
};

[[nodiscard]] std::string_view Name(Kind kind) {
  switch (kind) {
    case Kind::kTakeoverBudgets: return "takeover budgets";
    case Kind::kPoseSources: return "pose sources";
    case Kind::kRules: return "global rules";
    case Kind::kPlanner: return "planner settings";
    case Kind::kDefaultTcp: return "default TCP";
    case Kind::kBasePose: return "base pose";
    case Kind::kCollisionExclusions: return "collision exclusions";
    case Kind::kKernelScalar: return "kernel scalar";
    case Kind::kChildCustomTcp: return "mounted child custom TCP";
    case Kind::kInitialPosition: return "initial joint position";
  }
  return "";
}

[[nodiscard]] bool IsContext(Kind kind) {
  return kind != Kind::kKernelScalar && kind != Kind::kChildCustomTcp &&
         kind != Kind::kInitialPosition;
}

constexpr std::array<Kind, 10> kKinds{
    Kind::kTakeoverBudgets, Kind::kPoseSources,         Kind::kRules,
    Kind::kPlanner,         Kind::kDefaultTcp,          Kind::kBasePose,
    Kind::kCollisionExclusions, Kind::kKernelScalar,    Kind::kChildCustomTcp,
    Kind::kInitialPosition};

/// The edited root, or nullopt when the example has nothing of this kind.
[[nodiscard]] std::optional<CanonicalValue> Edit(const CanonicalValue &root, Kind kind) {
  const auto &graph = At(root, "cell_graph");
  switch (kind) {
    case Kind::kTakeoverBudgets:
      return With(With(root, "motion_takeover_preparation_budget_s", CanonicalValue(7.0)),
                  "motion_takeover_planning_budget_s", CanonicalValue(2.0));
    case Kind::kPoseSources:
      if (root.find_object_member("scene_pose_sources") == nullptr) return std::nullopt;
      return WithoutMember(root, "scene_pose_sources");
    case Kind::kRules:
      if (root.find_object_member("interrupt_policy_configuration") == nullptr) return std::nullopt;
      return WithoutMember(root, "interrupt_policy_configuration");
    case Kind::kPlanner:
      return EditGraph(root, [](const auto &cell) {
        return EditHosts(cell, [](const auto &configuration) {
          return With(configuration, "planner_rng_seed",
                      CanonicalValue(At(configuration, "planner_rng_seed").as_number() + 1.0));
        });
      });
    case Kind::kKernelScalar:
      return EditGraph(root, [](const auto &cell) {
        return EditHosts(cell, [](const auto &configuration) {
          return With(configuration, "collision_monitor_period_ms",
                      CanonicalValue(At(configuration, "collision_monitor_period_ms").as_number() * 2.0));
        });
      });
    case Kind::kInitialPosition: {
      bool edited = false;
      auto result = EditGraph(root, [&](const auto &cell) {
        return EditHosts(cell, [&](const auto &configuration) {
          const auto &positions = At(configuration, "initial_joint_positions_rad").as_array();
          if (edited || positions.empty()) return configuration;
          edited = true;
          auto changed = positions;
          changed.front() = CanonicalValue(positions.front().as_number() + 0.01);
          return With(configuration, "initial_joint_positions_rad", CanonicalValue(std::move(changed)));
        });
      });
      if (!edited) return std::nullopt;
      return result;
    }
    case Kind::kDefaultTcp: {
      // The first component with a control binding gets (or loses) one.
      const auto &bindings = At(graph, "control_bindings").as_array();
      if (bindings.empty()) return std::nullopt;
      const auto &component_id = At(bindings.front(), "controllable_component_id").as_string();
      const auto target = At(bindings.front(), "command_target_id");
      return EditGraph(root, [&](const auto &cell) {
        return With(cell, "controllable_components",
                    MapArray(At(cell, "controllable_components"), [&](const auto &component) {
                      if (At(component, "controllable_component_id").as_string() != component_id)
                        return component;
                      if (component.find_object_member("default_tcp") != nullptr)
                        return WithoutMember(component, "default_tcp");
                      return With(component, "default_tcp",
                                  CanonicalValue(Object{{"command_target_id", target},
                                                        {"frame_role_id", CanonicalValue("parity_tcp")}}));
                    }));
      });
    }
    case Kind::kBasePose: {
      bool edited = false;
      auto result = EditGraph(root, [&](const auto &cell) {
        return With(cell, "controllable_components",
                    MapArray(At(cell, "controllable_components"), [&](const auto &component) {
                      if (component.find_object_member("root_pose_in_cell") == nullptr) return component;
                      edited = true;
                      return With(component, "root_pose_in_cell", Pose(0.125));
                    }));
      });
      if (!edited) return std::nullopt;
      return result;
    }
    case Kind::kCollisionExclusions: {
      if (At(graph, "component_attachments").as_array().empty()) return std::nullopt;
      return EditGraph(root, [](const auto &cell) {
        return With(cell, "component_attachments",
                    MapArray(At(cell, "component_attachments"), [](const auto &edge) {
                      return With(edge, "collision_exclusions",
                                  CanonicalValue(Array{CanonicalValue(Object{
                                      {"child_link", CanonicalValue("parity_child_link")},
                                      {"parent_link", CanonicalValue("parity_parent_link")}})}));
                    }));
      });
    }
    case Kind::kChildCustomTcp: {
      // Every mounted child's model occurrences get one custom TCP.
      std::set<std::string> children;
      for (const auto &edge : At(graph, "component_attachments").as_array())
        children.insert(At(At(edge, "child"), "controllable_component_id").as_string());
      std::set<std::string> models;
      for (const auto &component : At(graph, "controllable_components").as_array()) {
        if (!children.contains(At(component, "controllable_component_id").as_string())) continue;
        for (const auto &role : At(component, "model_role_bindings").as_array())
          models.insert(At(role, "model_instance_id").as_string());
      }
      if (models.empty()) return std::nullopt;
      return EditGraph(root, [&](const auto &cell) {
        return With(cell, "model_instances", MapArray(At(cell, "model_instances"), [&](const auto &model) {
                      if (!models.contains(At(model, "model_instance_id").as_string())) return model;
                      return With(model, "custom_tcps",
                                  CanonicalValue(Array{CanonicalValue(Object{
                                      {"frame_role_id", CanonicalValue("parity_tcp")},
                                      {"parent_link_name", CanonicalValue("parity_link")},
                                      {"pose_in_parent_link", Pose(0.01)}})}));
                    }));
      });
    }
  }
  return std::nullopt;
}

// ===========================================================================
// Facts about one bundle
// ===========================================================================

[[nodiscard]] std::vector<core::ControllableComponentId> Ids(const RetainedBundle &bundle) {
  std::vector<core::ControllableComponentId> ids;
  for (const auto &component : bundle.value().cell_graph.controllable_components)
    ids.push_back(component.controllable_component_id);
  return ids;
}

/// The execution hosts' whole configurations.
[[nodiscard]] std::vector<ros2_control_execution_runtime_contract::Configuration>
HostConfigurations(const RetainedBundle &bundle) {
  std::vector<ros2_control_execution_runtime_contract::Configuration> result;
  for (const auto &unit : bundle.value().cell_graph.runtime_units)
    if (unit.runtime_profile_id.value() == ros2_control_execution_runtime_contract::kRuntimeProfileId)
      result.push_back(ros2_control_execution_runtime_contract::require_configuration(bundle, unit.runtime_unit_id));
  return result;
}

[[nodiscard]] bool IsMotion(const RetainedBundle &bundle, const core::ControllableComponentId &id) {
  for (const auto &configuration : HostConfigurations(bundle))
    for (const auto &row : configuration.motion_target_limits)
      if (row.controllable_component_id == id) return true;
  return false;
}

const SiteFacts kNoSite{};

[[nodiscard]] std::map<std::string, std::string> SpecBytes(const RetainedBundle &bundle,
                                                           const SiteFacts &facts = kNoSite) {
  std::map<std::string, std::string> result;
  for (const auto &id : Ids(bundle))
    result[id.value()] = canonical_json(component_spec(bundle, facts, id).value());
  return result;
}

/// Each member and its owner (from the owner fingerprint in its spec).
[[nodiscard]] std::map<std::string, std::string> Owners(const RetainedBundle &bundle,
                                                        const SiteFacts &facts = kNoSite) {
  std::map<std::string, std::string> by_fingerprint;
  for (const auto &id : Ids(bundle))
    by_fingerprint[component_spec_fingerprint(component_spec(bundle, facts, id)).value()] = id.value();
  std::map<std::string, std::string> owners;
  for (const auto &id : Ids(bundle)) {
    const auto spec = component_spec(bundle, facts, id);
    if (!spec.owner.is_null()) owners[id.value()] = by_fingerprint.at(spec.owner.as_string());
  }
  return owners;
}

// ===========================================================================
// Tests
// ===========================================================================

TEST(ComponentSpec, EveryExampleHasASpecForEveryComponentAndAStableContext) {
  for (const auto &example : Examples()) {
    SCOPED_TRACE(example.name);
    const auto &bundle = *example.bundle;
    for (const auto &id : Ids(bundle)) {
      const auto spec = component_spec(bundle, kNoSite, id);
      EXPECT_EQ(component_spec_fingerprint(spec), component_spec_fingerprint(component_spec(bundle, kNoSite, id)));
      EXPECT_EQ(derive_component_runtime_identity(bundle, kNoSite, id), component_spec_fingerprint(spec));
    }
    EXPECT_EQ(cell_context(bundle).value(), cell_context(bundle).value());
    for (const auto &row : derive_apply_component_delta(bundle, bundle, kNoSite).components)
      EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain);
  }
}

// A cell-global provider (assistant, Scene, collision query) is in a spec
// only when one of the component's bindings cites it.
TEST(ComponentSpec, CellGlobalProvidersAreInASpecOnlyWhenCited) {
  for (const auto &example : Examples()) {
    SCOPED_TRACE(example.name);
    const auto &bundle = *example.bundle;
    for (const auto &id : Ids(bundle)) {
      const auto spec = component_spec(bundle, kNoSite, id);
      std::set<std::string> cited;
      for (const auto &binding : spec.bindings.as_array())
        cited.insert(At(binding, "provider_instance_id").as_string());
      for (const auto &provider : spec.providers.as_array())
        if (At(provider, "scope").as_string() == "cell_global") {
          EXPECT_TRUE(cited.contains(At(provider, "provider_instance_id").as_string()))
              << id.value() << " holds an uncited cell-global provider";
        }
    }
  }
}

// Property: a CellContext edit leaves every spec byte-identical and changes
// the context; a spec edit changes only the edited component's fingerprint
// and its members'.
TEST(ComponentSpec, ContextEditsLeaveEverySpecAndSpecEditsOnlyTheirComponent) {
  for (const auto &example : Examples()) {
    const auto &bundle = *example.bundle;
    const auto specs = SpecBytes(bundle);
    const auto owners = Owners(bundle);
    for (const auto kind : kKinds) {
      SCOPED_TRACE(example.name + ": " + std::string(Name(kind)));
      const auto edited_root = Edit(bundle.normalized_root(), kind);
      if (!edited_root) continue;
      const auto edited = Reseal(bundle, [&](const auto &) { return *edited_root; });
      const auto edited_specs = SpecBytes(*edited);
      if (IsContext(kind)) {
        EXPECT_EQ(edited_specs, specs);
        EXPECT_NE(cell_context(*edited).value(), cell_context(bundle).value());
        for (const auto &row : derive_apply_component_delta(bundle, *edited, kNoSite).components)
          EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain)
              << row.controllable_component_id.value();
        continue;
      }
      // The components whose own sections changed, and every member whose
      // owner changed (its only reason is its owner).
      std::set<std::string> direct;
      for (const auto &row : derive_apply_component_delta(bundle, *edited, kNoSite).components) {
        if (row.disposition != ApplyComponentDisposition::kReplace) continue;
        if (row.replacement_reasons == std::vector<std::string>{std::string(kComponentSpecOwner)}) {
          ASSERT_TRUE(owners.contains(row.controllable_component_id.value()));
          EXPECT_NE(edited_specs.at(owners.at(row.controllable_component_id.value())),
                    specs.at(owners.at(row.controllable_component_id.value())));
        } else {
          direct.insert(row.controllable_component_id.value());
        }
      }
      // A machine owner whose arm member changed is replaced with it (its
      // process is launched with the arm).
      std::set<std::string> edited_components = direct;
      for (const auto &id : direct)
        if (owners.contains(id)) edited_components.erase(owners.at(id));
      if (kind == Kind::kInitialPosition) {
        EXPECT_EQ(edited_components.size(), 1U);
      }
      if (kind == Kind::kChildCustomTcp) {
        EXPECT_FALSE(direct.empty());
      }
      if (kind == Kind::kKernelScalar) {
        for (const auto &id : edited_components) {
          EXPECT_TRUE(IsMotion(bundle, core::ControllableComponentId(id))) << id;
        }
      }
      for (const auto &[id, bytes] : specs) {
        if (!direct.contains(id) && !owners.contains(id)) {
          EXPECT_EQ(edited_specs.at(id), bytes) << id;
        }
      }
    }
  }
}

// Property: a device address is a spec input of exactly the components that
// use its reference (and their members).
TEST(ComponentSpec, AnAddressChangeChangesOnlyTheComponentsUsingIt) {
  for (const auto &example : Examples()) {
    SCOPED_TRACE(example.name);
    const auto &bundle = *example.bundle;
    for (const auto &id : Ids(bundle)) {
      const auto spec = component_spec(bundle, kNoSite, id);
      if (spec.address.is_null()) continue;
      for (const auto &[reference, value] : spec.address.as_object()) {
        EXPECT_TRUE(value.is_null());
        const SiteFacts first{{{reference, "10.0.0.1"}}};
        const SiteFacts second{{{reference, "10.0.0.2"}}};
        const auto owners = Owners(bundle, first);
        for (const auto &other : Ids(bundle)) {
          const auto before = component_spec(bundle, first, other);
          const auto after = component_spec(bundle, second, other);
          const bool uses = !before.address.is_null() &&
                            before.address.find_object_member(reference) != nullptr;
          const bool member_of_user =
              owners.contains(other.value()) &&
              !component_spec(bundle, first, core::ControllableComponentId(owners.at(other.value())))
                   .address.is_null();
          if (uses || member_of_user) {
            EXPECT_NE(component_spec_fingerprint(before), component_spec_fingerprint(after)) << other.value();
          } else {
            EXPECT_EQ(component_spec_fingerprint(before), component_spec_fingerprint(after)) << other.value();
          }
        }
      }
    }
  }
}

// Coordinator (mount exclusions): editing only an attachment's collision
// exclusion pairs restarts nothing; the Scene is reprojected.
TEST(ComponentSpec, ACollisionExclusionEditRetainsArmAndTool) {
  bool exercised = false;
  for (const auto &example : Examples()) {
    SCOPED_TRACE(example.name);
    const auto &bundle = *example.bundle;
    const auto edited_root = Edit(bundle.normalized_root(), Kind::kCollisionExclusions);
    if (!edited_root) continue;
    exercised = true;
    const auto edited = Reseal(bundle, [&](const auto &) { return *edited_root; });
    const auto delta = derive_apply_component_delta(bundle, *edited, kNoSite);
    ASSERT_TRUE(delta.valid());
    for (const auto &row : delta.components)
      EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain) << row.controllable_component_id.value();
    EXPECT_TRUE(delta.attachment_changed_components.empty());
    EXPECT_TRUE(delta.shared_scene_projection_changed);
  }
  EXPECT_TRUE(exercised);
}

// The planner settings read from the cell context are the execution host's
// configured ones (Deployment defaults where it names none).
TEST(ComponentSpec, PlannerSettingsAreTheHostConfiguration) {
  for (const auto &example : Examples()) {
    SCOPED_TRACE(example.name);
    const auto &bundle = *example.bundle;
    const auto context = cell_context(bundle);
    for (const auto &unit : bundle.value().cell_graph.runtime_units) {
      if (unit.runtime_profile_id.value() != ros2_control_execution_runtime_contract::kRuntimeProfileId)
        continue;
      const auto configuration =
          ros2_control_execution_runtime_contract::require_configuration(bundle, unit.runtime_unit_id);
      const auto settings = planner_settings(context, unit.runtime_unit_id);
      EXPECT_EQ(settings.rng_seed, configuration.planner_rng_seed);
      EXPECT_EQ(settings.collision_check_step_rad,
                configuration.collision_check_step_rad.value_or(core::kDefaultCollisionCheckStepRad));
      EXPECT_EQ(settings.collision_clearance_m,
                configuration.collision_clearance_m.value_or(core::kDefaultCollisionClearanceM));
      EXPECT_EQ(settings.collision_check_fresh_plans,
                configuration.collision_check_fresh_plans.value_or(
                    core::kDefaultCollisionCheckFreshPlans));
    }
  }
}

// `collision_check_fresh_plans` is a per-host Deployment field
// (on by default). Turning it off reaches the typed configuration and the
// planner settings, and like the clearance it is cell context: every
// component is retained and takes it at Apply.
TEST(ComponentSpec, TurningTheFreshPlanCheckOffRetainsEveryComponent) {
  EXPECT_TRUE(core::kDefaultCollisionCheckFreshPlans);
  for (const auto &example : Examples()) {
    SCOPED_TRACE(example.name);
    const auto &bundle = *example.bundle;
    const auto edited = Reseal(bundle, [](const auto &root) {
      return EditGraph(root, [](const auto &cell) {
        return EditHosts(cell, [](const auto &configuration) {
          return With(configuration, "collision_check_fresh_plans", CanonicalValue(false));
        });
      });
    });
    EXPECT_EQ(SpecBytes(*edited), SpecBytes(bundle));
    for (const auto &unit : edited->value().cell_graph.runtime_units) {
      if (unit.runtime_profile_id.value() != ros2_control_execution_runtime_contract::kRuntimeProfileId)
        continue;
      EXPECT_TRUE(planner_settings(cell_context(bundle), unit.runtime_unit_id)
                      .collision_check_fresh_plans);
      EXPECT_EQ(ros2_control_execution_runtime_contract::require_configuration(
                    *edited, unit.runtime_unit_id).collision_check_fresh_plans,
                std::optional<bool>(false));
      EXPECT_FALSE(planner_settings(cell_context(*edited), unit.runtime_unit_id)
                       .collision_check_fresh_plans);
    }
    for (const auto &row : derive_apply_component_delta(bundle, *edited, kNoSite).components)
      EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain)
          << row.controllable_component_id.value();
  }
}

// `motion_timing` is a per-host Deployment field (smooth by
// default). Setting fast passes the closed schema, reaches the planner
// settings and, like the planner block, is cell context: every component is
// retained and takes it at Apply.
TEST(ComponentSpec, ChangingMotionTimingRetainsEveryComponent) {
  EXPECT_EQ(core::kDefaultMotionTiming, core::MotionTimingDTO::kSmooth);
  for (const auto &example : Examples()) {
    SCOPED_TRACE(example.name);
    const auto &bundle = *example.bundle;
    const auto edited = Reseal(bundle, [](const auto &root) {
      return EditGraph(root, [](const auto &cell) {
        return EditHosts(cell, [](const auto &configuration) {
          return With(configuration, "motion_timing", CanonicalValue(std::string("fast")));
        });
      });
    });
    EXPECT_EQ(SpecBytes(*edited), SpecBytes(bundle));
    for (const auto &unit : edited->value().cell_graph.runtime_units) {
      if (unit.runtime_profile_id.value() != ros2_control_execution_runtime_contract::kRuntimeProfileId)
        continue;
      EXPECT_EQ(planner_settings(cell_context(bundle), unit.runtime_unit_id).motion_timing,
                core::MotionTimingDTO::kSmooth);
      EXPECT_EQ(planner_settings(cell_context(*edited), unit.runtime_unit_id).motion_timing,
                core::MotionTimingDTO::kFast);
    }
    for (const auto &row : derive_apply_component_delta(bundle, *edited, kNoSite).components)
      EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain)
          << row.controllable_component_id.value();
  }
}

// The typed facts the host builds a component from are its own rows
// only, with no placement and no planner settings, and say what the hashed
// sections say.
TEST(ComponentSpec, TypedFactsAreTheComponentsOwnHashedRows) {
  for (const auto &example : Examples()) {
    SCOPED_TRACE(example.name);
    const auto &bundle = *example.bundle;
    for (const auto &id : Ids(bundle)) {
      SCOPED_TRACE(id.value());
      const auto spec = component_spec(bundle, kNoSite, id);
      EXPECT_EQ(spec.cell_id, bundle.value().cell_graph.cell_id);
      const auto &host = spec.host_configuration;
      const auto own = [&](const auto &rows) {
        return std::all_of(rows.begin(), rows.end(),
                           [&](const auto &row) { return row.controllable_component_id == id; });
      };
      EXPECT_TRUE(own(host.controller_profiles) && own(host.machines) && own(host.sensors) &&
                  own(host.motion_target_limits) && own(host.gripper_target_configurations) &&
                  own(host.default_fault_stop_profiles) && own(host.pose_sources) &&
                  own(host.ai_models) && own(host.component_streams));
      EXPECT_EQ(host.planner_rng_seed, 0U);
      EXPECT_FALSE(host.collision_clearance_m || host.collision_check_step_rad ||
                   host.collision_check_fresh_plans);
      if (!IsMotion(bundle, id)) {
        EXPECT_EQ(host.execution_tick_hz, 0U);
      }
      for (const auto &model : spec.models) EXPECT_FALSE(model.root_pose_in_cell);
      const auto &mounts = At(spec.description, "mounts").as_array();
      ASSERT_EQ(mounts.size(), spec.mounts.size());
      for (std::size_t index = 0; index < mounts.size(); ++index)
        EXPECT_EQ(At(mounts[index], "child_runs_no_process").as_bool(),
                  spec.mounts[index].child_runs_no_process);
      for (const auto &signal : spec.io_signals)
        EXPECT_EQ(signal.descriptor.signal_id.controllable_component_id, id);
      if (spec.io_device) {
        EXPECT_EQ(spec.io_device->controllable_component_id, id);
      }
      EXPECT_EQ(spec.launch.find_object_member("io_host_simulated") != nullptr,
                spec.io_host_simulated.has_value());
      // Exactly an arm launches with operator Stop decelerations.
      for (const auto &profile : host.controller_profiles)
        EXPECT_EQ(profile.max_deceleration_on_cancel_rad_s2.empty(), !IsMotion(bundle, id));
    }
  }
}

// Every controller profile carries what the host launches the
// component with: its description (a URDF) and either an arm's joints and
// operator Stop decelerations (a motion component) or a tool's gripper joint.
TEST(ComponentSpec, ControllerProfilesCarryTheirLaunchValues) {
  for (const auto &example : Examples()) {
    SCOPED_TRACE(example.name);
    const auto &bundle = *example.bundle;
    for (const auto &configuration : HostConfigurations(bundle))
      for (const auto &profile : configuration.controller_profiles) {
        SCOPED_TRACE(profile.controllable_component_id.value());
        EXPECT_NO_THROW(
            static_cast<void>(parse_model_documents_for_projection(profile.description, std::nullopt)));
        EXPECT_NE(profile.joint_names.empty(), !profile.gripper_joint_name.has_value());
        EXPECT_EQ(!profile.joint_names.empty(), IsMotion(bundle, profile.controllable_component_id));
        // Exactly the arm rows carry the operator Stop decelerations.
        EXPECT_EQ(profile.max_deceleration_on_cancel_rad_s2.empty(), profile.joint_names.empty());
      }
  }
}

// The rows an arm's launch edit replaces: the arm (`launch`, and `owner` when
// its machine owner is replaced with it), its machine owner (the
// owner launches with its arm member, reason `launch`), and every member
// of a replaced component (`owner`); every other component is retained.
void ExpectLaunchEditReplaces(const ApplyComponentDeltaDTO &delta, const std::string &edited,
                              const std::map<std::string, std::string> &owners) {
  const auto owner_of = [&](const std::string &id) {
    const auto found = owners.find(id);
    return found == owners.end() ? std::string() : found->second;
  };
  const auto machine = owner_of(edited);
  const auto has = [](const auto &row, std::string_view reason) {
    return std::find(row.replacement_reasons.begin(), row.replacement_reasons.end(),
                     std::string(reason)) != row.replacement_reasons.end();
  };
  for (const auto &row : delta.components) {
    const auto &id = row.controllable_component_id.value();
    const auto owner = owner_of(id);
    if (id == edited) {
      EXPECT_EQ(row.disposition, ApplyComponentDisposition::kReplace);
      auto reasons = std::vector<std::string>{std::string(kComponentSpecLaunch)};
      if (!machine.empty()) reasons.emplace_back(kComponentSpecOwner);
      EXPECT_EQ(row.replacement_reasons, reasons);
    } else if (!machine.empty() && id == machine) {
      EXPECT_EQ(row.disposition, ApplyComponentDisposition::kReplace) << id;
      EXPECT_TRUE(has(row, kComponentSpecLaunch)) << id;
    } else if (!owner.empty() && (owner == edited || owner == machine)) {
      EXPECT_EQ(row.disposition, ApplyComponentDisposition::kReplace) << id;
      EXPECT_TRUE(has(row, kComponentSpecOwner)) << id;
    } else {
      EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain) << id;
    }
  }
}

// A controller profile's description is a launch input of its own
// component only (and its members', through their owner; an arm member's
// machine owner's, which launches with it).
TEST(ComponentSpec, ADescriptionChangeReplacesItsComponentForLaunch) {
  bool exercised = false;
  for (const auto &example : Examples()) {
    SCOPED_TRACE(example.name);
    const auto &bundle = *example.bundle;
    std::string edited_id;
    const auto edited = Reseal(bundle, [&](const auto &root) {
      return EditGraph(root, [&](const auto &cell) {
        return EditHosts(cell, [&](const auto &configuration) {
          const auto &profiles = At(configuration, "controller_profiles").as_array();
          if (!edited_id.empty() || profiles.empty()) return configuration;
          edited_id = At(profiles.front(), "controllable_component_id").as_string();
          auto rows = profiles;
          rows.front() = With(rows.front(), "description",
                              CanonicalValue(At(rows.front(), "description").as_string() + "\n"));
          return With(configuration, "controller_profiles", CanonicalValue(std::move(rows)));
        });
      });
    });
    if (edited_id.empty()) continue;
    exercised = true;
    ExpectLaunchEditReplaces(derive_apply_component_delta(bundle, *edited, kNoSite), edited_id,
                             Owners(bundle));
  }
  EXPECT_TRUE(exercised);
}

// An arm's operator Stop decelerations are a launch input of that arm
// (and of its machine owner, which launches with it).
// Perturbing one joint's value replaces that arm, with reason `launch`, and the host reads the new value from its spec. (A Deployment edit
// also moves the arm's control-domain product, reason `controllers`; the
// example compile checks that the row and the product's Stop row agree.)
TEST(ComponentSpec, AStopDecelerationEditReplacesItsArmAndMachine) {
  bool exercised = false;
  for (const auto &example : Examples()) {
    SCOPED_TRACE(example.name);
    const auto &bundle = *example.bundle;
    std::optional<std::string> arm;
    std::string joint;
    double value = 0.0;
    const auto edited = Reseal(bundle, [&](const auto &root) {
      return EditGraph(root, [&](const auto &cell) {
        return EditHosts(cell, [&](const auto &configuration) {
          return With(configuration, "controller_profiles",
                      MapArray(At(configuration, "controller_profiles"), [&](const auto &profile) {
                        const auto *cancel =
                            profile.find_object_member("max_deceleration_on_cancel_rad_s2");
                        if (arm || cancel == nullptr) return profile;
                        arm = At(profile, "controllable_component_id").as_string();
                        auto rows = cancel->as_array();
                        joint = At(rows.front(), "source_joint_name").as_string();
                        value = At(rows.front(), "max_deceleration_rad_s2").as_number() + 1.0;
                        rows.front() = With(rows.front(), "max_deceleration_rad_s2", CanonicalValue(value));
                        return With(profile, "max_deceleration_on_cancel_rad_s2",
                                    CanonicalValue(std::move(rows)));
                      }));
        });
      });
    });
    if (!arm) continue;
    exercised = true;
    const core::ControllableComponentId arm_id(*arm);
    const auto before = component_spec(bundle, kNoSite, arm_id);
    const auto after = component_spec(*edited, kNoSite, arm_id);
    const auto *old_profile = ros2_control_execution_runtime_contract::find_controller_profile(
        before.host_configuration, arm_id);
    const auto *new_profile = ros2_control_execution_runtime_contract::find_controller_profile(
        after.host_configuration, arm_id);
    ASSERT_TRUE(old_profile != nullptr && new_profile != nullptr);
    auto expected = old_profile->max_deceleration_on_cancel_rad_s2;
    expected.at(joint) = value;
    EXPECT_EQ(new_profile->max_deceleration_on_cancel_rad_s2, expected);
    EXPECT_NE(canonical_json(before.launch), canonical_json(after.launch));
    ExpectLaunchEditReplaces(derive_apply_component_delta(bundle, *edited, kNoSite), *arm,
                             Owners(bundle));
  }
  EXPECT_TRUE(exercised);
}

// Every field of the execution host's configuration schema has a tier: a
// new field fails here until it is given one.
TEST(ComponentSpec, EveryExecutionHostFieldHasATier) {
  std::ifstream stream(MANYFORGE_HOST_CONFIGURATION_SCHEMA);
  ASSERT_TRUE(stream);
  // The root definition (the first) lists its fields at this indentation.
  const std::string prefix = "        - field_id: ";
  std::size_t fields = 0;
  std::size_t definitions = 0;
  for (std::string line; std::getline(stream, line);) {
    if (line.starts_with("  - definition_id:") && ++definitions > 1) break;
    if (!line.starts_with(prefix)) continue;
    auto name = line.substr(prefix.size());
    std::erase(name, '"');
    ++fields;
    EXPECT_TRUE(execution_host_field_has_tier(name)) << name;
  }
  EXPECT_GT(fields, 20U);
}

// Only an override a component is constructed with is spec data
// (construction_stream_overrides). An I/O provider re-reads its input-stream
// overrides on retain, so their edit stays CellContext: every component is
// retained and nothing is fenced.
TEST(ComponentSpec, IoProviderStreamOverrideEditRetainsEveryComponent) {
  std::size_t providers = 0U;
  for (const auto &example : Examples()) {
    SCOPED_TRACE(example.name);
    const auto &bundle = *example.bundle;
    for (const auto &id : Ids(bundle)) {
      if (!component_spec(bundle, kNoSite, id).io_device) continue;
      SCOPED_TRACE(id.value());
      ++providers;
      const auto with_override = [&](const RetainedBundle &source, const std::int64_t budget_ns) {
        return Reseal(source, [&](const auto &root) {
          return With(root, "component_streams",
                      encode_component_stream_overrides({core::StreamParameterOverrideDTO{
                          id, "inputs", budget_ns, std::nullopt, std::nullopt}}));
        });
      };
      const auto before = with_override(bundle, 500'000'000);
      const auto after = with_override(*before, 2'000'000'000);
      EXPECT_TRUE(construction_stream_overrides(*after, id).as_array().empty());
      EXPECT_NE(cell_context(*before).value(), cell_context(*after).value());
      const auto delta = derive_apply_component_delta(*before, *after, kNoSite);
      ASSERT_TRUE(delta.valid());
      for (const auto &row : delta.components)
        EXPECT_EQ(row.disposition, ApplyComponentDisposition::kRetain)
            << row.controllable_component_id.value();
      EXPECT_TRUE(derive_apply_configuration_fence_scope(*before, *after).resources.empty());
    }
  }
  EXPECT_GT(providers, 0U);
}

} // namespace
} // namespace manyforge::deployment
