#include <manyforge_roboplan/retained_vendor_device_status.hpp>

#include <array>
#include <stdexcept>
#include <string>

#include <manyforge_xarm_control/xarm_device_adapter.hpp>

namespace manyforge::roboplan_backend {
namespace {

// V-A: one row per vendor adapter library this build links, each row from
// its library. An explicit list rather than self-registration at static
// initialization: a shared vendor library whose symbols nothing references
// would be dropped by `--as-needed`, and its row with it.
[[nodiscard]] const std::array<VendorAdapterFamily, 1> &families() {
  static const std::array<VendorAdapterFamily, 1> rows{
      xarm_control::xarm_vendor_adapter_family()};
  return rows;
}

} // namespace

const VendorAdapterFamily *find_vendor_adapter_family(const std::string_view vendor) {
  for (const auto &family : families())
    if (family.vendor == vendor) return &family;
  return nullptr;
}

std::optional<VendorLifecycleProvider>
find_vendor_lifecycle_provider(const std::string_view provider_manifest_id) {
  for (const auto &family : families()) {
    if (family.arm_provider_manifest_id == provider_manifest_id)
      return VendorLifecycleProvider{&family, VendorLifecycleProviderRole::kArm};
    if (family.gripper_provider_manifest_id == provider_manifest_id)
      return VendorLifecycleProvider{&family, VendorLifecycleProviderRole::kGripper};
  }
  return std::nullopt;
}

std::shared_ptr<IRetainedDeviceStatusAdapter>
make_retained_vendor_device_status_adapter(
    const VendorAdapterSelection &selection) {
  if (const auto *family = find_vendor_adapter_family(selection.vendor))
    return family->make_adapter(selection);
  throw std::invalid_argument("no vendor adapter for '" + selection.vendor + "'");
}

} // namespace manyforge::roboplan_backend
