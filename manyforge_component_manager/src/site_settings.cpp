#include "manyforge_component_manager/site_settings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
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

/// The Deployment's portable id (`^[a-z][a-z0-9._-]{0,127}$`), the rule the
/// compilers and the GUI apply to `device_address_ref`.
[[nodiscard]] bool IsPortableId(const std::string_view value) noexcept {
  const auto lower = [](const char character) { return character >= 'a' && character <= 'z'; };
  return !value.empty() && value.size() <= 128U && lower(value.front()) &&
         std::all_of(value.begin() + 1, value.end(), [&](const char character) {
           return lower(character) || (character >= '0' && character <= '9') ||
                  character == '.' || character == '_' || character == '-';
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

/// The members a `manyforge.site.v1` file cannot carry.
constexpr const char *kVersionTwoMembers[] = {
    "disk_max_percent", "disk_always_free_bytes", "disk_machine_minimum_bytes",
    "core_dumps"};

constexpr double kMaxExactInteger = 9007199254740991.0;  // 2^53 - 1

/// The one C++ decision on which site settings schema lines are accepted;
/// true when the schema carries the disk limits and `core_dumps`. Decided
/// exception: version 1 files carry neither and use the defaults.
bool AcceptedSchemaHasDiskLimits(const std::string &schema) {
  if (schema == "manyforge.site.v2")
    return true;
  if (schema == "manyforge.site.v1")
    return false;
  Fail("site settings schema must be manyforge.site.v2");
}

/// The disk limits are read by the launchers; here they are only validated
/// (an integral JSON number in [low, high]).
void RequireInteger(const CanonicalValue &root, const char *name, double low,
                    double high) {
  const auto *value = root.find_object_member(name);
  if (value == nullptr)
    return;
  if (!value->is_number() || value->as_number() < low ||
      value->as_number() > high ||
      std::trunc(value->as_number()) != value->as_number())
    Fail(std::string("site settings ") + name + " must be an integer from " +
         std::to_string(static_cast<long long>(low)) + " to " +
         std::to_string(static_cast<long long>(high)));
}

/// `core_dumps`: absent, or exactly `enabled` (bool) and `max_bytes`
/// (integer 1 to 2^53-1). The soft core-file size limit it asks for; 0 when
/// absent or not enabled.
std::uint64_t CoreDumpLimitBytes(const CanonicalValue &root) {
  const auto *core_dumps = root.find_object_member("core_dumps");
  if (core_dumps == nullptr)
    return 0U;
  const auto *enabled = core_dumps->is_object()
                            ? core_dumps->find_object_member("enabled")
                            : nullptr;
  if (enabled == nullptr || !enabled->is_bool() ||
      core_dumps->find_object_member("max_bytes") == nullptr ||
      core_dumps->as_object().size() != 2U)
    Fail("site settings core_dumps must be an object with exactly enabled "
         "(true or false) and max_bytes");
  RequireInteger(*core_dumps, "max_bytes", 1.0, kMaxExactInteger);
  return enabled->as_bool()
             ? static_cast<std::uint64_t>(
                   core_dumps->find_object_member("max_bytes")->as_number())
             : 0U;
}

} // namespace

SiteSettings::SiteSettings(DeviceAddresses device_addresses,
                           std::vector<std::string> project_folders,
                           const std::uint64_t core_dump_limit_bytes)
    : device_addresses_(std::move(device_addresses)),
      project_folders_(std::move(project_folders)),
      core_dump_limit_bytes_(core_dump_limit_bytes) {}

std::uint64_t SiteSettings::core_dump_limit_bytes() const noexcept {
  return core_dump_limit_bytes_;
}

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
        name != "project_folders" && name != "disk_max_percent" &&
        name != "disk_always_free_bytes" &&
        name != "disk_machine_minimum_bytes" && name != "core_dumps")
      Fail("site settings has unknown field " + name);
  }
  const auto *schema = root.find_object_member("schema");
  const auto *addresses = root.find_object_member("device_addresses");
  const auto *folders = root.find_object_member("project_folders");
  if (schema == nullptr)
    Fail("site settings requires schema");
  if (!AcceptedSchemaHasDiskLimits(Text(*schema, "site settings schema"))) {
    for (const char *name : kVersionTwoMembers)
      if (root.find_object_member(name) != nullptr)
        Fail(std::string("site settings ") + name +
             " needs schema manyforge.site.v2: change the first line's schema "
             "to manyforge.site.v2; no other change is needed");
  }
  RequireInteger(root, "disk_max_percent", 1.0, 100.0);
  RequireInteger(root, "disk_always_free_bytes", 0.0, kMaxExactInteger);
  RequireInteger(root, "disk_machine_minimum_bytes", 0.0, kMaxExactInteger);
  const std::uint64_t core_dump_limit_bytes = CoreDumpLimitBytes(root);
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
  return SiteSettings(std::move(device_addresses), std::move(project_folders),
                      core_dump_limit_bytes);
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

deployment::SiteFacts SiteFactsOf(const std::optional<SiteSettings> &settings) {
  deployment::SiteFacts facts;
  facts.configured = settings.has_value();
  if (settings.has_value())
    facts.device_addresses.insert(settings->device_addresses().begin(),
                                  settings->device_addresses().end());
  return facts;
}

core::Result<std::string> ResolveSpecDeviceAddress(const deployment::ComponentSpec &spec,
                                                   const std::string_view device_address_ref) {
  const std::map<std::string, std::string> details{
      {"device_address_ref", std::string(device_address_ref)}};
  if (!spec.site_configured)
    return core::Result<std::string>::Err(core::ErrorDTO{
        std::string(kSiteSettingsUnavailable),
        "no site settings file is configured for this machine", details});
  const auto found = spec.device_addresses.find(device_address_ref);
  if (found == spec.device_addresses.end() || !found->second)
    return core::Result<std::string>::Err(core::ErrorDTO{
        std::string(kSiteDeviceAddressRefUnknown),
        "site settings name no address for this device reference", details});
  return core::Result<std::string>::Ok(*found->second);
}

deployment::SiteFacts SiteFactsOfDocument(const std::optional<std::string> &document) {
  return SiteFactsOf(document.has_value()
                         ? std::optional<SiteSettings>(ParseSiteSettings(*document))
                         : std::nullopt);
}

std::uint64_t CoreDumpLimitBytesOfDocument(const std::optional<std::string> &document) {
  return document.has_value() ? ParseSiteSettings(*document).core_dump_limit_bytes()
                              : 0U;
}

} // namespace manyforge::component_manager
