#include "host_io_signal_runtime.hpp"
#include "io_provider_lifecycle_product.hpp"

#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/io_condition_evaluator.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_roboplan/native_runtime_support.hpp>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <set>
#include <std_msgs/msg/bool.hpp>
#include <stdexcept>

namespace manyforge::roboplan_backend::host_detail {
namespace {
const core::InterruptSourceId kSource("mock.inputs");
constexpr core::Duration kDelay = 250'000'000;
core::IoSignalId signal() {
  return {core::ControllableComponentId("io"), "ready"};
}
core::IoSignalDescriptorDTO descriptor() {
  return {signal(),
          kSource,
          core::IoSignalValueType::kBoolean,
          core::IoSignalDirection::kInput,
          core::IoSignalUpdateMode::kChangeOnly,
          core::IoTransitionCapability::kSampledOnly,
          {core::IoFreshnessMode::kProviderValidity, std::nullopt},
          {},
          core::IoSignalLivenessMode::kInProcess};
}
core::IoLevelWatchRequestDTO watch(std::string node, bool expected = true,
                                   std::string run = "run") {
  return {{core::RequestOwnerEpoch("runner"),
           core::ProgramRunId(std::move(run)),
           {core::ProgramNodeInstanceId(std::move(node)), "attempt"}},
          signal(),
          expected};
}
class HostIoSignalRuntimeTest : public ::testing::Test {
protected:
  void Build(bool initial = false, std::string occurrence = "installation.1",
             std::shared_ptr<core::IScheduler> runtime_scheduler = {}) {
    initialized_simulation_runs.clear();
    deployment::IoSignalProviderConfiguration provider_config;
    provider_config.profile = "mock";
    provider_config.initial_value = initial;
    provider_config.ingress_depth = 64U;
    deployment::RetainedIoSignalConfiguration row{
        core::RuntimeUnitId("runtime.io"), core::CommandTargetId("inputs"),
        core::ResourceRoleId("ready"), descriptor(),
        std::move(provider_config)};
    driver = MakeDriver(row, std::move(occurrence));
    std::map<core::InterruptSourceId, HostIoSignalRuntime::SourceInput> sources;
    sources.emplace(kSource,
                    HostIoSignalRuntime::SourceInput{
                        {row}, {}, driver->runtime_unit_epoch(), std::nullopt,
                        driver});
    runtime = HostIoSignalRuntime::Create(
        core::RequestOwnerEpoch("host"), 1, std::move(sources), clock,
        runtime_scheduler ? std::move(runtime_scheduler) : scheduler);
    for (const auto &[actual, inputs] : runtime->driver_input_installations()) {
      auto delivery = actual->AcquireDelivery();
      const auto installed = actual->InstallResourcesLocked(
          inputs, {}, runtime->cancellation_hook(actual));
      if (!installed.ok())
        throw std::runtime_error(installed.error().message);
    }
    runtime->Start();
    // Creation/Start are not lifecycle activation. These tests exercise the
    // actual retained catalog/driver and acquire the first value at Enable.
    const auto before_enable = Observe();
    if (before_enable.availability != core::IoSignalAvailability::kMissing ||
        before_enable.latest_sample)
      throw std::runtime_error("unenabled fixture must have no reported sample");
    Step("prepare");
    Step("enable");
    const auto drained = runtime->DrainPending();
    if (!drained.ok())
      throw std::runtime_error(drained.error().message);
    scheduler->advance_to(clock->now(), core::SchedulerPriority::RuntimeSafety);
    const auto enabled = Observe();
    if (enabled.availability != core::IoSignalAvailability::kFresh ||
        !enabled.latest_sample ||
        enabled.latest_sample->publication.value != initial ||
        enabled.latest_sample->publication.source_sequence != 1U)
      throw std::runtime_error("actual Mock Enable did not publish its initial sample");
  }
  void TearDown() override {
    if (runtime)
      runtime->Shutdown();
    if (driver)
      driver->Shutdown();
  }
  std::shared_ptr<IoProviderDriverRecord>
  MakeDriver(const deployment::RetainedIoSignalConfiguration &row,
             std::string epoch) {
    const core::ContentDigest digest("sha256:" + std::string(64U, 'a'));
    const core::ProviderManifestId id(
        std::string(core::kIoProviderLifecycleManifestId));
    const auto plans = core::MakeIoProviderLifecyclePlans();
    if (!plans.ok())
      throw std::runtime_error(plans.error().message);
    const auto semantic =
        core::ComputeBrandLifecyclePlanCatalogSemanticDigest(id, plans.value());
    if (!semantic.ok())
      throw std::runtime_error(semantic.error().message);
    std::vector<core::ProviderCapabilityOfferDTO> offers;
    for (const auto *name :
         {"component.lifecycle.observe", "component.lifecycle.prepare",
          "component.lifecycle.enable", "component.lifecycle.disable",
          "component.lifecycle.recover"}) {
      const auto *definition = core::find_builtin_capability_definition(name);
      if (!definition)
        throw std::runtime_error("missing lifecycle definition");
      offers.push_back({core::CapabilityId(name),
                        core::CapabilityDefinitionRevision(std::string(
                            definition->capability_definition_revision)),
                        {}});
    }
    // Plain unit product: the real Core factory validates this exact semantic
    // anchor; it is not a fabricated verified Deployment or admission proof.
    const core::ProviderManifestDTO manifest{
        id,
        core::ProviderManifestRevision(digest.value()),
        std::move(offers),
        {},
        core::ProviderLifecyclePlanCatalogDeclarationDTO{
            core::SchemaId(
                std::string(core::kBrandLifecyclePlanCatalogApiVersion)),
            semantic.value()}};
    const auto catalog = core::MakeIoProviderLifecycleCatalog(manifest);
    if (!catalog.ok())
      throw std::runtime_error(catalog.error().message);
    const core::ControlDomainId domain("io.domain");
    const core::ControlDomainRevision revision(digest.value());
    const core::ProviderInstanceId instance("io.provider");
    const core::RuntimeEpoch runtime_epoch(std::move(epoch));
    core::ResolvedLifecycleControlDomainScopeDTO scope{
        domain, revision,
        core::ResolvedProviderDomainScopeDTO{
            {row.descriptor.signal_id.controllable_component_id,
             row.runtime_unit_id,
             instance,
             id,
             manifest.provider_manifest_revision,
             catalog.value().catalog_digest,
             digest,
             {row.descriptor.signal_id.name},
             30'000'000'000LL},
            runtime_epoch,
            std::nullopt,
            {}}};
    core::ResolvedLifecycleProviderOccurrenceDTO actual{
        domain,
        revision,
        instance,
        id,
        manifest.provider_manifest_revision,
        row.runtime_unit_id,
        runtime_epoch,
        catalog.value().catalog_digest};
    auto made =
        IoProviderDriverRecord::Create(scope, actual, manifest, clock, 1000);
    if (!made.ok())
      throw std::runtime_error(made.error().message);
    return std::move(made.value());
  }
  void Step(const char *action) {
    const auto snapshot = driver->ReadLifecycleSnapshot(
        driver->domain().control_domain_id, clock->now());
    if (!snapshot.ok())
      throw std::runtime_error(snapshot.error().message);
    const auto &actual = driver->occurrence();
    const core::BrandLifecyclePrimitiveIntentDTO intent{
        core::LifecycleTransactionId(std::string("fixture.") + action),
        core::LifecycleTransactionGeneration(1U),
        signal().controllable_component_id,
        actual.control_domain_id,
        actual.control_domain_revision,
        actual.provider_instance_id,
        actual.provider_manifest_id,
        actual.provider_manifest_revision,
        actual.provider_runtime_unit_id,
        actual.provider_runtime_epoch,
        core::LifecyclePrimitiveId(std::string("io.provider.") + action),
        core::LifecyclePrimitiveAttemptId(std::string("attempt.") + action),
        snapshot.value().source_generation,
        1'000'000};
    const auto outcome = driver->Dispatch(intent, {});
    if (outcome.known != core::LifecyclePrimitiveKnownResultDTO::kSucceeded)
      throw std::runtime_error(outcome.error ? outcome.error->message
                                             : "fixture lifecycle failed");
  }
  void Advance(core::TimePoint now) {
    clock->set(now);
    scheduler->advance_to(now, core::SchedulerPriority::RuntimeSafety);
  }
  core::IoSignalObservationDTO Observe() {
    return runtime->ReadSignal(signal()).value();
  }
  core::Result<MockIoInputSetResult> Set(bool value) {
    return runtime->SetMockInput(signal(), *Observe().installed_incarnation,
                                 value);
  }
  core::ProgramNodeMockSimulationRequestDTO
  Simulation(std::string node = "node", std::uint64_t index = 1U,
             std::string attempt = "attempt", bool start = false,
             std::optional<core::ProgramMockSimulationChangeDTO> change =
                 core::ProgramMockSimulationChangeDTO{250U, true},
             std::string run = "run") {
    return {{core::RequestOwnerEpoch("host"),
             core::ProgramBindingId("binding"),
             core::RequestOwnerEpoch("runner"),
             core::CellAdmissionClaimGeneration(1U),
             core::ProgramRunId(std::move(run)),
             {core::ProgramNodeInstanceId(std::move(node)), std::move(attempt)},
             index},
            {{core::TargetRoleId("inputs"), core::ResourceRoleId("ready"),
              start, change}}};
  }
  core::Result<void>
  SetRunPaused(const core::ProgramNodeMockSimulationScopeDTO &scope,
               std::uint64_t revision, bool paused,
               std::vector<core::ProgramNodeInstanceId> held = {}) {
    return runtime->SetProgramRunMockSimulationPaused(
        {{scope.request_owner_epoch, scope.program_binding_id,
          scope.program_runner_owner_epoch, scope.claim_generation,
          scope.program_run_id},
         revision,
         paused,
         std::move(held)});
  }
  core::Result<void> InitializeSimulationRun(
      const core::ProgramNodeMockSimulationScopeDTO &scope) {
    const auto key = std::make_pair(scope.program_runner_owner_epoch.value(),
                                    scope.program_run_id.value());
    if (initialized_simulation_runs.contains(key))
      return core::Result<void>::Ok();
    const auto initialized = SetRunPaused(scope, 1U, false);
    if (initialized.ok())
      initialized_simulation_runs.insert(key);
    return initialized;
  }
  std::set<std::pair<std::string, std::string>> initialized_simulation_runs;
  core::Result<core::ProgramNodeMockSimulationAcknowledgementDTO>
  StartSimulation(const core::ProgramNodeMockSimulationRequestDTO &request,
                  std::size_t maximum_nodes = core::kMaxProgramTreeNodes,
                  bool initialize_clock = true) {
    if (initialize_clock) {
      const auto initialized = InitializeSimulationRun(request.scope);
      if (!initialized.ok())
        return core::Result<core::ProgramNodeMockSimulationAcknowledgementDTO>::
            Err(initialized.error());
    }
    const auto observed = Observe();
    const auto digest =
        core::ComputeIoSignalDescriptorDigest(observed.descriptor);
    if (!digest.ok())
      throw std::runtime_error(digest.error().message);
    std::vector<core::ResolvedProgramMockSimulationSignalDTO> proof;
    for (std::size_t index = 0U; index < request.signals.size(); ++index)
      proof.push_back({signal(), digest.value(),
                       observed.cursor.catalog_generation,
                       observed.installed_incarnation});
    // Plain service unit fixture: proof comes from actual installed catalog
    // observations. Kernel captured-Run authority is tested at its own
    // boundary.
    return runtime->StartProgramNodeMockSimulation(request, proof,
                                                   maximum_nodes);
  }
  core::IoRuleArmRequestDTO Rule(core::MotionInterruptActivationMode mode,
                                 bool search = false) {
    core::IoConditionDefinitionDTO definition;
    definition.inputs.push_back(
        {signal(), core::IoConditionValueKind::kBoolean});
    definition.nodes.emplace_back(
        core::IoConditionCompareDTO{core::IoConditionComparison::kEqual,
                                    core::IoConditionInputReferenceDTO{0},
                                    core::IoConditionValueDTO{true}});
    auto checked =
        core::CheckIoCondition(std::move(definition), {1, 1, 1, 1, 128, 128});
    if (!checked.ok())
      throw std::runtime_error("fixture condition invalid");
    core::IoRuleArmRequestDTO request{
        core::InterruptPolicyId("rule"),
        core::IoActivationRuleScopeDTO{core::RetainedBundleId("bundle"),
                                       core::GraphRevision("graph")},
        std::make_shared<const core::CheckedIoCondition>(
            std::move(checked.value())),
        mode,
        {kSource},
        {}};
    if (search) {
      request.scope = core::IoOperationRuleScopeDTO{
          core::OperationId("operation"),
          core::OperationRevision(1U),
          core::OperationParticipantId("participant"),
          core::ControllableComponentId("io"),
          core::CommandTargetId("inputs"),
          core::ControllerGoalId("goal"),
          core::ControllerGoalGeneration(1U),
          1U,
          core::ContentDigest(std::string(64U, 'a')),
          core::MotionInterruptScopeId("search")};
      request.search_expectation =
          core::IoBooleanLevelExpectationDTO{signal(), true};
    }
    return request;
  }
  std::shared_ptr<core::FakeClock> clock = std::make_shared<core::FakeClock>();
  std::shared_ptr<core::ManualScheduler> scheduler =
      std::make_shared<core::ManualScheduler>();
  std::shared_ptr<HostIoSignalRuntime> runtime;
  std::shared_ptr<IoProviderDriverRecord> driver;
};

TEST_F(HostIoSignalRuntimeTest,
       ActualDriverAcquisitionAndDisableDrainWakeProcessorOutsideDeliveryLock) {
  Build();
  std::vector<core::IoSignalAvailability> availability;
  std::vector<std::optional<bool>> values;
  runtime->SetRuleEvidenceProcessor([&] {
    // This is the same actual record barrier held during source delivery.
    // A scheduled processor must run after that barrier and ingress drain.
    auto delivery = driver->AcquireDelivery();
    const auto observed = Observe();
    availability.push_back(observed.availability);
    values.push_back(observed.latest_sample
                         ? observed.latest_sample->publication.value
                         : std::nullopt);
    return core::Result<bool>::Ok(true);
  });
  const auto changed = Set(true);
  ASSERT_TRUE(changed.ok()) << changed.error().message;
  EXPECT_TRUE(availability.empty()); // processor never runs inline in ingress
  Advance(clock->now());
  ASSERT_EQ(availability.size(), 1U);
  EXPECT_EQ(availability.back(), core::IoSignalAvailability::kFresh);
  EXPECT_EQ(values.back(), std::optional<bool>(true));
  Step("disable");
  ASSERT_TRUE(runtime->DrainPending().ok());
  Advance(clock->now());
  ASSERT_EQ(availability.size(), 2U);
  EXPECT_EQ(availability.back(), core::IoSignalAvailability::kUnavailable);
  runtime->DetachRuleEvidenceProcessor();
}

TEST_F(HostIoSignalRuntimeTest,
       RealPeriodicRosReceiptSchedulesExpiryAndWakesWithoutAnotherAcquisition) {
  constexpr core::Duration maximum_age = 1'000;
  constexpr core::TimePoint received_at = 42;
  clock->set(received_at);
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  rclcpp::NodeOptions options;
  options.context(context);
  options.use_global_arguments(false);
  auto node = std::make_shared<rclcpp::Node>("runtime_periodic_input", options);
  auto publisher = node->create_publisher<std_msgs::msg::Bool>(
      "/test/runtime_periodic_input", rclcpp::QoS(1));
  auto periodic = descriptor();
  periodic.update_mode = core::IoSignalUpdateMode::kSampled;
  periodic.freshness = {core::IoFreshnessMode::kSampleAge, maximum_age};
  periodic.liveness = core::IoSignalLivenessMode::kPeriodic;
  deployment::IoSignalProviderConfiguration periodic_config;
  periodic_config.profile = "ros_bool";
  periodic_config.topic = "/test/runtime_periodic_input";
  periodic_config.ingress_depth = 64U;
  deployment::RetainedIoSignalConfiguration row{
      core::RuntimeUnitId("runtime.io"), core::CommandTargetId("inputs"),
      core::ResourceRoleId("ready"), periodic, std::move(periodic_config)};
  driver = MakeDriver(row, "periodic.installation.1");
  runtime = HostIoSignalRuntime::Create(
      core::RequestOwnerEpoch("host"), 1U,
      {{kSource,
        HostIoSignalRuntime::SourceInput{
            {row}, node, driver->runtime_unit_epoch(), std::nullopt, driver}}},
      clock, scheduler);
  for (const auto &[actual, inputs] : runtime->driver_input_installations()) {
    auto delivery = actual->AcquireDelivery();
    ASSERT_TRUE(actual
                    ->InstallResourcesLocked(inputs, {},
                                             runtime->cancellation_hook(actual))
                    .ok());
  }
  runtime->Start();
  EXPECT_FALSE(Observe().latest_sample);
  const auto inactive = StartSimulation(Simulation());
  ASSERT_TRUE(inactive.ok()) << inactive.error().message;
  ASSERT_EQ(inactive.value().signals.size(), 1U);
  EXPECT_EQ(inactive.value().signals.front().disposition,
            core::ProgramMockSimulationSignalDisposition::kInactive);
  EXPECT_FALSE(Observe().latest_sample); // real ROS input is never fabricated
  Step("prepare");
  Step("enable");
  EXPECT_FALSE(
      Observe().latest_sample); // Enable does not fabricate a ROS value
  rclcpp::ExecutorOptions executor_options;
  executor_options.context = context;
  rclcpp::executors::SingleThreadedExecutor executor(executor_options);
  executor.add_node(node);
  const auto spin_until = [&](const auto &condition) {
    const auto limit =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!condition() && std::chrono::steady_clock::now() < limit) {
      executor.spin_once(std::chrono::milliseconds(5));
      const auto drained = runtime->DrainPending();
      if (!drained.ok())
        throw std::runtime_error(drained.error().message);
    }
    return condition();
  };
  ASSERT_TRUE(
      spin_until([&] { return publisher->get_subscription_count() == 1U; }));
  std_msgs::msg::Bool sample;
  sample.data = false; // The real rule baseline is inactive.
  publisher->publish(sample);
  ASSERT_TRUE(spin_until([&] { return Observe().latest_sample.has_value(); }));
  Advance(clock->now()); // consume the actual ingress drain wake
  const auto baseline = Observe();
  ASSERT_EQ(baseline.availability, core::IoSignalAvailability::kFresh);
  ASSERT_TRUE(baseline.latest_sample);
  EXPECT_EQ(baseline.latest_sample->received_at_ns, received_at);
  EXPECT_EQ(baseline.latest_sample->publication.value,
            std::optional<bool>(false));
  const auto armed = runtime->ArmRule(
      Rule(core::MotionInterruptActivationMode::kRequireInactiveAtArm));
  ASSERT_TRUE(armed.ok()) << armed.error().message;
  const auto expiry = runtime->NextRuleExpiry();
  ASSERT_TRUE(expiry.ok());
  ASSERT_TRUE(expiry.value());
  EXPECT_EQ(*expiry.value(), received_at + maximum_age + 1);
  unsigned calls{};
  std::vector<core::IoSignalAvailability> observed;
  runtime->SetRuleEvidenceProcessor([&] {
    // Evaluate the actual Core registration, then let the runtime query that
    // same service's NextRuleExpiry. No Kernel/installed-global grant is faked.
    const auto evaluated = runtime->EvaluateRules();
    if (!evaluated.ok())
      return core::Result<bool>::Err(evaluated.error());
    auto delivery = driver->AcquireDelivery();
    ++calls;
    observed.push_back(Observe().availability);
    return core::Result<bool>::Ok(true);
  });
  runtime->RequestRuleEvidenceProcessing();
  EXPECT_EQ(calls, 0U);
  Advance(received_at);
  ASSERT_EQ(calls, 1U);
  EXPECT_EQ(observed.back(), core::IoSignalAvailability::kFresh);
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety),
            1U);
  Advance(*expiry.value() - 1);
  EXPECT_EQ(calls, 1U);
  EXPECT_EQ(Observe().availability, core::IoSignalAvailability::kFresh);
  Advance(*expiry.value());
  ASSERT_EQ(calls, 2U);
  EXPECT_EQ(observed.back(), core::IoSignalAvailability::kStale);
  EXPECT_EQ(Observe().latest_sample, baseline.latest_sample);
  const auto evidence = runtime->ReadRuleEvidence(armed.value());
  ASSERT_TRUE(evidence.ok());
  EXPECT_FALSE(evidence.value().trip);
  ASSERT_EQ(evidence.value().source_losses.size(), 1U);
  const auto &loss = evidence.value().source_losses.front();
  EXPECT_EQ(loss.kind, core::IoRuleSourceFailureKind::kStale);
  EXPECT_EQ(loss.evaluated_at_ns, *expiry.value());
  EXPECT_EQ(loss.sample, baseline.latest_sample);
  EXPECT_FALSE(
      loss.source_loss); // derived expiry, not an invented ingress event
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety),
            0U);
  // Obtain a genuinely fresh inactive reading while the old token retains
  // its first loss. Processing that receipt alone cannot schedule its expiry.
  publisher->publish(sample);
  ASSERT_TRUE(spin_until([&] {
    return Observe().latest_sample->publication.source_sequence >
           baseline.latest_sample->publication.source_sequence;
  }));
  Advance(clock->now());
  ASSERT_EQ(calls, 3U);
  const auto renewed = Observe();
  ASSERT_EQ(renewed.availability, core::IoSignalAvailability::kFresh);
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety),
            0U);
  const auto retired = runtime->RetireRule(armed.value());
  ASSERT_TRUE(retired.ok());
  EXPECT_EQ(retired.value().source_losses, evidence.value().source_losses);
  const auto rearmed = runtime->ArmRule(
      Rule(core::MotionInterruptActivationMode::kRequireInactiveAtArm));
  ASSERT_TRUE(rearmed.ok());
  // These activation-token calls are deliberately pure service operations.
  // Actual operator/journal/latch recovery is covered by Core79/86 tests.
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety),
            0U);
  const auto rearmed_expiry =
      renewed.latest_sample->received_at_ns + maximum_age + 1;
  runtime->RequestRuleEvidenceProcessing(); // same host wake after recovery
                                            // succeeds
  Advance(clock->now());
  ASSERT_EQ(calls, 4U);
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety),
            1U);
  Advance(rearmed_expiry - 1);
  EXPECT_EQ(calls, 4U);
  Advance(rearmed_expiry);
  ASSERT_EQ(calls, 5U);
  EXPECT_EQ(observed.back(), core::IoSignalAvailability::kStale);
  EXPECT_EQ(Observe().latest_sample, renewed.latest_sample);
  const auto successor_loss = runtime->ReadRuleEvidence(rearmed.value());
  ASSERT_TRUE(successor_loss.ok());
  ASSERT_EQ(successor_loss.value().source_losses.size(), 1U);
  EXPECT_EQ(successor_loss.value().source_losses.front().kind,
            core::IoRuleSourceFailureKind::kStale);
  EXPECT_EQ(successor_loss.value().source_losses.front().evaluated_at_ns,
            rearmed_expiry);
  EXPECT_EQ(successor_loss.value().source_losses.front().sample,
            renewed.latest_sample);
  EXPECT_FALSE(successor_loss.value().trip);
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety),
            0U);

  // An ordinary operation rule uses the same real source/timer, but its
  // retirement must request recomputation rather than leave an obsolete due
  // task. It carries no search expectation and performs no Mock auto-answer.
  publisher->publish(sample);
  ASSERT_TRUE(spin_until([&] {
    return Observe().latest_sample->publication.source_sequence >
           renewed.latest_sample->publication.source_sequence;
  }));
  Advance(clock->now());
  ASSERT_EQ(calls, 6U);
  const auto third = Observe();
  ASSERT_EQ(third.availability, core::IoSignalAvailability::kFresh);
  ASSERT_TRUE(runtime->RetireRule(rearmed.value()).ok());
  auto ordinary_request =
      Rule(core::MotionInterruptActivationMode::kRequireInactiveAtArm, true);
  ordinary_request.search_expectation.reset();
  const auto ordinary = runtime->ArmRule(ordinary_request);
  ASSERT_TRUE(ordinary.ok());
  Advance(clock->now());
  ASSERT_EQ(calls, 7U);
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety),
            1U);
  const auto ordinary_expiry =
      third.latest_sample->received_at_ns + maximum_age + 1;
  const auto ordinary_retired = runtime->RetireRule(ordinary.value());
  ASSERT_TRUE(ordinary_retired.ok());
  EXPECT_TRUE(ordinary_retired.value().source_losses.empty());
  EXPECT_EQ(calls, 7U); // never invoke the processor inline under delivery lock
  Advance(clock->now());
  ASSERT_EQ(calls, 8U);
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety),
            0U);
  Advance(ordinary_expiry);
  EXPECT_EQ(calls, 8U);
  EXPECT_EQ(Observe().latest_sample, third.latest_sample);
  EXPECT_EQ(runtime->ReadRuleEvidence(ordinary.value()).value().state,
            core::IoRuleRegistrationState::kRetired);
  runtime->DetachRuleEvidenceProcessor();
  runtime->Shutdown();
  driver->Shutdown();
  executor.remove_node(node);
  context->shutdown("periodic input expiry fixture complete");
}

