#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <manyforge_core/types/capability_definition.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "closed_schema_internal.hpp"
#include "gripper_only_retention_fixture.hpp"
#include "lite6_controllable_retention_fixture.hpp"
#include "execution_host_unit_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "manyforge_deployment/deployment_authoring.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"
#include "manyforge_deployment/capability_definition_codec.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "proof_root_transfer_test.hpp"

namespace manyforge::deployment {
namespace {

static_assert(!std::is_aggregate_v<CompiledClosedSchema>);
static_assert(!std::is_default_constructible_v<CompiledClosedSchema>);
static_assert(
    !std::is_aggregate_v<UnqualifiedSchemaNormalizedArtifactCandidate>);
static_assert(!std::is_default_constructible_v<
              UnqualifiedSchemaNormalizedArtifactCandidate>);
static_assert(!std::is_aggregate_v<NonGraphQualifiedSchemaNormalizedValue>);
static_assert(
    !std::is_default_constructible_v<NonGraphQualifiedSchemaNormalizedValue>);
static_assert(!std::is_aggregate_v<BuildCatalogSchemaEntry>);
static_assert(!std::is_aggregate_v<BuildQualifiedValueSchemaEntry>);
static_assert(!std::is_aggregate_v<QualifiedCapabilityDefinition>);
static_assert(!std::is_default_constructible_v<QualifiedCapabilityDefinition>);
static_assert(kMaxClosedSchemaEnumValues == 1024U);
static_assert(kMaxClosedSchemaKeyPaths == 16U);
static_assert(kMaxClosedSchemaKeyPathSegments == 16U);

[[nodiscard]] std::string read_resource(std::string_view relative_path) {
  const auto path =
      std::filesystem::path(MANYFORGE_CLOSED_SCHEMA_RESOURCE_DIR) /
      relative_path;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("unable to open test resource: " + path.string());
  }
  std::ostringstream bytes;
  bytes << input.rdbuf();
  return bytes.str();
}

[[nodiscard]] CompiledClosedSchema
compile_resource(std::string_view relative_path) {
  return compile_closed_schema_descriptor(
      parse_strict_json_model_yaml(read_resource(relative_path)));
}

void expect_compile_error(std::string yaml, std::string_view expected_message) {
  try {
    (void)compile_closed_schema_descriptor(parse_strict_json_model_yaml(yaml));
    FAIL() << "expected closed-schema compilation to fail";
  } catch (const ClosedSchemaError &error) {
    EXPECT_NE(std::string(error.what()).find(expected_message),
              std::string::npos)
        << error.what();
  }
}

[[nodiscard]] const CanonicalValue &member(const CanonicalValue &value,
                                           std::string_view name) {
  const auto *found = value.find_object_member(name);
  if (found == nullptr) {
    throw std::runtime_error("missing test projection member: " +
                             std::string(name));
  }
  return *found;
}

[[nodiscard]] CanonicalValue value_object(
    std::initializer_list<std::pair<std::string, CanonicalValue>> members) {
  return CanonicalValue(CanonicalValue::Object(members));
}

[[nodiscard]] std::string capability_definition_document(
    std::string_view capability_id, core::CapabilityScope scope,
    core::CapabilityProviderBindingKind provider_binding_kind,
    std::optional<core::ValueSchemaRefDescriptor> schema_ref = std::nullopt) {
  CanonicalValue::Object identity_fields{
      {"api_version",
       CanonicalValue(std::string(core::kCapabilityDefinitionApiVersion))},
      {"capability_id", CanonicalValue(std::string(capability_id))},
      {"scope", CanonicalValue(std::string(core::to_string(scope)))},
      {"provider_binding_kind",
       CanonicalValue(std::string(core::to_string(provider_binding_kind)))},
  };
  if (schema_ref.has_value()) {
    identity_fields.emplace_back(
        "requirement_parameters_schema_ref",
        value_object(
            {{"schema_id", CanonicalValue(std::string(schema_ref->schema_id))},
             {"schema_revision",
              CanonicalValue(std::string(schema_ref->schema_revision))}}));
  }
  const auto identity_value = CanonicalValue(std::move(identity_fields));
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/capability_definition");
  preimage.push_back('\0');
  preimage.append(core::kCapabilityDefinitionApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(identity_value));
  const auto revision = sha256_digest(preimage);

  auto root = identity_value.as_object();
  root.emplace_back("capability_definition_revision", CanonicalValue(revision));
  return canonical_json(CanonicalValue(std::move(root)));
}

[[nodiscard]] core::CapabilityDefinitionCatalogRecordRefDTO
capability_definition_reference(
    std::string_view capability_id, std::string_view record_revision,
    std::string_view stored_bytes,
    const core::ContentAddressedStoreKey &store_key) {
  return {
      core::CapabilityId(std::string(capability_id)),
      core::CapabilityDefinitionRevision(std::string(record_revision)),
      core::SchemaId(std::string(core::kCapabilityDefinitionApiVersion)),
      core::SchemaRevision(
          "sha256:"
          "2de23ede0e941349e90f0a3a344b7d240426703d250234f86e788343213727e0"),
      core::ContentDigest(sha256_digest(stored_bytes)),
      store_key,
  };
}

[[nodiscard]] std::string enum_limit_schema(std::size_t enum_count) {
  std::string yaml = R"(api_version: manyforge.closed_schema.v1
schema_id: manyforge.closed_schema.enum_limit_probe.v1
schema_revision: sha256:e0db232cd5da19fb03b2fc15cc94aaa899064d39ae09222676737e188200e1d9
schema_purpose: value
root_definition_id: value
definitions:
  - definition_id: value
    value_schema:
      kind: scalar
      scalar_type: string
      constraints:
        - kind: enum
          values:
)";
  for (std::size_t index = 0U; index < enum_count; ++index) {
    yaml.append("            - v");
    yaml.append(std::to_string(index));
    yaml.push_back('\n');
  }
  return yaml;
}

[[nodiscard]] std::string invalid_key_path_schema(std::size_t path_count,
                                                  std::size_t segment_count) {
  std::string yaml = R"(api_version: manyforge.closed_schema.v1
schema_id: manyforge.closed_schema.key_path_probe.v1
schema_revision: sha256:0000000000000000000000000000000000000000000000000000000000000000
schema_purpose: value
root_definition_id: value
definitions:
  - definition_id: value
    value_schema:
      kind: array
      min_items: 0
      max_items: 1
      collection:
        kind: keyed_record_set
        key_paths:
)";
  for (std::size_t path = 0U; path < path_count; ++path) {
    yaml.append("          - [");
    for (std::size_t segment = 0U; segment < segment_count; ++segment) {
      if (segment != 0U) {
        yaml.append(", ");
      }
      yaml.append("field");
      yaml.append(std::to_string(path));
      yaml.push_back('_');
      yaml.append(std::to_string(segment));
    }
    yaml.append("]\n");
  }
  yaml.append(R"(      item_schema:
        kind: scalar
        scalar_type: string
        constraints: []
)");
  return yaml;
}

[[nodiscard]] std::string required_string_field(std::string_view field_id,
                                                std::string value_schema) {
  std::string result{"{\"field_id\":\""};
  result.append(field_id);
  result.append(
      "\",\"presence\":\"required\",\"default\":{\"kind\":\"absent\"},"
      "\"value_schema\":");
  result.append(value_schema);
  result.push_back('}');
  return result;
}

[[nodiscard]] std::string exact_key_path_limit_schema() {
  const auto scalar = std::string{
      "{\"kind\":\"scalar\",\"scalar_type\":\"string\",\"constraints\":[]}"};
  std::string nested = scalar;
  for (std::size_t index = kMaxClosedSchemaKeyPathSegments - 1U; index > 0U;
       --index) {
    std::string field_id{"s"};
    field_id.append(std::to_string(index));
    auto child = required_string_field(field_id, std::move(nested));
    nested = "{\"kind\":\"object\",\"fields\":[";
    nested.append(child);
    nested.append("]}");
  }

  std::string fields = required_string_field("p0", std::move(nested));
  for (std::size_t index = 1U; index < kMaxClosedSchemaKeyPaths; ++index) {
    fields.append(",");
    std::string field_id{"p"};
    field_id.append(std::to_string(index));
    fields.append(required_string_field(field_id, scalar));
  }
  std::string paths = "[\"p0\"";
  for (std::size_t index = 1U; index < kMaxClosedSchemaKeyPathSegments;
       ++index) {
    paths.append(",\"s");
    paths.append(std::to_string(index));
    paths.push_back('"');
  }
  paths.push_back(']');
  for (std::size_t index = 1U; index < kMaxClosedSchemaKeyPaths; ++index) {
    paths.append(",[\"p");
    paths.append(std::to_string(index));
    paths.append("\"]");
  }

  return "{\"api_version\":\"manyforge.closed_schema.v1\","
         "\"schema_id\":\"manyforge.closed_schema.key_path_limit_probe.v1\","
         "\"schema_revision\":\"sha256:"
         "4d8c2f689ae7ad23ce44f16aa3b814cfde138d036ce49509e29afd09196ee192\","
         "\"schema_purpose\":\"value\",\"root_definition_id\":\"value\","
         "\"definitions\":[{\"definition_id\":\"value\",\"value_schema\":{"
         "\"kind\":\"array\",\"min_items\":0,\"max_items\":1,"
         "\"collection\":{\"kind\":\"keyed_record_set\",\"key_paths\":[" +
         paths + "]},\"item_schema\":{\"kind\":\"object\",\"fields\":[" +
         fields + "]}}}]}";
}

[[nodiscard]] std::string local_ref_cycle_schema(std::size_t definition_count) {
  std::string yaml = R"(api_version: manyforge.closed_schema.v1
schema_id: manyforge.closed_schema.ref_cycle_probe.v1
schema_revision: sha256:0000000000000000000000000000000000000000000000000000000000000000
schema_purpose: value
root_definition_id: alias_0
definitions:
)";
  for (std::size_t index = 0U; index < definition_count; ++index) {
    yaml.append("  - definition_id: alias_");
    yaml.append(std::to_string(index));
    yaml.append("\n    value_schema:\n      kind: local_ref\n      "
                "definition_id: alias_");
    yaml.append(std::to_string((index + 1U) % definition_count));
    yaml.push_back('\n');
  }
  return yaml;
}

