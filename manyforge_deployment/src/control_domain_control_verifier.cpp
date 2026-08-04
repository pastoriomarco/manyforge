#include <algorithm>
#include <array>
#include <exception>
#include <initializer_list>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "content_identity_internal.hpp"
#include "control_domain_control_internal.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/cell_applicability.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

constexpr std::string_view kImplementationProfileApiVersion =
    "manyforge.control_implementation_profile.v1";
constexpr std::string_view kImplementationProfileId = "manyforge.reference_ros2_control";
constexpr std::string_view kImplementationApiVersion =
    "manyforge.control_controller_implementation.v1";
constexpr std::string_view kSourceConformanceApiVersion = "manyforge.control_source_conformance.v1";

struct ImplementationDescriptor final {
  std::string_view implementation_id;
  std::string_view implementation_type;
};

constexpr std::array<ImplementationDescriptor, 3U> kImplementations{{
    {"manyforge.ros2_control.admittance_controller", "admittance_controller/AdmittanceController"},
    {"manyforge.ros2_control.joint_trajectory_controller.chainable",
     "joint_trajectory_controller/JointTrajectoryController"},
    {"manyforge.ros2_control.joint_trajectory_controller.direct",
     "joint_trajectory_controller/JointTrajectoryController"},
}};

[[nodiscard]] const CanonicalValue::Object& require_object(const CanonicalValue& value,
                                                           std::string_view path) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail_retained_bundle(std::string(path) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array& require_array(const CanonicalValue& value,
                                                         std::string_view path) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail_retained_bundle(std::string(path) + " must be an array");
  }
  return *array;
}

[[nodiscard]] const std::string& require_string(const CanonicalValue& value,
                                                std::string_view path) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail_retained_bundle(std::string(path) + " must be a string");
  }
  return *text;
}

[[nodiscard]] bool require_bool(const CanonicalValue& value, std::string_view path) {
  const auto* result = std::get_if<bool>(&value.storage());
  if (result == nullptr) {
    fail_retained_bundle(std::string(path) + " must be a boolean");
  }
  return *result;
}

[[nodiscard]] const CanonicalValue& require_member(const CanonicalValue::Object& object,
                                                   std::string_view name, std::string_view path) {
  const auto found = std::find_if(object.begin(), object.end(),
                                  [name](const auto& member) { return member.first == name; });
  if (found == object.end()) {
    fail_retained_bundle(std::string(path) + " is missing required member " + std::string(name));
  }
  return found->second;
}

void require_exact_fields(const CanonicalValue::Object& object,
                          std::initializer_list<std::string_view> fields, std::string_view path) {
  if (object.size() != fields.size()) {
    fail_retained_bundle(std::string(path) + " has an incorrect closed field count");
  }
  for (const auto& [name, unused] : object) {
    (void)unused;
    if (std::find(fields.begin(), fields.end(), name) == fields.end()) {
      fail_retained_bundle(std::string(path) + " contains unknown member " + name);
    }
  }
  for (const auto field : fields) {
    static_cast<void>(require_member(object, field, path));
  }
}

[[nodiscard]] const std::string& require_identity_member(const CanonicalValue::Object& object,
                                                         std::string_view name,
                                                         std::string_view path) {
  const std::string& value = require_string(require_member(object, name, path),
                                            std::string(path) + "." + std::string(name));
  require_retained_portable_identity(value, std::string(path) + "." + std::string(name));
  return value;
}

[[nodiscard]] const std::string& require_digest_member(const CanonicalValue::Object& object,
                                                       std::string_view name,
                                                       std::string_view path) {
  const std::string& value = require_string(require_member(object, name, path),
                                            std::string(path) + "." + std::string(name));
  require_retained_digest(value, std::string(path) + "." + std::string(name));
  return value;
}

[[nodiscard]] std::string implementation_revision(const ImplementationDescriptor& descriptor) {
  const CanonicalValue root(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kImplementationApiVersion))},
      {"implementation_id", CanonicalValue(std::string(descriptor.implementation_id))},
      {"implementation_revision",
       CanonicalValue("sha256:"
                      "0000000000000000000000000000000000000000000000000000000000000000")},
      {"implementation_type", CanonicalValue(std::string(descriptor.implementation_type))},
  });
  return compute_content_identity(
      root, ContentIdentitySpec{
                .identity_domain = "control-controller-implementation-revision",
                .api_version = std::string(kImplementationApiVersion),
                .own_field = "implementation_revision",
            });
}

[[nodiscard]] CanonicalValue implementation_projection(const ImplementationDescriptor& descriptor) {
  return CanonicalValue(CanonicalValue::Object{
      {"implementation_id", CanonicalValue(std::string(descriptor.implementation_id))},
      {"implementation_revision", CanonicalValue(implementation_revision(descriptor))},
      {"implementation_type", CanonicalValue(std::string(descriptor.implementation_type))},
  });
}

[[nodiscard]] std::string implementation_profile_revision() {
  CanonicalValue::Array implementations;
  implementations.reserve(kImplementations.size());
  for (const auto& implementation : kImplementations) {
    implementations.push_back(implementation_projection(implementation));
  }
  const CanonicalValue root(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kImplementationProfileApiVersion))},
      {"implementation_profile_id", CanonicalValue(std::string(kImplementationProfileId))},
      {"implementation_profile_revision",
       CanonicalValue("sha256:"
                      "0000000000000000000000000000000000000000000000000000000000000000")},
      {"implementations", CanonicalValue(std::move(implementations))},
  });
  return compute_content_identity(root,
                                  ContentIdentitySpec{
                                      .identity_domain = "control-implementation-profile-revision",
                                      .api_version = std::string(kImplementationProfileApiVersion),
                                      .own_field = "implementation_profile_revision",
                                  });
}

[[nodiscard]] std::string expected_controller_manager_name(std::string_view domain_id,
                                                           std::string_view domain_revision,
                                                           std::string_view topology_id,
                                                           std::string_view topology_revision) {
  std::string preimage = "manyforge.control-domain-manager-name.v1";
  preimage.push_back('\0');
  preimage.append(domain_id);
  preimage.push_back('\0');
  preimage.append(domain_revision);
  preimage.push_back('\0');
  preimage.append(topology_id);
  preimage.push_back('\0');
  preimage.append(topology_revision);
  const std::string digest = sha256_digest(preimage);
  return "mf1_controller_manager_" + digest.substr(7U, 24U);
}

struct ClaimKey final {
  std::string resource_role_id;
  std::string resource_id;
  std::string interface_name;
  std::string logical_interface_symbol;

