#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <manyforge_roboplan/vendor_device_adapter.hpp>

namespace manyforge::roboplan_backend {

/// V-A: the registry of the vendor adapter families this build links, one
/// `VendorAdapterFamily` row each, filled by the vendor libraries. The
/// family `vendor`; none for a vendor without an adapter.
[[nodiscard]] const VendorAdapterFamily *
find_vendor_adapter_family(std::string_view vendor);

/// Which of its family's Real lifecycle providers a provider is.
enum class VendorLifecycleProviderRole : std::uint8_t {
  kArm,
  kGripper,
};

struct VendorLifecycleProvider final {
  const VendorAdapterFamily *family{};
  VendorLifecycleProviderRole role{VendorLifecycleProviderRole::kArm};
};

/// The registered family whose Real arm or gripper lifecycle provider is
/// `provider_manifest_id`; none for every other provider.
[[nodiscard]] std::optional<VendorLifecycleProvider>
find_vendor_lifecycle_provider(std::string_view provider_manifest_id);

/// R7 packages 3 and 4: the vendor adapter a Real component's launch artifacts
/// select (`ControllerLaunchArtifacts::vendor_adapter`), not started yet; the
/// handle starts it on its own node. Until then its status is `availability`
/// and its primitives and Enable gate refuse.
/// Throws for a vendor without an adapter.
[[nodiscard]] std::shared_ptr<IRetainedDeviceStatusAdapter>
make_retained_vendor_device_status_adapter(
    const VendorAdapterSelection &selection);

} // namespace manyforge::roboplan_backend
