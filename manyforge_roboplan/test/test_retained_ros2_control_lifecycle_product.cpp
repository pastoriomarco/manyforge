#include "../src/io_provider_lifecycle_product.hpp"
#include "io_retained_fixture.hpp"
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
      fixture.bundle, manifests, {binding}, {handle}, clock, 1000);
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
      scope, occurrence, manifest->value(), clock, 1000);
  ASSERT_TRUE(made.ok());
  auto driver = std::move(made.value());
  EXPECT_FALSE(RetainedHostLifecycleProduct::Create(
                   fixture.bundle, manifests, {}, {}, *clock, 1000, {driver})
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
      fixture.bundle, manifests, {}, {}, *clock, 1000, {driver});
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

} // namespace
} // namespace manyforge::roboplan_backend
