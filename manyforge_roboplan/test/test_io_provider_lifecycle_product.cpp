#include "../src/component_runtime_pins.hpp"
#include "../src/host_io_signal_runtime.hpp"
#include "../src/io_provider_lifecycle_product.hpp"
#include "../src/simulated_plc_readiness.hpp"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
#include <chrono>
#include <control_msgs/msg/dynamic_interface_group_values.hpp>
#include <gtest/gtest.h>
#include <limits>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <manyforge_core/io_condition_evaluator.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <std_srvs/srv/set_bool.hpp>

namespace manyforge::roboplan_backend::host_detail {
namespace {

using Outcome = RetainedRos2ControlSwitchOutcome;
const core::ContentDigest kDigest("sha256:" + std::string(64U, 'a'));
const core::ControlDomainId kDomain("io.domain");
const core::RuntimeUnitId kUnit("shared.host");
const core::InterruptSourceId kSource("io.source");
const core::IoSignalId kSignal{core::ControllableComponentId("io.device"),
                               "ready"};

class CapturingIngress final : public core::IIoSignalIngress {
public:
  bool TryEnqueue(const core::IoSourceAcquisitionDTO &frame) noexcept override {
    frames.push_back(frame);
    return true;
  }
  void
  NotifyLoss(const core::IoIngressLossNotificationDTO &loss) noexcept override {
    losses.push_back(loss);
  }
  std::vector<core::IoSourceAcquisitionDTO> frames;
  std::vector<core::IoIngressLossNotificationDTO> losses;
};

const core::CollisionBodyId
    kGeometricEndpoint("component/robot.arm/model/arm/body/tool/collision/0");
const core::CollisionBodyId kGeometricTarget("scene/fixture/shape/0");

class GeometricSnapshotPublisher final
    : public core::ICollisionWorldSnapshotPublisher {
public:
  GeometricSnapshotPublisher() {
    auto world = std::make_shared<core::CollisionWorldSnapshot>();
    core::CollisionWorldBody endpoint{};
    endpoint.body_id = kGeometricEndpoint;
    core::CollisionWorldBody target{};
    target.body_id = kGeometricTarget;
    world->bodies = {std::move(endpoint), std::move(target)};
    std::sort(world->bodies.begin(), world->bodies.end(),
              [](const auto &left, const auto &right) {
                return left.body_id < right.body_id;
              });
    bundle_ = std::make_shared<core::RuntimeCollisionQueryBundle>();
    bundle_->world = std::move(world);
    bundle_->lookup_order_valid = true;
  }
  core::CollisionWorldSnapshotPtr snapshot() const override {
    return bundle_->world;
  }
  core::RuntimeCollisionQueryBundlePtr
  published_runtime_collision_query_bundle() const override {
    return bundle_;
  }

private:
  std::shared_ptr<core::RuntimeCollisionQueryBundle> bundle_;
};

class AdvancingCollisionEngine final : public core::ICollisionQueryEngine {
public:
  explicit AdvancingCollisionEngine(core::FakeClock &clock) : clock_(clock) {
    capability_.runtime_query_max_joints = 4;
    capability_.runtime_query_max_world_bodies = 4;
    capability_.runtime_query_max_collision_pairs = 4;
  }
  const core::CollisionEngineCapabilityDescriptor &capability() const override {
    return capability_;
  }
  core::CollisionResult
  check(const core::CollisionWorldSnapshot &, const core::CollisionQuery &,
        const core::CollisionPolicySnapshot &,
        const core::CollisionCancellationToken &) override {
    return {};
  }
  core::RuntimeCollisionQueryPrepareStatus
  prepare_runtime_query(std::span<const std::string>) noexcept override {
    return core::RuntimeCollisionQueryPrepareStatus::kReady;
  }
  core::RuntimeCollisionQueryPrepareStatus prepare_runtime_snapshot(
      const core::CollisionWorldSnapshot &) noexcept override {
    return core::RuntimeCollisionQueryPrepareStatus::kReady;
  }
  core::RuntimeCollisionQueryResultView
  check_runtime(const core::CollisionWorldSnapshot &,
                const core::RuntimeCollisionQueryRequest &) noexcept override {
    ++queries;
    if (duration_ns > 0)
      clock_.advance(duration_ns);
    return {core::RuntimeCollisionQueryStatus::kClear,
            core::RuntimeCollisionQueryFailureReason::kNone,
            core::CollisionEngineStatus::kOk,
            duration_ns,
            {},
            {}};
  }
  std::string engine_id() const override { return "lifecycle-test"; }
  std::string engine_version() const override { return "1"; }

