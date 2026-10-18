#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <manyforge_core/types/command_schema.hpp>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/command_schema_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment {
namespace {

static_assert(!std::is_aggregate_v<QualifiedCommandSchema>);
static_assert(!std::is_default_constructible_v<QualifiedCommandSchema>);

inline constexpr std::string_view kCommandSchemaClosedSchemaRevision =
    "sha256:fdce9bb1facd259bde85f502197fd534dc05346a030605a55aa951d9e88c73dd";

[[nodiscard]] std::string read_command_schema() {
  const auto path = std::filesystem::path(MANYFORGE_DEPLOYMENT_RESOURCE_DIR) /
                    "catalog" / "gripper.command_position.v1.yaml";
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("unable to open command-schema resource: " +
                             path.string());
  }
  std::ostringstream bytes;
  bytes << input.rdbuf();
  return bytes.str();
}

[[nodiscard]] const CanonicalValue &require_member(const CanonicalValue &value,
                                                   std::string_view name) {
  const auto *member = value.find_object_member(name);
  if (member == nullptr) {
    throw std::runtime_error("missing command-schema test member: " +
                             std::string(name));
  }
  return *member;
}

[[nodiscard]] std::string require_string_member(const CanonicalValue &value,
                                                std::string_view name) {
  const auto *text =
      std::get_if<std::string>(&require_member(value, name).storage());
  if (text == nullptr) {
    throw std::runtime_error("command-schema test member is not a string: " +
                             std::string(name));
  }
  return *text;
}

[[nodiscard]] core::CommandSchemaCatalogRecordRefDTO
command_schema_reference(std::string_view stored_bytes,
                         const core::ContentAddressedStoreKey &store_key,
                         std::string_view record_revision =
                             core::kGripperPositionCommandSchemaRevision) {
  return {
      core::CommandSchemaId(std::string(core::kGripperPositionCommandSchemaId)),
      core::CommandSchemaRevision(std::string(record_revision)),
      core::SchemaId(std::string(core::kCommandSchemaApiVersion)),
      core::SchemaRevision(std::string(kCommandSchemaClosedSchemaRevision)),
      core::ContentDigest(sha256_digest(stored_bytes)),
      store_key,
  };
}

[[nodiscard]] std::string
reidentify_command_schema(std::string_view candidate_bytes) {
  const auto parsed = parse_strict_json_model_yaml(candidate_bytes);
  CanonicalValue::Object semantic_members;
  for (const auto &[name, value] : parsed.root().as_object()) {
    if (name != "command_schema_revision") {
      semantic_members.emplace_back(name, value);
    }
  }
  const CanonicalValue semantic_value(std::move(semantic_members));
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/command_schema");
  preimage.push_back('\0');
  preimage.append(core::kCommandSchemaApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(semantic_value));

  auto identified = semantic_value.as_object();
  identified.emplace_back("command_schema_revision",
                          CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(identified)));
}

TEST(CommandSchemaCodec, QualifiesExactBuiltinGripperPositionFamily) {
  const auto stored_bytes = read_command_schema();
  const core::ContentAddressedStoreKey store_key{
      "sha256/gripper-command-position-v1"};
  const auto reference = command_schema_reference(stored_bytes, store_key);

  const auto qualified =
      resolve_and_qualify_command_schema(reference, store_key, stored_bytes);
  EXPECT_EQ(qualified.reference(), reference);
  EXPECT_EQ(qualified.value(), core::builtin_gripper_position_command_schema());
  EXPECT_EQ(require_string_member(qualified.normalized_root(),
                                  "command_schema_revision"),
            core::kGripperPositionCommandSchemaRevision);

  ASSERT_EQ(qualified.value().compatible_capabilities.size(), 1U);
  EXPECT_EQ(qualified.value().compatible_capabilities.front().capability_id,
            core::CapabilityId("tool.gripper.command"));
  const auto *command_value =
      std::get_if<core::GripperPositionCommandValueContractDTO>(
          &qualified.value().command_value);
  ASSERT_NE(command_value, nullptr);
  EXPECT_EQ(command_value->accepted_position_kinds,
            (std::vector<core::CoordinateKind>{core::CoordinateKind::kAngular,
                                               core::CoordinateKind::kLinear}));
}

