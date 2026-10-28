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

#include <manyforge_xarm_control/lifecycle_profile.hpp>

#include <manyforge_core/types/lifecycle_cause.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace manyforge::xarm_control {
namespace {

using core::ComponentLifecycleIntentKindDTO;
using core::ComponentLifecycleStateDTO;
using core::LifecycleFactId;
using core::LifecycleFactPredicateDTO;
using core::LifecycleFactValueDTO;
using core::LifecycleFactValueKindDTO;
using core::LifecycleIntentPlanDTO;
using core::LifecyclePredicateDTO;
using core::LifecyclePrimitiveId;

constexpr std::int64_t kPlanTimeoutNs = 30'000'000'000LL;
// m5.5: every Real xArm arm lifecycle step (Prepare, Enable, Disable,
// Recover) is bounded at 10 s: arming takes 3-4 s, the rest covers a clear.
constexpr std::int64_t kRealArmPlanTimeoutNs = 10'000'000'000LL;

[[nodiscard]] LifecycleFactValueDTO boolean_value(const bool value) {
  return {LifecycleFactValueKindDTO::kBoolean, value, std::nullopt,
          std::nullopt};
}

[[nodiscard]] LifecycleFactPredicateDTO boolean_fact(const std::string_view id,
                                                     const bool value) {
  return {LifecycleFactId(std::string(id)), boolean_value(value)};
}

[[nodiscard]] LifecyclePredicateDTO
predicate(std::vector<ComponentLifecycleStateDTO> states,
          std::vector<LifecycleFactPredicateDTO> facts) {
  std::sort(states.begin(), states.end());
  std::sort(facts.begin(), facts.end(),
            [](const auto &left, const auto &right) {
              return left.lifecycle_fact_id < right.lifecycle_fact_id;
            });
  return {std::move(states), std::move(facts)};
}

[[nodiscard]] std::vector<LifecycleIntentPlanDTO> xarm_arm_plans() {
  constexpr std::string_view kError = core::kDeviceErrorPresentFactId;
  constexpr std::string_view kWarning = core::kDeviceWarningPresentFactId;
  const auto recoverable = std::vector<ComponentLifecycleStateDTO>{
      ComponentLifecycleStateDTO::kDisabled, ComponentLifecycleStateDTO::kFaulted};
  const auto recovered =
      predicate({ComponentLifecycleStateDTO::kDisabled},
                {boolean_fact(kError, false), boolean_fact(kWarning, false)});
  return {{ComponentLifecycleIntentKindDTO::kRecover,
           ComponentLifecycleStateDTO::kDisabled,
           predicate(recoverable, {}),
           recovered,
           kPlanTimeoutNs,
           {{LifecyclePrimitiveId(std::string(kXArmClearErrorPrimitiveId)),
             predicate(recoverable, {boolean_fact(kError, true)}),
             predicate(recoverable, {boolean_fact(kError, false)})},
            {LifecyclePrimitiveId(std::string(kXArmClearWarningPrimitiveId)),
             predicate(recoverable, {boolean_fact(kError, false),
                                     boolean_fact(kWarning, true)}),
             recovered}}}};
}

// Facts of the ros2_control motion domain (RetainedRos2ControlRuntimeHandle):
// the generic ones, and those it reports only for a Real arm with a vendor
// adapter (hardware, broadcaster and controller activity, measured joints).
constexpr std::string_view kBroadcasterActive = "external.broadcaster_active";
constexpr std::string_view kControllerActive = "external.controller_active";
constexpr std::string_view kControllerLoaded = "external.controller_loaded";
constexpr std::string_view kControllerReady =
    "external.controller_configuration_ready";
constexpr std::string_view kControllerSpawned = "external.controller_spawned";
constexpr std::string_view kFaultPresent = "external.fault_present";
constexpr std::string_view kHardwareActive = "external.hardware_active";
constexpr std::string_view kMeasuredJointsValid =
    "external.measured_joints_valid";
constexpr std::string_view kTransportReady = "external.transport_ready";

[[nodiscard]] std::vector<LifecycleIntentPlanDTO> xarm_ros2_control_plans() {
  using State = ComponentLifecycleStateDTO;
  using Intent = ComponentLifecycleIntentKindDTO;
  constexpr std::string_view kError = core::kDeviceErrorPresentFactId;
  constexpr std::string_view kWarning = core::kDeviceWarningPresentFactId;
  constexpr std::string_view kReadyForCommands =
      core::kDeviceAcceptsCommandsFactId;
  const auto b = boolean_fact;
  const auto primitive = [](const std::string_view id) {
    return LifecyclePrimitiveId(std::string(id));
  };
  const std::vector<State> unarmed{State::kDeclared, State::kDisabled};
  const std::vector<State> any{State::kDeclared, State::kPrepared,
                               State::kActive,   State::kDegraded,
                               State::kFaulted,  State::kDisabled};

  // Prepared: armed, ready, broadcasting valid joints, controller inactive.
  const auto prepared = predicate(
      {State::kPrepared},
      {b(kReadyForCommands, true), b(kBroadcasterActive, true),
       b(kControllerActive, false), b(kControllerLoaded, true),
       b(kFaultPresent, false), b(kHardwareActive, true),
       b(kMeasuredJointsValid, true), b(kTransportReady, true)});
  // Armed by the vendor activation, before ManyForge marks it prepared. The
  // broadcaster may still be off: the plugin's deactivate-all can leave it
  // inactive, and the broadcaster step below turns it back on.
  const auto armed = predicate(
      unarmed, {b(kReadyForCommands, true), b(kFaultPresent, false),
                b(kHardwareActive, true), b(kTransportReady, true)});
  const auto active = predicate(
      {State::kActive},
      {b(kReadyForCommands, true), b(kControllerReady, true),
       b(kControllerLoaded, true), b(kFaultPresent, false),
       b(kTransportReady, true)});
  const auto disabled = predicate(
      {State::kDisabled},
      {b(kControllerReady, false), b(kControllerActive, false),
       b(kFaultPresent, false), b(kHardwareActive, false),
       b(kTransportReady, true)});
  const auto recovered = predicate(
      unarmed, {b(kError, false), b(kWarning, false),
                b(kControllerActive, false), b(kFaultPresent, false),
                b(kHardwareActive, false), b(kTransportReady, true)});
  constexpr std::string_view kHardwareActivate = "ros2_control.hardware.activate";
  constexpr std::string_view kHardwareDeactivate =
      "ros2_control.hardware.deactivate";
  constexpr std::string_view kControllerPrepare =
      "ros2_control.controller.prepare";
  constexpr std::string_view kControllerActivate =
      "ros2_control.controller.activate";
  constexpr std::string_view kControllerDeactivate =
      "ros2_control.controller.deactivate";
  constexpr std::string_view kBroadcasterActivate =
      "ros2_control.broadcaster.activate";
  return {
      {Intent::kPrepare,
       State::kPrepared,
       std::nullopt,
       prepared,
       kRealArmPlanTimeoutNs,
       {// m5.5: a vendor error left on the unarmed robot (C2 after an
        // emergency stop) is cleared first; the clear is verified by the
        // adapter (an error still reported afterwards is an incident).
        {primitive(kXArmClearErrorPrimitiveId),
         predicate(unarmed, {b(kError, true), b(kControllerActive, false),
                             b(kFaultPresent, false), b(kHardwareActive, false),
                             b(kTransportReady, true)}),
         predicate(unarmed, {b(kError, false), b(kControllerActive, false),
                             b(kFaultPresent, false), b(kHardwareActive, false),
                             b(kTransportReady, true)})},
        // D1: arming (the plugin's activation clears the warning, enables the
        // motors, selects servo mode and the ready state); never over a
        // vendor error.
        {primitive(kHardwareActivate),
         predicate(unarmed, {b(kError, false), b(kControllerSpawned, true),
                             b(kFaultPresent, false), b(kHardwareActive, false),
                             b(kTransportReady, true)}),
         armed},
        // §3.3: the plugin's reactivate-all turned the controller on.
        {primitive(kControllerDeactivate),
         predicate({State::kDeclared, State::kPrepared, State::kDisabled},
                   {b(kControllerActive, true), b(kFaultPresent, false),
                    b(kHardwareActive, true), b(kTransportReady, true)}),
         predicate({State::kDisabled},
                   {b(kControllerActive, false), b(kFaultPresent, false),
                    b(kHardwareActive, true), b(kTransportReady, true)})},
        // The plugin's reactivate-all did not bring the broadcaster back:
        // switch it on alone, then wait for its first post-activation sample.
        {primitive(kBroadcasterActivate),
         predicate(unarmed,
                   {b(kBroadcasterActive, false), b(kControllerActive, false),
                    b(kFaultPresent, false), b(kHardwareActive, true),
                    b(kTransportReady, true)}),
         predicate(unarmed,
                   {b(kBroadcasterActive, true), b(kFaultPresent, false),
                    b(kHardwareActive, true), b(kMeasuredJointsValid, true),
                    b(kTransportReady, true)})},
        {primitive(kControllerPrepare),
         predicate(unarmed,
                   {b(kReadyForCommands, true), b(kBroadcasterActive, true),
                    b(kControllerActive, false), b(kControllerLoaded, true),
                    b(kFaultPresent, false), b(kHardwareActive, true),
                    b(kMeasuredJointsValid, true), b(kTransportReady, true)}),
         prepared}}},
      {Intent::kEnable,
       State::kActive,
       std::nullopt,
       active,
       kRealArmPlanTimeoutNs,
       {{primitive(kControllerActivate),
         predicate({State::kPrepared},
                   {b(kReadyForCommands, true), b(kBroadcasterActive, true),
                    b(kControllerReady, false), b(kControllerActive, false),
                    b(kControllerLoaded, true), b(kFaultPresent, false),
                    b(kMeasuredJointsValid, true), b(kTransportReady, true)}),
         active}}},
      {Intent::kDisable,
       State::kDisabled,
       std::nullopt,
       disabled,
       kRealArmPlanTimeoutNs,
       {// D4 option B: the controller first, then the hardware.
        {primitive(kControllerDeactivate),
         predicate({State::kPrepared, State::kActive, State::kDegraded},
                   {b(kControllerActive, true), b(kTransportReady, true)}),
         predicate({State::kDisabled},
                   {b(kControllerActive, false), b(kTransportReady, true)})},
        {primitive(kHardwareDeactivate),
         predicate({State::kDeclared, State::kPrepared, State::kDegraded,
                    State::kDisabled},
                   {b(kControllerActive, false), b(kHardwareActive, true),
                    b(kTransportReady, true)}),
         disabled}}},
      {Intent::kRecover,
       State::kDisabled,
       predicate({State::kDegraded, State::kFaulted}, {b(kTransportReady, true)}),
       recovered,
       kRealArmPlanTimeoutNs,
       {// Each step only while its precondition holds (§3.1, D10).
        {primitive(kControllerDeactivate),
         predicate(any, {b(kControllerActive, true), b(kTransportReady, true)}),
         predicate(any, {b(kControllerActive, false), b(kTransportReady, true)})},
        {primitive(kHardwareDeactivate),
         predicate(any, {b(kControllerActive, false), b(kHardwareActive, true),
                         b(kTransportReady, true)}),
         predicate(any, {b(kControllerActive, false), b(kHardwareActive, false),
                         b(kTransportReady, true)})},
        {primitive(kXArmClearErrorPrimitiveId),
         predicate(any, {b(kError, true), b(kControllerActive, false),
                         b(kHardwareActive, false), b(kTransportReady, true)}),
         predicate(any, {b(kError, false), b(kControllerActive, false),
                         b(kHardwareActive, false), b(kTransportReady, true)})},
        {primitive(kXArmClearWarningPrimitiveId),
         predicate(any, {b(kError, false), b(kWarning, true),
                         b(kControllerActive, false), b(kHardwareActive, false),
                         b(kTransportReady, true)}),
         predicate(any, {b(kError, false), b(kWarning, false),
                         b(kControllerActive, false), b(kHardwareActive, false),
                         b(kTransportReady, true)})}}},
  };
}

[[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
anchored_catalog(const std::string_view provider_id,
                 const core::ProviderManifestDTO &provider_manifest,
                 std::vector<LifecycleIntentPlanDTO> plans) {
  const core::ProviderManifestId expected_id{std::string(provider_id)};
  if (provider_manifest.provider_manifest_id != expected_id) {
    return core::Result<core::BrandLifecyclePlanCatalogDTO>::Err(
        {"xarm_lifecycle_provider_manifest_mismatch",
         "the qualified provider manifest is not this xArm lifecycle owner",
         {{"reason", "provider_manifest_id_mismatch"}}});
  }
  auto catalog = core::MakeBrandLifecyclePlanCatalog(
      expected_id, provider_manifest.provider_manifest_revision,
      std::move(plans));
  if (!catalog.ok()) {
    return catalog;
  }
  const auto anchored = core::VerifyBrandLifecyclePlanCatalogManifestAnchor(
      provider_manifest, catalog.value());
  if (!anchored.ok()) {
    return core::Result<core::BrandLifecyclePlanCatalogDTO>::Err(
        anchored.error());
  }
  return catalog;
}

[[nodiscard]] LifecyclePredicateDTO gripper_predicate(
    const core::ComponentLifecycleStateDTO state, const bool enabled,
    const bool fault_present) {
  return predicate(
      {state},
      {boolean_fact("native.gripper.command_admission_enabled", enabled),
       boolean_fact("native.gripper.fault_present", fault_present)});
}

[[nodiscard]] std::vector<LifecycleIntentPlanDTO> gripper_plans() {
  auto prepare_eligible =
      gripper_predicate(ComponentLifecycleStateDTO::kDeclared, false, false);
  prepare_eligible.allowed_states.push_back(
      ComponentLifecycleStateDTO::kDisabled);
  const auto prepared = gripper_predicate(
      ComponentLifecycleStateDTO::kPrepared, false, false);
  const auto active = gripper_predicate(
      ComponentLifecycleStateDTO::kActive, true, false);
  const auto disabled = gripper_predicate(
      ComponentLifecycleStateDTO::kDisabled, false, false);
  return {
      {ComponentLifecycleIntentKindDTO::kPrepare,
       ComponentLifecycleStateDTO::kPrepared,
       std::nullopt,
       prepared,
       kPlanTimeoutNs,
       {{LifecyclePrimitiveId("native.gripper.prepare"), prepare_eligible,
         prepared}}},
      {ComponentLifecycleIntentKindDTO::kEnable,
       ComponentLifecycleStateDTO::kActive,
       std::nullopt,
       active,
       kPlanTimeoutNs,
       {{LifecyclePrimitiveId("native.gripper.enable"), prepared, active}}},
      {ComponentLifecycleIntentKindDTO::kDisable,
       ComponentLifecycleStateDTO::kDisabled,
       std::nullopt,
       disabled,
       kPlanTimeoutNs,
       {{LifecyclePrimitiveId("native.gripper.disable"),
         predicate({ComponentLifecycleStateDTO::kPrepared,
                    ComponentLifecycleStateDTO::kActive,
                    ComponentLifecycleStateDTO::kDegraded,
                    ComponentLifecycleStateDTO::kFaulted},
                   {}),
         disabled}}},
      {ComponentLifecycleIntentKindDTO::kRecover,
       ComponentLifecycleStateDTO::kDisabled,
       gripper_predicate(ComponentLifecycleStateDTO::kDisabled, false, true),
       disabled,
       kPlanTimeoutNs,
       {{LifecyclePrimitiveId("native.gripper.clear_fault"),
         gripper_predicate(ComponentLifecycleStateDTO::kDisabled, false, true),
         disabled}}},
  };
}

} // namespace

std::vector<core::LifecycleIntentPlanDTO>
make_lite6_gripper_lifecycle_plans() {
  return gripper_plans();
}

core::Result<core::ContentDigest>
compute_lite6_gripper_lifecycle_catalog_semantic_digest() {
  return core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
      core::ProviderManifestId(
          std::string(kXArmGripperLifecycleProviderManifestId)),
      make_lite6_gripper_lifecycle_plans());
}

core::Result<core::BrandLifecyclePlanCatalogDTO>
make_lite6_gripper_lifecycle_catalog(
    const core::ProviderManifestDTO &provider_manifest) {
  return anchored_catalog(kXArmGripperLifecycleProviderManifestId,
                          provider_manifest,
                          make_lite6_gripper_lifecycle_plans());
}

std::vector<core::LifecycleIntentPlanDTO>
make_xarm_ros2_control_lifecycle_plans() {
  return xarm_ros2_control_plans();
}

core::Result<core::ContentDigest>
compute_xarm_ros2_control_lifecycle_catalog_semantic_digest() {
  return core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
      core::ProviderManifestId(
          std::string(kXArmRos2ControlLifecycleProviderManifestId)),
      make_xarm_ros2_control_lifecycle_plans());
}

core::Result<core::BrandLifecyclePlanCatalogDTO>
make_xarm_ros2_control_lifecycle_catalog(
    const core::ProviderManifestDTO &provider_manifest) {
  return anchored_catalog(kXArmRos2ControlLifecycleProviderManifestId,
                          provider_manifest,
                          make_xarm_ros2_control_lifecycle_plans());
}

std::vector<core::LifecycleIntentPlanDTO> make_xarm_arm_lifecycle_plans() {
  return xarm_arm_plans();
}

core::Result<core::ContentDigest>
compute_xarm_arm_lifecycle_catalog_semantic_digest() {
  return core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
      core::ProviderManifestId(std::string(kXArmArmLifecycleProviderManifestId)),
      make_xarm_arm_lifecycle_plans());
}

core::Result<core::BrandLifecyclePlanCatalogDTO>
make_xarm_arm_lifecycle_catalog(
    const core::ProviderManifestDTO &provider_manifest) {
  return anchored_catalog(kXArmArmLifecycleProviderManifestId,
                          provider_manifest, make_xarm_arm_lifecycle_plans());
}

} // namespace manyforge::xarm_control