TEST_F(HostIoSignalRuntimeTest,
       InitialSnapshotDoesNotAgeOrSchedulePeriodicAcquisitions) {
  Build();
  EXPECT_EQ(runtime->service().get(), runtime.get());
  EXPECT_EQ(Observe().latest_sample->publication.value, false);
  Advance(1'000'000'000'000);
  EXPECT_EQ(Observe().availability, core::IoSignalAvailability::kFresh);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 1U);
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety),
            0U);
}

TEST_F(HostIoSignalRuntimeTest,
       NodeScriptPublishesInitialAndDelayedEvidenceOnRuntimeSafetyLane) {
  Build();
  const auto token = runtime->RegisterLevelWatch(watch("wait"));
  ASSERT_TRUE(token.ok());
  Advance(kDelay);
  EXPECT_EQ(runtime->ReadWatch(token.value()).value().state,
            core::IoWatchState::kPending);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 1U);
  const auto started = StartSimulation(Simulation());
  ASSERT_TRUE(started.ok()) << started.error().message;
  EXPECT_EQ(started.value().started_at_ns, kDelay);
  EXPECT_EQ(started.value().signals.front().disposition,
            core::ProgramMockSimulationSignalDisposition::kApplied);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 2U);
  EXPECT_EQ(Observe().latest_sample->publication.value, false);
  EXPECT_EQ(scheduler->last_priority(), core::SchedulerPriority::RuntimeSafety);
  Advance(2 * kDelay - 1);
  EXPECT_EQ(runtime->ReadWatch(token.value()).value().state,
            core::IoWatchState::kPending);
  Advance(2 * kDelay);
  const auto result = runtime->ReadWatch(token.value()).value();
  ASSERT_EQ(result.state, core::IoWatchState::kMatched);
  ASSERT_TRUE(result.matched_sample);
  EXPECT_EQ(result.matched_sample->publication.source_sequence, 3U);
  EXPECT_EQ(result.matched_sample->publication.value, true);
  EXPECT_EQ(result.matched_sample->received_at_ns, 2 * kDelay);
  EXPECT_EQ(Observe().latest_sample, result.matched_sample);
}

