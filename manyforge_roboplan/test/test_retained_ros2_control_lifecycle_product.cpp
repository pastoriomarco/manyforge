#include "../src/io_provider_lifecycle_product.hpp"
#include "io_retained_fixture.hpp"
#include "retained_lifecycle_product_fixture.hpp"

#include <gtest/gtest.h>
#include <manyforge_core/component_lifecycle_admission_query.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_roboplan/retained_ros2_control_lifecycle_product.hpp>
#include <manyforge_roboplan/retained_vendor_device_status.hpp>
#include <manyforge_xarm_control/lifecycle_profile.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

class ProductSink final : public core::IExecutionStateSink {
public:
  core::ExecutionStateSinkResult
  apply_group_joint_positions(const std::string &, const std::vector<double> &,
                              core::ExecutionStateUpdateMode) override {
    return {};
  }
  std::optional<double>
  group_joint_position(const std::string &,
                       const std::string &) const override {
    return 0.;
  }
  core::ExecutionStateSinkResult
  apply_group_joint_position(const std::string &, const std::string &, double,
                             core::ExecutionStateUpdateMode) override {
    return {};
  }
};

void check_owned_process_health(bool with_pure_projection) {
  SCOPED_TRACE(with_pure_projection ? "pure projection" : "missing projection");
  product_test::PackageRoot root;
  const auto anchor = core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
      core::ProviderManifestId(
          std::string(kRos2ControlLifecycleProviderManifestId)),
      retained_ros2_control_lifecycle_plans());
  ASSERT_TRUE(anchor.ok());
  const auto fixture =
      deployment::test::materialize_lite6_retained_consumer_fixture_from(
          root.path(),
          deployment::test::make_lite6_ros2_control_motion_retention_fixture(
              false, deployment::test::kLite6ArmTargetId, std::nullopt,
              anchor.value().value(), kRos2ControlLifecycleProviderManifestId,
              true));
  const auto binding = product_test::ArmBinding(*fixture.bundle, root.path());
  const auto domain = binding.value().control_path->control_domain_id;
  std::vector<std::string> joints, source_joints;
  for (const auto &resource : binding.value().resources) {
    ASSERT_TRUE(resource.source_joint_name);
    joints.push_back(resource.compiled_resource_name);
    source_joints.push_back(*resource.source_joint_name);
  }
  core::FakeClock clock;
  clock.set(100);
  core::ManualScheduler scheduler;
  core::ObservedRobotStatePublication publication(
      joints,
      {{core::ObservedRobotStateSourceKind::kCommandedState, "commanded",
        core::ObservedRobotStateAuthorityProfile::kCommandedSimulation, 0}});
  RetainedRos2ControlArmTransport arm;
  arm.adapter.action_name = "/product_health/arm/follow_joint_trajectory";
  arm.adapter.state_topic = "/product_health/arm/controller_state";
  arm.adapter.scale_topic = "/product_health/arm/speed_scaling_input";
  arm.adapter.lane_facts.joint_names = source_joints;
  arm.adapter.lane_facts.supports_scale = true;
  arm.adapter.lane_facts.state_rate_hz = 100.;
  arm.adapter.lane_facts.feedback_rate_hz = 100.;
  arm.adapter.lane_facts.controller_update_rate_hz = 100.;
  arm.controller = {"/product_health/controller_manager", "arm", "TestSystem"};
  arm.compiled_joint_names = joints;
  unsigned observes = 0, pure_reads = 0;
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  const auto bound = handle->Bind(
      std::make_unique<ros2_control_execution::Ros2TransportResources>(
          "manyforge_product_health", "/", 1U, std::make_shared<ProductSink>(),
          "arm", clock),
      clock, std::move(arm),
      [&](std::shared_ptr<core::IExecutionPort> port) {
        core::ExecutionPortParticipantConfig config;
        config.stop.trip_authority = core::StateAuthorityClass::kCommanded;
        config.stop.stop_traj_delta_s = .1;
        config.stop.sample_period_s = .01;
        config.stop.replacement_start_allowance_s = .02;
        config.stop.hard_stop_completion_timeout_ns = 1'000'000'000;
        config.late_cycle_margin_s = .5;
        return std::make_shared<core::ExecutionPortParticipant>(
            binding, std::move(port), clock, scheduler,
            [](core::TimePoint, const std::stop_token &) {},
            core::ExecutionPortParticipantStateAuthority{
                &publication, [] { return core::Result<void>::Ok(); }},
            config);
      },
      nullptr, binding, std::nullopt);
  ASSERT_TRUE(bound.ok()) << bound.error().message;
  const auto advancement_source = [&]() -> std::optional<std::string> {
    ++observes;
    return "process absent";
  };
  if (with_pure_projection) {
    handle->InstallControllerProcessFaultSource(
        advancement_source, [&] {
          ++pure_reads;
          return core::Result<std::optional<std::string>>::Ok("process absent");
        });
  } else {
    // A separate actual owner installs the legacy source once before start.
    // Missing pure health must never advance it or invent an alive process.
    handle->InstallControllerProcessFaultSource(advancement_source);
  }
  const deployment::FilesystemVerifiedRetainedProviderManifestSource manifests(
      root.path());
  auto product = RetainedHostLifecycleProduct::Create(
      fixture.bundle, manifests, {binding}, {handle}, clock, 1000, 1'000'000);
  ASSERT_TRUE(product.ok()) << product.error().message;
  ASSERT_EQ(product.value()->brand_ports().size(), 1U);
  const auto occurrence =
      product.value()->brand_ports().front().provider_occurrence;
  auto projection = product.value()->brand_ports().front().health_projection;
  ASSERT_TRUE(projection);
  const std::weak_ptr<RetainedRos2ControlRuntimeHandle> weak_handle = handle;
  const auto before = handle->ReadOwnedControllerOccurrence();
  ASSERT_TRUE(before.locally_valid());
  {
    core::FixedCCLCPortRegistry registry(product.value()->brand_ports(), {});
    product.value().reset();
    handle.reset();
    ASSERT_FALSE(weak_handle.expired());
    for (int read = 0; read < 3; ++read) {
      const auto health =
          registry.ReadExactLifecycleHealth(occurrence, 200 + read);
      if (with_pure_projection) {
        ASSERT_TRUE(health.ok()) << health.error().message;
        EXPECT_EQ(health.value().observed_state,
                  core::ComponentLifecycleStateDTO::kFaulted);
        ASSERT_TRUE(health.value().diagnostic);
        EXPECT_EQ(health.value().diagnostic->message, "process absent");
      } else {
        ASSERT_FALSE(health.ok());
        EXPECT_EQ(health.error().details.at("reason"),
                  "ros2_control_process_health_unavailable");
      }
    }
    EXPECT_EQ(observes, 0U);
    EXPECT_EQ(pure_reads, with_pure_projection ? 3U : 0U);
    auto foreign = occurrence;
    foreign.provider_runtime_epoch = core::RuntimeEpoch("foreign.epoch");
    const auto refused = registry.ReadExactLifecycleHealth(foreign, 200);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().details.at("reason"),
              "provider_occurrence_not_registered");
    EXPECT_EQ(pure_reads, with_pure_projection ? 3U : 0U);
    EXPECT_FALSE(projection(-1).ok());
    clock.set(-1);
    EXPECT_EQ(registry.ReadExactLifecycleHealth(occurrence, 200).ok(),
              with_pure_projection);
    clock.set(100);
    const auto after = weak_handle.lock()->ReadOwnedControllerOccurrence();
    EXPECT_EQ(after, before);
    EXPECT_EQ(observes, 0U);
  }
  ASSERT_FALSE(weak_handle.expired());
  projection = {};
  EXPECT_TRUE(weak_handle.expired());
}

