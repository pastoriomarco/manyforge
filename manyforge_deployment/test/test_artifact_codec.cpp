#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <string>
#include <variant>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/activation_preparation_request.hpp"
#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {
namespace {

[[nodiscard]] std::string wide_mapping(std::size_t member_count, bool append_first_key_duplicate) {
  std::string yaml;
  yaml.reserve(member_count * 24U);
  for (std::size_t offset = 0U; offset < member_count; ++offset) {
    const auto index = member_count - offset - 1U;
    const auto suffix = std::to_string(index);
    yaml.append("key_");
    if (suffix.size() < 6U) {
      yaml.append(6U - suffix.size(), '0');
    }
    yaml.append(suffix);
    yaml.append(": null\n");
  }
  if (append_first_key_duplicate) {
    yaml.append("key_000000: null\n");
  }
  return yaml;
}

TEST(ArtifactCodec, DecodesOneClosedJsonModelDocument) {
  const auto candidate = parse_strict_json_model_yaml(R"(
api_version: manyforge.cell_graph.v1
enabled: true
optional: null
count: 12
ratio: 1.25
quoted_number: "12"
single_quoted_boolean: 'true'
names: [alpha, beta]
)");

  const auto& root = candidate.root();
  ASSERT_TRUE(root.is_object());
  ASSERT_NE(root.find_object_member("count"), nullptr);
  EXPECT_EQ(std::get<double>(root.find_object_member("count")->storage()), 12.0);
  EXPECT_EQ(std::get<std::string>(root.find_object_member("quoted_number")->storage()), "12");
  EXPECT_EQ(std::get<std::string>(root.find_object_member("single_quoted_boolean")->storage()),
            "true");
}

TEST(ArtifactCodec, RejectsDuplicateAndNonStringMappingKeys) {
  EXPECT_THROW(parse_strict_json_model_yaml("key: one\nkey: two\n"), ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("1: one\n"), ArtifactCodecError);
}

TEST(ArtifactCodec, RejectsAliasesAnchorsMergesAndTags) {
  EXPECT_THROW(parse_strict_json_model_yaml("base: &base {value: one}\ncopy: *base\n"),
               ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("base: &base {value: one}\ncopy:\n  <<: *base\n"),
               ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("value: !!str text\n"), ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("%YAML 1.1\n---\nvalue: text\n"), ArtifactCodecError);
}

TEST(ArtifactCodec, RejectsMultipleOrEmptyDocuments) {
  EXPECT_THROW(parse_strict_json_model_yaml("---\na: 1\n---\nb: 2\n"), ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml(""), ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("---\n...\n"), ArtifactCodecError);
}

TEST(ArtifactCodec, RejectsUnsafeOrNonFiniteNumbersAndNegativeZero) {
  EXPECT_THROW(parse_strict_json_model_yaml("value: 9007199254740992\n"), ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("value: 9007199254740992.0\n"), ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("value: 9.007199254740992e15\n"), ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("value: 1e999\n"), ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("value: -.inf\n"), ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("value: .nan\n"), ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("value: +.NaN\n"), ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("value: -.nan\n"), ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("value: -0\n"), ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("value: 1e-999\n"), ArtifactCodecError);

  const auto exact_zero = parse_strict_json_model_yaml("value: 0e-999\n");
  EXPECT_EQ(std::get<double>(exact_zero.root().find_object_member("value")->storage()), 0.0);
}

TEST(ArtifactCodec, KeepsYamlOnlyPlainScalarsAsStrings) {
  const auto candidate = parse_strict_json_model_yaml(R"(
leading_zero: 01
date_like: 2026-07-29
hex_like: 0x10
upper_boolean: TRUE
)");

  const auto& root = candidate.root();
  EXPECT_EQ(std::get<std::string>(root.find_object_member("leading_zero")->storage()), "01");
  EXPECT_EQ(std::get<std::string>(root.find_object_member("date_like")->storage()), "2026-07-29");
  EXPECT_EQ(std::get<std::string>(root.find_object_member("hex_like")->storage()), "0x10");
  EXPECT_EQ(std::get<std::string>(root.find_object_member("upper_boolean")->storage()), "TRUE");
}

TEST(ArtifactCodec, RequiresExplicitJsonNullSpelling) {
  EXPECT_THROW(parse_strict_json_model_yaml("value: ~\n"), ArtifactCodecError);
  EXPECT_THROW(parse_strict_json_model_yaml("value:\n"), ArtifactCodecError);

  const auto candidate = parse_strict_json_model_yaml("value: \"~\"\n");
  EXPECT_EQ(std::get<std::string>(candidate.root().find_object_member("value")->storage()), "~");
}

TEST(ArtifactCodec, RejectsInvalidUtf8AndNonNfcText) {
  std::string invalid{"value: \""};
  invalid.push_back(static_cast<char>(0xff));
  invalid += "\"\n";
  EXPECT_THROW(parse_strict_json_model_yaml(invalid), ArtifactCodecError);

  EXPECT_THROW(parse_strict_json_model_yaml("value: \"e\xCC\x81\"\n"), ArtifactCodecError);
}

TEST(ArtifactCodec, DistinguishesMergeSyntaxFromQuotedStringKey) {
  EXPECT_THROW(parse_strict_json_model_yaml("<<: value\n"), ArtifactCodecError);

  const auto candidate = parse_strict_json_model_yaml("\"<<\": value\n");
  EXPECT_EQ(std::get<std::string>(candidate.root().find_object_member("<<")->storage()), "value");
}

TEST(ArtifactCodec, EnforcesFixedResourceBounds) {
  std::string too_deep(kMaxArtifactNestingDepth + 1U, '[');
  too_deep += "null";
  too_deep.append(kMaxArtifactNestingDepth + 1U, ']');
  EXPECT_THROW(parse_strict_json_model_yaml(too_deep), ArtifactCodecError);

  std::string too_many_nodes = "values: [";
  for (std::size_t index = 0; index < kMaxArtifactNodeCount; ++index) {
    if (index != 0U) {
      too_many_nodes.push_back(',');
    }
    too_many_nodes.push_back('x');
  }
  too_many_nodes += "]\n";
  EXPECT_THROW(parse_strict_json_model_yaml(too_many_nodes), ArtifactCodecError);

  const std::string too_large_scalar(kMaxArtifactScalarBytes + 1U, 'x');
  EXPECT_THROW(parse_strict_json_model_yaml("value: \"" + too_large_scalar + "\"\n"),
               ArtifactCodecError);

  const std::string too_many_bytes(kMaxArtifactEncodedBytes + 1U, 'x');
  EXPECT_THROW(parse_strict_json_model_yaml(too_many_bytes), ArtifactCodecError);
}

TEST(ArtifactCodec, AppliesDedicatedLimitToAggregatePreparationEnvelope) {
  constexpr std::size_t kRows = 8193U;
  constexpr std::size_t kScalarBytes = 2048U;
  const std::string scalar(kScalarBytes, 'x');
  std::string request = "[";
  for (std::size_t index = 0U; index < kRows; ++index) {
    if (index != 0U) request.push_back(',');
    request.push_back('"');
    request += scalar;
    request.push_back('"');
  }
  request.push_back(']');
  ASSERT_GT(request.size(), kMaxArtifactEncodedBytes);
  ASSERT_LT(request.size(), kMaxActivationPreparationRequestBytes);

  EXPECT_THROW(parse_strict_json_model_yaml(request), ArtifactCodecError);
  const auto envelope = parse_strict_json_model_yaml(
      request, kMaxActivationPreparationRequestBytes);
  EXPECT_EQ(envelope.root().as_array().size(), kRows);
}

TEST(ArtifactCodec, HandlesNearLimitWideMappingWithBoundedKeyWork) {
  constexpr std::size_t kMemberCount = 120000U;
  const auto yaml = wide_mapping(kMemberCount, false);

  const auto started = std::chrono::steady_clock::now();
  const auto candidate = parse_strict_json_model_yaml(yaml);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  const auto& object = candidate.root().as_object();
  ASSERT_EQ(object.size(), kMemberCount);
  EXPECT_EQ(object.front().first, "key_000000");
  EXPECT_EQ(object.back().first, "key_119999");
  std::cout << "[ metric ] wide mapping members=" << kMemberCount << " parse_ms=" << elapsed.count()
            << '\n';
}

TEST(ArtifactCodec, RejectsNearLimitTrailingDuplicateWithBoundedKeyWork) {
  constexpr std::size_t kMemberCount = 120000U;
  const auto yaml = wide_mapping(kMemberCount, true);

  const auto started = std::chrono::steady_clock::now();
  EXPECT_THROW(parse_strict_json_model_yaml(yaml), ArtifactCodecError);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  std::cout << "[ metric ] trailing duplicate after " << kMemberCount
            << " unique members rejection_ms=" << elapsed.count() << '\n';
}

}  // namespace
}  // namespace manyforge::deployment