  core::Duration duration_ns{};
  std::size_t queries{};

private:
  core::FakeClock &clock_;
  core::CollisionEngineCapabilityDescriptor capability_;
};

core::ProviderManifestDTO manifest(
    std::string_view manifest_id = core::kIoProviderLifecycleManifestId) {
  const auto plans = core::MakeIoProviderLifecyclePlans();
  EXPECT_TRUE(plans.ok());
  const core::ProviderManifestId id{std::string(manifest_id)};
  const auto semantic =
      core::ComputeBrandLifecyclePlanCatalogSemanticDigest(id, plans.value());
  EXPECT_TRUE(semantic.ok());
  std::vector<core::ProviderCapabilityOfferDTO> offers;
  for (const auto *name :
       {"component.lifecycle.observe", "component.lifecycle.prepare",
        "component.lifecycle.enable", "component.lifecycle.disable",
        "component.lifecycle.recover"}) {
    const auto *definition = core::find_builtin_capability_definition(name);
    EXPECT_NE(definition, nullptr);
    offers.push_back({core::CapabilityId(name),
                      core::CapabilityDefinitionRevision(std::string(
                          definition->capability_definition_revision)),
                      {}});
  }
  return {id,
          core::ProviderManifestRevision(kDigest.value()),
          std::move(offers),
          {},
          core::ProviderLifecyclePlanCatalogDeclarationDTO{
              core::SchemaId(
                  std::string(core::kBrandLifecyclePlanCatalogApiVersion)),
              semantic.value()}};
}

core::LifecycleBlockingCauseClassDTO
cause(const std::vector<core::LifecycleFactDTO> &facts) {
  const auto found =
      std::find_if(facts.begin(), facts.end(), [](const auto &fact) {
        return fact.lifecycle_fact_id.value() ==
               core::kLifecycleBlockingCauseFactId;
      });
  if (found == facts.end() || !found->value.token_value)
    throw std::logic_error("actual driver standard cause is absent");
  const auto parsed =
      core::parse_lifecycle_blocking_cause_class(*found->value.token_value);
  if (!parsed)
    throw std::logic_error("actual driver standard cause is invalid");
  return *parsed;
}

class IoProviderLifecycleProductTest : public ::testing::Test {
protected:
  void TearDown() override { driver.reset(); }
  void Build(bool output_only = false, std::string epoch = "runtime.1",
             std::vector<std::string> pose_streams = {},
             std::string_view observer_manifest =
                 core::kPoseSourceLifecycleManifestId) {
    const bool pose = !pose_streams.empty();
    auto provider_manifest =
        pose ? manifest(observer_manifest) : manifest();
    const auto catalog =
        core::MakeIoProviderLifecycleCatalog(provider_manifest);
    ASSERT_TRUE(catalog.ok());
    core::ProviderControlDomainDTO provider{
        kSignal.controllable_component_id,
        kUnit,
        core::ProviderInstanceId("io.provider"),
        provider_manifest.provider_manifest_id,
        provider_manifest.provider_manifest_revision,
        catalog.value().catalog_digest,
        kDigest,
        pose ? pose_streams : std::vector<std::string>{kSignal.name},
        30'000'000'000LL};
    std::vector<core::ResourceId> resources;
    if (output_only)
      resources.emplace_back("output.channel");
    core::ResolvedLifecycleControlDomainScopeDTO scope{
        kDomain, core::ControlDomainRevision(kDigest.value()),
        core::ResolvedProviderDomainScopeDTO{
            provider, core::RuntimeEpoch(epoch), std::nullopt, resources}};
    core::ResolvedLifecycleProviderOccurrenceDTO occurrence{
        kDomain,
        scope.control_domain_revision,
        provider.provider_instance_id,
        provider.provider_manifest_id,
        provider.provider_manifest_revision,
        kUnit,
        core::RuntimeEpoch(epoch),
        catalog.value().catalog_digest};
    auto created = IoProviderDriverRecord::Create(
        scope, occurrence, provider_manifest, clock, 1000, 1'000'000);
    ASSERT_TRUE(created.ok()) << created.error().message;
    driver = std::move(created.value());
    descriptor = core::IoSignalDescriptorDTO{
        kSignal,
        kSource,
        core::IoSignalValueType::kBoolean,
        output_only ? core::IoSignalDirection::kOutput
                    : core::IoSignalDirection::kInput,
        core::IoSignalUpdateMode::kChangeOnly,
        core::IoTransitionCapability::kSampledOnly,
        {core::IoFreshnessMode::kProviderValidity, std::nullopt},
        output_only ? std::optional(resources.front()) : std::nullopt,
        output_only ? std::nullopt
                    : std::optional(core::IoSignalLivenessMode::kInProcess)};
  }

  IoProviderInputSourceInstallation
  Input(std::string incarnation = "apply.1") const {
    deployment::IoSignalProviderConfiguration config{
        "mock", {}, {}, false, 8};
    return {{{kUnit, core::CommandTargetId("gpio"),
              core::ResourceRoleId("ready"), *descriptor, config}},
            {},
            ingress,
            core::IoSourceIncarnation(std::move(incarnation)),
            std::nullopt};
  }

  core::BrandLifecyclePrimitiveIntentDTO
  Intent(const char *primitive, std::uint64_t generation) const {
    const auto &provider = driver->occurrence();
    return {
        core::LifecycleTransactionId("transaction"),
        core::LifecycleTransactionGeneration(1U),
        kSignal.controllable_component_id,
        kDomain,
        provider.control_domain_revision,
        provider.provider_instance_id,
        provider.provider_manifest_id,
        provider.provider_manifest_revision,
        provider.provider_runtime_unit_id,
        provider.provider_runtime_epoch,
        core::LifecyclePrimitiveId(std::string("io.provider.") + primitive),
        core::LifecyclePrimitiveAttemptId(std::string("attempt.") + primitive),
        generation,
        1'000'000,
        core::ComponentLifecycleStateDTO::kUndeclared};
  }

  RetainedExternallyManagedPrimitiveOutcome Step(const char *primitive) {
    const auto snapshot = driver->ReadLifecycleSnapshot(kDomain, clock->now());
    EXPECT_TRUE(snapshot.ok());
    return driver->Dispatch(
        Intent(primitive, snapshot.value().source_generation), {});
  }

  void InstallInput() {
    auto lock = driver->AcquireDelivery();
    const auto installed =
        driver->InstallResourcesLocked({Input()}, {}, [&] { ++cancellations; });
    ASSERT_TRUE(installed.ok()) << installed.error().message;
  }
  std::shared_ptr<core::FakeClock> clock = std::make_shared<core::FakeClock>();
  std::shared_ptr<CapturingIngress> ingress =
      std::make_shared<CapturingIngress>();
  std::shared_ptr<IoProviderDriverRecord> driver;
  std::optional<core::IoSignalDescriptorDTO> descriptor;
  unsigned cancellations{};
};

TEST_F(IoProviderLifecycleProductTest,
       ActualResourcesAndFreshDriverGenerationFenceLifecycleEffects) {
  Build();
  EXPECT_FALSE(driver->resources_installed());
  EXPECT_EQ(Step("prepare").outcome,
            Outcome::kNotSent);
  InstallInput();
  EXPECT_TRUE(driver->resources_installed());
  EXPECT_TRUE(ingress->frames.empty());
  EXPECT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  const auto stale = driver->ReadLifecycleSnapshot(kDomain, clock->now())
                         .value()
                         .source_generation;
  EXPECT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(ingress->frames.size(), 1U);
  EXPECT_EQ(ingress->frames.back().updates.front().value,
            std::optional<bool>(false));
  EXPECT_EQ(driver->Dispatch(Intent("disable", stale), {}).outcome,
            Outcome::kNotSent);
  EXPECT_EQ(driver->ReadLifecycleSnapshot(kDomain, clock->now())
                .value()
                .component_state,
            core::ComponentLifecycleStateDTO::kActive);
  std::stop_source stop;
  stop.request_stop();
  const auto current = driver->ReadLifecycleSnapshot(kDomain, clock->now())
                           .value()
                           .source_generation;
  EXPECT_EQ(
      driver->Dispatch(Intent("disable", current), stop.get_token()).outcome,
      Outcome::kNotSent);
  EXPECT_EQ(cancellations, 2U);
}

TEST_F(IoProviderLifecycleProductTest,
       DisabledManualModelSurvivesEnableWithoutFabricatedDelivery) {
  Build();
  InstallInput();
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("disable").outcome,
            Outcome::kSucceeded);
  const auto before = ingress->frames.size();
  {
    auto lock = driver->AcquireDelivery();
    const auto changed = driver->SetMockValueLocked(kSignal, true);
    ASSERT_TRUE(changed.ok());
    EXPECT_TRUE(changed.value().model_value);
    EXPECT_FALSE(changed.value().enqueued_source_sequence);
  }
  EXPECT_EQ(ingress->frames.size(), before);
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(ingress->frames.size(), before + 1U);
  EXPECT_EQ(ingress->frames.back().updates.front().value,
            std::optional<bool>(true));
}

TEST_F(IoProviderLifecycleProductTest,
       ActualRuntimeDriverEnableAndDisableFenceDeliveryAndPendingScriptedChange) {
  Build();
  auto scheduler = std::make_shared<core::ManualScheduler>();
  std::map<core::InterruptSourceId, HostIoSignalRuntime::SourceInput> sources;
  sources.emplace(kSource,
                  HostIoSignalRuntime::SourceInput{
                      Input().rows, {}, driver->runtime_unit_epoch(),
                      std::nullopt, driver});
  auto runtime =
      HostIoSignalRuntime::Create(core::RequestOwnerEpoch("host"), 1U,
                                  std::move(sources), clock, scheduler);
  {
    auto delivery = driver->AcquireDelivery();
    ASSERT_TRUE(driver
                    ->InstallResourcesLocked(
                        runtime->driver_input_installations().at(driver), {},
                        runtime->cancellation_hook(driver))
                    .ok());
  }
  runtime->Start();
  const auto initial = runtime->ReadSignal(kSignal);
  ASSERT_TRUE(initial.ok());
  EXPECT_NE(initial.value().availability, core::IoSignalAvailability::kFresh);
  EXPECT_FALSE(initial.value().latest_sample);
  ASSERT_TRUE(initial.value().installed_incarnation);
  const auto incarnation = *initial.value().installed_incarnation;
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(runtime->DrainPending().ok());
  scheduler->advance_to(clock->now(), core::SchedulerPriority::RuntimeSafety);
  const auto enabled = runtime->ReadSignal(kSignal).value();
  ASSERT_EQ(enabled.availability, core::IoSignalAvailability::kFresh);
  ASSERT_TRUE(enabled.latest_sample);
  EXPECT_EQ(enabled.latest_sample->publication.value,
            std::optional<bool>(false));
  const core::IoLevelWatchRequestDTO watch{
      {core::RequestOwnerEpoch("runner"),
       core::ProgramRunId("run"),
       {core::ProgramNodeInstanceId("wait"), "attempt"}},
      kSignal,
      true};
  ASSERT_TRUE(runtime->RegisterLevelWatch(watch).ok());
  // Watches only observe. The node's explicit script owns any timed change.
  ASSERT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety),
            0U);
  const core::ProgramNodeMockSimulationRequestDTO simulation{
      {core::RequestOwnerEpoch("host"),
       core::ProgramBindingId(kDigest.value()),
       core::RequestOwnerEpoch("runner"),
       core::CellAdmissionClaimGeneration(1U),
       core::ProgramRunId("run"),
       {core::ProgramNodeInstanceId("wait"), "attempt"},
       1U},
      {{core::TargetRoleId("inputs"), core::ResourceRoleId("ready"), false,
        core::ProgramMockSimulationChangeDTO{250U, true}}}};
  ASSERT_TRUE(runtime->SetProgramRunMockSimulationPaused(
                         {{simulation.scope.request_owner_epoch,
                           simulation.scope.program_binding_id,
                           simulation.scope.program_runner_owner_epoch,
                           simulation.scope.claim_generation,
                           simulation.scope.program_run_id},
                          1U, false, {}})
                  .ok());
  const auto digest = core::ComputeIoSignalDescriptorDigest(enabled.descriptor);
  ASSERT_TRUE(digest.ok());
  const std::vector<core::ResolvedProgramMockSimulationSignalDTO> resolved{
      {kSignal, digest.value(), enabled.cursor.catalog_generation,
       enabled.installed_incarnation}};
  // This service-boundary fixture uses actual installed source observations;
  // captured Program/Run admission is exercised by the Kernel fixtures.
  const auto started =
      runtime->StartProgramNodeMockSimulation(simulation, resolved, 1U);
  ASSERT_TRUE(started.ok()) << started.error().message;
  ASSERT_EQ(started.value().signals.size(), 1U);
  EXPECT_EQ(started.value().signals.front().disposition,
            core::ProgramMockSimulationSignalDisposition::kApplied);
  ASSERT_TRUE(runtime->DrainPending().ok());
  scheduler->advance_to(clock->now(), core::SchedulerPriority::RuntimeSafety);
  ASSERT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety),
            1U);
  ASSERT_EQ(Step("disable").outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(runtime->DrainPending().ok());
  scheduler->advance_to(clock->now(), core::SchedulerPriority::RuntimeSafety);
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety),
            0U);
  const auto changed = runtime->SetMockInput(kSignal, incarnation, true);
  ASSERT_TRUE(changed.ok());
  EXPECT_TRUE(changed.value().model_value);
  EXPECT_FALSE(changed.value().published_cursor);
  EXPECT_NE(changed.value().observation.availability,
            core::IoSignalAvailability::kFresh);
  const auto restored = runtime->SetMockInput(kSignal, incarnation, false);
  ASSERT_TRUE(restored.ok());
  EXPECT_FALSE(restored.value().model_value);
  EXPECT_FALSE(restored.value().published_cursor);
  // The canceled node script cannot overwrite the preserved manual model.
  clock->set(250'000'000LL);
  scheduler->advance_to(clock->now(), core::SchedulerPriority::RuntimeSafety);
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(runtime->DrainPending().ok());
  const auto resumed = runtime->ReadSignal(kSignal).value();
  ASSERT_EQ(resumed.availability, core::IoSignalAvailability::kFresh);
  ASSERT_TRUE(resumed.latest_sample);
  EXPECT_EQ(resumed.latest_sample->publication.value,
            std::optional<bool>(false));
  runtime->Shutdown();
}

