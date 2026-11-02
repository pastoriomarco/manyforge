#include <gtest/gtest.h>

#include <algorithm>
#include <manyforge_core/types/runtime_profile.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment {
namespace {

constexpr std::string_view kRuntimeProfileSchemaRevision =
    "sha256:c254afa8c2bfcc803f12a7af63b1f09a8ee804b1ffa2a65b7361ad9309b87ae7";
constexpr std::string_view kEmptyConfigurationSchemaRevision =
    "sha256:03457d535d31818fedaa7e44f48432a08feb705e4d22c9a8f5e50977c8dbf785";

static_assert(!std::is_aggregate_v<QualifiedRuntimeProfile>);
static_assert(!std::is_default_constructible_v<QualifiedRuntimeProfile>);
static_assert(core::kMaxRuntimeProfileRoleIds == 256U);

struct ProfileSpec final {
  std::string profile_id{"ros2_control_xarm"};
  std::string lifecycle_control{"manager_managed"};
  std::string configuration_schema_id{
      "manyforge.value_schema.runtime.empty.v1"};
  std::string configuration_schema_revision{
      std::string(kEmptyConfigurationSchemaRevision)};
  std::string status_endpoint_role{"runtime_status"};
  std::vector<std::string> runtime_roles{"driver", "state_source"};
  std::vector<std::string> endpoint_roles{"joint_state", "runtime_status"};
};

[[nodiscard]] CanonicalValue string_set(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (auto& value : values) {
    result.emplace_back(std::move(value));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue profile_identity_value(ProfileSpec spec) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kRuntimeProfileApiVersion))},
      {"runtime_profile_id", CanonicalValue(std::move(spec.profile_id))},
      {"configuration_schema_ref",
       CanonicalValue(CanonicalValue::Object{
           {"schema_id",
            CanonicalValue(std::move(spec.configuration_schema_id))},
           {"schema_revision",
            CanonicalValue(std::move(spec.configuration_schema_revision))},
       })},
      {"process_contract",
       CanonicalValue(CanonicalValue::Object{
           {"lifecycle_control",
            CanonicalValue(std::move(spec.lifecycle_control))},
       })},
      {"supervision_contract",
       CanonicalValue(CanonicalValue::Object{
           {"status_endpoint_role_id",
            CanonicalValue(std::move(spec.status_endpoint_role))},
       })},
      {"offered_runtime_role_ids",
       string_set(std::move(spec.runtime_roles))},
      {"source_endpoint_role_ids",
       string_set(std::move(spec.endpoint_roles))},
  });
}

[[nodiscard]] std::string document_from_identity(CanonicalValue identity) {
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/runtime_profile");
  preimage.push_back('\0');
  preimage.append(core::kRuntimeProfileApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(identity));
  auto root = identity.as_object();
  root.emplace_back("runtime_profile_revision",
                    CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(root)));
}

[[nodiscard]] std::string profile_document(const ProfileSpec& spec) {
  return document_from_identity(profile_identity_value(spec));
}

[[nodiscard]] std::string revision_from_document(
    std::string_view document) {
  const auto parsed = parse_strict_json_model_yaml(document);
  const auto* revision =
      parsed.root().find_object_member("runtime_profile_revision");
  EXPECT_NE(revision, nullptr);
  if (revision == nullptr) {
    return {};
  }
  const auto* text = std::get_if<std::string>(&revision->storage());
  EXPECT_NE(text, nullptr);
  return text == nullptr ? std::string{} : *text;
}

[[nodiscard]] core::RuntimeProfileCatalogRecordRefDTO reference_for(
    std::string_view profile_id, std::string_view document,
    const core::ContentAddressedStoreKey& store_key) {
  return {
      core::RuntimeProfileId(std::string(profile_id)),
      core::RuntimeProfileRevision(revision_from_document(document)),
      core::SchemaId(std::string(core::kRuntimeProfileApiVersion)),
      core::SchemaRevision(std::string(kRuntimeProfileSchemaRevision)),
      core::ContentDigest(sha256_digest(document)),
      store_key,
  };
}

[[nodiscard]] QualifiedRuntimeProfile qualify(
    const ProfileSpec& spec,
    const core::ContentAddressedStoreKey& store_key =
        core::ContentAddressedStoreKey("sha256/runtime-profile")) {
  const auto document = profile_document(spec);
  return resolve_and_qualify_runtime_profile(
      reference_for(spec.profile_id, document, store_key), store_key,
      document);
}

void expect_schema_rejection(CanonicalValue identity) {
  const auto document = document_from_identity(std::move(identity));
  const core::ContentAddressedStoreKey store_key{
      "sha256/runtime-profile-shape-negative"};
  EXPECT_THROW(resolve_and_qualify_runtime_profile(
                   reference_for("ros2_control_xarm", document, store_key),
                   store_key, document),
               ClosedSchemaError);
}

