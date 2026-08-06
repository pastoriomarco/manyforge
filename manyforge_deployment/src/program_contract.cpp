#include "manyforge_deployment/program_contract.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "content_identity_internal.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::size_t kMaximumIdentityBytes = 128U;
constexpr std::size_t kMaximumProgramTreeDepth = 256U;
constexpr std::size_t kMaximumProgramTreeNodes = 100000U;
constexpr std::size_t kMaximumProgramChildren = 10000U;

[[noreturn]] void fail(std::string message) {
  throw ProgramContractError(std::move(message));
}

[[nodiscard]] const CanonicalValue::Object& require_object(
    const CanonicalValue& value, std::string_view path) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(std::string(path) + ": expected object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view path) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(std::string(path) + ": expected array");
  }
  return *array;
}

[[nodiscard]] const std::string& require_string(
    const CanonicalValue& value, std::string_view path) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail(std::string(path) + ": expected string");
  }
  return *text;
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue::Object& object, std::string_view name,
    std::string_view path) {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  if (found == object.end()) {
    fail(std::string(path) + ": missing required field '" +
         std::string(name) + "'");
  }
  return found->second;
}

[[nodiscard]] const CanonicalValue* find_member(
    const CanonicalValue::Object& object, std::string_view name) noexcept {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  return found == object.end() ? nullptr : &found->second;
}

void require_fields(const CanonicalValue::Object& object,
                    std::initializer_list<std::string_view> required,
                    std::initializer_list<std::string_view> optional,
                    std::string_view path) {
  for (const auto& [name, unused] : object) {
    (void)unused;
    if (std::find(required.begin(), required.end(), name) == required.end() &&
        std::find(optional.begin(), optional.end(), name) == optional.end()) {
      fail(std::string(path) + ": unknown field '" + name + "'");
    }
  }
  for (const std::string_view name : required) {
    (void)require_member(object, name, path);
  }
}

void require_exact_fields(const CanonicalValue::Object& object,
                          std::initializer_list<std::string_view> expected,
                          std::string_view path) {
  require_fields(object, expected, {}, path);
}

[[nodiscard]] std::string child_path(std::string_view parent,
                                     std::string_view child) {
  return std::string(parent) + "." + std::string(child);
}

[[nodiscard]] std::string index_path(std::string_view parent,
                                     std::size_t index) {
  return std::string(parent) + "[" + std::to_string(index) + "]";
}

[[nodiscard]] bool is_ascii_alphanumeric(unsigned char value) noexcept {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9');
}

void require_identity(std::string_view value, std::string_view path) {
  if (value.empty() || value.size() > kMaximumIdentityBytes ||
      !is_ascii_alphanumeric(static_cast<unsigned char>(value.front())) ||
      !std::all_of(value.begin() + 1, value.end(), [](unsigned char byte) {
        return is_ascii_alphanumeric(byte) || byte == '.' || byte == '_' ||
               byte == '-';
      })) {
    fail(std::string(path) + ": invalid bounded portable identity");
  }
}

void require_program_local_identity(std::string_view value,
                                    std::string_view path) {
  require_identity(value, path);
  if (value.front() < 'a' || value.front() > 'z' ||
      !std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
               byte == '-';
      })) {
    fail(std::string(path) + ": expected lower-case Program-local ID");
  }
}

template <typename StrongType>
[[nodiscard]] StrongType parse_program_identity(const CanonicalValue& value,
                                                std::string_view path) {
  const std::string& spelling = require_string(value, path);
  require_program_local_identity(spelling, path);
  return StrongType(spelling);
}

