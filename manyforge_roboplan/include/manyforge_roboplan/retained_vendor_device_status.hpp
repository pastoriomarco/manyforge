#pragma once

#include <memory>

#include <manyforge_roboplan/controller_launch_artifacts.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>

namespace manyforge::roboplan_backend {

/// R7 packages 3 and 4: the vendor adapter a Real component's launch artifacts
/// select (`ControllerLaunchArtifacts::vendor_adapter`), not started yet; the
/// handle starts it on its own node. Until then its status is `availability`
/// and its primitives and Enable gate refuse.
/// Throws for a vendor without an adapter.
[[nodiscard]] std::shared_ptr<IRetainedDeviceStatusAdapter>
make_retained_vendor_device_status_adapter(
    const VendorAdapterSelection &selection);

} // namespace manyforge::roboplan_backend