  friend bool operator<(const ClaimKey& left, const ClaimKey& right) {
    return std::tie(left.resource_role_id, left.resource_id, left.interface_name,
                    left.logical_interface_symbol) <
           std::tie(right.resource_role_id, right.resource_id, right.interface_name,
                    right.logical_interface_symbol);
  }
  friend bool operator==(const ClaimKey&, const ClaimKey&) = default;
};

[[nodiscard]] std::vector<ClaimKey> verify_claims(const CanonicalValue& value,
                                                  std::string_view path,
                                                  bool require_non_empty = false) {
  const auto& rows = require_array(value, path);
  if (require_non_empty && rows.empty()) {
    fail_retained_bundle(std::string(path) + " must be non-empty");
  }
  if (rows.size() > kMaxCellExpandedControlInterfaceClaimsPerDirection) {
    fail_retained_bundle(std::string(path) + " exceeds the expanded-claim bound");
  }
  std::vector<ClaimKey> result;
  result.reserve(rows.size());
  std::set<ClaimKey> unique;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string item_path = std::string(path) + "[" + std::to_string(index) + "]";
    const auto& row = require_object(rows[index], item_path);
    require_exact_fields(
        row, {"resource_role_id", "resource_id", "interface_name", "logical_interface_symbol"},
        item_path);
    ClaimKey claim{
        require_identity_member(row, "resource_role_id", item_path),
        require_identity_member(row, "resource_id", item_path),
        require_identity_member(row, "interface_name", item_path),
        require_identity_member(row, "logical_interface_symbol", item_path),
    };
    if (!unique.insert(claim).second) {
      fail_retained_bundle(item_path + " duplicates an interface claim");
    }
    result.push_back(std::move(claim));
  }
  return result;
}

struct BindingFacts final {
  std::string revision;
  std::vector<ClaimKey> command_claims;
  std::vector<ClaimKey> state_claims;
};

[[nodiscard]] std::map<std::string, BindingFacts, std::less<>> verify_bindings(
    const CanonicalValue& value) {
  const auto& rows = require_array(value, "control.bindings");
  if (rows.empty() || rows.size() > core::kMaxDeploymentApplicabilityRows) {
    fail_retained_bundle("control.bindings must be non-empty and within its fixed bound");
  }
  std::map<std::string, BindingFacts, std::less<>> result;
  std::string previous;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path = "control.bindings[" + std::to_string(index) + "]";
    const auto& row = require_object(rows[index], path);
    require_exact_fields(
        row,
        {"control_binding_id", "control_binding_revision", "controllable_component_id",
         "command_target_id", "capability_id", "capability_definition_revision",
         "control_interface_contract_id", "control_interface_revision", "provider_instance_id",
         "provider_manifest_id", "provider_manifest_revision", "state_authority_id",
         "state_authority_class", "stop_evidence_policy_id", "stop_evidence_policy_revision",
         "containment_granularity", "command_claims", "state_claims"},
        path);
    const std::string id = require_identity_member(row, "control_binding_id", path);
    if (!previous.empty() && previous >= id) {
      fail_retained_bundle("control bindings must be strictly sorted by binding ID");
    }
    previous = id;
    BindingFacts facts{
        require_digest_member(row, "control_binding_revision", path),
        verify_claims(require_member(row, "command_claims", path), path + ".command_claims", true),
        verify_claims(require_member(row, "state_claims", path), path + ".state_claims"),
    };
    for (const auto field :
         {"controllable_component_id", "command_target_id", "capability_id",
          "control_interface_contract_id", "provider_instance_id", "provider_manifest_id",
          "state_authority_id", "stop_evidence_policy_id"}) {
      static_cast<void>(require_identity_member(row, field, path));
    }
    for (const auto field : {"capability_definition_revision", "control_interface_revision",
                             "provider_manifest_revision", "stop_evidence_policy_revision"}) {
      static_cast<void>(require_digest_member(row, field, path));
    }
    const std::string& authority = require_string(
        require_member(row, "state_authority_class", path), path + ".state_authority_class");
    if (authority != "measured" && authority != "commanded" && authority != "predicted") {
      fail_retained_bundle(path + " has an unknown state-authority class");
    }
    const std::string& containment = require_string(
        require_member(row, "containment_granularity", path), path + ".containment_granularity");
    if (containment != "resource" && containment != "controller_set" &&
        containment != "control_domain") {
      fail_retained_bundle(path + " has unknown containment granularity");
    }
    if (!result.emplace(id, std::move(facts)).second) {
      fail_retained_bundle(path + " duplicates a control binding");
    }
  }
  return result;
}

[[nodiscard]] std::set<std::string> verify_directions(const CanonicalValue& value,
                                                      std::string_view path) {
  const auto& rows = require_array(value, path);
  if (rows.size() > 2U) {
    fail_retained_bundle(std::string(path) + " has too many directions");
  }
  std::set<std::string> result;
  std::string previous;
  for (const auto& row : rows) {
    const std::string& direction = require_string(row, path);
    if (direction != "command" && direction != "state") {
      fail_retained_bundle(std::string(path) + " contains an unknown direction");
    }
    if (!previous.empty() && previous >= direction) {
      fail_retained_bundle(std::string(path) + " is not canonical and unique");
    }
    previous = direction;
    result.insert(direction);
  }
  return result;
}

using ProjectionKey = std::tuple<std::string, std::string, std::string>;

struct ProjectionFacts final {
  std::set<std::string> claimed_directions;
  std::vector<ClaimKey> exported_reference_interfaces;
  std::vector<ClaimKey> exported_state_interfaces;
};

struct ControllerFacts final {
  std::string logical_symbol;
  std::string implementation_id;
  std::map<ProjectionKey, ProjectionFacts> projections;
};

