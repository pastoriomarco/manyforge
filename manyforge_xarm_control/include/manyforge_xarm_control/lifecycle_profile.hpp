// Copyright 2026 Flexin Group SRL
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
#include <string_view>
#include <vector>

namespace manyforge::xarm_control {

/// The two vendor primitives of the real xArm catalog (R7 package 4): the
/// unmodified driver's `clean_error` and `clean_warn` services. The rest of
/// the vendor ready sequence runs inside the plugin's own activation.
inline constexpr std::string_view kXArmClearErrorPrimitiveId =
    "xarm.clear_error";
inline constexpr std::string_view kXArmClearWarningPrimitiveId =
    "xarm.clear_warning";

/// The maintained xArm-family lifecycle providers (R7 D12).
inline constexpr std::string_view kXArmArmLifecycleProviderManifestId =
    "xarm_arm_provider";
inline constexpr std::string_view kXArmGripperLifecycleProviderManifestId =
    "xarm_gripper_provider";

/// The xArm arm plans of the maintained catalog (mirrors the deployment
/// generator): the operator's Recover clears the vendor error, then the
/// warning, on the `device.*` facts. The vendor plugin's activation runs the
/// rest of the ready sequence, so no other vendor primitive exists.
[[nodiscard]] std::vector<core::LifecycleIntentPlanDTO>
make_xarm_arm_lifecycle_plans();

[[nodiscard]] core::Result<core::ContentDigest>
compute_xarm_arm_lifecycle_catalog_semantic_digest();

[[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
make_xarm_arm_lifecycle_catalog(
    const core::ProviderManifestDTO &provider_manifest);

/// R7 package 4: the ros2_control provider of a Real xArm-family arm. It
/// offers what `manyforge.provider.ros2_control` offers; only its lifecycle
/// catalog differs, so Mock and Isaac keep theirs.
inline constexpr std::string_view kXArmRos2ControlLifecycleProviderManifestId =
    "manyforge.provider.ros2_control.xarm";

/// The Real xArm arm plans (mirrors the deployment generator), on stock
/// ros2_control plus the two vendor services (R7 §3.1, D1, D4, D10):
/// - Prepare arms through the hardware activation (the plugin's
///   `on_activate`), only with fresh status and no vendor error; it then
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
[[nodiscard]] std::vector<core::LifecycleIntentPlanDTO>
make_xarm_ros2_control_lifecycle_plans();

[[nodiscard]] core::Result<core::ContentDigest>
compute_xarm_ros2_control_lifecycle_catalog_semantic_digest();

[[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
make_xarm_ros2_control_lifecycle_catalog(
    const core::ProviderManifestDTO &provider_manifest);

/// The maintained native Lite 6 gripper plans, provided by `xarm_gripper_provider`.

[[nodiscard]] std::vector<core::LifecycleIntentPlanDTO>
make_lite6_gripper_lifecycle_plans();

[[nodiscard]] core::Result<core::ContentDigest>
compute_lite6_gripper_lifecycle_catalog_semantic_digest();

[[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
make_lite6_gripper_lifecycle_catalog(
    const core::ProviderManifestDTO &provider_manifest);

} // namespace manyforge::xarm_control
