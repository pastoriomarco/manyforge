#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/capability_definition_codec.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/cell_structure.hpp>
#include <manyforge_deployment/component_type_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/model_asset_codec.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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

static_assert(!std::is_aggregate_v<StructurallyQualifiedCellGraph>);
static_assert(
    !std::is_default_constructible_v<StructurallyQualifiedCellGraph>);

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

[[nodiscard]] QualifiedSource qualify_source(const AssetSpec& input) {
  AssetSpec spec = input;
  std::sort(spec.frames.begin(), spec.frames.end());
  std::sort(spec.groups.begin(), spec.groups.end());
  std::sort(spec.resources.begin(), spec.resources.end());
  std::sort(spec.bodies.begin(), spec.bodies.end());

  const std::string urdf = urdf_for(spec.id);
  const std::optional<std::string> srdf =
      spec.with_srdf ? std::optional<std::string>(srdf_for(spec.id))
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

[[nodiscard]] QualifiedType qualify_type(const TypeSpec& spec) {
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
                  CanonicalValue(0.0), CanonicalValue(0.0),
                  CanonicalValue(0.0)})},
             {"orientation_xyzw",
              CanonicalValue(CanonicalValue::Array{
                  CanonicalValue(0.0), CanonicalValue(0.0),
                  CanonicalValue(0.0), CanonicalValue(1.0)})},
         })},
    });
  }

  CanonicalValue::Array targets;
  std::vector<QualifiedCapabilityDefinition> capabilities;
  if (spec.with_target) {
    auto capability = observed_state_capability();
    const auto revision =
        capability.value().capability_definition_revision.value();
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
        {"resource_roles",
         CanonicalValue(CanonicalValue::Array{
             CanonicalValue(CanonicalValue::Object{
                 {"resource_role_id", CanonicalValue("joints")},
                 {"source_kind", CanonicalValue("model_role")},
                 {"model_role_id",
                  CanonicalValue(spec.target_model_role)},
                 {"asset_resource_role_id",
                  CanonicalValue(spec.target_resource_source)},
             })})},
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

[[nodiscard]] GraphRevisionVerifiedCellGraph graph_for(
    std::vector<ModelSpec> models, std::vector<ComponentSpec> components,
    std::vector<AttachmentSpec> attachments = {}) {
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
      row.emplace_back("root_pose_in_cell", identity_pose());
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
      row.emplace_back("root_pose_in_cell", identity_pose());
    }
    component_rows.emplace_back(std::move(row));
  }
  CanonicalValue::Array attachment_rows;
  for (const auto& attachment : attachments) {
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
        {"child_interface_pose_in_parent_interface", identity_pose()},
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
          {"control_domains", CanonicalValue(CanonicalValue::Array{})},
          {"control_bindings", CanonicalValue(CanonicalValue::Array{})},
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
  const auto qualified = qualify_cell_structure(
      graph, std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});

  EXPECT_EQ(qualified.graph(), graph.graph());
  EXPECT_TRUE(qualified.model_assets().empty());
  EXPECT_TRUE(qualified.component_types().empty());
  EXPECT_TRUE(qualified.model_instances().empty());
  EXPECT_TRUE(qualified.components().empty());
  EXPECT_TRUE(qualified.attachments().empty());
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

  const auto qualified = qualify_cell_structure(graph, sources, types);

  EXPECT_EQ(qualified.graph(), graph_before);
  EXPECT_EQ(qualified.verified_graph().normalized_root(), root_before);
  ASSERT_EQ(qualified.model_assets().size(), 3U);
  EXPECT_EQ(qualified.model_assets().front().value().model_asset_id.value(),
            "arm_asset");
  ASSERT_EQ(qualified.model_instances().size(), 3U);
  EXPECT_EQ(qualified.model_instances()[0].placement_source,
            StructuralPlacementSource::kDirectComponentPoseInCell);
  EXPECT_EQ(qualified.model_instances()[1].placement_source,
            StructuralPlacementSource::kDirectModelPoseInCell);
  EXPECT_FALSE(
      qualified.model_instances()[1].owner_component_id.has_value());
  EXPECT_EQ(qualified.model_instances()[2].placement_source,
            StructuralPlacementSource::kDerivedFromComponentAttachment);
  ASSERT_EQ(qualified.components().size(), 2U);
  ASSERT_EQ(qualified.components()[0].command_targets.size(), 1U);
  EXPECT_EQ(qualified.components()[0]
                .command_targets[0]
                .model_groups[0]
                .source_group_name,
            "arm_group");
  EXPECT_EQ(std::get<StructuralModelResourceResolution>(
                qualified.components()[0]
                    .command_targets[0]
                    .resources[0])
                .source_joint_names,
            (std::vector<std::string>{"joint1"}));
  ASSERT_EQ(qualified.attachments().size(), 1U);
  EXPECT_EQ(qualified.attachments()[0].parent.frame.source_link_name,
            "tip_link");
  EXPECT_EQ(qualified.attachments()[0].child.frame.source_link_name,
            "base_link");
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
      (void)qualify_cell_structure(
          graph, std::span<const SourceVerifiedModelAsset>{}, types),
      CellStructureError);
  const std::vector<SourceVerifiedModelAsset> duplicate_sources{
      source.value, source.value};
  EXPECT_THROW(
      (void)qualify_cell_structure(graph, duplicate_sources, types),
      CellStructureError);
  auto stale_source = qualify_source(
      AssetSpec{.id = "asset",
                .cardinality = "one_per_cell",
                .with_srdf = true});
  const auto stale_sources = source_values({&stale_source});
  EXPECT_THROW(
      (void)qualify_cell_structure(graph, stale_sources, types),
      CellStructureError);
  auto extra_source =
      qualify_source(AssetSpec{.id = "extra", .with_srdf = true});
  const auto extra_sources = source_values({&source, &extra_source});
  EXPECT_THROW(
      (void)qualify_cell_structure(graph, extra_sources, types),
      CellStructureError);

  EXPECT_THROW(
      (void)qualify_cell_structure(
          graph, sources, std::span<const QualifiedComponentType>{}),
      CellStructureError);
  const std::vector<QualifiedComponentType> duplicate_types{
      type.value, type.value};
  EXPECT_THROW(
      (void)qualify_cell_structure(graph, sources, duplicate_types),
      CellStructureError);
  auto stale_type = qualify_type(
      TypeSpec{.id = "type", .tags = {"changed"}, .with_target = true});
  const auto stale_types = type_values({&stale_type});
  EXPECT_THROW(
      (void)qualify_cell_structure(graph, sources, stale_types),
      CellStructureError);
  auto extra_type =
      qualify_type(TypeSpec{.id = "extra_type", .tags = {}});
  const auto extra_types = type_values({&type, &extra_type});
  EXPECT_THROW(
      (void)qualify_cell_structure(graph, sources, extra_types),
      CellStructureError);
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
      (void)qualify_cell_structure(
          two_passive, singleton_sources,
          std::span<const QualifiedComponentType>{}),
      CellStructureError);

  auto source = qualify_source(AssetSpec{.id = "asset"});
  auto type = qualify_type(TypeSpec{.id = "type", .tags = {}});
  const auto wrong_role = graph_for(
      {{"model", &source, false}},
      {{"component", &type, {{"wrong", "model"}}, true}});
  const auto sources = source_values({&source});
  const auto types = type_values({&type});
  EXPECT_THROW(
      (void)qualify_cell_structure(wrong_role, sources, types),
      CellStructureError);
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
      (void)qualify_cell_structure(
          wrong_root_graph, sources, wrong_root_types),
      CellStructureError);

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
      (void)qualify_cell_structure(
          missing_target_graph, sources, missing_target_types),
      CellStructureError);

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
  const auto mounted = qualify_cell_structure(
      good_mount_graph, mount_sources, good_mount_types);
  ASSERT_EQ(mounted.components()[0].model_mounts.size(), 1U);
  EXPECT_EQ(mounted.components()[0].model_mounts[0].parent.source_link_name,
            "tip_link");
  EXPECT_EQ(mounted.components()[0].model_mounts[0].child.source_link_name,
            "base_link");
  EXPECT_EQ(mounted.model_instances()[1].placement_source,
            StructuralPlacementSource::kDerivedFromComponentModelMount);

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
      (void)qualify_cell_structure(
          bad_mount_graph, mount_sources, mount_types),
      CellStructureError);
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
      (void)qualify_cell_structure(missing, sources, types),
      CellStructureError);

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
      (void)qualify_cell_structure(
          incompatible, sources, incompatible_types),
      CellStructureError);

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
      (void)qualify_cell_structure(
          nonroot_child, sources, nonroot_types),
      CellStructureError);

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
      (void)qualify_cell_structure(capacity, sources, types),
      CellStructureError);
}

}  // namespace
}  // namespace manyforge::deployment