[[nodiscard]] std::map<std::string, ControllerFacts, std::less<>> verify_controllers(
    const CanonicalValue& value, const std::map<std::string, BindingFacts, std::less<>>& bindings) {
  const auto& rows = require_array(value, "control.controllers");
  if (rows.empty() || rows.size() > 2U) {
    fail_retained_bundle("reference control profile requires one or two controllers");
  }
  std::map<std::string, ControllerFacts, std::less<>> result;
  std::string previous;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path = "control.controllers[" + std::to_string(index) + "]";
    const auto& row = require_object(rows[index], path);
    require_exact_fields(
        row,
        {"controller_role_id", "logical_controller_symbol", "generated_controller_name",
         "implementation", "configuration_digest", "binding_projections"},
        path);
    const std::string role = require_identity_member(row, "controller_role_id", path);
    if (!previous.empty() && previous >= role) {
      fail_retained_bundle("control controllers must be strictly sorted by role ID");
    }
    previous = role;
    const std::string logical = require_identity_member(row, "logical_controller_symbol", path);
    if (require_identity_member(row, "generated_controller_name", path) != logical) {
      fail_retained_bundle(path + " generated controller name differs from its exact symbol");
    }

    const auto& implementation =
        require_object(require_member(row, "implementation", path), path + ".implementation");
    require_exact_fields(implementation,
                         {"implementation_id", "implementation_revision", "implementation_type"},
                         path + ".implementation");
    const std::string id =
        require_identity_member(implementation, "implementation_id", path + ".implementation");
    const auto descriptor =
        std::find_if(kImplementations.begin(), kImplementations.end(),
                     [&](const auto& candidate) { return candidate.implementation_id == id; });
    if (descriptor == kImplementations.end() ||
        require_digest_member(implementation, "implementation_revision",
                              path + ".implementation") != implementation_revision(*descriptor) ||
        require_string(
            require_member(implementation, "implementation_type", path + ".implementation"),
            path + ".implementation.implementation_type") != descriptor->implementation_type) {
      fail_retained_bundle(path + " selects an unqualified controller implementation");
    }
    const std::string configuration_digest =
        require_digest_member(row, "configuration_digest", path);
    const std::string expected_configuration_digest = compute_content_identity(
        rows[index],
        ContentIdentitySpec{
            .identity_domain = "control-controller-configuration-digest",
            .api_version = std::string(kControlDomainControlApiVersion) + "#controller",
            .own_field = "configuration_digest",
            .root_shape = ContentIdentityRootShape::kNestedRecord,
        });
    if (configuration_digest != expected_configuration_digest) {
      fail_retained_bundle(path + " has an invalid configuration digest");
    }

    ControllerFacts facts{logical, id, {}};
    const auto& projections = require_array(require_member(row, "binding_projections", path),
                                            path + ".binding_projections");
    if (projections.empty()) {
      fail_retained_bundle(path + " has no binding projection");
    }
    std::tuple<std::string, std::string, std::string> previous_projection;
    bool has_previous = false;
    for (std::size_t projection_index = 0U; projection_index < projections.size();
         ++projection_index) {
      const std::string projection_path =
          path + ".binding_projections[" + std::to_string(projection_index) + "]";
      const auto& projection = require_object(projections[projection_index], projection_path);
      require_exact_fields(projection,
                           {"control_binding_id", "control_binding_revision", "control_route_id",
                            "activation_set_id", "claimed_directions", "claimed_command_interfaces",
                            "claimed_state_interfaces", "exported_reference_interfaces",
                            "exported_state_interfaces"},
                           projection_path);
      const std::string binding_id =
          require_identity_member(projection, "control_binding_id", projection_path);
      const auto binding = bindings.find(binding_id);
      if (binding == bindings.end() ||
          require_digest_member(projection, "control_binding_revision", projection_path) !=
              binding->second.revision) {
        fail_retained_bundle(projection_path + " selects an unknown binding revision");
      }
      const std::string route_id =
          require_identity_member(projection, "control_route_id", projection_path);
      const std::string activation_id =
          require_identity_member(projection, "activation_set_id", projection_path);
      const auto key = std::make_tuple(binding_id, route_id, activation_id);
      if (has_previous && previous_projection >= key) {
        fail_retained_bundle(projection_path + " is not canonical and unique");
      }
      previous_projection = key;
      has_previous = true;
      const auto directions =
          verify_directions(require_member(projection, "claimed_directions", projection_path),
                            projection_path + ".claimed_directions");
      const auto command =
          verify_claims(require_member(projection, "claimed_command_interfaces", projection_path),
                        projection_path + ".claimed_command_interfaces");
      const auto state =
          verify_claims(require_member(projection, "claimed_state_interfaces", projection_path),
                        projection_path + ".claimed_state_interfaces");
      if ((directions.contains("command") ? command != binding->second.command_claims
                                          : !command.empty()) ||
          (directions.contains("state") ? state != binding->second.state_claims : !state.empty())) {
        fail_retained_bundle(projection_path +
                             " does not conform exactly to the binding interface claims");
      }
      auto exported_reference = verify_claims(
          require_member(projection, "exported_reference_interfaces", projection_path),
          projection_path + ".exported_reference_interfaces");
      auto exported_state =
          verify_claims(require_member(projection, "exported_state_interfaces", projection_path),
                        projection_path + ".exported_state_interfaces");
      if (!facts.projections
               .emplace(key, ProjectionFacts{directions, std::move(exported_reference),
                                             std::move(exported_state)})
               .second) {
        fail_retained_bundle(projection_path + " duplicates a controller projection");
      }
    }

    bool has_claim = false;
    bool has_export = false;
    for (const auto& [unused, projection] : facts.projections) {
      static_cast<void>(unused);
      has_claim |= !projection.claimed_directions.empty();
      has_export |= !projection.exported_reference_interfaces.empty() ||
                    !projection.exported_state_interfaces.empty();
    }
    const std::string_view expected_implementation =
        has_export ? "manyforge.ros2_control.admittance_controller"
                   : (has_claim ? "manyforge.ros2_control.joint_trajectory_controller.direct"
                                : "manyforge.ros2_control.joint_trajectory_controller."
                                  "chainable");
    if ((has_export && !has_claim) || facts.implementation_id != expected_implementation) {
      fail_retained_bundle(path +
                           " implementation does not match its exact claim/export semantics");
    }
    if (!result.emplace(role, std::move(facts)).second) {
      fail_retained_bundle(path + " duplicates a controller role");
    }
  }
  return result;
}

struct EdgeRef final {
  std::string upstream;
  std::string downstream;

  friend bool operator<(const EdgeRef& left, const EdgeRef& right) {
    return std::tie(left.upstream, left.downstream) < std::tie(right.upstream, right.downstream);
  }
  friend bool operator==(const EdgeRef&, const EdgeRef&) = default;
};

[[nodiscard]] std::vector<std::string> verify_identity_set(const CanonicalValue& value,
                                                           std::string_view path,
                                                           bool require_non_empty = false) {
  const auto& rows = require_array(value, path);
  if (require_non_empty && rows.empty()) {
    fail_retained_bundle(std::string(path) + " must be non-empty");
  }
  std::vector<std::string> result;
  result.reserve(rows.size());
  std::string previous;
  for (const auto& row : rows) {
    const std::string& identity = require_string(row, path);
    require_retained_portable_identity(identity, path);
    if (!previous.empty() && previous >= identity) {
      fail_retained_bundle(std::string(path) + " is not canonical and unique");
    }
    previous = identity;
    result.push_back(identity);
  }
  return result;
}