TEST_F(IoProviderLifecycleProductTest,
       RealApplyRebindPreservesDriverAndBrandSequenceResetsModelOnce) {
  Build();
  InstallInput();
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  const auto registration = driver->brand_registration();
  const auto &provider = driver->occurrence();
  const core::BrandLifecycleObservationQueryDTO query{
      core::LifecycleTransactionId("read"),
      core::LifecycleTransactionGeneration(1U),
      kSignal.controllable_component_id,
      kDomain,
      provider.control_domain_revision,
      provider.provider_instance_id,
      provider.provider_manifest_id,
      provider.provider_manifest_revision,
      kUnit,
      provider.provider_runtime_epoch};
  const auto before = registration.port->Observe(query);
  ASSERT_TRUE(before.ok());
  {
    auto lock = driver->AcquireDelivery();
    ASSERT_TRUE(driver->SetMockValueLocked(kSignal, true).ok());
    ASSERT_TRUE(
        driver->RebindInputsLocked({Input("apply.2")}, [&] { ++cancellations; })
            .ok());
    const auto count = ingress->frames.size();
    ASSERT_TRUE(
        driver->RebindInputsLocked({Input("apply.2")}, [&] { ++cancellations; })
            .ok());
    EXPECT_EQ(ingress->frames.size(), count);
  }
  EXPECT_EQ(ingress->frames.back().incarnation,
            core::IoSourceIncarnation("apply.2"));
  EXPECT_EQ(ingress->frames.back().updates.front().value,
            std::optional<bool>(false));
  EXPECT_EQ(driver->brand_registration().port, registration.port);
  const auto after = registration.port->Observe(query);
  ASSERT_TRUE(after.ok());
  EXPECT_GT(after.value().source_sequence, before.value().source_sequence);
  EXPECT_GT(after.value().observation_generation,
            before.value().observation_generation);
  EXPECT_EQ(after.value().observed_state,
            core::ComponentLifecycleStateDTO::kActive);
}

TEST_F(IoProviderLifecycleProductTest,
       CompleteImmutablePinRequiresEveryActualMemberAndEpoch) {
  Build();
  const auto first = driver;
  const auto group = std::make_shared<const IoProviderDriverGroup>(
      IoProviderDriverGroup{first});
  const auto pin = std::make_shared<const ComponentRuntimePins>(
      ComponentRuntimePins{{}, {}, group});
  EXPECT_TRUE(pin->matches({}, {}, group));
  Build(false, "runtime.2");
  EXPECT_NE(first->occurrence(), driver->occurrence());
  const auto changed = std::make_shared<const IoProviderDriverGroup>(
      IoProviderDriverGroup{driver});
  EXPECT_FALSE(pin->matches({}, {}, changed));
  const auto omitted = std::make_shared<const IoProviderDriverGroup>();
  EXPECT_FALSE(pin->matches({}, {}, omitted));
  EXPECT_EQ(pin->io_provider_drivers->front(), first);
}

TEST_F(IoProviderLifecycleProductTest,
       OutputOnlyReadinessUsesActualChannelWithoutRequiringAnInputSample) {
  Build(true);
  const core::ResourceId resource("output.channel");
  const auto occurrence = driver->occurrence();
  auto created = MockBooleanOutputProvider::Create(
      *clock, {resource,
               "channel",
               {{kSignal, occurrence, 1U, driver}},
               core::ActiveBundlePublicationGeneration(1U)});
  ASSERT_TRUE(created.ok());
  std::shared_ptr<MockBooleanOutputProvider> output(std::move(created.value()));
  deployment::IoSignalProviderConfiguration config{
      "mock_output", {}, {}, {}, 0, {}, "channel"};
  deployment::RetainedIoSignalConfiguration row{
      kUnit, core::CommandTargetId("gpio"), core::ResourceRoleId("ready"),
      *descriptor, config};
  {
    auto lock = driver->AcquireDelivery();
    ASSERT_TRUE(
        driver->InstallResourcesLocked({}, {{resource, {row}, output}}, {})
            .ok());
  }
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  EXPECT_TRUE(ingress->frames.empty());
  const core::IoOutputWriteRequestDTO write{
      {{core::OperationId("write"), core::OperationRevision(1U),
        core::OperationParticipantId("participant"),
        core::ControllerGoalId("goal"), core::ControllerGoalGeneration(1U)},
       kDigest,
       core::ComponentLifecycleGeneration(7U),
       occurrence},
      kSignal,
      true,
      std::nullopt};
  const auto accepted = output->SubmitOutput(write);
  ASSERT_TRUE(accepted.ok());
  EXPECT_EQ(accepted.value().evidence, core::IoOutputEvidence::kSourceAccepted);
  EXPECT_EQ(output->model_value(), std::optional<bool>(true));
  ASSERT_TRUE(output->RetireOutput(write.identity).ok());
  ASSERT_EQ(Step("disable").outcome,
            Outcome::kSucceeded);
  auto disabled_write = write;
  disabled_write.identity.command.operation_id =
      core::OperationId("write.disabled");
  EXPECT_EQ(output->SubmitOutput(disabled_write).value().evidence,
            core::IoOutputEvidence::kNotSubmitted);
  EXPECT_EQ(output->model_value(), std::optional<bool>(true));
  driver->Shutdown();
  EXPECT_FALSE(output
                   ->Resume(occurrence,
                            std::numeric_limits<std::uint64_t>::max(), driver)
                   .ok());
  EXPECT_EQ(
      output->ObserveOutputResult(disabled_write.identity).value().evidence,
      core::IoOutputEvidence::kNotSubmitted);
  EXPECT_EQ(output->model_value(), std::optional<bool>(true));
}

