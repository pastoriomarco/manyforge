#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/capability_definition_codec.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/component_type_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/model_asset_codec.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "proof_root_transfer_test.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kCapabilitySchemaRevision =
    "sha256:2de23ede0e941349e90f0a3a344b7d240426703d250234f86e788343213727e0";
constexpr std::string_view kComponentTypeSchemaRevision =
    "sha256:fdd03c397fb792d387002df453055d9ccd7d1826ae41d3b0706e5f7c27480d87";
constexpr std::string_view kModelAssetSchemaRevision =
    "sha256:0820a2fbe627f2ccc5b76ff8d5348137d796893d3bd67707f7171fb420ec671c";

static_assert(!std::is_aggregate_v<CompositionQualifiedCellIr>);
static_assert(!std::is_default_constructible_v<CompositionQualifiedCellIr>);

[[nodiscard]] CanonicalValue string_array(
    const std::vector<std::string>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] std::string revisioned_document(
    CanonicalValue identity, std::string_view identity_domain,
    std::string_view api_version, std::string_view revision_field) {
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append(identity_domain);
  preimage.push_back('\0');
  preimage.append(api_version);
  preimage.push_back('\0');
  preimage.append(canonical_json(identity));
  auto root = identity.as_object();
  root.emplace_back(std::string(revision_field),
                    CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(root)));
}

[[nodiscard]] std::string revision_from_document(
    std::string_view document, std::string_view field) {
  const auto parsed = parse_strict_json_model_yaml(document);
  const auto* value = parsed.root().find_object_member(field);
  if (value == nullptr) {
    throw std::runtime_error("test document is missing its revision");
  }
  return std::get<std::string>(value->storage());
}

[[nodiscard]] std::string store_key_for(std::string_view digest) {
  return "sha256/" + std::string(digest.substr(7U));
}

[[nodiscard]] QualifiedCapabilityDefinition observed_state_capability() {
  constexpr std::string_view id = "state.observed.current";
  const auto* descriptor = core::find_builtin_capability_definition(id);
  if (descriptor == nullptr) {
    throw std::runtime_error("missing built-in test capability");
  }
  CanonicalValue identity(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kCapabilityDefinitionApiVersion))},
      {"capability_id", CanonicalValue(std::string(id))},
      {"scope",
       CanonicalValue(std::string(core::to_string(descriptor->scope)))},
      {"provider_binding_kind",
       CanonicalValue(
           std::string(core::to_string(descriptor->provider_binding_kind)))},
  });
  auto root = identity.as_object();
  root.emplace_back(
      "capability_definition_revision",
      CanonicalValue(
          std::string(descriptor->capability_definition_revision)));
  const auto document = canonical_json(CanonicalValue(std::move(root)));
  const core::ContentAddressedStoreKey store_key(
      "sha256/test-observed-state-capability");
  return resolve_and_qualify_capability_definition(
      {core::CapabilityId(std::string(id)),
       core::CapabilityDefinitionRevision(
           std::string(descriptor->capability_definition_revision)),
       core::SchemaId(
           std::string(core::kCapabilityDefinitionApiVersion)),
       core::SchemaRevision(std::string(kCapabilitySchemaRevision)),
       core::ContentDigest(sha256_digest(document)), store_key},
      store_key, document);
}

struct AssetSpec final {
  std::string id;
  std::string cardinality{"multiple_per_cell"};
  bool with_srdf = false;
  std::vector<std::pair<std::string, std::string>> frames{
      {"base", "base_link"}, {"tcp", "tip_link"}};
  std::vector<std::pair<std::string, std::string>> groups{
      {"arm", "arm_group"}};
  std::vector<std::pair<std::string, std::vector<std::string>>> resources{
      {"arm_joints", {"joint1"}}};
  std::vector<std::pair<std::string, std::vector<std::string>>> bodies{
      {"arm_collision", {"base_link", "tip_link"}}};
};

[[nodiscard]] std::string urdf_for(std::string_view id) {
  return "<robot name=\"" + std::string(id) +
         "\"><link name=\"base_link\"/><link name=\"tip_link\"/>"
         "<joint name=\"joint1\" type=\"revolute\">"
         "<parent link=\"base_link\"/><child link=\"tip_link\"/>"
         "<axis xyz=\"0 0 1\"/><limit lower=\"-3\" upper=\"3\" "
         "effort=\"1\" velocity=\"1\"/></joint></robot>";
}

[[nodiscard]] std::string srdf_for(std::string_view id) {
  return "<robot name=\"" + std::string(id) +
         "\"><group name=\"arm_group\"><chain base_link=\"base_link\" "
         "tip_link=\"tip_link\"/></group></robot>";
}

struct QualifiedSource final {
  SourceVerifiedModelAsset value;
  std::string document;
};

[[nodiscard]] QualifiedSource qualify_source_impl(
    const AssetSpec& input,
    std::optional<std::string> urdf_override = std::nullopt,
    std::optional<std::string> srdf_override = std::nullopt) {
  AssetSpec spec = input;
  std::sort(spec.frames.begin(), spec.frames.end());
  std::sort(spec.groups.begin(), spec.groups.end());
  std::sort(spec.resources.begin(), spec.resources.end());
  std::sort(spec.bodies.begin(), spec.bodies.end());

  const std::string urdf = urdf_override.value_or(urdf_for(spec.id));
  const std::optional<std::string> srdf =
      spec.with_srdf
          ? std::optional<std::string>(
                srdf_override.value_or(srdf_for(spec.id)))
                     : std::nullopt;
  const std::string urdf_uri = "asset://" + spec.id + "/a.urdf";
  const std::string srdf_uri = "asset://" + spec.id + "/b.srdf";

  CanonicalValue::Array artifacts{
      CanonicalValue(CanonicalValue::Object{
          {"logical_uri", CanonicalValue(urdf_uri)},
          {"media_type", CanonicalValue("application/xml")},
          {"byte_size", CanonicalValue(static_cast<double>(urdf.size()))},
          {"content_digest", CanonicalValue(sha256_digest(urdf))},
      })};
  if (srdf.has_value()) {
    artifacts.emplace_back(CanonicalValue::Object{
        {"logical_uri", CanonicalValue(srdf_uri)},
        {"media_type", CanonicalValue("application/xml")},
        {"byte_size", CanonicalValue(static_cast<double>(srdf->size()))},
        {"content_digest", CanonicalValue(sha256_digest(*srdf))},
    });
  }
  CanonicalValue::Object expanded{
      {"urdf_logical_uri", CanonicalValue(urdf_uri)},
      {"artifacts", CanonicalValue(std::move(artifacts))},
  };
  if (srdf.has_value()) {
    expanded.emplace_back("srdf_logical_uri", CanonicalValue(srdf_uri));
  }

  CanonicalValue::Array frames;
  for (const auto& [role, source] : spec.frames) {
    frames.emplace_back(CanonicalValue::Object{
        {"frame_role_id", CanonicalValue(role)},
        {"source_link_name", CanonicalValue(source)},
    });
  }
  CanonicalValue::Array groups;
  if (spec.with_srdf) {
    for (const auto& [role, source] : spec.groups) {
      groups.emplace_back(CanonicalValue::Object{
          {"model_group_role_id", CanonicalValue(role)},
          {"source_group_name", CanonicalValue(source)},
      });
    }
  }
  CanonicalValue::Array resources;
  for (const auto& [role, joints] : spec.resources) {
    resources.emplace_back(CanonicalValue::Object{
        {"resource_role_id", CanonicalValue(role)},
        {"source_joint_names", string_array(joints)},
    });
  }
  CanonicalValue::Array bodies;
  for (const auto& [role, links] : spec.bodies) {
    auto sorted_links = links;
    std::sort(sorted_links.begin(), sorted_links.end());
    bodies.emplace_back(CanonicalValue::Object{
        {"collision_body_role_id", CanonicalValue(role)},
        {"source_link_names", string_array(sorted_links)},
    });
  }

  const auto document = revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kModelAssetApiVersion))},
          {"model_asset_id", CanonicalValue(spec.id)},
          {"instance_cardinality", CanonicalValue(spec.cardinality)},
          {"expanded_source", CanonicalValue(std::move(expanded))},
          {"root_frame_role_id", CanonicalValue("base")},
          {"frame_role_bindings", CanonicalValue(std::move(frames))},
          {"model_group_role_bindings", CanonicalValue(std::move(groups))},
          {"resource_role_bindings",
           CanonicalValue(std::move(resources))},
          {"collision_body_role_bindings",
           CanonicalValue(std::move(bodies))},
      }),
      "catalog-record-revision/model_asset",
      core::kModelAssetApiVersion, "model_revision");
  const auto revision =
      revision_from_document(document, "model_revision");
  const core::ContentAddressedStoreKey catalog_key(
      "sha256/test-model-" + spec.id + "-" +
      revision.substr(revision.size() - 8U));
  const auto qualified = resolve_and_qualify_model_asset(
      {core::ModelAssetId(spec.id), core::ModelRevision(revision),
       core::SchemaId(std::string(core::kModelAssetApiVersion)),
       core::SchemaRevision(std::string(kModelAssetSchemaRevision)),
       core::ContentDigest(sha256_digest(document)), catalog_key},
      catalog_key, document);

  std::vector<ResolvedModelSourceArtifactCandidate> closure{
      {urdf_uri,
       core::ContentAddressedStoreKey(
           store_key_for(sha256_digest(urdf))),
       urdf}};
  if (srdf.has_value()) {
    closure.push_back(
        {srdf_uri,
         core::ContentAddressedStoreKey(
             store_key_for(sha256_digest(*srdf))),
         *srdf});
  }
  return {verify_model_source_closure(qualified, std::move(closure)),
          document};
}

[[nodiscard]] QualifiedSource qualify_source(const AssetSpec& input) {
  return qualify_source_impl(input);
}

[[nodiscard]] QualifiedSource qualify_source_with_documents(
    const AssetSpec& input, std::string urdf,
    std::optional<std::string> srdf = std::nullopt) {
  return qualify_source_impl(input, std::move(urdf), std::move(srdf));
}

struct TypeSpec final {
  std::string id;
  std::vector<std::string> tags;
  std::vector<std::string> model_roles{"body"};
  std::string root_role{"body"};
  std::string root_frame{"base"};
  bool with_target = false;
  std::string target_model_role{"body"};
  std::string target_group_source{"arm"};
  std::string target_resource_source{"arm_joints"};
  std::string target_frame_source{"tcp"};
  std::string target_body_source{"arm_collision"};
  bool with_interface = false;
  std::string interface_id{"flange"};
  std::string interface_model_role{"body"};
  std::string interface_frame{"tcp"};
  std::vector<std::string> accepts_tags{"robot-tool"};
  std::uint64_t max_attachments = 1U;
  std::optional<core::ComponentTypeModelMountDTO> mount{std::nullopt};
};

struct QualifiedType final {
  QualifiedComponentType value;
  std::string document;
};

