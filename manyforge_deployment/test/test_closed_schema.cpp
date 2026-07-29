#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment {
namespace {

static_assert(!std::is_aggregate_v<CompiledClosedSchema>);
static_assert(!std::is_default_constructible_v<CompiledClosedSchema>);
static_assert(!std::is_aggregate_v<UnqualifiedSchemaNormalizedArtifactCandidate>);
static_assert(
    !std::is_default_constructible_v<UnqualifiedSchemaNormalizedArtifactCandidate>);
static_assert(!std::is_aggregate_v<BuildCatalogSchemaEntry>);
static_assert(!std::is_aggregate_v<BuildQualifiedValueSchemaEntry>);
static_assert(kMaxClosedSchemaEnumValues == 1024U);
static_assert(kMaxClosedSchemaKeyPaths == 16U);
static_assert(kMaxClosedSchemaKeyPathSegments == 16U);

[[nodiscard]] std::string read_resource(std::string_view relative_path) {
  const auto path =
      std::filesystem::path(MANYFORGE_CLOSED_SCHEMA_RESOURCE_DIR) / relative_path;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("unable to open test resource: " + path.string());
  }
  std::ostringstream bytes;
  bytes << input.rdbuf();
  return bytes.str();
}

[[nodiscard]] CompiledClosedSchema compile_resource(std::string_view relative_path) {
  return compile_closed_schema_descriptor(
      parse_strict_json_model_yaml(read_resource(relative_path)));
}

void expect_compile_error(std::string yaml, std::string_view expected_message) {
  try {
    (void)compile_closed_schema_descriptor(parse_strict_json_model_yaml(yaml));
    FAIL() << "expected closed-schema compilation to fail";
  } catch (const ClosedSchemaError& error) {
    EXPECT_NE(std::string(error.what()).find(expected_message), std::string::npos)
        << error.what();
  }
}