TEST(RetainedRos2ControlLifecycleProductTest,
     ExactRegistryKeepsDomainOwnerAndUsesPureProcessHealthWithoutObservation) {
  check_owned_process_health(true);
  if (::testing::Test::HasFatalFailure())
    return;
  check_owned_process_health(false);
}

TEST(RetainedRos2ControlLifecycleProductTest,
     GenuineProviderOnlyProductRequiresRealResourcesAndPinsActualDriver) {
  product_test::PackageRoot root;
  const auto fixture =
      deployment::test::materialize_boolean_io_retained_fixture(root.path());
  const auto &domain =
      fixture.bundle->value().cell_graph.control_domains.front();
  ASSERT_NE(domain.provider(), nullptr);
  const auto &provider = *domain.provider();
  deployment::FilesystemVerifiedRetainedProviderManifestSource manifests(
      root.path());
  const auto manifest = manifests.get_verified(
      {fixture.bundle->retained_bundle_id(), provider.provider_manifest_id,
       provider.provider_manifest_revision});
  ASSERT_TRUE(manifest);
  auto clock = std::make_shared<core::FakeClock>();
  const core::RuntimeEpoch epoch("actual.host.epoch");
  core::ResolvedLifecycleControlDomainScopeDTO scope{
      domain.control_domain_id, domain.control_domain_revision,
      core::ResolvedProviderDomainScopeDTO{provider, epoch, std::nullopt, {}}};
  core::ResolvedLifecycleProviderOccurrenceDTO occurrence{
      domain.control_domain_id,
      domain.control_domain_revision,
      provider.provider_instance_id,
      provider.provider_manifest_id,
      provider.provider_manifest_revision,
      provider.provider_runtime_unit_id,
      epoch,
      provider.lifecycle_plan_catalog_digest};
  auto made = host_detail::IoProviderDriverRecord::Create(
      scope, occurrence, manifest->value(), clock, 1000, 1'000'000);
  ASSERT_TRUE(made.ok());
  auto driver = std::move(made.value());
  EXPECT_FALSE(RetainedHostLifecycleProduct::Create(
                   fixture.bundle, manifests, {}, {}, *clock, 1000, 1'000'000, {driver})
                   .ok());
  const auto rows = deployment::ResolveRetainedIoSignalCatalog(
      fixture.bundle->value().cell_graph);
  ASSERT_TRUE(rows.ok());
  ASSERT_EQ(rows.value().size(), 1U);
  class Ingress final : public core::IIoSignalIngress {
  public:
    bool TryEnqueue(const core::IoSourceAcquisitionDTO &) noexcept override {
      ++frames;
      return true;
    }
    void
    NotifyLoss(const core::IoIngressLossNotificationDTO &) noexcept override {}
    unsigned frames{};
  };
  const auto ingress = std::make_shared<Ingress>();
  {
    auto lock = driver->AcquireDelivery();
    ASSERT_TRUE(driver
                    ->InstallResourcesLocked(
                        {{rows.value(),
                          {},
                          ingress,
                          core::IoSourceIncarnation("actual.apply"),
                          std::nullopt}},
                        {}, {})
                    .ok());
  }
  auto product = RetainedHostLifecycleProduct::Create(
      fixture.bundle, manifests, {}, {}, *clock, 1000, 1'000'000, {driver});
  ASSERT_TRUE(product.ok()) << product.error().message;
  ASSERT_EQ(product.value()->brand_ports().size(), 1U);
  EXPECT_EQ(product.value()->brand_ports().front().provider_occurrence,
            occurrence);
  EXPECT_EQ(product.value()->brand_ports().front().port,
            driver->brand_registration().port);
  const std::weak_ptr<host_detail::IoProviderDriverRecord> actual_driver =
      driver;
  {
    core::FixedCCLCPortRegistry registry(product.value()->brand_ports(), {});
    product.value().reset();
    driver.reset();
    ASSERT_FALSE(actual_driver.expired());
    const auto health = registry.ReadExactLifecycleHealth(occurrence, 0);
    ASSERT_TRUE(health.ok());
    EXPECT_EQ(health.value().observed_state,
              core::ComponentLifecycleStateDTO::kDeclared);
    EXPECT_EQ(ingress->frames, 0U);
  }
  EXPECT_TRUE(actual_driver.expired());
}