[[nodiscard]] std::string
ref_expanded_depth_schema(std::size_t depth, std::string_view schema_revision) {
  std::string yaml = R"(api_version: manyforge.closed_schema.v1
schema_id: manyforge.closed_schema.depth_probe.v1
schema_revision: )";
  yaml.append(schema_revision);
  yaml.append(R"(
schema_purpose: value
root_definition_id: depth_0
definitions:
)");
  for (std::size_t index = 0U; index < depth; ++index) {
    yaml.append("  - definition_id: depth_");
    yaml.append(std::to_string(index));
    yaml.append("\n    value_schema:\n      kind: object\n      fields:\n");
    if (index + 1U < depth) {
      yaml.append(R"(        - field_id: child
          presence: required
          default:
            kind: absent
          value_schema:
            kind: local_ref
            definition_id: depth_)");
      yaml.append(std::to_string(index + 1U));
      yaml.push_back('\n');
    } else {
      yaml.append("        []\n");
    }
  }
  return yaml;
}

[[nodiscard]] std::string repeated_empty_objects(std::size_t count) {
  std::string yaml;
  yaml.reserve(3U + count * 5U);
  yaml.append("[");
  for (std::size_t index = 0U; index < count; ++index) {
    if (index != 0U) {
      yaml.push_back(',');
    }
    yaml.append("{}");
  }
  yaml.append("]\n");
  return yaml;
}

TEST(ClosedSchema, PythonOracleGoldenDescriptorsCompileAndOwnTheirStorage) {
  auto inventory = compile_resource(
      "examples/manyforge.closed_schema.example_inventory.v1.yaml");
  EXPECT_EQ(inventory.schema_id().value(),
            "manyforge.closed_schema.example_inventory.v1");
  EXPECT_EQ(inventory.schema_revision().value(),
            "sha256:"
            "dd5840f1d78b171d86631d73b333625011c81fcae9b54221ab3ea63c65446c55");
  EXPECT_EQ(inventory.schema_purpose(), ClosedSchemaPurpose::kArtifactDocument);
  EXPECT_EQ(inventory.root_shape(), ClosedSchemaRootShape::kObject);

  // The parsed descriptor has already died; the compiled schema owns every
  // definition, string, normalized default, and lookup target it uses.
  const auto candidate = parse_strict_json_model_yaml(R"(
api_version: manyforge.closed_schema.example_inventory.v1
inventory_id: cell_inventory
labels: [zeta, alpha]
resources:
  - resource_id: robot_b
    revision: sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
  - resource_id: robot_a
    revision: sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
steps:
  - name: second
  - name: first
)");
  const auto normalized = validate_and_normalize_unqualified_artifact_candidate(
      inventory, candidate);
  EXPECT_EQ(normalized.schema_purpose(),
            ClosedSchemaPurpose::kArtifactDocument);
  EXPECT_EQ(
      canonical_json(normalized.normalized_root()),
      R"({"api_version":"manyforge.closed_schema.example_inventory.v1","enabled":true,"inventory_id":"cell_inventory","labels":["alpha","zeta"],"resources":[{"resource_id":"robot_a","revision":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},{"resource_id":"robot_b","revision":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}],"retries":3,"steps":[{"name":"second"},{"name":"first"}]})");
}

TEST(ClosedSchema,
     SharedNormalizerMaterializesDefaultsSortsSetsAndPreservesOrder) {
  const auto schema = compile_resource(
      "examples/manyforge.closed_schema.example_inventory.v1.yaml");
  const auto normalized = validate_and_normalize_unqualified_artifact_candidate(
      schema, parse_strict_json_model_yaml(R"(
api_version: manyforge.closed_schema.example_inventory.v1
inventory_id: cell_inventory
labels: [zeta, alpha]
resources:
  - resource_id: robot_b
    revision: sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
  - resource_id: robot_a
    revision: sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
steps:
  - name: second
  - name: first
)"));

  EXPECT_EQ(
      std::get<bool>(member(normalized.normalized_root(), "enabled").storage()),
      true);
  EXPECT_EQ(std::get<double>(
                member(normalized.normalized_root(), "retries").storage()),
            3.0);
  EXPECT_EQ(canonical_json(member(normalized.normalized_root(), "labels")),
            R"(["alpha","zeta"])");
  EXPECT_EQ(
      canonical_json(member(normalized.normalized_root(), "resources")),
      R"([{"resource_id":"robot_a","revision":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},{"resource_id":"robot_b","revision":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}])");
  EXPECT_EQ(canonical_json(member(normalized.normalized_root(), "steps")),
            R"([{"name":"second"},{"name":"first"}])");
}

[[nodiscard]] std::string keyed_tagged_union_schema() {
  return R"({
  "api_version": "manyforge.closed_schema.v1",
  "schema_id": "manyforge.closed_schema.keyed_union_probe.v1",
  "schema_revision": "sha256:9d5c3aad5f1b022d9bbae59a3dc94c760604474ae493ab9c2c75c8ddedee7651",
  "schema_purpose": "value",
  "root_definition_id": "rows",
  "definitions": [
    {
      "definition_id": "rows",
      "value_schema": {
        "kind": "array",
        "min_items": 0,
        "max_items": 4,
        "collection": {
          "kind": "keyed_record_set",
          "key_paths": [
            [
              "id"
            ]
          ]
        },
        "item_schema": {
          "kind": "local_ref",
          "definition_id": "row"
        }
      }
    },
    {
      "definition_id": "row",
      "value_schema": {
        "kind": "tagged_union",
        "tag_field": "kind",
        "alternatives": [
          {
            "tag_value": "input",
            "value_schema": {
              "kind": "object",
              "fields": [
                {
                  "field_id": "kind",
                  "presence": "required",
                  "default": {
                    "kind": "absent"
                  },
                  "value_schema": {
                    "kind": "scalar",
                    "scalar_type": "string",
                    "constraints": [
                      {
                        "kind": "enum",
                        "values": [
                          "input"
                        ]
                      }
                    ]
                  }
                },
                {
                  "field_id": "id",
                  "presence": "required",
                  "default": {
                    "kind": "absent"
                  },
                  "value_schema": {
                    "kind": "scalar",
                    "scalar_type": "string",
                    "constraints": []
                  }
                }
              ]
            }
          },
          {
            "tag_value": "output",
            "value_schema": {
              "kind": "object",
              "fields": [
                {
                  "field_id": "kind",
                  "presence": "required",
                  "default": {
                    "kind": "absent"
                  },
                  "value_schema": {
                    "kind": "scalar",
                    "scalar_type": "string",
                    "constraints": [
                      {
                        "kind": "enum",
                        "values": [
                          "output"
                        ]
                      }
                    ]
                  }
                },
                {
                  "field_id": "id",
                  "presence": "required",
                  "default": {
                    "kind": "absent"
                  },
                  "value_schema": {
                    "kind": "scalar",
                    "scalar_type": "string",
                    "constraints": []
                  }
                }
              ]
            }
          }
        ]
      }
    }
  ]
})";
}

TEST(ClosedSchema,
     KeyedTaggedUnionsKeepCanonicalOrderingAndCrossBranchUniqueness) {
  const auto schema = compile_closed_schema_descriptor(
      parse_strict_json_model_yaml(keyed_tagged_union_schema()));
  const auto normalized = validate_and_normalize_unqualified_artifact_candidate(
      schema, parse_strict_json_model_yaml(
                  R"([{"kind":"output","id":"z"},{"kind":"input","id":"a"}])"));
  EXPECT_EQ(canonical_json(normalized.normalized_root()),
            R"([{"id":"a","kind":"input"},{"id":"z","kind":"output"}])");
  EXPECT_THROW(
      validate_and_normalize_unqualified_artifact_candidate(
          schema,
          parse_strict_json_model_yaml(
              R"([{"kind":"input","id":"same"},{"kind":"output","id":"same"}])")),
      ClosedSchemaError);
  EXPECT_THROW(validate_and_normalize_unqualified_artifact_candidate(
                   schema, parse_strict_json_model_yaml(
                               R"([{"kind":"output","id":"x","extra":true}])")),
               ClosedSchemaError);
}

TEST(ClosedSchema,
     KeyedTaggedUnionsRequireTheSameRequiredScalarPathInEveryBranch) {
  for (const auto &[before, after, message] :
       std::array<std::tuple<std::string, std::string, std::string>, 3>{
           {{"\"field_id\": \"id\"", "\"field_id\": \"other\"",
             "field id is not declared"},
            {"\"presence\": \"required\"", "\"presence\": \"optional\"",
             "key-path fields must be required"},
            {"\"scalar_type\": \"string\"", "\"scalar_type\": \"integer\"",
             "key-path scalar type differs"}}}) {
    auto source = keyed_tagged_union_schema();
    const auto where = source.rfind(before);
    ASSERT_NE(where, std::string::npos);
    source.replace(where, before.size(), after);
    expect_compile_error(std::move(source), message);
  }
}