template <typename StrongType>
[[nodiscard]] std::vector<StrongType> parse_identity_set(
    const CanonicalValue* value, std::string_view path) {
  if (value == nullptr) {
    return {};
  }
  const auto& array = require_array(*value, path);
  if (array.size() > core::kMaxProgramRoleRequirements) {
    fail(std::string(path) + ": exceeds fixed Program role bound");
  }
  std::vector<StrongType> result;
  result.reserve(array.size());
  for (std::size_t index = 0U; index < array.size(); ++index) {
    result.push_back(parse_program_identity<StrongType>(
        array[index], index_path(path, index)));
  }
  if (!std::is_sorted(result.begin(), result.end()) ||
      std::adjacent_find(result.begin(), result.end()) != result.end()) {
    fail(std::string(path) + ": must be duplicate-free and canonical-sorted");
  }
  return result;
}

[[nodiscard]] std::vector<std::string> parse_text_set(
    const CanonicalValue* value, std::string_view path) {
  if (value == nullptr) {
    return {};
  }
  const auto& array = require_array(*value, path);
  if (array.size() > core::kMaxProgramRoleRequirements) {
    fail(std::string(path) + ": exceeds fixed Program role bound");
  }
  std::vector<std::string> result;
  result.reserve(array.size());
  for (std::size_t index = 0U; index < array.size(); ++index) {
    const std::string& item = require_string(array[index], index_path(path, index));
    require_program_local_identity(item, index_path(path, index));
    result.push_back(item);
  }
  if (!std::is_sorted(result.begin(), result.end()) ||
      std::adjacent_find(result.begin(), result.end()) != result.end()) {
    fail(std::string(path) + ": must be duplicate-free and canonical-sorted");
  }
  return result;
}

[[nodiscard]] const core::CapabilityDefinitionDescriptor*
require_capability_definition(const core::RetainedBundleDTO& bundle,
                              const core::CapabilityId& capability_id) {
  const auto* descriptor =
      core::find_builtin_capability_definition(capability_id.value());
  if (descriptor == nullptr) {
    fail("Program target requires an unknown capability: " +
         capability_id.value());
  }
  std::size_t exact_matches = 0U;
  std::size_t conflicting_matches = 0U;
  for (const auto& record : bundle.catalogs.records) {
    if (core::catalog_record_kind(record) !=
            core::CatalogRecordKind::kCapabilityDefinition ||
        core::catalog_record_id_value(record) != capability_id.value()) {
      continue;
    }
    if (core::catalog_record_revision_value(record) ==
        descriptor->capability_definition_revision) {
      ++exact_matches;
    } else {
      ++conflicting_matches;
    }
  }
  if (exact_matches != 1U || conflicting_matches != 0U) {
    fail("retained catalog does not contain exactly one selected capability "
         "definition revision");
  }
  return descriptor;
}

[[nodiscard]] core::ProgramCapabilityDisposition parse_disposition(
    const CanonicalValue& value, std::string_view path) {
  const std::string& spelling = require_string(value, path);
  if (spelling == "required") {
    return core::ProgramCapabilityDisposition::kRequired;
  }
  if (spelling == "optional") {
    return core::ProgramCapabilityDisposition::kOptional;
  }
  fail(std::string(path) + ": expected required or optional");
}

void verify_tree(const CanonicalValue& value, std::string_view path,
                 std::size_t depth, std::size_t& count,
                 std::set<std::string>& instance_ids) {
  if (depth > kMaximumProgramTreeDepth || ++count > kMaximumProgramTreeNodes) {
    fail(std::string(path) + ": Program tree exceeds fixed traversal bounds");
  }
  const auto& node = require_object(value, path);
  require_fields(node, {"node_id", "instance_id", "parameters", "children"},
                 {"metadata"}, path);
  const std::string& node_id = require_string(
      require_member(node, "node_id", path), child_path(path, "node_id"));
  const std::string& instance_id = require_string(
      require_member(node, "instance_id", path),
      child_path(path, "instance_id"));
  require_program_local_identity(node_id, child_path(path, "node_id"));
  require_program_local_identity(instance_id, child_path(path, "instance_id"));
  if (!instance_ids.insert(instance_id).second) {
    fail(std::string(path) + ": duplicate tree instance_id");
  }
  (void)require_object(require_member(node, "parameters", path),
                       child_path(path, "parameters"));
  if (const auto* metadata = find_member(node, "metadata");
      metadata != nullptr) {
    (void)require_object(*metadata, child_path(path, "metadata"));
  }
  const auto& children = require_array(require_member(node, "children", path),
                                       child_path(path, "children"));
  if (children.size() > kMaximumProgramChildren) {
    fail(std::string(path) + ": child count exceeds fixed bound");
  }
  for (std::size_t index = 0U; index < children.size(); ++index) {
    verify_tree(children[index], index_path(child_path(path, "children"), index),
                depth + 1U, count, instance_ids);
  }
}

