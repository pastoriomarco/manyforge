// Unit tests for ContentHasher: determinism, field boundaries, order
// sensitivity, finalize-without-consume, reset.

#include <manyforge_core/types/content_hash.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace manyforge::core {
namespace {

TEST(ContentHasher, EmptyHasherHasOffsetBasisDigest) {
  ContentHasher h;
  // Right-justified 16-char hex of FNV-1a 64-bit offset basis.
  EXPECT_EQ(h.finalize(), "cbf29ce484222325");
}

TEST(ContentHasher, IdenticalInputsProduceIdenticalDigests) {
  ContentHasher a;
  a.mix("hello").mix("world").mix(static_cast<std::int64_t>(42));
  ContentHasher b;
  b.mix("hello").mix("world").mix(static_cast<std::int64_t>(42));
  EXPECT_EQ(a.finalize(), b.finalize());
}

TEST(ContentHasher, FieldBoundariesPreventBoundaryCollision) {
  // Without field boundaries "ab" + "c" and "a" + "bc" would hash identically.
  ContentHasher a;
  a.mix("ab").mix("c");
  ContentHasher b;
  b.mix("a").mix("bc");
  EXPECT_NE(a.finalize(), b.finalize());
}

TEST(ContentHasher, FieldBoundariesHandleEmbeddedDelimiterBytes) {
  std::string a_left{"a"};
  a_left.push_back(static_cast<char>(0xff));
  std::string b_right;
  b_right.push_back(static_cast<char>(0xff));
  b_right.push_back('b');
  ContentHasher a;
  a.mix(a_left).mix("b");
  ContentHasher b;
  b.mix("a").mix(b_right);
  EXPECT_NE(a.finalize(), b.finalize());
}

TEST(ContentHasher, TypeTagsSeparateStringAndIntegerFields) {
  ContentHasher a;
  a.mix(std::string("\x01\x00\x00\x00\x00\x00\x00\x00", 8));
  ContentHasher b;
  b.mix(static_cast<std::uint64_t>(1));
  EXPECT_NE(a.finalize(), b.finalize());
}

TEST(ContentHasher, FieldOrderMatters) {
  ContentHasher a;
  a.mix("alpha").mix("beta");
  ContentHasher b;
  b.mix("beta").mix("alpha");
  EXPECT_NE(a.finalize(), b.finalize());
}

TEST(ContentHasher, FinalizeIsNonDestructive) {
  ContentHasher h;
  h.mix("x");
  const std::string first = h.finalize();
  const std::string second = h.finalize();
  EXPECT_EQ(first, second);
  h.mix("y");
  const std::string third = h.finalize();
  EXPECT_NE(first, third);
}

TEST(ContentHasher, ResetReturnsToInitialState) {
  ContentHasher h;
  h.mix("noise");
  h.reset();
  EXPECT_EQ(h.finalize(), "cbf29ce484222325");
}

TEST(ContentHasher, DigestIsFixedWidthHex) {
  ContentHasher h;
  h.mix("any");
  const std::string d = h.finalize();
  EXPECT_EQ(d.size(), 16u);
  for (char c : d) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
        << "digest contains non-hex char: " << c;
  }
}

TEST(ContentHasher, IntegerMixingIsBitExact) {
  ContentHasher a;
  a.mix(static_cast<std::int64_t>(1));
  ContentHasher b;
  b.mix(static_cast<std::int64_t>(2));
  EXPECT_NE(a.finalize(), b.finalize());
}

TEST(ContentHasher, DoubleMixingDistinguishesValues) {
  ContentHasher a;
  a.mix(1.0);
  ContentHasher b;
  b.mix(1.0000000000001);
  EXPECT_NE(a.finalize(), b.finalize());
}

}  // namespace
}  // namespace manyforge::core
