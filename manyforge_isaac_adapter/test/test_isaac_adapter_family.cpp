#include <gtest/gtest.h>

#include <manyforge_roboplan/component_adapter_registry.hpp>
#include <manyforge_roboplan/ros2_control_lane_family.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

namespace manyforge::isaac_adapter {
namespace {

using roboplan_backend::ComponentAdapterContext;
using roboplan_backend::ComponentAdapterRegistry;

class IdleAdapter final : public core::IComponentAdapter {
 public:
  [[nodiscard]] core::ComponentState State(std::int64_t now_ns) const override {
    core::ComponentState state;
    state.observed_at_ns = now_ns;
    return state;
  }
  [[nodiscard]] core::ActionResult Run(core::ComponentAction, std::stop_token) override {
    return {};
  }
  [[nodiscard]] std::vector<core::Capability> capabilities() const override { return {}; }
};

// CONTRACTS §6: the execution host reaches Isaac only through the
// registry: the library registered under `isaac` exports the family with its
// launch contribution and its ros2_control lane hooks (a simulator lane).
TEST(IsaacAdapterFamily, LoadsFromTheIndexWithItsLaneHooks) {
  const std::filesystem::path library(MANYFORGE_ISAAC_ADAPTER_FAMILY_LIBRARY);
  const auto prefix = std::filesystem::temp_directory_path() /
                      ("manyforge_isaac_family_index_" + std::to_string(::getpid()));
  std::filesystem::remove_all(prefix);
  const auto index = prefix / "share" / "ament_index" / "resource_index" /
                     std::string(roboplan_backend::kComponentAdapterResourceType);
  std::filesystem::create_directories(index);
  std::filesystem::create_directories(prefix / "lib");
  std::ofstream(index / "isaac") << library.filename().string() << '\n';
  std::filesystem::create_symlink(library, prefix / "lib" / library.filename());
  ::setenv("AMENT_PREFIX_PATH", prefix.c_str(), 1);

  auto loaded = ComponentAdapterRegistry::LoadInstalled();
  ASSERT_TRUE(loaded.ok()) << loaded.error().message;
  const auto *family = loaded.value().Find("isaac");
  ASSERT_NE(family, nullptr);
  EXPECT_NE(family->launch_contribution, nullptr);
  ASSERT_NE(family->lane, nullptr);
  EXPECT_TRUE(family->lane->simulated);
  EXPECT_TRUE(family->lane->seeds_initial_pose);  // the Deployment's initial joints
  EXPECT_NE(family->lane->arm_backend_config, nullptr);
  EXPECT_NE(family->lane->tool, nullptr);

  // Isaac has no vendor steps: its adapter is the lane adapter.
  ComponentAdapterContext context{core::ControllableComponentId("robot"), {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, false, {}, {}, {}};
  context.lane = "isaac";
  context.row.adapter = "isaac";
  context.lane_adapter = std::make_shared<IdleAdapter>();
  const auto made = loaded.value().Make(context);
  ASSERT_TRUE(made.ok()) << made.error().message;
  EXPECT_EQ(made.value().component, context.lane_adapter);
  EXPECT_EQ(made.value().owner_handle, nullptr);
  EXPECT_FALSE(made.value().tool_context.has_value());
}

}  // namespace
}  // namespace manyforge::isaac_adapter