TEST(ClosedSchema, PythonOracleTaggedUnionGoldenSelectsOneClosedBranch) {
  const auto command = compile_resource(
      "examples/manyforge.closed_schema.example_command.v1.yaml");
  EXPECT_EQ(command.schema_revision().value(),
            "sha256:"
            "37985bd40f1c50cca9a82e3952bf407ddecca0d1c32d7787cd903bfffbf5fce7");
  const auto normalized = validate_and_normalize_unqualified_artifact_candidate(
      command, parse_strict_json_model_yaml(R"(
api_version: manyforge.closed_schema.example_command.v1
command_kind: position
target: 0.5
)"));
  EXPECT_EQ(
      canonical_json(normalized.normalized_root()),
      R"({"api_version":"manyforge.closed_schema.example_command.v1","command_kind":"position","target":0.5})");
  EXPECT_THROW(validate_and_normalize_unqualified_artifact_candidate(
                   command, parse_strict_json_model_yaml(R"(
api_version: manyforge.closed_schema.example_command.v1
command_kind: position
value: true
)")),
               ClosedSchemaError);
}

TEST(ClosedSchema, OrdinaryArtifactRootsStillRequireTheirExactSchemaApi) {
  for (const auto name : {"inventory", "command"}) {
    const auto family =
        "manyforge.closed_schema.example_" + std::string(name) + ".v1";
    auto source = read_resource("examples/" + family + ".yaml");
    const auto before = "schema_id: " + family;
    const auto offset = source.find(before);
    ASSERT_NE(offset, std::string::npos);
    source.replace(offset, before.size(),
                   "schema_id: manyforge.other_family.v1");
    expect_compile_error(std::move(source),
                         "single-value string enum equal to schema_id");
  }
}

TEST(ClosedSchema, RejectsTaggedUnionBranchesThatDoNotResolveToObjects) {
  expect_compile_error(R"(
api_version: manyforge.closed_schema.v1
schema_id: manyforge.closed_schema.scalar_union_probe.v1
schema_revision: sha256:0000000000000000000000000000000000000000000000000000000000000000
schema_purpose: value
root_definition_id: value
definitions:
  - definition_id: value
    value_schema:
      kind: tagged_union
      tag_field: kind
      alternatives:
        - tag_value: scalar
          value_schema:
            kind: scalar
            scalar_type: string
            constraints: []
)",
                       "tagged-union branch must resolve to object");
}

TEST(ClosedSchema, ValuePurposeAllowsAnUntaggedScalarRoot) {
  const auto schema =
      compile_closed_schema_descriptor(parse_strict_json_model_yaml(R"(
api_version: manyforge.closed_schema.v1
schema_id: manyforge.closed_schema.example_scalar.v1
schema_revision: sha256:7ae878e5161ead2b6325bb22cc5cc06402a3e2f43aad5a3916676098512b9546
schema_purpose: value
root_definition_id: value
definitions:
  - definition_id: value
    value_schema:
      kind: scalar
      scalar_type: string
      constraints: []
)"));
  EXPECT_EQ(schema.schema_purpose(), ClosedSchemaPurpose::kValue);
  EXPECT_EQ(schema.root_shape(), ClosedSchemaRootShape::kScalar);
  const auto result = validate_and_normalize_unqualified_artifact_candidate(
      schema, parse_strict_json_model_yaml("hello\n"));
  EXPECT_EQ(result.schema_purpose(), ClosedSchemaPurpose::kValue);
  EXPECT_EQ(canonical_json(result.normalized_root()), R"("hello")");
}

TEST(ClosedSchema, IntegerSchemaUsesMathematicalBinary64Value) {
  const auto schema =
      compile_closed_schema_descriptor(parse_strict_json_model_yaml(R"(
api_version: manyforge.closed_schema.v1
schema_id: manyforge.closed_schema.integer_probe.v1
schema_revision: sha256:4ee1cd37b2449c78fdc8e31935da29d0c53c23e9f59260b62309f8713a60428c
schema_purpose: value
root_definition_id: value
definitions:
  - definition_id: value
    value_schema:
      kind: scalar
      scalar_type: integer
      constraints: []
)"));
  for (const auto spelling : {"15\n", "15.0\n", "1.5e1\n"}) {
    const auto normalized =
        validate_and_normalize_unqualified_artifact_candidate(
            schema, parse_strict_json_model_yaml(spelling));
    EXPECT_EQ(canonical_json(normalized.normalized_root()), "15");
  }
  EXPECT_THROW(validate_and_normalize_unqualified_artifact_candidate(
                   schema, parse_strict_json_model_yaml("15.5\n")),
               ClosedSchemaError);
}

TEST(ClosedSchema, ArtifactDocumentPurposeRequiresTheExactApiEnvelope) {
  expect_compile_error(R"(
api_version: manyforge.closed_schema.v1
schema_id: manyforge.closed_schema.bad_document.v1
schema_revision: sha256:0000000000000000000000000000000000000000000000000000000000000000
schema_purpose: artifact_document
root_definition_id: value
definitions:
  - definition_id: value
    value_schema:
      kind: scalar
      scalar_type: string
      constraints: []
)",
                       "root definition and every root tagged-union branch "
                       "must resolve to object");
}

TEST(ClosedSchema, BuildRegistryQualifiesOnlyTheCompleteAdmittedFamilies) {
  const auto &registry = build_qualified_closed_schema_registry();
  ASSERT_EQ(registry.catalog_schemas().size(),
            core::kCatalogRecordTypeRegistry.size());
  ASSERT_EQ(registry.value_schemas().size(), 6U);
  for (const auto &value_schema : registry.value_schemas()) {
    const auto expected_shape = value_schema.schema_id().value() ==
                                        "manyforge.value_schema.provider_"
                                        "contract.motion_controlled_stop.v1"
                                    ? ClosedSchemaRootShape::kTaggedUnion
                                    : ClosedSchemaRootShape::kObject;
    EXPECT_EQ(value_schema.compiled_schema().root_shape(), expected_shape);
  }
  std::size_t qualified_count = 0U;
  for (const auto &entry : registry.catalog_schemas()) {
    ASSERT_NE(registry.find_catalog_schema_family(entry.record_kind()),
              nullptr);
    EXPECT_EQ(entry.schema_id().value(),
              core::catalog_record_type_descriptor(entry.record_kind())
                  ->schema_family);
    if (entry.record_kind() == core::CatalogRecordKind::kCapabilityDefinition) {
      ++qualified_count;
      EXPECT_EQ(entry.qualification(), CatalogSchemaQualification::kQualified);
      ASSERT_NE(entry.compiled_schema(), nullptr);
      EXPECT_EQ(
          entry.compiled_schema()->schema_revision().value(),
          "sha256:"
          "2de23ede0e941349e90f0a3a344b7d240426703d250234f86e788343213727e0");
      EXPECT_EQ(registry.find_qualified_catalog_schema(
                    entry.record_kind(), entry.schema_id(),
                    entry.compiled_schema()->schema_revision()),
                &entry);
    } else if (entry.record_kind() == core::CatalogRecordKind::kCommandSchema) {
      ++qualified_count;
      EXPECT_EQ(entry.qualification(), CatalogSchemaQualification::kQualified);
      ASSERT_NE(entry.compiled_schema(), nullptr);
      EXPECT_EQ(
          entry.compiled_schema()->schema_revision().value(),
          "sha256:"
          "4f23fe66c0dd587d998a52a7b166bddd22d7ab8e5b28caacc48373ff373dcd84");
      EXPECT_EQ(registry.find_qualified_catalog_schema(
                    entry.record_kind(), entry.schema_id(),
                    entry.compiled_schema()->schema_revision()),
                &entry);
    } else if (entry.record_kind() == core::CatalogRecordKind::kComponentType) {
      ++qualified_count;
      EXPECT_EQ(entry.qualification(), CatalogSchemaQualification::kQualified);
      ASSERT_NE(entry.compiled_schema(), nullptr);
      EXPECT_EQ(
          entry.compiled_schema()->schema_revision().value(),
          "sha256:"
          "d971a7022a7977625256649896f8253020686a7b543c30d557162ebee8f69dde");
      EXPECT_EQ(registry.find_qualified_catalog_schema(
                    entry.record_kind(), entry.schema_id(),
                    entry.compiled_schema()->schema_revision()),
                &entry);
    } else if (entry.record_kind() ==
               core::CatalogRecordKind::kControlInterfaceContract) {
      ++qualified_count;
      EXPECT_EQ(entry.qualification(), CatalogSchemaQualification::kQualified);
      ASSERT_NE(entry.compiled_schema(), nullptr);
      EXPECT_EQ(
          entry.compiled_schema()->schema_revision().value(),
          "sha256:"
          "4abdf2c402d8e6e818bc26df59ce1c9cdc911d1e807144bdf525d2f69d0a449d");
      EXPECT_EQ(registry.find_qualified_catalog_schema(
                    entry.record_kind(), entry.schema_id(),
                    entry.compiled_schema()->schema_revision()),
                &entry);
    } else if (entry.record_kind() ==
               core::CatalogRecordKind::kControllerTopology) {
      ++qualified_count;
      EXPECT_EQ(entry.qualification(), CatalogSchemaQualification::kQualified);
      ASSERT_NE(entry.compiled_schema(), nullptr);
      EXPECT_EQ(
          entry.compiled_schema()->schema_revision().value(),
          "sha256:"
          "7415c5e2df17491366755b401337e3ad96dd96703357b20c9e0d08bf3f46a9f6");
      EXPECT_EQ(registry.find_qualified_catalog_schema(
                    entry.record_kind(), entry.schema_id(),
                    entry.compiled_schema()->schema_revision()),
                &entry);
    } else if (entry.record_kind() ==
               core::CatalogRecordKind::kStopEvidencePolicy) {
      ++qualified_count;
      EXPECT_EQ(entry.qualification(), CatalogSchemaQualification::kQualified);
      ASSERT_NE(entry.compiled_schema(), nullptr);
      EXPECT_EQ(
          entry.compiled_schema()->schema_revision().value(),
          "sha256:"
          "d55eb024cdd03360b1833671b656c962b2608227a1f1b47ed67cc019b5442f13");
      EXPECT_EQ(registry.find_qualified_catalog_schema(
                    entry.record_kind(), entry.schema_id(),
                    entry.compiled_schema()->schema_revision()),
                &entry);
    } else if (entry.record_kind() == core::CatalogRecordKind::kModelAsset) {
      ++qualified_count;
      EXPECT_EQ(entry.qualification(), CatalogSchemaQualification::kQualified);
      ASSERT_NE(entry.compiled_schema(), nullptr);
      EXPECT_EQ(
          entry.compiled_schema()->schema_revision().value(),
          "sha256:"
          "397af1d5fbe5d2e83d442565c9096637a26595d63b5d8e03012e8f2d082fa0b6");
      EXPECT_EQ(registry.find_qualified_catalog_schema(
                    entry.record_kind(), entry.schema_id(),
                    entry.compiled_schema()->schema_revision()),
                &entry);
    } else if (entry.record_kind() ==
               core::CatalogRecordKind::kRuntimeProfile) {
      ++qualified_count;
      EXPECT_EQ(entry.qualification(), CatalogSchemaQualification::kQualified);
      ASSERT_NE(entry.compiled_schema(), nullptr);
      EXPECT_EQ(
          entry.compiled_schema()->schema_revision().value(),
          "sha256:"
          "c254afa8c2bfcc803f12a7af63b1f09a8ee804b1ffa2a65b7361ad9309b87ae7");
      EXPECT_EQ(registry.find_qualified_catalog_schema(
                    entry.record_kind(), entry.schema_id(),
                    entry.compiled_schema()->schema_revision()),
                &entry);
    } else if (entry.record_kind() == core::CatalogRecordKind::kContactPolicy) {
      ++qualified_count;
      EXPECT_EQ(entry.qualification(), CatalogSchemaQualification::kQualified);
      ASSERT_NE(entry.compiled_schema(), nullptr);
      EXPECT_EQ(
          entry.compiled_schema()->schema_revision().value(),
          "sha256:"
          "f7adb173ef71d9de9011c85a19224cace5e721918fb1659fcada6ab93901ad33");
      EXPECT_EQ(registry.find_qualified_catalog_schema(
                    entry.record_kind(), entry.schema_id(),
                    entry.compiled_schema()->schema_revision()),
                &entry);
    } else if (entry.record_kind() ==
               core::CatalogRecordKind::kProviderManifest) {
      ++qualified_count;
      EXPECT_EQ(entry.qualification(), CatalogSchemaQualification::kQualified);
      ASSERT_NE(entry.compiled_schema(), nullptr);
      EXPECT_EQ(
          entry.compiled_schema()->schema_revision().value(),
          "sha256:"
          "b242431032bf183f81718ae1b7b8501ee5f705e52d455732e0052215565bf4cc");
      EXPECT_EQ(registry.find_qualified_catalog_schema(
                    entry.record_kind(), entry.schema_id(),
                    entry.compiled_schema()->schema_revision()),
                &entry);
    } else {
      EXPECT_EQ(entry.qualification(),
                CatalogSchemaQualification::kUnqualified);
      EXPECT_EQ(entry.compiled_schema(), nullptr);
    }
    EXPECT_EQ(registry.find_qualified_catalog_schema(
                  entry.record_kind(), entry.schema_id(),
                  core::SchemaRevision("sha256:"
                                       "000000000000000000000000000000000000000"
                                       "0000000000000000000000000")),
              nullptr);
  }
  EXPECT_EQ(qualified_count, 10U);
  EXPECT_EQ(registry.qualification_registry_digest().value(),
            sha256_digest(canonical_json(registry.normalized_projection())));
  EXPECT_NE(
      registry.find_qualified_value_schema(
          core::SchemaId("manyforge.value_schema.provider_contract.empty.v1"),
          core::SchemaRevision("sha256:"
                               "fb466dd341076cbdb6fc97558ad28bc46032d25daaf4d4b"
                               "b9d548bf642b83270")),
      nullptr);
  EXPECT_NE(registry.find_qualified_value_schema(
                core::SchemaId("manyforge.value_schema.provider_contract."
                               "motion_controlled_stop.v1"),
                core::SchemaRevision("sha256:"
                                     "65e6f4462981225916834490d4e3cb4d42b70611"
                                     "a2287e1b76fdb1cbcabcb113")),
            nullptr);
  EXPECT_NE(registry.find_qualified_value_schema(
                core::SchemaId("manyforge.value_schema.runtime.empty.v1"),
                core::SchemaRevision("sha256:"
                                     "03457d535d31818fedaa7e44f48432a08feb705e4"
                                     "d22c9a8f5e50977c8dbf785")),
            nullptr);
  EXPECT_EQ(registry.find_qualified_value_schema(
                core::SchemaId("manyforge.none.v1"),
                core::SchemaRevision("sha256:"
                                     "00000000000000000000000000000000000000000"
                                     "00000000000000000000000")),
            nullptr);
}

TEST(ClosedSchema,
     DirectCanonicalValueNormalizationRequiresABuildQualifiedValueSchema) {
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *schema = registry.find_qualified_value_schema(
      core::SchemaId("manyforge.value_schema.runtime.empty.v1"),
      core::SchemaRevision(
          "sha256:"
          "03457d535d31818fedaa7e44f48432a08feb705e4d22c9a8f5e50977c8dbf785"));
  ASSERT_NE(schema, nullptr);

  const auto normalized = validate_and_normalize_build_qualified_value(
      *schema, CanonicalValue(CanonicalValue::Object{}));
  EXPECT_EQ(normalized.schema_id(), schema->schema_id());
  EXPECT_EQ(normalized.schema_revision(), schema->schema_revision());
  EXPECT_EQ(canonical_json(normalized.normalized_value()), "{}");

  test::expect_source_preserving_rvalue_transfers(
      *schema, [](const BuildQualifiedValueSchemaEntry &value) {
        return std::tuple{value.schema_id(), value.schema_revision(),
                          std::string(value.relative_path())};
      });

  auto schema_source = *schema;
  const auto schema_destination = std::move(schema_source);
  EXPECT_EQ(schema_source.schema_id(), schema->schema_id());
  EXPECT_EQ(schema_source.schema_revision(), schema->schema_revision());
  EXPECT_EQ(schema_source.relative_path(), schema->relative_path());
  EXPECT_EQ(schema_destination.schema_id(), schema->schema_id());
  EXPECT_EQ(schema_destination.schema_revision(), schema->schema_revision());
  EXPECT_EQ(schema_destination.relative_path(), schema->relative_path());
  EXPECT_EQ(validate_and_normalize_build_qualified_value(
                schema_source, CanonicalValue(CanonicalValue::Object{}))
                .normalized_value(),
            validate_and_normalize_build_qualified_value(
                schema_destination, CanonicalValue(CanonicalValue::Object{}))
                .normalized_value());

  test::expect_source_preserving_rvalue_transfers(
      normalized, [](const NonGraphQualifiedSchemaNormalizedValue &value) {
        return std::tuple{value.schema_id(), value.schema_revision(),
                          value.normalized_value()};
      });

  EXPECT_THROW(
      validate_and_normalize_build_qualified_value(
          *schema, value_object({{"unexpected", CanonicalValue(true)}})),
      ClosedSchemaError);
}

TEST(ClosedSchema, IoComponentWrapperUsesSharedRowsAndMaterializesDeviceLimit) {
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *schema = registry.find_qualified_value_schema(
      core::SchemaId("manyforge.value_schema.runtime.io_provider_execution.v1"),
      core::SchemaRevision(
          "sha256:"
          "500cfe739120003e06057cdf78a94f006eb4fc4ef157a7f55f70ab4d1aa45b1d"));
  ASSERT_NE(schema, nullptr);
  const auto source = parse_strict_json_model_yaml(R"({
    "io_signals":[
      {"controllable_component_id":"device.io","command_target_id":"gpio",
       "resource_role_id":"ready","signal_name":"ready","source_id":"source.ready",
       "value_type":"bool","direction":"input","update_mode":"change_only",
       "transition_capability":"sampled_only","freshness":{"mode":"provider_validity"},
       "provider":{"profile":"mock","initial_value":false,"ingress_depth":64}},
      {"controllable_component_id":"device.io","command_target_id":"gpio",
       "resource_role_id":"lamp","signal_name":"lamp","source_id":"source.lamp",
       "value_type":"bool","direction":"output",
       "provider":{"profile":"mock_output","channel":"lamp"}}],
    "io_provider_devices":[{"controllable_component_id":"device.io"}]
  })");
  const auto normalized = validate_and_normalize_build_qualified_value(*schema, source.root());
  const auto &root = normalized.normalized_value();
  ASSERT_EQ(root.find_object_member("io_signals")->as_array().size(), 2U);
  const auto &device = root.find_object_member("io_provider_devices")->as_array().front();
  EXPECT_EQ(canonical_json(*device.find_object_member("lifecycle_command_timeout_s")), "10");
  const auto &output = root.find_object_member("io_signals")->as_array().front();
  EXPECT_EQ(output.find_object_member("resource_role_id")->as_string(), "lamp");
  EXPECT_EQ(output.find_object_member("freshness"), nullptr);
  EXPECT_EQ(output.find_object_member("output_completion"), nullptr);
  for (const auto *field : {"auto_answer_delay_ns", "manual_only"}) {
    auto invalid_rows = source.root().find_object_member("io_signals")->as_array();
    auto invalid_row = invalid_rows.front().as_object();
    for (auto &[name, value] : invalid_row) {
      if (name == "provider") {
        auto invalid_provider = value.as_object();
        invalid_provider.emplace_back(
            field, std::string_view(field) == "auto_answer_delay_ns"
                       ? CanonicalValue(250000000.0)
                       : CanonicalValue(false));
        value = CanonicalValue(std::move(invalid_provider));
      }
    }
    invalid_rows.front() = CanonicalValue(std::move(invalid_row));
    auto invalid_root = source.root().as_object();
    for (auto &[name, value] : invalid_root) {
      if (name == "io_signals")
        value = CanonicalValue(std::move(invalid_rows));
    }
    EXPECT_THROW((void)validate_and_normalize_build_qualified_value(
                     *schema, CanonicalValue(std::move(invalid_root))),
                 ClosedSchemaError) << field;
  }
}

TEST(ClosedSchema, IoComponentWrapperRefusesEmptySignalsMultipleDevicesAndMotionFields) {
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *schema = registry.find_qualified_value_schema(
      core::SchemaId("manyforge.value_schema.runtime.io_provider_execution.v1"),
      core::SchemaRevision(
          "sha256:"
          "500cfe739120003e06057cdf78a94f006eb4fc4ef157a7f55f70ab4d1aa45b1d"));
  ASSERT_NE(schema, nullptr);
  const std::string signal = R"({"controllable_component_id":"device.io",
    "command_target_id":"gpio","resource_role_id":"lamp","signal_name":"lamp",
    "source_id":"source.lamp","value_type":"bool","direction":"output",
    "provider":{"profile":"ros_set_bool","service":"/device/lamp"}})";
  const auto normalize = [&](const std::string &signals, const std::string &devices,
                             const std::string &extra = "") {
    return validate_and_normalize_build_qualified_value(*schema, parse_strict_json_model_yaml(
        "{\"io_signals\":" + signals + ",\"io_provider_devices\":" + devices + extra + "}").root());
  };
  const std::string device = R"({"controllable_component_id":"device.io"})";
  EXPECT_NO_THROW((void)normalize("[" + signal + "]", "[" + device + "]"));
  EXPECT_THROW((void)normalize("[]", "[" + device + "]"), ClosedSchemaError);
  EXPECT_THROW((void)normalize("[" + signal + "]", "[]"), ClosedSchemaError);
  EXPECT_THROW((void)normalize("[" + signal + "]", "[" + device + "," + device + "]"),
               ClosedSchemaError);
  EXPECT_THROW((void)normalize("[" + signal + "]", "[" + device + "]",
                               ",\"controller_profiles\":[]"), ClosedSchemaError);
  EXPECT_THROW((void)normalize("[" + signal + "]",
      R"([{"controllable_component_id":"device.io","lifecycle_command_timeout_s":3601}])"),
      ClosedSchemaError);
}

