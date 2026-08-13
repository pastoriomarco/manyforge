#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::content_identity {
namespace {

[[nodiscard]] double double_from_bits(std::uint64_t bits) { return std::bit_cast<double>(bits); }

TEST(CanonicalValue, SerializesPublishedRfc8785Example) {
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
  EXPECT_THROW(CanonicalValue(std::numeric_limits<double>::infinity()), CanonicalValueError);
  EXPECT_THROW(CanonicalValue(std::string("\xc3\x28", 2)), CanonicalValueError);
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
  for (const auto &[bits, expected] : vectors) {
    EXPECT_EQ(canonical_json(CanonicalValue(double_from_bits(bits))), expected);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(parse_canonical_json(expected).as_number()), bits);
  }
}

TEST(CanonicalValue, ExposesClosedConstTypeAccessors) {
  const CanonicalValue null_value(nullptr);
  const CanonicalValue boolean(true);
  const CanonicalValue number(1.5);
  const CanonicalValue string("value");
  const CanonicalValue array(CanonicalValue::Array{CanonicalValue(false)});
  const CanonicalValue object(CanonicalValue::Object{{"member", CanonicalValue(2.0)}});

  EXPECT_TRUE(null_value.is_null());
  EXPECT_EQ(null_value.as_null(), nullptr);
  EXPECT_TRUE(boolean.is_bool());
  EXPECT_TRUE(boolean.as_bool());
  EXPECT_TRUE(number.is_number());
  EXPECT_EQ(number.as_number(), 1.5);
  EXPECT_TRUE(string.is_string());
  EXPECT_EQ(string.as_string(), "value");
  EXPECT_TRUE(array.is_array());
  ASSERT_EQ(array.as_array().size(), 1U);
  EXPECT_FALSE(array.as_array().front().as_bool());
  EXPECT_TRUE(object.is_object());
  ASSERT_EQ(object.as_object().size(), 1U);
  EXPECT_EQ(object.as_object().front().second.as_number(), 2.0);

  EXPECT_THROW(static_cast<void>(number.as_null()), CanonicalValueError);
  EXPECT_THROW(static_cast<void>(number.as_bool()), CanonicalValueError);
  EXPECT_THROW(static_cast<void>(number.as_string()), CanonicalValueError);
  EXPECT_THROW(static_cast<void>(number.as_array()), CanonicalValueError);
  EXPECT_THROW(static_cast<void>(number.as_object()), CanonicalValueError);
}

TEST(CanonicalJsonParser, ParsesOneBoundedCanonicalValueAndRoundTripsExactBytes) {
  constexpr std::string_view document =
      R"({"a":[null,true,false,0,1.5,"text"],"emoji":"😀","z":{"x":1e+30}})";
  const auto parsed = parse_canonical_json(document);
  EXPECT_EQ(canonical_json(parsed), document);
  ASSERT_TRUE(parsed.is_object());
  const auto *values = parsed.find_object_member("a");
  ASSERT_NE(values, nullptr);
  ASSERT_TRUE(values->is_array());
  ASSERT_EQ(values->as_array().size(), 6U);
  EXPECT_TRUE(values->as_array()[0].is_null());
  EXPECT_TRUE(values->as_array()[1].as_bool());
  EXPECT_FALSE(values->as_array()[2].as_bool());
  EXPECT_EQ(values->as_array()[3].as_number(), 0.0);
  EXPECT_EQ(values->as_array()[4].as_number(), 1.5);
  EXPECT_EQ(values->as_array()[5].as_string(), "text");
}

TEST(CanonicalJsonParser, RejectsNoncanonicalWhitespaceOrderingEscapesAndNumbers) {
  const std::vector<std::string> noncanonical{
      " null",       "null\n",  "[1, 2]", R"({"a": 1})", R"({"b":1,"a":2})",
      R"("\u0061")", R"("\/")", "1.0",    "1E+1",        "1e0",
      "-0",          "-0.0",    "0e-999", "01"};
  for (const auto &document : noncanonical) {
    EXPECT_THROW(parse_canonical_json(document), CanonicalValueError) << document;
  }
}

