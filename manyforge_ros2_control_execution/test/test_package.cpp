#include <manyforge_ros2_control_execution/package.hpp>

#include <gtest/gtest.h>

namespace manyforge::ros2_control_execution {
namespace {

TEST(Ros2ControlExecutionPackage, ReportsStablePackageName) {
  EXPECT_EQ(package_name(), "manyforge_ros2_control_execution");
}

}  // namespace
}  // namespace manyforge::ros2_control_execution