TEST_F(HostIoSignalRuntimeTest,
       ExactReplayDoesNotRepublishOrRestartCompletedTimers) {
  Build();
  const auto request = Simulation();
  const auto first = StartSimulation(request);
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 2U);
  Advance(kDelay / 2);
  const auto replay = StartSimulation(request);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), first.value());
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 2U);
  Advance(kDelay);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 3U);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  const auto completed = StartSimulation(request);
  ASSERT_TRUE(completed.ok());
  EXPECT_EQ(completed.value(), first.value());
  Advance(3 * kDelay);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 3U);
}

TEST_F(HostIoSignalRuntimeTest,
       NewerNodeStartReplacesOnlyItsOwnTimersAndRejectsStalePackets) {
  Build();
  const auto original = Simulation("one");
  ASSERT_TRUE(StartSimulation(original).ok());
  ASSERT_TRUE(StartSimulation(Simulation("two", 1U, "second")).ok());
  Advance(kDelay / 2);
  const auto replacement =
      Simulation("one", 2U, "replacement", true,
                 core::ProgramMockSimulationChangeDTO{250U, false});
  const auto replaced = StartSimulation(replacement);
  ASSERT_TRUE(replaced.ok());
  const auto cursor = Observe().cursor;
  auto conflict = replacement;
  conflict.signals.front().start_value = false;
  EXPECT_FALSE(StartSimulation(conflict).ok());
  EXPECT_FALSE(StartSimulation(original).ok());
  auto reused_attempt = replacement;
  ++reused_attempt.scope.node_start_index;
  EXPECT_FALSE(StartSimulation(reused_attempt).ok());
  EXPECT_EQ(Observe().cursor, cursor);
  Advance(kDelay); // node two's original timeline remains independent
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 5U);
  Advance(kDelay + kDelay / 2);
  EXPECT_EQ(Observe().latest_sample->publication.value, false);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 6U);
}