// Live defect (2026-10-27, Real Lite 6 with its no-output pneumatic gripper):
// the gripper domain shared the Real arm's catalog, whose only Recover target
// (`disabled`) is eligible only for a degraded/faulted arm, so no target was
// common and Recover failed `no_common_recover_target` before `clean_error`.
namespace real_recover {

constexpr std::string_view kRevision =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";

[[nodiscard]] core::BrandLifecyclePlanCatalogDTO
Catalog(std::string_view id, std::vector<core::LifecycleIntentPlanDTO> plans) {
  auto made = core::MakeBrandLifecyclePlanCatalog(
      core::ProviderManifestId(std::string(id)),
      core::ProviderManifestRevision(std::string(kRevision)), std::move(plans));
  if (!made.ok()) throw std::logic_error(made.error().message);
  return made.value();
}

[[nodiscard]] core::LifecycleFactDTO Fact(std::string_view id, bool value) {
  return {core::LifecycleFactId(std::string(id)),
          {core::LifecycleFactValueKindDTO::kBoolean, value, std::nullopt,
           std::nullopt}};
}

[[nodiscard]] core::LifecycleFactDTO
Cause(core::LifecycleBlockingCauseClassDTO cause) {
  return {core::LifecycleFactId(std::string(core::kLifecycleBlockingCauseFactId)),
          {core::LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt,
           std::string(core::to_string(cause))}};
}

/// One domain's catalog and fresh observation.
struct RecoverInput final {
  core::BrandLifecyclePlanCatalogDTO lifecycle_plan_catalog;
  core::BrandLifecycleObservationDTO fresh_observation;
};

/// V-B2: the domain's per-target answers from its adapter's admission.
[[nodiscard]] std::array<core::LifecyclePostconditionResultDTO, 3>
Targets(const RecoverInput &input) {
  const auto targets = core::CatalogLifecycleAdmission(input.lifecycle_plan_catalog)
                           .RecoverTargets(input.fresh_observation);
  if (!targets.ok()) throw std::logic_error(targets.error().message);
  return targets.value();
}

[[nodiscard]] RecoverInput
Input(std::string_view domain, std::string_view instance,
      core::BrandLifecyclePlanCatalogDTO catalog,
      core::ComponentLifecycleStateDTO state,
      std::vector<core::LifecycleFactDTO> facts) {
  std::sort(facts.begin(), facts.end(), [](const auto &left, const auto &right) {
    return left.lifecycle_fact_id < right.lifecycle_fact_id;
  });
  core::ResolvedLifecycleProviderOccurrenceDTO occurrence{
      core::ControlDomainId(std::string(domain)),
      core::ControlDomainRevision("domain-revision"),
      core::ProviderInstanceId(std::string(instance)),
      catalog.provider_manifest_id,
      catalog.provider_manifest_revision,
      core::RuntimeUnitId("robot_cell.ros2_control_execution"),
      core::RuntimeEpoch("runtime-epoch.1"),
      catalog.catalog_digest};
  core::BrandLifecycleObservationDTO observation{
      core::LifecycleTransactionId("lifecycle.1"),
      core::LifecycleTransactionGeneration(1U),
      core::ControllableComponentId("lite6_robot"),
      occurrence.control_domain_id,
      occurrence.control_domain_revision,
      occurrence.provider_instance_id,
      occurrence.provider_manifest_id,
      occurrence.provider_manifest_revision,
      occurrence.provider_runtime_unit_id,
      occurrence.provider_runtime_epoch,
      1U,
      1U,
      10,
      20,
      30,
      state,
      std::move(facts)};
  return {std::move(catalog), std::move(observation)};
}

/// The Real arm after an emergency stop at start-up: not armed, vendor error.
[[nodiscard]] RecoverInput EmergencyStoppedArm() {
  return Input(
      "lite6_robot.arm.control", "lite6_robot.ros2_control",
      Catalog(xarm_control::kXArmRos2ControlLifecycleProviderManifestId,
              xarm_control::make_xarm_ros2_control_lifecycle_plans()),
      core::ComponentLifecycleStateDTO::kFaulted,
      {Fact(core::kDeviceAcceptsCommandsFactId, false),
       Fact(core::kDeviceErrorPresentFactId, true),
       Fact(core::kDeviceWarningPresentFactId, false),
       Fact("external.broadcaster_active", true),
       Fact("external.controller_active", false),
       Fact("external.controller_configuration_ready", false),
       Fact("external.controller_inactive_unrequested", false),
       Fact("external.controller_loaded", false),
       Fact("external.controller_spawned", true),
       Fact("external.fault_present", true),
       Fact("external.hardware_active", false),
       Fact("external.measured_joints_valid", true),
       Fact("external.transport_ready", true),
       Cause(core::LifecycleBlockingCauseClassDTO::kEmergencyStop)});
}

/// The no-output pneumatic gripper: its logical owner is ready (active).
[[nodiscard]] RecoverInput
NoOutputGripper(std::string_view manifest, std::string_view instance,
                std::vector<core::LifecycleIntentPlanDTO> plans) {
  return Input("lite6_robot.gripper.control", instance,
               Catalog(manifest, std::move(plans)),
               core::ComponentLifecycleStateDTO::kActive,
               {Fact("external.controller_configuration_ready", true),
                Fact("external.controller_inactive_unrequested", false),
                Fact("external.controller_loaded", true),
                Fact("external.fault_present", false),
                Fact("external.transport_ready", true),
                Cause(core::LifecycleBlockingCauseClassDTO::kNone)});
}

} // namespace real_recover

