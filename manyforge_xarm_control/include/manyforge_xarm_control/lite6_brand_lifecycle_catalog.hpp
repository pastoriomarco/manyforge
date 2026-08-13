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

#include <string_view>

#include <manyforge_core/build_owned_lifecycle_plan_catalog_source.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>

namespace manyforge::xarm_control {

inline constexpr std::string_view kLite6LifecycleProviderManifestId =
    "lite6_arm_provider";

inline constexpr std::string_view kXArmMotionEnabledFactId =
    "xarm.motion_enabled";
inline constexpr std::string_view kXArmPositionValidFactId =
    "xarm.position_valid";
inline constexpr std::string_view kXArmJointServoModeSelectedFactId =
    "xarm.joint_servo_mode_selected";
inline constexpr std::string_view kXArmVendorControlReadyFactId =
    "xarm.vendor_control_ready";
inline constexpr std::string_view kXArmVendorStoppedFactId =
    "xarm.vendor_stopped";
inline constexpr std::string_view kXArmFaultPresentFactId =
    "xarm.fault_present";
inline constexpr std::string_view kXArmRecoveryRequiredFactId =
    "xarm.recovery_required";
inline constexpr std::string_view kXArmWarningPresentFactId =
    "xarm.warning_present";
inline constexpr std::string_view kXArmReadyStateRequestEligibleFactId =
    "xarm.ready_state_request_eligible";
inline constexpr std::string_view kXArmVendorFeedbackClassFactId =
    "xarm.vendor_feedback_class";

inline constexpr std::string_view kXArmEnableMotionPrimitiveId =
    "xarm.enable_motion";
inline constexpr std::string_view kXArmSelectJointServoModePrimitiveId =
    "xarm.select_joint_servo_mode";
inline constexpr std::string_view kXArmSetReadyStatePrimitiveId =
    "xarm.set_ready_state";
inline constexpr std::string_view kXArmSetStoppedStatePrimitiveId =
    "xarm.set_stopped_state";
inline constexpr std::string_view kXArmDisableMotionPrimitiveId =
    "xarm.disable_motion";
inline constexpr std::string_view kXArmClearErrorPrimitiveId =
    "xarm.clear_error";
inline constexpr std::string_view kXArmClearWarningPrimitiveId =
    "xarm.clear_warning";

/// Compute the acyclic semantic anchor which the build-owned Lite6 provider
/// manifest must carry before its content-derived revision is minted.
[[nodiscard]] core::Result<core::ContentDigest>
compute_lite6_brand_lifecycle_catalog_semantic_digest();

/// Return the exact generator-owned production provider manifest DTO whose
/// revision carries the Lite6 lifecycle semantic declaration.
[[nodiscard]] core::Result<core::ProviderManifestDTO>
make_build_owned_lite6_lifecycle_provider_manifest();

/// Construct the closed Lite6 catalog for one already qualified build-owned
/// provider manifest. The final catalog digest is derived by Core's canonical
/// codec and the manifest/catalog semantic anchor is verified before return.
[[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
make_lite6_brand_lifecycle_plan_catalog(
    const core::ProviderManifestDTO &provider_manifest);

/// One-row immutable build-owned source for the Lite6 lifecycle catalog.
class Lite6BrandLifecyclePlanCatalogSource final
    : public core::ICCLCLifecyclePlanCatalogSource {
public:
  /// Load the immutable generator-owned production manifest/catalog row.
  [[nodiscard]] static core::Result<Lite6BrandLifecyclePlanCatalogSource>
  make_build_owned();

  [[nodiscard]] static core::Result<Lite6BrandLifecyclePlanCatalogSource>
  make(core::ProviderManifestDTO provider_manifest);

  [[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
  LoadExactLifecyclePlanCatalog(
      const core::ProviderManifestId &provider_manifest_id,
      const core::ProviderManifestRevision &provider_manifest_revision,
      const core::ContentDigest &catalog_digest) const override;

  [[nodiscard]] const core::BrandLifecyclePlanCatalogDTO &
  catalog() const noexcept;

private:
  Lite6BrandLifecyclePlanCatalogSource(
      core::BrandLifecyclePlanCatalogDTO catalog,
      core::BuildOwnedLifecyclePlanCatalogSource source);

  core::BrandLifecyclePlanCatalogDTO catalog_;
  core::BuildOwnedLifecyclePlanCatalogSource source_;
};

} // namespace manyforge::xarm_control
