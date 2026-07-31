#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::content_identity {
namespace {

[[nodiscard]] double double_from_bits(std::uint64_t bits) {
  return std::bit_cast<double>(bits);
}

TEST(CanonicalValue, SerializesPublishedRfc8785Example) {
  const CanonicalValue value(CanonicalValue::Object{
      {"numbers",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(333333333.33333329), CanonicalValue(1e30),
           CanonicalValue(4.50), CanonicalValue(2e-3),
           CanonicalValue(1e-27)})},
      {"string", CanonicalValue("€$\u000f\nA'B\"\\\\\"/")},
      {"literals",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(nullptr), CanonicalValue(true),
           CanonicalValue(false)})},
  });

  EXPECT_EQ(
      canonical_json(value),
      R"({"literals":[null,true,false],"numbers":[333333333.3333333,1e+30,4.5,0.002,1e-27],"string":"€$\u000f\nA'B\"\\\\\"/"})");
}

TEST(CanonicalValue, UsesUtf16ObjectOrderingAndRejectsInvalidValues) {
  const CanonicalValue value(CanonicalValue::Object{
      {"€", CanonicalValue("Euro")},
      {"\r", CanonicalValue("Carriage Return")},
      {"\ue000", CanonicalValue("Private Use")},
      {"1", CanonicalValue("One")},
      {"😀", CanonicalValue("Emoji")},
      {"\u0080", CanonicalValue("Control")},
      {"ö", CanonicalValue("Latin")},
  });
  EXPECT_EQ(
      canonical_json(value),
      R"({"\r":"Carriage Return","1":"One","":"Control","ö":"Latin","€":"Euro","😀":"Emoji","":"Private Use"})");

  EXPECT_THROW(CanonicalValue(-0.0), CanonicalValueError);
  EXPECT_THROW(CanonicalValue(std::numeric_limits<double>::infinity()),
               CanonicalValueError);
  EXPECT_THROW(CanonicalValue(std::string("\xc3\x28", 2)),
               CanonicalValueError);
  EXPECT_THROW(CanonicalValue(CanonicalValue::Object{
                   {"duplicate", CanonicalValue(true)},
                   {"duplicate", CanonicalValue(false)},
               }),
               CanonicalValueError);
}

TEST(CanonicalValue, SerializesOfficialRfc8785NumberVectors) {
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
    EXPECT_EQ(canonical_json(CanonicalValue(double_from_bits(bits))), expected);
  }
}

TEST(ContentIdentity, HashesExactBytesAndFramesNormalizedPayload) {
  EXPECT_EQ(
      sha256_digest(""),
      "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(
      sha256_digest("abc"),
      "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  const CanonicalValue payload(CanonicalValue::Object{
      {"bootstrap_generation", CanonicalValue(1.0)},
      {"bootstrap_id", CanonicalValue("bootstrap-alpha")},
      {"cell_id", CanonicalValue("cell-alpha")},
  });
  EXPECT_EQ(
      framed_sha256("scene-id", "manyforge.logical_scene.v1", payload),
      "sha256:1702618655fff8c8b8f86181440bc94fb28d0a7853113bb5d162ebbe1d71be17");
  EXPECT_NE(framed_sha256("other-domain", "manyforge.logical_scene.v1",
                          payload),
            framed_sha256("scene-id", "manyforge.logical_scene.v1",
                          payload));
  EXPECT_THROW(framed_sha256("", "manyforge.logical_scene.v1", payload),
               CanonicalValueError);
  EXPECT_THROW(framed_sha256("scene-id", "bad\napi", payload),
               CanonicalValueError);
}

TEST(ContentIdentity, ValidatesAndComparesExactDigestSpelling) {
  constexpr std::string_view digest =
      "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  EXPECT_TRUE(is_sha256_digest(digest));
  EXPECT_TRUE(digest_equal(digest, digest));
  EXPECT_FALSE(is_sha256_digest(
      "sha256:BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
  EXPECT_FALSE(digest_equal(digest, "sha256:1234"));
}

}  // namespace
}  // namespace manyforge::content_identity