TEST_F(IoProviderLifecycleProductTest,
       ActualDisconnectedServiceStillAllowsOwnedLocalDisableButNotEnable) {
  Build(true);
  using Service = std_srvs::srv::SetBool;
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  rclcpp::NodeOptions options;
  options.context(context);
  options.use_global_arguments(false);
  auto node = std::make_shared<rclcpp::Node>("local_disable_client", options);
  auto server = std::make_shared<rclcpp::Node>("local_disable_server", options);
  const auto answer = [](Service::Request::SharedPtr,
                         Service::Response::SharedPtr response) {
    response->success = true;
  };
  auto service = server->create_service<Service>("/test/local_disable", answer);
  const core::ResourceId resource("output.channel");
  const auto occurrence = driver->occurrence();
  auto made = RosBooleanOutputProvider::Create(
      node, *clock,
      {resource,
       RosSetBoolOutputAddress{"/test/local_disable"},
       {{kSignal, occurrence, 1U, driver}},
       core::ActiveBundlePublicationGeneration(1U)});
  ASSERT_TRUE(made.ok());
  std::shared_ptr<RosBooleanOutputProvider> output(std::move(made.value()));
  deployment::IoSignalProviderConfiguration configuration{
      "ros_set_bool", {}, {}, {}, 0, "/test/local_disable"};
  deployment::RetainedIoSignalConfiguration row{
      kUnit, core::CommandTargetId("gpio"), core::ResourceRoleId("ready"),
      *descriptor, configuration};
  {
    auto delivery = driver->AcquireDelivery();
    ASSERT_TRUE(driver
                    ->InstallResourcesLocked({}, {{resource, {row}, output}},
                                             [&] { ++cancellations; })
                    .ok());
  }
  rclcpp::ExecutorOptions executor_options;
  executor_options.context = context;
  rclcpp::executors::SingleThreadedExecutor executor(executor_options);
  executor.add_node(node);
  executor.add_node(server);
  const auto wait_ready = [&](bool expected) {
    const auto limit =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (output->ServiceReady() != expected &&
           std::chrono::steady_clock::now() < limit)
      executor.spin_once(std::chrono::milliseconds(5));
    return output->ServiceReady() == expected;
  };
  ASSERT_TRUE(wait_ready(true));
  EXPECT_FALSE(driver->ReadLifecycleHealth(kDomain, clock->now())
                   .value()
                   .diagnostic.has_value());
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  service.reset(); // Actual remote service disappears; local connection owner
                   // survives.
  ASSERT_TRUE(wait_ready(false));
  {
    // Presentation diagnostic names the missing service; facts, cause and the
    // snapshot (lifecycle decision input) stay exactly as before.
    const auto health = driver->ReadLifecycleHealth(kDomain, clock->now());
    const auto snapshot = driver->ReadLifecycleSnapshot(kDomain, clock->now());
    ASSERT_TRUE(health.ok() && snapshot.ok());
    EXPECT_TRUE(health.value().locally_valid());
    EXPECT_EQ(health.value().facts, snapshot.value().facts);
    EXPECT_FALSE(snapshot.value().diagnostic.has_value());
    ASSERT_TRUE(health.value().diagnostic.has_value());
    const auto &diagnostic = *health.value().diagnostic;
    EXPECT_EQ(diagnostic.code, "io_provider_transport_unavailable");
    EXPECT_EQ(diagnostic.message,
              "ROS service /test/local_disable is not available");
    EXPECT_EQ(diagnostic.details.at("ros_name"), "/test/local_disable");
    EXPECT_EQ(diagnostic.details.at("signal_name"), kSignal.name);
  }
  EXPECT_EQ(
      cause(driver->ReadLifecycleSnapshot(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kProviderLost);
  EXPECT_EQ(
      cause(driver->ReadLifecycleHealth(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kProviderLost);
  ASSERT_EQ(Step("disable").outcome,
            Outcome::kSucceeded);
  EXPECT_EQ(driver->ReadLifecycleSnapshot(kDomain, clock->now())
                .value()
                .component_state,
            core::ComponentLifecycleStateDTO::kDisabled);
  EXPECT_EQ(cancellations, 3U);
  EXPECT_EQ(
      cause(driver->ReadLifecycleSnapshot(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kAvailability);
  EXPECT_EQ(Step("prepare").outcome,
            Outcome::kNotSent);
  EXPECT_EQ(Step("enable").outcome,
            Outcome::kNotSent);
  EXPECT_EQ(Step("recover").outcome,
            Outcome::kNotSent);
  service = server->create_service<Service>("/test/local_disable", answer);
  ASSERT_TRUE(wait_ready(true));
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  driver->Shutdown();
  EXPECT_EQ(
      cause(driver->ReadLifecycleSnapshot(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kNone);
  executor.remove_node(node);
  executor.remove_node(server);
  output.reset();
  context->shutdown("local Disable fixture complete");
}

TEST_F(IoProviderLifecycleProductTest,
       InitiallyMissingServiceIsDiagnosedAvailabilityUntilItAppears) {
  Build(true);
  using Service = std_srvs::srv::SetBool;
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  rclcpp::NodeOptions options;
  options.context(context);
  options.use_global_arguments(false);
  auto node = std::make_shared<rclcpp::Node>("missing_service_client", options);
  auto server =
      std::make_shared<rclcpp::Node>("missing_service_server", options);
  const core::ResourceId resource("output.channel");
  const auto occurrence = driver->occurrence();
  auto made = RosBooleanOutputProvider::Create(
      node, *clock,
      {resource,
       RosSetBoolOutputAddress{"/test/plc_io/start"},
       {{kSignal, occurrence, 1U, driver}},
       core::ActiveBundlePublicationGeneration(1U)});
  ASSERT_TRUE(made.ok());
  std::shared_ptr<RosBooleanOutputProvider> output(std::move(made.value()));
  deployment::IoSignalProviderConfiguration configuration{
      "ros_set_bool", {}, {}, {}, 0, "/test/plc_io/start"};
  deployment::RetainedIoSignalConfiguration row{
      kUnit, core::CommandTargetId("gpio"), core::ResourceRoleId("ready"),
      *descriptor, configuration};
  {
    auto delivery = driver->AcquireDelivery();
    ASSERT_TRUE(
        driver->InstallResourcesLocked({}, {{resource, {row}, output}}, [] {})
            .ok());
  }
  const auto catalog = core::MakeIoProviderLifecycleCatalog(manifest());
  ASSERT_TRUE(catalog.ok());
  const auto boolean_fact = [](const auto &facts, std::string_view id) {
    for (const auto &fact : facts)
      if (fact.lifecycle_fact_id.value() == id)
        return fact.value.boolean_value;
    return std::optional<bool>{};
  };
  // Same driver occurrence: missing service is availability, never a fault.
  ASSERT_FALSE(output->ServiceReady());
  auto health = driver->ReadLifecycleHealth(kDomain, clock->now());
  ASSERT_TRUE(health.ok());
  EXPECT_TRUE(health.value().locally_valid());
  EXPECT_EQ(cause(health.value().facts),
            core::LifecycleBlockingCauseClassDTO::kAvailability);
  EXPECT_EQ(boolean_fact(health.value().facts, "io.driver.fault_present"),
            std::optional(false));
  EXPECT_EQ(boolean_fact(health.value().facts, "io.driver.transport_ready"),
            std::optional(false));
  const auto snapshot = driver->ReadLifecycleSnapshot(kDomain, clock->now());
  ASSERT_TRUE(snapshot.ok());
  EXPECT_EQ(health.value().facts, snapshot.value().facts);
  EXPECT_FALSE(snapshot.value().diagnostic.has_value());
  ASSERT_TRUE(health.value().diagnostic.has_value());
  EXPECT_EQ(health.value().diagnostic->code,
            "io_provider_transport_unavailable");
  EXPECT_EQ(health.value().diagnostic->message,
            "ROS service /test/plc_io/start is not available");
  EXPECT_EQ(health.value().diagnostic->details.at("ros_name"),
            "/test/plc_io/start");
  EXPECT_EQ(health.value().diagnostic->details.at("signal_name"),
            kSignal.name);
  auto decision = core::EvaluateLifecycleDomainPlan(
      core::ComponentLifecycleIntentKindDTO::kPrepare,
      core::ComponentLifecycleStateDTO::kPrepared, catalog.value(), occurrence,
      health.value());
  ASSERT_FALSE(decision.ok());
  EXPECT_EQ(decision.error().code, "component_lifecycle_transition_missing");
  EXPECT_EQ(decision.error().details.at("observed_transport_ready"), "false");
  EXPECT_EQ(decision.error().details.at("observed_fault_present"), "false");
  EXPECT_EQ(decision.error().details.at("observed_blocking_cause"),
            "availability");
  EXPECT_EQ(decision.error().details.at("observed_diagnostic_code"),
            "io_provider_transport_unavailable");
  EXPECT_EQ(decision.error().details.at("decisive_fact"),
            "io.driver.transport_ready=false");
  EXPECT_EQ(Step("prepare").outcome,
            Outcome::kNotSent);

  // The service appears: the same observation clears the diagnostic and
  // Prepare becomes eligible and succeeds.
  auto service = server->create_service<Service>(
      "/test/plc_io/start",
      [](Service::Request::SharedPtr, Service::Response::SharedPtr response) {
        response->success = true;
      });
  rclcpp::ExecutorOptions executor_options;
  executor_options.context = context;
  rclcpp::executors::SingleThreadedExecutor executor(executor_options);
  executor.add_node(node);
  executor.add_node(server);
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!output->ServiceReady() && std::chrono::steady_clock::now() < limit)
    executor.spin_once(std::chrono::milliseconds(5));
  ASSERT_TRUE(output->ServiceReady());
  health = driver->ReadLifecycleHealth(kDomain, clock->now());
  ASSERT_TRUE(health.ok());
  EXPECT_FALSE(health.value().diagnostic.has_value());
  EXPECT_EQ(cause(health.value().facts),
            core::LifecycleBlockingCauseClassDTO::kNone);
  decision = core::EvaluateLifecycleDomainPlan(
      core::ComponentLifecycleIntentKindDTO::kPrepare,
      core::ComponentLifecycleStateDTO::kPrepared, catalog.value(), occurrence,
      health.value());
  ASSERT_TRUE(decision.ok());
  EXPECT_EQ(decision.value().kind,
            core::LifecycleDomainPlanDecisionKind::kDispatchPrimitive);
  EXPECT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  driver->Shutdown();
  executor.remove_node(node);
  executor.remove_node(server);
  output.reset();
  context->shutdown("missing service fixture complete");
}

TEST_F(IoProviderLifecycleProductTest,
       RetainedBrandPortPinsExactClockAfterActualDriverRetires) {
  Build();
  auto registration = driver->brand_registration();
  const auto provider = driver->occurrence();
  const core::BrandLifecycleObservationQueryDTO query{
      core::LifecycleTransactionId("read"),
      core::LifecycleTransactionGeneration(1U),
      kSignal.controllable_component_id,
      kDomain,
      provider.control_domain_revision,
      provider.provider_instance_id,
      provider.provider_manifest_id,
      provider.provider_manifest_revision,
      kUnit,
      provider.provider_runtime_epoch};
  const std::weak_ptr<core::FakeClock> actual_clock = clock;
  driver.reset();
  clock.reset();
  EXPECT_FALSE(actual_clock.expired());
  const auto observed = registration.port->Observe(query);
  ASSERT_FALSE(observed.ok());
  EXPECT_EQ(observed.error().details.at("reason"), "driver_retired");
  registration.port.reset();
  EXPECT_TRUE(actual_clock.expired());
}

} // namespace
TEST_F(IoProviderLifecycleProductTest,
       GenericTypedSourceConnectsAtEnableAndAwaitsActualMessage) {
  Build();
  ASSERT_TRUE(driver);
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  rclcpp::NodeOptions options;
  options.context(context);
  options.use_global_arguments(false);
  auto node =
      std::make_shared<rclcpp::Node>("io_driver_mapped_input_test", options);
  rclcpp::ExecutorOptions executor_options;
  executor_options.context = context;
  rclcpp::executors::SingleThreadedExecutor executor(executor_options);
  executor.add_node(node);
  auto input = Input();
  input.node = node;
  auto &row = input.rows.front();
  row.provider.profile = "ros2_control_gpio";
  row.provider.topic = "/test/driver_gpio_states";
  row.provider.group = "bank";
  row.provider.interface_name = "ready";
  row.provider.initial_value.reset();
  row.descriptor.update_mode = core::IoSignalUpdateMode::kSampled;
  row.descriptor.liveness = core::IoSignalLivenessMode::kPeriodic;
  row.descriptor.freshness = {core::IoFreshnessMode::kSampleAge, 100};
  auto publisher =
      node->create_publisher<control_msgs::msg::DynamicInterfaceGroupValues>(
          *row.provider.topic, rclcpp::QoS(8));
  {
    auto lock = driver->AcquireDelivery();
    const auto installed = driver->InstallResourcesLocked({input}, {}, {});
    ASSERT_TRUE(installed.ok()) << installed.error().message;
  }
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  EXPECT_TRUE(ingress->frames.empty()); // connected is not a fabricated reading
  EXPECT_EQ(
      cause(driver->ReadLifecycleSnapshot(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kNone);
  const auto spin = [&](const auto &predicate) {
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate() && std::chrono::steady_clock::now() < until)
      executor.spin_once(std::chrono::milliseconds(5));
    return predicate();
  };
  ASSERT_TRUE(spin([&] { return publisher->get_subscription_count() == 1U; }));
  control_msgs::msg::DynamicInterfaceGroupValues message;
  message.interface_groups = {"bank"};
  control_msgs::msg::InterfaceValue values;
  values.interface_names = {"ready"};
  values.values = {1.0};
  message.interface_values.push_back(values);
  clock->set(20);
  publisher->publish(message);
  ASSERT_TRUE(spin([&] { return ingress->frames.size() == 1U; }));
  EXPECT_EQ(ingress->frames.back().trusted_received_at_ns, 20);
  EXPECT_EQ(ingress->frames.back().updates.front().value, true);
  ASSERT_EQ(Step("disable").outcome,
            Outcome::kSucceeded);
  publisher->publish(message);
  executor.spin_some();
  EXPECT_EQ(ingress->frames.size(), 1U);
  EXPECT_FALSE(ingress->losses.empty());
  driver->Shutdown();
  executor.cancel();
  executor.remove_node(node);
  driver.reset();
  publisher.reset();
  node.reset();
  context->shutdown("mapped driver fixture complete");
}

TEST_F(
    IoProviderLifecycleProductTest,
    ActualInputConnectionLossAfterEnableReportsProviderLostWithoutResources) {
  Build();
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  rclcpp::NodeOptions options;
  options.context(context);
  options.use_global_arguments(false);
  auto node = std::make_shared<rclcpp::Node>("io_input_loss_test", options);
  auto input = Input();
  input.node = node;
  input.rows.front().provider.profile = "ros_bool";
  input.rows.front().provider.topic = "/test/input_loss";
  input.rows.front().descriptor.update_mode =
      core::IoSignalUpdateMode::kSampled;
  input.rows.front().descriptor.liveness =
      core::IoSignalLivenessMode::kPeriodic;
  input.rows.front().descriptor.freshness = {core::IoFreshnessMode::kSampleAge,
                                             100};
  input.rows.front().provider.initial_value.reset();
  {
    auto delivery = driver->AcquireDelivery();
    ASSERT_TRUE(driver->InstallResourcesLocked({input}, {}, {}).ok());
  }
  ASSERT_TRUE(driver->domain().resource_ids().empty());
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  EXPECT_TRUE(ingress->frames.empty());
  EXPECT_EQ(
      cause(driver->ReadLifecycleSnapshot(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kNone);
  context->shutdown("actual input connection lost after Enable");
  EXPECT_EQ(
      cause(driver->ReadLifecycleSnapshot(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kProviderLost);
  EXPECT_EQ(
      cause(driver->ReadLifecycleHealth(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kProviderLost);
  ASSERT_EQ(Step("disable").outcome,
            Outcome::kSucceeded);
  EXPECT_EQ(
      cause(driver->ReadLifecycleSnapshot(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kAvailability);
  driver->Shutdown();
  EXPECT_EQ(
      cause(driver->ReadLifecycleSnapshot(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kNone);
}

TEST_F(IoProviderLifecycleProductTest,
       ActualLeafFailureBeforeFirstEnableReportsExplicitFaultUntilRetirement) {
  Build(true);
  const core::ResourceId resource("output.channel");
  auto made = MockBooleanOutputProvider::Create(
      *clock, {resource,
               "channel",
               {{kSignal, driver->occurrence(), 1U, driver}},
               core::ActiveBundlePublicationGeneration(1U)});
  ASSERT_TRUE(made.ok());
  std::shared_ptr<MockBooleanOutputProvider> output(std::move(made.value()));
  deployment::IoSignalProviderConfiguration config{
      "mock_output", {}, {}, {}, 0, {}, "channel"};
  deployment::RetainedIoSignalConfiguration row{
      kUnit, core::CommandTargetId("gpio"), core::ResourceRoleId("ready"),
      *descriptor, config};
  {
    auto delivery = driver->AcquireDelivery();
    ASSERT_TRUE(
        driver->InstallResourcesLocked({}, {{resource, {row}, output}}, {})
            .ok());
  }
  // An actual leaf refuses the owner's Prepare after its occurrence retired.
  // This exercises the real failure path; no fault setter or fabricated cause.
  ASSERT_TRUE(output->ShutdownOccurrence(driver->occurrence(), driver).ok());
  const auto attempted = Step("prepare");
  EXPECT_EQ(attempted.outcome, Outcome::kFailedEffectObserved);
  const auto snapshot = driver->ReadLifecycleSnapshot(kDomain, clock->now());
  ASSERT_TRUE(snapshot.ok());
  EXPECT_EQ(snapshot.value().component_state,
            core::ComponentLifecycleStateDTO::kFaulted);
  ASSERT_TRUE(snapshot.value().diagnostic);
  EXPECT_EQ(cause(snapshot.value().facts),
            core::LifecycleBlockingCauseClassDTO::kFault);
  EXPECT_EQ(
      cause(driver->ReadLifecycleHealth(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kFault);
  driver->Shutdown();
  EXPECT_EQ(
      cause(driver->ReadLifecycleSnapshot(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kNone);
}

TEST_F(IoProviderLifecycleProductTest,
       PlannedInputDisableProcessesPriorEvidenceBeforePauseAndFreshEnable) {
  Build();
  auto scheduler = std::make_shared<core::ManualScheduler>();
  std::map<core::InterruptSourceId, HostIoSignalRuntime::SourceInput> sources;
  sources.emplace(kSource,
                  HostIoSignalRuntime::SourceInput{
                      Input().rows, {}, driver->runtime_unit_epoch(),
                      std::nullopt, driver});
  auto runtime =
      HostIoSignalRuntime::Create(core::RequestOwnerEpoch("host"), 1U,
                                  std::move(sources), clock, scheduler);
  std::vector<bool> transitions;
  runtime->SetRuleEvidenceProcessor(
      [] { return core::Result<bool>::Ok(true); },
      [&](const auto &service,
          std::span<const core::InterruptSourceId> selected,
          const core::LifecycleTransactionId &transaction, bool disabled) {
        EXPECT_EQ(service.get(), runtime.get());
        EXPECT_EQ(transaction, core::LifecycleTransactionId("transaction"));
        EXPECT_EQ(selected.size(), 1U);
        if (!selected.empty()) {
          EXPECT_EQ(selected.front(), kSource);
        }
        const auto signal = runtime->ReadSignal(kSignal);
        EXPECT_TRUE(signal.ok());
        if (disabled) {
          EXPECT_EQ(signal.value().availability,
                    core::IoSignalAvailability::kFresh);
          EXPECT_TRUE(signal.value().latest_sample);
          if (signal.value().latest_sample) {
            EXPECT_EQ(signal.value().latest_sample->publication.value,
                      std::optional(true));
          }
        } else {
          EXPECT_NE(signal.value().availability,
                    core::IoSignalAvailability::kFresh);
        }
        transitions.push_back(disabled);
        return core::Result<void>::Ok();
      });
  {
    auto delivery = driver->AcquireDelivery();
    ASSERT_TRUE(driver
                    ->InstallResourcesLocked(
                        runtime->driver_input_installations().at(driver), {},
                        runtime->cancellation_hook(driver),
                        runtime->input_lifecycle_hook(driver))
                    .ok());
  }
  runtime->Start();
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  {
    auto delivery = driver->AcquireDelivery();
    ASSERT_TRUE(driver->SetMockValueLocked(kSignal, true).ok());
  }
  // No explicit drain: Disable must see the queued true acquisition, and
  // immediate Enable must consume Pause loss before removing the arm fence.
  ASSERT_EQ(Step("disable").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(runtime->DrainPending().ok());
  EXPECT_EQ(runtime->ReadSignal(kSignal).value().availability,
            core::IoSignalAvailability::kFresh);
  EXPECT_EQ(transitions, (std::vector<bool>{false, true, false}));
}

TEST_F(IoProviderLifecycleProductTest,
       GeometricEnableAcceptsNewerAndStaleBaselinesAndAbortsFailedTransition) {
  Build();
  descriptor->update_mode = core::IoSignalUpdateMode::kSampled;
  descriptor->freshness = {core::IoFreshnessMode::kSampleAge, 25};
  descriptor->liveness = core::IoSignalLivenessMode::kPeriodic;
  auto input = Input();
  input.rows.front().descriptor = *descriptor;
  deployment::IoSignalProviderConfiguration config;
  config.profile = "geometric_contact";
  config.ingress_depth = 8;
  config.robot_component_id = kSignal.controllable_component_id;
  config.target_scene_resource_id = core::SceneResourceId("fixture");
  input.rows.front().provider = std::move(config);

  const std::vector<std::string> names{"arm_joint"};
  const std::vector<double> position{0.2};
  const std::vector<double> velocity{0.0};
  auto observed = std::make_shared<core::ObservedRobotStatePublication>(
      names, std::vector<core::ObservedRobotStateSourceConfig>{
                 {core::ObservedRobotStateSourceKind::kJointState, "motion",
                  core::ObservedRobotStateAuthorityProfile::kMeasured, 1'000}});
  ASSERT_TRUE(observed
                  ->publish(core::ObservedRobotStateSourceKind::kJointState,
                            names, position, velocity, 0, 0)
                  .published);
  auto scheduler = std::make_shared<core::ManualScheduler>();
  auto publisher = std::make_shared<GeometricSnapshotPublisher>();
  auto engine = std::make_shared<AdvancingCollisionEngine>(*clock);
  input.geometric_contact = GeometricContactInputBinding{
      kSource,
      core::IoSourceIncarnation("installation-owned"),
      {*descriptor},
      [] {
        return core::Result<std::vector<core::CollisionBodyId>>::Ok({kGeometricEndpoint});
      },
      core::SceneResourceId("fixture"),
      {observed},
      publisher,
      engine,
      scheduler,
      10,
      5,
      [](const core::CollisionWorldSnapshot &, const core::CollisionBodyId &)
          -> std::optional<bool> { return false; }};

  std::map<core::InterruptSourceId, HostIoSignalRuntime::SourceInput> sources;
  sources.emplace(kSource,
                  HostIoSignalRuntime::SourceInput{input.rows,
                                                   {},
                                                   driver->runtime_unit_epoch(),
                                                   input.geometric_contact,
                                                   driver});
  auto runtime =
      HostIoSignalRuntime::Create(core::RequestOwnerEpoch("host"), 1U,
                                  std::move(sources), clock, scheduler);
  auto acquisition = runtime->input_acquisition_hook(driver);
  unsigned acquisition_calls{};
  auto boundary = [&, acquisition = std::move(acquisition)](
                      std::span<const IoProviderInputAcquisitionBaseline>
                          baselines) mutable {
    ++acquisition_calls;
    if (acquisition_calls == 1U) {
      clock->set(10);
      scheduler->advance_to(10, core::SchedulerPriority::Monitoring);
    }
    auto acquired = acquisition(baselines);
    if (acquisition_calls == 3U && acquired.ok())
      return core::Result<void>::Err(
          {"test_acquisition_failure",
           "Test rejected an otherwise successful acquisition", {}});
    return acquired;
  };
  {
    auto delivery = driver->AcquireDelivery();
    ASSERT_TRUE(driver
                    ->InstallResourcesLocked(
                        runtime->driver_input_installations().at(driver), {},
                        runtime->cancellation_hook(driver),
                        runtime->input_lifecycle_hook(driver),
                        std::move(boundary))
                    .ok());
  }
  runtime->Start();
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  EXPECT_EQ(engine->queries, 2U);
  const auto newer = runtime->ReadSignal(kSignal).value();
  ASSERT_TRUE(newer.latest_sample);
  EXPECT_EQ(newer.latest_sample->publication.source_sequence, 2U);
  EXPECT_EQ(newer.availability, core::IoSignalAvailability::kFresh);

  ASSERT_EQ(Step("disable").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  engine->duration_ns = 30;
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  EXPECT_EQ(driver->ReadLifecycleSnapshot(kDomain, clock->now())
                .value()
                .component_state,
            core::ComponentLifecycleStateDTO::kActive);
  EXPECT_EQ(engine->queries, 3U);
  const auto stale = runtime->ReadSignal(kSignal).value();
  ASSERT_TRUE(stale.latest_sample);
  EXPECT_EQ(stale.latest_sample->publication.validity,
            core::IoSampleValidity::kValid);
  EXPECT_EQ(stale.availability, core::IoSignalAvailability::kStale);

  core::IoConditionDefinitionDTO condition;
  condition.inputs.push_back({kSignal, core::IoConditionValueKind::kBoolean});
  condition.nodes.emplace_back(core::IoConditionCompareDTO{
      core::IoConditionComparison::kEqual,
      core::IoConditionInputReferenceDTO{0},
      core::IoConditionValueDTO{true}});
  auto checked =
      core::CheckIoCondition(std::move(condition), {1, 1, 1, 1, 128, 128});
  ASSERT_TRUE(checked.ok());
  const auto armed = runtime->ArmRule(
      {core::InterruptPolicyId("stale-geometric-contact"),
       core::IoActivationRuleScopeDTO{core::RetainedBundleId("bundle"),
                                      core::GraphRevision("graph")},
       std::make_shared<const core::CheckedIoCondition>(
           std::move(checked.value())),
       core::MotionInterruptActivationMode::kRequireInactiveAtArm,
       {kSource},
       {}});
  ASSERT_FALSE(armed.ok());
  EXPECT_EQ(armed.error().code, core::kErrorMotionInterruptArmFailed);
  EXPECT_EQ(armed.error().details.at("reason"),
            "io_rule_baseline_unavailable");

  ASSERT_EQ(Step("disable").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  engine->duration_ns = 0;
  const auto failed = Step("enable");
  EXPECT_EQ(failed.outcome, Outcome::kFailedEffectObserved);
  EXPECT_EQ(driver->ReadLifecycleSnapshot(kDomain, clock->now())
                .value()
                .component_state,
            core::ComponentLifecycleStateDTO::kFaulted);
  EXPECT_EQ(engine->queries, 4U);
  clock->set(100);
  scheduler->advance_to(100, core::SchedulerPriority::Monitoring);
  EXPECT_EQ(engine->queries, 4U);
}

TEST_F(IoProviderLifecycleProductTest,
       RejectedPlannedInputDisableDoesNotPauseOrChangeDriverState) {
  for (bool partial : {false, true}) {
    Build();
    {
      auto delivery = driver->AcquireDelivery();
      ASSERT_TRUE(
          driver
              ->InstallResourcesLocked(
                  {Input()}, {}, {},
                  [partial](const auto &, bool disabled) {
                    if (!disabled)
                      return core::Result<void>::Ok();
                    core::ErrorDTO error{"test_refused",
                                         "planned source transition refused",
                                         {}};
                    if (partial)
                      error.details.emplace("io_source_transition_effects",
                                            "observed");
                    return core::Result<void>::Err(std::move(error));
                  })
              .ok());
    }
    ASSERT_EQ(Step("prepare").outcome,
              Outcome::kSucceeded);
    ASSERT_EQ(Step("enable").outcome,
              Outcome::kSucceeded);
    const auto before =
        driver->ReadLifecycleSnapshot(kDomain, clock->now()).value();
    const auto refused = Step("disable");
    EXPECT_EQ(refused.outcome,
              partial ? Outcome::kFailedEffectObserved : Outcome::kNotSent);
    EXPECT_TRUE(
        ingress->losses.empty()); // Neither outcome claims Pause happened.
    const auto after =
        driver->ReadLifecycleSnapshot(kDomain, clock->now()).value();
    EXPECT_EQ(after.component_state,
              partial ? core::ComponentLifecycleStateDTO::kFaulted
                      : core::ComponentLifecycleStateDTO::kActive);
    EXPECT_EQ(after.source_generation,
              before.source_generation + (partial ? 1U : 0U));
    driver->Shutdown();
    driver.reset();
    ingress->frames.clear();
    ingress->losses.clear();
  }
}

TEST_F(IoProviderLifecycleProductTest,
       PoseSourceDriverOwnsItsStreamsAndReportsTheTypedProcessFault) {
  const std::vector<std::string> streams{"poses/box/latch",
                                         "poses/robot/viewer"};
  Build(false, "runtime.1", streams);
  ASSERT_TRUE(driver->stream_observer());
  EXPECT_FALSE(driver->resources_installed());
  EXPECT_EQ(Step("prepare").outcome,
            Outcome::kNotSent);

  core::ManualScheduler scheduler;
  core::ComponentStreamHealth health(*clock, scheduler);
  const auto declare = [&](const std::string &id) {
    auto declared = health.Declare(
        kSignal.controllable_component_id,
        {id, 1'000'000'000LL, core::StreamSeverityDTO::kWarning,
         core::StreamRecoveryDTO::kAuto,
         core::StreamTimestampSourceDTO::kMessageStamp});
    EXPECT_TRUE(declared.ok());
    return std::move(declared.value());
  };
  {
    // The recorders must close exactly the domain's stream membership.
    std::vector<std::unique_ptr<core::StreamRecorder>> partial;
    partial.push_back(declare(streams.front()));
    auto lock = driver->AcquireDelivery();
    const auto refused = driver->InstallObserverStreamsLocked(std::move(partial));
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().details.at("reason"),
              "driver_resource_closure_not_exact");
  }
  {
    std::vector<std::unique_ptr<core::StreamRecorder>> recorders;
    recorders.push_back(declare(streams.back()));
    recorders.push_back(declare(streams.front()));
    auto lock = driver->AcquireDelivery();
    const auto installed = driver->InstallObserverStreamsLocked(std::move(recorders));
    ASSERT_TRUE(installed.ok()) << installed.error().message;
  }
  EXPECT_TRUE(driver->resources_installed());
  EXPECT_EQ(health.Read(kSignal.controllable_component_id).size(), 2U);

  auto exit_fault = std::make_shared<std::optional<core::ErrorDTO>>();
  driver->SetSupervisedProcessFaultSource([exit_fault] { return *exit_fault; });
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  const auto running = driver->ReadLifecycleHealth(kDomain, clock->now());
  ASSERT_TRUE(running.ok());
  EXPECT_EQ(running.value().observed_state,
            core::ComponentLifecycleStateDTO::kActive);
  EXPECT_EQ(cause(running.value().facts),
            core::LifecycleBlockingCauseClassDTO::kNone);

  // The observer exits on a configuration error: the fault source names
  // the typed code and carries the output tail.
  *exit_fault = core::ErrorDTO{"pose_source_config_invalid",
                               "pose source exited with status 78",
                               {{"output_tail", "isaac_pose_observer_failed"}}};
  const auto faulted = driver->ReadLifecycleHealth(kDomain, clock->now());
  ASSERT_TRUE(faulted.ok());
  EXPECT_EQ(faulted.value().observed_state,
            core::ComponentLifecycleStateDTO::kFaulted);
  EXPECT_EQ(cause(faulted.value().facts),
            core::LifecycleBlockingCauseClassDTO::kFault);
  ASSERT_TRUE(faulted.value().diagnostic.has_value());
  EXPECT_EQ(faulted.value().diagnostic->code, "pose_source_config_invalid");
  EXPECT_EQ(faulted.value().diagnostic->details.at("output_tail"),
            "isaac_pose_observer_failed");
  EXPECT_EQ(Step("recover").outcome,
            Outcome::kNotSent);

  // Retirement releases the recorders, which undeclares the streams.
  driver->Shutdown();
  EXPECT_TRUE(health.Read(kSignal.controllable_component_id).empty());
}

TEST_F(IoProviderLifecycleProductTest,
       AiModelDriverOwnsItsEndpointStreamAndReportsTheTypedProcessFault) {
  Build(false, "runtime.1", {"endpoint"}, core::kAiModelLifecycleManifestId);
  ASSERT_TRUE(driver->stream_observer());
  EXPECT_EQ(Step("prepare").outcome,
            Outcome::kNotSent);

  core::ManualScheduler scheduler;
  core::ComponentStreamHealth health(*clock, scheduler);
  {
    std::vector<std::unique_ptr<core::StreamRecorder>> recorders;
    auto declared = health.Declare(
        kSignal.controllable_component_id,
        {"endpoint", 10'000'000'000LL, core::StreamSeverityDTO::kWarning,
         core::StreamRecoveryDTO::kAuto,
         core::StreamTimestampSourceDTO::kArrival});
    ASSERT_TRUE(declared.ok());
    recorders.push_back(std::move(declared.value()));
    auto lock = driver->AcquireDelivery();
    const auto installed =
        driver->InstallObserverStreamsLocked(std::move(recorders));
    ASSERT_TRUE(installed.ok()) << installed.error().message;
  }
  EXPECT_TRUE(driver->resources_installed());

  // Prepare/enable follow the probe process, not the endpoint: the stream
  // has no sample yet and both still succeed.
  auto exit_fault = std::make_shared<std::optional<core::ErrorDTO>>();
  driver->SetSupervisedProcessFaultSource([exit_fault] { return *exit_fault; });
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);

  *exit_fault = core::ErrorDTO{"ai_model_config_invalid",
                               "ai model probe exited with status 78",
                               {}};
  const auto faulted = driver->ReadLifecycleHealth(kDomain, clock->now());
  ASSERT_TRUE(faulted.ok());
  EXPECT_EQ(faulted.value().observed_state,
            core::ComponentLifecycleStateDTO::kFaulted);
  ASSERT_TRUE(faulted.value().diagnostic.has_value());
  EXPECT_EQ(faulted.value().diagnostic->code, "ai_model_config_invalid");

  driver->Shutdown();
  EXPECT_TRUE(health.Read(kSignal.controllable_component_id).empty());
}

TEST_F(IoProviderLifecycleProductTest,
       SupervisedSimulatedPlcExitIsADriverFaultUntilItsSourceIsReplaced) {
  Build();
  InstallInput();
  auto exit_reason = std::make_shared<std::optional<std::string>>();
  driver->SetSupervisedProcessFaultSource(
      [exit_reason]() -> std::optional<core::ErrorDTO> {
        if (!*exit_reason)
          return std::nullopt;
        return core::ErrorDTO{"simulated_plc_exited", **exit_reason, {}};
      });
  ASSERT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome,
            Outcome::kSucceeded);
  const auto running = driver->ReadLifecycleHealth(kDomain, clock->now());
  ASSERT_TRUE(running.ok());
  EXPECT_EQ(running.value().observed_state,
            core::ComponentLifecycleStateDTO::kActive);
  EXPECT_EQ(cause(running.value().facts),
            core::LifecycleBlockingCauseClassDTO::kNone);

  // The simulated PLC process exits (e.g. endpoint occupied, status 3).
  *exit_reason = "controller_launch_exited: controller launch exited with "
                 "status 3 component=plc_io";
  const auto health = driver->ReadLifecycleHealth(kDomain, clock->now());
  ASSERT_TRUE(health.ok());
  EXPECT_TRUE(health.value().locally_valid());
  EXPECT_EQ(health.value().observed_state,
            core::ComponentLifecycleStateDTO::kFaulted);
  EXPECT_EQ(cause(health.value().facts),
            core::LifecycleBlockingCauseClassDTO::kFault);
  ASSERT_TRUE(health.value().diagnostic.has_value());
  EXPECT_EQ(health.value().diagnostic->code, "simulated_plc_exited");
  EXPECT_NE(health.value().diagnostic->message.find("status 3"),
            std::string::npos);
  const auto snapshot = driver->ReadLifecycleSnapshot(kDomain, clock->now());
  ASSERT_TRUE(snapshot.ok());
  EXPECT_TRUE(snapshot.value().locally_valid());
  EXPECT_EQ(snapshot.value().component_state,
            core::ComponentLifecycleStateDTO::kFaulted);
  EXPECT_EQ(cause(snapshot.value().facts),
            core::LifecycleBlockingCauseClassDTO::kFault);
  // No local recovery while the process is gone; only Apply restarts it.
  EXPECT_EQ(Step("recover").outcome,
            Outcome::kNotSent);
  ASSERT_EQ(Step("disable").outcome,
            Outcome::kSucceeded);
  EXPECT_EQ(Step("prepare").outcome,
            Outcome::kNotSent);

  // The next Apply installs the restarted process's source.
  driver->SetSupervisedProcessFaultSource(
      [] { return std::optional<core::ErrorDTO>{}; });
  const auto restarted = driver->ReadLifecycleHealth(kDomain, clock->now());
  ASSERT_TRUE(restarted.ok());
  EXPECT_EQ(restarted.value().observed_state,
            core::ComponentLifecycleStateDTO::kDisabled);
  EXPECT_EQ(cause(restarted.value().facts),
            core::LifecycleBlockingCauseClassDTO::kNone);
  EXPECT_EQ(Step("prepare").outcome,
            Outcome::kSucceeded);
}

TEST_F(IoProviderLifecycleProductTest,
       FailedSimulatedPlcCandidateKeepsTheInstalledCrashFault) {
  using namespace std::chrono_literals;
  // A fake `ros2` that runs until interrupted, first on PATH for this test.
  const auto root = std::filesystem::temp_directory_path() /
                    ("manyforge-sim-plc-candidate-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "bin");
  {
    std::ofstream script(root / "bin/ros2");
    script << "#!/bin/sh\ntrap 'exit 0' INT TERM\nwhile :; do sleep 0.05; done\n";
  }
  std::filesystem::permissions(root / "bin/ros2", std::filesystem::perms::owner_all);
  const std::string previous_path = std::getenv("PATH") ? std::getenv("PATH") : "";
  ::setenv("PATH", ((root / "bin").string() + ":" + previous_path).c_str(), 1);
  struct Restore {
    std::string path;
    std::filesystem::path root;
    ~Restore() {
      ::setenv("PATH", path.c_str(), 1);
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
  } restore{previous_path, root};

  Build();
  InstallInput();
  const auto installed_driver = driver;
  const IoProviderDriverGroup installed_drivers{installed_driver};
  const auto plan = plan_simulated_plc(
      true, "plc_io", {{"ros_bool", "/plc_io/ready", std::nullopt}},
      root / "sim_plc");
  ASSERT_TRUE(plan.has_value());
  ControllerLaunchSupervisor supervisor(2s);
  const SimulatedPlcWait ready = [](const auto &) { return SimulatedPlcReadiness{}; };
  const SimulatedPlcWait not_ready = [&](const auto &) {
    return SimulatedPlcReadiness{{}, plan->subscribed_topics};
  };
  const auto artifacts = root / "artifacts";
  const auto fault_message = [&](const std::shared_ptr<IoProviderDriverRecord> &record)
      -> std::optional<std::string> {
    const auto health = record->ReadLifecycleHealth(kDomain, clock->now());
    if (!health.ok() || !health.value().diagnostic ||
        health.value().diagnostic->code != "simulated_plc_exited")
      return std::nullopt;
    return health.value().diagnostic->message;
  };

  // Installed (fresh drivers observe their process), then the PLC crashes.
  const auto installed = StartSimulatedPlcCandidate(
      supervisor, *plan, artifacts, "", installed_drivers, false, nullptr, ready);
  ASSERT_EQ(Step("prepare").outcome, Outcome::kSucceeded);
  ASSERT_EQ(Step("enable").outcome, Outcome::kSucceeded);
  const auto crashed_pid = std::to_string(installed.process->pid());
  ASSERT_EQ(::kill(-installed.process->pid(), SIGKILL), 0);
  std::optional<std::string> original;
  for (int attempt = 0; attempt < 300 && !original; ++attempt) {
    original = fault_message(installed_driver);
    if (!original) std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(original.has_value());
  EXPECT_NE(original->find("pid=" + crashed_pid), std::string::npos);
  const auto refused = [](const std::function<void()> &start) {
    try {
      start();
    } catch (const std::runtime_error &error) {
      return std::string(error.what()).find("simulated_plc_not_ready") !=
             std::string::npos;
    }
    return false;
  };

  // A candidate that borrows the installed drivers may only keep exactly the
  // installed process; the dead one forces a refusal, drivers untouched.
  EXPECT_TRUE(refused([&] {
    (void)StartSimulatedPlcCandidate(supervisor, *plan, artifacts, "",
                                     installed_drivers, true, installed.process,
                                     ready);
  }));
  EXPECT_EQ(fault_message(installed_driver), original);

  // The rebuild candidate has fresh drivers; when it fails readiness, its
  // fresh process is discarded and the installed driver still reports the crash.
  Build();
  InstallInput();
  const IoProviderDriverGroup fresh_drivers{driver};
  EXPECT_TRUE(refused([&] {
    (void)StartSimulatedPlcCandidate(supervisor, *plan, artifacts, "",
                                     fresh_drivers, false, nullptr, not_ready);
  }));
  EXPECT_EQ(fault_message(installed_driver), original);
  EXPECT_EQ(cause(installed_driver->ReadLifecycleHealth(kDomain, clock->now()).value().facts),
            core::LifecycleBlockingCauseClassDTO::kFault);
  {
    const auto snapshot = installed_driver->ReadLifecycleSnapshot(kDomain, clock->now());
    ASSERT_TRUE(snapshot.ok());
    for (const char *primitive : {"prepare", "enable", "recover"})
      EXPECT_EQ(installed_driver
                    ->Dispatch(Intent(primitive, snapshot.value().source_generation), {})
                    .outcome,
                Outcome::kNotSent)
          << primitive;
  }

  // A successful rebuild: the restarted process serves the fresh drivers.
  const auto restarted = StartSimulatedPlcCandidate(
      supervisor, *plan, artifacts, "", fresh_drivers, false, nullptr, ready);
  EXPECT_NE(restarted.process, installed.process);
  EXPECT_FALSE(fault_message(driver).has_value());
  EXPECT_EQ(Step("prepare").outcome, Outcome::kSucceeded);
  // An unchanged retained device keeps exactly that process.
  const auto kept = StartSimulatedPlcCandidate(
      supervisor, *plan, artifacts, "", fresh_drivers, true, restarted.process, ready);
  EXPECT_EQ(kept.decision, ControllerLaunchDecision::kKeep);
  EXPECT_EQ(kept.process, restarted.process);
  supervisor.stop_all();
}

TEST_F(IoProviderLifecycleProductTest,
       ThrowingSimulatedPlcWaitDiscardsTheFreshProcess) {
  using namespace std::chrono_literals;
  const auto root = std::filesystem::temp_directory_path() /
                    ("manyforge-sim-plc-throw-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "bin");
  {
    std::ofstream script(root / "bin/ros2");
    script << "#!/bin/sh\ntrap 'exit 0' INT TERM\nwhile :; do sleep 0.05; done\n";
  }
  std::filesystem::permissions(root / "bin/ros2", std::filesystem::perms::owner_all);
  const std::string previous_path = std::getenv("PATH") ? std::getenv("PATH") : "";
  ::setenv("PATH", ((root / "bin").string() + ":" + previous_path).c_str(), 1);
  struct Restore {
    std::string path;
    std::filesystem::path root;
    ~Restore() {
      ::setenv("PATH", path.c_str(), 1);
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
  } restore{previous_path, root};
  Build();
  InstallInput();
  const IoProviderDriverGroup drivers{driver};
  const auto plan = plan_simulated_plc(
      true, "plc_io", {{"ros_bool", "/plc_io/ready", std::nullopt}},
      root / "sim_plc");
  ASSERT_TRUE(plan.has_value());
  ControllerLaunchSupervisor supervisor(2s);
  std::shared_ptr<ControllerLaunchProcess> started;
  // E.g. the ContextProvider or the ROS probe throws inside the wait.
  const SimulatedPlcWait throwing = [&](const auto &process) -> SimulatedPlcReadiness {
    started = process;
    throw std::runtime_error("probe failed");
  };
  EXPECT_THROW((void)StartSimulatedPlcCandidate(supervisor, *plan, root / "artifacts",
                                                "", drivers, false, nullptr, throwing),
               std::runtime_error);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->exited().has_value());
  EXPECT_FALSE(supervisor.observe("plc_io").has_value() &&
               supervisor.observe("plc_io")->running);
  EXPECT_FALSE(ControllerLaunchSupervisor::fault_source(started)());
  // The drivers never observed the discarded process.
  const auto health = driver->ReadLifecycleHealth(kDomain, clock->now());
  ASSERT_TRUE(health.ok());
  EXPECT_FALSE(health.value().diagnostic.has_value());
  supervisor.stop_all();
}

} // namespace manyforge::roboplan_backend::host_detail
