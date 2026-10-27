#include "activation_pose_source_internal.hpp"

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <stdexcept>

#include "content_identity_internal.hpp"

namespace manyforge::deployment::detail {
namespace {
using Value = CanonicalValue;
using Object = Value::Object;
using Array = Value::Array;
constexpr auto kPoseSourceConfigurationSchema =
    "manyforge.value_schema.runtime.pose_source_execution.v1";
constexpr auto kPoseCapability = "scene.pose.observe";
constexpr auto kPoseTarget = "poses";

const Value &member(const Value &value, std::string_view name) {
  const auto *result = value.find_object_member(name);
  if (!result)
    throw std::invalid_argument("pose-source configuration missing " +
                                std::string(name));
  return *result;
}

Value with(Value value, std::string_view field, Value selected) {
  auto object = value.as_object();
  for (auto &[name, current] : object) {
    if (name == field) {
      current = std::move(selected);
      return Value(std::move(object));
    }
  }
  object.emplace_back(field, std::move(selected));
  return Value(std::move(object));
}

Value normalize_configuration(const Value &value) {
  for (const auto &entry :
       build_qualified_closed_schema_registry().value_schemas()) {
    if (entry.schema_id().value() == kPoseSourceConfigurationSchema)
      return validate_and_normalize_build_qualified_value(entry, value)
          .normalized_value();
  }
  throw std::invalid_argument(
      "pose-source configuration schema is not qualified by this build");
}

const QualifiedCapabilityDefinition &
pose_capability(std::span<const QualifiedCapabilityDefinition> capabilities) {
  const auto *builtin = core::find_builtin_capability_definition(kPoseCapability);
  if (!builtin)
    throw std::invalid_argument(
        "pose capability is not qualified by this build");
  for (const auto &candidate : capabilities) {
    if (candidate.value().capability_id.value() == kPoseCapability &&
        candidate.value().capability_definition_revision.value() ==
            builtin->capability_definition_revision)
      return candidate;
  }
  throw std::invalid_argument(
      "pose capability missing from exact source catalog");
}
} // namespace

std::vector<ActivationPreparedIoType> prepare_activation_pose_source_types(
    const ActivationDeploymentIntent &intent,
    std::span<const ActivationComponentProfile> profiles,
    std::span<const QualifiedCapabilityDefinition> capabilities) {
  if (intent.components.size() != profiles.size())
    throw std::invalid_argument("pose-source component profile join differs");
  std::vector<ActivationPreparedIoType> result;
  for (std::size_t index = 0; index != profiles.size(); ++index) {
    const auto &profile = profiles[index];
    const auto &component = intent.components[index];
    if (component.component_id != profile.component_id)
      throw std::invalid_argument(
          "pose-source component profile ordering differs");
    if (profile.runtime_kind != ActivationRuntimeKind::kPoseSource)
      continue;
    const auto config = normalize_configuration(
        member(component.configuration, "runtime_configuration"));
    // The wrapper schema admits exactly one row; it must name this component.
    const auto &rows = member(config, "pose_sources").as_array();
    if (rows.size() != 1U ||
        member(rows.front(), "controllable_component_id").as_string() !=
            component.component_id)
      throw std::invalid_argument(
          "pose-source configuration does not name its actual component");
    const auto &capability = pose_capability(capabilities);
    Array target_rows{Value(Object{
        {"command_target_id", Value(kPoseTarget)},
        {"model_group_roles", Value(Array{})},
        {"frame_roles", Value(Array{})},
        {"collision_body_roles", Value(Array{})},
        {"resource_roles", Value(Array{})},
        {"capabilities",
         Value(Array{Value(Object{
             {"capability_id",
              Value(capability.value().capability_id.value())},
             {"capability_definition_revision",
              Value(capability.value()
                        .capability_definition_revision.value())}})})}})};
    Value shape(Object{{"tags", Value(Array{Value("pose_source")})},
                       {"model_roles", Value(Array{})},
                       {"model_mounts", Value(Array{})},
                       {"attachment_interfaces", Value(Array{})},
                       {"command_targets", Value(std::move(target_rows))}});
    const auto shape_digest = content_identity::framed_sha256(
        "pose-source-component-shape", core::kComponentTypeApiVersion, shape);
    const core::ComponentTypeId id("manyforge.pose_source." +
                                   shape_digest.substr(7U));
    auto document = with(
        with(with(shape, "api_version",
                  Value(std::string(core::kComponentTypeApiVersion))),
             "component_type_id", Value(id.value())),
        "component_type_revision", Value("sha256:" + std::string(64, '0')));
    const auto *schema =
        build_qualified_closed_schema_registry().find_catalog_schema_family(
            core::CatalogRecordKind::kComponentType);
    if (!schema || !schema->compiled_schema())
      throw std::logic_error("component type schema is unavailable");
    const auto normalized =
        validate_and_normalize_unqualified_artifact_candidate(
            *schema->compiled_schema(),
            parse_strict_json_model_yaml(canonical_json(document)))
            .normalized_root();
    const auto revision = compute_content_identity(
        normalized, {catalog_record_identity_domain("component_type"),
                     std::string(core::kComponentTypeApiVersion),
                     "component_type_revision"});
    document = with(normalized, "component_type_revision", Value(revision));
    const auto bytes = canonical_json(document);
    const core::ContentDigest digest(sha256_digest(bytes));
    const core::ContentAddressedStoreKey key("sha256/" +
                                             digest.value().substr(7U));
    const core::ComponentTypeCatalogRecordRefDTO reference{
        id,
        core::ComponentTypeRevision(revision),
        schema->schema_id(),
        schema->compiled_schema()->schema_revision(),
        digest,
        key};
    std::vector<QualifiedCapabilityDefinition> selected{capability};
    auto type = resolve_and_qualify_component_type(reference, key, bytes,
                                                   selected);
    result.push_back({component.component_id, std::move(type), bytes, config});
  }
  return result;
}
} // namespace manyforge::deployment::detail