[[nodiscard]] std::vector<std::string> verify_identity_sequence(const CanonicalValue& value,
                                                                std::string_view path,
                                                                bool require_non_empty = false) {
  const auto& rows = require_array(value, path);
  if (require_non_empty && rows.empty()) {
    fail_retained_bundle(std::string(path) + " must be non-empty");
  }
  std::vector<std::string> result;
  result.reserve(rows.size());
  std::set<std::string, std::less<>> unique;
  for (const auto& row : rows) {
    const std::string& identity = require_string(row, path);
    require_retained_portable_identity(identity, path);
    if (!unique.insert(identity).second) {
      fail_retained_bundle(std::string(path) + " contains a duplicate ordered identity");
    }
    result.push_back(identity);
  }
  return result;
}

[[nodiscard]] EdgeRef verify_edge_ref(const CanonicalValue& value, std::string_view path) {
  const auto& row = require_object(value, path);
  require_exact_fields(row, {"upstream_controller_role_id", "downstream_controller_role_id"}, path);
  EdgeRef result{
      require_identity_member(row, "upstream_controller_role_id", path),
      require_identity_member(row, "downstream_controller_role_id", path),
  };
  if (result.upstream == result.downstream) {
    fail_retained_bundle(std::string(path) + " contains a self edge");
  }
  return result;
}

struct ActivationFacts final {
  std::set<std::string> roles;
  std::set<EdgeRef> edges;
};

[[nodiscard]] std::map<std::string, ActivationFacts, std::less<>> verify_activation_sets(
    const CanonicalValue& value,
    const std::map<std::string, ControllerFacts, std::less<>>& controllers) {
  const auto& rows = require_array(value, "control.activation_sets");
  if (rows.empty() || rows.size() > core::kMaxControllerTopologyActivationSets) {
    fail_retained_bundle("control.activation_sets is empty or exceeds its bound");
  }
  std::map<std::string, ActivationFacts, std::less<>> result;
  std::string previous;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path = "control.activation_sets[" + std::to_string(index) + "]";
    const auto& row = require_object(rows[index], path);
    require_exact_fields(
        row, {"activation_set_id", "active_controller_role_ids", "active_chain_edges"}, path);
    const std::string id = require_identity_member(row, "activation_set_id", path);
    if (!previous.empty() && previous >= id) {
      fail_retained_bundle("control activation sets are not canonical and unique");
    }
    previous = id;
    ActivationFacts facts;
    for (const auto& role :
         verify_identity_set(require_member(row, "active_controller_role_ids", path),
                             path + ".active_controller_role_ids", true)) {
      if (!controllers.contains(role)) {
        fail_retained_bundle(path + " names an unknown controller role");
      }
      facts.roles.insert(role);
    }
    const auto& edges = require_array(require_member(row, "active_chain_edges", path),
                                      path + ".active_chain_edges");
    EdgeRef previous_edge;
    bool has_previous = false;
    for (std::size_t edge_index = 0U; edge_index < edges.size(); ++edge_index) {
      const EdgeRef edge = verify_edge_ref(
          edges[edge_index], path + ".active_chain_edges[" + std::to_string(edge_index) + "]");
      if (!facts.roles.contains(edge.upstream) || !facts.roles.contains(edge.downstream) ||
          (has_previous && !(previous_edge < edge)) || !facts.edges.insert(edge).second) {
        fail_retained_bundle(path + " has an invalid active chain-edge set");
      }
      previous_edge = edge;
      has_previous = true;
    }
    if (!result.emplace(id, std::move(facts)).second) {
      fail_retained_bundle(path + " duplicates an activation set");
    }
  }
  return result;
}

struct RouteFacts final {
  std::string binding_id;
  std::string binding_revision;
  std::set<std::string> activation_sets;
  std::vector<std::string> controller_roles;
  std::set<EdgeRef> path_edges;
};

[[nodiscard]] bool is_ordered_subsequence(std::span<const ClaimKey> selected,
                                          std::span<const ClaimKey> available) {
  std::size_t cursor = 0U;
  for (const auto& claim : selected) {
    while (cursor < available.size() && available[cursor] != claim) {
      ++cursor;
    }
    if (cursor == available.size()) {
      return false;
    }
    ++cursor;
  }
  return true;
}