TEST(RuntimeProfileCodec, OldSealedRosSchemaRefusesWithRecompileGuidance) {
  ProfileSpec old;
  old.profile_id = "manyforge.ros2_control_execution_runtime";
  old.configuration_schema_id = "manyforge.value_schema.runtime.ros2_control_execution.v1";
  old.configuration_schema_revision =
      "sha256:f927705d80b40e266f3c62341911a223de2a3811188a2c4656d4bee98f81c30b";
  old.status_endpoint_role = "ros2_control_execution_status";
  old.runtime_roles = {"ros2_control_execution"};
  old.endpoint_roles = {"logical_scene_services", "planner_scene_projection",
                        "ros2_control_execution_status"};
  EXPECT_NE(profile_document(old).find("sha256:0f916fb469d27a221a97cf72737e006c4c4fc566752ea477a141847a0012fef5"),
            std::string::npos);
  try {
    static_cast<void>(qualify(old));
    FAIL() << "old sealed ROS schema must not enter the current registry";
  } catch (const RuntimeProfileCodecError &error) {
    EXPECT_NE(std::string(error.what()).find("recompile"), std::string::npos);
  }
}

TEST(RuntimeProfileCodec,
     QualifiesManagerManagedProfileAndCanonicalizesBothRoleSets) {
  ProfileSpec spec;
  spec.runtime_roles = {"state_source", "driver"};
  spec.endpoint_roles = {"runtime_status", "joint_state"};
  const auto document = profile_document(spec);
  const auto qualified = qualify(spec);
  const auto& profile = qualified.value();
  EXPECT_EQ(
      qualified.reference(),
      reference_for(spec.profile_id, document,
                    core::ContentAddressedStoreKey("sha256/runtime-profile")));

  EXPECT_EQ(profile.runtime_profile_id,
            core::RuntimeProfileId("ros2_control_xarm"));
  EXPECT_EQ(profile.process_contract.lifecycle_control,
            core::RuntimeLifecycleControl::kManagerManaged);
  EXPECT_EQ(
      profile.configuration_schema_ref.schema_id,
      core::SchemaId("manyforge.value_schema.runtime.empty.v1"));
  EXPECT_EQ(
      profile.offered_runtime_role_ids,
      (std::vector<core::RuntimeRoleId>{
          core::RuntimeRoleId("driver"),
          core::RuntimeRoleId("state_source")}));
  EXPECT_EQ(
      profile.source_endpoint_role_ids,
      (std::vector<core::EndpointRoleId>{
          core::EndpointRoleId("joint_state"),
          core::EndpointRoleId("runtime_status")}));
  EXPECT_EQ(profile.supervision_contract.status_endpoint_role_id,
            core::EndpointRoleId("runtime_status"));
  EXPECT_EQ(canonical_json(qualified.normalized_root()), document);
}

TEST(RuntimeProfileCodec, QualifiesExternallyManagedProfile) {
  ProfileSpec spec;
  spec.lifecycle_control = "externally_managed";
  const auto qualified = qualify(spec);
  EXPECT_EQ(qualified.value().process_contract.lifecycle_control,
            core::RuntimeLifecycleControl::kExternallyManaged);
}

TEST(RuntimeProfileCodec,
     RequiresAnExactBuildQualifiedConfigurationSchema) {
  const auto& registry = build_qualified_closed_schema_registry();
  const auto* empty = registry.find_qualified_value_schema(
      core::SchemaId("manyforge.value_schema.runtime.empty.v1"),
      core::SchemaRevision(std::string(kEmptyConfigurationSchemaRevision)));
  ASSERT_NE(empty, nullptr);
  EXPECT_EQ(empty->compiled_schema().schema_purpose(),
            ClosedSchemaPurpose::kValue);
  EXPECT_EQ(empty->compiled_schema().root_shape(),
            ClosedSchemaRootShape::kObject);
  const auto normalized = validate_and_normalize_unqualified_artifact_candidate(
      empty->compiled_schema(), parse_strict_json_model_yaml("{}"));
  EXPECT_EQ(canonical_json(normalized.normalized_root()), "{}");
  EXPECT_THROW(validate_and_normalize_unqualified_artifact_candidate(
                   empty->compiled_schema(),
                   parse_strict_json_model_yaml("{unexpected: true}")),
               ClosedSchemaError);

  ProfileSpec unknown;
  unknown.configuration_schema_id =
      "manyforge.value_schema.runtime.unknown.v1";
  const auto document = profile_document(unknown);
  const core::ContentAddressedStoreKey store_key{
      "sha256/runtime-profile-unqualified-value-schema"};
  EXPECT_THROW(resolve_and_qualify_runtime_profile(
                   reference_for(unknown.profile_id, document, store_key),
                   store_key, document),
               RuntimeProfileCodecError);

  ProfileSpec stale_revision;
  stale_revision.configuration_schema_revision =
      "sha256:0000000000000000000000000000000000000000000000000000000000000000";
  const auto stale_document = profile_document(stale_revision);
  const core::ContentAddressedStoreKey stale_store_key{
      "sha256/runtime-profile-stale-value-schema"};
  EXPECT_THROW(resolve_and_qualify_runtime_profile(
                   reference_for(stale_revision.profile_id, stale_document,
                                 stale_store_key),
                   stale_store_key, stale_document),
               RuntimeProfileCodecError);
}

