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

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <string_view>
#include <vector>

namespace manyforge::xarm_control {

/// The two vendor primitives of the real xArm catalog (R7 package 4): the
/// unmodified driver's `clean_error` and `clean_warn` services. The rest of
/// the vendor ready sequence runs inside the plugin's own activation. V-B1:
/// the error clear is the shared `core::kDeviceClearErrorPrimitiveId`
/// (`device.clear_error`); the warning clear is xArm-only and keeps its name.
inline constexpr std::string_view kXArmClearWarningPrimitiveId =
    "xarm.clear_warning";

/// R7 package 4: the ros2_control provider of a Real xArm-family arm. It
/// offers what `manyforge.provider.ros2_control` offers; only its lifecycle
/// catalog differs, so Mock and Isaac keep theirs.
inline constexpr std::string_view kXArmRos2ControlLifecycleProviderManifestId =
    "manyforge.provider.ros2_control.xarm";

/// The provider of a Real xArm-family component's gripper domain: the shared
/// Real gripper plans (`manyforge_roboplan`), Recover targeting `disabled`
/// like the arm.
inline constexpr std::string_view kXArmRos2ControlGripperLifecycleProviderManifestId =
    "manyforge.provider.ros2_control.xarm.gripper";

/// The Real xArm arm plans (the deployment generator reads their copy in
/// `resources/xarm_ros2_control_lifecycle_plans.json`), on stock
/// ros2_control plus the two vendor services (R7 §3.1, D1, D4, D10):
/// - Prepare first clears a vendor error left on the unarmed robot (m5.5:
///   C2 after an emergency stop; the adapter verifies the clear), then arms
///   through the hardware activation (the plugin's `on_activate`: clear,
///   motion enable, servo mode, ready state), only with fresh status and no
///   vendor error; it then
///   absorbs the plugin's reactivate-all (the trajectory controller it turned
///   on is switched off again) and marks the arm prepared once the robot is
///   ready, the broadcaster active and the measured joints valid.
/// - Enable is the existing trajectory-controller activation; the handle's
///   Enable gate refuses before it unless the robot is ready and the measured
///   joints agree with the reported angles.
/// - Disable switches off the trajectory controller, then the hardware: a
///   disabled robot is not armed.
/// - Recover switches off the controller and the hardware, then clears the
///   vendor error and the warning; the target driver re-drives Prepare and
///   Enable.
/// Every step of these plans is bounded at 10 s (m5.5).
[[nodiscard]] std::vector<core::LifecycleIntentPlanDTO>
make_xarm_ros2_control_lifecycle_plans();

[[nodiscard]] core::Result<core::ContentDigest>
compute_xarm_ros2_control_lifecycle_catalog_semantic_digest();

[[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
make_xarm_ros2_control_lifecycle_catalog(
    const core::ProviderManifestDTO &provider_manifest);

} // namespace manyforge::xarm_control
