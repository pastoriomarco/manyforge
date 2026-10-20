#include "../src/component_runtime_pins.hpp"
#include "../src/host_io_signal_runtime.hpp"
#include "../src/io_provider_lifecycle_product.hpp"

#include <algorithm>
#include <chrono>
#include <control_msgs/msg/dynamic_interface_group_values.hpp>
#include <gtest/gtest.h>
#include <limits>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <std_srvs/srv/set_bool.hpp>

namespace manyforge::roboplan_backend::host_detail {
namespace {
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

core::ProviderManifestDTO manifest() {
  const auto plans = core::MakeIoProviderLifecyclePlans();
  EXPECT_TRUE(plans.ok());
  const core::ProviderManifestId id{
      std::string(core::kIoProviderLifecycleManifestId)};
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
  void Build(bool output_only = false, std::string epoch = "runtime.1") {
    auto provider_manifest = manifest();
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
        {kSignal.name},
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
        scope, occurrence, provider_manifest, clock, 1000);
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
            core::IoSourceIncarnation(std::move(incarnation))};
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
        1'000'000};
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
  EXPECT_EQ(Step("prepare").dispatch,
            core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  InstallInput();
  EXPECT_TRUE(driver->resources_installed());
  EXPECT_TRUE(ingress->frames.empty());
  EXPECT_EQ(Step("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  const auto stale = driver->ReadLifecycleSnapshot(kDomain, clock->now())
                         .value()
                         .source_generation;
  EXPECT_EQ(Step("enable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(ingress->frames.size(), 1U);
  EXPECT_EQ(ingress->frames.back().updates.front().value,
            std::optional<bool>(false));
  EXPECT_EQ(driver->Dispatch(Intent("disable", stale), {}).dispatch,
            core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
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
      driver->Dispatch(Intent("disable", current), stop.get_token()).dispatch,
      core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  EXPECT_EQ(cancellations, 2U);
}

TEST_F(IoProviderLifecycleProductTest,
       DisabledManualModelSurvivesEnableWithoutFabricatedDelivery) {
  Build();
  InstallInput();
  ASSERT_EQ(Step("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(Step("enable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(Step("disable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  const auto before = ingress->frames.size();
  {
    auto lock = driver->AcquireDelivery();
    const auto changed = driver->SetMockValueLocked(kSignal, true);
    ASSERT_TRUE(changed.ok());
    EXPECT_TRUE(changed.value().model_value);
    EXPECT_FALSE(changed.value().enqueued_source_sequence);
  }
  EXPECT_EQ(ingress->frames.size(), before);
  ASSERT_EQ(Step("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(Step("enable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
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
                      Input().rows, {}, driver->runtime_unit_epoch(), driver});
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
  ASSERT_EQ(Step("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(Step("enable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
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
  ASSERT_EQ(Step("disable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
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
  ASSERT_EQ(Step("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(Step("enable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
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
  ASSERT_EQ(Step("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(Step("enable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
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
  ASSERT_EQ(Step("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(Step("enable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
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
  ASSERT_EQ(Step("disable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
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
  ASSERT_EQ(Step("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(Step("enable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  service.reset(); // Actual remote service disappears; local connection owner
                   // survives.
  ASSERT_TRUE(wait_ready(false));
  EXPECT_EQ(
      cause(driver->ReadLifecycleSnapshot(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kProviderLost);
  EXPECT_EQ(
      cause(driver->ReadLifecycleHealth(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kProviderLost);
  ASSERT_EQ(Step("disable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  EXPECT_EQ(driver->ReadLifecycleSnapshot(kDomain, clock->now())
                .value()
                .component_state,
            core::ComponentLifecycleStateDTO::kDisabled);
  EXPECT_EQ(cancellations, 3U);
  EXPECT_EQ(
      cause(driver->ReadLifecycleSnapshot(kDomain, clock->now()).value().facts),
      core::LifecycleBlockingCauseClassDTO::kAvailability);
  EXPECT_EQ(Step("prepare").dispatch,
            core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  EXPECT_EQ(Step("enable").dispatch,
            core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  EXPECT_EQ(Step("recover").dispatch,
            core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  service = server->create_service<Service>("/test/local_disable", answer);
  ASSERT_TRUE(wait_ready(true));
  ASSERT_EQ(Step("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(Step("enable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
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
  ASSERT_EQ(Step("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(Step("enable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
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
  ASSERT_EQ(Step("disable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
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
  ASSERT_EQ(Step("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(Step("enable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
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
  ASSERT_EQ(Step("disable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
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
  EXPECT_EQ(attempted.known, core::LifecyclePrimitiveKnownResultDTO::kFailed);
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
                      Input().rows, {}, driver->runtime_unit_epoch(), driver});
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
  ASSERT_EQ(Step("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(Step("enable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  {
    auto delivery = driver->AcquireDelivery();
    ASSERT_TRUE(driver->SetMockValueLocked(kSignal, true).ok());
  }
  // No explicit drain: Disable must see the queued true acquisition, and
  // immediate Enable must consume Pause loss before removing the arm fence.
  ASSERT_EQ(Step("disable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(Step("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(Step("enable").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(runtime->DrainPending().ok());
  EXPECT_EQ(runtime->ReadSignal(kSignal).value().availability,
            core::IoSignalAvailability::kFresh);
  EXPECT_EQ(transitions, (std::vector<bool>{false, true, false}));
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
    ASSERT_EQ(Step("prepare").known,
              core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
    ASSERT_EQ(Step("enable").known,
              core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
    const auto before =
        driver->ReadLifecycleSnapshot(kDomain, clock->now()).value();
    const auto refused = Step("disable");
    EXPECT_EQ(refused.dispatch,
              partial
                  ? core::LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown
                  : core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
    EXPECT_EQ(refused.effect, partial
                                  ? core::LifecyclePrimitiveEffectDTO::kObserved
                                  : core::LifecyclePrimitiveEffectDTO::kNone);
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

} // namespace manyforge::roboplan_backend::host_detail