TEST(ClosedSchema, PoseSourceComponentSchemaIsClosedAndBuildQualified) {
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *schema = registry.find_qualified_value_schema(
      core::SchemaId("manyforge.value_schema.runtime.pose_source.v2"),
      core::SchemaRevision(
          "sha256:"
          "a7e77850ee255f3bfc989e5105e1c37ca7c767fa63768160e75e81bb8e6e7173"));
  ASSERT_NE(schema, nullptr);
  const std::string simulation = R"("simulation":{"poll_period_ms":250,
    "source_coordinate_frame_id":"universe","target_frame_id":"universe",
    "relative_reference":{"entity":"/World/tool","link":"tool_link",
      "source_header_frame_id":"tool"}})";
  const std::string detection = R"("detection3d":{"topic":"/output",
    "target_frame_id":"link_base","fallback_frame_id":""})";
  const std::string overlay = R"("robot_overlay":{"robot_component_id":"robot",
    "joint_state":{"topic":"/isaac_joint_states",
      "required_joint_names":["joint2","joint1"]},
    "tcp_entity":{"entity":"/World/tool","source_header_frame_id":"tool",
      "local_position_xyz":[0,0,0.2]}})";
  const auto normalize = [&](const std::string &fields) {
    return validate_and_normalize_build_qualified_value(
        *schema, parse_strict_json_model_yaml("{" + fields + "}").root());
  };
  const auto budget = std::string(R"("stream_budget_ns":1000000000)");
  const auto normalized = normalize(budget + "," + simulation + "," +
                                    detection + "," + overlay);
  // Joint order is authored observer order, not a set.
  EXPECT_EQ(canonical_json(*normalized.normalized_value()
                                .find_object_member("robot_overlay")
                                ->find_object_member("joint_state")
                                ->find_object_member("required_joint_names")),
            R"(["joint2","joint1"])");
  EXPECT_NO_THROW((void)normalize(budget));
  EXPECT_THROW((void)normalize(simulation), ClosedSchemaError);
  EXPECT_THROW((void)normalize(budget + R"(,"pose_sources":[])"),
               ClosedSchemaError);
  EXPECT_THROW((void)normalize(R"("stream_budget_ns":0)"), ClosedSchemaError);
  auto bad_poll = simulation;
  bad_poll.replace(bad_poll.find("250"), 3U, "10");
  EXPECT_THROW((void)normalize(budget + "," + bad_poll), ClosedSchemaError);
  // The header frame is an explicit authored field of every entity.
  const std::string header_field = R"(,
      "source_header_frame_id":"tool")";
  auto header = simulation;
  header.replace(header.find(header_field), header_field.size(), "");
  EXPECT_THROW((void)normalize(budget + "," + header), ClosedSchemaError);
}