TEST(RetainedRos2ControlLifecycleProductTest,
     RealArmWithNoOutputGripperRecoversToDisabledAndClearsTheArmError) {
  using Intent = core::ComponentLifecycleIntentKindDTO;
  using State = core::ComponentLifecycleStateDTO;
  const auto arm = real_recover::EmergencyStoppedArm();
  const auto gripper = real_recover::NoOutputGripper(
      xarm_control::kXArmRos2ControlGripperLifecycleProviderManifestId,
      "lite6_robot.ros2_control.gripper",
      retained_ros2_control_gripper_lifecycle_plans());

  // The live failure: the gripper on the arm's catalog has no common target.
  const auto shared = real_recover::NoOutputGripper(
      xarm_control::kXArmRos2ControlLifecycleProviderManifestId,
      "lite6_robot.ros2_control",
      xarm_control::make_xarm_ros2_control_lifecycle_plans());
  const auto refused = core::SelectCommonRecoverTarget(
      {real_recover::Targets(arm), real_recover::Targets(shared)});
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_lifecycle_recover_target_missing");
  EXPECT_EQ(refused.error().details.at("reason"), "no_common_recover_target");

  // Fixed: the gripper's own provider shares the arm's `disabled` target.
  const auto target = core::SelectCommonRecoverTarget(
      {real_recover::Targets(arm), real_recover::Targets(gripper)});
  ASSERT_TRUE(target.ok()) << target.error().message;
  EXPECT_EQ(target.value(), State::kDisabled);
  // The arm dispatches the vendor `clean_error`...
  const auto arm_step =
      core::EvaluateLifecycleDomainPlan(Intent::kRecover, target.value(),
                                        arm.lifecycle_plan_catalog,
                                        arm.fresh_observation);
  ASSERT_TRUE(arm_step.ok()) << arm_step.error().message;
  ASSERT_EQ(arm_step.value().kind,
            core::LifecycleDomainPlanDecisionKind::kDispatchPrimitive);
  ASSERT_TRUE(arm_step.value().selected_transition.has_value());
  EXPECT_EQ(arm_step.value().selected_transition->lifecycle_primitive_id.value(),
            core::kDeviceClearErrorPrimitiveId);
  // ...while the healthy gripper has nothing to clear and dispatches nothing.
  const auto gripper_step =
      core::EvaluateLifecycleDomainPlan(Intent::kRecover, target.value(),
                                        gripper.lifecycle_plan_catalog,
                                        gripper.fresh_observation);
  ASSERT_TRUE(gripper_step.ok()) << gripper_step.error().message;
  EXPECT_EQ(gripper_step.value().kind,
            core::LifecycleDomainPlanDecisionKind::kAlreadySatisfied);

  // The target driver's next Prepare/Enable need nothing from the active
  // gripper (shared plans, as on Mock and Isaac).
  for (const auto &[intent, state] :
       {std::pair{Intent::kPrepare, State::kPrepared},
        std::pair{Intent::kEnable, State::kActive}}) {
    const auto next = core::EvaluateLifecycleDomainPlan(
        intent, state, gripper.lifecycle_plan_catalog, gripper.fresh_observation);
    ASSERT_TRUE(next.ok()) << next.error().message;
    EXPECT_EQ(next.value().kind,
              core::LifecycleDomainPlanDecisionKind::kAlreadySatisfied);
  }
}