[[nodiscard]] std::map<std::string, RouteFacts, std::less<>> verify_routes(
    const CanonicalValue& value, const std::map<std::string, BindingFacts, std::less<>>& bindings,
    const std::map<std::string, ControllerFacts, std::less<>>& controllers,
    const std::map<std::string, ActivationFacts, std::less<>>& activations) {
  const auto& rows = require_array(value, "control.routes");
  if (rows.empty() || rows.size() > core::kMaxDeploymentApplicabilityRows) {
    fail_retained_bundle("control.routes is empty or exceeds its bound");
  }
  std::map<std::string, RouteFacts, std::less<>> result;
  std::tuple<std::string, std::string> previous;
  bool has_previous = false;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path = "control.routes[" + std::to_string(index) + "]";
    const auto& row = require_object(rows[index], path);
    require_exact_fields(
        row,
        {"control_binding_id", "control_binding_revision", "control_route_id",
         "logical_route_symbol", "controller_role_ids", "logical_controller_symbols", "path_edges",
         "compatible_activation_set_ids", "endpoints"},
        path);
    const std::string binding_id = require_identity_member(row, "control_binding_id", path);
    const auto binding = bindings.find(binding_id);
    const std::string binding_revision =
        require_digest_member(row, "control_binding_revision", path);
    if (binding == bindings.end() || binding->second.revision != binding_revision) {
      fail_retained_bundle(path + " selects an unknown binding revision");
    }
    const std::string route_id = require_identity_member(row, "control_route_id", path);
    const auto key = std::make_tuple(binding_id, route_id);
    if (has_previous && previous >= key) {
      fail_retained_bundle("control routes are not canonical and unique");
    }
    previous = key;
    has_previous = true;
    static_cast<void>(require_identity_member(row, "logical_route_symbol", path));

    const auto roles = verify_identity_sequence(require_member(row, "controller_role_ids", path),
                                                path + ".controller_role_ids", true);
    const auto symbols =
        verify_identity_sequence(require_member(row, "logical_controller_symbols", path),
                                 path + ".logical_controller_symbols", true);
    if (roles.size() != symbols.size()) {
      fail_retained_bundle(path + " controller roles and symbols differ in size");
    }
    for (std::size_t role_index = 0U; role_index < roles.size(); ++role_index) {
      const auto controller = controllers.find(roles[role_index]);
      if (controller == controllers.end() ||
          controller->second.logical_symbol != symbols[role_index]) {
        fail_retained_bundle(path + " does not map an exact controller role/symbol");
      }
    }

    RouteFacts facts{binding_id, binding_revision, {}, roles, {}};
    const auto compatible_activations =
        verify_identity_set(require_member(row, "compatible_activation_set_ids", path),
                            path + ".compatible_activation_set_ids", true);
    for (const auto& activation : compatible_activations) {
      const auto found = activations.find(activation);
      if (found == activations.end() ||
          !std::all_of(roles.begin(), roles.end(),
                       [&](const auto& role) { return found->second.roles.contains(role); })) {
        fail_retained_bundle(path + " selects an incompatible activation set");
      }
      for (const auto& role : roles) {
        const auto controller = controllers.find(role);
        if (controller == controllers.end() ||
            !controller->second.projections.contains(
                ProjectionKey{binding_id, route_id, activation})) {
          fail_retained_bundle(path +
                               " has no exact controller projection for its compatible "
                               "activation set");
        }
      }
      facts.activation_sets.insert(activation);
    }

    const auto& edge_rows =
        require_array(require_member(row, "path_edges", path), path + ".path_edges");
    if (edge_rows.size() + 1U != roles.size()) {
      fail_retained_bundle(path + " path-edge count does not join its role path");
    }
    std::set<EdgeRef> path_edges;
    for (std::size_t edge_index = 0U; edge_index < edge_rows.size(); ++edge_index) {
      const std::string edge_path = path + ".path_edges[" + std::to_string(edge_index) + "]";
      const auto& edge = require_object(edge_rows[edge_index], edge_path);
      require_exact_fields(edge,
                           {"upstream_controller_role_id", "upstream_logical_controller_symbol",
                            "downstream_controller_role_id", "downstream_logical_controller_symbol",
                            "reference_interfaces", "state_interfaces"},
                           edge_path);
      const std::string upstream =
          require_identity_member(edge, "upstream_controller_role_id", edge_path);
      const std::string downstream =
          require_identity_member(edge, "downstream_controller_role_id", edge_path);
      if (upstream != roles[edge_index] || downstream != roles[edge_index + 1U] ||
          require_identity_member(edge, "upstream_logical_controller_symbol", edge_path) !=
              symbols[edge_index] ||
          require_identity_member(edge, "downstream_logical_controller_symbol", edge_path) !=
              symbols[edge_index + 1U] ||
          !path_edges.insert({upstream, downstream}).second) {
        fail_retained_bundle(edge_path + " does not match the ordered controller path");
      }
      const auto reference_interfaces =
          verify_claims(require_member(edge, "reference_interfaces", edge_path),
                        edge_path + ".reference_interfaces", true);
      const auto state_interfaces = verify_claims(
          require_member(edge, "state_interfaces", edge_path), edge_path + ".state_interfaces");
      const auto& downstream_facts = controllers.at(downstream);
      for (const auto& activation : compatible_activations) {
        const auto projection =
            downstream_facts.projections.find(ProjectionKey{binding_id, route_id, activation});
        if (projection == downstream_facts.projections.end() ||
            !is_ordered_subsequence(reference_interfaces,
                                    projection->second.exported_reference_interfaces) ||
            !is_ordered_subsequence(state_interfaces,
                                    projection->second.exported_state_interfaces)) {
          fail_retained_bundle(edge_path +
                               " does not conform to the downstream exported interfaces");
        }
      }
    }

    for (const auto& activation : compatible_activations) {
      const auto& found = activations.at(activation);
      if (!std::all_of(path_edges.begin(), path_edges.end(),
                       [&](const auto& edge) { return found.edges.contains(edge); })) {
        fail_retained_bundle(path + " selects an incompatible activation set");
      }
    }
    facts.path_edges = path_edges;

    const auto& endpoints =
        require_array(require_member(row, "endpoints", path), path + ".endpoints");
    if (endpoints.empty() || endpoints.size() > core::kMaxCapabilityProviderEndpointBindings) {
      fail_retained_bundle(path + " has an invalid endpoint-role inventory");
    }
    std::string previous_endpoint;
    for (std::size_t endpoint_index = 0U; endpoint_index < endpoints.size(); ++endpoint_index) {
      const std::string endpoint_path = path + ".endpoints[" + std::to_string(endpoint_index) + "]";
      const auto& endpoint = require_object(endpoints[endpoint_index], endpoint_path);
      require_exact_fields(endpoint,
                           {"endpoint_role_id", "logical_endpoint_symbol",
                            "generated_endpoint_name", "is_command_endpoint", "is_state_endpoint"},
                           endpoint_path);
      const std::string endpoint_id =
          require_identity_member(endpoint, "endpoint_role_id", endpoint_path);
      if (!previous_endpoint.empty() && previous_endpoint >= endpoint_id) {
        fail_retained_bundle(endpoint_path + " is not canonical and unique");
      }
      previous_endpoint = endpoint_id;
      const std::string logical =
          require_identity_member(endpoint, "logical_endpoint_symbol", endpoint_path);
      if (require_identity_member(endpoint, "generated_endpoint_name", endpoint_path) != logical ||
          (!require_bool(require_member(endpoint, "is_command_endpoint", endpoint_path),
                         endpoint_path + ".is_command_endpoint") &&
           !require_bool(require_member(endpoint, "is_state_endpoint", endpoint_path),
                         endpoint_path + ".is_state_endpoint"))) {
        fail_retained_bundle(endpoint_path + " has an invalid generated endpoint mapping");
      }
    }
    const std::string semantic_key = binding_id + "\x1f" + route_id;
    if (!result.emplace(semantic_key, std::move(facts)).second) {
      fail_retained_bundle(path + " duplicates a binding route");
    }
  }
  return result;
}