TEST(ClosedSchema, QualifiesEveryAndOnlyTheExactBuiltinCapabilityDefinition) {
  ASSERT_FALSE(core::kBuiltinCapabilityDefinitionRegistry.empty());
  CanonicalValue::Array registry_projection;
  registry_projection.reserve(
      core::kBuiltinCapabilityDefinitionRegistry.size());
  const core::ContentAddressedStoreKey store_key{
      "sha256/capability-definition-fixture"};
  for (const auto &expected : core::kBuiltinCapabilityDefinitionRegistry) {
    const auto document = capability_definition_document(
        expected.capability_id, expected.scope, expected.provider_binding_kind,
        expected.requirement_parameters_schema_ref);
    const auto parsed = parse_strict_json_model_yaml(document);
    const auto &computed_revision = std::get<std::string>(
        member(parsed.root(), "capability_definition_revision").storage());
    ASSERT_EQ(computed_revision, expected.capability_definition_revision)
        << expected.capability_id;

    const auto reference = capability_definition_reference(
        expected.capability_id, expected.capability_definition_revision,
        document, store_key);
    const auto qualified = resolve_and_qualify_capability_definition(
        reference, store_key, document);
    EXPECT_EQ(qualified.reference(), reference);
    EXPECT_EQ(qualified.value(),
              core::materialize_capability_definition(expected));
    EXPECT_EQ(qualified.normalized_root(), parsed.root());
    registry_projection.push_back(qualified.normalized_root());
  }
  EXPECT_EQ(sha256_digest(
                canonical_json(CanonicalValue(std::move(registry_projection)))),
            core::kBuiltinCapabilityDefinitionRegistryDigest);

  const auto unknown = capability_definition_document(
      "unknown.capability", core::CapabilityScope::kCellGlobal,
      core::CapabilityProviderBindingKind::kCapabilityProviderBinding);
  const auto unknown_parsed = parse_strict_json_model_yaml(unknown);
  const auto &unknown_revision = std::get<std::string>(
      member(unknown_parsed.root(), "capability_definition_revision")
          .storage());
  const auto unknown_reference = capability_definition_reference(
      "unknown.capability", unknown_revision, unknown, store_key);
  EXPECT_THROW(resolve_and_qualify_capability_definition(unknown_reference,
                                                         store_key, unknown),
               CapabilityDefinitionCodecError);

  const auto altered_scope = capability_definition_document(
      "planning.motion.joint_goal", core::CapabilityScope::kCellGlobal,
      core::CapabilityProviderBindingKind::kCapabilityProviderBinding);
  const auto altered_scope_parsed = parse_strict_json_model_yaml(altered_scope);
  const auto &altered_scope_revision = std::get<std::string>(
      member(altered_scope_parsed.root(), "capability_definition_revision")
          .storage());
  const auto altered_scope_reference = capability_definition_reference(
      "planning.motion.joint_goal", altered_scope_revision, altered_scope,
      store_key);
  EXPECT_THROW(resolve_and_qualify_capability_definition(
                   altered_scope_reference, store_key, altered_scope),
               CapabilityDefinitionCodecError);

  const auto added_schema_ref = capability_definition_document(
      "planning.motion.joint_goal", core::CapabilityScope::kTargetBinding,
      core::CapabilityProviderBindingKind::kCapabilityProviderBinding,
      core::ValueSchemaRefDescriptor{
          "manyforge.capability_parameters.example.v1",
          "sha256:"
          "0000000000000000000000000000000000000000000000000000000000000000"});
  const auto added_schema_ref_parsed =
      parse_strict_json_model_yaml(added_schema_ref);
  const auto &added_schema_ref_revision = std::get<std::string>(
      member(added_schema_ref_parsed.root(), "capability_definition_revision")
          .storage());
  const auto added_schema_ref_reference = capability_definition_reference(
      "planning.motion.joint_goal", added_schema_ref_revision, added_schema_ref,
      store_key);
  EXPECT_THROW(resolve_and_qualify_capability_definition(
                   added_schema_ref_reference, store_key, added_schema_ref),
               CapabilityDefinitionCodecError);
}

TEST(ClosedSchema, CapabilityDefinitionRejectsStaleIdentityAndOpenShape) {
  const auto &expected = core::kBuiltinCapabilityDefinitionRegistry.front();
  auto stale = capability_definition_document(
      expected.capability_id, expected.scope, expected.provider_binding_kind);
  const auto digest_offset = stale.find("sha256:");
  ASSERT_NE(digest_offset, std::string::npos);
  stale[digest_offset + 7U] = stale[digest_offset + 7U] == '0' ? '1' : '0';
  const core::ContentAddressedStoreKey store_key{
      "sha256/capability-definition-negative"};
  const auto stale_reference = capability_definition_reference(
      expected.capability_id, expected.capability_definition_revision, stale,
      store_key);
  EXPECT_THROW(resolve_and_qualify_capability_definition(stale_reference,
                                                         store_key, stale),
               CapabilityDefinitionCodecError);

  const std::string open_shape =
      "{\"api_version\":\"manyforge.capability_definition.v1\","
      "\"capability_id\":\"planning.motion.joint_goal\","
      "\"capability_definition_revision\":\"" +
      std::string(expected.capability_definition_revision) +
      "\",\"scope\":\"target_binding\","
      "\"provider_binding_kind\":\"capability_provider_binding\","
      "\"unexpected\":true}";
  const auto open_reference = capability_definition_reference(
      expected.capability_id, expected.capability_definition_revision,
      open_shape, store_key);
  EXPECT_THROW(resolve_and_qualify_capability_definition(open_reference,
                                                         store_key, open_shape),
               ClosedSchemaError);
}

TEST(ClosedSchema,
     CapabilityDefinitionCatalogReferenceQualifiesAllSevenFields) {
  const auto &expected = core::kBuiltinCapabilityDefinitionRegistry.front();
  const auto stored_bytes = capability_definition_document(
      expected.capability_id, expected.scope, expected.provider_binding_kind);
  const core::ContentAddressedStoreKey store_key{
      "sha256/capability-definition-planning-motion-joint-goal"};
  auto reference = capability_definition_reference(
      expected.capability_id, expected.capability_definition_revision,
      stored_bytes, store_key);

  const auto qualified = resolve_and_qualify_capability_definition(
      reference, store_key, stored_bytes);
  EXPECT_EQ(qualified.value(),
            core::materialize_capability_definition(expected));

  auto wrong_id = reference;
  wrong_id.record_id = core::CapabilityId("planning.motion.pose_goal");
  EXPECT_THROW(resolve_and_qualify_capability_definition(wrong_id, store_key,
                                                         stored_bytes),
               CapabilityDefinitionCodecError);

  auto wrong_revision = reference;
  wrong_revision.record_revision = core::CapabilityDefinitionRevision(
      "sha256:"
      "0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_capability_definition(
                   wrong_revision, store_key, stored_bytes),
               CapabilityDefinitionCodecError);

  auto wrong_schema_id = reference;
  wrong_schema_id.schema_id = core::SchemaId("manyforge.command_schema.v1");
  EXPECT_THROW(resolve_and_qualify_capability_definition(
                   wrong_schema_id, store_key, stored_bytes),
               CapabilityDefinitionCodecError);

  auto wrong_schema_revision = reference;
  wrong_schema_revision.schema_revision = core::SchemaRevision(
      "sha256:"
      "0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_capability_definition(
                   wrong_schema_revision, store_key, stored_bytes),
               CapabilityDefinitionCodecError);

  auto wrong_digest = reference;
  wrong_digest.content_digest = core::ContentDigest(
      "sha256:"
      "0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_capability_definition(
                   wrong_digest, store_key, stored_bytes),
               CapabilityDefinitionCodecError);

  EXPECT_THROW(resolve_and_qualify_capability_definition(
                   reference, core::ContentAddressedStoreKey("sha256/other"),
                   stored_bytes),
               CapabilityDefinitionCodecError);

  EXPECT_THROW(resolve_and_qualify_capability_definition(
                   reference, core::ContentAddressedStoreKey(""), stored_bytes),
               CapabilityDefinitionCodecError);

  const std::string oversized(kMaxArtifactEncodedBytes + 1U, ' ');
  EXPECT_THROW(resolve_and_qualify_capability_definition(reference, store_key,
                                                         oversized),
               CapabilityDefinitionCodecError);
}