[[nodiscard]] QualifiedType qualify_type_impl(
    const TypeSpec& spec,
    std::vector<std::pair<std::string, std::string>>
        target_resource_roles = {},
    std::vector<std::string> component_local_resource_roles = {}) {
  CanonicalValue::Array roles;
  for (const auto& role : spec.model_roles) {
    roles.emplace_back(CanonicalValue::Object{
        {"model_role_id", CanonicalValue(role)},
        {"cardinality", CanonicalValue("exactly_one")},
    });
  }
  CanonicalValue::Array mounts;
  if (spec.mount.has_value()) {
    const auto& mount = *spec.mount;
    mounts.emplace_back(CanonicalValue::Object{
        {"parent",
         CanonicalValue(CanonicalValue::Object{
             {"model_role_id",
              CanonicalValue(mount.parent.model_role_id.value())},
             {"frame_role_id",
              CanonicalValue(mount.parent.frame_role_id.value())},
         })},
        {"child",
         CanonicalValue(CanonicalValue::Object{
             {"model_role_id",
              CanonicalValue(mount.child.model_role_id.value())},
             {"frame_role_id",
              CanonicalValue(mount.child.frame_role_id.value())},
         })},
        {"child_frame_pose_in_parent_frame",
         CanonicalValue(CanonicalValue::Object{
             {"position_m",
              CanonicalValue(CanonicalValue::Array{
                  CanonicalValue(
                      mount.child_frame_pose_in_parent_frame.position_m[0]),
                  CanonicalValue(
                      mount.child_frame_pose_in_parent_frame.position_m[1]),
                  CanonicalValue(
                      mount.child_frame_pose_in_parent_frame.position_m[2])})},
             {"orientation_xyzw",
              CanonicalValue(CanonicalValue::Array{
                  CanonicalValue(mount.child_frame_pose_in_parent_frame
                                     .orientation_xyzw[0]),
                  CanonicalValue(mount.child_frame_pose_in_parent_frame
                                     .orientation_xyzw[1]),
                  CanonicalValue(mount.child_frame_pose_in_parent_frame
                                     .orientation_xyzw[2]),
                  CanonicalValue(mount.child_frame_pose_in_parent_frame
                                     .orientation_xyzw[3])})},
         })},
    });
  }

  CanonicalValue::Array targets;
  std::vector<QualifiedCapabilityDefinition> capabilities;
  if (spec.with_target) {
    auto capability = observed_state_capability();
    const auto revision =
        capability.value().capability_definition_revision.value();
    CanonicalValue::Array target_resources;
    const auto resource_roles =
        target_resource_roles.empty() &&
                component_local_resource_roles.empty()
            ? std::vector<std::pair<std::string, std::string>>{
                  {"joints", spec.target_resource_source}}
            : std::move(target_resource_roles);
    for (const auto& [alias, source] : resource_roles) {
      target_resources.emplace_back(CanonicalValue::Object{
          {"resource_role_id", CanonicalValue(alias)},
          {"source_kind", CanonicalValue("model_role")},
          {"model_role_id", CanonicalValue(spec.target_model_role)},
          {"asset_resource_role_id", CanonicalValue(source)},
      });
    }
    for (const auto& role : component_local_resource_roles) {
      target_resources.emplace_back(CanonicalValue::Object{
          {"resource_role_id", CanonicalValue(role)},
          {"source_kind", CanonicalValue("component_local")},
      });
    }
    targets.emplace_back(CanonicalValue::Object{
        {"command_target_id", CanonicalValue("command")},
        {"model_group_roles",
         CanonicalValue(CanonicalValue::Array{
             CanonicalValue(CanonicalValue::Object{
                 {"model_group_role_id", CanonicalValue("planning_group")},
                 {"model_role_id",
                  CanonicalValue(spec.target_model_role)},
                 {"asset_model_group_role_id",
                  CanonicalValue(spec.target_group_source)},
             })})},
        {"resource_roles", CanonicalValue(std::move(target_resources))},
        {"frame_roles",
         CanonicalValue(CanonicalValue::Array{
             CanonicalValue(CanonicalValue::Object{
                 {"frame_role_id", CanonicalValue("tcp")},
                 {"model_role_id",
                  CanonicalValue(spec.target_model_role)},
                 {"asset_frame_role_id",
                  CanonicalValue(spec.target_frame_source)},
             })})},
        {"collision_body_roles",
         CanonicalValue(CanonicalValue::Array{
             CanonicalValue(CanonicalValue::Object{
                 {"collision_body_role_id",
                  CanonicalValue("collision")},
                 {"model_role_id",
                  CanonicalValue(spec.target_model_role)},
                 {"asset_collision_body_role_id",
                  CanonicalValue(spec.target_body_source)},
             })})},
        {"capabilities",
         CanonicalValue(CanonicalValue::Array{
             CanonicalValue(CanonicalValue::Object{
                 {"capability_id",
                  CanonicalValue("state.observed.current")},
                 {"capability_definition_revision",
                  CanonicalValue(revision)},
             })})},
    });
    capabilities.push_back(std::move(capability));
  }

  CanonicalValue::Array interfaces;
  if (spec.with_interface) {
    interfaces.emplace_back(CanonicalValue::Object{
        {"attachment_interface_id", CanonicalValue(spec.interface_id)},
        {"model_role_id",
         CanonicalValue(spec.interface_model_role)},
        {"frame_role_id", CanonicalValue(spec.interface_frame)},
        {"accepts_tags", string_array(spec.accepts_tags)},
        {"max_attachments",
         CanonicalValue(static_cast<double>(spec.max_attachments))},
    });
  }

  CanonicalValue::Object identity{
      {"api_version",
       CanonicalValue(std::string(core::kComponentTypeApiVersion))},
      {"component_type_id", CanonicalValue(spec.id)},
      {"tags", string_array(spec.tags)},
      {"model_roles", CanonicalValue(std::move(roles))},
      {"component_root",
       CanonicalValue(CanonicalValue::Object{
           {"model_role_id", CanonicalValue(spec.root_role)},
           {"frame_role_id", CanonicalValue(spec.root_frame)},
       })},
      {"model_mounts", CanonicalValue(std::move(mounts))},
      {"command_targets", CanonicalValue(std::move(targets))},
      {"attachment_interfaces", CanonicalValue(std::move(interfaces))},
  };
  const auto document = revisioned_document(
      CanonicalValue(std::move(identity)),
      "catalog-record-revision/component_type",
      core::kComponentTypeApiVersion, "component_type_revision");
  const auto revision =
      revision_from_document(document, "component_type_revision");
  const core::ContentAddressedStoreKey store_key(
      "sha256/test-type-" + spec.id + "-" +
      revision.substr(revision.size() - 8U));
  return {
      resolve_and_qualify_component_type(
          {core::ComponentTypeId(spec.id),
           core::ComponentTypeRevision(revision),
           core::SchemaId(std::string(core::kComponentTypeApiVersion)),
           core::SchemaRevision(
               std::string(kComponentTypeSchemaRevision)),
           core::ContentDigest(sha256_digest(document)), store_key},
          store_key, document, capabilities),
      document};
}

[[nodiscard]] QualifiedType qualify_type(const TypeSpec& spec) {
  return qualify_type_impl(spec);
}

[[nodiscard]] QualifiedType qualify_type_with_resources(
    const TypeSpec& spec,
    std::vector<std::pair<std::string, std::string>> resources) {
  return qualify_type_impl(spec, std::move(resources));
}

[[nodiscard]] QualifiedType qualify_type_with_component_local_resources(
    const TypeSpec& spec,
    std::vector<std::pair<std::string, std::string>> model_resources,
    std::vector<std::string> component_local_resources) {
  return qualify_type_impl(spec, std::move(model_resources),
                           std::move(component_local_resources));
}

struct ModelSpec final {
  std::string instance_id;
  const QualifiedSource* source;
  bool passive = false;
};

struct ComponentSpec final {
  std::string id;
  const QualifiedType* type;
  std::vector<std::pair<std::string, std::string>> model_bindings;
  bool placed = false;
};

struct AttachmentSpec final {
  std::string id;
  std::string parent_component;
  std::string parent_interface;
  std::string child_component;
  std::string child_interface;
};

struct BindingSpec final {
  std::string id;
  std::string component_id;
  std::string target_id{"command"};
  std::vector<std::pair<std::string, std::string>> resources;
};

[[nodiscard]] CanonicalValue identity_pose() {
  return CanonicalValue(CanonicalValue::Object{
      {"position_m",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(0.0), CanonicalValue(0.0),
           CanonicalValue(0.0)})},
      {"orientation_xyzw",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(0.0), CanonicalValue(0.0),
           CanonicalValue(0.0), CanonicalValue(1.0)})},
  });
}

[[nodiscard]] CanonicalValue pose_value(
    const core::PlacementPoseDTO& pose) {
  return CanonicalValue(CanonicalValue::Object{
      {"position_m",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(pose.position_m[0]),
           CanonicalValue(pose.position_m[1]),
           CanonicalValue(pose.position_m[2])})},
      {"orientation_xyzw",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(pose.orientation_xyzw[0]),
           CanonicalValue(pose.orientation_xyzw[1]),
           CanonicalValue(pose.orientation_xyzw[2]),
           CanonicalValue(pose.orientation_xyzw[3])})},
  });
}

[[nodiscard]] GraphRevisionVerifiedCellGraph graph_for(
    std::vector<ModelSpec> models, std::vector<ComponentSpec> components,
    std::vector<AttachmentSpec> attachments = {},
    std::vector<BindingSpec> control_bindings = {},
    std::map<std::string, core::PlacementPoseDTO, std::less<>>
        model_poses = {},
    std::map<std::string, core::PlacementPoseDTO, std::less<>>
        component_poses = {},
    std::map<std::string, core::PlacementPoseDTO, std::less<>>
        attachment_poses = {}) {
  std::sort(models.begin(), models.end(),
            [](const ModelSpec& left, const ModelSpec& right) {
              return left.instance_id < right.instance_id;
            });
  std::sort(components.begin(), components.end(),
            [](const ComponentSpec& left, const ComponentSpec& right) {
              return left.id < right.id;
            });
  std::sort(attachments.begin(), attachments.end(),
            [](const AttachmentSpec& left, const AttachmentSpec& right) {
              return left.id < right.id;
            });
  std::sort(control_bindings.begin(), control_bindings.end(),
            [](const BindingSpec& left, const BindingSpec& right) {
              return left.id < right.id;
            });

  CanonicalValue::Array model_rows;
  for (const auto& model : models) {
    CanonicalValue::Object row{
        {"model_instance_id", CanonicalValue(model.instance_id)},
        {"model_asset_id",
         CanonicalValue(model.source->value.value().model_asset_id.value())},
        {"model_revision",
         CanonicalValue(model.source->value.value().model_revision.value())},
    };
    if (model.passive) {
      const auto pose = model_poses.find(model.instance_id);
      row.emplace_back("root_pose_in_cell",
                       pose == model_poses.end()
                           ? identity_pose()
                           : pose_value(pose->second));
    }
    model_rows.emplace_back(std::move(row));
  }
  CanonicalValue::Array component_rows;
  for (auto component : components) {
    std::sort(component.model_bindings.begin(),
              component.model_bindings.end());
    CanonicalValue::Array bindings;
    for (const auto& [role, instance] : component.model_bindings) {
      bindings.emplace_back(CanonicalValue::Object{
          {"model_role_id", CanonicalValue(role)},
          {"model_instance_id", CanonicalValue(instance)},
      });
    }
    CanonicalValue::Object row{
        {"controllable_component_id", CanonicalValue(component.id)},
        {"component_type_id",
         CanonicalValue(
             component.type->value.value().component_type_id.value())},
        {"component_type_revision",
         CanonicalValue(
             component.type->value.value().component_type_revision.value())},
        {"model_role_bindings", CanonicalValue(std::move(bindings))},
        {"runtime_unit_ids", CanonicalValue(CanonicalValue::Array{})},
    };
    if (component.placed) {
      const auto pose = component_poses.find(component.id);
      row.emplace_back("root_pose_in_cell",
                       pose == component_poses.end()
                           ? identity_pose()
                           : pose_value(pose->second));
    }
    component_rows.emplace_back(std::move(row));
  }
  CanonicalValue::Array attachment_rows;
  for (const auto& attachment : attachments) {
    const auto pose = attachment_poses.find(attachment.id);
    attachment_rows.emplace_back(CanonicalValue::Object{
        {"component_attachment_id", CanonicalValue(attachment.id)},
        {"component_attachment_revision",
         CanonicalValue(std::string(kZeroDigest))},
        {"parent",
         CanonicalValue(CanonicalValue::Object{
             {"controllable_component_id",
              CanonicalValue(attachment.parent_component)},
             {"attachment_interface_id",
              CanonicalValue(attachment.parent_interface)},
         })},
        {"child",
         CanonicalValue(CanonicalValue::Object{
             {"controllable_component_id",
              CanonicalValue(attachment.child_component)},
             {"attachment_interface_id",
              CanonicalValue(attachment.child_interface)},
         })},
        {"child_interface_pose_in_parent_interface",
         pose == attachment_poses.end()
             ? identity_pose()
             : pose_value(pose->second)},
    });
  }
  CanonicalValue::Array domain_rows;
  if (!control_bindings.empty()) {
    domain_rows.emplace_back(CanonicalValue::Object{
        {"control_domain_id", CanonicalValue("test_domain")},
        {"control_domain_revision", CanonicalValue(std::string(kZeroDigest))},
        {"runtime_unit_ids", CanonicalValue(CanonicalValue::Array{})},
        {"control_coordinator_endpoint_role", CanonicalValue("coordinator")},
        {"controller_topology_id", CanonicalValue("test_topology")},
        {"topology_revision", CanonicalValue(std::string(kZeroDigest))},
        {"containment_granularity", CanonicalValue("resource")},
    });
  }
  CanonicalValue::Array control_binding_rows;
  for (const auto& binding : control_bindings) {
    CanonicalValue::Array resources;
    for (const auto& [role, resource] : binding.resources) {
      resources.emplace_back(CanonicalValue::Object{
          {"resource_role_id", CanonicalValue(role)},
          {"resource_id", CanonicalValue(resource)},
      });
    }
    control_binding_rows.emplace_back(CanonicalValue::Object{
        {"control_binding_id", CanonicalValue(binding.id)},
        {"control_binding_revision", CanonicalValue(std::string(kZeroDigest))},
        {"controllable_component_id", CanonicalValue(binding.component_id)},
        {"command_target_id", CanonicalValue(binding.target_id)},
        {"capability_id", CanonicalValue("state.observed.current")},
        {"capability_definition_revision",
         CanonicalValue(std::string(kZeroDigest))},
        {"control_domain_id", CanonicalValue("test_domain")},
        {"resource_bindings", CanonicalValue(std::move(resources))},
        {"control_interface_contract_id", CanonicalValue("test_interface")},
        {"control_interface_revision", CanonicalValue(std::string(kZeroDigest))},
        {"controller_topology_id", CanonicalValue("test_topology")},
        {"topology_revision", CanonicalValue(std::string(kZeroDigest))},
        {"legal_control_route_ids",
         CanonicalValue(CanonicalValue::Array{
             CanonicalValue("test_route")})},
        {"route_endpoint_projections",
         CanonicalValue(CanonicalValue::Array{
             CanonicalValue(CanonicalValue::Object{
                 {"control_route_id", CanonicalValue("test_route")},
                 {"command_endpoint_role_ids",
                  CanonicalValue(CanonicalValue::Array{})},
                 {"state_endpoint_role_ids",
                  CanonicalValue(CanonicalValue::Array{})},
             })})},
        {"provider_instance_id", CanonicalValue("test_provider")},
        {"provider_manifest_id", CanonicalValue("test_manifest")},
        {"provider_manifest_revision", CanonicalValue(std::string(kZeroDigest))},
        {"state_authority_id", CanonicalValue("test_authority")},
        {"state_authority_class", CanonicalValue("measured")},
        {"stop_evidence_policy_id", CanonicalValue("test_stop")},
        {"stop_evidence_policy_revision",
         CanonicalValue(std::string(kZeroDigest))},
        {"containment_granularity", CanonicalValue("resource")},
    });
  }

  const auto parsed = parse_strict_json_model_yaml(canonical_json(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kCellGraphApiVersion))},
          {"graph_revision", CanonicalValue(std::string(kZeroDigest))},
          {"cell_id", CanonicalValue("structural_test")},
          {"runtime_units", CanonicalValue(CanonicalValue::Array{})},
          {"model_instances", CanonicalValue(std::move(model_rows))},
          {"controllable_components",
           CanonicalValue(std::move(component_rows))},
          {"control_domains", CanonicalValue(std::move(domain_rows))},
          {"control_bindings",
           CanonicalValue(std::move(control_binding_rows))},
          {"component_attachments",
           CanonicalValue(std::move(attachment_rows))},
      })));
  const auto intrinsic = validate_intrinsic_cell_graph_candidate(
      decode_cell_graph_candidate(parsed));
  return compute_graph_revision(prepare_cell_graph_identity(intrinsic, {}));
}

