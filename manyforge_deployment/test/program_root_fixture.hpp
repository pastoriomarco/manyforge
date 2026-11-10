#pragma once

// Shared Program Canonical builders for the deployment C++ tests. Each test
// file picks its own bundle and revision; the document shape lives here once.

#include <manyforge_deployment/canonical_value.hpp>

#include <string>
#include <string_view>
#include <vector>

#include "content_identity_internal.hpp"

namespace manyforge::deployment::test {

using manyforge::deployment::CanonicalValue;
using manyforge::deployment::detail::compute_content_identity;
using manyforge::deployment::detail::ContentIdentitySpec;

inline constexpr std::string_view kProgramFixtureZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

struct ProgramOptions final {
  std::string catalog_content_version;
  bool include_legacy_robot{false};
  bool duplicate_tree_instance{false};
  bool include_contact_grasp{false};
  std::string value_source_kind;
  bool duplicate_value_source_parameter{false};
  bool include_node_policy{false};
  bool explicit_default_node_policy{false};
};

[[nodiscard]] inline CanonicalValue
text_array(const std::vector<std::string> &values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto &value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] inline CanonicalValue make_contact_grasp_policy() {
  CanonicalValue policy(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.contact_policy.v1")},
      {"policy_id", CanonicalValue("selected_object_grasp")},
      {"version", CanonicalValue(std::string(kProgramFixtureZeroDigest))},
      {"mode", CanonicalValue("contact_grasp")},
      {"sensing_mode", CanonicalValue("geometry_only")},
      {"default_unmatched_response", CanonicalValue("hard_stop")},
      {"zones",
       CanonicalValue(
           CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
               {"zone_id", CanonicalValue("tool_contact")},
               {"body_pairs",
                CanonicalValue(
                    CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                        {"body_a",
                         CanonicalValue(CanonicalValue::Object{
                             {"kind", CanonicalValue("component_model_body")},
                             {"body", CanonicalValue(CanonicalValue::Object{
                                 {"component_id", CanonicalValue("lite6_arm")},
                                 {"collision_body_id", CanonicalValue("arm_collision")}})},
                         })},
                        {"body_b",
                         CanonicalValue(CanonicalValue::Object{
                             {"kind", CanonicalValue("runtime_object")},
                             {"runtime_object_id",
                              CanonicalValue("selected_object")},
                         })},
                    })})},
               {"violation_response", CanonicalValue("hard_stop")},
               {"max_penetration_m", CanonicalValue(0.002)},
               {"terminal_window_start_fraction", CanonicalValue(0.8)},
           })})},
  });
  const std::string revision = compute_content_identity(
      policy, ContentIdentitySpec{
                  .identity_domain = "catalog-record-revision/contact_policy",
                  .api_version = "manyforge.contact_policy.v1",
                  .own_field = "version",
              });
  auto members = policy.as_object();
  for (auto &[name, value] : members) {
    if (name == "version") {
      value = CanonicalValue(revision);
      return CanonicalValue(std::move(members));
    }
  }
  throw std::logic_error("test contact policy omitted version");
}