void verify_optional_top_level_shapes(const CanonicalValue::Object& root) {
  if (const auto* compatibility = find_member(root, "compatibility");
      compatibility != nullptr) {
    const auto& object = require_object(*compatibility, "program.compatibility");
    require_fields(object, {}, {"lanes", "assistant", "requires", "tags"},
                   "program.compatibility");
    if (const auto* lanes = find_member(object, "lanes"); lanes != nullptr) {
      const auto values = parse_text_set(lanes, "program.compatibility.lanes");
      for (const auto& lane : values) {
        if (lane != "manyforge-only" && lane != "isaac-sim" &&
            lane != "real-robot") {
          fail("program.compatibility.lanes: unknown lane");
        }
      }
    }
    if (const auto* assistant = find_member(object, "assistant");
        assistant != nullptr) {
      const std::string& spelling =
          require_string(*assistant, "program.compatibility.assistant");
      if (spelling != "required" && spelling != "optional" &&
          spelling != "forbidden") {
        fail("program.compatibility.assistant: invalid disposition");
      }
    }
    (void)parse_text_set(find_member(object, "requires"),
                         "program.compatibility.requires");
    (void)parse_text_set(find_member(object, "tags"),
                         "program.compatibility.tags");
  }
  if (const auto* viewer = find_member(root, "viewer"); viewer != nullptr) {
    (void)require_object(*viewer, "program.viewer");
  }
  for (const std::string_view field : {"scene", "parameters",
                                       "contact_policies"}) {
    if (const auto* value = find_member(root, field); value != nullptr) {
      (void)require_array(*value, child_path("program", field));
    }
  }
  if (const auto* blackboard = find_member(root, "blackboard");
      blackboard != nullptr) {
    (void)require_object(*blackboard, "program.blackboard");
  }
}