TEST_F(HostIoSignalRuntimeTest, ManualToggleNeverCancelsTheLaterNodeChange) {
  Build();
  ASSERT_TRUE(StartSimulation(Simulation()).ok());
  const auto first = runtime->RegisterLevelWatch(watch("first"));
  const auto second = runtime->RegisterLevelWatch(watch("second"));
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(runtime->ReleaseWatch(first.value()).ok());
  ASSERT_TRUE(Set(false).ok());
  const auto sequence = Observe().latest_sample->publication.source_sequence;
  Advance(kDelay);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence,
            sequence + 1U);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  EXPECT_EQ(runtime->ReadWatch(first.value()).value().state,
            core::IoWatchState::kReleased);
  EXPECT_EQ(runtime->ReadWatch(second.value()).value().state,
            core::IoWatchState::kMatched);
}

TEST_F(HostIoSignalRuntimeTest, RunTeardownCancelsOnlyItsOwnTimers) {
  Build();
  ASSERT_TRUE(StartSimulation(
                  Simulation("one", 1U, "first", false,
                             core::ProgramMockSimulationChangeDTO{250U, false},
                             "run.one"))
                  .ok());
  ASSERT_TRUE(StartSimulation(
                  Simulation("two", 1U, "second", false,
                             core::ProgramMockSimulationChangeDTO{250U, true},
                             "run.two"))
                  .ok());
  const auto other =
      runtime->RegisterLevelWatch(watch("wait", true, "run.two"));
  ASSERT_TRUE(other.ok());
  ASSERT_TRUE(runtime
                  ->ReleaseRunWatches(core::RequestOwnerEpoch("runner"),
                                      core::ProgramRunId("run.one"))
                  .ok());
  Advance(kDelay);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 4U);
  EXPECT_EQ(runtime->ReadWatch(other.value()).value().state,
            core::IoWatchState::kMatched);
}

TEST_F(HostIoSignalRuntimeTest,
       ExactScopeReleaseRetainsOtherAttemptWatchesWithoutAutoAnswer) {
  Build();
  const auto request = watch("same_node");
  const auto first = runtime->RegisterLevelWatch(request);
  auto next_request = request;
  next_request.scope.node.node_attempt_id = "next_attempt";
  const auto second = runtime->RegisterLevelWatch(next_request);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(runtime->ReleaseNodeWatches(request.scope).ok());
  ASSERT_TRUE(runtime->ReleaseNodeWatches(request.scope).ok());
  Advance(kDelay);
  EXPECT_EQ(runtime->ReadWatch(first.value()).value().state,
            core::IoWatchState::kReleased);
  EXPECT_EQ(runtime->ReadWatch(second.value()).value().state,
            core::IoWatchState::kPending);
  ASSERT_TRUE(Set(true).ok());
  EXPECT_EQ(runtime->ReadWatch(second.value()).value().state,
            core::IoWatchState::kMatched);
}

TEST_F(HostIoSignalRuntimeTest,
       InitialValuesAndUnscriptedWatchesNeverCreateAnswers) {
  Build(true);
  const auto already = runtime->RegisterLevelWatch(watch("already"));
  ASSERT_TRUE(already.ok());
  EXPECT_EQ(runtime->ReadWatch(already.value()).value().state,
            core::IoWatchState::kMatched);
  const auto opposite = runtime->RegisterLevelWatch(watch("opposite", false));
  ASSERT_TRUE(opposite.ok());
  Advance(2 * kDelay);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 1U);
  EXPECT_EQ(runtime->ReadWatch(opposite.value()).value().state,
            core::IoWatchState::kPending);
}

TEST_F(HostIoSignalRuntimeTest, GuardAndSearchUseOnlyActualExplicitChanges) {
  Build();
  const auto guard = runtime->ArmRule(
      Rule(core::MotionInterruptActivationMode::kRequireInactiveAtArm));
  const auto search = runtime->ArmRule(
      Rule(core::MotionInterruptActivationMode::kFreshEdgeAfterArm, true));
  ASSERT_TRUE(guard.ok());
  ASSERT_TRUE(search.ok());
  Advance(kDelay);
  EXPECT_FALSE(runtime->ReadRuleEvidence(guard.value()).value().trip);
  EXPECT_FALSE(runtime->ReadRuleEvidence(search.value()).value().trip);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 1U);
  ASSERT_TRUE(StartSimulation(Simulation()).ok());
  Advance(2 * kDelay);
  ASSERT_TRUE(runtime->ReadRuleEvidence(search.value()).value().trip);
  const auto seq = Observe().latest_sample->publication.source_sequence;
  const auto fresh = runtime->ArmRule(
      Rule(core::MotionInterruptActivationMode::kFreshEdgeAfterArm, true));
  ASSERT_TRUE(fresh.ok());
  EXPECT_FALSE(
      runtime
          ->ArmRule(Rule(
              core::MotionInterruptActivationMode::kRequireInactiveAtArm, true))
          .ok());
  Advance(3 * kDelay);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, seq);
  EXPECT_FALSE(runtime->ReadRuleEvidence(fresh.value()).value().trip);
}

TEST_F(HostIoSignalRuntimeTest,
       GlobalGuardRefusesSearchWithoutSchedulingOrMutation) {
  Build();
  auto request = Rule(core::MotionInterruptActivationMode::kFreshEdgeAfterArm);
  request.search_expectation =
      core::IoBooleanLevelExpectationDTO{signal(), true};
  const auto before = Observe().latest_sample;
  const auto rejected = runtime->ArmRule(request);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "io_global_rule_search_expectation_invalid");
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety),
            0U);
  Advance(2 * kDelay);
  EXPECT_EQ(Observe().latest_sample, before);
  request.search_expectation.reset();
  const auto guard = runtime->ArmRule(request);
  ASSERT_TRUE(guard.ok());
  EXPECT_TRUE(runtime->RetireRule(guard.value()).ok());
}

