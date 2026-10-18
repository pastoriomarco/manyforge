#include "retained_lifecycle_product_fixture.hpp"

#include <gtest/gtest.h>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_roboplan/retained_ros2_control_lifecycle_product.hpp>

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

TEST(RetainedLifecycleProductTest,
     ExactRegistryReadsActualOwnedPureHealthWithoutObservation) {
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
  auto handle = std::make_shared<RetainedRos2ControlRuntimeHandle>();
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
  std::optional<core::ErrorDTO> source_error;
  handle->InstallControllerProcessFaultSource(
      [&]() -> std::optional<std::string> {
        ++observes;
        return "process absent";
      },
      [&]() -> core::Result<std::optional<std::string>> {
        ++pure_reads;
        if (source_error)
          return core::Result<std::optional<std::string>>::Err(*source_error);
        return core::Result<std::optional<std::string>>::Ok("process absent");
      });
  const deployment::FilesystemVerifiedRetainedProviderManifestSource manifests(
      root.path());
  auto product = RetainedHostLifecycleProduct::Create(
      fixture.bundle, manifests, {binding}, {handle}, clock, 1000);
  ASSERT_TRUE(product.ok()) << product.error().message;
  ASSERT_EQ(product.value()->brand_ports().size(), 1U);
  const auto occurrence =
      product.value()->brand_ports().front().provider_occurrence;
  auto projection = product.value()->brand_ports().front().health_projection;
  ASSERT_TRUE(projection);
  const std::weak_ptr<core::IBrandLifecyclePort> weak_port =
      product.value()->brand_ports().front().port;
  const std::weak_ptr<RetainedRos2ControlRuntimeHandle> weak_handle = handle;
  const auto before = handle->ReadOwnedControllerOccurrence();
  {
    core::FixedCCLCPortRegistry registry(product.value()->brand_ports(), {});
    product.value().reset();
    handle.reset();
    ASSERT_FALSE(weak_handle.expired());
    ASSERT_FALSE(weak_port.expired());
    for (int read = 0; read < 3; ++read) {
      const auto health =
          registry.ReadExactLifecycleHealth(occurrence, 200 + read);
      ASSERT_TRUE(health.ok()) << health.error().message;
      EXPECT_EQ(health.value().observed_state,
                core::ComponentLifecycleStateDTO::kFaulted);
      ASSERT_TRUE(health.value().diagnostic);
      EXPECT_EQ(health.value().diagnostic->message, "process absent");
    }
    EXPECT_EQ(observes, 0U);
    EXPECT_EQ(pure_reads, 3U);
    auto foreign = occurrence;
    foreign.provider_runtime_epoch = core::RuntimeEpoch("foreign.epoch");
    const auto reads_before = pure_reads;
    const auto rejected = registry.ReadExactLifecycleHealth(foreign, 200);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().details.at("reason"),
              "provider_occurrence_not_registered");
    EXPECT_EQ(pure_reads, reads_before);
    // Caller time reaches the pure projection; it does not read Observe's clock.
    clock.set(-1);
    EXPECT_TRUE(registry.ReadExactLifecycleHealth(occurrence, 200).ok());
    EXPECT_FALSE(projection(-1).ok());
    clock.set(100);
    source_error = core::ErrorDTO{
        "process_health_unavailable", "selected process source unavailable",
        {{"domain", domain.value()}}};
    const auto absent = registry.ReadExactLifecycleHealth(occurrence, 200);
    ASSERT_FALSE(absent.ok());
    EXPECT_EQ(absent.error().code, source_error->code);
    EXPECT_EQ(absent.error().message, source_error->message);
    EXPECT_EQ(absent.error().details, source_error->details);
    source_error.reset();
    EXPECT_EQ(weak_handle.lock()->ReadOwnedControllerOccurrence(), before);
    EXPECT_EQ(observes, 0U);
    auto port = registry.ResolveExactLifecyclePort(occurrence);
    ASSERT_TRUE(port.ok());
    const auto first = port.value()->Observe(product_test::Query(
        occurrence, binding.value().controllable_component_id));
    ASSERT_TRUE(first.ok()) << first.error().message;
    EXPECT_EQ(first.value().source_sequence, 1U);
    EXPECT_EQ(observes, 1U);
  }
  EXPECT_TRUE(weak_port.expired());
  ASSERT_FALSE(weak_handle.expired());
  EXPECT_TRUE(projection(200).ok());
  projection = {};
  EXPECT_TRUE(weak_port.expired());
  EXPECT_TRUE(weak_handle.expired());
}

} // namespace
} // namespace manyforge::roboplan_backend