[[nodiscard]] core::ProgramBindingContractDTO extract_contract(
    const CanonicalValue& root_value, const RetainedBundle& retained_bundle) {
  const auto& root = require_object(root_value, "program");
  require_fields(
      root,
      {"api_version", "program_id", "program_revision", "program_profile",
       "name", "catalog", "component_roles", "target_roles", "tree"},
      {"description", "compatibility", "viewer", "scene", "parameters",
       "blackboard", "contact_policies"},
      "program");
  if (require_string(require_member(root, "api_version", "program"),
                     "program.api_version") != core::kProgramApiVersion) {
    fail("program.api_version: expected manyforge.program.v1");
  }
  if (require_string(require_member(root, "program_profile", "program"),
                     "program.program_profile") !=
      core::kComposableCellProgramProfile) {
    fail("program.program_profile: expected composable-cell-v1");
  }
  const std::string& name = require_string(
      require_member(root, "name", "program"), "program.name");
  if (name.empty()) {
    fail("program.name: expected non-empty text");
  }
  if (const auto* description = find_member(root, "description");
      description != nullptr) {
    (void)require_string(*description, "program.description");
  }

  const auto& bundle = retained_bundle.value();
  const auto& catalog = require_object(require_member(root, "catalog", "program"),
                                       "program.catalog");
  require_exact_fields(catalog, {"api_version", "content_version"},
                       "program.catalog");
  if (require_string(require_member(catalog, "api_version", "program.catalog"),
                     "program.catalog.api_version") !=
          "manyforge.node_catalog.v2" ||
      require_string(
          require_member(catalog, "content_version", "program.catalog"),
          "program.catalog.content_version") !=
          bundle.catalogs.node_environment.content_version) {
    fail("Program catalog selector differs from the retained node environment");
  }

  const auto& component_roles = require_object(
      require_member(root, "component_roles", "program"),
      "program.component_roles");
  if (component_roles.empty() ||
      component_roles.size() > core::kMaxProgramContractRoles) {
    fail("program.component_roles: violates fixed non-empty role bound");
  }
  std::vector<core::ProgramComponentRoleRequirementDTO> components;
  components.reserve(component_roles.size());
  for (const auto& [role_name, role_value] : component_roles) {
    require_program_local_identity(role_name, "program.component_roles key");
    const std::string path = "program.component_roles." + role_name;
    const auto& role = require_object(role_value, path);
    require_fields(role, {},
                   {"required_component_type_tags", "required_model_roles",
                    "required_attachment_interfaces"},
                   path);
    components.push_back(core::ProgramComponentRoleRequirementDTO{
        core::ComponentRoleId(role_name),
        parse_text_set(find_member(role, "required_component_type_tags"),
                       child_path(path, "required_component_type_tags")),
        parse_identity_set<core::ModelRoleId>(
            find_member(role, "required_model_roles"),
            child_path(path, "required_model_roles")),
        parse_identity_set<core::AttachmentInterfaceId>(
            find_member(role, "required_attachment_interfaces"),
            child_path(path, "required_attachment_interfaces")),
    });
  }

  const auto& target_roles = require_object(
      require_member(root, "target_roles", "program"),
      "program.target_roles");
  if (target_roles.size() > core::kMaxProgramContractRoles) {
    fail("program.target_roles: exceeds fixed role bound");
  }
  std::vector<core::ProgramTargetRoleRequirementDTO> targets;
  targets.reserve(target_roles.size());
  for (const auto& [role_name, role_value] : target_roles) {
    require_program_local_identity(role_name, "program.target_roles key");
    const std::string path = "program.target_roles." + role_name;
    const auto& role = require_object(role_value, path);
    require_fields(
        role, {"component_role", "required_capabilities"},
        {"required_frame_roles", "required_resource_roles",
         "required_collision_body_roles", "required_model_roles"},
        path);
    const core::ComponentRoleId component_role =
        parse_program_identity<core::ComponentRoleId>(
            require_member(role, "component_role", path),
            child_path(path, "component_role"));
    if (std::none_of(components.begin(), components.end(),
                     [&](const auto& component) {
                       return component.component_role_id == component_role;
                     })) {
      fail(path + ": references an undeclared component role");
    }

    const auto& capability_values = require_array(
        require_member(role, "required_capabilities", path),
        child_path(path, "required_capabilities"));
    if (capability_values.empty() ||
        capability_values.size() > core::kMaxProgramRoleRequirements) {
      fail(path + ": required_capabilities violates fixed non-empty bound");
    }
    std::vector<core::ProgramCapabilityRequirementDTO> capabilities;
    capabilities.reserve(capability_values.size());
    for (std::size_t index = 0U; index < capability_values.size(); ++index) {
      const std::string capability_path =
          index_path(child_path(path, "required_capabilities"), index);
      const auto& capability =
          require_object(capability_values[index], capability_path);
      require_exact_fields(capability,
                           {"capability_id", "parameters", "disposition"},
                           capability_path);
      const core::CapabilityId capability_id =
          parse_program_identity<core::CapabilityId>(
              require_member(capability, "capability_id", capability_path),
              child_path(capability_path, "capability_id"));
      (void)require_object(
          require_member(capability, "parameters", capability_path),
          child_path(capability_path, "parameters"));
      const auto* definition =
          require_capability_definition(bundle, capability_id);
      capabilities.push_back(core::ProgramCapabilityRequirementDTO{
          capability_id,
          core::CapabilityDefinitionRevision(
              std::string(definition->capability_definition_revision)),
          parse_disposition(
              require_member(capability, "disposition", capability_path),
              child_path(capability_path, "disposition")),
      });
    }
    if (!std::is_sorted(
            capabilities.begin(), capabilities.end(),
            [](const auto& left, const auto& right) {
              return left.capability_id < right.capability_id;
            }) ||
        std::adjacent_find(
            capabilities.begin(), capabilities.end(),
            [](const auto& left, const auto& right) {
              return left.capability_id == right.capability_id;
            }) != capabilities.end()) {
      fail(path +
           ": required_capabilities must be unique and canonical-sorted");
    }

    targets.push_back(core::ProgramTargetRoleRequirementDTO{
        core::TargetRoleId(role_name), component_role,
        std::move(capabilities),
        parse_identity_set<core::FrameRoleId>(
            find_member(role, "required_frame_roles"),
            child_path(path, "required_frame_roles")),
        parse_identity_set<core::ResourceRoleId>(
            find_member(role, "required_resource_roles"),
            child_path(path, "required_resource_roles")),
        parse_identity_set<core::CollisionBodyRoleId>(
            find_member(role, "required_collision_body_roles"),
            child_path(path, "required_collision_body_roles")),
        parse_identity_set<core::ModelRoleId>(
            find_member(role, "required_model_roles"),
            child_path(path, "required_model_roles")),
    });
  }

  verify_optional_top_level_shapes(root);
  std::size_t tree_count = 0U;
  std::set<std::string> instance_ids;
  verify_tree(require_member(root, "tree", "program"), "program.tree", 0U,
              tree_count, instance_ids);

  const std::string& revision = require_string(
      require_member(root, "program_revision", "program"),
      "program.program_revision");
  if (!is_sha256_digest(revision)) {
    fail("program.program_revision: expected lowercase sha256 digest");
  }
  return core::ProgramBindingContractDTO{
      parse_program_identity<core::ProgramId>(
          require_member(root, "program_id", "program"),
          "program.program_id"),
      core::ProgramRevision(revision),
      bundle.catalogs.node_environment,
      std::move(components),
      std::move(targets),
  };
}

}  // namespace