[[nodiscard]] std::vector<SourceVerifiedModelAsset> source_values(
    std::initializer_list<const QualifiedSource*> sources) {
  std::vector<SourceVerifiedModelAsset> result;
  for (const auto* source : sources) {
    result.push_back(source->value);
  }
  return result;
}

[[nodiscard]] std::vector<QualifiedComponentType> type_values(
    std::initializer_list<const QualifiedType*> types) {
  std::vector<QualifiedComponentType> result;
  for (const auto* type : types) {
    result.push_back(type->value);
  }
  return result;
}

TEST(CellStructure, QualifiesTheEmptyGraphWithExactEmptyClosures) {
  const auto graph = graph_for({}, {});
  const auto qualified = compose_cell_occurrences(
      graph, std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});

  EXPECT_EQ(qualified.graph(), graph.graph());
  EXPECT_TRUE(qualified.model_assets().empty());
  EXPECT_TRUE(qualified.component_types().empty());
  EXPECT_TRUE(qualified.model_occurrences().empty());
  EXPECT_TRUE(qualified.targets().empty());
  EXPECT_TRUE(qualified.fixed_connections().empty());
}

TEST(CellStructure,
     ResolvesOwnedAttachedAndPassiveOccurrencesWithoutChangingGraphIdentity) {
  auto arm_source = qualify_source(
      AssetSpec{.id = "arm_asset", .with_srdf = true});
  auto tool_source = qualify_source(
      AssetSpec{.id = "tool_asset",
                .groups = {},
                .resources = {},
                .bodies = {{"tool_collision", {"tip_link"}}}});
  auto fixture_source = qualify_source(
      AssetSpec{.id = "fixture_asset",
                .groups = {},
                .resources = {},
                .bodies = {{"fixture_collision", {"base_link"}}}});

  auto arm_type = qualify_type(
      TypeSpec{.id = "arm_type",
               .tags = {"articulated-arm"},
               .with_target = true,
               .with_interface = true});
  auto tool_type = qualify_type(
      TypeSpec{.id = "tool_type",
               .tags = {"robot-tool"},
               .root_frame = "base",
               .with_interface = true,
               .interface_id = "mount",
               .interface_frame = "base",
               .accepts_tags = {"articulated-arm"}});

  const auto graph = graph_for(
      {{"arm_model", &arm_source, false},
       {"fixture_model", &fixture_source, true},
       {"tool_model", &tool_source, false}},
      {{"arm", &arm_type, {{"body", "arm_model"}}, true},
       {"tool", &tool_type, {{"body", "tool_model"}}, false}},
      {{"tool_on_arm", "arm", "flange", "tool", "mount"}});
  const auto graph_before = graph.graph();
  const auto root_before = graph.normalized_root();
  const auto sources =
      source_values({&tool_source, &fixture_source, &arm_source});
  const auto types = type_values({&tool_type, &arm_type});

  const auto qualified = compose_cell_occurrences(graph, sources, types);

  EXPECT_EQ(qualified.graph(), graph_before);
  EXPECT_EQ(qualified.verified_graph().normalized_root(), root_before);
  ASSERT_EQ(qualified.model_assets().size(), 3U);
  EXPECT_EQ(qualified.model_assets()[0].value().model_asset_id.value(),
            "arm_asset");
  EXPECT_EQ(qualified.model_assets()[0].value().model_revision,
            arm_source.value.value().model_revision);
  EXPECT_EQ(qualified.model_assets()[0].retrieval_plan().artifacts(),
            arm_source.value.retrieval_plan().artifacts());
  EXPECT_EQ(qualified.model_assets()[0].retrieval_plan().total_bytes(),
            arm_source.value.retrieval_plan().total_bytes());
  EXPECT_EQ(qualified.model_assets()[1].value().model_asset_id.value(),
            "fixture_asset");
  EXPECT_EQ(qualified.model_assets()[1].value().model_revision,
            fixture_source.value.value().model_revision);
  EXPECT_EQ(qualified.model_assets()[1].retrieval_plan().artifacts(),
            fixture_source.value.retrieval_plan().artifacts());
  EXPECT_EQ(qualified.model_assets()[1].retrieval_plan().total_bytes(),
            fixture_source.value.retrieval_plan().total_bytes());
  EXPECT_EQ(qualified.model_assets()[2].value().model_asset_id.value(),
            "tool_asset");
  EXPECT_EQ(qualified.model_assets()[2].value().model_revision,
            tool_source.value.value().model_revision);
  EXPECT_EQ(qualified.model_assets()[2].retrieval_plan().artifacts(),
            tool_source.value.retrieval_plan().artifacts());
  EXPECT_EQ(qualified.model_assets()[2].retrieval_plan().total_bytes(),
            tool_source.value.retrieval_plan().total_bytes());

  ASSERT_EQ(qualified.component_types().size(), 2U);
  EXPECT_EQ(
      qualified.component_types()[0].value().component_type_id.value(),
      "arm_type");
  EXPECT_EQ(qualified.component_types()[0].value().component_type_revision,
            arm_type.value.value().component_type_revision);
  EXPECT_EQ(
      qualified.component_types()[1].value().component_type_id.value(),
      "tool_type");
  EXPECT_EQ(qualified.component_types()[1].value().component_type_revision,
            tool_type.value.value().component_type_revision);

  test::expect_source_preserving_rvalue_transfers(
      arm_type.value, [](const QualifiedComponentType& value) {
        return std::tuple{value.reference(), value.value(),
                          value.normalized_root()};
      });
  test::expect_source_preserving_rvalue_transfers(
      qualified, [](const CompositionQualifiedCellIr& value) {
        return std::tuple{
            value.verified_graph().normalized_root(),
            emit_canonical_urdf_document(value.flattened_urdf()),
            emit_canonical_srdf_document(value.flattened_srdf()),
            value.model_occurrences(), value.fixed_connections(),
            value.targets()};
      });

  auto graph_source = graph;
  const auto graph_destination = std::move(graph_source);
  const auto source_composition =
      compose_cell_occurrences(graph_source, sources, types);
  const auto destination_composition =
      compose_cell_occurrences(graph_destination, sources, types);
  EXPECT_EQ(source_composition.verified_graph().normalized_root(),
            destination_composition.verified_graph().normalized_root());
  EXPECT_EQ(source_composition.model_assets().size(),
            destination_composition.model_assets().size());
  EXPECT_EQ(source_composition.component_types().size(),
            destination_composition.component_types().size());
  EXPECT_EQ(source_composition.model_occurrences(),
            destination_composition.model_occurrences());

  ASSERT_EQ(qualified.model_occurrences().size(), 3U);
  EXPECT_EQ(qualified.model_occurrences()[0].owner_component_id,
            std::optional<core::ControllableComponentId>(
                core::ControllableComponentId("arm")));
  EXPECT_FALSE(
      qualified.model_occurrences()[1].owner_component_id.has_value());
  EXPECT_EQ(qualified.model_occurrences()[2].owner_component_id,
            std::optional<core::ControllableComponentId>(
                core::ControllableComponentId("tool")));
  ASSERT_EQ(qualified.targets().size(), 1U);
  ASSERT_EQ(qualified.targets()[0].model_groups.size(), 1U);
  EXPECT_EQ(qualified.targets()[0].model_groups[0].model_instance_id.value(),
            "arm_model");
  EXPECT_FALSE(
      qualified.targets()[0].model_groups[0].compiled_group_name.empty());
  ASSERT_EQ(qualified.fixed_connections().size(), 3U);
  EXPECT_TRUE(std::holds_alternative<CompositionDirectComponentAnchor>(
      qualified.fixed_connections()[0].source));
  EXPECT_TRUE(std::holds_alternative<CompositionDirectModelAnchor>(
      qualified.fixed_connections()[1].source));
  const auto* attachment =
      std::get_if<CompositionComponentAttachment>(
          &qualified.fixed_connections()[2].source);
  ASSERT_NE(attachment, nullptr);
  EXPECT_EQ(attachment->component_attachment_id.value(), "tool_on_arm");
  EXPECT_EQ(attachment->parent_component_id.value(), "arm");
  EXPECT_EQ(attachment->child_component_id.value(), "tool");
}

TEST(CellStructure, RequiresExactModelAndComponentTypeDependencyClosures) {
  auto source =
      qualify_source(AssetSpec{.id = "asset", .with_srdf = true});
  auto type = qualify_type(
      TypeSpec{.id = "type", .tags = {}, .with_target = true});
  const auto graph = graph_for(
      {{"model", &source, false}},
      {{"component", &type, {{"body", "model"}}, true}});
  const auto sources = source_values({&source});
  const auto types = type_values({&type});

  EXPECT_THROW(
      (void)compose_cell_occurrences(
          graph, std::span<const SourceVerifiedModelAsset>{}, types),
      CellCompositionError);
  const std::vector<SourceVerifiedModelAsset> duplicate_sources{
      source.value, source.value};
  EXPECT_THROW(
      (void)compose_cell_occurrences(graph, duplicate_sources, types),
      CellCompositionError);
  auto stale_source = qualify_source(
      AssetSpec{.id = "asset",
                .cardinality = "one_per_cell",
                .with_srdf = true});
  const auto stale_sources = source_values({&stale_source});
  EXPECT_THROW(
      (void)compose_cell_occurrences(graph, stale_sources, types),
      CellCompositionError);
  auto extra_source =
      qualify_source(AssetSpec{.id = "extra", .with_srdf = true});
  const auto extra_sources = source_values({&source, &extra_source});
  EXPECT_THROW(
      (void)compose_cell_occurrences(graph, extra_sources, types),
      CellCompositionError);

  EXPECT_THROW(
      (void)compose_cell_occurrences(
          graph, sources, std::span<const QualifiedComponentType>{}),
      CellCompositionError);
  const std::vector<QualifiedComponentType> duplicate_types{
      type.value, type.value};
  EXPECT_THROW(
      (void)compose_cell_occurrences(graph, sources, duplicate_types),
      CellCompositionError);
  auto stale_type = qualify_type(
      TypeSpec{.id = "type", .tags = {"changed"}, .with_target = true});
  const auto stale_types = type_values({&stale_type});
  EXPECT_THROW(
      (void)compose_cell_occurrences(graph, sources, stale_types),
      CellCompositionError);
  auto extra_type =
      qualify_type(TypeSpec{.id = "extra_type", .tags = {}});
  const auto extra_types = type_values({&type, &extra_type});
  EXPECT_THROW(
      (void)compose_cell_occurrences(graph, sources, extra_types),
      CellCompositionError);
}