TEST(RetainedRos2ControlLifecycleProductTest,
     MaintainedRealGripperManifestAnchorsTheGripperPlans) {
  // The generator's `xarm_ros2_control_gripper_lifecycle_plans` mirrors these
  // plans; resources/maintained/catalog/provider_manifest/
  // manyforge.provider.ros2_control.xarm.gripper.json carries this digest.
  const auto semantic = core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
      core::ProviderManifestId(std::string(
          xarm_control::kXArmRos2ControlGripperLifecycleProviderManifestId)),
      retained_ros2_control_gripper_lifecycle_plans());
  ASSERT_TRUE(semantic.ok()) << semantic.error().message;
  EXPECT_EQ(semantic.value().value(),
            "sha256:0ee1390f57b4c69e521124f96068fd4b7903ad0d423b7aadef66bf1dfcd37d4f");
  // Only the Recover row differs from the shared plans.
  auto plans = retained_ros2_control_gripper_lifecycle_plans();
  auto shared = retained_ros2_control_lifecycle_plans();
  ASSERT_EQ(plans.size(), shared.size());
  EXPECT_EQ(plans.back().intent, core::ComponentLifecycleIntentKindDTO::kRecover);
  EXPECT_EQ(plans.back().requested_state, core::ComponentLifecycleStateDTO::kDisabled);
  EXPECT_TRUE(plans.back().transitions.empty());
  plans.pop_back();
  shared.pop_back();
  EXPECT_EQ(plans, shared);
}