namespace detail {

class ProgramBindingContractConstructionAuthority final {
 public:
  [[nodiscard]] static std::shared_ptr<const ProgramBindingContract> construct(
      core::ProgramBindingContractDTO value, CanonicalValue root,
      std::string bytes) {
    return std::shared_ptr<const ProgramBindingContract>(
        new ProgramBindingContract(std::move(value), std::move(root),
                                   std::move(bytes)));
  }
};

}  // namespace detail

std::shared_ptr<const ProgramBindingContract>
verify_program_binding_contract(std::string_view canonical_program_bytes,
                                const RetainedBundle& retained_bundle) {
  try {
    if (canonical_program_bytes.size() > kMaxArtifactEncodedBytes) {
      fail("canonical Program exceeds fixed artifact byte bound");
    }
    if (!verify_retained_bundle_identity(retained_bundle)) {
      fail("Program binding contract selected an invalid retained bundle");
    }
    const auto parsed = parse_strict_json_model_yaml(canonical_program_bytes);
    CanonicalValue root = parsed.root();
    if (canonical_json(root) != canonical_program_bytes) {
      fail("Program binding input is not schema-normalized RFC 8785 JSON");
    }
    core::ProgramBindingContractDTO value =
        extract_contract(root, retained_bundle);
    if (!detail::verify_content_identity(
            root,
            detail::ContentIdentitySpec{
                .identity_domain = "program-revision",
                .api_version = std::string(core::kProgramApiVersion),
                .own_field = "program_revision",
            })) {
      fail("ProgramRevision does not match the complete canonical Program");
    }
    return detail::ProgramBindingContractConstructionAuthority::construct(
        std::move(value), std::move(root),
        std::string(canonical_program_bytes));
  } catch (const ProgramContractError&) {
    throw;
  } catch (const std::exception& error) {
    fail(std::string("Program binding-contract verification failed: ") +
         error.what());
  }
}

}  // namespace manyforge::deployment