TEST(CellStructure, EnforcesAssetInstanceCardinalityAndRoleBijection) {
  auto singleton = qualify_source(
      AssetSpec{.id = "singleton",
                .cardinality = "one_per_cell",
                .groups = {},
                .resources = {}});
  const auto two_passive = graph_for(
      {{"model_a", &singleton, true}, {"model_b", &singleton, true}}, {});
  const auto singleton_sources = source_values({&singleton});
  EXPECT_THROW(
      (void)compose_cell_occurrences(
          two_passive, singleton_sources,
          std::span<const QualifiedComponentType>{}),
      CellCompositionError);

  auto source = qualify_source(AssetSpec{.id = "asset"});
  auto type = qualify_type(TypeSpec{.id = "type", .tags = {}});
  const auto wrong_role = graph_for(
      {{"model", &source, false}},
      {{"component", &type, {{"wrong", "model"}}, true}});
  const auto sources = source_values({&source});
  const auto types = type_values({&type});
  EXPECT_THROW(
      (void)compose_cell_occurrences(wrong_role, sources, types),
      CellCompositionError);
}

TEST(CellStructure, ProvesRootMountAndCommandTargetSourceRoles) {
  auto source =
      qualify_source(AssetSpec{.id = "asset", .with_srdf = true});

  auto wrong_root_type = qualify_type(
      TypeSpec{.id = "wrong_root", .tags = {}, .root_frame = "tcp"});
  const auto wrong_root_graph = graph_for(
      {{"model", &source, false}},
      {{"component", &wrong_root_type, {{"body", "model"}}, true}});
  const auto sources = source_values({&source});
  auto wrong_root_types = type_values({&wrong_root_type});
  EXPECT_THROW(
      (void)compose_cell_occurrences(
          wrong_root_graph, sources, wrong_root_types),
      CellCompositionError);

  auto missing_target_type = qualify_type(
      TypeSpec{.id = "missing_target",
               .tags = {},
               .with_target = true,
               .target_group_source = "missing_group"});
  const auto missing_target_graph = graph_for(
      {{"model", &source, false}},
      {{"component", &missing_target_type, {{"body", "model"}}, true}});
  auto missing_target_types = type_values({&missing_target_type});
  EXPECT_THROW(
      (void)compose_cell_occurrences(
          missing_target_graph, sources, missing_target_types),
      CellCompositionError);

  auto child_source = qualify_source(AssetSpec{.id = "child_asset"});
  const core::PlacementPoseDTO identity_pose_dto{
      {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  TypeSpec good_mount{
      .id = "good_mount",
      .tags = {},
      .model_roles = {"base", "tool"},
      .root_role = "base",
  };
  good_mount.mount = core::ComponentTypeModelMountDTO{
      {core::ModelRoleId("base"), core::FrameRoleId("tcp")},
      {core::ModelRoleId("tool"), core::FrameRoleId("base")},
      identity_pose_dto};
  auto good_mount_type = qualify_type(good_mount);
  const auto good_mount_graph = graph_for(
      {{"base_model", &source, false},
       {"tool_model", &child_source, false}},
      {{"component",
        &good_mount_type,
        {{"base", "base_model"}, {"tool", "tool_model"}},
        true}});
  const auto mount_sources = source_values({&source, &child_source});
  const auto good_mount_types = type_values({&good_mount_type});
  const auto mounted = compose_cell_occurrences(
      good_mount_graph, mount_sources, good_mount_types);
  ASSERT_EQ(mounted.fixed_connections().size(), 2U);
  EXPECT_TRUE(std::holds_alternative<CompositionDirectComponentAnchor>(
      mounted.fixed_connections()[0].source));
  const auto* mount = std::get_if<CompositionModelMount>(
      &mounted.fixed_connections()[1].source);
  ASSERT_NE(mount, nullptr);
  EXPECT_EQ(mount->parent_model_instance_id.value(), "base_model");
  EXPECT_EQ(mount->child_model_instance_id.value(), "tool_model");

  TypeSpec bad_mount{
      .id = "bad_mount",
      .tags = {},
      .model_roles = {"base", "tool"},
      .root_role = "base",
  };
  bad_mount.mount = core::ComponentTypeModelMountDTO{
      {core::ModelRoleId("base"), core::FrameRoleId("tcp")},
      {core::ModelRoleId("tool"), core::FrameRoleId("tcp")},
      identity_pose_dto};
  auto bad_mount_type = qualify_type(bad_mount);
  const auto bad_mount_graph = graph_for(
      {{"base_model", &source, false},
       {"tool_model", &child_source, false}},
      {{"component",
        &bad_mount_type,
        {{"base", "base_model"}, {"tool", "tool_model"}},
        true}});
  const auto mount_types = type_values({&bad_mount_type});
  EXPECT_THROW(
      (void)compose_cell_occurrences(
          bad_mount_graph, mount_sources, mount_types),
      CellCompositionError);
}

TEST(CellStructure,
     RejectsMissingIncompatibleAndOverCapacityAttachmentEndpoints) {
  auto arm_source = qualify_source(AssetSpec{.id = "arm"});
  auto tool_source = qualify_source(AssetSpec{.id = "tool"});
  auto arm_type = qualify_type(
      TypeSpec{.id = "arm_type",
               .tags = {"arm"},
               .with_interface = true,
               .accepts_tags = {"tool"}});
  auto tool_type = qualify_type(
      TypeSpec{.id = "tool_type",
               .tags = {"tool"},
               .with_interface = true,
               .interface_id = "mount",
               .interface_frame = "base",
               .accepts_tags = {"arm"}});
  const auto sources = source_values({&arm_source, &tool_source});
  const auto types = type_values({&arm_type, &tool_type});

  const auto missing = graph_for(
      {{"arm_model", &arm_source, false},
       {"tool_model", &tool_source, false}},
      {{"arm", &arm_type, {{"body", "arm_model"}}, true},
       {"tool", &tool_type, {{"body", "tool_model"}}, false}},
      {{"edge", "arm", "missing", "tool", "mount"}});
  EXPECT_THROW(
      (void)compose_cell_occurrences(missing, sources, types),
      CellCompositionError);

  auto incompatible_tool_type = qualify_type(
      TypeSpec{.id = "incompatible_tool",
               .tags = {"tool"},
               .with_interface = true,
               .interface_id = "mount",
               .interface_frame = "base",
               .accepts_tags = {"other"}});
  const auto incompatible = graph_for(
      {{"arm_model", &arm_source, false},
       {"tool_model", &tool_source, false}},
      {{"arm", &arm_type, {{"body", "arm_model"}}, true},
       {"tool",
        &incompatible_tool_type,
        {{"body", "tool_model"}},
        false}},
      {{"edge", "arm", "flange", "tool", "mount"}});
  const auto incompatible_types =
      type_values({&arm_type, &incompatible_tool_type});
  EXPECT_THROW(
      (void)compose_cell_occurrences(
          incompatible, sources, incompatible_types),
      CellCompositionError);

  auto nonroot_tool_type = qualify_type(
      TypeSpec{.id = "nonroot_tool",
               .tags = {"tool"},
               .with_interface = true,
               .interface_id = "mount",
               .interface_frame = "tcp",
               .accepts_tags = {"arm"}});
  const auto nonroot_child = graph_for(
      {{"arm_model", &arm_source, false},
       {"tool_model", &tool_source, false}},
      {{"arm", &arm_type, {{"body", "arm_model"}}, true},
       {"tool", &nonroot_tool_type, {{"body", "tool_model"}}, false}},
      {{"edge", "arm", "flange", "tool", "mount"}});
  const auto nonroot_types =
      type_values({&arm_type, &nonroot_tool_type});
  EXPECT_THROW(
      (void)compose_cell_occurrences(
          nonroot_child, sources, nonroot_types),
      CellCompositionError);

  const auto capacity = graph_for(
      {{"arm_model", &arm_source, false},
       {"tool_a_model", &tool_source, false},
       {"tool_b_model", &tool_source, false}},
      {{"arm", &arm_type, {{"body", "arm_model"}}, true},
       {"tool_a", &tool_type, {{"body", "tool_a_model"}}, false},
       {"tool_b", &tool_type, {{"body", "tool_b_model"}}, false}},
      {{"edge_a", "arm", "flange", "tool_a", "mount"},
       {"edge_b", "arm", "flange", "tool_b", "mount"}});
  EXPECT_THROW(
      (void)compose_cell_occurrences(capacity, sources, types),
      CellCompositionError);
}

[[nodiscard]] bool is_mf1_name(std::string_view value) {
  if (!value.starts_with("mf1_")) {
    return false;
  }
  const auto separator = value.rfind('_');
  if (separator == std::string_view::npos ||
      value.size() - separator - 1U != 64U) {
    return false;
  }
  return std::all_of(
      value.begin() + static_cast<std::ptrdiff_t>(separator + 1U),
      value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
      });
}

[[nodiscard]] std::array<double, 4> multiply_quaternion(
    const std::array<double, 4>& left,
    const std::array<double, 4>& right) {
  return {
      left[3] * right[0] + left[0] * right[3] +
          left[1] * right[2] - left[2] * right[1],
      left[3] * right[1] - left[0] * right[2] +
          left[1] * right[3] + left[2] * right[0],
      left[3] * right[2] + left[0] * right[1] -
          left[1] * right[0] + left[2] * right[3],
      left[3] * right[3] - left[0] * right[0] -
          left[1] * right[1] - left[2] * right[2],
  };
}

[[nodiscard]] std::array<double, 3> rotate_vector(
    const std::array<double, 4>& rotation,
    const std::array<double, 3>& vector) {
  const std::array<double, 4> vector_quaternion{
      vector[0], vector[1], vector[2], 0.0};
  const std::array<double, 4> inverse{
      -rotation[0], -rotation[1], -rotation[2], rotation[3]};
  const auto rotated = multiply_quaternion(
      multiply_quaternion(rotation, vector_quaternion), inverse);
  return {rotated[0], rotated[1], rotated[2]};
}

[[nodiscard]] core::PlacementPoseDTO compose_pose(
    const core::PlacementPoseDTO& parent,
    const core::PlacementPoseDTO& child) {
  const auto translated =
      rotate_vector(parent.orientation_xyzw, child.position_m);
  return {{
              parent.position_m[0] + translated[0],
              parent.position_m[1] + translated[1],
              parent.position_m[2] + translated[2],
          },
          multiply_quaternion(parent.orientation_xyzw,
                              child.orientation_xyzw)};
}

[[nodiscard]] core::PlacementPoseDTO pose_from_origin(
    const UrdfOriginIr& origin) {
  const auto half_roll = origin.rpy_rad[0] * 0.5;
  const auto half_pitch = origin.rpy_rad[1] * 0.5;
  const auto half_yaw = origin.rpy_rad[2] * 0.5;
  const auto cr = std::cos(half_roll);
  const auto sr = std::sin(half_roll);
  const auto cp = std::cos(half_pitch);
  const auto sp = std::sin(half_pitch);
  const auto cy = std::cos(half_yaw);
  const auto sy = std::sin(half_yaw);
  return {{origin.xyz_m[0], origin.xyz_m[1], origin.xyz_m[2]},
          {sr * cp * cy - cr * sp * sy,
           cr * sp * cy + sr * cp * sy,
           cr * cp * sy - sr * sp * cy,
           cr * cp * cy + sr * sp * sy}};
}

void expect_pose_near(const core::PlacementPoseDTO& actual,
                      const core::PlacementPoseDTO& expected,
                      double tolerance = 1e-12) {
  for (std::size_t index = 0; index < 3U; ++index) {
    EXPECT_NEAR(actual.position_m[index], expected.position_m[index],
                tolerance);
  }
  for (std::size_t index = 0; index < 4U; ++index) {
    EXPECT_NEAR(actual.orientation_xyzw[index],
                expected.orientation_xyzw[index], tolerance);
  }
}

[[nodiscard]] core::PlacementPoseDTO reconstruct_path_pose(
    const CompositionQualifiedCellIr& composed,
    const CompositionFramePath& path) {
  core::PlacementPoseDTO result;
  for (const auto& segment : path.segments) {
    std::visit(
        [&](const auto& selected) {
          using T = std::decay_t<decltype(selected)>;
          if constexpr (std::is_same_v<T,
                                       CompositionFixedPathSegment>) {
            if (selected.fixed_connection_index >=
                composed.fixed_connections().size()) {
              throw std::runtime_error(
                  "test path references a missing fixed connection");
            }
            result = compose_pose(
                result,
                composed.fixed_connections()[selected.fixed_connection_index]
                    .child_pose_in_parent);
          } else {
            const auto joint = std::find_if(
                composed.flattened_urdf().joints.begin(),
                composed.flattened_urdf().joints.end(),
                [&](const UrdfJointIr& candidate) {
                  return candidate.name == selected.compiled_joint_name;
                });
            if (joint == composed.flattened_urdf().joints.end()) {
              throw std::runtime_error(
                  "test path references a missing model joint");
            }
            result = compose_pose(result, pose_from_origin(joint->origin));
          }
        },
        segment);
  }
  return result;
}

TEST(CellComposition,
     EmptyCellContainsOnlyItsSemanticFrameAndUsesTheOrdinaryPath) {
  const auto graph = graph_for({}, {});
  const auto composed = compose_cell_occurrences(
      graph, std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});

  EXPECT_EQ(composed.graph(), graph.graph());
  EXPECT_TRUE(is_mf1_name(composed.cell_frame().compiled_frame_name));
  ASSERT_EQ(composed.flattened_urdf().links.size(), 1U);
  EXPECT_EQ(composed.flattened_urdf().links[0].name,
            composed.cell_frame().compiled_frame_name);
  EXPECT_TRUE(composed.flattened_urdf().materials.empty());
  EXPECT_TRUE(composed.flattened_urdf().joints.empty());
  EXPECT_TRUE(composed.flattened_srdf().groups.empty());
  EXPECT_TRUE(composed.flattened_srdf().group_states.empty());
  EXPECT_TRUE(composed.flattened_srdf().end_effectors.empty());
  EXPECT_TRUE(composed.flattened_srdf().passive_joint_names.empty());
  EXPECT_TRUE(composed.flattened_srdf().disabled_collisions.empty());
  EXPECT_TRUE(composed.model_occurrences().empty());
  EXPECT_TRUE(composed.symbol_rewrites().empty());
  EXPECT_TRUE(composed.reverse_symbol_rewrites().empty());
  EXPECT_TRUE(composed.reference_rewrites().empty());
  EXPECT_TRUE(composed.frames().empty());
  EXPECT_TRUE(composed.fixed_connections().empty());
  EXPECT_TRUE(composed.coordinates().empty());
  EXPECT_TRUE(composed.control_binding_coordinate_maps().empty());
  EXPECT_TRUE(composed.targets().empty());
  ASSERT_EQ(composed.frame_paths().size(), 1U);
  EXPECT_TRUE(composed.frame_paths()[0].segments.empty());
  EXPECT_TRUE(std::holds_alternative<CompositionCellFrame>(
      composed.frame_paths()[0].terminal_frame));
}