TEST_F(HostIoSignalRuntimeTest, RetiredInstallationCannotApplyItsLateScript) {
  Build();
  const auto request = Simulation();
  ASSERT_TRUE(StartSimulation(request).ok());
  const auto search = runtime->ArmRule(
      Rule(core::MotionInterruptActivationMode::kFreshEdgeAfterArm, true));
  ASSERT_TRUE(search.ok());
  const auto retired = runtime->RetireRule(search.value());
  ASSERT_TRUE(retired.ok());
  EXPECT_EQ(retired.value().state, core::IoRuleRegistrationState::kRetired);
  const auto old_epoch = *Observe().installed_incarnation;
  const auto before_retirement = Observe();
  auto old = runtime;
  old->Shutdown();
  Build(false, "installation.2");
  EXPECT_NE(Observe().installed_incarnation, old_epoch);
  EXPECT_FALSE(runtime->SetMockInput(signal(), old_epoch, true).ok());
  Advance(2 * kDelay);
  EXPECT_EQ(Observe().latest_sample->publication.value, false);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 1U);
  const auto after_retirement = old->ReadSignal(signal());
  ASSERT_TRUE(after_retirement.ok());
  EXPECT_EQ(after_retirement.value().availability, before_retirement.availability);
  EXPECT_EQ(after_retirement.value().latest_sample, before_retirement.latest_sample);
  EXPECT_EQ(after_retirement.value().installed_incarnation,
            before_retirement.installed_incarnation);
  EXPECT_EQ(after_retirement.value().cursor, before_retirement.cursor);
  EXPECT_EQ(after_retirement.value().continuity_loss_generation,
            before_retirement.continuity_loss_generation);
  std::vector<core::ResolvedProgramMockSimulationSignalDTO> no_proof;
  EXPECT_FALSE(old->StartProgramNodeMockSimulation(request, no_proof, 1U).ok());
}

TEST_F(HostIoSignalRuntimeTest,
       ManualSetReturnsModelAndExactAdmittedAcquisitionCursor) {
  Build();
  const auto before = Observe();
  const auto changed = Set(true);
  ASSERT_TRUE(changed.ok()) << changed.error().message;
  EXPECT_TRUE(changed.value().model_value);
  EXPECT_FALSE(changed.value().publication_error);
  ASSERT_TRUE(changed.value().published_cursor);
  const auto &cursor = *changed.value().published_cursor;
  EXPECT_EQ(cursor.owner_epoch, before.cursor.owner_epoch);
  EXPECT_EQ(cursor.catalog_generation, before.cursor.catalog_generation);
  EXPECT_EQ(cursor.source_id, kSource);
  EXPECT_GT(cursor.record_sequence, before.cursor.record_sequence);
  ASSERT_TRUE(changed.value().observation.latest_sample);
  EXPECT_EQ(changed.value().observation.latest_sample->publication.value, true);
  EXPECT_EQ(changed.value().observation.latest_sample->publication.incarnation,
            before.installed_incarnation);
  EXPECT_EQ(changed.value().observation.cursor, cursor);
}

class FailingNodeScriptScheduler final : public core::IScheduler {
public:
  explicit FailingNodeScriptScheduler(
      std::shared_ptr<core::ManualScheduler> actual)
      : actual_(std::move(actual)) {}
  core::ScheduleHandle schedule(core::TimePoint at,
                                std::function<void()> callback,
                                core::SchedulerPriority priority) override {
    if (fail_next) {
      fail_next = false;
      throw std::runtime_error("injected schedule allocation failure");
    }
    return actual_->schedule(at, std::move(callback), priority);
  }
  void cancel(core::ScheduleHandle handle) override { actual_->cancel(handle); }
  bool fail_next{};

private:
  std::shared_ptr<core::ManualScheduler> actual_;
};

class DequeuedFailureNodeScriptScheduler final : public core::IScheduler {
public:
  struct Completion final {
    std::promise<void> entered;
    std::promise<void> completed;
  };
  explicit DequeuedFailureNodeScriptScheduler(
      std::shared_ptr<core::ManualScheduler> manual)
      : manual_(std::move(manual)),
        actual_(std::make_shared<SimpleScheduler>(SimpleSchedulerConfig{})),
        completion(std::make_shared<Completion>()),
        entered(completion->entered.get_future()),
        completed(completion->completed.get_future()) {}
  core::ScheduleHandle schedule(core::TimePoint at,
                                std::function<void()> callback,
                                core::SchedulerPriority priority) override {
    if (!fail_dequeued)
      return manual_->schedule(at, std::move(callback), priority);
    fail_dequeued = false;
    const auto gate = completion;
    const auto handle = actual_->schedule(
        at,
        [gate, callback = std::move(callback)] {
          gate->entered.set_value();
          try {
            callback();
            gate->completed.set_value();
          } catch (...) {
            gate->completed.set_exception(std::current_exception());
          }
        },
        priority);
    // Deterministic synchronization, not a timing/performance assertion. The
    // real worker has dequeued and starts the callback before setup returns.
    const auto ready = entered.wait_for(std::chrono::seconds(2));
    actual_->cancel(handle); // cannot revoke a callback already dequeued
    if (ready != std::future_status::ready)
      throw std::runtime_error("actual scheduler callback did not enter");
    throw std::runtime_error("injected setup failure after callback dequeue");
  }
  void cancel(core::ScheduleHandle handle) override { manual_->cancel(handle); }
  bool fail_dequeued{};

private:
  std::shared_ptr<core::ManualScheduler> manual_;
  std::shared_ptr<SimpleScheduler> actual_;

public:
  std::shared_ptr<Completion> completion;
  std::future<void> entered;
  std::future<void> completed;
};

TEST_F(HostIoSignalRuntimeTest,
       DequeuedRealWorkerChecksFailedEntryBeforeAnyModelPublication) {
  auto injected =
      std::make_shared<DequeuedFailureNodeScriptScheduler>(scheduler);
  Build(false, "installation.1", injected);
  const auto before = Observe();
  injected->fail_dequeued = true;
  // Fake host time starts at zero; the real monotonic scheduler sees this
  // positive deadline already due, with no wall-clock sleeps or tight cap.
  const auto request =
      Simulation("node", 1U, "dequeued", true,
                 core::ProgramMockSimulationChangeDTO{1U, false});
  const auto failed = StartSimulation(request);
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, "mock_simulation_start_failed");
  ASSERT_EQ(injected->completed.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(injected->completed.get());
  EXPECT_EQ(Observe().cursor, before.cursor);
  EXPECT_EQ(Observe().latest_sample, before.latest_sample);
  const auto replay = StartSimulation(request);
  ASSERT_FALSE(replay.ok());
  EXPECT_EQ(replay.error().code, failed.error().code);
  EXPECT_EQ(Observe().cursor, before.cursor);
}

TEST_F(HostIoSignalRuntimeTest,
       SchedulingFailureHasNoInitialEffectAndItsReplayRemainsFailed) {
  auto failing = std::make_shared<FailingNodeScriptScheduler>(scheduler);
  Build(false, "installation.1", failing);
  const auto before = Observe();
  const auto request = Simulation("node", 1U, "failed", true);
  failing->fail_next = true;
  const auto failed = StartSimulation(request);
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, "mock_simulation_start_failed");
  EXPECT_EQ(Observe().latest_sample, before.latest_sample);
  EXPECT_EQ(Observe().cursor, before.cursor);
  ASSERT_TRUE(runtime->EndProgramNodeMockSimulation(request.scope, 1U).ok());
  const auto replay = StartSimulation(request);
  ASSERT_FALSE(replay.ok());
  EXPECT_EQ(replay.error().code, failed.error().code);
  EXPECT_EQ(replay.error().message, failed.error().message);
  Advance(2 * kDelay);
  EXPECT_EQ(Observe().latest_sample, before.latest_sample);
  const auto fresh = StartSimulation(Simulation("node", 2U, "fresh", true));
  ASSERT_TRUE(fresh.ok()) << fresh.error().message;
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
}

TEST_F(HostIoSignalRuntimeTest,
       ActualIngressOverflowRetainsFailedStartWithoutModelRollbackOrReplay) {
  Build();
  {
    auto delivery = driver->AcquireDelivery();
    // The actual bounded ingress fills without its queued drain running.
    // The subsequent start's model write can precede an actual publish error.
    for (std::size_t i = 0U; i < 65U; ++i)
      ASSERT_TRUE(driver->SetMockValueLocked(signal(), true).ok());
  }
  const auto request = Simulation("node", 1U, "overflow", false);
  const auto failed = StartSimulation(request);
  ASSERT_FALSE(failed.ok());
  const auto manual = Set(true);
  ASSERT_TRUE(manual.ok());
  EXPECT_TRUE(manual.value().model_value);
  const auto after_manual = Observe();
  const auto replay = StartSimulation(request);
  ASSERT_FALSE(replay.ok());
  EXPECT_EQ(replay.error().code, failed.error().code);
  EXPECT_EQ(replay.error().message, failed.error().message);
  EXPECT_EQ(replay.error().details, failed.error().details);
  Advance(2 * kDelay);
  EXPECT_EQ(Observe().cursor, after_manual.cursor);
  EXPECT_EQ(Observe().latest_sample, after_manual.latest_sample);
}

