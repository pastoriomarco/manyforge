#include "manyforge_component_manager/site_settings.hpp"

#include <algorithm>
#include <manyforge_deployment/artifact_codec.hpp>
#include <utility>
#include <vector>

namespace manyforge::component_manager {
namespace {

using manyforge::deployment::CanonicalValue;

[[noreturn]] void Fail(const std::string &reason) {
  throw SiteSettingsError(reason);
}

const CanonicalValue::Object &RequireObject(const CanonicalValue &value,
                                            const std::string &label) {
  if (!value.is_object())
    Fail(label + " must be an object");
  return value.as_object();
}

const std::string &Text(const CanonicalValue &value, const std::string &label) {
  if (!value.is_string())
    Fail(label + " must be text");
  return value.as_string();
}

[[nodiscard]] bool IsPortableId(const std::string_view value) noexcept {
  const auto alphanumeric = [](const char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9');
  };
  return !value.empty() && value.size() <= 128U && alphanumeric(value.front()) &&
         std::all_of(value.begin() + 1, value.end(), [&](const char character) {
           return alphanumeric(character) || character == '.' ||
                  character == '_' || character == '-';
         });
}

/// Same host rule as the Real lane's xArm hardware address.
[[nodiscard]] bool IsHostAddress(const std::string_view value) noexcept {
  return !value.empty() && value.size() <= 253U &&
         std::all_of(value.begin(), value.end(), [](const char character) {
           return (character >= 'A' && character <= 'Z') ||
                  (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '.' ||
                  character == '-' || character == ':';
         });
}

} // namespace

SiteSettings::SiteSettings(DeviceAddresses device_addresses,
                           std::vector<std::string> project_folders)
    : device_addresses_(std::move(device_addresses)),
      project_folders_(std::move(project_folders)) {}

const std::vector<std::string> &SiteSettings::project_folders() const noexcept {
  return project_folders_;
}

const SiteSettings::DeviceAddresses &
SiteSettings::device_addresses() const noexcept {
  return device_addresses_;
}

SiteSettings ParseSiteSettings(std::string_view bytes) {
  CanonicalValue root;
  try {
    root = manyforge::deployment::parse_strict_json_model_yaml(bytes).root();
  } catch (const std::exception &error) {
    Fail("cannot decode site settings: " + std::string(error.what()));
  }
  const auto fields = RequireObject(root, "site settings");
  for (const auto &[name, ignored] : fields) {
    static_cast<void>(ignored);
    if (name != "schema" && name != "device_addresses" &&
        name != "project_folders")
      Fail("site settings has unknown field " + name);
  }
  const auto *schema = root.find_object_member("schema");
  const auto *addresses = root.find_object_member("device_addresses");
  const auto *folders = root.find_object_member("project_folders");
  if (schema == nullptr)
    Fail("site settings requires schema");
  if (Text(*schema, "site settings schema") != "manyforge.site.v1")
    Fail("site settings schema must be manyforge.site.v1");
  SiteSettings::DeviceAddresses device_addresses;
  const auto entries =
      addresses == nullptr
          ? CanonicalValue::Object{}
          : RequireObject(*addresses, "site settings device_addresses");
  for (const auto &[ref, value] : entries) {
    const std::string label = "site settings device_addresses." + ref;
    if (!IsPortableId(ref))
      Fail("site settings device address reference is not a portable id: " +
           ref);
    const auto &address = Text(value, label);
    if (!IsHostAddress(address))
      Fail(label + " must be a host name or IP address");
    device_addresses.emplace(ref, address);
  }
  // Carried only; the launcher reads the folders (batch E).
  std::vector<std::string> project_folders;
  if (folders != nullptr) {
    if (!folders->is_array())
      Fail("site settings project_folders must be a list");
    for (const auto &value : folders->as_array()) {
      const std::string folder = Text(value, "site settings project_folders[]");
      if (folder.empty() || folder.front() != '/' ||
          folder.find('\0') != std::string::npos)
        Fail("site settings project_folders entries must be absolute paths");
      if (std::find(project_folders.begin(), project_folders.end(), folder) !=
          project_folders.end())
        Fail("site settings project_folders repeats " + folder);
      project_folders.push_back(folder);
    }
  }
  return SiteSettings(std::move(device_addresses), std::move(project_folders));
}

core::Result<std::string>
ResolveSiteDeviceAddress(const std::optional<SiteSettings> &settings,
                         const std::string_view device_address_ref) {
  const std::map<std::string, std::string> details{
      {"device_address_ref", std::string(device_address_ref)}};
  if (!settings.has_value()) {
    return core::Result<std::string>::Err(core::ErrorDTO{
        std::string(kSiteSettingsUnavailable),
        "no site settings file is configured for this machine", details});
  }
  const auto found = settings->device_addresses().find(device_address_ref);
  if (found == settings->device_addresses().end()) {
    return core::Result<std::string>::Err(core::ErrorDTO{
        std::string(kSiteDeviceAddressRefUnknown),
        "site settings name no address for this device reference", details});
  }
  return core::Result<std::string>::Ok(found->second);
}

} // namespace manyforge::component_manager