TEST(CellComposition,
     PassiveOnlyCellHasNoFabricatedComponentOrBindingIdentity) {
  auto passive_source = qualify_source_with_documents(
      AssetSpec{.id = "passive_only_asset",
                .frames = {{"base", "fixture_link"},
                           {"tcp", "fixture_link"}},
                .groups = {},
                .resources = {},
                .bodies = {{"fixture", {"fixture_link"}}}},
      "<robot name=\"passive_fixture\"><link name=\"fixture_link\"/>"
      "</robot>");
  const auto graph = graph_for(
      {{"fixture", &passive_source, true}}, {});
  const auto composed = compose_cell_occurrences(
      graph, source_values({&passive_source}),
      std::span<const QualifiedComponentType>{});

  ASSERT_EQ(composed.model_occurrences().size(), 1U);
  EXPECT_FALSE(
      composed.model_occurrences()[0].owner_component_id.has_value());
  ASSERT_EQ(composed.flattened_urdf().links.size(), 2U);
  ASSERT_EQ(composed.fixed_connections().size(), 1U);
  EXPECT_TRUE(std::holds_alternative<CompositionDirectModelAnchor>(
      composed.fixed_connections()[0].source));
  EXPECT_TRUE(composed.coordinates().empty());
  EXPECT_TRUE(composed.targets().empty());
  EXPECT_TRUE(composed.control_binding_coordinate_maps().empty());
  ASSERT_EQ(composed.frames().size(), 2U);
  for (const auto& frame : composed.frames()) {
    EXPECT_TRUE(std::holds_alternative<CompositionPassiveModelFrame>(
        frame.frame));
  }
}

TEST(CellComposition,
     FlattensPassiveAggregateAndAttachedModelsWithTotalRewriteEvidence) {
  auto arm_source = qualify_source(
      AssetSpec{.id = "arm_vendor_a", .with_srdf = true});
  auto tool_source = qualify_source(
      AssetSpec{.id = "tool_vendor_b",
                .groups = {},
                .resources = {},
                .bodies = {{"tool_collision", {"tip_link"}}}});
  auto fixture_source = qualify_source(
      AssetSpec{.id = "fixture_vendor_c",
                .groups = {},
                .resources = {},
                .bodies = {{"fixture_collision", {"base_link"}}}});
  auto arm_type = qualify_type(
      TypeSpec{.id = "arm_type",
               .tags = {"articulated-arm"},
               .with_target = true,
               .with_interface = true});
  auto tool_type = qualify_type(
      TypeSpec{.id = "tool_type",
               .tags = {"robot-tool"},
               .with_interface = true,
               .interface_id = "mount",
               .interface_frame = "base",
               .accepts_tags = {"articulated-arm"}});
  const auto graph = graph_for(
      {{"arm_model", &arm_source, false},
       {"fixture_model", &fixture_source, true},
       {"tool_model", &tool_source, false}},
      {{"arm", &arm_type, {{"body", "arm_model"}}, true},
       {"tool", &tool_type, {{"body", "tool_model"}}, false}},
      {{"tool_on_arm", "arm", "flange", "tool", "mount"}});
  const auto sources =
      source_values({&tool_source, &fixture_source, &arm_source});
  const auto types = type_values({&tool_type, &arm_type});
  const auto composed = compose_cell_occurrences(graph, sources, types);
  const auto composed_again =
      compose_cell_occurrences(
          graph, source_values({&arm_source, &tool_source, &fixture_source}),
          type_values({&arm_type, &tool_type}));

  EXPECT_EQ(emit_canonical_urdf_document(composed.flattened_urdf()),
            emit_canonical_urdf_document(composed_again.flattened_urdf()));
  EXPECT_EQ(emit_canonical_srdf_document(composed.flattened_srdf()),
            emit_canonical_srdf_document(composed_again.flattened_srdf()));
  EXPECT_EQ(composed.symbol_rewrites(),
            composed_again.symbol_rewrites());
  EXPECT_EQ(composed.reference_rewrites(),
            composed_again.reference_rewrites());

  ASSERT_EQ(composed.model_occurrences().size(), 3U);
  ASSERT_EQ(composed.fixed_connections().size(), 3U);
  EXPECT_TRUE(std::holds_alternative<CompositionDirectComponentAnchor>(
      composed.fixed_connections()[0].source));
  EXPECT_TRUE(std::holds_alternative<CompositionDirectModelAnchor>(
      composed.fixed_connections()[1].source));
  EXPECT_TRUE(std::holds_alternative<CompositionComponentAttachment>(
      composed.fixed_connections()[2].source));
  EXPECT_EQ(composed.flattened_urdf().links.size(), 7U);
  EXPECT_EQ(composed.flattened_urdf().joints.size(), 6U);
  EXPECT_EQ(composed.frames().size(), 6U);
  EXPECT_EQ(composed.frame_paths().size(), 7U);
  ASSERT_EQ(composed.targets().size(), 1U);
  ASSERT_EQ(composed.targets()[0].model_groups.size(), 1U);
  EXPECT_EQ(composed.targets()[0]
                .model_groups[0]
                .expanded_compiled_link_names.size(),
            2U);
  EXPECT_EQ(composed.targets()[0]
                .model_groups[0]
                .expanded_compiled_joint_names.size(),
            1U);

  ASSERT_EQ(composed.symbol_rewrites().size(),
            composed.reverse_symbol_rewrites().size());
  std::map<std::string, OccurrenceSymbolKey, std::less<>> reverse;
  for (const auto& row : composed.reverse_symbol_rewrites()) {
    EXPECT_TRUE(is_mf1_name(row.compiled_name));
    EXPECT_TRUE(reverse.emplace(row.compiled_name, row.source).second);
  }
  for (const auto& row : composed.symbol_rewrites()) {
    EXPECT_TRUE(is_mf1_name(row.compiled_name));
    const auto found = reverse.find(row.compiled_name);
    ASSERT_NE(found, reverse.end());
    EXPECT_EQ(found->second, row.source);
  }
  for (const auto& reference : composed.reference_rewrites()) {
    const auto found = reverse.find(reference.compiled_target_name);
    ASSERT_NE(found, reverse.end());
    EXPECT_EQ(found->second, reference.source_target);
  }

  for (const auto& connection : composed.fixed_connections()) {
    EXPECT_EQ(connection.child_pose_in_parent,
              std::visit(
                  [](const auto& source)
                      -> const core::PlacementPoseDTO& {
                    if constexpr (std::is_same_v<
                                      std::decay_t<decltype(source)>,
                                      CompositionDirectModelAnchor>) {
                      return source.root_pose_in_cell;
                    } else if constexpr (std::is_same_v<
                                             std::decay_t<decltype(source)>,
                                             CompositionDirectComponentAnchor>) {
                      return source.root_pose_in_cell;
                    } else if constexpr (std::is_same_v<
                                             std::decay_t<decltype(source)>,
                                             CompositionModelMount>) {
                      return source.child_frame_pose_in_parent_frame;
                    } else {
                      return source
                          .child_interface_pose_in_parent_interface;
                    }
                  },
                  connection.source));
  }
}

TEST(CellComposition,
     DistinguishesInlineMaterialDefinitionsFromTopLevelReferences) {
  const std::string urdf =
      "<robot name=\"materials\">"
      "<material name=\"paint\"><color rgba=\"1 0 0 1\"/></material>"
      "<link name=\"base_link\">"
      "<visual name=\"reference_visual\"><geometry>"
      "<box size=\"1 1 1\"/></geometry><material name=\"paint\"/>"
      "</visual>"
      "<visual name=\"inline_visual_a\"><geometry>"
      "<box size=\"1 1 1\"/></geometry><material name=\"paint\">"
      "<color rgba=\"0 1 0 1\"/></material></visual>"
      "<visual name=\"inline_visual_b\"><geometry>"
      "<box size=\"1 1 1\"/></geometry><material name=\"paint\">"
      "<color rgba=\"0 0 1 1\"/></material></visual>"
      "</link></robot>";
  auto source = qualify_source_with_documents(
      AssetSpec{.id = "material_asset",
                .frames = {{"base", "base_link"}},
                .groups = {},
                .resources = {},
                .bodies = {{"body", {"base_link"}}}},
      urdf);
  const auto composed = compose_cell_occurrences(
      graph_for({{"material_model", &source, true}}, {}),
      source_values({&source}),
      std::span<const QualifiedComponentType>{});

  std::map<std::pair<std::string, std::string>, std::string>
      material_rewrites;
  for (const auto& rewrite : composed.symbol_rewrites()) {
    if (rewrite.source.kind == OccurrenceSymbolKind::kMaterial) {
      material_rewrites.emplace(
          std::pair{rewrite.source.scope_name,
                    rewrite.source.source_name},
          rewrite.compiled_name);
    }
  }
  ASSERT_EQ(material_rewrites.size(), 3U);
  const auto& top_level =
      material_rewrites.at({"", "paint"});
  const auto& inline_a =
      material_rewrites.at({"base_link/visual/0", "paint"});
  const auto& inline_b =
      material_rewrites.at({"base_link/visual/1", "paint"});
  EXPECT_NE(top_level, inline_a);
  EXPECT_NE(top_level, inline_b);
  EXPECT_NE(inline_a, inline_b);

  const auto rewritten_link = std::find_if(
      composed.flattened_urdf().links.begin(),
      composed.flattened_urdf().links.end(),
      [](const UrdfLinkIr& link) {
        return link.visuals.size() == 3U;
      });
  ASSERT_NE(rewritten_link, composed.flattened_urdf().links.end());
  ASSERT_TRUE(rewritten_link->visuals[2].material.has_value());
  ASSERT_TRUE(
      rewritten_link->visuals[2].material->resolved_reference.has_value());
  EXPECT_EQ(rewritten_link->visuals[2].material->name, top_level);
  EXPECT_EQ(
      rewritten_link->visuals[2]
          .material->resolved_reference->definition_name,
      top_level);
  ASSERT_TRUE(rewritten_link->visuals[0].material.has_value());
  ASSERT_TRUE(rewritten_link->visuals[1].material.has_value());
  EXPECT_FALSE(
      rewritten_link->visuals[0].material->resolved_reference.has_value());
  EXPECT_FALSE(
      rewritten_link->visuals[1].material->resolved_reference.has_value());
  EXPECT_EQ(rewritten_link->visuals[0].material->name, inline_a);
  EXPECT_EQ(rewritten_link->visuals[1].material->name, inline_b);

  const auto reference = std::find_if(
      composed.reference_rewrites().begin(),
      composed.reference_rewrites().end(),
      [](const OccurrenceReferenceRewrite& row) {
        return row.kind == OccurrenceReferenceKind::kVisualMaterial;
      });
  ASSERT_NE(reference, composed.reference_rewrites().end());
  EXPECT_EQ(reference->owner_scope_name, "base_link/visual/2");
  EXPECT_EQ(reference->owner_source_name, "reference_visual");
  EXPECT_TRUE(reference->source_target.scope_name.empty());
  EXPECT_EQ(reference->source_target.source_name, "paint");
  EXPECT_EQ(reference->compiled_target_name, top_level);
}

