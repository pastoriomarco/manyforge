#pragma once

#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/error_code.hpp>

namespace manyforge::component_manager {

class SiteSettingsError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// No site settings file is configured for this machine.
inline constexpr std::string_view kSiteSettingsUnavailable =
    "site_settings_unavailable";
/// The configured site settings name no address for the reference.
inline constexpr std::string_view kSiteDeviceAddressRefUnknown =
    "site_device_address_ref_unknown";

/// One structurally decoded generic machine-level site document
/// (`manyforge.site.v1`). It holds machine facts only, such as a device's
/// host name or IP keyed by a portable reference name. No file access or
/// defaults.
class SiteSettings final {
public:
  using DeviceAddresses = std::map<std::string, std::string, std::less<>>;

  [[nodiscard]] const DeviceAddresses &device_addresses() const noexcept;
  /// Absolute host project folders, in file order (validated and carried
  /// only; the development launcher reads them).
  [[nodiscard]] const std::vector<std::string> &project_folders() const noexcept;

private:
  SiteSettings(DeviceAddresses device_addresses,
               std::vector<std::string> project_folders);

  DeviceAddresses device_addresses_;
  std::vector<std::string> project_folders_;

  friend SiteSettings ParseSiteSettings(std::string_view bytes);
};

/// Reuses the existing strict JSON/YAML model decoder (duplicate keys refuse)
/// and closes the approved shape: `schema` plus the optional
/// `device_addresses` (absent: none; a sim-only machine) and the optional
/// `project_folders` (unique absolute paths).
/// References are portable ids; addresses are non-empty host names or IPs of
/// at most 253 characters from [A-Za-z0-9.:-]. Throws SiteSettingsError.
[[nodiscard]] SiteSettings ParseSiteSettings(std::string_view bytes);

/// The configured address for `device_address_ref`; never a fallback.
/// Errors: kSiteSettingsUnavailable, kSiteDeviceAddressRefUnknown (details
/// carry `device_address_ref`).
[[nodiscard]] core::Result<std::string>
ResolveSiteDeviceAddress(const std::optional<SiteSettings> &settings,
                         std::string_view device_address_ref);

} // namespace manyforge::component_manager
