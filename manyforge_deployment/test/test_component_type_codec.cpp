#include <gtest/gtest.h>

#include <algorithm>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/component_type.hpp>
#include <manyforge_deployment/capability_definition_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/component_type_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment {
namespace {

constexpr std::string_view kCapabilitySchemaRevision =
    "sha256:2de23ede0e941349e90f0a3a344b7d240426703d250234f86e788343213727e0";
constexpr std::string_view kComponentTypeSchemaRevision =
    "sha256:d971a7022a7977625256649896f8253020686a7b543c30d557162ebee8f69dde";
constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

static_assert(!std::is_aggregate_v<QualifiedComponentType>);
static_assert(!std::is_default_constructible_v<QualifiedComponentType>);

struct CapabilitySelection final {
  std::string capability_id;
  std::optional<std::string> revision_override;
};

struct TargetSpec final {
  std::string target_id{"arm"};
  std::string source_model_role{"base"};
  std::string collision_body_model_role;
  std::vector<std::string> contact_body_role_ids;
  std::vector<std::string> sibling_contact_body_role_ids;
  bool include_model_aliases = true;
  bool include_model_resource = true;
  bool include_local_resource = true;
  bool duplicate_group_source = false;
  bool duplicate_resource_source = false;
  std::vector<CapabilitySelection> capabilities{
      {"planning.motion.pose_goal", std::nullopt},
      {"state.observed.current", std::nullopt},
  };
  // TCP-A: frame roles tagged `tcp` (`flange` adds a flange row) and the
  // target's default TCP.
  std::vector<std::string> tcp_frames{};
  std::optional<std::string> default_tcp{};
};

struct TypeSpec final {
  std::string type_id{"composite_arm"};
  std::vector<std::string> model_roles{"base", "tool"};
  bool include_root = true;
  bool force_root_without_models = false;
  bool invalid_root_child_mount = false;
  bool disconnected_mount_cycle = false;
  bool invalid_mount_pose = false;
  bool include_interface = true;
  std::string interface_model_role{"tool"};
  std::vector<std::string> tags{"articulated-arm", "robot-tool"};
  std::vector<TargetSpec> targets{TargetSpec{}};
  bool omit_max_attachments = false;
};

[[nodiscard]] CanonicalValue string_array(
    const std::vector<std::string>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] std::string capability_revision(std::string_view id) {
  const auto* descriptor =
      core::find_builtin_capability_definition(id);
  if (descriptor == nullptr) {
    throw std::runtime_error("test selected an unknown capability");
  }
  return std::string(descriptor->capability_definition_revision);
}

[[nodiscard]] std::string capability_document(std::string_view id) {
  const auto* descriptor =
      core::find_builtin_capability_definition(id);
  if (descriptor == nullptr ||
      descriptor->requirement_parameters_schema_ref.has_value()) {
    throw std::runtime_error(
        "test requires a known capability without a value-schema ref");
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
  return canonical_json(CanonicalValue(std::move(root)));
}

[[nodiscard]] QualifiedCapabilityDefinition qualify_capability(
    std::string_view id) {
  const auto document = capability_document(id);
  const auto revision = capability_revision(id);
  const core::ContentAddressedStoreKey store_key{
      "sha256/capability/" + std::string(id)};
  return resolve_and_qualify_capability_definition(
      {
          core::CapabilityId(std::string(id)),
          core::CapabilityDefinitionRevision(revision),
          core::SchemaId(
              std::string(core::kCapabilityDefinitionApiVersion)),
          core::SchemaRevision(std::string(kCapabilitySchemaRevision)),
          core::ContentDigest(sha256_digest(document)),
          store_key,
      },
      store_key, document);
}

[[nodiscard]] CanonicalValue capability_rows(
    const std::vector<CapabilitySelection>& capabilities) {
  CanonicalValue::Array rows;
  rows.reserve(capabilities.size());
  for (const auto& capability : capabilities) {
    rows.emplace_back(CanonicalValue::Object{
        {"capability_id", CanonicalValue(capability.capability_id)},
        {"capability_definition_revision",
         CanonicalValue(
             capability.revision_override.value_or(
                 capability_revision(capability.capability_id)))},
    });
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue target_value(const TargetSpec& spec) {
  CanonicalValue::Array groups;
  CanonicalValue::Array frames;
  CanonicalValue::Array bodies;
  const auto tagged = [&spec](std::string_view role) {
    return std::find(spec.tcp_frames.begin(), spec.tcp_frames.end(), role) !=
           spec.tcp_frames.end();
  };
  if (spec.include_model_aliases) {
    groups.emplace_back(CanonicalValue::Object{
        {"model_group_role_id", CanonicalValue("arm_group")},
        {"model_role_id", CanonicalValue(spec.source_model_role)},
        {"asset_model_group_role_id", CanonicalValue("arm")},
    });
    if (tagged("flange")) {
      frames.emplace_back(CanonicalValue::Object{
          {"frame_role_id", CanonicalValue("flange")},
          {"model_role_id", CanonicalValue(spec.source_model_role)},
          {"asset_frame_role_id", CanonicalValue("flange")},
          {"tcp", CanonicalValue(true)},
      });
    }
    CanonicalValue::Object frame{
        {"frame_role_id", CanonicalValue("tcp")},
        {"model_role_id", CanonicalValue(spec.source_model_role)},
        {"asset_frame_role_id", CanonicalValue("tcp")},
    };
    if (tagged("tcp")) {
      frame.emplace_back("tcp", CanonicalValue(true));
    }
    if (!spec.contact_body_role_ids.empty()) {
      frame.emplace_back("contact_body_role_ids",
                         string_array(spec.contact_body_role_ids));
    }
    if (!spec.sibling_contact_body_role_ids.empty()) {
      frames.emplace_back(CanonicalValue::Object{
          {"frame_role_id", CanonicalValue("sibling_tcp")},
          {"model_role_id", CanonicalValue(spec.source_model_role)},
          {"asset_frame_role_id", CanonicalValue("sibling_tcp")},
          {"contact_body_role_ids",
           string_array(spec.sibling_contact_body_role_ids)},
      });
    }
    frames.emplace_back(std::move(frame));
    bodies.emplace_back(CanonicalValue::Object{
        {"collision_body_role_id", CanonicalValue("arm_collision")},
        {"model_role_id",
         CanonicalValue(spec.collision_body_model_role.empty()
                            ? spec.source_model_role
                            : spec.collision_body_model_role)},
        {"asset_collision_body_role_id",
         CanonicalValue("arm_collision")},
    });
    if (!spec.sibling_contact_body_role_ids.empty()) {
      bodies.emplace_back(CanonicalValue::Object{
          {"collision_body_role_id", CanonicalValue("sibling_collision")},
          {"model_role_id", CanonicalValue(spec.source_model_role)},
          {"asset_collision_body_role_id",
           CanonicalValue("sibling_collision")},
      });
    }
    if (spec.duplicate_group_source) {
      groups.emplace_back(CanonicalValue::Object{
          {"model_group_role_id", CanonicalValue("arm_group_alias")},
          {"model_role_id", CanonicalValue(spec.source_model_role)},
          {"asset_model_group_role_id", CanonicalValue("arm")},
      });
    }
  }

  CanonicalValue::Array resources;
  if (spec.include_model_resource) {
    resources.emplace_back(CanonicalValue::Object{
        {"resource_role_id", CanonicalValue("arm_joints")},
        {"source_kind", CanonicalValue("model_role")},
        {"model_role_id", CanonicalValue(spec.source_model_role)},
        {"asset_resource_role_id", CanonicalValue("arm_joints")},
    });
    if (spec.duplicate_resource_source) {
      resources.emplace_back(CanonicalValue::Object{
          {"resource_role_id", CanonicalValue("arm_joints_alias")},
          {"source_kind", CanonicalValue("model_role")},
          {"model_role_id", CanonicalValue(spec.source_model_role)},
          {"asset_resource_role_id", CanonicalValue("arm_joints")},
      });
    }
  }
  if (spec.include_local_resource) {
    resources.emplace_back(CanonicalValue::Object{
        {"resource_role_id", CanonicalValue("vacuum")},
        {"source_kind", CanonicalValue("component_local")},
    });
  }

  CanonicalValue::Object target{
      {"command_target_id", CanonicalValue(spec.target_id)},
      {"model_group_roles", CanonicalValue(std::move(groups))},
      {"resource_roles", CanonicalValue(std::move(resources))},
      {"frame_roles", CanonicalValue(std::move(frames))},
      {"collision_body_roles", CanonicalValue(std::move(bodies))},
      {"capabilities", capability_rows(spec.capabilities)},
  };
  if (spec.default_tcp.has_value()) {
    target.emplace_back("default_tcp_frame_role_id",
                        CanonicalValue(*spec.default_tcp));
  }
  return CanonicalValue(std::move(target));
}

[[nodiscard]] CanonicalValue component_identity_value(
    const TypeSpec& spec) {
  CanonicalValue::Array model_roles;
  for (const auto& role : spec.model_roles) {
    model_roles.emplace_back(CanonicalValue::Object{
        {"model_role_id", CanonicalValue(role)},
        {"cardinality", CanonicalValue("exactly_one")},
    });
  }

  CanonicalValue::Array mounts;
  if (spec.disconnected_mount_cycle && spec.model_roles.size() == 3U) {
    const std::vector<std::pair<std::string, std::string>> edges{
        {spec.model_roles[2], spec.model_roles[1]},
        {spec.model_roles[1], spec.model_roles[2]},
    };
    for (const auto& [parent, child] : edges) {
      mounts.emplace_back(CanonicalValue::Object{
          {"parent",
           CanonicalValue(CanonicalValue::Object{
               {"model_role_id", CanonicalValue(parent)},
               {"frame_role_id", CanonicalValue("flange")},
           })},
          {"child",
           CanonicalValue(CanonicalValue::Object{
               {"model_role_id", CanonicalValue(child)},
               {"frame_role_id", CanonicalValue("base")},
           })},
          {"child_frame_pose_in_parent_frame",
           CanonicalValue(CanonicalValue::Object{
               {"position_m",
                CanonicalValue(CanonicalValue::Array{
                    CanonicalValue(0.0), CanonicalValue(0.0),
                    CanonicalValue(0.1)})},
               {"orientation_xyzw",
                CanonicalValue(CanonicalValue::Array{
                    CanonicalValue(0.0), CanonicalValue(0.0),
                    CanonicalValue(0.0),
                    CanonicalValue(spec.invalid_mount_pose ? 2.0 : 1.0)})},
           })},
      });
    }
  } else if (spec.model_roles.size() == 2U) {
    const auto parent =
        spec.invalid_root_child_mount ? spec.model_roles[1] : spec.model_roles[0];
    const auto child =
        spec.invalid_root_child_mount ? spec.model_roles[0] : spec.model_roles[1];
    mounts.emplace_back(CanonicalValue::Object{
        {"parent",
         CanonicalValue(CanonicalValue::Object{
             {"model_role_id", CanonicalValue(parent)},
             {"frame_role_id", CanonicalValue("flange")},
         })},
        {"child",
         CanonicalValue(CanonicalValue::Object{
             {"model_role_id", CanonicalValue(child)},
             {"frame_role_id", CanonicalValue("base")},
         })},
        {"child_frame_pose_in_parent_frame",
         CanonicalValue(CanonicalValue::Object{
             {"position_m",
              CanonicalValue(CanonicalValue::Array{
                  CanonicalValue(0.0), CanonicalValue(0.0),
                  CanonicalValue(0.1)})},
             {"orientation_xyzw",
              CanonicalValue(CanonicalValue::Array{
                  CanonicalValue(0.0), CanonicalValue(0.0),
                  CanonicalValue(0.0),
                  CanonicalValue(spec.invalid_mount_pose ? 2.0 : 1.0)})},
         })},
    });
  }

  CanonicalValue::Array targets;
  for (const auto& target : spec.targets) {
    targets.push_back(target_value(target));
  }

  CanonicalValue::Array interfaces;
  if (spec.include_interface) {
    interfaces.emplace_back(CanonicalValue::Object{
        {"attachment_interface_id", CanonicalValue("flange")},
        {"model_role_id", CanonicalValue(spec.interface_model_role)},
        {"frame_role_id", CanonicalValue("tcp")},
        {"accepts_tags",
         string_array(std::vector<std::string>{"end-effector"})},
        {"max_attachments", CanonicalValue(1.0)},
    });
  }

  CanonicalValue::Object root{
      {"api_version",
       CanonicalValue(std::string(core::kComponentTypeApiVersion))},
      {"component_type_id", CanonicalValue(spec.type_id)},
      {"tags", string_array(spec.tags)},
      {"model_roles", CanonicalValue(std::move(model_roles))},
      {"model_mounts", CanonicalValue(std::move(mounts))},
      {"command_targets", CanonicalValue(std::move(targets))},
      {"attachment_interfaces", CanonicalValue(std::move(interfaces))},
  };
  if (spec.include_root &&
      (!spec.model_roles.empty() || spec.force_root_without_models)) {
    const auto root_model_role =
        spec.model_roles.empty() ? std::string("base")
                                 : spec.model_roles.front();
    root.emplace_back(
        "component_root",
        CanonicalValue(CanonicalValue::Object{
            {"model_role_id", CanonicalValue(root_model_role)},
            {"frame_role_id", CanonicalValue("base")},
        }));
  }
  return CanonicalValue(std::move(root));
}

[[nodiscard]] std::string component_document(const TypeSpec& spec) {
  const auto identity = component_identity_value(spec);
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/component_type");
  preimage.push_back('\0');
  preimage.append(core::kComponentTypeApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(identity));
  auto root = identity.as_object();
  root.emplace_back("component_type_revision",
                    CanonicalValue(sha256_digest(preimage)));
  auto document = canonical_json(CanonicalValue(std::move(root)));
  if (spec.omit_max_attachments) {
    constexpr std::string_view field = ",\"max_attachments\":1";
    const auto offset = document.find(field);
    if (offset == std::string::npos) {
      throw std::runtime_error(
          "test failed to find max_attachments for omission");
    }
    document.erase(offset, field.size());
  }
  return document;
}

[[nodiscard]] std::string revision_from_document(
    std::string_view document) {
  const auto parsed = parse_strict_json_model_yaml(document);
  const auto* revision =
      parsed.root().find_object_member("component_type_revision");
  if (revision == nullptr) {
    throw std::runtime_error("test document has no component revision");
  }
  return std::get<std::string>(revision->storage());
}

[[nodiscard]] core::ComponentTypeCatalogRecordRefDTO component_reference(
    std::string_view type_id, std::string_view document,
    const core::ContentAddressedStoreKey& store_key) {
  return {
      core::ComponentTypeId(std::string(type_id)),
      core::ComponentTypeRevision(revision_from_document(document)),
      core::SchemaId(std::string(core::kComponentTypeApiVersion)),
      core::SchemaRevision(std::string(kComponentTypeSchemaRevision)),
      core::ContentDigest(sha256_digest(document)),
      store_key,
  };
}

[[nodiscard]] QualifiedComponentType qualify(
    const TypeSpec& spec,
    const std::vector<QualifiedCapabilityDefinition>& dependencies) {
  const auto document = component_document(spec);
  const core::ContentAddressedStoreKey store_key{
      "sha256/component-type/" + spec.type_id};
  return resolve_and_qualify_component_type(
      component_reference(spec.type_id, document, store_key),
      store_key, document,
      std::span<const QualifiedCapabilityDefinition>{dependencies});
}

[[nodiscard]] std::vector<QualifiedCapabilityDefinition>
rich_dependencies() {
  std::vector<QualifiedCapabilityDefinition> result;
  result.push_back(qualify_capability("planning.motion.pose_goal"));
  result.push_back(qualify_capability("state.observed.current"));
  return result;
}

void expect_rejection(
    const TypeSpec& spec,
    const std::vector<QualifiedCapabilityDefinition>& dependencies,
    std::string_view expected) {
  try {
    static_cast<void>(qualify(spec, dependencies));
    FAIL() << "expected ComponentType qualification to fail";
  } catch (const ComponentTypeCodecError& error) {
    EXPECT_NE(std::string(error.what()).find(expected),
              std::string::npos)
        << error.what();
  }
}

void expect_closed_schema_rejection(
    const TypeSpec& spec,
    const std::vector<QualifiedCapabilityDefinition>& dependencies) {
  EXPECT_THROW(static_cast<void>(qualify(spec, dependencies)),
               ClosedSchemaError);
}

TEST(ComponentTypeCodecTest,
     QualifiesStructureAliasesMixedResourcesAndDefault) {
  TypeSpec spec;
  spec.omit_max_attachments = true;
  const auto qualified = qualify(spec, rich_dependencies());
  const auto& type = qualified.value();
  EXPECT_EQ(
      qualified.reference(),
      component_reference(
          spec.type_id, component_document(spec),
          core::ContentAddressedStoreKey("sha256/component-type/" +
                                         spec.type_id)));

  EXPECT_EQ(type.component_type_id.value(), "composite_arm");
  ASSERT_TRUE(type.component_root.has_value());
  EXPECT_EQ(type.model_roles.size(), 2U);
  EXPECT_EQ(type.model_mounts.size(), 1U);
  ASSERT_EQ(type.command_targets.size(), 1U);
  const auto& target = type.command_targets.front();
  ASSERT_EQ(target.resource_roles.size(), 2U);
  ASSERT_TRUE(std::holds_alternative<
              core::ComponentTypeModelResourceRoleDTO>(
      target.resource_roles[0]));
  const auto& model_resource =
      std::get<core::ComponentTypeModelResourceRoleDTO>(
          target.resource_roles[0]);
  EXPECT_EQ(model_resource.resource_role_id.value(), "arm_joints");
  EXPECT_EQ(model_resource.model_role_id.value(), "base");
  EXPECT_EQ(core::resource_source_kind(target.resource_roles[0]),
            core::ComponentTypeResourceSourceKind::kModelRole);
  ASSERT_TRUE(std::holds_alternative<
              core::ComponentTypeLocalResourceRoleDTO>(
      target.resource_roles[1]));
  EXPECT_EQ(
      std::get<core::ComponentTypeLocalResourceRoleDTO>(
          target.resource_roles[1])
          .resource_role_id.value(),
      "vacuum");
  EXPECT_EQ(core::resource_source_kind(target.resource_roles[1]),
            core::ComponentTypeResourceSourceKind::kComponentLocal);
  ASSERT_EQ(type.attachment_interfaces.size(), 1U);
  EXPECT_EQ(type.attachment_interfaces.front().max_attachments, 1U);
  EXPECT_NE(canonical_json(qualified.normalized_root())
                .find("\"max_attachments\":1"),
            std::string::npos);
  EXPECT_EQ(canonical_json(qualified.normalized_root())
                .find("controller_topology"),
            std::string::npos);
}

TEST(ComponentTypeCodecTest,
     EndpointContactRolesAreOptionalAndBoundToTheirTargetModel) {
  TypeSpec legacy_spec;
  const auto legacy = qualify(legacy_spec, rich_dependencies());
  ASSERT_EQ(legacy.value().command_targets.size(), 1U);
  ASSERT_EQ(legacy.value().command_targets.front().frame_roles.size(), 1U);
  EXPECT_TRUE(legacy.value().command_targets.front()
                  .frame_roles.front()
                  .contact_body_role_ids.empty());

  TypeSpec endpoint_spec;
  endpoint_spec.targets.front().contact_body_role_ids = {"arm_collision"};
  endpoint_spec.targets.front().sibling_contact_body_role_ids = {
      "sibling_collision"};
  const auto endpoint = qualify(endpoint_spec, rich_dependencies());
  const auto& endpoint_frames =
      endpoint.value().command_targets.front().frame_roles;
  ASSERT_EQ(endpoint_frames.size(), 2U);
  const auto frame = std::find_if(
      endpoint_frames.begin(), endpoint_frames.end(), [](const auto& value) {
        return value.frame_role_id == core::FrameRoleId("tcp");
      });
  ASSERT_NE(frame, endpoint_frames.end());
  ASSERT_EQ(frame->contact_body_role_ids.size(), 1U);
  EXPECT_EQ(frame->contact_body_role_ids.front().value(), "arm_collision");
  const auto sibling_frame = std::find_if(
      endpoint_frames.begin(), endpoint_frames.end(), [](const auto& value) {
        return value.frame_role_id == core::FrameRoleId("sibling_tcp");
      });
  ASSERT_NE(sibling_frame, endpoint_frames.end());
  ASSERT_EQ(sibling_frame->contact_body_role_ids.size(), 1U);
  EXPECT_EQ(sibling_frame->contact_body_role_ids.front().value(),
            "sibling_collision");

  auto wrong_model = endpoint_spec;
  wrong_model.targets.front().collision_body_model_role = "tool";
  EXPECT_THROW(qualify(wrong_model, rich_dependencies()),
               ComponentTypeCodecError);

  auto missing_body = endpoint_spec;
  missing_body.targets.front().contact_body_role_ids = {"missing_body"};
  EXPECT_THROW(qualify(missing_body, rich_dependencies()),
               ComponentTypeCodecError);

  auto duplicate_body = endpoint_spec;
  duplicate_body.targets.front().contact_body_role_ids = {
      "arm_collision", "arm_collision"};
  EXPECT_ANY_THROW(qualify(duplicate_body, rich_dependencies()));
}

// TCP-A: TCP tags and the default TCP; a motion target tags its flange.
TEST(ComponentTypeCodecTest, TcpTagsNeedOneTaggedDefaultAndAMotionFlange) {
  TypeSpec spec;
  spec.targets.front().tcp_frames = {"tcp"};
  spec.targets.front().default_tcp = "tcp";
  const auto qualified = qualify(spec, rich_dependencies());
  const auto& target = qualified.value().command_targets.front();
  ASSERT_EQ(target.frame_roles.size(), 1U);
  EXPECT_TRUE(target.frame_roles.front().tcp);
  EXPECT_EQ(target.default_tcp_frame_role_id,
            std::optional<core::FrameRoleId>(core::FrameRoleId("tcp")));

  TypeSpec untagged;  // no TCP: no default, and nothing changes
  const auto plain = qualify(untagged, rich_dependencies());
  EXPECT_FALSE(plain.value().command_targets.front().frame_roles.front().tcp);
  EXPECT_FALSE(plain.value().command_targets.front()
                   .default_tcp_frame_role_id.has_value());

  auto missing_default = spec;
  missing_default.targets.front().default_tcp.reset();
  expect_rejection(missing_default, rich_dependencies(), "default_tcp_unknown");
  auto untagged_default = untagged;
  untagged_default.targets.front().default_tcp = "tcp";
  expect_rejection(untagged_default, rich_dependencies(), "default_tcp_unknown");

  auto motion = spec;
  // Capabilities are a keyed set: keep them sorted so the revision matches.
  motion.targets.front().capabilities.insert(
      motion.targets.front().capabilities.begin(),
      {"execution.trajectory.execute", std::nullopt});
  auto motion_dependencies = rich_dependencies();
  motion_dependencies.push_back(
      qualify_capability("execution.trajectory.execute"));
  expect_rejection(motion, motion_dependencies, "robot_flange_tcp_missing");
  motion.targets.front().tcp_frames = {"flange", "tcp"};
  const auto robot = qualify(motion, motion_dependencies);
  EXPECT_EQ(robot.value().command_targets.front().frame_roles.size(), 2U);
}

// O5: a passive tool's frame-only target selects no capability.
TEST(ComponentTypeCodecTest, TargetWithoutCapabilitiesExposesOnlyFrames) {
  TypeSpec spec;
  spec.targets.front().capabilities.clear();
  spec.targets.front().include_model_resource = false;
  spec.targets.front().include_local_resource = false;
  spec.targets.front().tcp_frames = {"tcp"};
  spec.targets.front().default_tcp = "tcp";
  auto with_group = spec;  // a model group needs a capability
  const std::vector<QualifiedCapabilityDefinition> none;
  expect_rejection(with_group, none, "may expose only frame");
}

TEST(ComponentTypeCodecTest, QualifiesZeroModelIoOnlyType) {
  TypeSpec spec;
  spec.type_id = "vacuum_io";
  spec.model_roles.clear();
  spec.include_root = false;
  spec.include_interface = false;
  spec.tags.clear();
  spec.targets = {TargetSpec{
      .target_id = "vacuum",
      .collision_body_model_role = {},
      .contact_body_role_ids = {},
      .sibling_contact_body_role_ids = {},
      .include_model_aliases = false,
      .include_model_resource = false,
      .include_local_resource = true,
      .capabilities = {
          {"state.observed.current", std::nullopt},
      },
  }};
  std::vector<QualifiedCapabilityDefinition> dependencies;
  dependencies.push_back(
      qualify_capability("state.observed.current"));
  const auto qualified = qualify(spec, dependencies);
  EXPECT_TRUE(qualified.value().model_roles.empty());
  EXPECT_FALSE(qualified.value().component_root.has_value());
  ASSERT_EQ(qualified.value().command_targets.size(), 1U);
  ASSERT_EQ(qualified.value().command_targets[0].resource_roles.size(),
            1U);
  EXPECT_EQ(
      core::resource_source_kind(
          qualified.value().command_targets[0].resource_roles[0]),
      core::ComponentTypeResourceSourceKind::kComponentLocal);
}

TEST(ComponentTypeCodecTest, QualifiesFixedStructuralToolWithoutTargets) {
  TypeSpec spec;
  spec.type_id = "fixed_tool";
  spec.model_roles = {"tool"};
  spec.targets.clear();
  const std::vector<QualifiedCapabilityDefinition> dependencies;
  const auto qualified = qualify(spec, dependencies);
  EXPECT_EQ(qualified.value().model_roles.size(), 1U);
  EXPECT_TRUE(qualified.value().command_targets.empty());
  ASSERT_EQ(qualified.value().attachment_interfaces.size(), 1U);
  EXPECT_EQ(
      qualified.value().attachment_interfaces.front().model_role_id.value(),
      "tool");
}

TEST(ComponentTypeCodecTest, RejectsEmptyAndBrokenStructuralTypes) {
  TypeSpec empty;
  empty.type_id = "empty";
  empty.model_roles.clear();
  empty.include_root = false;
  empty.include_interface = false;
  empty.tags.clear();
  empty.targets.clear();
  expect_rejection(empty, {}, "cannot be completely empty");

  TypeSpec missing_root;
  missing_root.include_root = false;
  expect_rejection(missing_root, rich_dependencies(),
                   "requires component_root");

  TypeSpec root_child;
  root_child.invalid_root_child_mount = true;
  expect_rejection(root_child, rich_dependencies(),
                   "root model role cannot be a mount child");

  TypeSpec disconnected_cycle;
  disconnected_cycle.model_roles = {"base", "sensor", "tool"};
  disconnected_cycle.disconnected_mount_cycle = true;
  expect_rejection(disconnected_cycle, rich_dependencies(),
                   "connected tree");

  TypeSpec zero_models_with_root;
  zero_models_with_root.model_roles.clear();
  zero_models_with_root.force_root_without_models = true;
  zero_models_with_root.include_interface = false;
  zero_models_with_root.targets = {TargetSpec{
      .target_id = "vacuum",
      .source_model_role = "base",
      .collision_body_model_role = {},
      .contact_body_role_ids = {},
      .sibling_contact_body_role_ids = {},
      .include_model_aliases = false,
      .include_model_resource = false,
      .include_local_resource = true,
      .capabilities = {
          {"state.observed.current", std::nullopt},
      },
  }};
  std::vector<QualifiedCapabilityDefinition> state_dependency;
  state_dependency.push_back(
      qualify_capability("state.observed.current"));
  expect_rejection(zero_models_with_root, state_dependency,
                   "without model roles cannot declare");

  TypeSpec interface_without_tags;
  interface_without_tags.tags.clear();
  expect_rejection(interface_without_tags, rich_dependencies(),
                   "requires at least one component tag");
}

TEST(ComponentTypeCodecTest,
     RejectsDuplicateExactSourcesButAllowsCrossTargetReuse) {
  TypeSpec duplicate_group;
  duplicate_group.targets[0].duplicate_group_source = true;
  expect_rejection(duplicate_group, rich_dependencies(),
                   "same exact model source");

  TypeSpec duplicate_resource;
  duplicate_resource.targets[0].duplicate_resource_source = true;
  expect_rejection(duplicate_resource, rich_dependencies(),
                   "same exact model source");

  TypeSpec cross_target;
  auto second = cross_target.targets.front();
  second.target_id = "arm_secondary";
  cross_target.targets.push_back(second);
  EXPECT_NO_THROW(static_cast<void>(
      qualify(cross_target, rich_dependencies())));
}

TEST(ComponentTypeCodecTest,
     EnforcesTargetCapabilityAndExactDependencyClosure) {
  TypeSpec no_resources;
  no_resources.targets[0].include_model_resource = false;
  no_resources.targets[0].include_local_resource = false;
  expect_rejection(no_resources, rich_dependencies(),
                   "control-binding capability requires");

  TypeSpec global_capability;
  global_capability.targets[0].capabilities = {
      {"scene.reference.resolve", std::nullopt}};
  std::vector<QualifiedCapabilityDefinition> global_dependency;
  global_dependency.push_back(
      qualify_capability("scene.reference.resolve"));
  expect_rejection(global_capability, global_dependency,
                   "outside target_binding scope");

  auto missing = rich_dependencies();
  missing.pop_back();
  expect_rejection(TypeSpec{}, missing, "missing qualified capability");

  auto extra = rich_dependencies();
  extra.push_back(
      qualify_capability("planning.motion.joint_goal"));
  expect_rejection(TypeSpec{}, extra, "not the exact selected closure");

  auto duplicate = rich_dependencies();
  duplicate.push_back(
      qualify_capability("planning.motion.pose_goal"));
  expect_rejection(TypeSpec{}, duplicate, "duplicate exact referent");

  TypeSpec stale;
  stale.targets[0].capabilities[0].revision_override =
      std::string(kZeroDigest);
  auto stale_dependencies = rich_dependencies();
  expect_rejection(stale, stale_dependencies,
                   "stale capability-definition revision");
}

TEST(ComponentTypeCodecTest,
     RejectsTwoRevisionsOfOneCapabilityAcrossTargets) {
  TypeSpec spec;
  auto second = spec.targets.front();
  second.target_id = "arm_secondary";
  second.capabilities[0].revision_override =
      std::string(kZeroDigest);
  spec.targets.push_back(std::move(second));
  expect_rejection(spec, rich_dependencies(),
                   "more than one revision");
}

TEST(ComponentTypeCodecTest, ClosedResourceUnionRejectsBranchLeakage) {
  TypeSpec spec;
  auto local_document = component_document(spec);
  const std::string local_needle =
      R"({"resource_role_id":"vacuum","source_kind":"component_local"})";
  const std::string local_replacement =
      R"({"model_role_id":"base","resource_role_id":"vacuum","source_kind":"component_local"})";
  const auto local_offset = local_document.find(local_needle);
  ASSERT_NE(local_offset, std::string::npos);
  local_document.replace(local_offset, local_needle.size(),
                         local_replacement);
  const core::ContentAddressedStoreKey local_store_key{
      "sha256/bad-local-union"};
  EXPECT_THROW(
      resolve_and_qualify_component_type(
          {
              core::ComponentTypeId(spec.type_id),
              core::ComponentTypeRevision(
                  revision_from_document(local_document)),
              core::SchemaId(
                  std::string(core::kComponentTypeApiVersion)),
              core::SchemaRevision(
                  std::string(kComponentTypeSchemaRevision)),
              core::ContentDigest(sha256_digest(local_document)),
              local_store_key,
          },
          local_store_key, local_document, rich_dependencies()),
      ClosedSchemaError);

  auto model_document = component_document(spec);
  const std::string model_needle =
      R"({"asset_resource_role_id":"arm_joints","model_role_id":"base","resource_role_id":"arm_joints","source_kind":"model_role"})";
  const std::string model_replacement =
      R"({"resource_role_id":"arm_joints","source_kind":"model_role"})";
  const auto model_offset = model_document.find(model_needle);
  ASSERT_NE(model_offset, std::string::npos);
  model_document.replace(model_offset, model_needle.size(),
                         model_replacement);
  const core::ContentAddressedStoreKey model_store_key{
      "sha256/bad-model-union"};
  EXPECT_THROW(
      resolve_and_qualify_component_type(
          {
              core::ComponentTypeId(spec.type_id),
              core::ComponentTypeRevision(
                  revision_from_document(model_document)),
              core::SchemaId(
                  std::string(core::kComponentTypeApiVersion)),
              core::SchemaRevision(
                  std::string(kComponentTypeSchemaRevision)),
              core::ContentDigest(sha256_digest(model_document)),
              model_store_key,
          },
          model_store_key, model_document, rich_dependencies()),
      ClosedSchemaError);
}

TEST(ComponentTypeCodecTest, RejectsEveryOuterReferentMismatch) {
  const TypeSpec spec;
  const auto document = component_document(spec);
  const core::ContentAddressedStoreKey store_key{
      "sha256/component-referent"};
  const auto reference =
      component_reference(spec.type_id, document, store_key);
  const auto dependencies = rich_dependencies();

  EXPECT_NO_THROW(resolve_and_qualify_component_type(
      reference, store_key, document, dependencies));

  EXPECT_THROW(resolve_and_qualify_component_type(
                   reference,
                   core::ContentAddressedStoreKey("sha256/wrong-key"),
                   document, dependencies),
               ComponentTypeCodecError);

  auto wrong_digest = reference;
  wrong_digest.content_digest =
      core::ContentDigest(std::string(kZeroDigest));
  EXPECT_THROW(resolve_and_qualify_component_type(
                   wrong_digest, store_key, document, dependencies),
               ComponentTypeCodecError);

  auto wrong_id = reference;
  wrong_id.record_id = core::ComponentTypeId("other_type");
  EXPECT_THROW(resolve_and_qualify_component_type(
                   wrong_id, store_key, document, dependencies),
               ComponentTypeCodecError);

  auto wrong_revision = reference;
  wrong_revision.record_revision =
      core::ComponentTypeRevision(std::string(kZeroDigest));
  EXPECT_THROW(resolve_and_qualify_component_type(
                   wrong_revision, store_key, document, dependencies),
               ComponentTypeCodecError);

  auto wrong_schema_id = reference;
  wrong_schema_id.schema_id =
      core::SchemaId("manyforge.model_asset.v1");
  EXPECT_THROW(resolve_and_qualify_component_type(
                   wrong_schema_id, store_key, document, dependencies),
               ComponentTypeCodecError);

  auto wrong_schema_revision = reference;
  wrong_schema_revision.schema_revision =
      core::SchemaRevision(std::string(kZeroDigest));
  EXPECT_THROW(resolve_and_qualify_component_type(
                   wrong_schema_revision, store_key, document,
                   dependencies),
               ComponentTypeCodecError);

  auto changed_document = document;
  const auto tag_offset = changed_document.find("robot-tool");
  ASSERT_NE(tag_offset, std::string::npos);
  changed_document.replace(tag_offset, std::string_view("robot-tool").size(),
                           "robot-tool-updated");
  auto changed_reference = reference;
  changed_reference.content_digest =
      core::ContentDigest(sha256_digest(changed_document));
  EXPECT_THROW(resolve_and_qualify_component_type(
                   changed_reference, store_key, changed_document,
                   dependencies),
               ComponentTypeCodecError);
}

TEST(ComponentTypeCodecTest, RejectsUnknownRootFields) {
  const TypeSpec spec;
  auto document = component_document(spec);
  const std::string needle = "\"tags\":";
  const auto offset = document.find(needle);
  ASSERT_NE(offset, std::string::npos);
  document.insert(offset, "\"runtime_state\":{},");
  const core::ContentAddressedStoreKey store_key{
      "sha256/component-open-root"};
  EXPECT_THROW(resolve_and_qualify_component_type(
                   component_reference(spec.type_id, document, store_key),
                   store_key, document, rich_dependencies()),
               ClosedSchemaError);
}

TEST(ComponentTypeCodecTest,
     RejectsMissingModelRoleReferencesAndInstanceBounds) {
  TypeSpec missing_model_role;
  missing_model_role.targets[0].source_model_role = "missing";
  expect_rejection(missing_model_role, rich_dependencies(),
                   "references a missing model role");

  TypeSpec dangling_interface;
  dangling_interface.interface_model_role = "missing";
  expect_rejection(dangling_interface, rich_dependencies(),
                   "references a missing model role");

  TypeSpec invalid_pose;
  invalid_pose.invalid_mount_pose = true;
  expect_rejection(invalid_pose, rich_dependencies(),
                   "quaternion norm squared");

  TypeSpec too_many_tags;
  too_many_tags.tags.clear();
  for (std::size_t index = 0U;
       index <= core::kMaxComponentTypeTags; ++index) {
    too_many_tags.tags.push_back("tag_" + std::to_string(index));
  }
  expect_closed_schema_rejection(too_many_tags, rich_dependencies());
}

}  // namespace
}  // namespace manyforge::deployment