TEST(CellComposition,
     PreservesAllCoordinateKindsNestedSrdfAndTransitiveMimicProjection) {
  const std::string urdf =
      "<robot name=\"multi_axis\">"
      "<material name=\"paint\"><color rgba=\"0.1 0.2 0.3 1\"/>"
      "</material>"
      "<link name=\"base_link\"><visual name=\"body_visual\">"
      "<geometry><box size=\"1 1 1\"/></geometry>"
      "<material name=\"paint\"/></visual>"
      "<collision name=\"body_collision\">"
      "<geometry><box size=\"1 1 1\"/></geometry>"
      "</collision></link><link name=\"arm_link\"/>"
      "<link name=\"mimic_link\"/><link name=\"mimic_b_link\"/>"
      "<link name=\"rail_link\"/>"
      "<link name=\"gantry_link\"/><link name=\"planar_link\"/>"
      "<link name=\"floating_link\"/><link name=\"fixed_link\"/>"
      "<joint name=\"z_drive\" type=\"revolute\"><parent link=\"base_link\"/>"
      "<child link=\"arm_link\"/><limit lower=\"-3\" upper=\"3\" "
      "effort=\"1\" velocity=\"1\"/></joint>"
      "<joint name=\"a_mimic\" type=\"revolute\"><parent link=\"arm_link\"/>"
      "<child link=\"mimic_link\"/><limit lower=\"-3\" upper=\"3\" "
      "effort=\"1\" velocity=\"1\"/><mimic joint=\"z_drive\" "
      "multiplier=\"-2\" offset=\"0.1\"/></joint>"
      "<joint name=\"b_mimic\" type=\"revolute\">"
      "<parent link=\"mimic_link\"/><child link=\"mimic_b_link\"/>"
      "<limit lower=\"-3\" upper=\"3\" effort=\"1\" velocity=\"1\"/>"
      "<mimic joint=\"a_mimic\" multiplier=\"0.5\" offset=\"-0.2\"/>"
      "</joint>"
      "<joint name=\"rail\" type=\"prismatic\"><parent link=\"base_link\"/>"
      "<child link=\"rail_link\"/><limit lower=\"-1\" upper=\"1\" "
      "effort=\"1\" velocity=\"1\"/></joint>"
      "<joint name=\"gantry\" type=\"prismatic\"><parent link=\"base_link\"/>"
      "<child link=\"gantry_link\"/><limit lower=\"-1\" upper=\"1\" "
      "effort=\"1\" velocity=\"1\"/></joint>"
      "<joint name=\"planar\" type=\"planar\"><parent link=\"base_link\"/>"
      "<child link=\"planar_link\"/></joint>"
      "<joint name=\"floating\" type=\"floating\"><parent link=\"base_link\"/>"
      "<child link=\"floating_link\"/></joint>"
      "<joint name=\"fixed\" type=\"fixed\"><parent link=\"base_link\"/>"
      "<child link=\"fixed_link\"/></joint></robot>";
  const std::string srdf =
      "<robot name=\"multi_axis\">"
      "<group name=\"drive_group\"><joint name=\"z_drive\"/></group>"
      "<group name=\"chain_group\"><chain base_link=\"base_link\" "
      "tip_link=\"fixed_link\"/></group>"
      "<group name=\"whole\"><group name=\"drive_group\"/>"
      "<joint name=\"rail\"/><joint name=\"gantry\"/>"
      "<link name=\"fixed_link\"/></group>"
      "<group_state name=\"ready\" group=\"whole\">"
      "<joint name=\"z_drive\" value=\"0\"/><joint name=\"rail\" value=\"0\"/>"
      "<joint name=\"gantry\" value=\"0\"/></group_state>"
      "<end_effector name=\"tool\" parent_link=\"fixed_link\" "
      "group=\"whole\" parent_group=\"whole\"/>"
      "<passive_joint name=\"a_mimic\"/><passive_joint name=\"b_mimic\"/>"
      "<disable_collisions link1=\"base_link\" link2=\"fixed_link\" "
      "reason=\"Adjacent\"/></robot>";
  auto source = qualify_source_with_documents(
      AssetSpec{.id = "multi_axis_asset",
                .with_srdf = true,
                .frames = {{"base", "base_link"}, {"tcp", "fixed_link"}},
                .groups = {{"whole_role", "whole"}},
                .resources = {{"drive_role", {"z_drive"}},
                              {"rail_role", {"rail", "gantry"}}},
                .bodies = {{"body", {"base_link", "fixed_link"}}}},
      urdf, srdf);
  auto type = qualify_type_with_resources(
      TypeSpec{.id = "multi_axis_type",
               .tags = {},
               .with_target = true,
               .target_group_source = "whole_role",
               .target_body_source = "body"},
      {{"drive", "drive_role"}, {"linear", "rail_role"}});
  const auto graph = graph_for(
      {{"multi_axis_model", &source, false}},
      {{"multi_axis", &type, {{"body", "multi_axis_model"}}, true}},
      {},
      {{"binding",
        "multi_axis",
        "command",
        {{"drive", "drive_resource"},
         {"linear", "rail_resource"},
         {"linear", "gantry_resource"}}}});
  const auto composed = compose_cell_occurrences(
      graph, source_values({&source}), type_values({&type}));

  ASSERT_EQ(composed.coordinates().size(), 8U);
  std::map<std::string, CompositionCoordinateUnit, std::less<>> units;
  for (const auto& coordinate : composed.coordinates()) {
    units.emplace(coordinate.source_joint_name, coordinate.unit);
  }
  EXPECT_EQ(units.at("z_drive"), CompositionCoordinateUnit::kRadians);
  EXPECT_EQ(units.at("rail"), CompositionCoordinateUnit::kMeters);
  EXPECT_EQ(units.at("gantry"), CompositionCoordinateUnit::kMeters);
  EXPECT_EQ(units.at("planar"),
            CompositionCoordinateUnit::kPlanarMetersRadians);
  EXPECT_EQ(units.at("floating"),
            CompositionCoordinateUnit::kFloatingMetersRadians);
  EXPECT_EQ(units.at("fixed"), CompositionCoordinateUnit::kNone);
  ASSERT_EQ(composed.control_binding_coordinate_maps().size(), 1U);
  const auto& map = composed.control_binding_coordinate_maps()[0];
  ASSERT_EQ(map.resources.size(), 3U);
  ASSERT_EQ(map.resources[0].coordinates.size(), 3U);
  EXPECT_FALSE(map.resources[0].coordinates[0].derived_mimic);
  EXPECT_EQ(map.resources[0].coordinates[0].source_joint_name, "z_drive");
  EXPECT_DOUBLE_EQ(map.resources[0].coordinates[0].multiplier, 1.0);
  EXPECT_DOUBLE_EQ(map.resources[0].coordinates[0].offset, 0.0);
  EXPECT_TRUE(map.resources[0].coordinates[1].derived_mimic);
  EXPECT_EQ(map.resources[0].coordinates[1].source_joint_name, "a_mimic");
  EXPECT_DOUBLE_EQ(map.resources[0].coordinates[1].multiplier, -2.0);
  EXPECT_DOUBLE_EQ(map.resources[0].coordinates[1].offset, 0.1);
  EXPECT_TRUE(map.resources[0].coordinates[2].derived_mimic);
  EXPECT_EQ(map.resources[0].coordinates[2].source_joint_name, "b_mimic");
  EXPECT_DOUBLE_EQ(map.resources[0].coordinates[2].multiplier, -1.0);
  EXPECT_DOUBLE_EQ(map.resources[0].coordinates[2].offset, -0.15);
  ASSERT_EQ(map.resources[1].coordinates.size(), 1U);
  EXPECT_EQ(map.resources[1].resource_id.value(), "rail_resource");
  EXPECT_EQ(map.resources[1].coordinates[0].source_joint_name, "rail");
  EXPECT_FALSE(map.resources[1].coordinates[0].derived_mimic);
  ASSERT_EQ(map.resources[2].coordinates.size(), 1U);
  EXPECT_EQ(map.resources[2].resource_id.value(), "gantry_resource");
  EXPECT_EQ(map.resources[2].coordinates[0].source_joint_name, "gantry");
  EXPECT_FALSE(map.resources[2].coordinates[0].derived_mimic);
  ASSERT_EQ(composed.targets().size(), 1U);
  ASSERT_EQ(composed.targets()[0].model_groups.size(), 1U);
  EXPECT_EQ(composed.targets()[0]
                .model_groups[0]
                .expanded_compiled_joint_names.size(),
            3U);
  EXPECT_EQ(composed.flattened_srdf().groups.size(), 3U);
  EXPECT_EQ(composed.flattened_srdf().group_states.size(), 1U);
  EXPECT_EQ(composed.flattened_srdf().end_effectors.size(), 1U);
  EXPECT_EQ(composed.flattened_srdf().passive_joint_names.size(), 2U);
  EXPECT_EQ(composed.flattened_srdf().disabled_collisions.size(), 1U);

  const auto& retained_source = source.value.source_ir();
  std::size_t expected_references = 0U;
  for (const auto& link : retained_source.urdf().links) {
    for (const auto& visual : link.visuals) {
      expected_references +=
          visual.material.has_value() &&
                  visual.material->resolved_reference.has_value()
              ? 1U
              : 0U;
    }
  }
  for (const auto& joint : retained_source.urdf().joints) {
    expected_references += joint.mimic.has_value() ? 3U : 2U;
  }
  ASSERT_TRUE(retained_source.srdf().has_value());
  for (const auto& group : retained_source.srdf()->groups) {
    for (const auto& member : group.members) {
      expected_references +=
          std::holds_alternative<SrdfChainGroupMemberIr>(member) ? 2U : 1U;
    }
  }
  for (const auto& state : retained_source.srdf()->group_states) {
    expected_references += 1U + state.joints.size();
  }
  for (const auto& eef : retained_source.srdf()->end_effectors) {
    expected_references += eef.parent_group_name.has_value() ? 3U : 2U;
  }
  expected_references +=
      retained_source.srdf()->passive_joint_names.size();
  expected_references +=
      2U * retained_source.srdf()->disabled_collisions.size();
  EXPECT_EQ(composed.reference_rewrites().size(), expected_references);

  std::set<OccurrenceReferenceKind> observed_reference_kinds;
  for (const auto& reference : composed.reference_rewrites()) {
    observed_reference_kinds.insert(reference.kind);
  }
  const std::set<OccurrenceReferenceKind> expected_reference_kinds{
      OccurrenceReferenceKind::kJointParentLink,
      OccurrenceReferenceKind::kJointChildLink,
      OccurrenceReferenceKind::kJointMimic,
      OccurrenceReferenceKind::kVisualMaterial,
      OccurrenceReferenceKind::kSrdfGroupLink,
      OccurrenceReferenceKind::kSrdfGroupJoint,
      OccurrenceReferenceKind::kSrdfChainBaseLink,
      OccurrenceReferenceKind::kSrdfChainTipLink,
      OccurrenceReferenceKind::kSrdfSubgroup,
      OccurrenceReferenceKind::kSrdfGroupStateGroup,
      OccurrenceReferenceKind::kSrdfGroupStateJoint,
      OccurrenceReferenceKind::kSrdfEndEffectorParentLink,
      OccurrenceReferenceKind::kSrdfEndEffectorGroup,
      OccurrenceReferenceKind::kSrdfEndEffectorParentGroup,
      OccurrenceReferenceKind::kSrdfPassiveJoint,
      OccurrenceReferenceKind::kSrdfDisabledCollisionFirstLink,
      OccurrenceReferenceKind::kSrdfDisabledCollisionSecondLink,
  };
  EXPECT_EQ(observed_reference_kinds, expected_reference_kinds);
}

