#include "manyforge_component_manager/isaac_site_settings.hpp"

#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::component_manager {
namespace {

std::string Site(std::string_view startup = "60",
                 std::string_view service = "60") {
  return R"({"schema":"manyforge.isaac_site.v1","scene_startup_timeout_s":)" +
         std::string(startup) + R"(,"scene_service_timeout_s":)" +
         std::string(service) +
         R"(,"worlds":{"custom_scene":{"world_uri":"file:/Isaac Host/Scene.usda","expected_world_name":"Scene Name"}},"pose_observers_by_cell":{"cell":{"site_config":" /site/observer config.json ","additional_required_topics":["/z","/a","/single,entry"," /tf "]}}})";
}

std::string Replace(std::string value, std::string_view before,
                    std::string_view after) {
  const auto position = value.find(before);
  EXPECT_NE(position, std::string::npos);
  if (position != std::string::npos)
    value.replace(position, before.size(), after);
  return value;
}

TEST(IsaacSiteSettings, PreservesExplicitCallerValuesAndSiteFacts) {
  for (const auto &[startup, service] :
       std::vector<std::pair<std::string, std::string>>{{"60", "60"},
                                                        {"180", "120"}}) {
    const auto settings = ParseIsaacSiteSettings(Site(startup, service));
    EXPECT_DOUBLE_EQ(settings.scene_startup_timeout_s(), std::stod(startup));
    EXPECT_DOUBLE_EQ(settings.scene_service_timeout_s(), std::stod(service));
    const auto &world = settings.ResolveWorld("custom_scene");
    EXPECT_EQ(world.world_uri, "file:/Isaac Host/Scene.usda");
    EXPECT_EQ(world.expected_world_name, "Scene Name");
    const auto *observer = settings.FindPoseObserver("cell");
    ASSERT_NE(observer, nullptr);
    EXPECT_EQ(observer->site_config, " /site/observer config.json ");
    EXPECT_EQ(observer->additional_required_topics,
              (std::vector<std::string>{"/z", "/a", "/single,entry", " /tf "}));
    EXPECT_EQ(settings.FindPoseObserver("Cell"), nullptr);
  }
}

TEST(IsaacSiteSettings, OpenMapsResolveExactIdsAndReportUnknownWorld) {
  const auto settings = ParseIsaacSiteSettings(Site());
  try {
    static_cast<void>(settings.ResolveWorld("custom_scene_extra"));
    FAIL() << "unknown world resolved";
  } catch (const IsaacSiteSettingsError &error) {
    EXPECT_NE(std::string(error.what()).find("custom_scene_extra"),
              std::string::npos);
  }
  const auto empty = ParseIsaacSiteSettings(
      R"({"schema":"manyforge.isaac_site.v1","scene_startup_timeout_s":60,"scene_service_timeout_s":60,"worlds":{},"pose_observers_by_cell":{}})");
  EXPECT_EQ(empty.FindPoseObserver("cell"), nullptr);
  EXPECT_THROW(static_cast<void>(empty.ResolveWorld("custom_scene")),
               IsaacSiteSettingsError);
}

TEST(IsaacSiteSettings, RejectsMalformedDuplicateAndNonClosedShapes) {
  const std::vector<std::string> invalid{
      "not: [valid",
      "[]",
      Replace(Site(), "manyforge.isaac_site.v1", "manyforge.isaac_site.v2"),
      Replace(Site(), R"("schema":)", R"("unknown":0,"schema":)"),
      Replace(Site(), R"("schema":)", R"("schema":"duplicate","schema":)"),
      Replace(Site(), R"("scene_startup_timeout_s":60,)", ""),
      Replace(Site(), R"("scene_service_timeout_s":60)",
              R"("scene_service_timeout_s":"60")"),
      Replace(
          Site(),
          R"("worlds":{"custom_scene":{"world_uri":"file:/Isaac Host/Scene.usda","expected_world_name":"Scene Name"}})",
          R"("worlds":[])"),
      Replace(Site(), R"("world_uri":)", R"("extra":true,"world_uri":)"),
      Replace(Site(), R"("world_uri":"file:/Isaac Host/Scene.usda",)", ""),
      Replace(Site(), R"("world_uri":"file:/Isaac Host/Scene.usda")",
              R"("world_uri":null)"),
      Replace(Site(), R"("site_config":)", R"("extra":0,"site_config":)"),
      Replace(Site(), R"("site_config":" /site/observer config.json ",)", ""),
      Replace(Site(), R"("site_config":" /site/observer config.json ")",
              R"("site_config":false)"),
      Replace(
          Site(),
          R"("additional_required_topics":["/z","/a","/single,entry"," /tf "])",
          R"("additional_required_topics":"/tf")"),
      Replace(Site(), R"("/z")", "5"),
  };
  for (const auto &bytes : invalid) {
    SCOPED_TRACE(bytes);
    EXPECT_THROW(static_cast<void>(ParseIsaacSiteSettings(bytes)),
                 IsaacSiteSettingsError);
  }
}

} // namespace
} // namespace manyforge::component_manager
