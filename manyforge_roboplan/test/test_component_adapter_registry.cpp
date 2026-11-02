#include <gtest/gtest.h>

#include <manyforge_roboplan/component_adapter_registry.hpp>
#include <manyforge_roboplan/controller_launch_artifacts.hpp>
#include <manyforge_roboplan/ros2_control_lane_family.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <unistd.h>
#include <string>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

// CONTRACTS §6: the core keeps no list of vendors or lanes; the registry
// serves whatever families are installed. These are in-process families.
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

/// An owner returns a handle; a member gets it back through its context.
ComponentAdapters MakeMachine(const ComponentAdapterContext &context) {
  if (context.machine_owner)
    return {std::make_shared<IdleAdapter>(), std::make_shared<std::string>("machine"),
            std::nullopt, std::nullopt};
  if (!context.role.empty() &&
      (!context.owner || *std::static_pointer_cast<std::string>(context.owner) != "machine"))
    throw std::invalid_argument("a member needs its owner's handle");
  return {std::make_shared<IdleAdapter>(), nullptr, std::nullopt, std::nullopt};
}

ComponentAdapters MakeRefusing(const ComponentAdapterContext &) {
  throw std::invalid_argument("the row names no driver");
}

LaunchContribution Contribute(const ModelLaneRow &row, const bool arm) {
  LaunchContribution contribution;
  contribution.hardware.plugin = row.adapter + (arm ? "/arm" : "/tool");
  contribution.controllers_yaml = "controllers";
  return contribution;
}

/// A vendor family's controllers for a model row (Mock and Isaac rows of its
/// models run them).
std::string VendorControllers(const ModelLaneRow &row) {
  return "vendor controllers of " + row.adapter_keys.at("vendor_model");
}

/// A lane family: its hardware, and the vendor's controllers when the row
/// names a vendor.
LaunchContribution LaneContribute(const ModelLaneRow &row, const bool arm) {
  auto contribution = Contribute(row, arm);
  contribution.controllers_yaml = row.vendor_controllers_yaml.value_or("lane controllers");
  return contribution;
}

[[nodiscard]] ComponentAdapterRegistry Registry() {
  return ComponentAdapterRegistry({{"vendor", &MakeMachine, &Contribute, nullptr, &VendorControllers},
                                   {"refusing", &MakeRefusing, nullptr, nullptr, nullptr},
                                   {"lane", &MakeMachine, &LaneContribute, nullptr, nullptr}});
}