TEST(CommandSchemaCodec, BindsEveryCatalogReferenceFieldAndStoredBytes) {
  const auto stored_bytes = read_command_schema();
  const core::ContentAddressedStoreKey store_key{
      "sha256/gripper-command-position-negative"};
  const auto reference = command_schema_reference(stored_bytes, store_key);

  auto wrong_id = reference;
  wrong_id.record_id = core::CommandSchemaId("unknown.command");
  EXPECT_THROW(
      resolve_and_qualify_command_schema(wrong_id, store_key, stored_bytes),
      CommandSchemaCodecError);

  auto wrong_revision = reference;
  wrong_revision.record_revision = core::CommandSchemaRevision(
      "sha256:"
      "0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_command_schema(wrong_revision, store_key,
                                                  stored_bytes),
               CommandSchemaCodecError);

  auto wrong_schema = reference;
  wrong_schema.schema_id = core::SchemaId("manyforge.contact_policy.v1");
  EXPECT_THROW(
      resolve_and_qualify_command_schema(wrong_schema, store_key, stored_bytes),
      CommandSchemaCodecError);

  auto wrong_schema_revision = reference;
  wrong_schema_revision.schema_revision = core::SchemaRevision(
      "sha256:"
      "0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_command_schema(wrong_schema_revision,
                                                  store_key, stored_bytes),
               CommandSchemaCodecError);

  auto wrong_digest = reference;
  wrong_digest.content_digest = core::ContentDigest(
      "sha256:"
      "0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(
      resolve_and_qualify_command_schema(wrong_digest, store_key, stored_bytes),
      CommandSchemaCodecError);

  EXPECT_THROW(resolve_and_qualify_command_schema(
                   reference,
                   core::ContentAddressedStoreKey("sha256/different-key"),
                   stored_bytes),
               CommandSchemaCodecError);
}

TEST(CommandSchemaCodec, RejectsOpenStaleAndNonBuiltinSemanticContent) {
  const auto original = read_command_schema();
  const core::ContentAddressedStoreKey store_key{
      "sha256/gripper-command-position-semantic-negative"};

  auto open = original;
  open.append("unexpected: true\n");
  const auto open_reference = command_schema_reference(open, store_key);
  EXPECT_THROW(
      resolve_and_qualify_command_schema(open_reference, store_key, open),
      ClosedSchemaError);

  auto stale = original;
  const auto maximum = stale.find("maximum_resource_count: 4096");
  ASSERT_NE(maximum, std::string::npos);
  stale.replace(maximum, std::string("maximum_resource_count: 4096").size(),
                "maximum_resource_count: 4095");
  const auto stale_reference = command_schema_reference(stale, store_key);
  EXPECT_THROW(
      resolve_and_qualify_command_schema(stale_reference, store_key, stale),
      CommandSchemaCodecError);

  const auto reidentified = reidentify_command_schema(stale);
  const auto parsed = parse_strict_json_model_yaml(reidentified);
  const auto semantic_revision =
      require_string_member(parsed.root(), "command_schema_revision");
  const auto reidentified_reference =
      command_schema_reference(reidentified, store_key, semantic_revision);
  EXPECT_THROW(resolve_and_qualify_command_schema(reidentified_reference,
                                                  store_key, reidentified),
               CommandSchemaCodecError);
}

