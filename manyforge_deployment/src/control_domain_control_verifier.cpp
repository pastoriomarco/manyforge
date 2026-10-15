#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <map>
#include <optional>
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
#include "manyforge_deployment/controller_topology_codec.hpp"
#include "manyforge_deployment/provider_manifest_codec.hpp"
#include "manyforge_deployment/stop_evidence_policy_codec.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

constexpr std::string_view kImplementationProfileApiVersion =
    "manyforge.control_implementation_profile.v1";
constexpr std::string_view kImplementationProfileId =
    "manyforge.reference_ros2_control";
constexpr std::string_view kImplementationApiVersion =
    "manyforge.control_controller_implementation.v1";
constexpr std::string_view kSourceConformanceApiVersion =
    "manyforge.control_source_conformance.v1";

struct ImplementationDescriptor final {
  std::string_view implementation_id;
  std::string_view implementation_type;
};

constexpr std::array<ImplementationDescriptor, 3U> kImplementations{{
    {"manyforge.ros2_control.admittance_controller",
     "admittance_controller/AdmittanceController"},
    {"manyforge.ros2_control.joint_trajectory_controller.chainable",
     "joint_trajectory_controller/JointTrajectoryController"},
    {"manyforge.ros2_control.joint_trajectory_controller.direct",
     "joint_trajectory_controller/JointTrajectoryController"},
}};

[[nodiscard]] const CanonicalValue::Object &
require_object(const CanonicalValue &value, std::string_view path) {
  const auto *object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail_retained_bundle(std::string(path) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array &
require_array(const CanonicalValue &value, std::string_view path) {
  const auto *array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail_retained_bundle(std::string(path) + " must be an array");
  }
  return *array;
}

[[nodiscard]] const std::string &require_string(const CanonicalValue &value,
                                                std::string_view path) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail_retained_bundle(std::string(path) + " must be a string");
  }
  return *text;
}

[[nodiscard]] bool require_bool(const CanonicalValue &value,
                                std::string_view path) {
  const auto *result = std::get_if<bool>(&value.storage());
  if (result == nullptr) {
    fail_retained_bundle(std::string(path) + " must be a boolean");
  }
  return *result;
}

[[nodiscard]] std::int64_t require_safe_integer(const CanonicalValue &value,
                                                std::string_view path) {
  const auto *number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) ||
      std::trunc(*number) != *number ||
      *number < -static_cast<double>(core::kMaxStopEvidencePolicyDurationNs) ||
      *number > static_cast<double>(core::kMaxStopEvidencePolicyDurationNs)) {
    fail_retained_bundle(std::string(path) +
                         " must be an exact JSON safe integer");
  }
  return static_cast<std::int64_t>(*number);
}

[[nodiscard]] bool is_null(const CanonicalValue &value) noexcept {
  return std::holds_alternative<std::nullptr_t>(value.storage());
}

[[nodiscard]] const CanonicalValue &
require_member(const CanonicalValue::Object &object, std::string_view name,
               std::string_view path) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [name](const auto &member) { return member.first == name; });
  if (found == object.end()) {
    fail_retained_bundle(std::string(path) + " is missing required member " +
                         std::string(name));
  }
  return found->second;
}

void require_exact_fields(const CanonicalValue::Object &object,
                          std::initializer_list<std::string_view> fields,
                          std::string_view path) {
  if (object.size() != fields.size()) {
    fail_retained_bundle(std::string(path) +
                         " has an incorrect closed field count");
  }
  for (const auto &[name, unused] : object) {
    (void)unused;
    if (std::find(fields.begin(), fields.end(), name) == fields.end()) {
      fail_retained_bundle(std::string(path) + " contains unknown member " +
                           name);
    }
  }
  for (const auto field : fields) {
    static_cast<void>(require_member(object, field, path));
  }
}

[[nodiscard]] const std::string &
require_identity_member(const CanonicalValue::Object &object,
                        std::string_view name, std::string_view path) {
  const std::string &value =
      require_string(require_member(object, name, path),
                     std::string(path) + "." + std::string(name));
  require_retained_portable_identity(value, std::string(path) + "." +
                                                std::string(name));
  return value;
}

[[nodiscard]] const std::string &
require_digest_member(const CanonicalValue::Object &object,
                      std::string_view name, std::string_view path) {
  const std::string &value =
      require_string(require_member(object, name, path),
                     std::string(path) + "." + std::string(name));
  require_retained_digest(value, std::string(path) + "." + std::string(name));
  return value;
}

[[nodiscard]] std::string
implementation_revision(const ImplementationDescriptor &descriptor) {
  const CanonicalValue root(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kImplementationApiVersion))},
      {"implementation_id",
       CanonicalValue(std::string(descriptor.implementation_id))},
      {"implementation_revision",
       CanonicalValue(
           "sha256:"
           "0000000000000000000000000000000000000000000000000000000000000000")},
      {"implementation_type",
       CanonicalValue(std::string(descriptor.implementation_type))},
  });
  return compute_content_identity(
      root, ContentIdentitySpec{
                .identity_domain = "control-controller-implementation-revision",
                .api_version = std::string(kImplementationApiVersion),
                .own_field = "implementation_revision",
            });
}

[[nodiscard]] CanonicalValue
implementation_projection(const ImplementationDescriptor &descriptor) {
  return CanonicalValue(CanonicalValue::Object{
      {"implementation_id",
       CanonicalValue(std::string(descriptor.implementation_id))},
      {"implementation_revision",
       CanonicalValue(implementation_revision(descriptor))},
      {"implementation_type",
       CanonicalValue(std::string(descriptor.implementation_type))},
  });
}

[[nodiscard]] std::string implementation_profile_revision() {
  CanonicalValue::Array implementations;
  implementations.reserve(kImplementations.size());
  for (const auto &implementation : kImplementations) {
    implementations.push_back(implementation_projection(implementation));
  }
  const CanonicalValue root(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kImplementationProfileApiVersion))},
      {"implementation_profile_id",
       CanonicalValue(std::string(kImplementationProfileId))},
      {"implementation_profile_revision",
       CanonicalValue(
           "sha256:"
           "0000000000000000000000000000000000000000000000000000000000000000")},
      {"implementations", CanonicalValue(std::move(implementations))},
  });
  return compute_content_identity(
      root, ContentIdentitySpec{
                .identity_domain = "control-implementation-profile-revision",
                .api_version = std::string(kImplementationProfileApiVersion),
                .own_field = "implementation_profile_revision",
            });
}

[[nodiscard]] std::string expected_controller_manager_name(
    std::string_view domain_id, std::string_view domain_revision,
    std::string_view topology_id, std::string_view topology_revision) {
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

  friend bool operator<(const ClaimKey &left, const ClaimKey &right) {
    return std::tie(left.resource_role_id, left.resource_id,
                    left.interface_name, left.logical_interface_symbol) <
           std::tie(right.resource_role_id, right.resource_id,
                    right.interface_name, right.logical_interface_symbol);
  }
  friend bool operator==(const ClaimKey &, const ClaimKey &) = default;
};

[[nodiscard]] std::vector<ClaimKey>
verify_claims(const CanonicalValue &value, std::string_view path,
              bool require_non_empty = false) {
  const auto &rows = require_array(value, path);
  if (require_non_empty && rows.empty()) {
    fail_retained_bundle(std::string(path) + " must be non-empty");
  }
  if (rows.size() > kMaxCellExpandedControlInterfaceClaimsPerDirection) {
    fail_retained_bundle(std::string(path) +
                         " exceeds the expanded-claim bound");
  }
  std::vector<ClaimKey> result;
  result.reserve(rows.size());
  std::set<ClaimKey> unique;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string item_path =
        std::string(path) + "[" + std::to_string(index) + "]";
    const auto &row = require_object(rows[index], item_path);
    require_exact_fields(row,
                         {"resource_role_id", "resource_id", "interface_name",
                          "logical_interface_symbol"},
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
  std::string component_id;
  std::string target_id;
  std::string interface_id;
  std::string interface_revision;
  std::string stop_policy_id;
  std::string stop_policy_revision;
  std::string containment;
  std::string capability_id;
  std::string capability_revision;
  std::string provider_instance_id;
  std::string provider_runtime_unit_id;
  std::string provider_manifest_id;
  std::string provider_manifest_revision;
  std::string state_authority_id;
  std::string state_authority_class;
  std::set<std::string, std::less<>> resource_ids;
  std::vector<ClaimKey> command_claims;
  std::vector<ClaimKey> state_claims;
};

[[nodiscard]] std::map<std::string, BindingFacts, std::less<>>
verify_bindings(const CanonicalValue &value) {
  const auto &rows = require_array(value, "control.bindings");
  if (rows.empty() || rows.size() > core::kMaxDeploymentApplicabilityRows) {
    fail_retained_bundle(
        "control.bindings must be non-empty and within its fixed bound");
  }
  std::map<std::string, BindingFacts, std::less<>> result;
  std::string previous;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path = "control.bindings[" + std::to_string(index) + "]";
    const auto &row = require_object(rows[index], path);
    require_exact_fields(
        row,
        {"control_binding_id", "control_binding_revision",
         "controllable_component_id", "command_target_id", "capability_id",
         "capability_definition_revision", "control_interface_contract_id",
         "control_interface_revision", "provider_instance_id",
         "provider_runtime_unit_id", "provider_manifest_id",
         "provider_manifest_revision", "state_authority_id",
         "state_authority_class", "stop_evidence_policy_id",
         "stop_evidence_policy_revision", "containment_granularity",
         "command_claims", "state_claims"},
        path);
    const std::string id =
        require_identity_member(row, "control_binding_id", path);
    if (!previous.empty() && previous >= id) {
      fail_retained_bundle(
          "control bindings must be strictly sorted by binding ID");
    }
    previous = id;
    BindingFacts facts;
    facts.revision =
        require_digest_member(row, "control_binding_revision", path);
    facts.command_claims =
        verify_claims(require_member(row, "command_claims", path),
                      path + ".command_claims", true);
    facts.state_claims = verify_claims(
        require_member(row, "state_claims", path), path + ".state_claims");
    for (const auto field :
         {"controllable_component_id", "command_target_id", "capability_id",
          "control_interface_contract_id", "provider_instance_id",
          "provider_manifest_id", "provider_runtime_unit_id",
          "state_authority_id", "stop_evidence_policy_id"}) {
      static_cast<void>(require_identity_member(row, field, path));
    }
    for (const auto field :
         {"capability_definition_revision", "control_interface_revision",
          "provider_manifest_revision", "stop_evidence_policy_revision"}) {
      static_cast<void>(require_digest_member(row, field, path));
    }
    const std::string &authority =
        require_string(require_member(row, "state_authority_class", path),
                       path + ".state_authority_class");
    if (authority != "measured" && authority != "commanded" &&
        authority != "predicted") {
      fail_retained_bundle(path + " has an unknown state-authority class");
    }
    const std::string &containment =
        require_string(require_member(row, "containment_granularity", path),
                       path + ".containment_granularity");
    if (containment != "resource" && containment != "controller_set" &&
        containment != "control_domain") {
      fail_retained_bundle(path + " has unknown containment granularity");
    }
    facts.component_id =
        require_identity_member(row, "controllable_component_id", path);
    facts.target_id = require_identity_member(row, "command_target_id", path);
    facts.interface_id =
        require_identity_member(row, "control_interface_contract_id", path);
    facts.interface_revision =
        require_digest_member(row, "control_interface_revision", path);
    facts.stop_policy_id =
        require_identity_member(row, "stop_evidence_policy_id", path);
    facts.stop_policy_revision =
        require_digest_member(row, "stop_evidence_policy_revision", path);
    facts.containment = containment;
    facts.capability_id = require_identity_member(row, "capability_id", path);
    facts.capability_revision =
        require_digest_member(row, "capability_definition_revision", path);
    facts.provider_instance_id =
        require_identity_member(row, "provider_instance_id", path);
    facts.provider_runtime_unit_id =
        require_identity_member(row, "provider_runtime_unit_id", path);
    facts.provider_manifest_id =
        require_identity_member(row, "provider_manifest_id", path);
    facts.provider_manifest_revision =
        require_digest_member(row, "provider_manifest_revision", path);
    facts.state_authority_id =
        require_identity_member(row, "state_authority_id", path);
    facts.state_authority_class = authority;
    for (const auto &claim : facts.command_claims) {
      facts.resource_ids.insert(claim.resource_id);
    }
    for (const auto &claim : facts.state_claims) {
      facts.resource_ids.insert(claim.resource_id);
    }
    if (!result.emplace(id, std::move(facts)).second) {
      fail_retained_bundle(path + " duplicates a control binding");
    }
  }
  return result;
}