TEST(RuntimeProfileCodec,
     RequiresStatusRoleMembershipAndClosedLifecycleControl) {
  ProfileSpec missing_status;
  missing_status.endpoint_roles = {"joint_state"};
  const auto document = profile_document(missing_status);
  const core::ContentAddressedStoreKey store_key{
      "sha256/runtime-profile-status-membership"};
  EXPECT_THROW(resolve_and_qualify_runtime_profile(
                   reference_for(missing_status.profile_id, document,
                                 store_key),
                   store_key, document),
               RuntimeProfileCodecError);

  auto invalid_lifecycle =
      profile_identity_value(ProfileSpec{});
  auto object = invalid_lifecycle.as_object();
  auto process = std::find_if(
      object.begin(), object.end(),
      [](const auto& item) { return item.first == "process_contract"; });
  ASSERT_NE(process, object.end());
  auto process_object = process->second.as_object();
  process_object.front().second = CanonicalValue("self_managed");
  process->second = CanonicalValue(std::move(process_object));
  expect_schema_rejection(CanonicalValue(std::move(object)));
}

TEST(RuntimeProfileCodec, EnforcesRoleBoundsAndUniquenessAtIngress) {
  ProfileSpec exact;
  exact.runtime_roles.clear();
  exact.endpoint_roles.clear();
  for (std::size_t index = 0U;
       index < core::kMaxRuntimeProfileRoleIds; ++index) {
    exact.runtime_roles.push_back("runtime_role_" + std::to_string(index));
    exact.endpoint_roles.push_back("endpoint_role_" +
                                   std::to_string(index));
  }
  exact.status_endpoint_role = exact.endpoint_roles.back();
  EXPECT_NO_THROW(qualify(exact));

  ProfileSpec too_many = exact;
  too_many.runtime_roles.push_back("runtime_role_overflow");
  expect_schema_rejection(profile_identity_value(std::move(too_many)));

  ProfileSpec too_many_endpoints = exact;
  too_many_endpoints.endpoint_roles.push_back("endpoint_role_overflow");
  expect_schema_rejection(
      profile_identity_value(std::move(too_many_endpoints)));

  ProfileSpec empty_runtime_roles;
  empty_runtime_roles.runtime_roles.clear();
  expect_schema_rejection(
      profile_identity_value(std::move(empty_runtime_roles)));

  ProfileSpec empty_endpoint_roles;
  empty_endpoint_roles.endpoint_roles.clear();
  expect_schema_rejection(
      profile_identity_value(std::move(empty_endpoint_roles)));

  auto duplicate_runtime = profile_identity_value(ProfileSpec{});
  auto root = duplicate_runtime.as_object();
  auto offered = std::find_if(
      root.begin(), root.end(),
      [](const auto& item) {
        return item.first == "offered_runtime_role_ids";
      });
  ASSERT_NE(offered, root.end());
  offered->second = CanonicalValue(CanonicalValue::Array{
      CanonicalValue("driver"), CanonicalValue("driver")});
  expect_schema_rejection(CanonicalValue(std::move(root)));

  auto duplicate_endpoint = profile_identity_value(ProfileSpec{});
  auto endpoint_root = duplicate_endpoint.as_object();
  auto endpoints = std::find_if(
      endpoint_root.begin(), endpoint_root.end(),
      [](const auto& item) {
        return item.first == "source_endpoint_role_ids";
      });
  ASSERT_NE(endpoints, endpoint_root.end());
  endpoints->second = CanonicalValue(CanonicalValue::Array{
      CanonicalValue("runtime_status"),
      CanonicalValue("runtime_status")});
  expect_schema_rejection(CanonicalValue(std::move(endpoint_root)));
}