// V-A: the vendor registry has the xArm family's row, from its library; the
// persisted provider ids are unchanged.
TEST(RetainedRos2ControlLifecycleProductTest, VendorRegistryHoldsTheXArmFamilyRow) {
  const auto *xarm = find_vendor_adapter_family("xarm");
  ASSERT_NE(xarm, nullptr);
  EXPECT_EQ(xarm->vendor, "xarm");
  EXPECT_EQ(xarm->arm_provider_manifest_id, "manyforge.provider.ros2_control.xarm");
  EXPECT_EQ(xarm->gripper_provider_manifest_id,
            "manyforge.provider.ros2_control.xarm.gripper");
  ASSERT_NE(xarm->make_adapter, nullptr);
  ASSERT_NE(xarm->make_arm_catalog, nullptr);
  EXPECT_EQ(find_vendor_adapter_family("ur"), nullptr);
  EXPECT_EQ(find_vendor_adapter_family(""), nullptr);

  const auto arm = find_vendor_lifecycle_provider(
      xarm_control::kXArmRos2ControlLifecycleProviderManifestId);
  ASSERT_TRUE(arm.has_value());
  EXPECT_EQ(arm->family, xarm);
  EXPECT_EQ(arm->role, VendorLifecycleProviderRole::kArm);
  const auto gripper = find_vendor_lifecycle_provider(
      xarm_control::kXArmRos2ControlGripperLifecycleProviderManifestId);
  ASSERT_TRUE(gripper.has_value());
  EXPECT_EQ(gripper->family, xarm);
  EXPECT_EQ(gripper->role, VendorLifecycleProviderRole::kGripper);
  EXPECT_FALSE(find_vendor_lifecycle_provider(kRos2ControlLifecycleProviderManifestId)
                   .has_value());
  EXPECT_FALSE(find_vendor_lifecycle_provider("xarm_arm_provider").has_value());
}

// V-A: the adapter factory routes by family; an unknown family still throws,
// and the xArm adapter still validates its data before any ROS effect.
TEST(RetainedRos2ControlLifecycleProductTest, VendorAdapterFactoryRoutesByFamily) {
  const VendorAdapterSelection xarm{.vendor = "xarm",
                                    .driver_namespace = "/lite6_robot/xarm",
                                    .axis_count = 6,
                                    .joint_agreement_tolerance_rad = 0.01};
  const auto adapter = make_retained_vendor_device_status_adapter(xarm);
  ASSERT_NE(adapter, nullptr);
  // Not started: the status is unavailable.
  EXPECT_EQ(adapter->ReadHealth(0).cause,
            core::LifecycleBlockingCauseClassDTO::kAvailability);

  auto unknown = xarm;
  unknown.vendor = "ur";
  EXPECT_THROW(static_cast<void>(make_retained_vendor_device_status_adapter(unknown)),
               std::invalid_argument);
  auto no_axes = xarm;
  no_axes.axis_count = 0;
  EXPECT_THROW(static_cast<void>(make_retained_vendor_device_status_adapter(no_axes)),
               std::invalid_argument);
  auto relative = xarm;
  relative.driver_namespace = "lite6_robot/xarm";
  EXPECT_THROW(static_cast<void>(make_retained_vendor_device_status_adapter(relative)),
               std::invalid_argument);
}