void verify_reference_profile_shape(
    const std::map<std::string, ControllerFacts, std::less<>>& controllers,
    const std::map<std::string, RouteFacts, std::less<>>& routes) {
  constexpr std::string_view kDirect = "manyforge.ros2_control.joint_trajectory_controller.direct";
  constexpr std::string_view kChainable =
      "manyforge.ros2_control.joint_trajectory_controller.chainable";
  constexpr std::string_view kAdmittance = "manyforge.ros2_control.admittance_controller";
  const auto count_implementation = [&](std::string_view expected) {
    return std::count_if(controllers.begin(), controllers.end(),
                         [&](const auto& row) { return row.second.implementation_id == expected; });
  };
  if ((controllers.size() == 1U && count_implementation(kDirect) != 1) ||
      (controllers.size() == 2U &&
       (count_implementation(kChainable) != 1 || count_implementation(kAdmittance) != 1))) {
    fail_retained_bundle(
        "control artifact does not select one exact direct JTC or one exact "
        "JTC/admittance chain");
  }
  for (const auto& [unused, route] : routes) {
    static_cast<void>(unused);
    if (route.controller_roles.size() != controllers.size() ||
        route.path_edges.size() + 1U != controllers.size() ||
        !std::all_of(controllers.begin(), controllers.end(), [&](const auto& row) {
          return std::find(route.controller_roles.begin(), route.controller_roles.end(),
                           row.first) != route.controller_roles.end();
        })) {
      fail_retained_bundle(
          "control route does not realize the complete reference-profile "
          "controller topology");
    }
  }
}

void verify_desired_configuration(
    const CanonicalValue& value, std::string_view domain_id, std::string_view domain_revision,
    std::string_view topology_id, std::string_view topology_revision,
    const std::map<std::string, BindingFacts, std::less<>>& bindings,
    const std::map<std::string, ActivationFacts, std::less<>>& activations,
    const std::map<std::string, RouteFacts, std::less<>>& routes) {
  const auto& desired = require_object(value, "control.desired_configuration");
  require_exact_fields(desired,
                       {"control_domain_id", "control_domain_revision", "controller_topology_id",
                        "topology_revision", "activation_set_id", "binding_routes"},
                       "control.desired_configuration");
  if (require_identity_member(desired, "control_domain_id", "control.desired_configuration") !=
          domain_id ||
      require_digest_member(desired, "control_domain_revision", "control.desired_configuration") !=
          domain_revision ||
      require_identity_member(desired, "controller_topology_id", "control.desired_configuration") !=
          topology_id ||
      require_digest_member(desired, "topology_revision", "control.desired_configuration") !=
          topology_revision) {
    fail_retained_bundle("desired control configuration differs from the product subject");
  }
  const std::string activation =
      require_identity_member(desired, "activation_set_id", "control.desired_configuration");
  if (!activations.contains(activation)) {
    fail_retained_bundle("desired control configuration selects an unknown activation set");
  }
  const auto& binding_routes =
      require_array(require_member(desired, "binding_routes", "control.desired_configuration"),
                    "control.desired_configuration.binding_routes");
  if (binding_routes.size() != bindings.size()) {
    fail_retained_bundle("desired configuration does not cover every exact domain binding");
  }
  std::string previous_binding;
  for (std::size_t index = 0U; index < binding_routes.size(); ++index) {
    const std::string path =
        "control.desired_configuration.binding_routes[" + std::to_string(index) + "]";
    const auto& row = require_object(binding_routes[index], path);
    require_exact_fields(
        row, {"control_binding_id", "control_binding_revision", "control_route_id"}, path);
    const std::string binding_id = require_identity_member(row, "control_binding_id", path);
    const std::string binding_revision =
        require_digest_member(row, "control_binding_revision", path);
    const std::string route_id = require_identity_member(row, "control_route_id", path);
    if (!previous_binding.empty() && previous_binding >= binding_id) {
      fail_retained_bundle("desired binding routes are not canonical and unique");
    }
    previous_binding = binding_id;
    const auto binding = bindings.find(binding_id);
    const auto route = routes.find(binding_id + "\x1f" + route_id);
    if (binding == bindings.end() || binding->second.revision != binding_revision ||
        route == routes.end() || !route->second.activation_sets.contains(activation)) {
      fail_retained_bundle(path + " selects an unavailable binding route/activation tuple");
    }
  }
}

void verify_source_conformance(const CanonicalValue& value, const CanonicalValue& bindings,
                               const CanonicalValue& controllers) {
  const auto& conformance = require_object(value, "control.source_conformance");
  require_exact_fields(conformance, {"api_version", "source_conformance_digest"},
                       "control.source_conformance");
  if (require_string(require_member(conformance, "api_version", "control.source_conformance"),
                     "control.source_conformance.api_version") != kSourceConformanceApiVersion) {
    fail_retained_bundle("control source-conformance evidence has an unsupported API");
  }
  const std::string digest =
      require_digest_member(conformance, "source_conformance_digest", "control.source_conformance");
  const CanonicalValue preimage(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kSourceConformanceApiVersion))},
      {"source_conformance_digest", CanonicalValue(digest)},
      {"bindings", bindings},
      {"controllers", controllers},
  });
  const std::string expected = compute_content_identity(
      preimage, ContentIdentitySpec{
                    .identity_domain = "control-source-conformance-digest",
                    .api_version = std::string(kSourceConformanceApiVersion),
                    .own_field = "source_conformance_digest",
                });
  if (digest != expected) {
    fail_retained_bundle(
        "control source-conformance digest does not bind its exact claims "
        "and controller configuration");
  }
}

}  // namespace

std::string control_domain_control_product_id(const core::ControlDomainId& control_domain_id) {
  require_retained_portable_identity(control_domain_id.value(), "control_domain_id");
  std::string preimage = "manyforge.control-domain-product-id.v1";
  preimage.push_back('\0');
  preimage.append(control_domain_id.value());
  const std::string digest = sha256_digest(preimage);
  return "control_domain_control_" + digest.substr(7U);
}