TEST(ClosedSchema, EveryPackagedYamlResourcePassesStrictUniqueKeyIngress) {
  const auto resource_root =
      std::filesystem::path(MANYFORGE_CLOSED_SCHEMA_RESOURCE_DIR);
  std::size_t parsed_count = 0U;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(resource_root)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".yaml") {
      continue;
    }
    const auto relative_path =
        std::filesystem::relative(entry.path(), resource_root).generic_string();
    EXPECT_NO_THROW(
        (void)parse_strict_json_model_yaml(read_resource(relative_path)))
        << relative_path;
    ++parsed_count;
  }
  EXPECT_EQ(parsed_count, 20U);
}

TEST(ClosedSchema,
     PackagedBootstrapAndRegistryAreExactNonAuthoritativeProjections) {
  const auto bootstrap = parse_strict_json_model_yaml(
      read_resource("manyforge.closed_schema.bootstrap.v1.yaml"));
  const auto &declared = std::get<std::string>(
      member(bootstrap.root(), "bootstrap_contract_digest").storage());
  EXPECT_EQ(declared, kClosedSchemaBootstrapContractDigest);
  CanonicalValue::Object descriptor;
  for (const auto &[name, value] : bootstrap.root().as_object()) {
    if (name != "bootstrap_contract_digest") {
      descriptor.emplace_back(name, value);
    }
  }
  EXPECT_EQ(
      sha256_digest(canonical_json(CanonicalValue(std::move(descriptor)))),
      kClosedSchemaBootstrapContractDigest);
  const auto &purposes = std::get<CanonicalValue::Array>(
      member(bootstrap.root(), "schema_purposes").storage());
  ASSERT_EQ(purposes.size(), 2U);
  EXPECT_EQ(std::get<std::string>(purposes[0].storage()), "artifact_document");
  EXPECT_EQ(std::get<std::string>(purposes[1].storage()), "value");

  const auto &limit_value = member(bootstrap.root(), "limits");
  const auto &limits = limit_value.as_object();
  const std::array<std::pair<std::string_view, double>, 19U> expected_limits{{
      {"max_alternatives", static_cast<double>(kMaxClosedSchemaAlternatives)},
      {"max_array_items", static_cast<double>(kMaxClosedSchemaArrayItems)},
      {"max_collection_depth", static_cast<double>(kMaxArtifactNestingDepth)},
      {"max_constraints", static_cast<double>(kMaxClosedSchemaConstraints)},
      {"max_definitions", static_cast<double>(kMaxClosedSchemaDefinitions)},
      {"max_encoded_document_bytes",
       static_cast<double>(kMaxArtifactEncodedBytes)},
      {"max_enum_values", static_cast<double>(kMaxClosedSchemaEnumValues)},
      {"max_fields", static_cast<double>(kMaxClosedSchemaFields)},
      {"max_key_path_segments",
       static_cast<double>(kMaxClosedSchemaKeyPathSegments)},
      {"max_key_paths", static_cast<double>(kMaxClosedSchemaKeyPaths)},
      {"max_normalized_aggregate_scalar_bytes",
       static_cast<double>(kMaxNormalizedAggregateScalarBytes)},
      {"max_normalized_value_depth",
       static_cast<double>(kMaxArtifactNestingDepth)},
      {"max_normalized_value_nodes",
       static_cast<double>(kMaxArtifactNodeCount)},
      {"max_safe_integer", static_cast<double>(kMaxArtifactSafeInteger)},
      {"max_scalar_utf8_bytes", static_cast<double>(kMaxArtifactScalarBytes)},
      {"max_schema_depth", static_cast<double>(kMaxArtifactNestingDepth)},
      {"max_schema_nodes", static_cast<double>(kMaxArtifactNodeCount)},
      {"max_schema_string_code_points",
       static_cast<double>(kMaxClosedSchemaStringCodePoints)},
      {"max_total_document_nodes", static_cast<double>(kMaxArtifactNodeCount)},
  }};
  ASSERT_EQ(limits.size(), expected_limits.size());
  for (const auto &[name, expected] : expected_limits) {
    EXPECT_EQ(std::get<double>(member(limit_value, name).storage()), expected)
        << name;
  }

  const auto registry_projection =
      parse_strict_json_model_yaml(read_resource("registry.v1.yaml"));
  EXPECT_EQ(registry_projection.root(),
            build_qualified_closed_schema_registry().normalized_projection());
  EXPECT_EQ(sha256_digest(canonical_json(registry_projection.root())),
            build_qualified_closed_schema_registry()
                .qualification_registry_digest()
                .value());
  EXPECT_EQ(registry_projection.root().as_object().size(), 6U);
  EXPECT_EQ(std::get<std::string>(
                member(registry_projection.root(), "api_version").storage()),
            "manyforge.closed_schema_registry.v1");
  EXPECT_EQ(
      std::get<std::string>(
          member(registry_projection.root(), "dialect_api_version").storage()),
      kClosedSchemaApiVersion);
  EXPECT_EQ(std::get<std::string>(
                member(registry_projection.root(), "bootstrap_contract_digest")
                    .storage()),
            kClosedSchemaBootstrapContractDigest);
  const auto &projected_examples = std::get<CanonicalValue::Array>(
      member(registry_projection.root(), "example_schemas").storage());
  struct ExpectedExample final {
    std::string_view schema_id;
    std::string_view schema_revision;
    std::string_view relative_path;
  };
  const std::array<ExpectedExample, 2U> expected_examples{{
      {"manyforge.closed_schema.example_command.v1",
       "sha256:"
       "37985bd40f1c50cca9a82e3952bf407ddecca0d1c32d7787cd903bfffbf5fce7",
       "examples/manyforge.closed_schema.example_command.v1.yaml"},
      {"manyforge.closed_schema.example_inventory.v1",
       "sha256:"
       "dd5840f1d78b171d86631d73b333625011c81fcae9b54221ab3ea63c65446c55",
       "examples/manyforge.closed_schema.example_inventory.v1.yaml"},
  }};
  ASSERT_EQ(projected_examples.size(), expected_examples.size());
  for (std::size_t index = 0U; index < projected_examples.size(); ++index) {
    const auto &projected = projected_examples[index];
    const auto &expected = expected_examples[index];
    EXPECT_EQ(projected.as_object().size(), 4U);
    EXPECT_EQ(std::get<std::string>(member(projected, "schema_id").storage()),
              expected.schema_id);
    EXPECT_EQ(
        std::get<std::string>(member(projected, "schema_revision").storage()),
        expected.schema_revision);
    EXPECT_EQ(
        std::get<std::string>(member(projected, "relative_path").storage()),
        expected.relative_path);
    EXPECT_EQ(
        std::get<std::string>(member(projected, "qualification").storage()),
        "dialect_example_only");
    const auto compiled = compile_resource(expected.relative_path);
    EXPECT_EQ(compiled.schema_id().value(), expected.schema_id);
    EXPECT_EQ(compiled.schema_revision().value(), expected.schema_revision);
  }
  const auto &projected_values = std::get<CanonicalValue::Array>(
      member(registry_projection.root(), "value_schemas").storage());
  const auto &projected_catalog = std::get<CanonicalValue::Array>(
      member(registry_projection.root(), "catalog_schemas").storage());
  const auto &registry = build_qualified_closed_schema_registry();
  ASSERT_EQ(projected_values.size(), registry.value_schemas().size());
  for (std::size_t index = 0U; index < projected_values.size(); ++index) {
    const auto &projected = projected_values[index];
    const auto &built = registry.value_schemas()[index];
    EXPECT_EQ(std::get<std::string>(member(projected, "schema_id").storage()),
              built.schema_id().value());
    EXPECT_EQ(
        std::get<std::string>(member(projected, "schema_revision").storage()),
        built.schema_revision().value());
    EXPECT_EQ(
        std::get<std::string>(member(projected, "relative_path").storage()),
        built.relative_path());
    EXPECT_EQ(
        std::get<std::string>(member(projected, "qualification").storage()),
        "qualified");
    const auto compiled = compile_resource(built.relative_path());
    EXPECT_EQ(compiled.schema_id(), built.schema_id());
    EXPECT_EQ(compiled.schema_revision(), built.schema_revision());
    EXPECT_EQ(compiled.schema_purpose(), ClosedSchemaPurpose::kValue);
  }
  ASSERT_EQ(projected_catalog.size(), registry.catalog_schemas().size());
  for (std::size_t index = 0U; index < projected_catalog.size(); ++index) {
    const auto &projected = projected_catalog[index];
    const auto &built = registry.catalog_schemas()[index];
    EXPECT_EQ(std::get<std::string>(member(projected, "record_kind").storage()),
              core::to_string(built.record_kind()));
    EXPECT_EQ(std::get<std::string>(member(projected, "schema_id").storage()),
              built.schema_id().value());
    if (built.record_kind() == core::CatalogRecordKind::kCapabilityDefinition ||
        built.record_kind() == core::CatalogRecordKind::kCommandSchema ||
        built.record_kind() == core::CatalogRecordKind::kComponentType ||
        built.record_kind() == core::CatalogRecordKind::kContactPolicy ||
        built.record_kind() ==
            core::CatalogRecordKind::kControlInterfaceContract ||
        built.record_kind() == core::CatalogRecordKind::kControllerTopology ||
        built.record_kind() == core::CatalogRecordKind::kModelAsset ||
        built.record_kind() == core::CatalogRecordKind::kProviderManifest ||
        built.record_kind() == core::CatalogRecordKind::kRuntimeProfile ||
        built.record_kind() == core::CatalogRecordKind::kStopEvidencePolicy) {
      EXPECT_EQ(projected.as_object().size(), 5U);
      EXPECT_EQ(
          std::get<std::string>(member(projected, "qualification").storage()),
          "qualified");
      std::string expected_revision;
      std::string expected_path;
      if (built.record_kind() ==
          core::CatalogRecordKind::kCapabilityDefinition) {
        expected_revision =
            "sha256:"
            "2de23ede0e941349e90f0a3a344b7d240426703d250234f86e788343213727e0";
        expected_path = "catalog/manyforge.capability_definition.v1.yaml";
      } else if (built.record_kind() ==
                 core::CatalogRecordKind::kCommandSchema) {
        expected_revision =
            "sha256:"
            "4f23fe66c0dd587d998a52a7b166bddd22d7ab8e5b28caacc48373ff373dcd84";
        expected_path = "catalog/manyforge.command_schema.v1.yaml";
      } else if (built.record_kind() ==
                 core::CatalogRecordKind::kComponentType) {
        expected_revision =
            "sha256:"
            "d971a7022a7977625256649896f8253020686a7b543c30d557162ebee8f69dde";
        expected_path = "catalog/manyforge.component_type.v1.yaml";
      } else if (built.record_kind() ==
                 core::CatalogRecordKind::kContactPolicy) {
        expected_revision =
            "sha256:"
            "f7adb173ef71d9de9011c85a19224cace5e721918fb1659fcada6ab93901ad33";
        expected_path = "catalog/manyforge.contact_policy.v1.yaml";
      } else if (built.record_kind() ==
                 core::CatalogRecordKind::kControlInterfaceContract) {
        expected_revision =
            "sha256:"
            "4abdf2c402d8e6e818bc26df59ce1c9cdc911d1e807144bdf525d2f69d0a449d";
        expected_path = "catalog/manyforge.control_interface_contract.v1.yaml";
      } else if (built.record_kind() ==
                 core::CatalogRecordKind::kControllerTopology) {
        expected_revision =
            "sha256:"
            "7415c5e2df17491366755b401337e3ad96dd96703357b20c9e0d08bf3f46a9f6";
        expected_path = "catalog/manyforge.controller_topology.v1.yaml";
      } else if (built.record_kind() == core::CatalogRecordKind::kModelAsset) {
        expected_revision =
            "sha256:"
            "397af1d5fbe5d2e83d442565c9096637a26595d63b5d8e03012e8f2d082fa0b6";
        expected_path = "catalog/manyforge.model_asset.v1.yaml";
      } else if (built.record_kind() ==
                 core::CatalogRecordKind::kProviderManifest) {
        expected_revision =
            "sha256:"
            "b242431032bf183f81718ae1b7b8501ee5f705e52d455732e0052215565bf4cc";
        expected_path = "catalog/manyforge.provider_manifest.v1.yaml";
      } else if (built.record_kind() ==
                 core::CatalogRecordKind::kRuntimeProfile) {
        expected_revision =
            "sha256:"
            "c254afa8c2bfcc803f12a7af63b1f09a8ee804b1ffa2a65b7361ad9309b87ae7";
        expected_path = "catalog/manyforge.runtime_profile.v1.yaml";
      } else {
        expected_revision =
            "sha256:"
            "d55eb024cdd03360b1833671b656c962b2608227a1f1b47ed67cc019b5442f13";
        expected_path = "catalog/manyforge.stop_evidence_policy.v2.yaml";
      }
      EXPECT_EQ(
          std::get<std::string>(member(projected, "schema_revision").storage()),
          expected_revision);
      EXPECT_EQ(
          std::get<std::string>(member(projected, "relative_path").storage()),
          expected_path);
      const auto compiled = compile_resource(expected_path);
      EXPECT_EQ(compiled.schema_revision(),
                built.compiled_schema()->schema_revision());
    } else {
      EXPECT_EQ(projected.as_object().size(), 3U);
      EXPECT_EQ(
          std::get<std::string>(member(projected, "qualification").storage()),
          "unqualified");
    }
  }

  std::set<std::string> source_tree_yaml;
  const auto resource_root =
      std::filesystem::path(MANYFORGE_CLOSED_SCHEMA_RESOURCE_DIR);
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(resource_root)) {
    if (entry.is_regular_file() && entry.path().extension() == ".yaml") {
      source_tree_yaml.insert(
          std::filesystem::relative(entry.path(), resource_root)
              .generic_string());
    }
  }
  const std::set<std::string> expected_source_tree_yaml{
      "manyforge.closed_schema.bootstrap.v1.yaml",
      "registry.v1.yaml",
      "catalog/manyforge.capability_definition.v1.yaml",
      "catalog/manyforge.command_schema.v1.yaml",
      "catalog/manyforge.component_type.v1.yaml",
      "catalog/manyforge.contact_policy.v1.yaml",
      "catalog/manyforge.control_interface_contract.v1.yaml",
      "catalog/manyforge.controller_topology.v1.yaml",
      "catalog/manyforge.model_asset.v1.yaml",
      "catalog/manyforge.provider_manifest.v1.yaml",
      "catalog/manyforge.runtime_profile.v1.yaml",
      "catalog/manyforge.stop_evidence_policy.v2.yaml",
      "examples/manyforge.closed_schema.example_command.v1.yaml",
      "examples/manyforge.closed_schema.example_inventory.v1.yaml",
      "value/manyforge.value_schema.provider_contract.empty.v1.yaml",
      "value/"
      "manyforge.value_schema.provider_contract.motion_controlled_stop.v1.yaml",
      "value/manyforge.value_schema.runtime.empty.v1.yaml",
      "value/manyforge.value_schema.runtime.ros2_control_execution.v1.yaml",
      "value/manyforge.value_schema.runtime.io_provider_execution.v1.yaml",
      "value/manyforge.value_schema.runtime.pose_source.v2.yaml",
  };
  EXPECT_EQ(source_tree_yaml, expected_source_tree_yaml);
}

