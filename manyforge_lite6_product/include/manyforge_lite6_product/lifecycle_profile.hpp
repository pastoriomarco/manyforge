#pragma once

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <string_view>
#include <vector>

namespace manyforge::lite6_product {

inline constexpr std::string_view kLifecycleProviderManifestId =
    "lite6_arm_provider";
inline constexpr std::string_view kGripperLifecycleProviderManifestId =
    "lite6_gripper_provider";

inline constexpr std::string_view kMotionEnabledFactId =
    "xarm.motion_enabled";
inline constexpr std::string_view kPositionValidFactId =
    "xarm.position_valid";
inline constexpr std::string_view kJointServoModeSelectedFactId =
    "xarm.joint_servo_mode_selected";
inline constexpr std::string_view kVendorControlReadyFactId =
    "xarm.vendor_control_ready";
inline constexpr std::string_view kVendorStoppedFactId =
    "xarm.vendor_stopped";
inline constexpr std::string_view kFaultPresentFactId =
    "xarm.fault_present";
inline constexpr std::string_view kRecoveryRequiredFactId =
    "xarm.recovery_required";
inline constexpr std::string_view kWarningPresentFactId =
    "xarm.warning_present";
inline constexpr std::string_view kReadyStateRequestEligibleFactId =
    "xarm.ready_state_request_eligible";
inline constexpr std::string_view kVendorFeedbackClassFactId =
    "xarm.vendor_feedback_class";

inline constexpr std::string_view kEnableMotionPrimitiveId =
    "xarm.enable_motion";
inline constexpr std::string_view kSelectJointServoModePrimitiveId =
    "xarm.select_joint_servo_mode";
inline constexpr std::string_view kSetReadyStatePrimitiveId =
    "xarm.set_ready_state";
inline constexpr std::string_view kSetStoppedStatePrimitiveId =
    "xarm.set_stopped_state";
inline constexpr std::string_view kDisableMotionPrimitiveId =
    "xarm.disable_motion";
inline constexpr std::string_view kClearErrorPrimitiveId =
    "xarm.clear_error";
inline constexpr std::string_view kClearWarningPrimitiveId =
    "xarm.clear_warning";

[[nodiscard]] std::vector<core::LifecycleIntentPlanDTO>
make_lite6_lifecycle_plans();

[[nodiscard]] core::Result<core::ContentDigest>
compute_lite6_lifecycle_catalog_semantic_digest();

[[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
make_lite6_lifecycle_catalog(
    const core::ProviderManifestDTO &provider_manifest);

[[nodiscard]] core::Result<std::vector<core::ProviderCapabilityOfferDTO>>
make_lite6_lifecycle_capability_offers();

[[nodiscard]] std::vector<core::LifecycleIntentPlanDTO>
make_lite6_gripper_lifecycle_plans();

[[nodiscard]] core::Result<core::ContentDigest>
compute_lite6_gripper_lifecycle_catalog_semantic_digest();

[[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
make_lite6_gripper_lifecycle_catalog(
    const core::ProviderManifestDTO &provider_manifest);

} // namespace manyforge::lite6_product