CanonicalValue parse_and_verify_control_domain_control(std::string_view canonical_bytes) {
  if (canonical_bytes.empty() || canonical_bytes.size() > kMaxControlDomainControlBytes) {
    fail_retained_bundle("control-domain control artifact is empty or exceeds its byte bound");
  }
  CanonicalValue root;
  try {
    root = parse_strict_json_model_yaml(canonical_bytes).root();
  } catch (const std::exception& error) {
    fail_retained_bundle(std::string("control-domain control artifact is invalid JSON: ") +
                         error.what());
  }
  if (canonical_json(root) != canonical_bytes) {
    fail_retained_bundle("control-domain control artifact is not exact canonical JSON");
  }
  const auto& object = require_object(root, "control");
  require_exact_fields(
      object,
      {"api_version", "control_domain_id", "control_domain_revision", "controller_topology_id",
       "topology_revision", "control_coordinator_endpoint_role", "containment_granularity",
       "implementation_profile", "desired_configuration", "bindings", "controllers",
       "activation_sets", "routes", "source_conformance"},
      "control");
  if (require_string(require_member(object, "api_version", "control"), "control.api_version") !=
      kControlDomainControlApiVersion) {
    fail_retained_bundle("control-domain control artifact has an unsupported API version");
  }
  const std::string domain_id = require_identity_member(object, "control_domain_id", "control");
  const std::string domain_revision =
      require_digest_member(object, "control_domain_revision", "control");
  const std::string topology_id =
      require_identity_member(object, "controller_topology_id", "control");
  const std::string topology_revision =
      require_digest_member(object, "topology_revision", "control");
  static_cast<void>(
      require_identity_member(object, "control_coordinator_endpoint_role", "control"));
  const std::string& containment =
      require_string(require_member(object, "containment_granularity", "control"),
                     "control.containment_granularity");
  if (containment != "resource" && containment != "controller_set" &&
      containment != "control_domain") {
    fail_retained_bundle("control-domain product has unknown containment granularity");
  }

  const auto& profile = require_object(require_member(object, "implementation_profile", "control"),
                                       "control.implementation_profile");
  require_exact_fields(profile,
                       {"implementation_profile_id", "implementation_profile_revision",
                        "generated_controller_manager_name"},
                       "control.implementation_profile");
  if (require_identity_member(profile, "implementation_profile_id",
                              "control.implementation_profile") != kImplementationProfileId ||
      require_digest_member(profile, "implementation_profile_revision",
                            "control.implementation_profile") !=
          implementation_profile_revision()) {
    fail_retained_bundle("control-domain product selects an unqualified implementation profile");
  }
  if (require_identity_member(profile, "generated_controller_manager_name",
                              "control.implementation_profile") !=
      expected_controller_manager_name(domain_id, domain_revision, topology_id,
                                       topology_revision)) {
    fail_retained_bundle(
        "control-domain product has the wrong deterministic controller "
        "manager name");
  }

  const CanonicalValue& bindings_value = require_member(object, "bindings", "control");
  const CanonicalValue& controllers_value = require_member(object, "controllers", "control");
  const auto bindings = verify_bindings(bindings_value);
  const auto controllers = verify_controllers(controllers_value, bindings);
  const auto activations =
      verify_activation_sets(require_member(object, "activation_sets", "control"), controllers);
  const auto routes = verify_routes(require_member(object, "routes", "control"), bindings,
                                    controllers, activations);
  verify_reference_profile_shape(controllers, routes);
  verify_desired_configuration(require_member(object, "desired_configuration", "control"),
                               domain_id, domain_revision, topology_id, topology_revision, bindings,
                               activations, routes);
  verify_source_conformance(require_member(object, "source_conformance", "control"), bindings_value,
                            controllers_value);
  return root;
}