TEST(RuntimeProfileCodec, RejectsDeletedLegacyAndOperationalFields) {
  const std::vector<std::string> forbidden_fields{
      "launcher",
      "launcher_type",
      "process_adapter_type",
      "adapter",
      "adapter_type",
      "image",
      "image_digest",
      "executable",
      "argv",
      "command",
      "path",
      "package",
      "package_name",
      "provider",
      "provider_instance_id",
      "provider_manifest",
      "provider_manifest_ref",
      "dependencies",
      "dependency_roles",
      "dependency_role_ids",
      "required_resources",
      "resource_requirements",
      "health_contract",
      "progress_contract",
      "startup_contract",
      "shutdown_contract",
      "restart_policy",
      "restart_backoff_ms",
      "backoff_policy",
      "freshness_policy",
      "heartbeat_period_ms",
      "stale_after_ms",
      "timeout_ms",
      "timeouts",
      "lane",
      "container",
      "container_name",
      "namespace",
      "prefix",
      "generated_endpoint_names",
      "desired_state",
  };
  for (const auto& field : forbidden_fields) {
    auto identity = profile_identity_value(ProfileSpec{});
    auto root = identity.as_object();
    root.emplace_back(field, CanonicalValue("forbidden"));
    expect_schema_rejection(CanonicalValue(std::move(root)));
  }

  auto nested_process = profile_identity_value(ProfileSpec{}).as_object();
  auto process = std::find_if(
      nested_process.begin(), nested_process.end(),
      [](const auto& item) { return item.first == "process_contract"; });
  ASSERT_NE(process, nested_process.end());
  auto process_fields = process->second.as_object();
  process_fields.emplace_back("restart_policy", CanonicalValue("always"));
  process->second = CanonicalValue(std::move(process_fields));
  expect_schema_rejection(CanonicalValue(std::move(nested_process)));

  auto nested_supervision =
      profile_identity_value(ProfileSpec{}).as_object();
  auto supervision = std::find_if(
      nested_supervision.begin(), nested_supervision.end(),
      [](const auto& item) {
        return item.first == "supervision_contract";
      });
  ASSERT_NE(supervision, nested_supervision.end());
  auto supervision_fields = supervision->second.as_object();
  supervision_fields.emplace_back("stale_after_ms",
                                  CanonicalValue(1000.0));
  supervision->second = CanonicalValue(std::move(supervision_fields));
  expect_schema_rejection(CanonicalValue(std::move(nested_supervision)));
}

TEST(RuntimeProfileCodec, RejectsEveryOuterReferentMismatch) {
  const ProfileSpec spec;
  const auto document = profile_document(spec);
  const core::ContentAddressedStoreKey store_key{
      "sha256/runtime-profile-referent"};
  const auto reference =
      reference_for(spec.profile_id, document, store_key);

  EXPECT_THROW(resolve_and_qualify_runtime_profile(
                   reference, core::ContentAddressedStoreKey("sha256/other"),
                   document),
               RuntimeProfileCodecError);

  auto wrong_digest = reference;
  wrong_digest.content_digest = core::ContentDigest(
      "sha256:0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_runtime_profile(
                   wrong_digest, store_key, document),
               RuntimeProfileCodecError);

  auto wrong_id = reference;
  wrong_id.record_id = core::RuntimeProfileId("other_profile");
  EXPECT_THROW(resolve_and_qualify_runtime_profile(
                   wrong_id, store_key, document),
               RuntimeProfileCodecError);

  auto wrong_revision = reference;
  wrong_revision.record_revision = core::RuntimeProfileRevision(
      "sha256:0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_runtime_profile(
                   wrong_revision, store_key, document),
               RuntimeProfileCodecError);

  auto wrong_schema_id = reference;
  wrong_schema_id.schema_id =
      core::SchemaId("manyforge.component_type.v1");
  EXPECT_THROW(resolve_and_qualify_runtime_profile(
                   wrong_schema_id, store_key, document),
               RuntimeProfileCodecError);

  auto wrong_schema = reference;
  wrong_schema.schema_revision = core::SchemaRevision(
      "sha256:0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_runtime_profile(
                   wrong_schema, store_key, document),
               RuntimeProfileCodecError);

  auto stale_revision_document = document;
  const auto lifecycle_offset =
      stale_revision_document.find("manager_managed");
  ASSERT_NE(lifecycle_offset, std::string::npos);
  stale_revision_document.replace(lifecycle_offset,
                                  std::string_view("manager_managed").size(),
                                  "externally_managed");
  EXPECT_THROW(resolve_and_qualify_runtime_profile(
                   reference_for(spec.profile_id, stale_revision_document,
                                 store_key),
                   store_key, stale_revision_document),
               RuntimeProfileCodecError);
}

}  // namespace
}  // namespace manyforge::deployment
