#include <gtest/gtest.h>
#include <manyforge_core/version.hpp>

TEST(VersionTest, MajorIsZero) {
  EXPECT_EQ(manyforge::core::VERSION_MAJOR, 0);
}

TEST(VersionTest, MinorIsOne) {
  EXPECT_EQ(manyforge::core::VERSION_MINOR, 1);
}