TEST(CanonicalJsonParser, RejectsDuplicateNamesMalformedEscapesUtf8AndTrailingBytes) {
  const std::vector<std::string> malformed{R"({"a":1,"a":2})",
                                           R"("\x")",
                                           R"("\u12")",
                                           R"("\uD800")",
                                           R"({"a":1,"\u0061":2})",
                                           R"("\uD800\u0041")",
                                           R"("\uDC00")",
                                           "truefalse",
                                           "{}x",
                                           "",
                                           "[1,]",
                                           R"({"a" 1})",
                                           "Infinity",
                                           "NaN",
                                           "1e999"};
  for (const auto &document : malformed) {
    EXPECT_THROW(parse_canonical_json(document), CanonicalValueError) << document;
  }

  EXPECT_THROW(parse_canonical_json(std::string("\"\xc3\x28\"", 4U)), CanonicalValueError);
  EXPECT_THROW(parse_canonical_json(std::string("\"e\xCC\x81\"", 5U)), CanonicalValueError);
}

TEST(CanonicalJsonParser, EnforcesByteDepthAndNodeLimitsAndFixedCeilings) {
  EXPECT_EQ(parse_canonical_json("null", CanonicalJsonParseLimits{4U, 1U, 1U}).as_null(), nullptr);
  EXPECT_THROW(parse_canonical_json("null", CanonicalJsonParseLimits{3U, 1U, 1U}),
               CanonicalValueError);
  EXPECT_THROW(parse_canonical_json("[0]", CanonicalJsonParseLimits{3U, 1U, 2U}),
               CanonicalValueError);
  EXPECT_THROW(parse_canonical_json("[0]", CanonicalJsonParseLimits{3U, 2U, 1U}),
               CanonicalValueError);
  EXPECT_EQ(parse_canonical_json("[0]", CanonicalJsonParseLimits{3U, 2U, 2U}).as_array().size(),
            1U);

  EXPECT_THROW(parse_canonical_json(
                   "null", CanonicalJsonParseLimits{kMaximumCanonicalJsonBytes + 1U, 1U, 1U}),
               CanonicalValueError);
  EXPECT_THROW(parse_canonical_json(
                   "null", CanonicalJsonParseLimits{4U, kMaximumCanonicalJsonDepth + 1U, 1U}),
               CanonicalValueError);
  EXPECT_THROW(parse_canonical_json(
                   "null", CanonicalJsonParseLimits{4U, 1U, kMaximumCanonicalJsonNodes + 1U}),
               CanonicalValueError);
  EXPECT_THROW(parse_canonical_json("null", CanonicalJsonParseLimits{0U, 1U, 1U}),
               CanonicalValueError);
}

TEST(CanonicalJsonParser, AcceptsCanonicalUtf16KeyOrderAndRequiredControlEscapes) {
  constexpr std::string_view document =
      R"({"\r":"control","1":"one","value":"\b\t\n\f\r\u0000\u001f","ö":"latin","€":"euro","😀":"emoji","":"private"})";
  const auto parsed = parse_canonical_json(document);
  EXPECT_EQ(canonical_json(parsed), document);
}

TEST(ContentIdentity, HashesExactBytesAndFramesNormalizedPayload) {
  EXPECT_EQ(sha256_digest(""),
            "sha256:"
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256_digest("abc"),
            "sha256:"
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  const CanonicalValue payload(CanonicalValue::Object{
      {"bootstrap_generation", CanonicalValue(1.0)},
      {"bootstrap_id", CanonicalValue("bootstrap-alpha")},
      {"cell_id", CanonicalValue("cell-alpha")},
  });
  EXPECT_EQ(framed_sha256("scene-id", "manyforge.logical_scene.v1", payload),
            "sha256:"
            "1702618655fff8c8b8f86181440bc94fb28d0a7853113bb5d162ebbe1d71be17");
  EXPECT_NE(framed_sha256("other-domain", "manyforge.logical_scene.v1", payload),
            framed_sha256("scene-id", "manyforge.logical_scene.v1", payload));
  EXPECT_THROW(framed_sha256("", "manyforge.logical_scene.v1", payload), CanonicalValueError);
  EXPECT_THROW(framed_sha256("scene-id", "bad\napi", payload), CanonicalValueError);
}

TEST(ContentIdentity, ValidatesAndComparesExactDigestSpelling) {
  constexpr std::string_view digest =
      "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  EXPECT_TRUE(is_sha256_digest(digest));
  EXPECT_TRUE(digest_equal(digest, digest));
  EXPECT_FALSE(
      is_sha256_digest("sha256:"
                       "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
  EXPECT_FALSE(digest_equal(digest, "sha256:1234"));
}

}  // namespace
}  // namespace manyforge::content_identity