[[nodiscard]] std::set<std::string>
verify_directions(const CanonicalValue &value, std::string_view path) {
  const auto &rows = require_array(value, path);
  if (rows.size() > 2U) {
    fail_retained_bundle(std::string(path) + " has too many directions");
  }
  std::set<std::string> result;
  std::string previous;
  for (const auto &row : rows) {
    const std::string &direction = require_string(row, path);
    if (direction != "command" && direction != "state") {
      fail_retained_bundle(std::string(path) +
                           " contains an unknown direction");
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

[[nodiscard]] std::map<std::string, ControllerFacts, std::less<>>
verify_controllers(
    const CanonicalValue &value,
    const std::map<std::string, BindingFacts, std::less<>> &bindings) {
  const auto &rows = require_array(value, "control.controllers");
  if (rows.empty() || rows.size() > 2U) {
    fail_retained_bundle(
        "reference control profile requires one or two controllers");
  }
  std::map<std::string, ControllerFacts, std::less<>> result;
  std::string previous;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path =
        "control.controllers[" + std::to_string(index) + "]";
    const auto &row = require_object(rows[index], path);
    require_exact_fields(row,
                         {"controller_role_id", "logical_controller_symbol",
                          "generated_controller_name", "implementation",
                          "configuration_digest", "binding_projections"},
                         path);
    const std::string role =
        require_identity_member(row, "controller_role_id", path);
    if (!previous.empty() && previous >= role) {
      fail_retained_bundle(
          "control controllers must be strictly sorted by role ID");
    }
    previous = role;
    const std::string logical =
        require_identity_member(row, "logical_controller_symbol", path);
    if (require_identity_member(row, "generated_controller_name", path) !=
        logical) {
      fail_retained_bundle(
          path + " generated controller name differs from its exact symbol");
    }

    const auto &implementation = require_object(
        require_member(row, "implementation", path), path + ".implementation");
    require_exact_fields(
        implementation,
        {"implementation_id", "implementation_revision", "implementation_type"},
        path + ".implementation");
    const std::string id = require_identity_member(
        implementation, "implementation_id", path + ".implementation");
    const auto descriptor =
        std::find_if(kImplementations.begin(), kImplementations.end(),
                     [&](const auto &candidate) {
                       return candidate.implementation_id == id;
                     });
    if (descriptor == kImplementations.end() ||
        require_digest_member(implementation, "implementation_revision",
                              path + ".implementation") !=
            implementation_revision(*descriptor) ||
        require_string(require_member(implementation, "implementation_type",
                                      path + ".implementation"),
                       path + ".implementation.implementation_type") !=
            descriptor->implementation_type) {
      fail_retained_bundle(path +
                           " selects an unqualified controller implementation");
    }
    const std::string configuration_digest =
        require_digest_member(row, "configuration_digest", path);
    const std::string expected_configuration_digest = compute_content_identity(
        rows[index],
        ContentIdentitySpec{
            .identity_domain = "control-controller-configuration-digest",
            .api_version =
                std::string(kControlDomainControlApiVersion) + "#controller",
            .own_field = "configuration_digest",
            .root_shape = ContentIdentityRootShape::kNestedRecord,
        });
    if (configuration_digest != expected_configuration_digest) {
      fail_retained_bundle(path + " has an invalid configuration digest");
    }

    ControllerFacts facts{logical, id, {}};
    const auto &projections =
        require_array(require_member(row, "binding_projections", path),
                      path + ".binding_projections");
    if (projections.empty()) {
      fail_retained_bundle(path + " has no binding projection");
    }
    std::tuple<std::string, std::string, std::string> previous_projection;
    bool has_previous = false;
    for (std::size_t projection_index = 0U;
         projection_index < projections.size(); ++projection_index) {
      const std::string projection_path = path + ".binding_projections[" +
                                          std::to_string(projection_index) +
                                          "]";
      const auto &projection =
          require_object(projections[projection_index], projection_path);
      require_exact_fields(
          projection,
          {"control_binding_id", "control_binding_revision", "control_route_id",
           "activation_set_id", "claimed_directions",
           "claimed_command_interfaces", "claimed_state_interfaces",
           "exported_reference_interfaces", "exported_state_interfaces"},
          projection_path);
      const std::string binding_id = require_identity_member(
          projection, "control_binding_id", projection_path);
      const auto binding = bindings.find(binding_id);
      if (binding == bindings.end() ||
          require_digest_member(projection, "control_binding_revision",
                                projection_path) != binding->second.revision) {
        fail_retained_bundle(projection_path +
                             " selects an unknown binding revision");
      }
      const std::string route_id = require_identity_member(
          projection, "control_route_id", projection_path);
      const std::string activation_id = require_identity_member(
          projection, "activation_set_id", projection_path);
      const auto key = std::make_tuple(binding_id, route_id, activation_id);
      if (has_previous && previous_projection >= key) {
        fail_retained_bundle(projection_path + " is not canonical and unique");
      }
      previous_projection = key;
      has_previous = true;
      const auto directions = verify_directions(
          require_member(projection, "claimed_directions", projection_path),
          projection_path + ".claimed_directions");
      const auto command =
          verify_claims(require_member(projection, "claimed_command_interfaces",
                                       projection_path),
                        projection_path + ".claimed_command_interfaces");
      const auto state =
          verify_claims(require_member(projection, "claimed_state_interfaces",
                                       projection_path),
                        projection_path + ".claimed_state_interfaces");
      if ((directions.contains("command")
               ? command != binding->second.command_claims
               : !command.empty()) ||
          (directions.contains("state") ? state != binding->second.state_claims
                                        : !state.empty())) {
        fail_retained_bundle(
            projection_path +
            " does not conform exactly to the binding interface claims");
      }
      auto exported_reference = verify_claims(
          require_member(projection, "exported_reference_interfaces",
                         projection_path),
          projection_path + ".exported_reference_interfaces");
      auto exported_state =
          verify_claims(require_member(projection, "exported_state_interfaces",
                                       projection_path),
                        projection_path + ".exported_state_interfaces");
      if (!facts.projections
               .emplace(key, ProjectionFacts{directions,
                                             std::move(exported_reference),
                                             std::move(exported_state)})
               .second) {
        fail_retained_bundle(projection_path +
                             " duplicates a controller projection");
      }
    }

    bool has_claim = false;
    bool has_export = false;
    for (const auto &[unused, projection] : facts.projections) {
      static_cast<void>(unused);
      has_claim |= !projection.claimed_directions.empty();
      has_export |= !projection.exported_reference_interfaces.empty() ||
                    !projection.exported_state_interfaces.empty();
    }
    const std::string_view expected_implementation =
        has_export
            ? "manyforge.ros2_control.admittance_controller"
            : (has_claim
                   ? "manyforge.ros2_control.joint_trajectory_controller.direct"
                   : "manyforge.ros2_control.joint_trajectory_controller."
                     "chainable");
    if ((has_export && !has_claim) ||
        facts.implementation_id != expected_implementation) {
      fail_retained_bundle(
          path +
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

  friend bool operator<(const EdgeRef &left, const EdgeRef &right) {
    return std::tie(left.upstream, left.downstream) <
           std::tie(right.upstream, right.downstream);
  }
  friend bool operator==(const EdgeRef &, const EdgeRef &) = default;
};

[[nodiscard]] std::vector<std::string>
verify_identity_set(const CanonicalValue &value, std::string_view path,
                    bool require_non_empty = false) {
  const auto &rows = require_array(value, path);
  if (require_non_empty && rows.empty()) {
    fail_retained_bundle(std::string(path) + " must be non-empty");
  }
  std::vector<std::string> result;
  result.reserve(rows.size());
  std::string previous;
  for (const auto &row : rows) {
    const std::string &identity = require_string(row, path);
    require_retained_portable_identity(identity, path);
    if (!previous.empty() && previous >= identity) {
      fail_retained_bundle(std::string(path) + " is not canonical and unique");
    }
    previous = identity;
    result.push_back(identity);
  }
  return result;
}

[[nodiscard]] std::vector<std::string>
verify_identity_sequence(const CanonicalValue &value, std::string_view path,
                         bool require_non_empty = false) {
  const auto &rows = require_array(value, path);
  if (require_non_empty && rows.empty()) {
    fail_retained_bundle(std::string(path) + " must be non-empty");
  }
  std::vector<std::string> result;
  result.reserve(rows.size());
  std::set<std::string, std::less<>> unique;
  for (const auto &row : rows) {
    const std::string &identity = require_string(row, path);
    require_retained_portable_identity(identity, path);
    if (!unique.insert(identity).second) {
      fail_retained_bundle(std::string(path) +
                           " contains a duplicate ordered identity");
    }
    result.push_back(identity);
  }
  return result;
}

[[nodiscard]] EdgeRef verify_edge_ref(const CanonicalValue &value,
                                      std::string_view path) {
  const auto &row = require_object(value, path);
  require_exact_fields(
      row, {"upstream_controller_role_id", "downstream_controller_role_id"},
      path);
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
  std::set<std::string, std::less<>> roles;
  std::set<EdgeRef> edges;
};

[[nodiscard]] std::map<std::string, ActivationFacts, std::less<>>
verify_activation_sets(
    const CanonicalValue &value,
    const std::map<std::string, ControllerFacts, std::less<>> &controllers) {
  const auto &rows = require_array(value, "control.activation_sets");
  if (rows.empty() ||
      rows.size() > core::kMaxControllerTopologyActivationSets) {
    fail_retained_bundle(
        "control.activation_sets is empty or exceeds its bound");
  }
  std::map<std::string, ActivationFacts, std::less<>> result;
  std::string previous;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path =
        "control.activation_sets[" + std::to_string(index) + "]";
    const auto &row = require_object(rows[index], path);
    require_exact_fields(row,
                         {"activation_set_id", "active_controller_role_ids",
                          "active_chain_edges"},
                         path);
    const std::string id =
        require_identity_member(row, "activation_set_id", path);
    if (!previous.empty() && previous >= id) {
      fail_retained_bundle(
          "control activation sets are not canonical and unique");
    }
    previous = id;
    ActivationFacts facts;
    for (const auto &role : verify_identity_set(
             require_member(row, "active_controller_role_ids", path),
             path + ".active_controller_role_ids", true)) {
      if (!controllers.contains(role)) {
        fail_retained_bundle(path + " names an unknown controller role");
      }
      facts.roles.insert(role);
    }
    const auto &edges =
        require_array(require_member(row, "active_chain_edges", path),
                      path + ".active_chain_edges");
    EdgeRef previous_edge;
    bool has_previous = false;
    for (std::size_t edge_index = 0U; edge_index < edges.size(); ++edge_index) {
      const EdgeRef edge = verify_edge_ref(
          edges[edge_index],
          path + ".active_chain_edges[" + std::to_string(edge_index) + "]");
      if (!facts.roles.contains(edge.upstream) ||
          !facts.roles.contains(edge.downstream) ||
          (has_previous && !(previous_edge < edge)) ||
          !facts.edges.insert(edge).second) {
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
  struct Endpoint final {
    std::string logical_symbol;
    bool is_command = false;
    bool is_state = false;
  };

  std::string binding_id;
  std::string binding_revision;
  std::set<std::string> activation_sets;
  std::vector<std::string> controller_roles;
  std::set<EdgeRef> path_edges;
  std::map<std::string, Endpoint, std::less<>> endpoints;
};

[[nodiscard]] bool is_ordered_subsequence(std::span<const ClaimKey> selected,
                                          std::span<const ClaimKey> available) {
  std::size_t cursor = 0U;
  for (const auto &claim : selected) {
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
    const CanonicalValue &value,
    const std::map<std::string, BindingFacts, std::less<>> &bindings,
    const std::map<std::string, ControllerFacts, std::less<>> &controllers,
    const std::map<std::string, ActivationFacts, std::less<>> &activations) {
  const auto &rows = require_array(value, "control.routes");
  if (rows.empty() || rows.size() > core::kMaxDeploymentApplicabilityRows) {
    fail_retained_bundle("control.routes is empty or exceeds its bound");
  }
  std::map<std::string, RouteFacts, std::less<>> result;
  std::tuple<std::string, std::string> previous;
  bool has_previous = false;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path = "control.routes[" + std::to_string(index) + "]";
    const auto &row = require_object(rows[index], path);
    require_exact_fields(row,
                         {"control_binding_id", "control_binding_revision",
                          "control_route_id", "logical_route_symbol",
                          "controller_role_ids", "logical_controller_symbols",
                          "path_edges", "compatible_activation_set_ids",
                          "endpoints"},
                         path);
    const std::string binding_id =
        require_identity_member(row, "control_binding_id", path);
    const auto binding = bindings.find(binding_id);
    const std::string binding_revision =
        require_digest_member(row, "control_binding_revision", path);
    if (binding == bindings.end() ||
        binding->second.revision != binding_revision) {
      fail_retained_bundle(path + " selects an unknown binding revision");
    }
    const std::string route_id =
        require_identity_member(row, "control_route_id", path);
    const auto key = std::make_tuple(binding_id, route_id);
    if (has_previous && previous >= key) {
      fail_retained_bundle("control routes are not canonical and unique");
    }
    previous = key;
    has_previous = true;
    static_cast<void>(
        require_identity_member(row, "logical_route_symbol", path));

    const auto roles = verify_identity_sequence(
        require_member(row, "controller_role_ids", path),
        path + ".controller_role_ids", true);
    const auto symbols = verify_identity_sequence(
        require_member(row, "logical_controller_symbols", path),
        path + ".logical_controller_symbols", true);
    if (roles.size() != symbols.size()) {
      fail_retained_bundle(path +
                           " controller roles and symbols differ in size");
    }
    for (std::size_t role_index = 0U; role_index < roles.size(); ++role_index) {
      const auto controller = controllers.find(roles[role_index]);
      if (controller == controllers.end() ||
          controller->second.logical_symbol != symbols[role_index]) {
        fail_retained_bundle(path +
                             " does not map an exact controller role/symbol");
      }
    }

    RouteFacts facts{binding_id, binding_revision, {}, roles, {}, {}};
    const auto compatible_activations = verify_identity_set(
        require_member(row, "compatible_activation_set_ids", path),
        path + ".compatible_activation_set_ids", true);
    for (const auto &activation : compatible_activations) {
      const auto found = activations.find(activation);
      if (found == activations.end() ||
          !std::all_of(roles.begin(), roles.end(), [&](const auto &role) {
            return found->second.roles.contains(role);
          })) {
        fail_retained_bundle(path + " selects an incompatible activation set");
      }
      for (const auto &role : roles) {
        const auto controller = controllers.find(role);
        if (controller == controllers.end() ||
            !controller->second.projections.contains(
                ProjectionKey{binding_id, route_id, activation})) {
          fail_retained_bundle(
              path + " has no exact controller projection for its compatible "
                     "activation set");
        }
      }
      facts.activation_sets.insert(activation);
    }

    const auto &edge_rows = require_array(
        require_member(row, "path_edges", path), path + ".path_edges");
    if (edge_rows.size() + 1U != roles.size()) {
      fail_retained_bundle(path +
                           " path-edge count does not join its role path");
    }
    std::set<EdgeRef> path_edges;
    for (std::size_t edge_index = 0U; edge_index < edge_rows.size();
         ++edge_index) {
      const std::string edge_path =
          path + ".path_edges[" + std::to_string(edge_index) + "]";
      const auto &edge = require_object(edge_rows[edge_index], edge_path);
      require_exact_fields(edge,
                           {"upstream_controller_role_id",
                            "upstream_logical_controller_symbol",
                            "downstream_controller_role_id",
                            "downstream_logical_controller_symbol",
                            "reference_interfaces", "state_interfaces"},
                           edge_path);
      const std::string upstream = require_identity_member(
          edge, "upstream_controller_role_id", edge_path);
      const std::string downstream = require_identity_member(
          edge, "downstream_controller_role_id", edge_path);
      if (upstream != roles[edge_index] ||
          downstream != roles[edge_index + 1U] ||
          require_identity_member(edge, "upstream_logical_controller_symbol",
                                  edge_path) != symbols[edge_index] ||
          require_identity_member(edge, "downstream_logical_controller_symbol",
                                  edge_path) != symbols[edge_index + 1U] ||
          !path_edges.insert({upstream, downstream}).second) {
        fail_retained_bundle(edge_path +
                             " does not match the ordered controller path");
      }
      const auto reference_interfaces =
          verify_claims(require_member(edge, "reference_interfaces", edge_path),
                        edge_path + ".reference_interfaces", true);
      const auto state_interfaces =
          verify_claims(require_member(edge, "state_interfaces", edge_path),
                        edge_path + ".state_interfaces");
      const auto &downstream_facts = controllers.at(downstream);
      for (const auto &activation : compatible_activations) {
        const auto projection = downstream_facts.projections.find(
            ProjectionKey{binding_id, route_id, activation});
        if (projection == downstream_facts.projections.end() ||
            !is_ordered_subsequence(
                reference_interfaces,
                projection->second.exported_reference_interfaces) ||
            !is_ordered_subsequence(
                state_interfaces,
                projection->second.exported_state_interfaces)) {
          fail_retained_bundle(
              edge_path +
              " does not conform to the downstream exported interfaces");
        }
      }
    }

    for (const auto &activation : compatible_activations) {
      const auto &found = activations.at(activation);
      if (!std::all_of(
              path_edges.begin(), path_edges.end(),
              [&](const auto &edge) { return found.edges.contains(edge); })) {
        fail_retained_bundle(path + " selects an incompatible activation set");
      }
    }
    facts.path_edges = path_edges;

    const auto &endpoints = require_array(
        require_member(row, "endpoints", path), path + ".endpoints");
    if (endpoints.empty() ||
        endpoints.size() > core::kMaxCapabilityProviderEndpointBindings) {
      fail_retained_bundle(path + " has an invalid endpoint-role inventory");
    }
    std::string previous_endpoint;
    for (std::size_t endpoint_index = 0U; endpoint_index < endpoints.size();
         ++endpoint_index) {
      const std::string endpoint_path =
          path + ".endpoints[" + std::to_string(endpoint_index) + "]";
      const auto &endpoint =
          require_object(endpoints[endpoint_index], endpoint_path);
      require_exact_fields(endpoint,
                           {"endpoint_role_id", "logical_endpoint_symbol",
                            "generated_endpoint_name", "is_command_endpoint",
                            "is_state_endpoint"},
                           endpoint_path);
      const std::string endpoint_id =
          require_identity_member(endpoint, "endpoint_role_id", endpoint_path);
      if (!previous_endpoint.empty() && previous_endpoint >= endpoint_id) {
        fail_retained_bundle(endpoint_path + " is not canonical and unique");
      }
      previous_endpoint = endpoint_id;
      const std::string logical = require_identity_member(
          endpoint, "logical_endpoint_symbol", endpoint_path);
      const bool is_command = require_bool(
          require_member(endpoint, "is_command_endpoint", endpoint_path),
          endpoint_path + ".is_command_endpoint");
      const bool is_state = require_bool(
          require_member(endpoint, "is_state_endpoint", endpoint_path),
          endpoint_path + ".is_state_endpoint");
      if (require_identity_member(endpoint, "generated_endpoint_name",
                                  endpoint_path) != logical ||
          (!is_command && !is_state)) {
        fail_retained_bundle(endpoint_path +
                             " has an invalid generated endpoint mapping");
      }
      facts.endpoints.emplace(
          endpoint_id, RouteFacts::Endpoint{logical, is_command, is_state});
    }
    const std::string semantic_key = binding_id + "\x1f" + route_id;
    if (!result.emplace(semantic_key, std::move(facts)).second) {
      fail_retained_bundle(path + " duplicates a binding route");
    }
  }
  return result;
}

struct ContractFacts final {
  std::string role_id;
  std::string schema_id;
  std::string schema_revision;

  friend bool operator==(const ContractFacts &,
                         const ContractFacts &) = default;
};

[[nodiscard]] ContractFacts
verify_provider_contract(const CanonicalValue &value, std::string_view path) {
  const auto &row = require_object(value, path);
  require_exact_fields(row,
                       {"provider_contract_role_id", "contract_schema_id",
                        "contract_schema_revision"},
                       path);
  return {
      require_identity_member(row, "provider_contract_role_id", path),
      require_identity_member(row, "contract_schema_id", path),
      require_digest_member(row, "contract_schema_revision", path),
  };
}

[[nodiscard]] std::optional<ContractFacts>
verify_optional_provider_contract(const CanonicalValue &value,
                                  std::string_view path) {
  if (is_null(value)) {
    return std::nullopt;
  }
  return verify_provider_contract(value, path);
}

[[nodiscard]] std::optional<std::string>
verify_optional_identity(const CanonicalValue &value, std::string_view path) {
  if (is_null(value)) {
    return std::nullopt;
  }
  const std::string &identity = require_string(value, path);
  require_retained_portable_identity(identity, path);
  return identity;
}

[[nodiscard]] bool is_one_of(std::string_view value,
                             std::initializer_list<std::string_view> allowed) {
  return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

struct StopEvidenceFacts final {
  std::string source_kind;
  std::optional<std::string> endpoint_role_id;
  std::optional<ContractFacts> provider_contract;
  std::vector<std::string> allowed_authority_classes;
};

void verify_stop_applicability(
    const CanonicalValue &value, std::string_view domain_id,
    std::string_view domain_revision, std::string_view topology_id,
    std::string_view topology_revision,
    const std::map<std::string, BindingFacts, std::less<>> &bindings,
    const std::map<std::string, ControllerFacts, std::less<>> &controllers,
    const std::map<std::string, ActivationFacts, std::less<>> &activations,
    const std::map<std::string, RouteFacts, std::less<>> &routes,
    const core::CellGraphDTO *source_graph,
    std::span<const CompositionControlBindingCoordinateMap> coordinate_maps) {
  const auto &rows = require_array(value, "control.stop_applicability");
  if (rows.size() != routes.size() ||
      rows.size() > core::kMaxDeploymentApplicabilityRows) {
    fail_retained_bundle(
        "control.stop_applicability must contain one bounded row per route");
  }
  std::set<std::string, std::less<>> seen_routes;
  std::pair<std::string, std::string> previous_key;
  bool has_previous = false;

  constexpr std::array<std::string_view, 11U> common_fields{
      "control_binding_id",
      "control_binding_revision",
      "control_route_id",
      "stop_evidence_policy_id",
      "stop_evidence_policy_revision",
      "control_domain_id",
      "control_domain_revision",
      "controller_topology_id",
      "topology_revision",
      "containment_granularity",
      "disposition"};

  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path =
        "control.stop_applicability[" + std::to_string(index) + "]";
    const auto &row = require_object(rows[index], path);
    const std::string &disposition = require_string(
        require_member(row, "disposition", path), path + ".disposition");
    if (disposition == "not_stoppable") {
      if (row.size() != common_fields.size()) {
        fail_retained_bundle(path +
                             " has an incorrect closed not-stoppable shape");
      }
      for (const auto field : common_fields) {
        static_cast<void>(require_member(row, field, path));
      }
      for (const auto &[name, unused] : row) {
        static_cast<void>(unused);
        if (std::find(common_fields.begin(), common_fields.end(), name) ==
            common_fields.end()) {
          fail_retained_bundle(path + " contains unknown member " + name);
        }
      }
    } else if (disposition == "stoppable") {
      require_exact_fields(
          row,
          {"control_binding_id", "control_binding_revision", "control_route_id",
           "stop_evidence_policy_id", "stop_evidence_policy_revision",
           "control_domain_id", "control_domain_revision",
           "controller_topology_id", "topology_revision",
           "containment_granularity", "disposition", "logical_stop_symbol",
           "escalation_stages"},
          path);
    } else {
      fail_retained_bundle(path + " has an unknown Stop disposition");
    }

    const std::string binding_id =
        require_identity_member(row, "control_binding_id", path);
    const std::string binding_revision =
        require_digest_member(row, "control_binding_revision", path);
    const std::string route_id =
        require_identity_member(row, "control_route_id", path);
    const auto key = std::pair{binding_id, route_id};
    if (has_previous && previous_key >= key) {
      fail_retained_bundle(
          "control.stop_applicability is not canonical and unique");
    }
    previous_key = key;
    has_previous = true;
    const std::string semantic_key = binding_id + "\x1f" + route_id;
    const auto binding = bindings.find(binding_id);
    const auto route = routes.find(semantic_key);
    if (binding == bindings.end() || route == routes.end() ||
        binding->second.revision != binding_revision ||
        route->second.binding_revision != binding_revision ||
        !seen_routes.insert(semantic_key).second) {
      fail_retained_bundle(path + " does not select one exact binding route");
    }
    if (require_identity_member(row, "stop_evidence_policy_id", path) !=
            binding->second.stop_policy_id ||
        require_digest_member(row, "stop_evidence_policy_revision", path) !=
            binding->second.stop_policy_revision ||
        require_identity_member(row, "control_domain_id", path) != domain_id ||
        require_digest_member(row, "control_domain_revision", path) !=
            domain_revision ||
        require_identity_member(row, "controller_topology_id", path) !=
            topology_id ||
        require_digest_member(row, "topology_revision", path) !=
            topology_revision ||
        require_string(require_member(row, "containment_granularity", path),
                       path + ".containment_granularity") !=
            binding->second.containment) {
      fail_retained_bundle(path +
                           " differs from its exact domain/binding subject");
    }
    std::optional<core::GripperActuationKind> actuation;
    if (source_graph != nullptr) {
      const auto source = std::find_if(
          source_graph->control_bindings.begin(),
          source_graph->control_bindings.end(), [&](const auto &candidate) {
            return candidate.control_binding_id.value() == binding_id;
          });
      if (source == source_graph->control_bindings.end() ||
          std::count_if(
              source_graph->control_bindings.begin(),
              source_graph->control_bindings.end(), [&](const auto &candidate) {
                return candidate.control_binding_id.value() == binding_id;
              }) != 1)
        fail_retained_bundle(path + " has no unique source graph binding");
      const auto domain = std::find_if(
          source_graph->control_domains.begin(),
          source_graph->control_domains.end(), [&](const auto &candidate) {
            return candidate.control_domain_id.value() == domain_id;
          });
      if (domain == source_graph->control_domains.end() ||
          std::count_if(source_graph->control_domains.begin(),
                        source_graph->control_domains.end(),
                        [&](const auto &candidate) {
                          return candidate.control_domain_id.value() ==
                                 domain_id;
                        }) != 1 ||
          domain->control_domain_revision.value() != domain_revision ||
          domain->controller_topology_id.value() != topology_id ||
          domain->topology_revision.value() != topology_revision ||
          std::find(domain->runtime_unit_ids.begin(),
                    domain->runtime_unit_ids.end(),
                    source->provider_runtime_unit_id) ==
              domain->runtime_unit_ids.end() ||
          std::none_of(source->legal_control_route_ids.begin(),
                       source->legal_control_route_ids.end(),
                       [&](const auto &id) { return id.value() == route_id; }))
        fail_retained_bundle(path +
                             " differs from its exact source domain/route");
      const auto &facts = binding->second;
      std::set<std::string, std::less<>> resources;
      for (const auto &resource : source->resource_bindings)
        resources.insert(resource.resource_id.value());
      if (source->control_binding_revision.value() != facts.revision ||
          source->controllable_component_id.value() != facts.component_id ||
          source->command_target_id.value() != facts.target_id ||
          source->control_interface_contract_id.value() != facts.interface_id ||
          source->control_interface_revision.value() !=
              facts.interface_revision ||
          source->control_domain_id.value() != domain_id ||
          source->controller_topology_id.value() != topology_id ||
          source->topology_revision.value() != topology_revision ||
          source->capability_id.value() != facts.capability_id ||
          source->capability_definition_revision.value() !=
              facts.capability_revision ||
          source->provider_instance_id.value() != facts.provider_instance_id ||
          source->provider_runtime_unit_id.value() !=
              facts.provider_runtime_unit_id ||
          source->provider_manifest_id.value() != facts.provider_manifest_id ||
          source->provider_manifest_revision.value() !=
              facts.provider_manifest_revision ||
          source->state_authority_id.value() != facts.state_authority_id ||
          core::to_string(source->state_authority_class) !=
              facts.state_authority_class ||
          source->stop_evidence_policy_id.value() != facts.stop_policy_id ||
          source->stop_evidence_policy_revision.value() !=
              facts.stop_policy_revision ||
          core::to_string(source->containment_granularity) !=
              facts.containment ||
          resources != facts.resource_ids)
        fail_retained_bundle(path +
                             " differs from its exact source graph binding");
      actuation = classify_ros_gripper_actuation_kind(*source_graph, *source,
                                                      coordinate_maps);
    }
    if (disposition == "not_stoppable") {
      continue;
    }

    static_cast<void>(
        require_identity_member(row, "logical_stop_symbol", path));
    const auto &stop_row = row;
    const std::string stop_path = path;
    const auto &stage_rows =
        require_array(require_member(stop_row, "escalation_stages", stop_path),
                      stop_path + ".escalation_stages");
    if (stage_rows.empty() ||
        stage_rows.size() > core::kMaxStopEvidencePolicyItems) {
      fail_retained_bundle(stop_path +
                           " has an invalid Stop escalation inventory");
    }
    int previous_granularity = -1;
    std::size_t software_hard_count = 0U;
    bool saw_safety_request = false;
    std::set<std::string, std::less<>> stage_ids;
    std::set<std::string, std::less<>> all_evidence_ids;
    for (std::size_t stage_index = 0U; stage_index < stage_rows.size();
         ++stage_index) {
      const std::string path =
          stop_path + ".escalation_stages[" + std::to_string(stage_index) + "]";
      const auto &row = require_object(stage_rows[stage_index], path);
      require_exact_fields(
          row,
          {"stop_escalation_stage_id", "stage_kind", "containment_granularity",
           "containment_command", "evidence_conjunction",
           "all_of_evidence_requirements", "default_contract_parameters",
           "commissioned_stop_interface", "provider_command_endpoint",
           "provider_evidence_endpoints", "binding_state_authority_sources",
           "containment_scopes"},
          path);
      const std::string stage_id =
          require_identity_member(row, "stop_escalation_stage_id", path);
      if (!stage_ids.insert(stage_id).second) {
        fail_retained_bundle(path + " duplicates a Stop escalation stage ID");
      }
      const std::string &stage_kind = require_string(
          require_member(row, "stage_kind", path), path + ".stage_kind");
      const std::string &stage_containment =
          require_string(require_member(row, "containment_granularity", path),
                         path + ".containment_granularity");
      const int stage_granularity = stage_containment == "resource"         ? 0
                                    : stage_containment == "controller_set" ? 1
                                    : stage_containment == "control_domain"
                                        ? 2
                                        : -1;
      if (stage_granularity < previous_granularity || stage_granularity < 0 ||
          saw_safety_request ||
          (stage_index == 0U && stage_kind != "nominal") ||
          (stage_index != 0U && stage_kind == "nominal")) {
        fail_retained_bundle(
            path + " has an invalid ordered Stop stage class or granularity");
      }
      previous_granularity = stage_granularity;
      if (stage_kind == "software_hard") {
        ++software_hard_count;
      } else if (stage_kind == "safety_system_request") {
        saw_safety_request = true;
        if (stage_index + 1U != stage_rows.size()) {
          fail_retained_bundle(path + " is a non-final safety-system request");
        }
      } else if (stage_kind != "nominal" &&
                 stage_kind != "controlled_fallback") {
        fail_retained_bundle(path + " has an unknown Stop stage kind");
      } else if (stage_kind == "controlled_fallback" &&
                 software_hard_count != 0U) {
        fail_retained_bundle(
            path +
            " places a controlled fallback after the software-hard floor");
      }
      const auto &command =
          require_object(require_member(row, "containment_command", path),
                         path + ".containment_command");
      require_exact_fields(command,
                           {"command_kind", "provider_contract_ref",
                            "registered_command_kind_id"},
                           path + ".containment_command");
      const std::string &command_kind =
          require_string(require_member(command, "command_kind",
                                        path + ".containment_command"),
                         path + ".containment_command.command_kind");
      if (!is_one_of(command_kind,
                     {"controller_goal_cancel", "motion_controlled_stop",
                      "capability_command_cancel", "safe_output_command",
                      "logical_cancel", "registered"})) {
        fail_retained_bundle(path + " has an unknown Stop command kind");
      }
      const auto command_contract = verify_optional_provider_contract(
          require_member(command, "provider_contract_ref",
                         path + ".containment_command"),
          path + ".containment_command.provider_contract_ref");
      const auto registered_command = verify_optional_identity(
          require_member(command, "registered_command_kind_id",
                         path + ".containment_command"),
          path + ".containment_command.registered_command_kind_id");
      if ((command_kind == "registered") != registered_command.has_value() ||
          (is_one_of(command_kind,
                     {"motion_controlled_stop", "safe_output_command",
                      "logical_cancel", "registered"}) !=
           command_contract.has_value())) {
        fail_retained_bundle(path + " has an incoherent Stop command shape");
      }
      const core::StopActionClass expected_action_class =
          stage_kind == "software_hard" ? core::StopActionClass::kSoftwareHard
          : stage_kind == "safety_system_request"
              ? core::StopActionClass::kSafetySystemRequest
              : core::StopActionClass::kControlled;
      core::StopActionClass actual_action_class =
          core::StopActionClass::kControlled;
      if (registered_command.has_value()) {
        const auto *definition = core::find_registered_stop_command_definition(
            core::RegisteredStopCommandKindId(*registered_command));
        if (definition == nullptr) {
          fail_retained_bundle(path +
                               " names an unknown registered Stop command");
        }
        actual_action_class = definition->stop_action_class;
      }
      if (actual_action_class != expected_action_class ||
          ((expected_action_class == core::StopActionClass::kSoftwareHard ||
            expected_action_class ==
                core::StopActionClass::kSafetySystemRequest) &&
           command_kind != "registered")) {
        fail_retained_bundle(
            path + " stage kind differs from its code-owned command class");
      }

      const CanonicalValue &parameters =
          require_member(row, "default_contract_parameters", path);
      if (command_contract.has_value()) {
        static_cast<void>(
            require_object(parameters, path + ".default_contract_parameters"));
      } else if (!is_null(parameters)) {
        fail_retained_bundle(path +
                             " retains parameters for an intrinsic command");
      }
      const CanonicalValue &commissioned =
          require_member(row, "commissioned_stop_interface", path);
      if ((stage_kind == "safety_system_request") == is_null(commissioned)) {
        fail_retained_bundle(path +
                             " has an invalid commissioned-interface presence");
      }
      if (!is_null(commissioned)) {
        const auto &commissioned_row =
            require_object(commissioned, path + ".commissioned_stop_interface");
        require_exact_fields(
            commissioned_row,
            {"commissioned_stop_interface_id",
             "commissioned_stop_interface_revision", "control_domain_id",
             "control_domain_revision", "control_binding_id",
             "control_binding_revision", "control_route_id",
             "provider_instance_id", "provider_manifest_id",
             "provider_manifest_revision", "registered_command_kind_id",
             "provider_contract_ref", "endpoint_role_id", "stop_action_class",
             "safety_system_request_action_class", "external_resource_alias",
             "external_resource_kind", "site_overlay_value_ref",
             "commissioning_evidence_artifact_ref"},
            path + ".commissioned_stop_interface");
        if (require_identity_member(commissioned_row, "control_domain_id",
                                    path) != domain_id ||
            require_digest_member(commissioned_row, "control_domain_revision",
                                  path) != domain_revision ||
            require_identity_member(commissioned_row, "control_binding_id",
                                    path) != binding_id ||
            require_digest_member(commissioned_row, "control_binding_revision",
                                  path) != binding_revision ||
            require_identity_member(commissioned_row, "control_route_id",
                                    path) != route_id ||
            require_string(
                require_member(commissioned_row, "stop_action_class", path),
                path + ".commissioned_stop_interface.stop_action_class") !=
                "safety_system_request") {
          fail_retained_bundle(
              path +
              " commissioned interface differs from its exact stage subject");
        }
      }

      if (require_string(require_member(row, "evidence_conjunction", path),
                         path + ".evidence_conjunction") != "all_of") {
        fail_retained_bundle(path + " has an unsupported evidence conjunction");
      }

      const auto &evidence_rows = require_array(
          require_member(row, "all_of_evidence_requirements", path),
          path + ".all_of_evidence_requirements");
      if (evidence_rows.empty() ||
          evidence_rows.size() > core::kMaxStopEvidencePolicyItems) {
        fail_retained_bundle(path + " has an invalid Stop evidence inventory");
      }
      std::map<std::string, StopEvidenceFacts, std::less<>> evidence_facts;
      for (std::size_t evidence_index = 0U;
           evidence_index < evidence_rows.size(); ++evidence_index) {
        const std::string evidence_path = path +
                                          ".all_of_evidence_requirements[" +
                                          std::to_string(evidence_index) + "]";
        const auto &evidence =
            require_object(evidence_rows[evidence_index], evidence_path);
        require_exact_fields(evidence,
                             {"evidence_item_id", "evidence_kind",
                              "provider_contract_ref",
                              "registered_evidence_kind_id",
                              "source_requirement", "observation_semantics"},
                             evidence_path);
        const std::string evidence_id = require_identity_member(
            evidence, "evidence_item_id", evidence_path);
        if (!all_evidence_ids.insert(evidence_id).second) {
          fail_retained_bundle(evidence_path +
                               " repeats an evidence item across Stop stages");
        }
        const std::string &evidence_kind = require_string(
            require_member(evidence, "evidence_kind", evidence_path),
            evidence_path + ".evidence_kind");
        if (!is_one_of(evidence_kind,
                       {"controller_goal_terminal", "motion_rest",
                        "discrete_acknowledgement", "io_safe_output",
                        "logical_cancellation_no_continuing_effect",
                        "registered"})) {
          fail_retained_bundle(evidence_path +
                               " has invalid ordered evidence semantics");
        }
        const auto evidence_contract = verify_optional_provider_contract(
            require_member(evidence, "provider_contract_ref", evidence_path),
            evidence_path + ".provider_contract_ref");
        const auto registered_evidence = verify_optional_identity(
            require_member(evidence, "registered_evidence_kind_id",
                           evidence_path),
            evidence_path + ".registered_evidence_kind_id");
        if ((evidence_kind == "registered") !=
                registered_evidence.has_value() ||
            (is_one_of(evidence_kind,
                       {"motion_rest", "io_safe_output",
                        "logical_cancellation_no_continuing_effect",
                        "registered"}) != evidence_contract.has_value())) {
          fail_retained_bundle(evidence_path +
                               " has an incoherent evidence kind shape");
        }
        const auto &source = require_object(
            require_member(evidence, "source_requirement", evidence_path),
            evidence_path + ".source_requirement");
        require_exact_fields(source,
                             {"source_kind", "freshness_budget_ns",
                              "endpoint_role_id", "allowed_authority_classes"},
                             evidence_path + ".source_requirement");
        const std::string &source_kind = require_string(
            require_member(source, "source_kind",
                           evidence_path + ".source_requirement"),
            evidence_path + ".source_requirement.source_kind");
        if (!is_one_of(source_kind,
                       {"controller_goal_status", "binding_state_authority",
                        "command_target_status", "provider_evidence"}) ||
            require_safe_integer(
                require_member(source, "freshness_budget_ns",
                               evidence_path + ".source_requirement"),
                evidence_path + ".source_requirement.freshness_budget_ns") <=
                0) {
          fail_retained_bundle(evidence_path +
                               " has an invalid evidence source");
        }
        const auto endpoint_role = verify_optional_identity(
            require_member(source, "endpoint_role_id",
                           evidence_path + ".source_requirement"),
            evidence_path + ".source_requirement.endpoint_role_id");
        const auto authority_classes = verify_identity_set(
            require_member(source, "allowed_authority_classes",
                           evidence_path + ".source_requirement"),
            evidence_path + ".source_requirement.allowed_authority_classes");
        if (!std::all_of(authority_classes.begin(), authority_classes.end(),
                         [](const auto &value) {
                           return is_one_of(
                               value, {"measured", "commanded", "predicted"});
                         }) ||
            ((source_kind == "binding_state_authority" ||
              source_kind == "provider_evidence") !=
             endpoint_role.has_value()) ||
            ((source_kind == "binding_state_authority") !=
             !authority_classes.empty())) {
          fail_retained_bundle(evidence_path +
                               " has an incoherent evidence source shape");
        }
        const auto &observation = require_object(
            require_member(evidence, "observation_semantics", evidence_path),
            evidence_path + ".observation_semantics");
        require_exact_fields(observation,
                             {"observation_kind", "minimum_dwell_ns"},
                             evidence_path + ".observation_semantics");
        const std::string &observation_kind = require_string(
            require_member(observation, "observation_kind",
                           evidence_path + ".observation_semantics"),
            evidence_path + ".observation_semantics.observation_kind");
        const CanonicalValue &dwell =
            require_member(observation, "minimum_dwell_ns",
                           evidence_path + ".observation_semantics");
        const bool compatible =
            (evidence_kind == "controller_goal_terminal" &&
             source_kind == "controller_goal_status" &&
             is_one_of(observation_kind,
                       {"command_result", "post_command_sample"})) ||
            (evidence_kind == "motion_rest" &&
             source_kind == "binding_state_authority" &&
             observation_kind == "post_command_dwell") ||
            (evidence_kind == "discrete_acknowledgement" &&
             source_kind == "command_target_status" &&
             is_one_of(observation_kind,
                       {"command_result", "post_command_sample"})) ||
            (evidence_kind == "io_safe_output" &&
             is_one_of(source_kind,
                       {"binding_state_authority", "provider_evidence"}) &&
             is_one_of(observation_kind,
                       {"post_command_sample", "post_command_dwell"})) ||
            (evidence_kind == "logical_cancellation_no_continuing_effect" &&
             is_one_of(source_kind,
                       {"command_target_status", "provider_evidence"})) ||
            (evidence_kind == "registered" &&
             source_kind == "provider_evidence");
        if (!compatible ||
            !is_one_of(observation_kind,
                       {"command_result", "post_command_sample",
                        "post_command_dwell"}) ||
            ((observation_kind == "post_command_dwell") != !is_null(dwell)) ||
            (!is_null(dwell) &&
             require_safe_integer(
                 dwell, evidence_path +
                            ".observation_semantics.minimum_dwell_ns") <= 0)) {
          fail_retained_bundle(evidence_path +
                               " has invalid observation semantics");
        }
        if (!evidence_facts
                 .emplace(evidence_id,
                          StopEvidenceFacts{source_kind, endpoint_role,
                                            evidence_contract,
                                            authority_classes})
                 .second) {
          fail_retained_bundle(evidence_path + " duplicates an evidence item");
        }
      }

      const CanonicalValue &command_endpoint_value =
          require_member(row, "provider_command_endpoint", path);
      if (is_null(command_endpoint_value)) {
        // A provider contract can deliberately be endpointless. Absence is
        // retained explicitly and remains distinct from an empty object.
      } else {
        const auto &endpoint = require_object(
            command_endpoint_value, path + ".provider_command_endpoint");
        require_exact_fields(endpoint,
                             {"stop_escalation_stage_id",
                              "provider_contract_ref", "endpoint_role_id",
                              "logical_endpoint_symbol"},
                             path + ".provider_command_endpoint");
        const ContractFacts contract = verify_provider_contract(
            require_member(endpoint, "provider_contract_ref",
                           path + ".provider_command_endpoint"),
            path + ".provider_command_endpoint.provider_contract_ref");
        const std::string endpoint_role = require_identity_member(
            endpoint, "endpoint_role_id", path + ".provider_command_endpoint");
        const auto route_endpoint = route->second.endpoints.find(endpoint_role);
        if (require_identity_member(endpoint, "stop_escalation_stage_id",
                                    path + ".provider_command_endpoint") !=
                stage_id ||
            !command_contract.has_value() || contract != *command_contract ||
            route_endpoint == route->second.endpoints.end() ||
            !route_endpoint->second.is_command ||
            require_identity_member(endpoint, "logical_endpoint_symbol",
                                    path + ".provider_command_endpoint") !=
                route_endpoint->second.logical_symbol) {
          fail_retained_bundle(
              path + " has an invalid resolved provider command endpoint");
        }
      }

      const auto &provider_rows = require_array(
          require_member(row, "provider_evidence_endpoints", path),
          path + ".provider_evidence_endpoints");
      std::set<std::string, std::less<>> provider_evidence_ids;
      std::string previous_evidence_id;
      for (std::size_t endpoint_index = 0U;
           endpoint_index < provider_rows.size(); ++endpoint_index) {
        const std::string endpoint_path = path +
                                          ".provider_evidence_endpoints[" +
                                          std::to_string(endpoint_index) + "]";
        const auto &endpoint =
            require_object(provider_rows[endpoint_index], endpoint_path);
        require_exact_fields(endpoint,
                             {"stop_escalation_stage_id", "evidence_item_id",
                              "provider_contract_ref", "endpoint_role_id",
                              "logical_endpoint_symbol"},
                             endpoint_path);
        const std::string evidence_id = require_identity_member(
            endpoint, "evidence_item_id", endpoint_path);
        const auto evidence = evidence_facts.find(evidence_id);
        const ContractFacts contract = verify_provider_contract(
            require_member(endpoint, "provider_contract_ref", endpoint_path),
            endpoint_path + ".provider_contract_ref");
        const std::string endpoint_role = require_identity_member(
            endpoint, "endpoint_role_id", endpoint_path);
        const auto route_endpoint = route->second.endpoints.find(endpoint_role);
        if (require_identity_member(endpoint, "stop_escalation_stage_id",
                                    endpoint_path) != stage_id ||
            (!previous_evidence_id.empty() &&
             previous_evidence_id >= evidence_id) ||
            evidence == evidence_facts.end() ||
            evidence->second.source_kind != "provider_evidence" ||
            evidence->second.endpoint_role_id != endpoint_role ||
            !evidence->second.provider_contract.has_value() ||
            contract != *evidence->second.provider_contract ||
            route_endpoint == route->second.endpoints.end() ||
            !route_endpoint->second.is_state ||
            require_identity_member(endpoint, "logical_endpoint_symbol",
                                    endpoint_path) !=
                route_endpoint->second.logical_symbol ||
            !provider_evidence_ids.insert(evidence_id).second) {
          fail_retained_bundle(endpoint_path +
                               " is not an exact provider evidence endpoint");
        }
        previous_evidence_id = evidence_id;
      }

      const auto &binding_rows = require_array(
          require_member(row, "binding_state_authority_sources", path),
          path + ".binding_state_authority_sources");
      std::set<std::string, std::less<>> binding_evidence_ids;
      previous_evidence_id.clear();
      for (std::size_t source_index = 0U; source_index < binding_rows.size();
           ++source_index) {
        const std::string source_path = path +
                                        ".binding_state_authority_sources[" +
                                        std::to_string(source_index) + "]";
        const auto &source =
            require_object(binding_rows[source_index], source_path);
        require_exact_fields(source,
                             {"stop_escalation_stage_id", "evidence_item_id",
                              "state_authority_id", "state_authority_class",
                              "state_endpoint_role_id",
                              "logical_endpoint_symbol"},
                             source_path);
        const std::string evidence_id =
            require_identity_member(source, "evidence_item_id", source_path);
        const auto evidence = evidence_facts.find(evidence_id);
        const std::string endpoint_role = require_identity_member(
            source, "state_endpoint_role_id", source_path);
        const auto route_endpoint = route->second.endpoints.find(endpoint_role);
        if (require_identity_member(source, "stop_escalation_stage_id",
                                    source_path) != stage_id ||
            (!previous_evidence_id.empty() &&
             previous_evidence_id >= evidence_id) ||
            evidence == evidence_facts.end() ||
            evidence->second.source_kind != "binding_state_authority" ||
            evidence->second.endpoint_role_id != endpoint_role ||
            std::find(evidence->second.allowed_authority_classes.begin(),
                      evidence->second.allowed_authority_classes.end(),
                      binding->second.state_authority_class) ==
                evidence->second.allowed_authority_classes.end() ||
            require_identity_member(source, "state_authority_id",
                                    source_path) !=
                binding->second.state_authority_id ||
            require_string(
                require_member(source, "state_authority_class", source_path),
                source_path + ".state_authority_class") !=
                binding->second.state_authority_class ||
            route_endpoint == route->second.endpoints.end() ||
            !route_endpoint->second.is_state ||
            require_identity_member(source, "logical_endpoint_symbol",
                                    source_path) !=
                route_endpoint->second.logical_symbol ||
            !binding_evidence_ids.insert(evidence_id).second) {
          fail_retained_bundle(source_path +
                               " is not an exact binding authority source");
        }
        previous_evidence_id = evidence_id;
      }
      for (const auto &[evidence_id, evidence] : evidence_facts) {
        if ((evidence.source_kind == "provider_evidence") !=
                provider_evidence_ids.contains(evidence_id) ||
            (evidence.source_kind == "binding_state_authority") !=
                binding_evidence_ids.contains(evidence_id)) {
          fail_retained_bundle(
              path + " does not resolve every endpoint-backed evidence item");
        }
      }

      const auto &scope_rows =
          require_array(require_member(row, "containment_scopes", path),
                        path + ".containment_scopes");
      if (scope_rows.size() != route->second.activation_sets.size()) {
        fail_retained_bundle(
            path + " scope inventory differs from compatible activations");
      }
      std::set<std::string, std::less<>> seen_activations;
      std::string previous_activation;
      for (std::size_t scope_index = 0U; scope_index < scope_rows.size();
           ++scope_index) {
        const std::string scope_path =
            path + ".containment_scopes[" + std::to_string(scope_index) + "]";
        const auto &scope = require_object(scope_rows[scope_index], scope_path);
        const std::string &scope_kind =
            require_string(require_member(scope, "scope_kind", scope_path),
                           scope_path + ".scope_kind");
        const std::string activation =
            require_identity_member(scope, "activation_set_id", scope_path);
        const auto activation_facts = activations.find(activation);
        if (require_identity_member(scope, "stop_escalation_stage_id",
                                    scope_path) != stage_id ||
            scope_kind != stage_containment ||
            !route->second.activation_sets.contains(activation) ||
            activation_facts == activations.end() ||
            (!previous_activation.empty() &&
             previous_activation >= activation) ||
            !seen_activations.insert(activation).second) {
          fail_retained_bundle(scope_path +
                               " is not an exact canonical containment scope");
        }
        previous_activation = activation;
        if (scope_kind == "resource") {
          require_exact_fields(scope,
                               {"stop_escalation_stage_id", "scope_kind",
                                "activation_set_id", "resource_ids"},
                               scope_path);
          const auto resources = verify_identity_set(
              require_member(scope, "resource_ids", scope_path),
              scope_path + ".resource_ids", true);
          if (std::set<std::string, std::less<>>(resources.begin(),
                                                 resources.end()) !=
              binding->second.resource_ids) {
            fail_retained_bundle(
                scope_path + " differs from the exact binding resource set");
          }
        } else if (scope_kind == "controller_set") {
          require_exact_fields(
              scope,
              {"stop_escalation_stage_id", "scope_kind", "activation_set_id",
               "active_controller_role_ids", "logical_controller_symbols"},
              scope_path);
          const auto roles = verify_identity_set(
              require_member(scope, "active_controller_role_ids", scope_path),
              scope_path + ".active_controller_role_ids", true);
          const auto symbols = verify_identity_sequence(
              require_member(scope, "logical_controller_symbols", scope_path),
              scope_path + ".logical_controller_symbols", true);
          if (roles.size() != symbols.size() ||
              std::set<std::string, std::less<>>(roles.begin(), roles.end()) !=
                  activation_facts->second.roles) {
            fail_retained_bundle(
                scope_path + " differs from the exact active controller set");
          }
          for (std::size_t role_index = 0U; role_index < roles.size();
               ++role_index) {
            const auto controller = controllers.find(roles[role_index]);
            if (controller == controllers.end() ||
                controller->second.logical_symbol != symbols[role_index]) {
              fail_retained_bundle(
                  scope_path +
                  " has an invalid controller role/symbol mapping");
            }
          }
        } else if (scope_kind == "control_domain") {
          require_exact_fields(scope,
                               {"stop_escalation_stage_id", "scope_kind",
                                "activation_set_id", "control_domain_id",
                                "control_domain_revision"},
                               scope_path);
          if (require_identity_member(scope, "control_domain_id", scope_path) !=
                  domain_id ||
              require_digest_member(scope, "control_domain_revision",
                                    scope_path) != domain_revision) {
            fail_retained_bundle(scope_path +
                                 " differs from the exact control domain");
          }
        } else {
          fail_retained_bundle(scope_path +
                               " has an unknown containment scope kind");
        }
      }
    }
    const int top_level_granularity =
        binding->second.containment == "resource"         ? 0
        : binding->second.containment == "controller_set" ? 1
                                                          : 2;
    const auto required_hard_count =
        actuation == core::GripperActuationKind::kPneumatic ? 0U : 1U;
    if (software_hard_count != required_hard_count ||
        previous_granularity != top_level_granularity) {
      fail_retained_bundle(stop_path + " has an invalid software-hard count or "
                                       "coarsest containment projection");
    }
  }
}

struct RetainedArtifactFacts final {
  std::string role;
  std::string media_type;
  std::uint64_t byte_size;
  std::string digest;
  std::string store_key;

  friend bool operator==(const RetainedArtifactFacts &,
                         const RetainedArtifactFacts &) = default;
};

[[nodiscard]] RetainedArtifactFacts
verify_retained_artifact_ref(const CanonicalValue &value,
                             std::string_view path) {
  const auto &row = require_object(value, path);
  require_exact_fields(row,
                       {"artifact_role_id", "media_type", "byte_size",
                        "content_digest", "content_addressed_store_key"},
                       path);
  const std::string role =
      require_identity_member(row, "artifact_role_id", path);
  const std::string media =
      require_string(require_member(row, "media_type", path),
                     std::string(path) + ".media_type");
  const std::int64_t size = require_safe_integer(
      require_member(row, "byte_size", path), std::string(path) + ".byte_size");
  const std::string digest = require_digest_member(row, "content_digest", path);
  const std::string key =
      require_string(require_member(row, "content_addressed_store_key", path),
                     std::string(path) + ".content_addressed_store_key");
  if (size <= 0 || key != retained_store_key_for_digest(digest)) {
    fail_retained_bundle(
        std::string(path) +
        " is not a safe non-empty retained artifact reference");
  }
  return {role, media, static_cast<std::uint64_t>(size), digest, key};
}

[[nodiscard]] std::optional<RetainedArtifactFacts>
verify_commissioned_stop_interfaces(const CanonicalValue::Object &control) {
  CanonicalValue::Array nested;
  for (const auto &stop_value :
       require_array(require_member(control, "stop_applicability", "control"),
                     "control.stop_applicability")) {
    const auto &stop = require_object(stop_value, "control.stop_applicability");
    if (require_string(
            require_member(stop, "disposition", "control.stop_applicability"),
            "control.stop_applicability.disposition") != "stoppable") {
      continue;
    }
    for (const auto &stage_value :
         require_array(require_member(stop, "escalation_stages",
                                      "control.stop_applicability"),
                       "control.stop_applicability.escalation_stages")) {
      const auto &stage = require_object(
          stage_value, "control.stop_applicability.escalation_stages");
      const auto &commissioned = require_member(
          stage, "commissioned_stop_interface", "control.stop_applicability");
      if (!is_null(commissioned)) {
        nested.push_back(commissioned);
      }
    }
  }
  const auto canonical_less = [](const auto &left, const auto &right) {
    return canonical_json(left) < canonical_json(right);
  };
  std::sort(nested.begin(), nested.end(), canonical_less);
  CanonicalValue::Array declared = require_array(
      require_member(control, "commissioned_stop_interfaces", "control"),
      "control.commissioned_stop_interfaces");
  std::sort(declared.begin(), declared.end(), canonical_less);
  if (declared != nested ||
      std::adjacent_find(declared.begin(), declared.end()) != declared.end()) {
    fail_retained_bundle("commissioned Stop interface table differs from the "
                         "exact final-stage projection");
  }
  std::optional<RetainedArtifactFacts> expected;
  for (const auto &value : declared) {
    const auto &row =
        require_object(value, "control.commissioned_stop_interfaces");
    const RetainedArtifactFacts artifact = verify_retained_artifact_ref(
        require_member(row, "commissioning_evidence_artifact_ref",
                       "control.commissioned_stop_interfaces"),
        "control.commissioned_stop_interfaces."
        "commissioning_evidence_artifact_ref");
    if (artifact.role != core::kStopInterfaceCommissioningArtifactRole ||
        artifact.media_type != core::kStopInterfaceCommissioningMediaType ||
        (expected.has_value() && *expected != artifact)) {
      fail_retained_bundle("commissioned Stop interfaces do not share one "
                           "exact evidence artifact");
    }
    expected = artifact;
  }
  return expected;
}

void verify_reference_profile_shape(
    const std::map<std::string, ControllerFacts, std::less<>> &controllers,
    const std::map<std::string, RouteFacts, std::less<>> &routes) {
  constexpr std::string_view kDirect =
      "manyforge.ros2_control.joint_trajectory_controller.direct";
  constexpr std::string_view kChainable =
      "manyforge.ros2_control.joint_trajectory_controller.chainable";
  constexpr std::string_view kAdmittance =
      "manyforge.ros2_control.admittance_controller";
  const auto count_implementation = [&](std::string_view expected) {
    return std::count_if(controllers.begin(), controllers.end(),
                         [&](const auto &row) {
                           return row.second.implementation_id == expected;
                         });
  };
  if ((controllers.size() == 1U && count_implementation(kDirect) != 1) ||
      (controllers.size() == 2U && (count_implementation(kChainable) != 1 ||
                                    count_implementation(kAdmittance) != 1))) {
    fail_retained_bundle(
        "control artifact does not select one exact direct JTC or one exact "
        "JTC/admittance chain");
  }
  for (const auto &[unused, route] : routes) {
    static_cast<void>(unused);
    if (route.controller_roles.size() != controllers.size() ||
        route.path_edges.size() + 1U != controllers.size() ||
        !std::all_of(
            controllers.begin(), controllers.end(), [&](const auto &row) {
              return std::find(route.controller_roles.begin(),
                               route.controller_roles.end(),
                               row.first) != route.controller_roles.end();
            })) {
      fail_retained_bundle(
          "control route does not realize the complete reference-profile "
          "controller topology");
    }
  }
}

void verify_desired_configuration(
    const CanonicalValue &value, std::string_view domain_id,
    std::string_view domain_revision, std::string_view topology_id,
    std::string_view topology_revision,
    const std::map<std::string, BindingFacts, std::less<>> &bindings,
    const std::map<std::string, ActivationFacts, std::less<>> &activations,
    const std::map<std::string, RouteFacts, std::less<>> &routes) {
  const auto &desired = require_object(value, "control.desired_configuration");
  require_exact_fields(desired,
                       {"control_domain_id", "control_domain_revision",
                        "controller_topology_id", "topology_revision",
                        "activation_set_id", "binding_routes"},
                       "control.desired_configuration");
  if (require_identity_member(desired, "control_domain_id",
                              "control.desired_configuration") != domain_id ||
      require_digest_member(desired, "control_domain_revision",
                            "control.desired_configuration") !=
          domain_revision ||
      require_identity_member(desired, "controller_topology_id",
                              "control.desired_configuration") != topology_id ||
      require_digest_member(desired, "topology_revision",
                            "control.desired_configuration") !=
          topology_revision) {
    fail_retained_bundle(
        "desired control configuration differs from the product subject");
  }
  const std::string activation = require_identity_member(
      desired, "activation_set_id", "control.desired_configuration");
  if (!activations.contains(activation)) {
    fail_retained_bundle(
        "desired control configuration selects an unknown activation set");
  }
  const auto &binding_routes =
      require_array(require_member(desired, "binding_routes",
                                   "control.desired_configuration"),
                    "control.desired_configuration.binding_routes");
  if (binding_routes.size() != bindings.size()) {
    fail_retained_bundle(
        "desired configuration does not cover every exact domain binding");
  }
  std::string previous_binding;
  for (std::size_t index = 0U; index < binding_routes.size(); ++index) {
    const std::string path = "control.desired_configuration.binding_routes[" +
                             std::to_string(index) + "]";
    const auto &row = require_object(binding_routes[index], path);
    require_exact_fields(
        row,
        {"control_binding_id", "control_binding_revision", "control_route_id"},
        path);
    const std::string binding_id =
        require_identity_member(row, "control_binding_id", path);
    const std::string binding_revision =
        require_digest_member(row, "control_binding_revision", path);
    const std::string route_id =
        require_identity_member(row, "control_route_id", path);
    if (!previous_binding.empty() && previous_binding >= binding_id) {
      fail_retained_bundle(
          "desired binding routes are not canonical and unique");
    }
    previous_binding = binding_id;
    const auto binding = bindings.find(binding_id);
    const auto route = routes.find(binding_id + "\x1f" + route_id);
    if (binding == bindings.end() ||
        binding->second.revision != binding_revision || route == routes.end() ||
        !route->second.activation_sets.contains(activation)) {
      fail_retained_bundle(
          path + " selects an unavailable binding route/activation tuple");
    }
  }
}

void verify_source_conformance(const CanonicalValue &value,
                               const CanonicalValue &bindings,
                               const CanonicalValue &controllers) {
  const auto &conformance = require_object(value, "control.source_conformance");
  require_exact_fields(conformance,
                       {"api_version", "source_conformance_digest"},
                       "control.source_conformance");
  if (require_string(require_member(conformance, "api_version",
                                    "control.source_conformance"),
                     "control.source_conformance.api_version") !=
      kSourceConformanceApiVersion) {
    fail_retained_bundle(
        "control source-conformance evidence has an unsupported API");
  }
  const std::string digest = require_digest_member(
      conformance, "source_conformance_digest", "control.source_conformance");
  const CanonicalValue preimage(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kSourceConformanceApiVersion))},
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

void verify_catalog_source_closure(
    const CanonicalValue::Object &control,
    const core::RetainedBundleDTO &retained_bundle,
    std::string_view topology_id, std::string_view topology_revision,
    const std::map<std::string, BindingFacts, std::less<>> &bindings,
    const std::map<std::string, ControllerFacts, std::less<>> &controllers,
    const std::map<std::string, ActivationFacts, std::less<>> &activations,
    const std::map<std::string, RouteFacts, std::less<>> &routes,
    std::span<const QualifiedStopEvidencePolicy> stop_policies,
    std::span<const QualifiedControllerTopology> controller_topologies,
    std::span<const QualifiedProviderManifest> provider_manifests) {
  if (stop_policies.empty() && controller_topologies.empty() &&
      provider_manifests.empty()) {
    return;
  }

  using Identity = std::pair<std::string, std::string>;
  std::set<Identity> expected_policy_ids;
  std::set<Identity> expected_manifest_ids;
  for (const auto &[unused, binding] : bindings) {
    static_cast<void>(unused);
    expected_manifest_ids.emplace(binding.provider_manifest_id,
                                  binding.provider_manifest_revision);
  }
  for (const auto &binding : retained_bundle.cell_graph.control_bindings)
    expected_policy_ids.emplace(binding.stop_evidence_policy_id.value(),
                                binding.stop_evidence_policy_revision.value());
  std::map<Identity, const QualifiedStopEvidencePolicy *> policies;
  for (const auto &policy : stop_policies) {
    const Identity key{policy.value().stop_evidence_policy_id.value(),
                       policy.value().stop_evidence_policy_revision.value()};
    if (!policies.emplace(key, &policy).second) {
      fail_retained_bundle("qualified Stop-policy closure is duplicate");
    }
  }
  std::map<Identity, const QualifiedProviderManifest *> manifests;
  for (const auto &manifest : provider_manifests) {
    const Identity key{manifest.value().provider_manifest_id.value(),
                       manifest.value().provider_manifest_revision.value()};
    if (!manifests.emplace(key, &manifest).second) {
      fail_retained_bundle("qualified provider-manifest closure is duplicate");
    }
  }
  std::set<Identity> supplied_policy_ids;
  for (const auto &[key, unused] : policies) {
    static_cast<void>(unused);
    supplied_policy_ids.insert(key);
  }
  std::set<Identity> supplied_manifest_ids;
  for (const auto &[key, unused] : manifests) {
    static_cast<void>(unused);
    supplied_manifest_ids.insert(key);
  }
  if (supplied_policy_ids != expected_policy_ids ||
      supplied_manifest_ids != expected_manifest_ids) {
    fail_retained_bundle(
        "control artifact does not have its exact qualified policy/provider "
        "source closure");
  }

  const QualifiedControllerTopology *selected_topology = nullptr;
  for (const auto &topology : controller_topologies) {
    if (topology.value().controller_topology_id.value() == topology_id &&
        topology.value().topology_revision.value() == topology_revision) {
      if (selected_topology != nullptr) {
        fail_retained_bundle("qualified topology closure is duplicate");
      }
      selected_topology = &topology;
    } else {
      fail_retained_bundle(
          "qualified topology closure contains an unrelated topology");
    }
  }
  if (selected_topology == nullptr) {
    fail_retained_bundle(
        "control artifact has no exact qualified topology source");
  }
  const auto &topology = selected_topology->value();

  std::set<std::string, std::less<>> topology_controller_ids;
  for (const auto &controller : topology.controllers) {
    topology_controller_ids.insert(controller.controller_role_id.value());
  }
  std::set<std::string, std::less<>> retained_controller_ids;
  for (const auto &[role, unused] : controllers) {
    static_cast<void>(unused);
    retained_controller_ids.insert(role);
  }
  if (topology_controller_ids != retained_controller_ids) {
    fail_retained_bundle(
        "retained controller roles differ from their qualified topology");
  }

  std::map<std::string, const core::ControllerActivationSetDTO *, std::less<>>
      topology_activations;
  for (const auto &activation : topology.activation_sets) {
    topology_activations.emplace(activation.activation_set_id.value(),
                                 &activation);
  }
  if (topology_activations.size() != activations.size()) {
    fail_retained_bundle("retained activation-set inventory differs from its "
                         "qualified topology");
  }
  for (const auto &[id, retained] : activations) {
    const auto source = topology_activations.find(id);
    if (source == topology_activations.end()) {
      fail_retained_bundle(
          "retained activation set is absent from its qualified topology");
    }
    std::set<std::string, std::less<>> roles;
    for (const auto &role : source->second->active_controller_role_ids) {
      roles.insert(role.value());
    }
    std::set<EdgeRef> edges;
    for (const auto &edge : source->second->active_chain_edges) {
      edges.insert({edge.upstream_controller_role_id.value(),
                    edge.downstream_controller_role_id.value()});
    }
    if (roles != retained.roles || edges != retained.edges) {
      fail_retained_bundle(
          "retained activation set differs from its qualified topology");
    }
  }

  std::map<std::string, const core::ControlRouteDTO *, std::less<>>
      topology_routes;
  for (const auto &route : topology.legal_routes) {
    topology_routes.emplace(route.control_route_id.value(), &route);
  }
  for (const auto &[route_key, retained] : routes) {
    const auto source =
        topology_routes.find(route_key.substr(route_key.find('\x1f') + 1U));
    if (source == topology_routes.end()) {
      fail_retained_bundle(
          "retained route is absent from its qualified topology");
    }
    std::vector<std::string> roles;
    for (const auto &role : source->second->ordered_controller_role_ids) {
      roles.push_back(role.value());
    }
    std::set<std::string> compatible;
    for (const auto &activation :
         source->second->compatible_activation_set_ids) {
      compatible.insert(activation.value());
    }
    if (roles != retained.controller_roles ||
        compatible != retained.activation_sets) {
      fail_retained_bundle(
          "retained route differs from its qualified topology");
    }
  }

  for (const auto &stop_value :
       require_array(require_member(control, "stop_applicability", "control"),
                     "control.stop_applicability")) {
    const auto &stop = require_object(stop_value, "control.stop_applicability");
    const std::string binding_id = require_identity_member(
        stop, "control_binding_id", "control.stop_applicability");
    const auto binding = bindings.find(binding_id);
    if (binding == bindings.end()) {
      fail_retained_bundle("retained Stop row lost its binding source");
    }
    const Identity policy_key{
        require_identity_member(stop, "stop_evidence_policy_id", "control"),
        require_digest_member(stop, "stop_evidence_policy_revision",
                              "control")};
    const auto policy = policies.find(policy_key);
    if (policy == policies.end()) {
      fail_retained_bundle("retained Stop row lost its exact policy source");
    }
    CanonicalValue::Object policy_fields{
        {"disposition", require_member(stop, "disposition", "control")},
        {"containment_granularity",
         require_member(stop, "containment_granularity", "control")}};
    if (require_string(require_member(stop, "disposition", "control"),
                       "control.stop_applicability.disposition") ==
        "stoppable") {
      CanonicalValue::Array stages;
      for (const auto &stage_value :
           require_array(require_member(stop, "escalation_stages", "control"),
                         "control.stop_applicability.escalation_stages")) {
        const auto &stage = require_object(
            stage_value, "control.stop_applicability.escalation_stages");
        stages.emplace_back(CanonicalValue::Object{
            {"stop_escalation_stage_id",
             require_member(stage, "stop_escalation_stage_id", "control")},
            {"stage_kind", require_member(stage, "stage_kind", "control")},
            {"containment_granularity",
             require_member(stage, "containment_granularity", "control")},
            {"containment_command",
             require_member(stage, "containment_command", "control")},
            {"evidence_conjunction",
             require_member(stage, "evidence_conjunction", "control")},
            {"all_of_evidence_requirements",
             require_member(stage, "all_of_evidence_requirements", "control")},
        });
      }
      policy_fields.emplace_back("escalation_stages",
                                 CanonicalValue(std::move(stages)));
    }
    const auto &expected_policy = policy->second->value();
    if (canonical_json(CanonicalValue(std::move(policy_fields))) !=
        canonical_json(stop_policy_applicability_projection(expected_policy))) {
      fail_retained_bundle(
          "retained Stop semantics differ from their qualified policy source");
    }

    const Identity manifest_key{binding->second.provider_manifest_id,
                                binding->second.provider_manifest_revision};
    const auto manifest = manifests.find(manifest_key);
    if (manifest == manifests.end()) {
      fail_retained_bundle(
          "retained binding lost its exact provider-manifest source");
    }
    const auto offered = std::find_if(
        manifest->second->value().offered_capabilities.begin(),
        manifest->second->value().offered_capabilities.end(),
        [&](const auto &capability) {
          return capability.capability_id.value() ==
                     binding->second.capability_id &&
                 capability.capability_definition_revision.value() ==
                     binding->second.capability_revision;
        });
    if (offered == manifest->second->value().offered_capabilities.end()) {
      fail_retained_bundle(
          "retained binding capability is absent from its provider manifest");
    }
    if (require_string(require_member(stop, "disposition", "control"),
                       "control.stop_applicability.disposition") !=
        "stoppable") {
      continue;
    }
    const auto declaration_for = [&](const ContractFacts &contract)
        -> const core::ProviderContractDeclarationDTO * {
      const auto found = std::find_if(
          manifest->second->value().provider_contracts.begin(),
          manifest->second->value().provider_contracts.end(),
          [&](const auto &declaration) {
            const auto &candidate = declaration.provider_contract_ref;
            return candidate.provider_contract_role_id.value() ==
                       contract.role_id &&
                   candidate.contract_schema_id.value() == contract.schema_id &&
                   candidate.contract_schema_revision.value() ==
                       contract.schema_revision;
          });
      return found == manifest->second->value().provider_contracts.end()
                 ? nullptr
                 : &*found;
    };
    for (const auto &stage_value :
         require_array(require_member(stop, "escalation_stages", "control"),
                       "control.stop_applicability.escalation_stages")) {
      const auto &stage = require_object(
          stage_value, "control.stop_applicability.escalation_stages");
      const auto &command = require_object(
          require_member(stage, "containment_command", "control"),
          "control.stop_applicability.containment_command");
      const auto contract = verify_optional_provider_contract(
          require_member(command, "provider_contract_ref", "control"),
          "control.stop_applicability.provider_contract_ref");
      if (contract.has_value()) {
        const auto *declaration = declaration_for(*contract);
        if (declaration == nullptr ||
            !declaration->stop_command_support.has_value()) {
          fail_retained_bundle(
              "retained Stop command is absent from its provider manifest");
        }
        const auto &support = *declaration->stop_command_support;
        const std::string &command_kind =
            require_string(require_member(command, "command_kind", "control"),
                           "control.stop_applicability.command_kind");
        const auto registered = verify_optional_identity(
            require_member(command, "registered_command_kind_id", "control"),
            "control.stop_applicability.registered_command_kind_id");
        if (core::to_string(support.command_kind) != command_kind ||
            support.registered_command_kind_id.has_value() !=
                registered.has_value() ||
            (registered.has_value() &&
             support.registered_command_kind_id->value() != *registered)) {
          fail_retained_bundle(
              "retained Stop command semantics differ from their provider "
              "manifest");
        }
        const CanonicalValue &endpoint_value =
            require_member(stage, "provider_command_endpoint", "control");
        std::optional<std::string> endpoint;
        if (!is_null(endpoint_value)) {
          endpoint = require_identity_member(
              require_object(endpoint_value, "control.stop_applicability."
                                             "provider_command_endpoint"),
              "endpoint_role_id",
              "control.stop_applicability.provider_command_endpoint");
        }
        const auto endpoint_supported = [&](std::string_view value) {
          return std::any_of(
              support.command_endpoint_role_ids.begin(),
              support.command_endpoint_role_ids.end(),
              [&](const auto &role) { return role.value() == value; });
        };
        if (support.command_endpoint_role_ids.empty() !=
                !endpoint.has_value() ||
            (endpoint.has_value() && !endpoint_supported(*endpoint))) {
          fail_retained_bundle(
              "retained Stop command endpoint differs from its provider "
              "manifest");
        }
      }

      for (const auto &evidence_value : require_array(
               require_member(stage, "all_of_evidence_requirements", "control"),
               "control.stop_applicability.all_of_evidence_requirements")) {
        const auto &evidence = require_object(
            evidence_value,
            "control.stop_applicability.all_of_evidence_requirements");
        const auto evidence_contract = verify_optional_provider_contract(
            require_member(evidence, "provider_contract_ref", "control"),
            "control.stop_applicability.evidence.provider_contract_ref");
        if (!evidence_contract.has_value()) {
          continue;
        }
        const auto *declaration = declaration_for(*evidence_contract);
        if (declaration == nullptr ||
            !declaration->stop_evidence_support.has_value()) {
          fail_retained_bundle(
              "retained Stop evidence is absent from its provider manifest");
        }
        const auto &support = *declaration->stop_evidence_support;
        const std::string &evidence_kind =
            require_string(require_member(evidence, "evidence_kind", "control"),
                           "control.stop_applicability.evidence_kind");
        const auto registered = verify_optional_identity(
            require_member(evidence, "registered_evidence_kind_id", "control"),
            "control.stop_applicability.registered_evidence_kind_id");
        if (core::to_string(support.evidence_kind) != evidence_kind ||
            support.registered_evidence_kind_id.has_value() !=
                registered.has_value() ||
            (registered.has_value() &&
             support.registered_evidence_kind_id->value() != *registered)) {
          fail_retained_bundle(
              "retained Stop evidence semantics differ from their provider "
              "manifest");
        }
        const auto &source = require_object(
            require_member(evidence, "source_requirement", "control"),
            "control.stop_applicability.source_requirement");
        const auto endpoint = verify_optional_identity(
            require_member(source, "endpoint_role_id", "control"),
            "control.stop_applicability.source_requirement.endpoint_role_id");
        const auto endpoint_supported = [&](std::string_view value) {
          return std::any_of(
              support.evidence_endpoint_role_ids.begin(),
              support.evidence_endpoint_role_ids.end(),
              [&](const auto &role) { return role.value() == value; });
        };
        if (support.evidence_endpoint_role_ids.empty() !=
                !endpoint.has_value() ||
            (endpoint.has_value() && !endpoint_supported(*endpoint))) {
          fail_retained_bundle(
              "retained Stop evidence endpoint differs from its provider "
              "manifest");
        }
      }
    }
  }
}

[[nodiscard]] const core::RetainedProductDTO &require_retained_control_product(
    const core::RetainedBundleDTO &retained_bundle,
    const core::ControlDomainId &control_domain_id) {
  const core::RetainedProductDTO *retained_product = nullptr;
  for (const auto &group :
       retained_bundle.compiled_products.control_domain_products) {
    if (group.control_domain_id != control_domain_id) {
      continue;
    }
    if (retained_product != nullptr || group.products.size() != 1U) {
      fail_retained_bundle(
          "retained bundle does not contain one exact control-domain product");
    }
    retained_product = &group.products.front();
  }
  if (retained_product == nullptr ||
      retained_product->product_id !=
          core::ProductId(
              control_domain_control_product_id(control_domain_id)) ||
      retained_product->product_kind !=
          core::RetainedProductKind::kControlDomainControl ||
      retained_product->product_contract_id.value() !=
          control_domain_control_product_contract_id() ||
      retained_product->product_contract_revision !=
          control_domain_control_product_contract_revision()) {
    fail_retained_bundle("retained bundle has no exact qualified "
                         "control-domain artifact anchor");
  }
  return *retained_product;
}

void require_retained_control_artifact_anchor(
    std::string_view canonical_bytes,
    const core::RetainedBundleDTO &retained_bundle,
    const core::ControlDomainId &control_domain_id) {
  const auto &retained_product =
      require_retained_control_product(retained_bundle, control_domain_id);
  const auto artifact = std::find_if(
      retained_product.artifact_refs.begin(),
      retained_product.artifact_refs.end(), [](const auto &candidate) {
        return candidate.artifact_role_id.value() ==
               kControlDomainControlArtifactRole;
      });
  if (artifact == retained_product.artifact_refs.end() ||
      artifact->media_type != kControlDomainControlMediaType ||
      artifact->byte_size !=
          static_cast<std::uint64_t>(canonical_bytes.size()) ||
      artifact->content_digest.value() != sha256_digest(canonical_bytes)) {
    fail_retained_bundle("control-domain artifact bytes differ from the "
                         "trusted retained producer digest");
  }
}

} // namespace

std::string control_domain_control_product_id(
    const core::ControlDomainId &control_domain_id) {
  require_retained_portable_identity(control_domain_id.value(),
                                     "control_domain_id");
  std::string preimage = "manyforge.control-domain-product-id.v1";
  preimage.push_back('\0');
  preimage.append(control_domain_id.value());
  const std::string digest = sha256_digest(preimage);
  return "control_domain_control_" + digest.substr(7U);
}

CanonicalValue parse_and_verify_control_domain_control(
    std::string_view canonical_bytes, const core::CellGraphDTO *source_graph,
    std::span<const CompositionControlBindingCoordinateMap> coordinate_maps) {
  if (canonical_bytes.empty() ||
      canonical_bytes.size() > kMaxControlDomainControlBytes) {
    fail_retained_bundle(
        "control-domain control artifact is empty or exceeds its byte bound");
  }
  CanonicalValue root;
  try {
    root = parse_strict_json_model_yaml(canonical_bytes).root();
  } catch (const std::exception &error) {
    fail_retained_bundle(
        std::string("control-domain control artifact is invalid JSON: ") +
        error.what());
  }
  if (canonical_json(root) != canonical_bytes) {
    fail_retained_bundle(
        "control-domain control artifact is not exact canonical JSON");
  }
  const auto &object = require_object(root, "control");
  require_exact_fields(
      object,
      {"api_version", "control_domain_id", "control_domain_revision",
       "controller_topology_id", "topology_revision",
       "control_coordinator_endpoint_role", "containment_granularity",
       "implementation_profile", "desired_configuration", "bindings",
       "controllers", "activation_sets", "routes", "stop_applicability",
       "commissioned_stop_interfaces", "source_conformance"},
      "control");
  if (require_string(require_member(object, "api_version", "control"),
                     "control.api_version") !=
      kControlDomainControlApiVersion) {
    fail_retained_bundle(
        "control-domain control artifact has an unsupported API version");
  }
  const std::string domain_id =
      require_identity_member(object, "control_domain_id", "control");
  const std::string domain_revision =
      require_digest_member(object, "control_domain_revision", "control");
  const std::string topology_id =
      require_identity_member(object, "controller_topology_id", "control");
  const std::string topology_revision =
      require_digest_member(object, "topology_revision", "control");
  static_cast<void>(require_identity_member(
      object, "control_coordinator_endpoint_role", "control"));
  const std::string &containment = require_string(
      require_member(object, "containment_granularity", "control"),
      "control.containment_granularity");
  if (containment != "resource" && containment != "controller_set" &&
      containment != "control_domain") {
    fail_retained_bundle(
        "control-domain product has unknown containment granularity");
  }

  const auto &profile = require_object(
      require_member(object, "implementation_profile", "control"),
      "control.implementation_profile");
  require_exact_fields(profile,
                       {"implementation_profile_id",
                        "implementation_profile_revision",
                        "generated_controller_manager_name"},
                       "control.implementation_profile");
  if (require_identity_member(profile, "implementation_profile_id",
                              "control.implementation_profile") !=
          kImplementationProfileId ||
      require_digest_member(profile, "implementation_profile_revision",
                            "control.implementation_profile") !=
          implementation_profile_revision()) {
    fail_retained_bundle(
        "control-domain product selects an unqualified implementation profile");
  }
  if (require_identity_member(profile, "generated_controller_manager_name",
                              "control.implementation_profile") !=
      expected_controller_manager_name(domain_id, domain_revision, topology_id,
                                       topology_revision)) {
    fail_retained_bundle(
        "control-domain product has the wrong deterministic controller "
        "manager name");
  }

  const CanonicalValue &bindings_value =
      require_member(object, "bindings", "control");
  const CanonicalValue &controllers_value =
      require_member(object, "controllers", "control");
  const auto bindings = verify_bindings(bindings_value);
  const auto controllers = verify_controllers(controllers_value, bindings);
  const auto activations = verify_activation_sets(
      require_member(object, "activation_sets", "control"), controllers);
  const auto routes = verify_routes(require_member(object, "routes", "control"),
                                    bindings, controllers, activations);
  verify_reference_profile_shape(controllers, routes);
  verify_stop_applicability(
      require_member(object, "stop_applicability", "control"), domain_id,
      domain_revision, topology_id, topology_revision, bindings, controllers,
      activations, routes, source_graph, coordinate_maps);
  static_cast<void>(verify_commissioned_stop_interfaces(object));
  verify_desired_configuration(
      require_member(object, "desired_configuration", "control"), domain_id,
      domain_revision, topology_id, topology_revision, bindings, activations,
      routes);
  verify_source_conformance(
      require_member(object, "source_conformance", "control"), bindings_value,
      controllers_value);
  return root;
}

void require_control_domain_control_matches_retained_bundle(
    std::string_view canonical_bytes,
    const core::RetainedBundleDTO &retained_bundle,
    const core::ControlDomainId &control_domain_id,
    std::span<const QualifiedStopEvidencePolicy> stop_policies,
    std::span<const QualifiedControllerTopology> controller_topologies,
    std::span<const QualifiedProviderManifest> provider_manifests,
    std::span<const CompositionControlBindingCoordinateMap> coordinate_maps) {
  require_retained_control_artifact_anchor(canonical_bytes, retained_bundle,
                                           control_domain_id);
  const CanonicalValue root = parse_and_verify_control_domain_control(
      canonical_bytes, &retained_bundle.cell_graph, coordinate_maps);
  const auto &object = require_object(root, "control");
  const auto source_bindings =
      verify_bindings(require_member(object, "bindings", "control"));
  const auto source_controllers = verify_controllers(
      require_member(object, "controllers", "control"), source_bindings);
  const auto source_activations = verify_activation_sets(
      require_member(object, "activation_sets", "control"), source_controllers);
  const auto source_routes =
      verify_routes(require_member(object, "routes", "control"),
                    source_bindings, source_controllers, source_activations);
  verify_catalog_source_closure(
      object, retained_bundle,
      require_identity_member(object, "controller_topology_id", "control"),
      require_digest_member(object, "topology_revision", "control"),
      source_bindings, source_controllers, source_activations, source_routes,
      stop_policies, controller_topologies, provider_manifests);
  const auto &graph = retained_bundle.cell_graph;
  const auto domain =
      std::find_if(graph.control_domains.begin(), graph.control_domains.end(),
                   [&](const auto &candidate) {
                     return candidate.control_domain_id == control_domain_id;
                   });
  if (domain == graph.control_domains.end() ||
      require_identity_member(object, "control_domain_id", "control") !=
          domain->control_domain_id.value() ||
      require_digest_member(object, "control_domain_revision", "control") !=
          domain->control_domain_revision.value() ||
      require_identity_member(object, "controller_topology_id", "control") !=
          domain->controller_topology_id.value() ||
      require_digest_member(object, "topology_revision", "control") !=
          domain->topology_revision.value() ||
      require_identity_member(object, "control_coordinator_endpoint_role",
                              "control") !=
          domain->control_coordinator_endpoint_role.value() ||
      require_string(
          require_member(object, "containment_granularity", "control"),
          "control.containment_granularity") !=
          core::to_string(domain->containment_granularity)) {
    fail_retained_bundle(
        "control-domain artifact subject differs from its retained graph");
  }

  std::map<std::string, const core::ControlBindingDTO *, std::less<>>
      expected_bindings;
  for (const auto &binding : graph.control_bindings) {
    if (binding.control_domain_id == control_domain_id) {
      expected_bindings.emplace(binding.control_binding_id.value(), &binding);
    }
  }
  const auto &binding_rows = require_array(
      require_member(object, "bindings", "control"), "control.bindings");
  if (binding_rows.size() != expected_bindings.size()) {
    fail_retained_bundle(
        "control-domain artifact binding set differs from its retained graph");
  }
  for (std::size_t index = 0U; index < binding_rows.size(); ++index) {
    const std::string path = "control.bindings[" + std::to_string(index) + "]";
    const auto &row = require_object(binding_rows[index], path);
    const auto expected = expected_bindings.find(
        require_identity_member(row, "control_binding_id", path));
    if (expected == expected_bindings.end()) {
      fail_retained_bundle(path + " has no retained graph binding subject");
    }
    const auto &binding = *expected->second;
    const bool scalar_mismatch =
        require_digest_member(row, "control_binding_revision", path) !=
            binding.control_binding_revision.value() ||
        require_identity_member(row, "controllable_component_id", path) !=
            binding.controllable_component_id.value() ||
        require_identity_member(row, "command_target_id", path) !=
            binding.command_target_id.value() ||
        require_identity_member(row, "capability_id", path) !=
            binding.capability_id.value() ||
        require_digest_member(row, "capability_definition_revision", path) !=
            binding.capability_definition_revision.value() ||
        require_identity_member(row, "control_interface_contract_id", path) !=
            binding.control_interface_contract_id.value() ||
        require_digest_member(row, "control_interface_revision", path) !=
            binding.control_interface_revision.value() ||
        require_identity_member(row, "provider_instance_id", path) !=
            binding.provider_instance_id.value() ||
        require_identity_member(row, "provider_runtime_unit_id", path) !=
            binding.provider_runtime_unit_id.value() ||
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
      fail_retained_bundle(
          path + " scalar semantics differ from its retained graph binding");
    }

    std::set<std::pair<std::string, std::string>> expected_resources;
    for (const auto &resource : binding.resource_bindings) {
      expected_resources.emplace(resource.resource_role_id.value(),
                                 resource.resource_id.value());
    }
    const auto claim_resources = [&](std::string_view field) {
      std::set<std::pair<std::string, std::string>> resources;
      const auto &claims = require_array(require_member(row, field, path),
                                         path + "." + std::string(field));
      for (const auto &claim_value : claims) {
        const auto &claim = require_object(claim_value, path);
        resources.emplace(
            require_identity_member(claim, "resource_role_id", path),
            require_identity_member(claim, "resource_id", path));
      }
      return resources;
    };
    if (claim_resources("command_claims") != expected_resources ||
        claim_resources("state_claims") != expected_resources) {
      fail_retained_bundle(
          path + " claim/resource coverage differs from its retained binding");
    }
  }

  const core::DesiredControlConfigurationDTO *expected_desired = nullptr;
  for (const auto &desired : retained_bundle.desired_control_configurations) {
    if (desired.control_domain_id == control_domain_id) {
      if (expected_desired != nullptr) {
        fail_retained_bundle(
            "retained bundle duplicates a desired control-domain subject");
      }
      expected_desired = &desired;
    }
  }
  if (expected_desired == nullptr) {
    fail_retained_bundle(
        "retained bundle omits the control-domain desired configuration");
  }
  const auto &desired =
      require_object(require_member(object, "desired_configuration", "control"),
                     "control.desired_configuration");
  if (require_identity_member(desired, "control_domain_id",
                              "control.desired_configuration") !=
          expected_desired->control_domain_id.value() ||
      require_digest_member(desired, "control_domain_revision",
                            "control.desired_configuration") !=
          expected_desired->control_domain_revision.value() ||
      require_identity_member(desired, "controller_topology_id",
                              "control.desired_configuration") !=
          expected_desired->controller_topology_id.value() ||
      require_digest_member(desired, "topology_revision",
                            "control.desired_configuration") !=
          expected_desired->topology_revision.value() ||
      require_identity_member(desired, "activation_set_id",
                              "control.desired_configuration") !=
          expected_desired->activation_set_id.value()) {
    fail_retained_bundle(
        "control-domain desired selection differs from its retained bundle");
  }
  const auto &desired_routes =
      require_array(require_member(desired, "binding_routes",
                                   "control.desired_configuration"),
                    "control.desired_configuration.binding_routes");
  if (desired_routes.size() != expected_desired->binding_routes.size()) {
    fail_retained_bundle(
        "control-domain desired route set differs from its retained bundle");
  }
  for (std::size_t index = 0U; index < desired_routes.size(); ++index) {
    const auto &row = require_object(
        desired_routes[index], "control.desired_configuration.binding_routes");
    const auto &expected = expected_desired->binding_routes[index];
    if (require_identity_member(row, "control_binding_id",
                                "control.desired_configuration") !=
            expected.control_binding_id.value() ||
        require_digest_member(row, "control_binding_revision",
                              "control.desired_configuration") !=
            expected.control_binding_revision.value() ||
        require_identity_member(row, "control_route_id",
                                "control.desired_configuration") !=
            expected.control_route_id.value()) {
      fail_retained_bundle(
          "control-domain desired route differs from its retained bundle");
    }
  }

  using RouteKey = std::pair<std::string, std::string>;
  std::map<RouteKey, const core::RouteEndpointProjectionDTO *> expected_routes;
  for (const auto &[unused, binding] : expected_bindings) {
    static_cast<void>(unused);
    if (binding->legal_control_route_ids.size() !=
        binding->route_endpoint_projections.size()) {
      fail_retained_bundle(
          "retained binding route/endpoint inventory is incoherent");
    }
    for (const auto &route_id : binding->legal_control_route_ids) {
      const auto endpoint =
          std::find_if(binding->route_endpoint_projections.begin(),
                       binding->route_endpoint_projections.end(),
                       [&](const auto &candidate) {
                         return candidate.control_route_id == route_id;
                       });
      if (endpoint == binding->route_endpoint_projections.end() ||
          !expected_routes
               .emplace(RouteKey{binding->control_binding_id.value(),
                                 route_id.value()},
                        &*endpoint)
               .second) {
        fail_retained_bundle(
            "retained binding has an invalid route/endpoint projection");
      }
    }
  }
  const auto &route_rows = require_array(
      require_member(object, "routes", "control"), "control.routes");
  if (route_rows.size() != expected_routes.size()) {
    fail_retained_bundle(
        "control-domain route set differs from its retained graph");
  }
  for (std::size_t index = 0U; index < route_rows.size(); ++index) {
    const std::string path = "control.routes[" + std::to_string(index) + "]";
    const auto &row = require_object(route_rows[index], path);
    const auto expected = expected_routes.find(
        RouteKey{require_identity_member(row, "control_binding_id", path),
                 require_identity_member(row, "control_route_id", path)});
    if (expected == expected_routes.end()) {
      fail_retained_bundle(path + " is absent from its retained graph");
    }
    std::set<std::string> command_roles;
    std::set<std::string> state_roles;
    for (const auto &endpoint_value : require_array(
             require_member(row, "endpoints", path), path + ".endpoints")) {
      const auto &endpoint = require_object(endpoint_value, path);
      const std::string role =
          require_identity_member(endpoint, "endpoint_role_id", path);
      if (require_bool(require_member(endpoint, "is_command_endpoint", path),
                       path + ".is_command_endpoint")) {
        command_roles.insert(role);
      }
      if (require_bool(require_member(endpoint, "is_state_endpoint", path),
                       path + ".is_state_endpoint")) {
        state_roles.insert(role);
      }
    }
    const auto strong_set = [](const auto &values) {
      std::set<std::string> result;
      for (const auto &value : values) {
        result.insert(value.value());
      }
      return result;
    };
    if (command_roles !=
            strong_set(expected->second->command_endpoint_role_ids) ||
        state_roles != strong_set(expected->second->state_endpoint_role_ids)) {
      fail_retained_bundle(path +
                           " endpoint roles differ from its retained binding");
    }
  }
}

void require_control_domain_control_matches_retained_bundle(
    const ControlDomainControlArtifactClosure &artifacts,
    const core::RetainedBundleDTO &retained_bundle,
    const core::ControlDomainId &control_domain_id,
    std::span<const QualifiedStopEvidencePolicy> stop_policies,
    std::span<const QualifiedControllerTopology> controller_topologies,
    std::span<const QualifiedProviderManifest> provider_manifests,
    std::span<const CompositionControlBindingCoordinateMap> coordinate_maps) {
  require_control_domain_control_matches_retained_bundle(
      artifacts.control_domain_control, retained_bundle, control_domain_id,
      stop_policies, controller_topologies, provider_manifests,
      coordinate_maps);

  const CanonicalValue root = parse_and_verify_control_domain_control(
      artifacts.control_domain_control, &retained_bundle.cell_graph,
      coordinate_maps);
  const auto &control = require_object(root, "control");
  const auto commissioned = verify_commissioned_stop_interfaces(control);
  if (commissioned.has_value() !=
      artifacts.stop_interface_commissioning_evidence.has_value()) {
    fail_retained_bundle("commissioned Stop-interface evidence presence "
                         "differs from its semantic rows");
  }
  if (commissioned.has_value()) {
    const std::string_view bytes =
        *artifacts.stop_interface_commissioning_evidence;
    if (bytes.empty() ||
        static_cast<std::uint64_t>(bytes.size()) != commissioned->byte_size ||
        sha256_digest(bytes) != commissioned->digest ||
        commissioned->store_key !=
            retained_store_key_for_digest(commissioned->digest)) {
      fail_retained_bundle("commissioned Stop-interface evidence bytes differ "
                           "from the retained artifact reference");
    }
  }
}

} // namespace manyforge::deployment::detail