[[nodiscard]] std::string read_output_command_schema() {
  std::ifstream input(std::filesystem::path(MANYFORGE_DEPLOYMENT_RESOURCE_DIR) /
                          "catalog" / "io.signal_write_boolean.v1.yaml",
                      std::ios::binary);
  if (!input)
    throw std::runtime_error("unable to open output command schema");
  std::ostringstream bytes;
  bytes << input.rdbuf();
  return bytes.str();
}

[[nodiscard]] core::CommandSchemaCatalogRecordRefDTO
output_schema_reference(std::string_view bytes,
                        const core::ContentAddressedStoreKey &key) {
  return {
      core::CommandSchemaId(std::string(core::kIoSignalWriteCommandSchemaId)),
      core::CommandSchemaRevision(
          std::string(core::kIoSignalWriteCommandSchemaRevision)),
      core::SchemaId(std::string(core::kCommandSchemaApiVersion)),
      core::SchemaRevision(std::string(kCommandSchemaClosedSchemaRevision)),
      core::ContentDigest(sha256_digest(bytes)),
      key};
}

TEST(CommandSchemaCodec,
     QualifiesBooleanOutputWithoutInventedFeedbackOrStopPolicy) {
  const auto bytes = read_output_command_schema();
  const core::ContentAddressedStoreKey key("sha256/io-output-schema");
  const auto qualified = resolve_and_qualify_command_schema(
      output_schema_reference(bytes, key), key, bytes);
  EXPECT_EQ(qualified.value(), core::builtin_io_signal_write_command_schema());
  EXPECT_TRUE(
      std::holds_alternative<core::IoSignalWriteCommandValueContractDTO>(
          qualified.value().command_value));
  EXPECT_FALSE(qualified.value().completion_rule);
  EXPECT_FALSE(qualified.value().confirmation_rule);
  EXPECT_TRUE(qualified.value().stop_evidence_requirements.empty());
}

TEST(
    CommandSchemaCodec,
    BooleanBranchRejectsAuthoredCompletionAndGripperBranchKeepsRequiredFields) {
  const core::ContentAddressedStoreKey key("sha256/io-output-closed-negative");
  auto output = read_output_command_schema();
  output.append("completion_rule: goal_fenced_target_reached\n");
  EXPECT_THROW(resolve_and_qualify_command_schema(
                   output_schema_reference(output, key), key, output),
               ClosedSchemaError);
  const auto original = read_command_schema();
  for (const std::string field :
       {"completion_rule: goal_fenced_target_reached\n",
        "confirmation_rule: binding_feedback_required\n",
        "stop_evidence_requirements: [command_terminal]\n"}) {
    auto missing = original;
    const auto position = missing.find(field);
    ASSERT_NE(position, std::string::npos);
    missing.erase(position, field.size());
    EXPECT_THROW(resolve_and_qualify_command_schema(
                     command_schema_reference(missing, key), key, missing),
                 ClosedSchemaError);
  }
}

TEST(CommandSchemaCodec, BooleanBranchRejectsCrossFamilyAndStaleValue) {
  const core::ContentAddressedStoreKey key("sha256/io-output-cross-negative");
  auto bytes = read_output_command_schema();
  const auto position = bytes.find("value_kind: io_signal_write");
  ASSERT_NE(position, std::string::npos);
  bytes.replace(position, std::string("value_kind: io_signal_write").size(),
                "value_kind: gripper_position");
  EXPECT_THROW(resolve_and_qualify_command_schema(
                   output_schema_reference(bytes, key), key, bytes),
               ClosedSchemaError);
  auto stale = read_output_command_schema();
  const auto id = stale.find("command_schema_id: io.signal_write_boolean");
  ASSERT_NE(id, std::string::npos);
  stale.replace(
      id, std::string("command_schema_id: io.signal_write_boolean").size(),
      "command_schema_id: io.other_boolean");
  EXPECT_THROW(resolve_and_qualify_command_schema(
                   output_schema_reference(stale, key), key, stale),
               CommandSchemaCodecError);
}

} // namespace
} // namespace manyforge::deployment