[[nodiscard]] const CanonicalValue& member(const CanonicalValue& value,
                                           std::string_view name) {
  const auto* found = value.find_object_member(name);
  if (found == nullptr) {
    throw std::runtime_error("missing test projection member: " + std::string(name));
  }
  return *found;
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
  const auto scalar =
      std::string{"{\"kind\":\"scalar\",\"scalar_type\":\"string\",\"constraints\":[]}"};
  std::string nested = scalar;
  for (std::size_t index = kMaxClosedSchemaKeyPathSegments - 1U; index > 0U; --index) {
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
  for (std::size_t index = 1U; index < kMaxClosedSchemaKeyPathSegments; ++index) {
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
         paths +
         "]},\"item_schema\":{\"kind\":\"object\",\"fields\":[" + fields + "]}}}]}";
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
    yaml.append("\n    value_schema:\n      kind: local_ref\n      definition_id: alias_");
    yaml.append(std::to_string((index + 1U) % definition_count));
    yaml.push_back('\n');
  }
  return yaml;
}

[[nodiscard]] std::string ref_expanded_depth_schema(
    std::size_t depth, std::string_view schema_revision) {
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
  auto inventory =
      compile_resource("examples/manyforge.closed_schema.example_inventory.v1.yaml");
  EXPECT_EQ(inventory.schema_id().value(),
            "manyforge.closed_schema.example_inventory.v1");
  EXPECT_EQ(inventory.schema_revision().value(),
            "sha256:dd5840f1d78b171d86631d73b333625011c81fcae9b54221ab3ea63c65446c55");
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
  const auto normalized =
      validate_and_normalize_unqualified_artifact_candidate(inventory, candidate);
  EXPECT_EQ(normalized.schema_purpose(), ClosedSchemaPurpose::kArtifactDocument);
  EXPECT_EQ(
      canonical_json(normalized.normalized_root()),
      R"({"api_version":"manyforge.closed_schema.example_inventory.v1","enabled":true,"inventory_id":"cell_inventory","labels":["alpha","zeta"],"resources":[{"resource_id":"robot_a","revision":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},{"resource_id":"robot_b","revision":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}],"retries":3,"steps":[{"name":"second"},{"name":"first"}]})");
}

TEST(ClosedSchema, PythonOracleTaggedUnionGoldenSelectsOneClosedBranch) {
  const auto command =
      compile_resource("examples/manyforge.closed_schema.example_command.v1.yaml");
  EXPECT_EQ(command.schema_revision().value(),
            "sha256:37985bd40f1c50cca9a82e3952bf407ddecca0d1c32d7787cd903bfffbf5fce7");
  const auto normalized = validate_and_normalize_unqualified_artifact_candidate(
      command, parse_strict_json_model_yaml(R"(
api_version: manyforge.closed_schema.example_command.v1
command_kind: position
target: 0.5
)"));
  EXPECT_EQ(canonical_json(normalized.normalized_root()),
            R"({"api_version":"manyforge.closed_schema.example_command.v1","command_kind":"position","target":0.5})");
  EXPECT_THROW(
      validate_and_normalize_unqualified_artifact_candidate(
          command, parse_strict_json_model_yaml(R"(
api_version: manyforge.closed_schema.example_command.v1
command_kind: position
value: true
)")),
      ClosedSchemaError);
}

TEST(ClosedSchema, ValuePurposeAllowsAnUntaggedScalarRoot) {
  const auto schema = compile_closed_schema_descriptor(parse_strict_json_model_yaml(R"(
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
  const auto result =
      validate_and_normalize_unqualified_artifact_candidate(
          schema, parse_strict_json_model_yaml("hello\n"));
  EXPECT_EQ(result.schema_purpose(), ClosedSchemaPurpose::kValue);
  EXPECT_EQ(canonical_json(result.normalized_root()), R"("hello")");
}

TEST(ClosedSchema, IntegerSchemaUsesMathematicalBinary64Value) {
  const auto schema = compile_closed_schema_descriptor(parse_strict_json_model_yaml(R"(
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
  EXPECT_THROW(
      validate_and_normalize_unqualified_artifact_candidate(
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
                       "root definition and every root tagged-union branch must resolve to object");
}

TEST(ClosedSchema, BuildRegistryCannotBePopulatedFromInput) {
  const auto& registry = build_qualified_closed_schema_registry();
  ASSERT_EQ(registry.catalog_schemas().size(), core::kCatalogRecordTypeRegistry.size());
  EXPECT_TRUE(registry.value_schemas().empty());
  for (const auto& entry : registry.catalog_schemas()) {
    EXPECT_EQ(entry.qualification(), CatalogSchemaQualification::kUnqualified);
    EXPECT_EQ(entry.compiled_schema(), nullptr);
    ASSERT_NE(registry.find_catalog_schema_family(entry.record_kind()), nullptr);
    EXPECT_EQ(registry.find_qualified_catalog_schema(
                  entry.record_kind(), entry.schema_id(),
                  core::SchemaRevision(
                      "sha256:0000000000000000000000000000000000000000000000000000000000000000")),
              nullptr);
    EXPECT_EQ(entry.schema_id().value(),
              core::catalog_record_type_descriptor(entry.record_kind())->schema_family);
  }
  EXPECT_EQ(registry.find_qualified_value_schema(
                core::SchemaId("manyforge.none.v1"),
                core::SchemaRevision(
                    "sha256:0000000000000000000000000000000000000000000000000000000000000000")),
            nullptr);
}

TEST(ClosedSchema, PackagedBootstrapAndRegistryAreExactNonAuthoritativeProjections) {
  const auto bootstrap = parse_strict_json_model_yaml(
      read_resource("manyforge.closed_schema.bootstrap.v1.yaml"));
  const auto& declared =
      std::get<std::string>(member(bootstrap.root(), "bootstrap_contract_digest").storage());
  EXPECT_EQ(declared, kClosedSchemaBootstrapContractDigest);
  CanonicalValue::Object descriptor;
  for (const auto& [name, value] : bootstrap.root().as_object()) {
    if (name != "bootstrap_contract_digest") {
      descriptor.emplace_back(name, value);
    }
  }
  EXPECT_EQ(sha256_digest(canonical_json(CanonicalValue(std::move(descriptor)))),
            kClosedSchemaBootstrapContractDigest);
  const auto& purposes =
      std::get<CanonicalValue::Array>(member(bootstrap.root(), "schema_purposes").storage());
  ASSERT_EQ(purposes.size(), 2U);
  EXPECT_EQ(std::get<std::string>(purposes[0].storage()), "artifact_document");
  EXPECT_EQ(std::get<std::string>(purposes[1].storage()), "value");

  const auto& limit_value = member(bootstrap.root(), "limits");
  const auto& limits = limit_value.as_object();
  const std::array<std::pair<std::string_view, double>, 19U> expected_limits{{
      {"max_alternatives", static_cast<double>(kMaxClosedSchemaAlternatives)},
      {"max_array_items", static_cast<double>(kMaxClosedSchemaArrayItems)},
      {"max_collection_depth", static_cast<double>(kMaxArtifactNestingDepth)},
      {"max_constraints", static_cast<double>(kMaxClosedSchemaConstraints)},
      {"max_definitions", static_cast<double>(kMaxClosedSchemaDefinitions)},
      {"max_encoded_document_bytes", static_cast<double>(kMaxArtifactEncodedBytes)},
      {"max_enum_values", static_cast<double>(kMaxClosedSchemaEnumValues)},
      {"max_fields", static_cast<double>(kMaxClosedSchemaFields)},
      {"max_key_path_segments", static_cast<double>(kMaxClosedSchemaKeyPathSegments)},
      {"max_key_paths", static_cast<double>(kMaxClosedSchemaKeyPaths)},
      {"max_normalized_aggregate_scalar_bytes",
       static_cast<double>(kMaxNormalizedAggregateScalarBytes)},
      {"max_normalized_value_depth", static_cast<double>(kMaxArtifactNestingDepth)},
      {"max_normalized_value_nodes", static_cast<double>(kMaxArtifactNodeCount)},
      {"max_safe_integer", static_cast<double>(kMaxArtifactSafeInteger)},
      {"max_scalar_utf8_bytes", static_cast<double>(kMaxArtifactScalarBytes)},
      {"max_schema_depth", static_cast<double>(kMaxArtifactNestingDepth)},
      {"max_schema_nodes", static_cast<double>(kMaxArtifactNodeCount)},
      {"max_schema_string_code_points",
       static_cast<double>(kMaxClosedSchemaStringCodePoints)},
      {"max_total_document_nodes", static_cast<double>(kMaxArtifactNodeCount)},
  }};
  ASSERT_EQ(limits.size(), expected_limits.size());
  for (const auto& [name, expected] : expected_limits) {
    EXPECT_EQ(std::get<double>(member(limit_value, name).storage()), expected)
        << name;
  }

  const auto registry_projection =
      parse_strict_json_model_yaml(read_resource("registry.v1.yaml"));
  EXPECT_EQ(registry_projection.root().as_object().size(), 6U);
  EXPECT_EQ(std::get<std::string>(
                member(registry_projection.root(), "api_version").storage()),
            "manyforge.closed_schema_registry.v1");
  EXPECT_EQ(std::get<std::string>(
                member(registry_projection.root(), "dialect_api_version").storage()),
            kClosedSchemaApiVersion);
  EXPECT_EQ(std::get<std::string>(
                member(registry_projection.root(), "bootstrap_contract_digest").storage()),
            kClosedSchemaBootstrapContractDigest);
  const auto& projected_examples = std::get<CanonicalValue::Array>(
      member(registry_projection.root(), "example_schemas").storage());
  struct ExpectedExample final {
    std::string_view schema_id;
    std::string_view schema_revision;
    std::string_view relative_path;
  };
  const std::array<ExpectedExample, 2U> expected_examples{{
      {"manyforge.closed_schema.example_command.v1",
       "sha256:37985bd40f1c50cca9a82e3952bf407ddecca0d1c32d7787cd903bfffbf5fce7",
       "examples/manyforge.closed_schema.example_command.v1.yaml"},
      {"manyforge.closed_schema.example_inventory.v1",
       "sha256:dd5840f1d78b171d86631d73b333625011c81fcae9b54221ab3ea63c65446c55",
       "examples/manyforge.closed_schema.example_inventory.v1.yaml"},
  }};
  ASSERT_EQ(projected_examples.size(), expected_examples.size());
  for (std::size_t index = 0U; index < projected_examples.size(); ++index) {
    const auto& projected = projected_examples[index];
    const auto& expected = expected_examples[index];
    EXPECT_EQ(projected.as_object().size(), 4U);
    EXPECT_EQ(std::get<std::string>(member(projected, "schema_id").storage()),
              expected.schema_id);
    EXPECT_EQ(
        std::get<std::string>(member(projected, "schema_revision").storage()),
        expected.schema_revision);
    EXPECT_EQ(std::get<std::string>(member(projected, "relative_path").storage()),
              expected.relative_path);
    EXPECT_EQ(std::get<std::string>(member(projected, "qualification").storage()),
              "dialect_example_only");
    const auto compiled = compile_resource(expected.relative_path);
    EXPECT_EQ(compiled.schema_id().value(), expected.schema_id);
    EXPECT_EQ(compiled.schema_revision().value(), expected.schema_revision);
  }
  EXPECT_TRUE(std::get<CanonicalValue::Array>(
                  member(registry_projection.root(), "value_schemas").storage())
                  .empty());
  const auto& projected_catalog = std::get<CanonicalValue::Array>(
      member(registry_projection.root(), "catalog_schemas").storage());
  const auto& registry = build_qualified_closed_schema_registry();
  ASSERT_EQ(projected_catalog.size(), registry.catalog_schemas().size());
  for (std::size_t index = 0U; index < projected_catalog.size(); ++index) {
    const auto& projected = projected_catalog[index];
    const auto& built = registry.catalog_schemas()[index];
    EXPECT_EQ(projected.as_object().size(), 3U);
    EXPECT_EQ(std::get<std::string>(member(projected, "record_kind").storage()),
              core::to_string(built.record_kind()));
    EXPECT_EQ(std::get<std::string>(member(projected, "schema_id").storage()),
              built.schema_id().value());
    EXPECT_EQ(std::get<std::string>(member(projected, "qualification").storage()),
              "unqualified");
  }

  std::set<std::string> source_tree_yaml;
  const auto resource_root =
      std::filesystem::path(MANYFORGE_CLOSED_SCHEMA_RESOURCE_DIR);
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(resource_root)) {
    if (entry.is_regular_file() && entry.path().extension() == ".yaml") {
      source_tree_yaml.insert(
          std::filesystem::relative(entry.path(), resource_root).generic_string());
    }
  }
  const std::set<std::string> expected_source_tree_yaml{
      "manyforge.closed_schema.bootstrap.v1.yaml",
      "registry.v1.yaml",
      "examples/manyforge.closed_schema.example_command.v1.yaml",
      "examples/manyforge.closed_schema.example_inventory.v1.yaml",
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
          "sha256:caa9da25b4b0b71373e9cd93ef251c2d09b09aab2c508c39c569e7b4aca83df3")));
  EXPECT_EQ(depth_64.root_shape(), ClosedSchemaRootShape::kObject);
  expect_compile_error(
      ref_expanded_depth_schema(
          65U,
          "sha256:1baa020354ffe2c111ab7937fd5a6787aa4af9c833423290fb865da22e54b731"),
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
  const auto schema =
      compile_closed_schema_descriptor(parse_strict_json_model_yaml(schema_yaml));
  EXPECT_NO_THROW(validate_and_normalize_unqualified_artifact_candidate(
      schema, parse_strict_json_model_yaml(repeated_empty_objects(16209U))));
  EXPECT_THROW(validate_and_normalize_unqualified_artifact_candidate(
                   schema, parse_strict_json_model_yaml(repeated_empty_objects(16210U))),
               ClosedSchemaError);
}

TEST(ClosedSchema, MaterializedDefaultsAndMappingKeysCountTowardNodeBudget) {
  const auto schema = compile_closed_schema_descriptor(parse_strict_json_model_yaml(R"(
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
  EXPECT_THROW(validate_and_normalize_unqualified_artifact_candidate(
                   schema, parse_strict_json_model_yaml(repeated_empty_objects(83334U))),
               ClosedSchemaError);
}

}  // namespace
}  // namespace manyforge::deployment
