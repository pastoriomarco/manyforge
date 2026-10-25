#include "manyforge_component_manager/isaac_site_settings.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace manyforge::component_manager {
namespace {

std::string Site(std::string_view timeout = "60") {
  return R"({"schema":"manyforge.isaac_attach_site.v1","observer_configuration_timeout_s":)" +
         std::string(timeout) +
         R"(,"pose_observers_by_cell":{"cell":{"site_config":" /site/observer config.json "}}})";
}

std::string Replace(std::string value, std::string_view before,
                    std::string_view after) {
  const auto position = value.find(before);
  EXPECT_NE(position, std::string::npos);
  if (position != std::string::npos)
    value.replace(position, before.size(), after);
  return value;
}

TEST(IsaacSiteSettings, PreservesObserverConfiguration) {
  const auto settings = ParseIsaacSiteSettings(Site("180"));
  EXPECT_DOUBLE_EQ(settings.observer_configuration_timeout_s(), 180.0);
  const auto *observer = settings.FindPoseObserver("cell");
  ASSERT_NE(observer, nullptr);
  EXPECT_EQ(observer->site_config, " /site/observer config.json ");
  EXPECT_EQ(settings.FindPoseObserver("Cell"), nullptr);
}

TEST(IsaacSiteSettings, AllowsNoConfiguredObservers) {
  const auto empty = ParseIsaacSiteSettings(
      R"({"schema":"manyforge.isaac_attach_site.v1","observer_configuration_timeout_s":60,"pose_observers_by_cell":{}})");
  EXPECT_EQ(empty.FindPoseObserver("cell"), nullptr);
}

TEST(IsaacSiteSettings, RejectsWorldLoaderAndNonClosedShapes) {
  const std::vector<std::string> invalid{
      "not: [valid",
      "[]",
      Replace(Site(), "manyforge.isaac_attach_site.v1",
              "manyforge.isaac_site.v1"),
      Replace(Site(), R"("schema":)", R"("unknown":0,"schema":)"),
      Replace(Site(), R"("schema":)",
              R"("schema":"duplicate","schema":)"),
      Replace(Site(), R"("observer_configuration_timeout_s":60,)", ""),
      Replace(Site(), R"("observer_configuration_timeout_s":60)",
              R"("observer_configuration_timeout_s":"60")"),
      Replace(Site(), R"("site_config":)", R"("extra":0,"site_config":)"),
      Replace(Site(), R"("site_config":" /site/observer config.json ")",
              R"("site_config":false)"),
      Replace(Site(), R"("pose_observers_by_cell":)",
              R"("worlds":{},"pose_observers_by_cell":)"),
  };
  for (const auto &bytes : invalid) {
    SCOPED_TRACE(bytes);
    EXPECT_THROW(static_cast<void>(ParseIsaacSiteSettings(bytes)),
                 IsaacSiteSettingsError);
  }
}

} // namespace
} // namespace manyforge::component_manager
