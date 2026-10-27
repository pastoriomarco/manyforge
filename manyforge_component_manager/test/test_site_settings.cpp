#include "manyforge_component_manager/site_settings.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace manyforge::component_manager {
namespace {

constexpr std::string_view kSample =
    R"({"schema":"manyforge.site.v1","device_addresses":{"lite6_main":"192.168.1.31"}})";

std::string Replace(std::string value, std::string_view before,
                    std::string_view after) {
  const auto position = value.find(before);
  EXPECT_NE(position, std::string::npos);
  if (position != std::string::npos)
    value.replace(position, before.size(), after);
  return value;
}

TEST(SiteSettings, PreservesDeviceAddresses) {
  const auto settings = ParseSiteSettings(kSample);
  const SiteSettings::DeviceAddresses expected{{"lite6_main", "192.168.1.31"}};
  EXPECT_EQ(settings.device_addresses(), expected);
  const auto hosts = ParseSiteSettings(
      R"({"schema":"manyforge.site.v1","device_addresses":{"A.b_c-9":"robot-1.local","v6":"fe80::1"}})");
  EXPECT_EQ(hosts.device_addresses().at("A.b_c-9"), "robot-1.local");
  EXPECT_EQ(hosts.device_addresses().at("v6"), "fe80::1");
}

TEST(SiteSettings, AllowsNoDeviceAddresses) {
  EXPECT_TRUE(ParseSiteSettings(
                  R"({"schema":"manyforge.site.v1","device_addresses":{}})")
                  .device_addresses()
                  .empty());
}

TEST(SiteSettings, DeviceAddressesAreOptionalForASimOnlyMachine) {
  const auto settings = ParseSiteSettings(R"({"schema":"manyforge.site.v1"})");
  EXPECT_TRUE(settings.device_addresses().empty());
  EXPECT_TRUE(settings.project_folders().empty());
  EXPECT_FALSE(ResolveSiteDeviceAddress(settings, "lite6_main").ok());
}

TEST(SiteSettings, CarriesAbsoluteProjectFoldersInFileOrder) {
  const auto settings = ParseSiteSettings(
      R"({"schema":"manyforge.site.v1","project_folders":["/home/op/cells","/data/projects"]})");
  EXPECT_EQ(settings.project_folders(),
            (std::vector<std::string>{"/home/op/cells", "/data/projects"}));
  EXPECT_TRUE(settings.device_addresses().empty());
  const auto both = ParseSiteSettings(
      R"({"schema":"manyforge.site.v1","device_addresses":{"lite6_main":"192.168.1.31"},"project_folders":[]})");
  EXPECT_EQ(both.device_addresses().at("lite6_main"), "192.168.1.31");
  EXPECT_TRUE(both.project_folders().empty());
}

TEST(SiteSettings, RejectsMalformedAndNonClosedShapes) {
  const std::string sample(kSample);
  const std::vector<std::string> invalid{
      "not: [valid",
      "[]",
      Replace(sample, "manyforge.site.v1", "manyforge.site.v2"),
      Replace(sample, R"("schema":"manyforge.site.v1",)", ""),
      Replace(sample, R"("schema":"manyforge.site.v1")", R"("schema":1)"),
      Replace(sample, "}}", R"(},"project_folders":"/data/projects"})"),
      Replace(sample, "}}", R"(},"project_folders":["relative/projects"]})"),
      Replace(sample, "}}", R"(},"project_folders":[""]})"),
      Replace(sample, "}}", R"(},"project_folders":[7]})"),
      Replace(sample, "}}", R"(},"project_folders":["/a","/a"]})"),
      Replace(sample, R"("schema":)", R"("unknown":0,"schema":)"),
      Replace(sample, R"("schema":)",
              R"("schema":"manyforge.site.v1","schema":)"),
      Replace(sample, R"({"lite6_main")",
              R"({"lite6_main":"192.168.1.30","lite6_main")"),
      Replace(sample, R"({"lite6_main":"192.168.1.31"})", "[]"),
      Replace(sample, R"({"lite6_main":"192.168.1.31"})", R"("192.168.1.31")"),
      Replace(sample, "lite6_main", "_lite6"),
      Replace(sample, "lite6_main", "lite6 main"),
      Replace(sample, R"("lite6_main")", R"("")"),
      Replace(sample, "lite6_main", std::string(129U, 'a')),
      Replace(sample, R"("192.168.1.31")", R"("")"),
      Replace(sample, R"("192.168.1.31")", "31"),
      Replace(sample, R"("192.168.1.31")", R"({"host":"192.168.1.31"})"),
      Replace(sample, "192.168.1.31", "192.168.1.31/24"),
      Replace(sample, "192.168.1.31", "robot host"),
      Replace(sample, "192.168.1.31", std::string(254U, 'a')),
  };
  for (const auto &bytes : invalid) {
    SCOPED_TRACE(bytes);
    EXPECT_THROW(static_cast<void>(ParseSiteSettings(bytes)),
                 SiteSettingsError);
  }
  EXPECT_NO_THROW(static_cast<void>(ParseSiteSettings(
      Replace(sample, "192.168.1.31", std::string(253U, 'a')))));
}

TEST(SiteSettings, ResolvesOnlyConfiguredReferences) {
  const std::optional<SiteSettings> settings = ParseSiteSettings(kSample);
  const auto resolved = ResolveSiteDeviceAddress(settings, "lite6_main");
  ASSERT_TRUE(resolved.ok());
  EXPECT_EQ(resolved.value(), "192.168.1.31");

  const auto unknown = ResolveSiteDeviceAddress(settings, "lite6_spare");
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().code, "site_device_address_ref_unknown");
  EXPECT_EQ(unknown.error().details.at("device_address_ref"), "lite6_spare");

  const auto unavailable = ResolveSiteDeviceAddress(std::nullopt, "lite6_main");
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().code, "site_settings_unavailable");
  EXPECT_EQ(unavailable.error().details.at("device_address_ref"), "lite6_main");
}

} // namespace
} // namespace manyforge::component_manager