TEST(CellComposition,
     IsolatesTwoSameTypeRobotsAndToolsAcrossTargetsAndResources) {
  auto robot_source = qualify_source(
      AssetSpec{.id = "robot_vendor_a", .with_srdf = true});
  auto tool_source = qualify_source(
      AssetSpec{.id = "tool_vendor_b", .with_srdf = true});
  auto passive_source = qualify_source(
      AssetSpec{.id = "fixture_vendor_c",
                .groups = {},
                .resources = {},
                .bodies = {{"fixture_collision", {"base_link"}}}});
  auto robot_type = qualify_type(
      TypeSpec{.id = "shared_robot_type",
               .tags = {"articulated-arm"},
               .with_target = true,
               .with_interface = true});
  auto tool_type = qualify_type_with_component_local_resources(
      TypeSpec{.id = "shared_tool_type",
               .tags = {"robot-tool"},
               .with_target = true,
               .with_interface = true,
               .interface_id = "mount",
               .interface_frame = "base",
               .accepts_tags = {"articulated-arm"}},
      {{"joints", "arm_joints"}}, {"vacuum"});
  const auto graph = graph_for(
      {{"fixture_model", &passive_source, true},
       {"robot_model_a", &robot_source, false},
       {"robot_model_b", &robot_source, false},
       {"tool_model_a", &tool_source, false},
       {"tool_model_b", &tool_source, false}},
      {{"robot_a", &robot_type, {{"body", "robot_model_a"}}, true},
       {"robot_b", &robot_type, {{"body", "robot_model_b"}}, true},
       {"tool_a", &tool_type, {{"body", "tool_model_a"}}, false},
       {"tool_b", &tool_type, {{"body", "tool_model_b"}}, false}},
      {{"attach_a", "robot_a", "flange", "tool_a", "mount"},
       {"attach_b", "robot_b", "flange", "tool_b", "mount"}},
      {{"binding_robot_a",
        "robot_a",
        "command",
        {{"joints", "robot_resource_a"}}},
       {"binding_robot_b",
        "robot_b",
        "command",
        {{"joints", "robot_resource_b"}}},
       {"binding_tool_a",
        "tool_a",
        "command",
        {{"joints", "tool_resource_a"},
         {"vacuum", "vacuum_resource_a"}}},
       {"binding_tool_b",
        "tool_b",
        "command",
        {{"joints", "tool_resource_b"},
         {"vacuum", "vacuum_resource_b"}}}});
  const auto composed = compose_cell_occurrences(
      graph,
      source_values({&tool_source, &passive_source, &robot_source}),
      type_values({&tool_type, &robot_type}));

  const std::map<std::string, std::string, std::less<>>
      expected_model_by_component{
          {"robot_a", "robot_model_a"},
          {"robot_b", "robot_model_b"},
          {"tool_a", "tool_model_a"},
          {"tool_b", "tool_model_b"},
      };
  ASSERT_EQ(composed.model_occurrences().size(), 5U);
  std::map<std::string, std::string, std::less<>> root_by_model;
  for (const auto& occurrence : composed.model_occurrences()) {
    EXPECT_TRUE(root_by_model
                    .emplace(occurrence.model_instance_id.value(),
                             occurrence.compiled_root_link_name)
                    .second);
  }
  EXPECT_NE(root_by_model.at("robot_model_a"),
            root_by_model.at("robot_model_b"));
  EXPECT_NE(root_by_model.at("tool_model_a"),
            root_by_model.at("tool_model_b"));

  ASSERT_EQ(composed.targets().size(), 4U);
  std::set<std::string, std::less<>> target_group_names;
  for (const auto& target : composed.targets()) {
    const auto expected = expected_model_by_component.find(
        target.controllable_component_id.value());
    ASSERT_NE(expected, expected_model_by_component.end());
    ASSERT_EQ(target.model_groups.size(), 1U);
    EXPECT_EQ(target.model_groups[0].model_instance_id.value(),
              expected->second);
    EXPECT_TRUE(target_group_names
                    .insert(target.model_groups[0].compiled_group_name)
                    .second);
  }

  ASSERT_EQ(composed.control_binding_coordinate_maps().size(), 4U);
  std::set<std::string, std::less<>> resource_ids;
  for (const auto& map :
       composed.control_binding_coordinate_maps()) {
    const auto expected = expected_model_by_component.find(
        map.controllable_component_id.value());
    ASSERT_NE(expected, expected_model_by_component.end());
    const auto is_tool =
        map.controllable_component_id.value().starts_with("tool_");
    ASSERT_EQ(map.resources.size(), is_tool ? 2U : 1U);
    EXPECT_TRUE(
        resource_ids.insert(map.resources[0].resource_id.value()).second);
    ASSERT_EQ(map.resources[0].coordinates.size(), 1U);
    EXPECT_EQ(map.resources[0].coordinates[0].model_instance_id.value(),
              expected->second);
    EXPECT_FALSE(map.resources[0].coordinates[0].derived_mimic);
    if (is_tool) {
      EXPECT_EQ(map.resources[1].resource_role_id.value(), "vacuum");
      EXPECT_TRUE(
          resource_ids.insert(map.resources[1].resource_id.value()).second);
      EXPECT_TRUE(map.resources[1].coordinates.empty());
    }
  }
}

TEST(CellComposition,
     RejectsMissingExtraDuplicateReorderedAndNonIndependentResourceMaps) {
  const std::string ordered_urdf =
      "<robot name=\"ordered\">"
      "<link name=\"base_link\"/><link name=\"middle_link\"/>"
      "<link name=\"tip_link\"/>"
      "<joint name=\"joint1\" type=\"revolute\">"
      "<parent link=\"base_link\"/><child link=\"middle_link\"/>"
      "<limit lower=\"-3\" upper=\"3\" effort=\"1\" velocity=\"1\"/>"
      "</joint>"
      "<joint name=\"joint2\" type=\"revolute\">"
      "<parent link=\"middle_link\"/><child link=\"tip_link\"/>"
      "<limit lower=\"-3\" upper=\"3\" effort=\"1\" velocity=\"1\"/>"
      "</joint></robot>";
  auto source = qualify_source_with_documents(
      AssetSpec{.id = "ordered_asset",
                .with_srdf = true,
                .resources = {{"first_role", {"joint1"}},
                              {"second_role", {"joint2"}}}},
      ordered_urdf, srdf_for("ordered"));
  auto type = qualify_type_with_resources(
      TypeSpec{.id = "ordered_type",
               .tags = {},
               .with_target = true},
      {{"first", "first_role"}, {"second", "second_role"}});
  const auto sources = source_values({&source});
  const auto types = type_values({&type});
  auto compose_binding = [&](std::vector<std::pair<std::string, std::string>>
                                 resources) {
    const auto graph = graph_for(
        {{"model", &source, false}},
        {{"component", &type, {{"body", "model"}}, true}}, {},
        {{"binding", "component", "command", std::move(resources)}});
    return compose_cell_occurrences(graph, sources, types);
  };

  EXPECT_THROW((void)compose_binding({{"first", "a"}}),
               CellCompositionError);
  EXPECT_THROW(
      (void)compose_binding(
          {{"first", "a"}, {"second", "b"}, {"third", "c"}}),
      CellCompositionError);
  EXPECT_THROW(
      (void)compose_binding({{"second", "a"}, {"first", "b"}}),
      CellCompositionError);
  EXPECT_THROW(
      (void)compose_binding(
          {{"first", "a"}, {"second", "b"}, {"first", "c"}}),
      CellCompositionError);
  EXPECT_THROW(
      (void)compose_binding(
          {{"first", "a"}, {"first", "b"}, {"second", "c"}}),
      CellCompositionError);
  EXPECT_NO_THROW(
      (void)compose_binding({{"first", "a"}, {"second", "b"}}));

  auto duplicate_source = qualify_source_with_documents(
      AssetSpec{.id = "duplicate_asset",
                .with_srdf = true,
                .resources = {{"first_role", {"joint1"}},
                              {"second_role", {"joint1"}}}},
      ordered_urdf, srdf_for("ordered"));
  auto duplicate_type = qualify_type_with_resources(
      TypeSpec{.id = "duplicate_type",
               .tags = {},
               .with_target = true},
      {{"first", "first_role"}, {"second", "second_role"}});
  const auto duplicate_graph = graph_for(
      {{"model", &duplicate_source, false}},
      {{"component", &duplicate_type, {{"body", "model"}}, true}}, {},
      {{"binding",
        "component",
        "command",
        {{"first", "a"}, {"second", "b"}}}});
  EXPECT_THROW(
      (void)compose_cell_occurrences(
          duplicate_graph, source_values({&duplicate_source}),
          type_values({&duplicate_type})),
      CellCompositionError);

  const std::string mimic_urdf =
      "<robot name=\"mimic\">"
      "<link name=\"base_link\"/><link name=\"middle_link\"/>"
      "<link name=\"tip_link\"/>"
      "<joint name=\"joint1\" type=\"revolute\">"
      "<parent link=\"base_link\"/><child link=\"middle_link\"/>"
      "<limit lower=\"-3\" upper=\"3\" effort=\"1\" velocity=\"1\"/>"
      "</joint>"
      "<joint name=\"joint2\" type=\"revolute\">"
      "<parent link=\"middle_link\"/><child link=\"tip_link\"/>"
      "<limit lower=\"-3\" upper=\"3\" effort=\"1\" velocity=\"1\"/>"
      "<mimic joint=\"joint1\"/></joint></robot>";
  auto mimic_source = qualify_source_with_documents(
      AssetSpec{.id = "mimic_asset",
                .with_srdf = true,
                .resources = {{"first_role", {"joint1"}},
                              {"second_role", {"joint2"}}}},
      mimic_urdf, srdf_for("mimic"));
  auto mimic_type = qualify_type_with_resources(
      TypeSpec{.id = "mimic_type",
               .tags = {},
               .with_target = true},
      {{"first", "first_role"}, {"second", "second_role"}});
  const auto mimic_graph = graph_for(
      {{"model", &mimic_source, false}},
      {{"component", &mimic_type, {{"body", "model"}}, true}}, {},
      {{"binding",
        "component",
        "command",
        {{"first", "a"}, {"second", "b"}}}});
  EXPECT_THROW(
      (void)compose_cell_occurrences(
          mimic_graph, source_values({&mimic_source}),
          type_values({&mimic_type})),
      CellCompositionError);
}

TEST(CellComposition, ResourceIdReuseRequiresOneGlobalCoordinateIdentity) {
  auto source = qualify_source(
      AssetSpec{.id = "resource_identity_asset", .with_srdf = true});
  auto type = qualify_type(
      TypeSpec{.id = "resource_identity_type",
               .tags = {},
               .with_target = true});

  const auto consistent = graph_for(
      {{"model", &source, false}},
      {{"component", &type, {{"body", "model"}}, true}},
      {},
      {{"binding_a",
        "component",
        "command",
        {{"joints", "shared_resource"}}},
       {"binding_b",
        "component",
        "command",
        {{"joints", "shared_resource"}}}});
  EXPECT_NO_THROW(
      (void)compose_cell_occurrences(
          consistent, source_values({&source}), type_values({&type})));

  const auto conflicting = graph_for(
      {{"model_a", &source, false}, {"model_b", &source, false}},
      {{"component_a", &type, {{"body", "model_a"}}, true},
       {"component_b", &type, {{"body", "model_b"}}, true}},
      {},
      {{"binding_a",
        "component_a",
        "command",
        {{"joints", "shared_resource"}}},
       {"binding_b",
        "component_b",
        "command",
        {{"joints", "shared_resource"}}}});
  EXPECT_THROW(
      (void)compose_cell_occurrences(
          conflicting, source_values({&source}), type_values({&type})),
      CellCompositionError);

  auto mixed_type = qualify_type_with_component_local_resources(
      TypeSpec{.id = "mixed_resource_identity_type",
               .tags = {},
               .with_target = true},
      {{"joints", "arm_joints"}}, {"vacuum"});
  const auto model_backed_vs_component_local = graph_for(
      {{"model", &source, false}},
      {{"component", &mixed_type, {{"body", "model"}}, true}},
      {},
      {{"binding_a",
        "component",
        "command",
        {{"joints", "shared_resource"}, {"vacuum", "vacuum_a"}}},
       {"binding_b",
        "component",
        "command",
        {{"joints", "joints_b"}, {"vacuum", "shared_resource"}}}});
  EXPECT_THROW(
      (void)compose_cell_occurrences(
          model_backed_vs_component_local, source_values({&source}),
          type_values({&mixed_type})),
      CellCompositionError);
}