[[nodiscard]] inline CanonicalValue make_program_root(const ProgramOptions &options) {
  CanonicalValue::Array children;
  if (options.duplicate_tree_instance) {
    children.emplace_back(CanonicalValue::Object{
        {"node_id", CanonicalValue("sequence")},
        {"instance_id", CanonicalValue("root")},
        {"parameters", CanonicalValue(CanonicalValue::Object{})},
        {"children", CanonicalValue(CanonicalValue::Array{})},
    });
  }

  CanonicalValue::Object tree_parameters;
  CanonicalValue::Object tree{
      {"node_id", CanonicalValue("sequence")},
      {"instance_id", CanonicalValue("root")},
      {"parameters", CanonicalValue(tree_parameters)},
      {"children", CanonicalValue(std::move(children))},
  };
  if (!options.value_source_kind.empty()) {
    // A timer whose duration comes from a declared Program value.
    CanonicalValue::Object source{
        {"kind", CanonicalValue(options.value_source_kind)}};
    if (options.value_source_kind == "program_parameter" ||
        options.value_source_kind == "blackboard_key") {
      source.emplace_back("reference", CanonicalValue("wait_s"));
    } else if (options.value_source_kind == "blackboard_field") {
      source.emplace_back("reference", CanonicalValue("plan"));
      source.emplace_back("field", CanonicalValue("wait_s"));
    }
    CanonicalValue::Object timer_parameters;
    if (options.duplicate_value_source_parameter) {
      timer_parameters.emplace_back("duration_s", CanonicalValue(0.5));
    }
    for (auto &[name, value] : tree) {
      if (name == "children") {
        value = CanonicalValue(CanonicalValue::Array{
            CanonicalValue(CanonicalValue::Object{
                {"node_id", CanonicalValue("timer")},
                {"instance_id", CanonicalValue("wait")},
                {"parameters", CanonicalValue(std::move(timer_parameters))},
                {"value_sources",
                 CanonicalValue(CanonicalValue::Object{
                     {"duration_s", CanonicalValue(std::move(source))}})},
                {"children", CanonicalValue(CanonicalValue::Array{})},
            })});
        break;
      }
    }
  }
  if (options.include_node_policy || options.explicit_default_node_policy) {
    const bool defaults = options.explicit_default_node_policy;
    tree.emplace_back(
        "recovery_policy",
        CanonicalValue(CanonicalValue::Object{
            {"allow_intervention", CanonicalValue(!defaults)},
            {"max_retries", CanonicalValue(defaults ? 0.0 : 2.0)},
        }));
    tree.emplace_back("operator_stop_policy",
                      CanonicalValue(defaults ? "inherit" : "hold"));
  }

  CanonicalValue::Object root{
      {"api_version", CanonicalValue("manyforge.program.v3")},
      {"program_id", CanonicalValue("contract_program")},
      {"program_revision", CanonicalValue(std::string(kProgramFixtureZeroDigest))},
      {"program_profile", CanonicalValue("composable-cell-v1")},
      {"name", CanonicalValue("Program contract fixture")},
      {"catalog",
       CanonicalValue(CanonicalValue::Object{
           {"api_version", CanonicalValue("manyforge.node_catalog.v3")},
           {"content_version", CanonicalValue(options.catalog_content_version)},
       })},
      {"tree", CanonicalValue(std::move(tree))},
  };
  if (options.include_legacy_robot) {
    root.emplace_back("robot", CanonicalValue(CanonicalValue::Object{
                                   {"robot_id", CanonicalValue("legacy_robot")},
                                   {"group_id", CanonicalValue("legacy_group")},
                               }));
  }
  if (!options.value_source_kind.empty()) {
    root.emplace_back(
        "parameters",
        CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
            {"parameter_id", CanonicalValue("wait_s")},
            {"type", CanonicalValue("float64")},
            {"default", CanonicalValue(0.5)},
        })}));
    const auto key = [](std::string name, std::string type) {
      return CanonicalValue::Object{
          {"key", CanonicalValue(std::move(name))},
          {"type", CanonicalValue(std::move(type))},
          {"access", CanonicalValue("read_write")},
          {"persistence", CanonicalValue("run")},
      };
    };
    auto plan = key("plan", "pick_planning_result");
    plan.emplace_back(
        "fields",
        CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
            {"field", CanonicalValue("wait_s")},
            {"type", CanonicalValue("float64")},
        })}));
    root.emplace_back(
        "blackboard",
        CanonicalValue(CanonicalValue::Object{
            {"namespaces", CanonicalValue(CanonicalValue::Array{})},
            {"keys", CanonicalValue(CanonicalValue::Array{
                         CanonicalValue(std::move(plan)),
                         CanonicalValue(key("wait_s", "float64"))})},
        }));
  }
  if (options.include_contact_grasp) {
    root.emplace_back(
        "blackboard",
        CanonicalValue(CanonicalValue::Object{
            {"namespaces", CanonicalValue(CanonicalValue::Array{})},
            {"keys", CanonicalValue(CanonicalValue::Array{
                         CanonicalValue(CanonicalValue::Object{
                             {"key", CanonicalValue("selected_object")},
                             {"type", CanonicalValue("scene_resource")},
                             {"access", CanonicalValue("read_write")},
                             {"persistence", CanonicalValue("run")},
                         })})},
        }));
    root.emplace_back("contact_policies", CanonicalValue(CanonicalValue::Array{
                                              make_contact_grasp_policy()}));
  }
  return CanonicalValue(std::move(root));
}

[[nodiscard]] inline CanonicalValue with_program_revision(CanonicalValue root) {
  const std::string revision =
      compute_content_identity(root, ContentIdentitySpec{
                                         .identity_domain = "program-revision",
                                         .api_version = "manyforge.program.v3",
                                         .own_field = "program_revision",
                                     });
  CanonicalValue::Object members = root.as_object();
  for (auto &[name, value] : members) {
    if (name == "program_revision") {
      value = CanonicalValue(revision);
      return CanonicalValue(std::move(members));
    }
  }
  throw std::logic_error("test Program omitted program_revision");
}


} // namespace manyforge::deployment::test