namespace vendor_catalog {

/// A lifecycle provider manifest anchoring `plans` under `id`.
[[nodiscard]] core::ProviderManifestDTO
Manifest(const std::string_view id,
         const std::vector<core::LifecycleIntentPlanDTO> &plans) {
  const core::ProviderManifestId manifest_id{std::string(id)};
  const auto semantic =
      core::ComputeBrandLifecyclePlanCatalogSemanticDigest(manifest_id, plans);
  if (!semantic.ok()) throw std::logic_error(semantic.error().message);
  std::vector<core::ProviderCapabilityOfferDTO> offers;
  for (const std::string_view capability :
       {"component.lifecycle.disable", "component.lifecycle.enable",
        "component.lifecycle.observe", "component.lifecycle.prepare",
        "component.lifecycle.recover"}) {
    const auto *definition = core::find_builtin_capability_definition(capability);
    if (definition == nullptr) throw std::logic_error("missing definition");
    offers.push_back({core::CapabilityId(std::string(capability)),
                      core::CapabilityDefinitionRevision(
                          std::string(definition->capability_definition_revision)),
                      {}});
  }
  return {manifest_id,
          core::ProviderManifestRevision(
              "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
          std::move(offers),
          {},
          core::ProviderLifecyclePlanCatalogDeclarationDTO{
              core::SchemaId(std::string(core::kBrandLifecyclePlanCatalogApiVersion)),
              semantic.value()}};
}

} // namespace vendor_catalog

// R7 package 4 through the V-A registry: the xArm arm provider gets the xArm
// catalog, its gripper provider the Real gripper plans, the shared provider
// the shared catalog; an unregistered vendor provider is refused.
TEST(RetainedRos2ControlLifecycleProductTest,
     PhysicalCatalogFollowsTheVendorRegistry) {
  const auto arm_manifest = vendor_catalog::Manifest(
      xarm_control::kXArmRos2ControlLifecycleProviderManifestId,
      xarm_control::make_xarm_ros2_control_lifecycle_plans());
  const auto arm = make_retained_physical_ros2_control_lifecycle_catalog(arm_manifest);
  ASSERT_TRUE(arm.ok()) << arm.error().message;
  const auto xarm = xarm_control::make_xarm_ros2_control_lifecycle_catalog(arm_manifest);
  ASSERT_TRUE(xarm.ok()) << xarm.error().message;
  EXPECT_EQ(arm.value(), xarm.value());

  const auto gripper_manifest = vendor_catalog::Manifest(
      xarm_control::kXArmRos2ControlGripperLifecycleProviderManifestId,
      retained_ros2_control_gripper_lifecycle_plans());
  const auto gripper =
      make_retained_physical_ros2_control_lifecycle_catalog(gripper_manifest);
  ASSERT_TRUE(gripper.ok()) << gripper.error().message;
  const auto real_gripper = core::MakeBrandLifecyclePlanCatalog(
      gripper_manifest.provider_manifest_id,
      gripper_manifest.provider_manifest_revision,
      retained_ros2_control_gripper_lifecycle_plans());
  ASSERT_TRUE(real_gripper.ok()) << real_gripper.error().message;
  EXPECT_EQ(gripper.value(), real_gripper.value());

  const auto shared_manifest = vendor_catalog::Manifest(
      kRos2ControlLifecycleProviderManifestId, retained_ros2_control_lifecycle_plans());
  const auto shared =
      make_retained_physical_ros2_control_lifecycle_catalog(shared_manifest);
  ASSERT_TRUE(shared.ok()) << shared.error().message;
  const auto expected_shared = make_retained_ros2_control_lifecycle_catalog(shared_manifest);
  ASSERT_TRUE(expected_shared.ok()) << expected_shared.error().message;
  EXPECT_EQ(shared.value(), expected_shared.value());

  const auto unregistered = make_retained_physical_ros2_control_lifecycle_catalog(
      vendor_catalog::Manifest("manyforge.provider.ros2_control.ur",
                               retained_ros2_control_lifecycle_plans()));
  ASSERT_FALSE(unregistered.ok());
  EXPECT_EQ(unregistered.error().code, "ros2_control_lifecycle_product_unsupported");
}

} // namespace
} // namespace manyforge::roboplan_backend
