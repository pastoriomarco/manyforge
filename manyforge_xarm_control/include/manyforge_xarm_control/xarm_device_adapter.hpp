// Copyright 2026 Marco Pastorio
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <memory>
#include <string_view>

#include <manyforge_roboplan/vendor_device_adapter.hpp>

namespace manyforge::xarm_control {

/// The xArm adapter family (`VendorAdapterSelection::vendor`) of every
/// xArm-family robot (xArm 5/6/7, Lite 6, UF850).
inline constexpr std::string_view kXArmVendorAdapterFamily = "xarm";

/// The xArm device adapter (R7 packages 3 and 4) of one Real component, over
/// its unmodified driver's `robot_states` and services; not started. Throws
/// `std::invalid_argument` for invalid adapter data (driver namespace, 1 to 7
/// axes, a positive joint agreement tolerance).
[[nodiscard]] std::shared_ptr<roboplan_backend::IRetainedDeviceStatusAdapter>
make_xarm_device_adapter(const roboplan_backend::VendorAdapterSelection &selection);

/// V-A: the xArm family's registry row: the adapter above, the Real arm's
/// provider and catalog (`lifecycle_profile.hpp`) and its gripper's provider.
[[nodiscard]] roboplan_backend::VendorAdapterFamily xarm_vendor_adapter_family();

} // namespace manyforge::xarm_control