TEST_F(HostIoSignalRuntimeTest,
       MoreThanSixteenAliasesCoalesceOneInputAndConflictsAreEffectFree) {
  Build();
  auto request = Simulation();
  request.signals.clear();
  for (std::size_t i = 0U; i < 17U; ++i)
    request.signals.push_back(
        {core::TargetRoleId("alias." + std::to_string(100U + i)),
         core::ResourceRoleId("ready"), false,
         core::ProgramMockSimulationChangeDTO{250U, true}});
  const auto started = StartSimulation(request, 1U);
  ASSERT_TRUE(started.ok()) << started.error().message;
  ASSERT_EQ(started.value().signals.size(), 17U);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 2U);
  Advance(kDelay);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 3U);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  auto conflicting = request;
  conflicting.scope.node.node_attempt_id = "conflict";
  conflicting.scope.node_start_index = 2U;
  conflicting.signals.back().start_value = true;
  const auto before = Observe();
  const auto rejected = StartSimulation(conflicting, 1U);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, "mock_simulation_invalid");
  EXPECT_EQ(Observe().cursor, before.cursor);
  EXPECT_EQ(Observe().latest_sample, before.latest_sample);
}

TEST_F(HostIoSignalRuntimeTest,
       ZeroOffsetPublishesInitialThenChangeAndFullWidthIndexDoesNotWrap) {
  Build();
  const auto request =
      Simulation("node", std::numeric_limits<std::uint64_t>::max(), "last",
                 false, core::ProgramMockSimulationChangeDTO{0U, true});
  const auto started = StartSimulation(request, 1U);
  ASSERT_TRUE(started.ok());
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 3U);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  const auto replay = StartSimulation(request, 1U);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), started.value());
  auto wrapped = request;
  wrapped.scope.node_start_index = 0U;
  wrapped.scope.node.node_attempt_id = "wrapped";
  EXPECT_FALSE(StartSimulation(wrapped, 1U).ok());
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 3U);
}

TEST_F(HostIoSignalRuntimeTest,
       BadProofAndClockOverflowRejectAllRowsBeforeAnyEffect) {
  Build();
  const auto request = Simulation("node", 1U, "valid", true);
  ASSERT_TRUE(InitializeSimulationRun(request.scope).ok());
  const auto observed = Observe();
  std::vector<core::ResolvedProgramMockSimulationSignalDTO> wrong{
      {signal(), core::ContentDigest("sha256:" + std::string(64U, 'f')),
       observed.cursor.catalog_generation, observed.installed_incarnation}};
  EXPECT_FALSE(
      runtime->StartProgramNodeMockSimulation(request, wrong, 1U).ok());
  EXPECT_EQ(Observe().cursor, observed.cursor);
  auto overflow = request;
  overflow.signals.front().change->after_ms =
      std::numeric_limits<std::uint64_t>::max();
  EXPECT_FALSE(StartSimulation(overflow, 1U).ok());
  EXPECT_EQ(Observe().cursor, observed.cursor);
  clock->set(std::numeric_limits<core::TimePoint>::max() - 1);
  EXPECT_FALSE(StartSimulation(request, 1U).ok());
  EXPECT_EQ(Observe().cursor, observed.cursor);
  clock->set(0);
  const auto valid = StartSimulation(request, 1U);
  ASSERT_TRUE(valid.ok()); // effect-free errors did not consume this identity
  const auto cursor = Observe().cursor;
  EXPECT_FALSE(StartSimulation(Simulation("other"), 1U).ok());
  EXPECT_EQ(Observe().cursor, cursor);
}

TEST_F(HostIoSignalRuntimeTest,
       DisableCancelsItsPendingChangeButExactReplayKeepsOriginalReceipt) {
  Build();
  const auto request = Simulation();
  const auto first = StartSimulation(request);
  ASSERT_TRUE(first.ok());
  Step("disable");
  ASSERT_TRUE(runtime->DrainPending().ok());
  EXPECT_EQ(Observe().availability, core::IoSignalAvailability::kUnavailable);
  const auto before = Observe();
  const auto replay = StartSimulation(request);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), first.value());
  Advance(2 * kDelay);
  EXPECT_EQ(Observe().latest_sample, before.latest_sample);
  EXPECT_EQ(Observe().cursor, before.cursor);
  EXPECT_FALSE(StartSimulation(Simulation("node", 2U, "new-attempt")).ok());
}

TEST_F(HostIoSignalRuntimeTest,
       NodeEndCancelsPendingChangeKeepsAppliedValueAndOriginalReplay) {
  Build();
  const auto request =
      Simulation("node", 1U, "running", true,
                 core::ProgramMockSimulationChangeDTO{250U, false});
  const auto started = StartSimulation(request, 1U);
  ASSERT_TRUE(started.ok());
  ASSERT_EQ(Observe().latest_sample->publication.value, true);
  const auto applied = Observe();
  ASSERT_TRUE(runtime->EndProgramNodeMockSimulation(request.scope, 1U).ok());
  ASSERT_TRUE(runtime->EndProgramNodeMockSimulation(request.scope, 1U).ok());
  Advance(2 * kDelay);
  EXPECT_EQ(Observe().latest_sample, applied.latest_sample);
  EXPECT_EQ(Observe().cursor, applied.cursor);
  const auto replay = StartSimulation(request, 1U);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), started.value());
  Advance(3 * kDelay);
  EXPECT_EQ(Observe().cursor, applied.cursor);
}

TEST_F(HostIoSignalRuntimeTest,
       OlderNodeEndIsInertAndConflictingAttemptCannotCancelNewerWork) {
  Build();
  const auto old = Simulation("node", 1U, "old", false);
  ASSERT_TRUE(StartSimulation(old, 1U).ok());
  const auto current =
      Simulation("node", 2U, "current", true,
                 core::ProgramMockSimulationChangeDTO{250U, false});
  const auto started = StartSimulation(current, 1U);
  ASSERT_TRUE(started.ok());
  const auto before_end = Observe();
  ASSERT_TRUE(runtime->EndProgramNodeMockSimulation(old.scope, 1U).ok());
  auto conflict = current.scope;
  conflict.node.node_attempt_id = "foreign-attempt";
  const auto rejected = runtime->EndProgramNodeMockSimulation(conflict, 1U);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, "mock_simulation_stale");
  auto foreign_authority = old.scope;
  foreign_authority.program_binding_id =
      core::ProgramBindingId("foreign-binding");
  EXPECT_FALSE(
      runtime->EndProgramNodeMockSimulation(foreign_authority, 1U).ok());
  EXPECT_EQ(Observe().cursor, before_end.cursor);
  Advance(kDelay);
  EXPECT_EQ(Observe().latest_sample->publication.value, false);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence,
            before_end.latest_sample->publication.source_sequence + 1U);
  ASSERT_TRUE(runtime->EndProgramNodeMockSimulation(current.scope, 1U).ok());
  const auto replay = StartSimulation(current, 1U);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), started.value());
}

TEST_F(HostIoSignalRuntimeTest,
       EndBeforeStartRetainsOnlyRealScopeAndPreventsLateResurrection) {
  Build();
  const auto never_started =
      Simulation("never-started", 3U, "ended-unseen", true);
  const auto untouched = Observe();
  ASSERT_TRUE(
      runtime->EndProgramNodeMockSimulation(never_started.scope, 2U).ok());
  const auto first_delayed = StartSimulation(never_started, 2U);
  ASSERT_FALSE(first_delayed.ok());
  EXPECT_EQ(first_delayed.error().code, "mock_simulation_ended");
  EXPECT_EQ(Observe().cursor, untouched.cursor);
  const auto earlier = Simulation("node", 1U, "earlier", false);
  ASSERT_TRUE(StartSimulation(earlier, 2U).ok());
  const auto unseen = Simulation("node", 3U, "ended-before-start", true);
  const auto before_end = Observe();
  ASSERT_TRUE(runtime->EndProgramNodeMockSimulation(unseen.scope, 2U).ok());
  ASSERT_TRUE(runtime->EndProgramNodeMockSimulation(unseen.scope, 2U).ok());
  const auto delayed_start = StartSimulation(unseen, 2U);
  ASSERT_FALSE(delayed_start.ok());
  EXPECT_EQ(delayed_start.error().code, "mock_simulation_ended");
  EXPECT_FALSE(StartSimulation(earlier, 2U).ok());
  const auto another_node = Simulation("other", 1U, "other-attempt");
  EXPECT_FALSE(
      runtime->EndProgramNodeMockSimulation(another_node.scope, 2U).ok());
  Advance(2 * kDelay);
  EXPECT_EQ(Observe().cursor, before_end.cursor);
  EXPECT_EQ(Observe().latest_sample, before_end.latest_sample);
  const auto next = StartSimulation(
      Simulation("node", 4U, "new-start", true, std::nullopt), 2U);
  ASSERT_TRUE(next.ok());
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence,
            before_end.latest_sample->publication.source_sequence + 1U);
}