TEST(CellComposition,
     ReconstructsRootToFrameTransformAcrossMountAndNonRootAttachment) {
  const auto quarter_turn = std::sqrt(0.5);
  const core::PlacementPoseDTO internal_pose{
      {0.25, 0.5, 0.75},
      {0.0, 0.0, quarter_turn, quarter_turn}};
  auto segment_source = qualify_source_with_documents(
      AssetSpec{.id = "segment_asset",
                .groups = {},
                .resources = {},
                .bodies = {{"body", {"base_link", "tip_link"}}}},
      "<robot name=\"segment_asset\"><link name=\"base_link\"/>"
      "<link name=\"tip_link\"/><joint name=\"joint1\" type=\"revolute\">"
      "<parent link=\"base_link\"/><child link=\"tip_link\"/>"
      "<origin xyz=\"0.25 0.5 0.75\" "
      "rpy=\"0 0 1.5707963267948966\"/>"
      "<limit lower=\"-3\" upper=\"3\" effort=\"1\" velocity=\"1\"/>"
      "</joint></robot>");
  auto tool_source = qualify_source(
      AssetSpec{.id = "path_tool_asset",
                .groups = {},
                .resources = {},
                .bodies = {{"body", {"base_link", "tip_link"}}}});
  const core::PlacementPoseDTO anchor_pose{
      {1.0, 2.0, 3.0}, {0.0, 0.0, quarter_turn, quarter_turn}};
  const core::PlacementPoseDTO mount_pose{
      {1.0, 0.0, 0.0}, {0.0, 0.0, quarter_turn, quarter_turn}};
  const core::PlacementPoseDTO attachment_pose{
      {0.0, 2.0, 0.0}, {0.0, 0.0, quarter_turn, quarter_turn}};

  TypeSpec aggregate_spec{
      .id = "mounted_parent_type",
      .tags = {"articulated-arm"},
      .model_roles = {"first", "second"},
      .root_role = "first",
      .with_interface = true,
      .interface_id = "flange",
      .interface_model_role = "second",
      .interface_frame = "tcp",
      .accepts_tags = {"robot-tool"},
  };
  aggregate_spec.mount = core::ComponentTypeModelMountDTO{
      {core::ModelRoleId("first"), core::FrameRoleId("tcp")},
      {core::ModelRoleId("second"), core::FrameRoleId("base")},
      mount_pose};
  auto aggregate_type = qualify_type(aggregate_spec);
  auto tool_type = qualify_type(
      TypeSpec{.id = "path_tool_type",
               .tags = {"robot-tool"},
               .with_interface = true,
               .interface_id = "mount",
               .interface_frame = "base",
               .accepts_tags = {"articulated-arm"}});
  const auto graph = graph_for(
      {{"parent_first", &segment_source, false},
       {"parent_second", &segment_source, false},
       {"tool_model", &tool_source, false}},
      {{"parent",
        &aggregate_type,
        {{"first", "parent_first"}, {"second", "parent_second"}},
        true},
       {"tool", &tool_type, {{"body", "tool_model"}}, false}},
      {{"tool_attachment", "parent", "flange", "tool", "mount"}},
      {}, {}, {{"parent", anchor_pose}},
      {{"tool_attachment", attachment_pose}});
  const auto composed = compose_cell_occurrences(
      graph, source_values({&tool_source, &segment_source}),
      type_values({&tool_type, &aggregate_type}));

  const auto path = std::find_if(
      composed.frame_paths().begin(), composed.frame_paths().end(),
      [](const CompositionFramePath& candidate) {
        const auto* frame =
            std::get_if<CompositionComponentModelFrame>(
                &candidate.terminal_frame);
        return frame != nullptr &&
               frame->controllable_component_id.value() == "tool" &&
               frame->frame_role_id.value() == "tcp";
      });
  ASSERT_NE(path, composed.frame_paths().end());

  std::size_t anchors = 0U;
  std::size_t mounts = 0U;
  std::size_t attachments = 0U;
  std::size_t model_joints = 0U;
  for (const auto& segment : path->segments) {
    std::visit(
        [&](const auto& selected) {
          using T = std::decay_t<decltype(selected)>;
          if constexpr (std::is_same_v<T,
                                       CompositionFixedPathSegment>) {
            ASSERT_LT(selected.fixed_connection_index,
                      composed.fixed_connections().size());
            const auto& source =
                composed.fixed_connections()[selected.fixed_connection_index]
                    .source;
            anchors +=
                std::holds_alternative<CompositionDirectComponentAnchor>(
                    source)
                    ? 1U
                    : 0U;
            mounts += std::holds_alternative<CompositionModelMount>(source)
                          ? 1U
                          : 0U;
            attachments +=
                std::holds_alternative<CompositionComponentAttachment>(
                    source)
                    ? 1U
                    : 0U;
          } else {
            ++model_joints;
          }
        },
        segment);
  }
  EXPECT_EQ(anchors, 1U);
  EXPECT_EQ(mounts, 1U);
  EXPECT_EQ(attachments, 1U);
  EXPECT_EQ(model_joints, 3U);

  const auto without_internal = compose_pose(
      compose_pose(anchor_pose, mount_pose), attachment_pose);
  const auto expected = compose_pose(
      compose_pose(
          compose_pose(
              compose_pose(anchor_pose, internal_pose),
              mount_pose),
          internal_pose),
      attachment_pose);
  expect_pose_near(reconstruct_path_pose(composed, *path), expected);
  EXPECT_NE(expected, without_internal);
}

TEST(CellComposition,
     ReusesOneAssetWithoutNameInjectionAndRetainsInternalMountPaths) {
  const std::string hostile_urdf =
      "<robot name=\"mf1_robot_injected\"><link name=\"mf1_link_injected\"/>"
      "<link name=\"tip\"/><joint name=\"mf1_joint_injected\" "
      "type=\"fixed\"><parent link=\"mf1_link_injected\"/>"
      "<child link=\"tip\"/></joint></robot>";
  auto source = qualify_source_with_documents(
      AssetSpec{.id = "same_tool",
                .frames = {{"base", "mf1_link_injected"},
                           {"tcp", "tip"}},
                .groups = {},
                .resources = {},
                .bodies = {{"body", {"mf1_link_injected", "tip"}}}},
      hostile_urdf);
  const core::PlacementPoseDTO identity{
      {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  TypeSpec type_spec{
      .id = "aggregate_type",
      .tags = {},
      .model_roles = {"first", "second"},
      .root_role = "first",
  };
  type_spec.mount = core::ComponentTypeModelMountDTO{
      {core::ModelRoleId("first"), core::FrameRoleId("tcp")},
      {core::ModelRoleId("second"), core::FrameRoleId("base")},
      identity};
  auto type = qualify_type(type_spec);
  const auto graph = graph_for(
      {{"tool_a", &source, false}, {"tool_b", &source, false}},
      {{"aggregate",
        &type,
        {{"first", "tool_a"}, {"second", "tool_b"}},
        true}});
  const auto composed = compose_cell_occurrences(
      graph, source_values({&source}), type_values({&type}));

  ASSERT_EQ(composed.model_occurrences().size(), 2U);
  EXPECT_NE(composed.model_occurrences()[0].compiled_root_link_name,
            composed.model_occurrences()[1].compiled_root_link_name);
  ASSERT_EQ(composed.fixed_connections().size(), 2U);
  EXPECT_TRUE(std::holds_alternative<CompositionDirectComponentAnchor>(
      composed.fixed_connections()[0].source));
  EXPECT_TRUE(std::holds_alternative<CompositionModelMount>(
      composed.fixed_connections()[1].source));
  for (const auto& rewrite : composed.symbol_rewrites()) {
    EXPECT_TRUE(is_mf1_name(rewrite.compiled_name));
    EXPECT_NE(rewrite.compiled_name, rewrite.source.source_name);
  }
  for (const auto& path : composed.frame_paths()) {
    const auto frame_name = std::visit(
        [](const auto& frame) { return frame.compiled_frame_name; },
        path.terminal_frame);
    EXPECT_TRUE(is_mf1_name(frame_name));
  }
}

TEST(CellComposition, PinsMf1DomainFieldEncodingAndBoundaryVectors) {
  const std::string urdf =
      "<robot name=\"golden:robot=1\">"
      "<link name=\"base:link=1\"/><link name=\"base:link=11\"/>"
      "<joint name=\"joint=1:two\" type=\"fixed\">"
      "<parent link=\"base:link=1\"/><child link=\"base:link=11\"/>"
      "</joint></robot>";
  auto source = qualify_source_with_documents(
      AssetSpec{.id = "golden_asset",
                .frames = {{"base", "base:link=1"},
                           {"tcp", "base:link=11"}},
                .groups = {},
                .resources = {},
                .bodies = {{"body",
                            {"base:link=1", "base:link=11"}}}},
      urdf);
  const auto composed = compose_cell_occurrences(
      graph_for({{"golden_model", &source, true}}, {}),
      source_values({&source}),
      std::span<const QualifiedComponentType>{});

  EXPECT_EQ(
      composed.cell_frame().compiled_frame_name,
      "mf1_cell_d2faaac9cc03c47c063f8f1b3d488d9b375589d0295f788c47552e2ce64b0cbc");
  EXPECT_EQ(
      composed.flattened_urdf().robot_name,
      "mf1_robot_cb4af488b4df85807d66e188803e78f2fab669bd5c3193178c9c5b5aae867a84");
  auto compiled_symbol =
      [&](OccurrenceSymbolKind kind,
          std::string_view source_name) -> std::string {
    const auto found = std::find_if(
        composed.symbol_rewrites().begin(),
        composed.symbol_rewrites().end(),
        [&](const OccurrenceSymbolRewrite& row) {
          return row.source.kind == kind &&
                 row.source.source_name == source_name;
        });
    if (found == composed.symbol_rewrites().end()) {
      throw std::runtime_error("golden symbol is absent");
    }
    return found->compiled_name;
  };
  EXPECT_EQ(
      compiled_symbol(OccurrenceSymbolKind::kLink, "base:link=1"),
      "mf1_link_f27ab8448bae2e59bec5afe8cd1dd898aa542dd1cb60504580725b9a5f2ba391");
  EXPECT_EQ(
      compiled_symbol(OccurrenceSymbolKind::kLink, "base:link=11"),
      "mf1_link_e78a3238d8cadd7dae93f697301dc8b96dc07fa46472ab925c977bab9c771261");
  ASSERT_EQ(composed.fixed_connections().size(), 1U);
  EXPECT_EQ(
      composed.fixed_connections()[0].compiled_joint_name,
      "mf1_anchor_32a472f805dd1e3eb2b7a9baeeda8d61df7aed146e730b165d50214c5ddd642d");
}

TEST(CellComposition,
     AcceptsExactPortableFrameBudgetAndRejectsTheFirstAdditionalRow) {
  std::vector<std::pair<std::string, std::string>> frame_roles;
  frame_roles.reserve(4096U);
  frame_roles.emplace_back("base", "base_link");
  for (std::size_t index = 1U; index < 4096U; ++index) {
    frame_roles.emplace_back(
        "frame_" + std::to_string(index), "base_link");
  }
  auto source = qualify_source(
      AssetSpec{.id = "frame_budget_asset",
                .frames = std::move(frame_roles),
                .groups = {},
                .resources = {}});
  auto extra_source = qualify_source(
      AssetSpec{.id = "frame_budget_extra_asset",
                .frames = {{"base", "base_link"}},
                .groups = {},
                .resources = {}});

  std::vector<ModelSpec> exact_models;
  exact_models.reserve(
      kMaxCompositionFrameResolutions / 4096U);
  for (std::size_t index = 0U;
       index < kMaxCompositionFrameResolutions / 4096U;
       ++index) {
    exact_models.push_back(
        {"frame_budget_model_" + std::to_string(index),
         &source,
         true});
  }
  const auto exact_graph = graph_for(exact_models, {});
  const auto exact = compose_cell_occurrences(
      exact_graph, source_values({&source}),
      std::span<const QualifiedComponentType>{});
  EXPECT_EQ(
      exact.frames().size(),
      kMaxCompositionFrameResolutions);

  exact_models.push_back(
      {"frame_budget_first_over", &extra_source, true});
  const auto first_over_graph = graph_for(exact_models, {});
  EXPECT_THROW(
      (void)compose_cell_occurrences(
          first_over_graph, source_values({&source, &extra_source}),
          std::span<const QualifiedComponentType>{}),
      CellCompositionError);
}

}  // namespace
}  // namespace manyforge::deployment
