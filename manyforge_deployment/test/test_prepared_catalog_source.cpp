#include <gtest/gtest.h>

#include <manyforge_deployment/content_identity.hpp>

#include "prepared_catalog_source_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

TEST(PreparedCatalogSourceTest, ReadsOnlyExactCapturedKeyDigestAndSize) {
  const std::string bytes = "captured catalog bytes";
  const core::ContentAddressedStoreKey key("opaque/key");
  const core::ContentDigest digest(sha256_digest(bytes));
  const std::vector<PreparedActivationArtifact> values{{key, digest, bytes}};
  const PreparedCatalogSource source(values);

  ASSERT_TRUE(source.get_verified(key, digest, bytes.size()).has_value());
  EXPECT_EQ(*source.get_verified(key, digest, bytes.size()), bytes);
  EXPECT_FALSE(source.get_verified(core::ContentAddressedStoreKey("missing"), digest, {}).has_value());
  EXPECT_FALSE(source.get_verified(key, core::ContentDigest(sha256_digest("other")), {}).has_value());
  EXPECT_FALSE(source.get_verified(key, digest, bytes.size() + 1U).has_value());

  const std::vector<PreparedActivationArtifact> corrupt{{key, digest, "altered"}};
  EXPECT_FALSE(PreparedCatalogSource(corrupt).get_verified(key, digest, {}).has_value());
}

}  // namespace
}  // namespace manyforge::deployment::detail