TEST(ClosedSchema, RejectsWrongDeclaredRevisionAfterNormalization) {
  auto yaml =
      read_resource("examples/manyforge.closed_schema.example_command.v1.yaml");
  const auto digest = yaml.find("sha256:37985");
  ASSERT_NE(digest, std::string::npos);
  yaml[digest + 7U] = 'f';
  EXPECT_THROW(
      compile_closed_schema_descriptor(parse_strict_json_model_yaml(yaml)),
      ClosedSchemaError);
}

TEST(ClosedSchema, PinsEnumAndKeyPathBootstrapCeilings) {
  const auto exact =
      compile_closed_schema_descriptor(parse_strict_json_model_yaml(
          enum_limit_schema(kMaxClosedSchemaEnumValues)));
  EXPECT_EQ(exact.root_shape(), ClosedSchemaRootShape::kScalar);
  const auto exact_paths = compile_closed_schema_descriptor(
      parse_strict_json_model_yaml(exact_key_path_limit_schema()));
  EXPECT_EQ(exact_paths.root_shape(), ClosedSchemaRootShape::kArray);
  expect_compile_error(enum_limit_schema(kMaxClosedSchemaEnumValues + 1U),
                       "item count outside [1, 1024]");
  expect_compile_error(
      invalid_key_path_schema(kMaxClosedSchemaKeyPaths + 1U, 1U),
      "item count outside [1, 16]");
  expect_compile_error(
      invalid_key_path_schema(1U, kMaxClosedSchemaKeyPathSegments + 1U),
      "item count outside [1, 16]");
}

TEST(ClosedSchema, DeepReferenceFailuresAreIterativeAndDeterministic) {
  expect_compile_error(local_ref_cycle_schema(kMaxClosedSchemaDefinitions),
                       "local_ref cycle is forbidden");
  const auto depth_64 = compile_closed_schema_descriptor(
      parse_strict_json_model_yaml(ref_expanded_depth_schema(
          64U,
          "sha256:"
          "caa9da25b4b0b71373e9cd93ef251c2d09b09aab2c508c39c569e7b4aca83df3")));
  EXPECT_EQ(depth_64.root_shape(), ClosedSchemaRootShape::kObject);
  expect_compile_error(
      ref_expanded_depth_schema(
          65U,
          "sha256:"
          "1baa020354ffe2c111ab7937fd5a6787aa4af9c833423290fb865da22e54b731"),
      "ref-expanded collection depth 65 exceeds 64");
}

TEST(ClosedSchema, MaterializedDefaultsCountTowardAggregateScalarBudget) {
  std::string schema_yaml = R"(api_version: manyforge.closed_schema.v1
schema_id: manyforge.closed_schema.output_budget_probe.v1
schema_revision: sha256:07a49ddc830162c6a3a23ab18d887c01daacd82eddb7897a69cfd77e1d10e917
schema_purpose: value
root_definition_id: rows
definitions:
  - definition_id: row
    value_schema:
      kind: object
      fields:
        - field_id: payload
          presence: optional
          default:
            kind: materialized
            value: ")";
  schema_yaml.append(1024U, 'x');
  schema_yaml.append(R"("
          value_schema:
            kind: scalar
            scalar_type: string
            constraints: []
  - definition_id: rows
    value_schema:
      kind: array
      min_items: 0
      max_items: 20000
      collection:
        kind: ordered
      item_schema:
        kind: local_ref
        definition_id: row
)");
  const auto schema = compile_closed_schema_descriptor(
      parse_strict_json_model_yaml(schema_yaml));
  EXPECT_NO_THROW(validate_and_normalize_unqualified_artifact_candidate(
      schema, parse_strict_json_model_yaml(repeated_empty_objects(16209U))));
  EXPECT_THROW(
      validate_and_normalize_unqualified_artifact_candidate(
          schema, parse_strict_json_model_yaml(repeated_empty_objects(16210U))),
      ClosedSchemaError);
}