void require_control_domain_control_matches_retained_bundle(
    std::string_view canonical_bytes, const core::RetainedBundleDTO& retained_bundle,
    const core::ControlDomainId& control_domain_id) {
  const CanonicalValue root = parse_and_verify_control_domain_control(canonical_bytes);
  const auto& object = require_object(root, "control");
  const auto& graph = retained_bundle.cell_graph;
  const auto domain = std::find_if(
      graph.control_domains.begin(), graph.control_domains.end(),
      [&](const auto& candidate) { return candidate.control_domain_id == control_domain_id; });
  if (domain == graph.control_domains.end() ||
      require_identity_member(object, "control_domain_id", "control") !=
          domain->control_domain_id.value() ||
      require_digest_member(object, "control_domain_revision", "control") !=
          domain->control_domain_revision.value() ||
      require_identity_member(object, "controller_topology_id", "control") !=
          domain->controller_topology_id.value() ||
      require_digest_member(object, "topology_revision", "control") !=
          domain->topology_revision.value() ||
      require_identity_member(object, "control_coordinator_endpoint_role", "control") !=
          domain->control_coordinator_endpoint_role.value() ||
      require_string(require_member(object, "containment_granularity", "control"),
                     "control.containment_granularity") !=
          core::to_string(domain->containment_granularity)) {
    fail_retained_bundle("control-domain artifact subject differs from its retained graph");
  }

  std::map<std::string, const core::ControlBindingDTO*, std::less<>> expected_bindings;
  for (const auto& binding : graph.control_bindings) {
    if (binding.control_domain_id == control_domain_id) {
      expected_bindings.emplace(binding.control_binding_id.value(), &binding);
    }
  }
  const auto& binding_rows =
      require_array(require_member(object, "bindings", "control"), "control.bindings");
  if (binding_rows.size() != expected_bindings.size()) {
    fail_retained_bundle("control-domain artifact binding set differs from its retained graph");
  }
  for (std::size_t index = 0U; index < binding_rows.size(); ++index) {
    const std::string path = "control.bindings[" + std::to_string(index) + "]";
    const auto& row = require_object(binding_rows[index], path);
    const auto expected =
        expected_bindings.find(require_identity_member(row, "control_binding_id", path));
    if (expected == expected_bindings.end()) {
      fail_retained_bundle(path + " has no retained graph binding subject");
    }
    const auto& binding = *expected->second;
    const bool scalar_mismatch =
        require_digest_member(row, "control_binding_revision", path) !=
            binding.control_binding_revision.value() ||
        require_identity_member(row, "controllable_component_id", path) !=
            binding.controllable_component_id.value() ||
        require_identity_member(row, "command_target_id", path) !=
            binding.command_target_id.value() ||
        require_identity_member(row, "capability_id", path) != binding.capability_id.value() ||
        require_digest_member(row, "capability_definition_revision", path) !=
            binding.capability_definition_revision.value() ||
        require_identity_member(row, "control_interface_contract_id", path) !=
            binding.control_interface_contract_id.value() ||
        require_digest_member(row, "control_interface_revision", path) !=
            binding.control_interface_revision.value() ||
        require_identity_member(row, "provider_instance_id", path) !=
            binding.provider_instance_id.value() ||
        require_identity_member(row, "provider_manifest_id", path) !=
            binding.provider_manifest_id.value() ||
        require_digest_member(row, "provider_manifest_revision", path) !=
            binding.provider_manifest_revision.value() ||
        require_identity_member(row, "state_authority_id", path) !=
            binding.state_authority_id.value() ||
        require_string(require_member(row, "state_authority_class", path),
                       path + ".state_authority_class") !=
            core::to_string(binding.state_authority_class) ||
        require_identity_member(row, "stop_evidence_policy_id", path) !=
            binding.stop_evidence_policy_id.value() ||
        require_digest_member(row, "stop_evidence_policy_revision", path) !=
            binding.stop_evidence_policy_revision.value() ||
        require_string(require_member(row, "containment_granularity", path),
                       path + ".containment_granularity") !=
            core::to_string(binding.containment_granularity);
    if (scalar_mismatch) {
      fail_retained_bundle(path + " scalar semantics differ from its retained graph binding");
    }

    std::set<std::pair<std::string, std::string>> expected_resources;
    for (const auto& resource : binding.resource_bindings) {
      expected_resources.emplace(resource.resource_role_id.value(), resource.resource_id.value());
    }
    const auto claim_resources = [&](std::string_view field) {
      std::set<std::pair<std::string, std::string>> resources;
      const auto& claims =
          require_array(require_member(row, field, path), path + "." + std::string(field));
      for (const auto& claim_value : claims) {
        const auto& claim = require_object(claim_value, path);
        resources.emplace(require_identity_member(claim, "resource_role_id", path),
                          require_identity_member(claim, "resource_id", path));
      }
      return resources;
    };
    if (claim_resources("command_claims") != expected_resources ||
        claim_resources("state_claims") != expected_resources) {
      fail_retained_bundle(path + " claim/resource coverage differs from its retained binding");
    }
  }

  const core::DesiredControlConfigurationDTO* expected_desired = nullptr;
  for (const auto& desired : retained_bundle.desired_control_configurations) {
    if (desired.control_domain_id == control_domain_id) {
      if (expected_desired != nullptr) {
        fail_retained_bundle("retained bundle duplicates a desired control-domain subject");
      }
      expected_desired = &desired;
    }
  }
  if (expected_desired == nullptr) {
    fail_retained_bundle("retained bundle omits the control-domain desired configuration");
  }
  const auto& desired = require_object(require_member(object, "desired_configuration", "control"),
                                       "control.desired_configuration");
  if (require_identity_member(desired, "control_domain_id", "control.desired_configuration") !=
          expected_desired->control_domain_id.value() ||
      require_digest_member(desired, "control_domain_revision", "control.desired_configuration") !=
          expected_desired->control_domain_revision.value() ||
      require_identity_member(desired, "controller_topology_id", "control.desired_configuration") !=
          expected_desired->controller_topology_id.value() ||
      require_digest_member(desired, "topology_revision", "control.desired_configuration") !=
          expected_desired->topology_revision.value() ||
      require_identity_member(desired, "activation_set_id", "control.desired_configuration") !=
          expected_desired->activation_set_id.value()) {
    fail_retained_bundle("control-domain desired selection differs from its retained bundle");
  }
  const auto& desired_routes =
      require_array(require_member(desired, "binding_routes", "control.desired_configuration"),
                    "control.desired_configuration.binding_routes");
  if (desired_routes.size() != expected_desired->binding_routes.size()) {
    fail_retained_bundle("control-domain desired route set differs from its retained bundle");
  }
  for (std::size_t index = 0U; index < desired_routes.size(); ++index) {
    const auto& row =
        require_object(desired_routes[index], "control.desired_configuration.binding_routes");
    const auto& expected = expected_desired->binding_routes[index];
    if (require_identity_member(row, "control_binding_id", "control.desired_configuration") !=
            expected.control_binding_id.value() ||
        require_digest_member(row, "control_binding_revision", "control.desired_configuration") !=
            expected.control_binding_revision.value() ||
        require_identity_member(row, "control_route_id", "control.desired_configuration") !=
            expected.control_route_id.value()) {
      fail_retained_bundle("control-domain desired route differs from its retained bundle");
    }
  }

  using RouteKey = std::pair<std::string, std::string>;
  std::map<RouteKey, const core::RouteEndpointProjectionDTO*> expected_routes;
  for (const auto& [unused, binding] : expected_bindings) {
    static_cast<void>(unused);
    if (binding->legal_control_route_ids.size() != binding->route_endpoint_projections.size()) {
      fail_retained_bundle("retained binding route/endpoint inventory is incoherent");
    }
    for (const auto& route_id : binding->legal_control_route_ids) {
      const auto endpoint = std::find_if(
          binding->route_endpoint_projections.begin(), binding->route_endpoint_projections.end(),
          [&](const auto& candidate) { return candidate.control_route_id == route_id; });
      if (endpoint == binding->route_endpoint_projections.end() ||
          !expected_routes
               .emplace(RouteKey{binding->control_binding_id.value(), route_id.value()}, &*endpoint)
               .second) {
        fail_retained_bundle("retained binding has an invalid route/endpoint projection");
      }
    }
  }
  const auto& route_rows =
      require_array(require_member(object, "routes", "control"), "control.routes");
  if (route_rows.size() != expected_routes.size()) {
    fail_retained_bundle("control-domain route set differs from its retained graph");
  }
  for (std::size_t index = 0U; index < route_rows.size(); ++index) {
    const std::string path = "control.routes[" + std::to_string(index) + "]";
    const auto& row = require_object(route_rows[index], path);
    const auto expected =
        expected_routes.find(RouteKey{require_identity_member(row, "control_binding_id", path),
                                      require_identity_member(row, "control_route_id", path)});
    if (expected == expected_routes.end()) {
      fail_retained_bundle(path + " is absent from its retained graph");
    }
    std::set<std::string> command_roles;
    std::set<std::string> state_roles;
    for (const auto& endpoint_value :
         require_array(require_member(row, "endpoints", path), path + ".endpoints")) {
      const auto& endpoint = require_object(endpoint_value, path);
      const std::string role = require_identity_member(endpoint, "endpoint_role_id", path);
      if (require_bool(require_member(endpoint, "is_command_endpoint", path),
                       path + ".is_command_endpoint")) {
        command_roles.insert(role);
      }
      if (require_bool(require_member(endpoint, "is_state_endpoint", path),
                       path + ".is_state_endpoint")) {
        state_roles.insert(role);
      }
    }
    const auto strong_set = [](const auto& values) {
      std::set<std::string> result;
      for (const auto& value : values) {
        result.insert(value.value());
      }
      return result;
    };
    if (command_roles != strong_set(expected->second->command_endpoint_role_ids) ||
        state_roles != strong_set(expected->second->state_endpoint_role_ids)) {
      fail_retained_bundle(path + " endpoint roles differ from its retained binding");
    }
  }
}

}  // namespace manyforge::deployment::detail