TEST_F(HostIoSignalRuntimeTest,
       RunClockRequiresExplicitInitialContextAndOrdersFullWidthRevisions) {
  Build();
  const auto request = Simulation();
  const auto before = Observe();
  const auto missing = StartSimulation(request, 1U, false);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, "mock_simulation_run_context_required");
  EXPECT_EQ(Observe().cursor, before.cursor);
  ASSERT_TRUE(InitializeSimulationRun(request.scope).ok());
  ASSERT_TRUE(StartSimulation(request, 1U).ok());
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  ASSERT_TRUE(SetRunPaused(request.scope, maximum, false).ok());
  ASSERT_TRUE(SetRunPaused(request.scope, maximum, false).ok());
  const auto conflict = SetRunPaused(request.scope, maximum, true);
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().code, "mock_simulation_stale");
  ASSERT_TRUE(SetRunPaused(request.scope, maximum - 1U, true).ok());
  auto foreign = request.scope;
  foreign.program_binding_id = core::ProgramBindingId("foreign-binding");
  EXPECT_FALSE(SetRunPaused(foreign, maximum, false).ok());
  EXPECT_FALSE(SetRunPaused(request.scope, 0U, false).ok());
  Advance(kDelay);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 3U);
}

TEST_F(HostIoSignalRuntimeTest,
       ProgramHaltFreezesRemainingDelayAndReplayDoesNotRefreshIt) {
  Build();
  const auto request = Simulation();
  const auto started = StartSimulation(request, 1U);
  ASSERT_TRUE(started.ok());
  Advance(100'000'000);
  ASSERT_TRUE(SetRunPaused(request.scope, 2U, true).ok());
  Advance(1'000'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.value, false);
  ASSERT_TRUE(Set(true).ok()); // manual toggle is still live while halted
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  ASSERT_TRUE(
      Set(false).ok()); // a later manual toggle also leaves the timer intact
  const auto paused = Observe();
  ASSERT_TRUE(SetRunPaused(request.scope, 2U, true).ok());
  ASSERT_TRUE(
      SetRunPaused(request.scope, 1U, false).ok()); // old packet is inert
  EXPECT_FALSE(SetRunPaused(request.scope, 2U, false).ok());
  EXPECT_EQ(Observe().cursor, paused.cursor);
  ASSERT_TRUE(SetRunPaused(request.scope, 3U, false).ok());
  Advance(1'100'000'000);
  ASSERT_TRUE(SetRunPaused(request.scope, 3U, false).ok()); // accepted replay
  const auto replay = StartSimulation(request, 1U);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), started.value());
  Advance(1'149'999'999);
  EXPECT_EQ(Observe().cursor, paused.cursor);
  Advance(1'150'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence,
            paused.latest_sample->publication.source_sequence + 1U);
  EXPECT_EQ(Observe().latest_sample->received_at_ns, 1'150'000'000);
}

TEST_F(HostIoSignalRuntimeTest,
       EndAndActualDisableClearPausedChangesWithoutResumeResurrection) {
  Build();
  const auto ended =
      Simulation("ended", 1U, "ended-attempt", true,
                 core::ProgramMockSimulationChangeDTO{250U, false});
  const auto original = StartSimulation(ended);
  ASSERT_TRUE(original.ok());
  Advance(100'000'000);
  ASSERT_TRUE(SetRunPaused(ended.scope, 2U, true).ok());
  ASSERT_TRUE(runtime->EndProgramNodeMockSimulation(ended.scope, 2U).ok());
  const auto applied = Observe();
  Advance(1'000'000'000);
  ASSERT_TRUE(SetRunPaused(ended.scope, 3U, false).ok());
  Advance(2'000'000'000);
  EXPECT_EQ(Observe().cursor, applied.cursor);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  const auto replay = StartSimulation(ended);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), original.value());
  const auto disabled = Simulation("disabled", 1U, "disabled-attempt", false);
  ASSERT_TRUE(StartSimulation(disabled).ok());
  Advance(2'100'000'000);
  ASSERT_TRUE(SetRunPaused(disabled.scope, 4U, true).ok());
  Step("disable");
  ASSERT_TRUE(runtime->DrainPending().ok());
  const auto closed = Observe();
  EXPECT_EQ(closed.availability, core::IoSignalAvailability::kUnavailable);
  ASSERT_TRUE(SetRunPaused(disabled.scope, 5U, false).ok());
  Advance(3'000'000'000);
  EXPECT_EQ(Observe().availability, core::IoSignalAvailability::kUnavailable);
  EXPECT_EQ(Observe().cursor, closed.cursor);
  EXPECT_EQ(Observe().latest_sample, closed.latest_sample);
}

TEST_F(HostIoSignalRuntimeTest,
       FailedResumePreparationKeepsPauseAndIdenticalRevisionCanRetry) {
  auto injected =
      std::make_shared<DequeuedFailureNodeScriptScheduler>(scheduler);
  Build(false, "installation.1", injected);
  const auto request = Simulation();
  const auto original = StartSimulation(request, 1U);
  ASSERT_TRUE(original.ok());
  Advance(100'000'000);
  ASSERT_TRUE(SetRunPaused(request.scope, 2U, true).ok());
  const auto before = Observe();
  injected->fail_dequeued = true;
  const auto failed = SetRunPaused(request.scope, 3U, false);
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, "mock_simulation_resume_not_applied");
  ASSERT_EQ(injected->completed.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(injected->completed.get());
  Advance(1'000'000'000);
  EXPECT_EQ(Observe().cursor, before.cursor);
  // The exact failed packet was never accepted. Same revision/body may retry;
  // the old accepted pause and remaining 150 ms were preserved, not refreshed.
  ASSERT_TRUE(SetRunPaused(request.scope, 3U, false).ok());
  Advance(1'100'000'000);
  ASSERT_TRUE(SetRunPaused(request.scope, 3U, false).ok());
  EXPECT_FALSE(SetRunPaused(request.scope, 3U, true).ok());
  Advance(1'149'999'999);
  EXPECT_EQ(Observe().cursor, before.cursor);
  Advance(1'150'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence,
            before.latest_sample->publication.source_sequence + 1U);
  const auto replay = StartSimulation(request, 1U);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), original.value());
}

TEST_F(HostIoSignalRuntimeTest,
       NewPausedStartAppliesInitialAndHoldsAllChangesIncludingZeroUntilResume) {
  Build();
  const auto positive =
      Simulation("positive", 1U, "new-paused", true,
                 core::ProgramMockSimulationChangeDTO{250U, false});
  ASSERT_TRUE(InitializeSimulationRun(positive.scope).ok());
  Advance(100'000'000);
  ASSERT_TRUE(SetRunPaused(positive.scope, 2U, true).ok());
  Advance(200'000'000);
  const auto started = StartSimulation(positive, 2U);
  ASSERT_TRUE(started.ok());
  EXPECT_EQ(started.value().started_at_ns, 200'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 2U);
  Advance(1'000'000'000);
  const auto replay_while_paused = StartSimulation(positive, 2U);
  ASSERT_TRUE(replay_while_paused.ok());
  EXPECT_EQ(replay_while_paused.value(), started.value());
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 2U);
  ASSERT_TRUE(SetRunPaused(positive.scope, 3U, false).ok());
  Advance(1'249'999'999);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  Advance(1'250'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.value, false);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 3U);

  ASSERT_TRUE(SetRunPaused(positive.scope, 4U, true).ok());
  const auto zero = Simulation("zero", 1U, "new-zero-paused", false,
                               core::ProgramMockSimulationChangeDTO{0U, true});
  const auto zero_started = StartSimulation(zero, 2U);
  ASSERT_TRUE(zero_started.ok());
  EXPECT_EQ(Observe().latest_sample->publication.value, false);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 4U);
  Advance(2'000'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 4U);
  ASSERT_TRUE(SetRunPaused(zero.scope, 5U, false).ok());
  // The actual queued scheduler runs zero remaining delay only after Resume.
  Advance(2'000'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 5U);
  const auto zero_replay = StartSimulation(zero, 2U);
  ASSERT_TRUE(zero_replay.ok());
  EXPECT_EQ(zero_replay.value(), zero_started.value());
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 5U);
}

TEST_F(HostIoSignalRuntimeTest,
       FirstActualPausedContextHoldsNewZeroChangeUntilResume) {
  Build();
  const auto request =
      Simulation("first-paused", 1U, "actual-paused-at-first-participation",
                 false, core::ProgramMockSimulationChangeDTO{0U, true});
  const auto before = Observe();
  ASSERT_TRUE(SetRunPaused(request.scope, 7U, true).ok());
  EXPECT_EQ(Observe().cursor,
            before.cursor); // clock receipt is no source write
  const auto started = StartSimulation(request, 1U, false);
  ASSERT_TRUE(started.ok());
  EXPECT_EQ(Observe().latest_sample->publication.value, false);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 2U);
  Advance(1'000'000'000);
  const auto replay = StartSimulation(request, 1U, false);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), started.value());
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 2U);
  ASSERT_TRUE(SetRunPaused(request.scope, 8U, false).ok());
  Advance(1'000'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 3U);
}

TEST_F(HostIoSignalRuntimeTest,
       HeldMaskAndRunHaltAreIndependentCausesWithOneRemainingDelay) {
  Build();
  const auto request = Simulation();
  const auto original = StartSimulation(request, 1U);
  ASSERT_TRUE(original.ok());
  Advance(100'000'000);
  ASSERT_TRUE(SetRunPaused(request.scope, 2U, false,
                           {core::ProgramNodeInstanceId("node")})
                  .ok());
  const auto held = Observe();
  Advance(200'000'000);
  ASSERT_TRUE(SetRunPaused(request.scope, 3U, true,
                           {core::ProgramNodeInstanceId("node")})
                  .ok());
  Advance(300'000'000);
  ASSERT_TRUE(SetRunPaused(request.scope, 4U, true, {}).ok());
  Advance(400'000'000);
  ASSERT_TRUE(SetRunPaused(request.scope, 5U, true,
                           {core::ProgramNodeInstanceId("node")})
                  .ok());
  Advance(1'000'000'000);
  ASSERT_TRUE(SetRunPaused(request.scope, 6U, false,
                           {core::ProgramNodeInstanceId("node")})
                  .ok());
  EXPECT_EQ(Observe().cursor, held.cursor);
  Advance(1'100'000'000);
  ASSERT_TRUE(SetRunPaused(request.scope, 7U, false, {}).ok());
  Advance(1'249'999'999);
  EXPECT_EQ(Observe().cursor, held.cursor);
  Advance(1'250'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence,
            held.latest_sample->publication.source_sequence + 1U);
  EXPECT_EQ(Observe().latest_sample->received_at_ns, 1'250'000'000);
  const auto completed = Observe();
  // A held notification arriving after an actual change never undoes it.
  ASSERT_TRUE(SetRunPaused(request.scope, 8U, false,
                           {core::ProgramNodeInstanceId("node")})
                  .ok());
  EXPECT_EQ(Observe().cursor, completed.cursor);
  const auto replay = StartSimulation(request, 1U);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), original.value());
}

TEST_F(HostIoSignalRuntimeTest,
       FreshStartsSampleCompleteHeldMaskAndEndNeverRearmsChanges) {
  Build();
  const auto request =
      Simulation("future", 1U, "first-held", true,
                 core::ProgramMockSimulationChangeDTO{0U, false});
  ASSERT_TRUE(SetRunPaused(request.scope, 1U, false,
                           {core::ProgramNodeInstanceId("future"),
                            core::ProgramNodeInstanceId("unused")})
                  .ok());
  // This plain service fixture trusts captured node IDs. The actual Kernel
  // validates them; mask-only nodes create no runtime entry or fake START.
  const auto first = StartSimulation(request, 2U, false);
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  Advance(1'000'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 2U);
  ASSERT_TRUE(runtime->EndProgramNodeMockSimulation(request.scope, 2U).ok());
  ASSERT_TRUE(SetRunPaused(request.scope, 2U, false, {}).ok());
  Advance(2'000'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 2U);
  const auto historical = StartSimulation(request, 2U, false);
  ASSERT_TRUE(historical.ok());
  EXPECT_EQ(historical.value(), first.value());

  ASSERT_TRUE(SetRunPaused(request.scope, 3U, false,
                           {core::ProgramNodeInstanceId("future")})
                  .ok());
  const auto newer = Simulation("future", 2U, "fresh-held-attempt", false,
                                core::ProgramMockSimulationChangeDTO{0U, true});
  const auto started = StartSimulation(newer, 2U, false);
  ASSERT_TRUE(started.ok());
  EXPECT_EQ(Observe().latest_sample->publication.value, false);
  Advance(3'000'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 3U);
  ASSERT_TRUE(SetRunPaused(newer.scope, 4U, false, {}).ok());
  Advance(3'000'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence, 4U);
}

TEST_F(HostIoSignalRuntimeTest,
       CombinedSnapshotOrderingRejectsMaskConflictsWithoutRefreshingDelay) {
  Build();
  const auto request = Simulation();
  ASSERT_TRUE(SetRunPaused(request.scope, 3U, false,
                           {core::ProgramNodeInstanceId("node")})
                  .ok());
  ASSERT_TRUE(StartSimulation(request, 1U, false).ok());
  const auto held = Observe();
  ASSERT_TRUE(SetRunPaused(request.scope, 3U, false,
                           {core::ProgramNodeInstanceId("node")})
                  .ok());
  const auto conflict = SetRunPaused(request.scope, 3U, false, {});
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().code, "mock_simulation_stale");
  ASSERT_TRUE(SetRunPaused(request.scope, 2U, false, {}).ok());
  const auto duplicate = SetRunPaused(request.scope, 4U, false,
                                      {core::ProgramNodeInstanceId("node"),
                                       core::ProgramNodeInstanceId("node")});
  ASSERT_FALSE(duplicate.ok());
  EXPECT_EQ(duplicate.error().code, "mock_simulation_invalid");
  const auto unsorted = SetRunPaused(
      request.scope, 4U, false,
      {core::ProgramNodeInstanceId("z"), core::ProgramNodeInstanceId("a")});
  EXPECT_FALSE(unsorted.ok());
  const auto overlong =
      SetRunPaused(request.scope, 4U, false,
                   {core::ProgramNodeInstanceId(std::string(129U, 'x'))});
  ASSERT_FALSE(overlong.ok());
  EXPECT_EQ(overlong.error().code, "mock_simulation_invalid");
  const auto embedded_nul = SetRunPaused(
      request.scope, 4U, false,
      {core::ProgramNodeInstanceId(std::string("node\0suffix", 11U))});
  ASSERT_FALSE(embedded_nul.ok());
  EXPECT_EQ(embedded_nul.error().code, "mock_simulation_invalid");
  Advance(1'000'000'000);
  EXPECT_EQ(Observe().cursor, held.cursor);
  ASSERT_TRUE(SetRunPaused(request.scope, 4U, false, {}).ok());
  Advance(1'100'000'000);
  ASSERT_TRUE(SetRunPaused(request.scope, 4U, false, {}).ok());
  ASSERT_TRUE(SetRunPaused(request.scope, 3U, true,
                           {core::ProgramNodeInstanceId("node")})
                  .ok());
  Advance(1'249'999'999);
  EXPECT_EQ(Observe().cursor, held.cursor);
  Advance(1'250'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence,
            held.latest_sample->publication.source_sequence + 1U);
}

TEST_F(HostIoSignalRuntimeTest,
       FailedMixedHeldUpdateKeepsWholeAcceptedSnapshotForIdenticalRetry) {
  auto injected =
      std::make_shared<DequeuedFailureNodeScriptScheduler>(scheduler);
  Build(false, "installation.1", injected);
  const auto left = Simulation("left", 1U, "left-attempt", false);
  const auto right =
      Simulation("right", 1U, "right-attempt", true,
                 core::ProgramMockSimulationChangeDTO{400U, false});
  ASSERT_TRUE(
      SetRunPaused(left.scope, 1U, false, {core::ProgramNodeInstanceId("left")})
          .ok());
  const auto left_started = StartSimulation(left, 2U, false);
  ASSERT_TRUE(left_started.ok());
  ASSERT_TRUE(StartSimulation(right, 2U, false).ok());
  const auto before = Observe();
  Advance(100'000'000);
  injected->fail_dequeued = true;
  // This one snapshot would resume left and pause right. Preparation failure
  // must leave BOTH previous effective clock states and the revision intact.
  const auto failed = SetRunPaused(left.scope, 2U, false,
                                   {core::ProgramNodeInstanceId("right")});
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, "mock_simulation_resume_not_applied");
  ASSERT_EQ(injected->completed.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(injected->completed.get());
  Advance(399'999'999);
  EXPECT_EQ(Observe().cursor, before.cursor);
  Advance(400'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.value, false);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence,
            before.latest_sample->publication.source_sequence + 1U);
  Advance(500'000'000);
  ASSERT_TRUE(SetRunPaused(left.scope, 2U, false,
                           {core::ProgramNodeInstanceId("right")})
                  .ok());
  const auto right_completed = Observe();
  Advance(749'999'999);
  EXPECT_EQ(Observe().cursor, right_completed.cursor);
  Advance(750'000'000);
  EXPECT_EQ(Observe().latest_sample->publication.value, true);
  EXPECT_EQ(Observe().latest_sample->publication.source_sequence,
            right_completed.latest_sample->publication.source_sequence + 1U);
  const auto replay = StartSimulation(left, 2U, false);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), left_started.value());
}

} // namespace
} // namespace manyforge::roboplan_backend::host_detail
