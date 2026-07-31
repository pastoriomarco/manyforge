#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "content_identity_internal.hpp"
#include "manyforge_deployment/canonical_value.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment {
namespace {

using detail::catalog_record_identity_domain;
using detail::compute_content_identity;
using detail::ContentIdentityRootShape;
using detail::ContentIdentitySpec;
using detail::verify_content_identity;

[[nodiscard]] double double_from_bits(std::uint64_t bits) {
  return std::bit_cast<double>(bits);
}

TEST(CanonicalValueTest, SerializesRfc8785PrimitiveExample) {
  const CanonicalValue value(CanonicalValue::Object{
      {"numbers", CanonicalValue(CanonicalValue::Array{
                      CanonicalValue(333333333.33333329), CanonicalValue(1e30),
                      CanonicalValue(4.50), CanonicalValue(2e-3), CanonicalValue(1e-27)})},
      {"string", CanonicalValue("€$\u000f\nA'B\"\\\\\"/")},
      {"literals", CanonicalValue(CanonicalValue::Array{
                       CanonicalValue(nullptr), CanonicalValue(true), CanonicalValue(false)})},
  });

  EXPECT_EQ(
      canonical_json(value),
      R"({"literals":[null,true,false],"numbers":[333333333.3333333,1e+30,4.5,0.002,1e-27],"string":"€$\u000f\nA'B\"\\\\\"/"})");
}

TEST(CanonicalValueTest, SortsObjectNamesByUtf16CodeUnits) {
  // RFC 8785's published sort fixture uses U+FB33, which is not NFC and is
  // therefore invalid under ManyForge's stronger input contract. U+E000 keeps
  // the fixture's important supplementary-before-high-BMP ordering case.
  const CanonicalValue value(CanonicalValue::Object{
      {"€", CanonicalValue("Euro Sign")},
      {"\r", CanonicalValue("Carriage Return")},
      {"\ue000", CanonicalValue("Private Use")},
      {"1", CanonicalValue("One")},
      {"😀", CanonicalValue("Emoji: Grinning Face")},
      {"\u0080", CanonicalValue("Control")},
      {"ö", CanonicalValue("Latin Small Letter O With Diaeresis")},
  });

  EXPECT_EQ(
      canonical_json(value),
      R"({"\r":"Carriage Return","1":"One","":"Control","ö":"Latin Small Letter O With Diaeresis","€":"Euro Sign","😀":"Emoji: Grinning Face","":"Private Use"})");

  const CanonicalValue same_members_different_order(CanonicalValue::Object{
      {"ö", CanonicalValue("Latin Small Letter O With Diaeresis")},
      {"\u0080", CanonicalValue("Control")},
      {"😀", CanonicalValue("Emoji: Grinning Face")},
      {"1", CanonicalValue("One")},
      {"\ue000", CanonicalValue("Private Use")},
      {"\r", CanonicalValue("Carriage Return")},
      {"€", CanonicalValue("Euro Sign")},
  });
  EXPECT_EQ(value, same_members_different_order);
}

TEST(CanonicalValueTest, SerializesOfficialRfc8785NumberVectors) {
  const std::vector<std::pair<std::uint64_t, std::string>> vectors{
      {0x0000000000000000ULL, "0"},
      {0x0000000000000001ULL, "5e-324"},
      {0x8000000000000001ULL, "-5e-324"},
      {0x7fefffffffffffffULL, "1.7976931348623157e+308"},
      {0xffefffffffffffffULL, "-1.7976931348623157e+308"},
      {0x4340000000000000ULL, "9007199254740992"},
      {0xc340000000000000ULL, "-9007199254740992"},
      {0x4430000000000000ULL, "295147905179352830000"},
      {0x44b52d02c7e14af5ULL, "9.999999999999997e+22"},
      {0x44b52d02c7e14af6ULL, "1e+23"},
      {0x44b52d02c7e14af7ULL, "1.0000000000000001e+23"},
      {0x444b1ae4d6e2ef4eULL, "999999999999999700000"},
      {0x444b1ae4d6e2ef4fULL, "999999999999999900000"},
      {0x444b1ae4d6e2ef50ULL, "1e+21"},
      {0x3eb0c6f7a0b5ed8cULL, "9.999999999999997e-7"},
      {0x3eb0c6f7a0b5ed8dULL, "0.000001"},
      {0x41b3de4355555553ULL, "333333333.3333332"},
      {0x41b3de4355555554ULL, "333333333.33333325"},
      {0x41b3de4355555555ULL, "333333333.3333333"},
      {0x41b3de4355555556ULL, "333333333.3333334"},
      {0x41b3de4355555557ULL, "333333333.33333343"},
      {0xbecbf647612f3696ULL, "-0.0000033333333333333333"},
      {0x43143ff3c1cb0959ULL, "1424953923781206.2"},
  };

  for (const auto& [bits, expected] : vectors) {
    EXPECT_EQ(canonical_json(CanonicalValue(double_from_bits(bits))), expected)
        << "IEEE-754 bits: 0x" << std::hex << bits;
  }
}

TEST(CanonicalValueTest, RejectsInvalidNumbers) {
  EXPECT_THROW(CanonicalValue(-0.0), CanonicalValueError);
  EXPECT_THROW(CanonicalValue(std::numeric_limits<double>::infinity()), CanonicalValueError);
  EXPECT_THROW(CanonicalValue(-std::numeric_limits<double>::infinity()), CanonicalValueError);
  EXPECT_THROW(CanonicalValue(std::numeric_limits<double>::quiet_NaN()), CanonicalValueError);
}

TEST(CanonicalValueTest, RejectsInvalidOrNonNfcStringsAndDuplicateNames) {
  EXPECT_FALSE(is_utf8_nfc(std::string("\xc3\x28", 2)));
  EXPECT_THROW(CanonicalValue(std::string("\xc3\x28", 2)), CanonicalValueError);

  const std::string decomposed = "e\xcc\x81";
  EXPECT_FALSE(is_utf8_nfc(decomposed));
  EXPECT_THROW(static_cast<void>(CanonicalValue{decomposed}), CanonicalValueError);
  EXPECT_TRUE(is_utf8_nfc("é"));

  EXPECT_THROW(CanonicalValue(CanonicalValue::Object{
                   {"duplicate", CanonicalValue(true)},
                   {"duplicate", CanonicalValue(false)},
               }),
               CanonicalValueError);
}

TEST(ContentIdentityTest, ComputesAndVerifiesDomainSeparatedIdentity) {
  const ContentIdentitySpec spec{
      .identity_domain = "graph-revision",
      .api_version = "manyforge.cell_graph.v1",
      .own_field = "graph_revision",
  };
  CanonicalValue value(CanonicalValue::Object{
      {"graph_revision",
       CanonicalValue("sha256:0000000000000000000000000000000000000000000000000000000000000000")},
      {"api_version", CanonicalValue("manyforge.cell_graph.v1")},
      {"cell_id", CanonicalValue("cell_a")},
      {"runtime_units", CanonicalValue(CanonicalValue::Array{})},
      {"model_instances", CanonicalValue(CanonicalValue::Array{})},
      {"controllable_components", CanonicalValue(CanonicalValue::Array{})},
      {"control_domains", CanonicalValue(CanonicalValue::Array{})},
      {"control_bindings", CanonicalValue(CanonicalValue::Array{})},
      {"component_attachments", CanonicalValue(CanonicalValue::Array{})},
  });

  const std::string identity = compute_content_identity(value, spec);
  EXPECT_EQ(identity, "sha256:443285fa98d7408c9249b581a8802f498bbe1501486bd579cc674be7a6b2cb34");
  EXPECT_FALSE(verify_content_identity(value, spec));

  CanonicalValue::Object matching_members = value.as_object();
  for (auto& [name, member] : matching_members) {
    if (name == "graph_revision") {
      member = CanonicalValue(identity);
    }
  }
  const CanonicalValue matching(std::move(matching_members));
  EXPECT_TRUE(verify_content_identity(matching, spec));

  const ContentIdentitySpec other_domain{
      .identity_domain = "deployment-revision",
      .api_version = spec.api_version,
      .own_field = spec.own_field,
  };
  EXPECT_NE(compute_content_identity(value, spec), compute_content_identity(value, other_domain));
}

TEST(ContentIdentityTest, ComputesNestedRecordIdentityFromExternalDocumentSchemaTag) {
  const ContentIdentitySpec spec{
      .identity_domain = "control-domain-revision",
      .api_version = "manyforge.cell_graph.v1",
      .own_field = "control_domain_revision",
      .root_shape = ContentIdentityRootShape::kNestedRecord,
  };
  CanonicalValue value(CanonicalValue::Object{
      {"control_domain_id", CanonicalValue("domain_a")},
      {"control_domain_revision",
       CanonicalValue("sha256:0000000000000000000000000000000000000000000000000000000000000000")},
      {"runtime_unit_ids",
       CanonicalValue(CanonicalValue::Array{CanonicalValue("runtime_a")})},
      {"control_coordinator_endpoint_role", CanonicalValue("control_coordinator")},
      {"controller_topology_id", CanonicalValue("topology_a")},
      {"topology_revision",
       CanonicalValue("sha256:1111111111111111111111111111111111111111111111111111111111111111")},
      {"containment_granularity", CanonicalValue("control_domain")},
  });

  const std::string identity = compute_content_identity(value, spec);
  EXPECT_EQ(identity, "sha256:9b611e3795858764a013af3690a674875b796f0f4ee882cbb3f6abc5a7aa5eeb");
  EXPECT_FALSE(verify_content_identity(value, spec));

  CanonicalValue::Object matching_members = value.as_object();
  for (auto& [name, member] : matching_members) {
    if (name == "control_domain_revision") {
      member = CanonicalValue(identity);
    }
  }
  EXPECT_TRUE(verify_content_identity(CanonicalValue(std::move(matching_members)), spec));

  const ContentIdentitySpec nested_schema_fragment{
      .identity_domain = spec.identity_domain,
      .api_version = "manyforge.cell_graph.v1#control_domain",
      .own_field = spec.own_field,
      .root_shape = ContentIdentityRootShape::kNestedRecord,
  };
  EXPECT_NE(compute_content_identity(value, spec),
            compute_content_identity(value, nested_schema_fragment));
}

TEST(ContentIdentityTest, ComputesLogicalSceneLineageGoldenThroughQualifiedPrimitive) {
  const ContentIdentitySpec spec{
      .identity_domain = "scene-id",
      .api_version = "manyforge.logical_scene.v1",
      .own_field = "scene_id",
      .root_shape = ContentIdentityRootShape::kNestedRecord,
  };
  const CanonicalValue root(CanonicalValue::Object{
      {"scene_id",
       CanonicalValue(
           "sha256:0000000000000000000000000000000000000000000000000000000000000000")},
      {"cell_id", CanonicalValue("cell-alpha")},
      {"bootstrap_id", CanonicalValue("bootstrap-alpha")},
      {"bootstrap_generation", CanonicalValue(1.0)},
  });

  EXPECT_EQ(
      compute_content_identity(root, spec),
      "sha256:1702618655fff8c8b8f86181440bc94fb28d0a7853113bb5d162ebbe1d71be17");
}

TEST(ContentIdentityTest, EnforcesDocumentAndNestedRecordRootShapes) {
  const ContentIdentitySpec document_spec{
      .identity_domain = "graph-revision",
      .api_version = "manyforge.cell_graph.v1",
      .own_field = "graph_revision",
  };
  const ContentIdentitySpec nested_spec{
      .identity_domain = "control-domain-revision",
      .api_version = "manyforge.cell_graph.v1",
      .own_field = "control_domain_revision",
      .root_shape = ContentIdentityRootShape::kNestedRecord,
  };
  const std::string zero_digest =
      "sha256:0000000000000000000000000000000000000000000000000000000000000000";

  EXPECT_THROW(
      compute_content_identity(
          CanonicalValue(CanonicalValue::Object{
              {"graph_revision", CanonicalValue(zero_digest)},
          }),
          document_spec),
      CanonicalValueError);
  EXPECT_THROW(
      compute_content_identity(
          CanonicalValue(CanonicalValue::Object{
              {"api_version", CanonicalValue("manyforge.cell_graph.v1")},
              {"control_domain_revision", CanonicalValue(zero_digest)},
          }),
          nested_spec),
      CanonicalValueError);
  EXPECT_THROW(
      {
        (void)verify_content_identity(
            CanonicalValue(CanonicalValue::Object{
                {"api_version",
                 CanonicalValue("manyforge.cell_graph.v1")},
                {"graph_revision", CanonicalValue("not-a-digest")},
            }),
            document_spec);
      },
      CanonicalValueError);
}

TEST(ContentIdentityTest, HashesExactBytesAndValidatesInputs) {
  EXPECT_EQ(sha256_digest(""),
            "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256_digest("abc"),
            "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_TRUE(
      is_sha256_digest("sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  EXPECT_FALSE(
      is_sha256_digest("sha256:BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
  EXPECT_FALSE(is_sha256_digest("ba7816bf"));

  const ContentIdentitySpec spec{
      .identity_domain = "graph-revision",
      .api_version = "manyforge.cell_graph.v1",
      .own_field = "graph_revision",
  };
  EXPECT_THROW(
      compute_content_identity(CanonicalValue(CanonicalValue::Object{
                                   {"api_version", CanonicalValue("manyforge.cell_graph.v1")}}),
                               spec),
      CanonicalValueError);
  EXPECT_THROW(
      compute_content_identity(
          CanonicalValue(CanonicalValue::Object{
              {"api_version", CanonicalValue("wrong.v1")},
              {"graph_revision",
               CanonicalValue(
                   "sha256:0000000000000000000000000000000000000000000000000000000000000000")},
          }),
          spec),
      CanonicalValueError);
}

TEST(ContentIdentityTest, BuildsCatalogDomainWithoutOwningKindRegistry) {
  EXPECT_EQ(catalog_record_identity_domain("component_type"),
            "catalog-record-revision/component_type");
  EXPECT_THROW(catalog_record_identity_domain("ComponentType"), CanonicalValueError);
  EXPECT_THROW(catalog_record_identity_domain("component/type"), CanonicalValueError);
}

}  // namespace
}  // namespace manyforge::deployment