[[nodiscard]] ComponentAdapterContext Context(const std::string &family) {
  ComponentAdapterContext context{core::ControllableComponentId("robot"), {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, false, {}, {}, {}};
  context.lane = "real";
  context.row.adapter = family;
  return context;
}

TEST(ComponentAdapterRegistry, UnknownFamilyIsRefused) {
  const auto made = Registry().Make(Context("absent"));
  ASSERT_FALSE(made.ok());
  EXPECT_EQ(made.error().code, "component_adapter_unknown");
  EXPECT_EQ(made.error().details.at("adapter_family"), "absent");
}

TEST(ComponentAdapterRegistry, FamilyRefusalIsInvalidAdapterData) {
  const auto made = Registry().Make(Context("refusing"));
  ASSERT_FALSE(made.ok());
  EXPECT_EQ(made.error().code, "component_adapter_invalid");
  EXPECT_EQ(made.error().message, "the row names no driver");
}

// CONTRACTS §16: one call per component; an owner's handle reaches its
// members through their context, opaque to the host.
TEST(ComponentAdapterRegistry, AnOwnersHandleReachesItsMembers) {
  const auto registry = Registry();
  auto owner_context = Context("vendor");
  owner_context.machine_owner = true;
  const auto owner = registry.Make(owner_context);
  ASSERT_TRUE(owner.ok()) << owner.error().message;
  ASSERT_NE(owner.value().component, nullptr);
  ASSERT_NE(owner.value().owner_handle, nullptr);

  auto member_context = Context("vendor");
  member_context.role = "arm";
  member_context.owner = owner.value().owner_handle;
  const auto member = registry.Make(member_context);
  ASSERT_TRUE(member.ok()) << member.error().message;
  EXPECT_NE(member.value().component, nullptr);
  EXPECT_EQ(member.value().owner_handle, nullptr);

  member_context.owner = nullptr;
  const auto orphan = registry.Make(member_context);
  ASSERT_FALSE(orphan.ok());
  EXPECT_EQ(orphan.error().code, "component_adapter_invalid");
}

TEST(ComponentAdapterRegistry, LaunchContributionComesOnlyFromAContributingFamily) {
  const auto registry = Registry();
  ModelLaneRow row;
  row.adapter = "vendor";
  EXPECT_EQ(registry.LaunchContributionOf(row, true).hardware.plugin, "vendor/arm");
  EXPECT_EQ(registry.LaunchContributionOf(row, false).hardware.plugin, "vendor/tool");
  row.adapter = "refusing";
  EXPECT_THROW((void)registry.LaunchContributionOf(row, true), ControllerLaunchArtifactError);
  row.adapter = "absent";
  EXPECT_THROW((void)registry.LaunchContributionOf(row, true), ControllerLaunchArtifactError);
}

TEST(ComponentAdapterRegistry, AVendorRowOnALaneTakesTheVendorsControllers) {
  const auto registry = Registry();
  ModelLaneRow row;
  row.adapter = "lane";
  EXPECT_EQ(registry.LaunchContributionOf(row, true).controllers_yaml, "lane controllers");
  row.adapter_keys = {{"vendor", "vendor"}, {"vendor_model", "lite6"}};
  const auto made = registry.LaunchContributionOf(row, true);
  EXPECT_EQ(made.hardware.plugin, "lane/arm");
  EXPECT_EQ(made.controllers_yaml, "vendor controllers of lite6");
  row.adapter_keys["vendor"] = "refusing";  // supplies no controllers
  EXPECT_THROW((void)registry.LaunchContributionOf(row, true), ControllerLaunchArtifactError);
  row.adapter_keys["vendor"] = "absent";
  EXPECT_THROW((void)registry.LaunchContributionOf(row, true), ControllerLaunchArtifactError);
}

TEST(ComponentAdapterRegistry, ListsItsFamilies) {
  EXPECT_EQ(Registry().families(), (std::vector<std::string>{"lane", "refusing", "vendor"}));
}

TEST(ComponentAdapterRegistry, FindsARegisteredFamilyOnly) {
  const auto registry = Registry();
  ASSERT_NE(registry.Find("vendor"), nullptr);
  EXPECT_EQ(registry.Find("vendor")->family, "vendor");
  EXPECT_EQ(registry.Find("absent"), nullptr);
}

/// An ament prefix whose `manyforge_component_adapters` index registers the
/// given family libraries (by family id), made the only search path.
void InstallTestIndex(const std::map<std::string, std::filesystem::path> &libraries) {
  const auto prefix = std::filesystem::temp_directory_path() /
                      ("manyforge_adapter_index_" + std::to_string(::getpid()));
  std::filesystem::remove_all(prefix);
  const auto index = prefix / "share" / "ament_index" / "resource_index" /
                     std::string(kComponentAdapterResourceType);
  std::filesystem::create_directories(index);
  std::filesystem::create_directories(prefix / "lib");
  for (const auto &[family, library] : libraries) {
    std::ofstream(index / family) << library.filename().string() << '\n';
    std::filesystem::create_symlink(library, prefix / "lib" / library.filename());
  }
  ::setenv("AMENT_PREFIX_PATH", prefix.c_str(), 1);
}

// CONTRACTS §6: Mock and the simulator are families like any other, loaded
// only through the index; Mock brings its ros2_control lane hooks.
TEST(ComponentAdapterRegistry, LoadsTheMockAndSimulationFamiliesFromTheIndex) {
  InstallTestIndex({{"mock", MANYFORGE_MOCK_ADAPTER_FAMILY_LIBRARY},
                    {"simulation_interfaces", MANYFORGE_SIMULATION_ADAPTER_FAMILY_LIBRARY}});
  auto loaded = ComponentAdapterRegistry::LoadInstalled();
  ASSERT_TRUE(loaded.ok()) << loaded.error().message;
  EXPECT_EQ(loaded.value().families(),
            (std::vector<std::string>{"mock", "simulation_interfaces"}));
  const auto *mock = loaded.value().Find("mock");
  ASSERT_NE(mock, nullptr);
  ASSERT_NE(mock->launch_contribution, nullptr);
  ASSERT_NE(mock->lane, nullptr);
  EXPECT_TRUE(mock->lane->seeds_initial_pose);
  EXPECT_FALSE(mock->lane->simulated);
  EXPECT_NE(mock->lane->arm_backend_config, nullptr);
  EXPECT_NE(mock->lane->tool, nullptr);
  // Mock has no vendor steps: its adapter is the lane adapter.
  auto context = Context("mock");
  context.lane_adapter = std::make_shared<IdleAdapter>();
  const auto made = loaded.value().Make(context);
  ASSERT_TRUE(made.ok()) << made.error().message;
  EXPECT_EQ(made.value().component, context.lane_adapter);
  EXPECT_EQ(made.value().owner_handle, nullptr);
  EXPECT_FALSE(made.value().tool_context.has_value());
  const auto *simulator = loaded.value().Find("simulation_interfaces");
  ASSERT_NE(simulator, nullptr);
  EXPECT_EQ(simulator->launch_contribution, nullptr);
  EXPECT_EQ(simulator->lane, nullptr);
}

TEST(ComponentAdapterRegistry, ALibraryExportingAnotherFamilyIsRefused) {
  InstallTestIndex({{"isaac", MANYFORGE_MOCK_ADAPTER_FAMILY_LIBRARY}});
  const auto loaded = ComponentAdapterRegistry::LoadInstalled();
  ASSERT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.error().code, "component_adapter_load_failed");
}

}  // namespace
}  // namespace manyforge::roboplan_backend