TEST(ClosedSchema, MaterializedDefaultsAndMappingKeysCountTowardNodeBudget) {
  const auto schema =
      compile_closed_schema_descriptor(parse_strict_json_model_yaml(R"(
api_version: manyforge.closed_schema.v1
schema_id: manyforge.closed_schema.node_budget_probe.v1
schema_revision: sha256:7c1c681dc8a8d6630d91b62f5fc0a5f3fd4e34007fcd9bfcd44c9256445d9d57
schema_purpose: value
root_definition_id: rows
definitions:
  - definition_id: row
    value_schema:
      kind: object
      fields:
        - field_id: enabled
          presence: optional
          default:
            kind: materialized
            value: true
          value_schema:
            kind: scalar
            scalar_type: boolean
            constraints: []
  - definition_id: rows
    value_schema:
      kind: array
      min_items: 0
      max_items: 100000
      collection:
        kind: ordered
      item_schema:
        kind: local_ref
        definition_id: row
)"));
  EXPECT_NO_THROW(validate_and_normalize_unqualified_artifact_candidate(
      schema, parse_strict_json_model_yaml(repeated_empty_objects(83333U))));
  EXPECT_THROW(
      validate_and_normalize_unqualified_artifact_candidate(
          schema, parse_strict_json_model_yaml(repeated_empty_objects(83334U))),
      ClosedSchemaError);
}

TEST(ClosedSchema, CompleteCanonicalValueOutputBudgetEnforcesEveryV1Limit) {
  auto nested_value = [](std::size_t depth) {
    CanonicalValue value(nullptr);
    for (std::size_t index = 0U; index < depth; ++index) {
      CanonicalValue::Array wrapper;
      wrapper.push_back(std::move(value));
      value = CanonicalValue(std::move(wrapper));
    }
    return value;
  };
  EXPECT_NO_THROW(detail::validate_canonical_value_output_budget(
      nested_value(kMaxArtifactNestingDepth)));
  EXPECT_THROW(detail::validate_canonical_value_output_budget(
                   nested_value(kMaxArtifactNestingDepth + 1U)),
               ClosedSchemaError);

  EXPECT_NO_THROW(detail::validate_canonical_value_output_budget(
      CanonicalValue(std::string(kMaxArtifactScalarBytes, 'x'))));
  EXPECT_THROW(detail::validate_canonical_value_output_budget(CanonicalValue(
                   std::string(kMaxArtifactScalarBytes + 1U, 'x'))),
               ClosedSchemaError);

  auto keyed_node_aggregate = [](std::size_t item_count) {
    CanonicalValue::Array items(item_count, CanonicalValue(nullptr));
    return value_object({{"items", CanonicalValue(std::move(items))}});
  };
  EXPECT_NO_THROW(detail::validate_canonical_value_output_budget(
      keyed_node_aggregate(kMaxArtifactNodeCount - 3U)));
  EXPECT_THROW(detail::validate_canonical_value_output_budget(
                   keyed_node_aggregate(kMaxArtifactNodeCount - 2U)),
               ClosedSchemaError);

  const auto scalar_size = kMaxNormalizedAggregateScalarBytes / 16U - 2U;
  auto scalar_aggregate = [scalar_size](bool append_overflow) {
    CanonicalValue::Array values;
    values.reserve(17U);
    for (std::size_t index = 0U; index < 16U; ++index) {
      values.emplace_back(std::string(scalar_size, 'x'));
    }
    if (append_overflow) {
      values.emplace_back(nullptr);
    }
    return CanonicalValue(std::move(values));
  };
  EXPECT_NO_THROW(
      detail::validate_canonical_value_output_budget(scalar_aggregate(false)));
  EXPECT_THROW(
      detail::validate_canonical_value_output_budget(scalar_aggregate(true)),
      ClosedSchemaError);
}

TEST(ClosedSchema,
     CumulativeConstructionBudgetRejectsBeforeRetainingAnotherConfiguration) {
  detail::CumulativeNormalizedValueBudget budget;
  const CanonicalValue one_mebibyte_scalar(
      std::string(kMaxArtifactScalarBytes - 2U, 'x'));
  const CanonicalValue large_configuration =
      value_object({{"payload", one_mebibyte_scalar}});

  // Every object-root configuration is individually within all ceilings.
  EXPECT_NO_THROW(
      detail::validate_canonical_value_output_budget(large_configuration));

  // Fifteen such configurations fit the shared construction budget. The
  // sixteenth is rejected before the graph can retain it.
  for (std::size_t index = 0U; index < 15U; ++index) {
    EXPECT_NO_THROW(detail::validate_canonical_value_output_budget_cumulatively(
        large_configuration, budget));
  }
  EXPECT_THROW(detail::validate_canonical_value_output_budget_cumulatively(
                   large_configuration, budget),
               ClosedSchemaError);

  detail::CumulativeNormalizedValueBudget node_budget;
  const CanonicalValue half_node_budget(CanonicalValue::Array(
      (kMaxArtifactNodeCount / 2U) - 1U, CanonicalValue(nullptr)));
  EXPECT_NO_THROW(detail::validate_canonical_value_output_budget_cumulatively(
      half_node_budget, node_budget));
  EXPECT_NO_THROW(detail::validate_canonical_value_output_budget_cumulatively(
      half_node_budget, node_budget));
  EXPECT_EQ(node_budget.nodes, kMaxArtifactNodeCount);
  EXPECT_THROW(detail::validate_canonical_value_output_budget_cumulatively(
                   CanonicalValue(nullptr), node_budget),
               ClosedSchemaError);

  // The final one-shot graph budget counts graph-owned content and all
  // configurations together rather than resetting at field boundaries.
  CanonicalValue::Array configurations;
  configurations.reserve(15U);
  for (std::size_t index = 0U; index < 15U; ++index) {
    configurations.push_back(large_configuration);
  }
  EXPECT_THROW(
      detail::validate_canonical_value_output_budget(value_object({
          {"cell_id", CanonicalValue("cell_a")},
          {"graph_owned_payload", one_mebibyte_scalar},
          {"configurations", CanonicalValue(std::move(configurations))},
      })),
      ClosedSchemaError);
}

TEST(ClosedSchema, RosGripperTargetRowColdDecodesThirteenFields) {
  for (const std::string_view kind : {"pneumatic", "position_commanded"}) {
    const auto fixture =
        test::make_lite_gripper_only_retention_fixture({}, kind);
    const auto &source = fixture.qualified.deployment().normalized_authored_root();
    const auto compiled = compile_deployment_draft(canonical_json(source));
    EXPECT_EQ(verify_deployment_draft(compiled.canonical_json).canonical_json,
              compiled.canonical_json);
    const std::array profiles{*fixture.ros2_control_execution_runtime_profile};
    // Independently parse and verify bytes again, then use the production cold
    // retained row decoder. The original fixture DTO is not the decoded input.
    const auto cold = verify_deployment_artifact_revision(
        parse_strict_json_model_yaml(compiled.canonical_json), profiles);
    const auto &unit = cold.graph().graph().runtime_units.front();
    const auto &rows = std::get<core::JsonValueDTO::Array>(
        unit.configuration.at("gripper_target_configurations").value);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(std::get<core::JsonObjectDTO>(rows.front().value).size(), 13U);
    const auto decoded =
        detail::decode_ros2_control_execution_gripper_target_configurations(rows);
    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_EQ(decoded.front().actuation_kind, kind);
    EXPECT_DOUBLE_EQ(decoded.front().command_duration_s, 0.8);
    EXPECT_EQ(decoded.front().output_mode, core::GripperOutputMode::kCommanded);
    EXPECT_NO_THROW((void)detail::require_ros2_control_execution_runtime_required_rows(
        cold.graph().graph(), unit));

    auto common = std::get<core::JsonObjectDTO>(rows.front().value);
    common.erase("output_mode");
    ASSERT_EQ(common.size(), 12U);
    const auto common_rows = detail::decode_execution_gripper_target_configurations(
        {core::JsonValueDTO{common}}, "common configuration");
    ASSERT_EQ(common_rows.size(), 1U);
    EXPECT_DOUBLE_EQ(common_rows.front().command_duration_s, 0.8);
    EXPECT_EQ(common_rows.front().actuation_kind, kind);
  }
}

TEST(ClosedSchema, RosGripperTargetRowRejectsForeignFieldAtBothBoundaries) {
  for (const std::string_view kind : {"pneumatic", "position_commanded"}) {
    const auto fixture =
        test::make_lite_gripper_only_retention_fixture({}, kind);
    const auto &graph = fixture.qualified.deployment().graph().graph();
    auto rows = std::get<core::JsonValueDTO::Array>(
        graph.runtime_units.front().configuration.at("gripper_target_configurations").value);
    std::get<core::JsonObjectDTO>(rows.front().value).emplace(
        "foreign_timeout", core::JsonValueDTO{2.5});
    auto foreign_unit = graph.runtime_units.front();
    foreign_unit.configuration.at("gripper_target_configurations").value = rows;
    EXPECT_THROW((void)detail::require_ros2_control_execution_runtime_required_rows(
                     graph, foreign_unit), RetainedBundleError);
    EXPECT_THROW((void)detail::decode_ros2_control_execution_gripper_target_configurations(rows),
                 RetainedBundleError);
  }
}

TEST(ClosedSchema, RosNoneOutputModeColdDecodes) {
  const auto fixture = test::make_lite_gripper_only_retention_fixture({}, "pneumatic", 0.25, "none");
  const auto &source = fixture.qualified.deployment().normalized_authored_root();
  const std::array profiles{*fixture.ros2_control_execution_runtime_profile};
  const auto compiled = compile_deployment_draft(canonical_json(source));
  const auto cold = verify_deployment_artifact_revision(
      parse_strict_json_model_yaml(compiled.canonical_json), profiles);
  const auto &rows = std::get<core::JsonValueDTO::Array>(
      cold.graph().graph().runtime_units.front().configuration.at("gripper_target_configurations").value);
  const auto typed = detail::decode_ros2_control_execution_gripper_target_configurations(rows);
  ASSERT_EQ(typed.size(), 1U);
  EXPECT_EQ(typed.front().output_mode, core::GripperOutputMode::kNone);
  EXPECT_DOUBLE_EQ(typed.front().command_duration_s, 0.25);
}

} // namespace
} // namespace manyforge::deployment
