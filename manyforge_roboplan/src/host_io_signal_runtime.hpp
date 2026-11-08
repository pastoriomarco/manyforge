#pragma once

#include "host_carried_object_lost_signals.hpp"
#include "io_provider_lifecycle_product.hpp"
#include "mock_io_input_result.hpp"

#include "execution_host_config.hpp"
#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <manyforge_core/execution_interrupt_support.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/io_signal_ingress.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <variant>

namespace manyforge::roboplan_backend::host_detail {

// Actual component DriverRecords own leaves and lifecycle. This publication
// owns its catalog, bounded ingress and timers; the Kernel retains the same
// service pointer. Scheduling reuses the existing host scheduler.
class HostIoSignalRuntime final
    : public core::IIoSignalService,
      public std::enable_shared_from_this<HostIoSignalRuntime> {
public:
  struct SourceInput final {
    std::vector<deployment::RetainedIoSignalConfiguration> rows;
    rclcpp::Node::SharedPtr node;
    std::optional<core::RuntimeUnitEpochBindingDTO> installation_occurrence;
    std::optional<GeometricContactInputBinding> geometric_contact;
    // Sole actual leaf/lifecycle owner for every qualified input. Empty
    // source inventory remains valid for a real BB-only service.
    std::shared_ptr<IoProviderDriverRecord> driver{};
  };

  static std::shared_ptr<HostIoSignalRuntime>
  Create(const core::RequestOwnerEpoch &owner, std::uint64_t generation,
         std::map<core::InterruptSourceId, SourceInput> sources,
         std::shared_ptr<core::IClock> clock,
         std::shared_ptr<core::IScheduler> scheduler,
         std::size_t compiled_blackboard_key_capacity = 0U,
         std::vector<deployment::RetainedIoSignalConfiguration> system_rows =
             {},
         std::vector<deployment::RetainedIoSignalConfiguration> adapter_rows = {}) {
    Require(clock && scheduler &&
                (!sources.empty() || !system_rows.empty() || !adapter_rows.empty() ||
                 compiled_blackboard_key_capacity > 0U),
            "I/O installation has no compiled source/time authority");
    // A machine's I/O member signals (profile
    // `component_adapter`): one in-process source per signal
    // (`<member>.<signal>`), fed by the family's I/O update sink; no driver.
    // Mock outputs (`mock_output`) are in-process sources too: each accepted
    // write is published back as their sample.
    std::map<core::InterruptSourceId,
             std::vector<deployment::RetainedIoSignalConfiguration>>
        adapter_sources;
    for (const auto &row : adapter_rows) {
      // An output row has no ingress queue: its source holds its latest
      // sample only.
      Require((row.provider.profile == deployment::kComponentAdapterIoProfile ||
               (row.provider.profile == "mock_output" &&
                row.descriptor.direction == core::IoSignalDirection::kOutput)) &&
                  !sources.contains(row.descriptor.source_id),
              "adapter signal source is not one exact in-process source");
      auto &rows = adapter_sources[row.descriptor.source_id];
      Require(rows.empty() || rows.front().descriptor.signal_id.controllable_component_id ==
                                  row.descriptor.signal_id.controllable_component_id,
              "adapter signal source spans several components");
      rows.push_back(row);
    }
    // Compiled system signals: one in-process source with no driver or
    // lifecycle; the host owns it.
    std::optional<core::InterruptSourceId> system_source;
    for (const auto &row : system_rows) {
      Require(row.provider.profile == deployment::kSystemCarriedObjectProfile &&
                  row.provider.ingress_depth > 0U &&
                  (!system_source || *system_source == row.descriptor.source_id) &&
                  !sources.contains(row.descriptor.source_id),
              "system signal source is not one exact in-process source");
      system_source = row.descriptor.source_id;
    }
    Require(sources.size() < std::numeric_limits<std::size_t>::max(),
            "I/O source capacity arithmetic overflow");
    core::IoSignalLimitsDTO limits{
        sources.size() + adapter_sources.size() + (system_source ? 1U : 0U) +
            (compiled_blackboard_key_capacity > 0U ? 1U : 0U),
        0U,
        compiled_blackboard_key_capacity > 0U ? 64U : 0U,
        core::kMaxProgramTreeNodes,
        kMaximumIdentityBytes,
        core::kMaxLifecycleErrorCodeBytes +
            core::kMaxLifecycleErrorMessageBytes +
            core::kMaxLifecycleErrorDetails *
                (core::kMaxLifecycleErrorDetailKeyBytes +
                 core::kMaxLifecycleErrorDetailValueBytes),
        {}};
    core::IoSignalCatalogDTO catalog{owner, generation, {}, {}};
    std::size_t drain_budget{};
    for (const auto &[source, input] : sources) {
      Require(!input.rows.empty() && input.driver &&
                  input.installation_occurrence == input.driver->runtime_unit_epoch(),
              "I/O source has no exact actual driver occurrence");
      const auto depth = input.rows.front().provider.ingress_depth;
      if (input.rows.front().provider.profile == "mock")
        catalog.authoritative_synthetic_sources.push_back(source);
      Require(depth > 0U && depth < std::numeric_limits<std::size_t>::max() &&
                  drain_budget <=
                      std::numeric_limits<std::size_t>::max() - depth - 1U,
              "I/O source capacity arithmetic overflow");
      drain_budget +=
          depth + 1U; // Retained acquisition queue and its loss barrier.
      limits.records_per_source = std::max(limits.records_per_source, depth);
      limits.records_per_source_overrides.emplace(source, depth);
      for (const auto &row : input.rows)
        catalog.signals.push_back(row.descriptor);
    }
    if (system_source) {
      const auto depth = system_rows.front().provider.ingress_depth;
      catalog.authoritative_synthetic_sources.push_back(*system_source);
      drain_budget += depth + 1U;
      limits.records_per_source = std::max(limits.records_per_source, depth);
      limits.records_per_source_overrides.emplace(*system_source, depth);
      for (const auto &row : system_rows)
        catalog.signals.push_back(row.descriptor);
    }
    for (const auto &[source, rows] : adapter_sources) {
      Require(!system_source || *system_source != source,
              "adapter signal source repeats the system source");
      const auto depth = std::max<std::size_t>(rows.front().provider.ingress_depth, 1U);
      drain_budget += depth + 1U;
      limits.records_per_source = std::max(limits.records_per_source, depth);
      limits.records_per_source_overrides.emplace(source, depth);
      for (const auto &row : rows)
        catalog.signals.push_back(row.descriptor);
    }
    Require(catalog.signals.size() <= std::numeric_limits<std::size_t>::max() -
                                          compiled_blackboard_key_capacity,
            "I/O key capacity arithmetic overflow");
    // Each core-held `<output>.changed_outside` signal has its own core
    // source and counts against both bounds.
    const auto changed_outside = static_cast<std::size_t>(
        std::count_if(catalog.signals.begin(), catalog.signals.end(),
                      [](const auto &descriptor) { return core::HoldsChangedOutside(descriptor); }));
    limits.maximum_sources += changed_outside;
    limits.maximum_signals =
        catalog.signals.size() + changed_outside + compiled_blackboard_key_capacity;
    auto made =
        core::IoSignalSubsystem::Create(std::move(catalog), limits, *clock);
    Require(made.ok(), "sealed I/O signal catalog cannot be installed");
    auto signals =
        std::shared_ptr<core::IoSignalSubsystem>(std::move(made.value()));
    auto result = std::shared_ptr<HostIoSignalRuntime>(
        new HostIoSignalRuntime(std::move(clock), std::move(scheduler), signals,
                                drain_budget, generation));
    for (const auto &[source, input] : sources) {
      Require(input.installation_occurrence.has_value(),
              "I/O source has no actual runtime occurrence");
      const auto incarnation =
          core::IoSourceIncarnation(content_identity::framed_sha256(
              "manyforge.host-io-source", "manyforge.host-io-source.v1",
              CanonicalValue(CanonicalValue::Object{
                  {"owner_epoch", CanonicalValue(owner.value())},
                  {"generation", CanonicalValue(std::to_string(generation))},
                  {"source_id", CanonicalValue(source.value())},
                  {"runtime_unit_id",
                   CanonicalValue(
                       input.installation_occurrence->runtime_unit_id.value())},
                  {"runtime_epoch",
                   CanonicalValue(input.installation_occurrence->runtime_epoch
                                      .value())}})));
      Require(signals->InstallSource(source, incarnation, std::nullopt).ok(),
              "sealed I/O source incarnation cannot be installed");
      result->incarnations_.emplace(source, incarnation);
    }
    std::optional<core::IoSourceIncarnation> system_incarnation;
    if (system_source) {
      system_incarnation =
          core::IoSourceIncarnation(content_identity::framed_sha256(
              "manyforge.host-io-source", "manyforge.host-io-source.v1",
              CanonicalValue(CanonicalValue::Object{
                  {"owner_epoch", CanonicalValue(owner.value())},
                  {"generation", CanonicalValue(std::to_string(generation))},
                  {"source_id", CanonicalValue(system_source->value())}})));
      Require(signals->InstallSource(*system_source, *system_incarnation,
                                     std::nullopt)
                  .ok(),
              "system signal source incarnation cannot be installed");
      result->incarnations_.emplace(*system_source, *system_incarnation);
    }
    for (const auto &[source, rows] : adapter_sources) {
      const auto incarnation =
          core::IoSourceIncarnation(content_identity::framed_sha256(
              "manyforge.host-io-source", "manyforge.host-io-source.v1",
              CanonicalValue(CanonicalValue::Object{
                  {"owner_epoch", CanonicalValue(owner.value())},
                  {"generation", CanonicalValue(std::to_string(generation))},
                  {"source_id", CanonicalValue(source.value())}})));
      Require(signals->InstallSource(source, incarnation, std::nullopt).ok(),
              "adapter signal source incarnation cannot be installed");
      result->incarnations_.emplace(source, incarnation);
      result->adapter_sources_.emplace(source, AdapterSource{source, incarnation, 0U});
      for (const auto &row : rows)
        result->adapter_signal_sources_.emplace(row.descriptor.signal_id, source);
    }
    std::shared_ptr<IngressWake> wake;
    if (!sources.empty() || system_source || !adapter_sources.empty()) {
      auto ingress = core::IoSignalIngress::Create(signals);
      Require(ingress.ok(), "sealed I/O ingress cannot be installed");
      result->ingress_ =
          std::shared_ptr<core::IoSignalIngress>(std::move(ingress.value()));
      wake = std::make_shared<IngressWake>(result->ingress_, result);
      result->adapter_ingress_ = wake;
    }
    if (system_source) {
      MockBooleanInputSource source{*system_source, *system_incarnation, {}};
      for (const auto &row : system_rows) {
        source.inputs.push_back({row.descriptor, false});
        result->system_signals_.push_back(row.descriptor.signal_id);
      }
      auto provider =
          MockBooleanInputProvider::Create(wake, *result->clock_, std::move(source));
      Require(provider.ok(), "system signal source cannot be created");
      result->system_provider_ = std::move(provider.value());
    }
    for (const auto &[source, input] : sources) {
      const auto &incarnation = result->incarnations_.at(source);
      const auto actual = input.driver->runtime_unit_epoch();
      Require(input.installation_occurrence == actual,
              "I/O source does not name its actual driver occurrence");
      auto [entry, inserted] = result->driver_inputs_.try_emplace(input.driver);
      if (inserted) result->drivers_.push_back(input.driver);
      entry->second.push_back({input.rows, input.node, wake, incarnation,
                               input.geometric_contact, result->scheduler_});
      for (const auto &row : input.rows) {
        const auto signal = row.descriptor.signal_id;
        Require(result->input_profiles_.emplace(signal, row.provider.profile)
                    .second,
                "I/O input repeats an installed signal");
        if (row.provider.profile == "mock")
          result->mock_inputs_.emplace(
              signal, MockInput{row.provider.ingress_depth, input.driver});
      }
      // The record alone installs the real initially paused leaf under its
      // delivery barrier. Runtime creation/Start never fabricates Enable.
    }
    std::sort(result->drivers_.begin(), result->drivers_.end(), DeliveryLockOrder);
    return result;
  }

  using DriverInputInstallations =
      std::map<std::shared_ptr<IoProviderDriverRecord>,
               std::vector<IoProviderInputSourceInstallation>,
               std::owner_less<std::shared_ptr<IoProviderDriverRecord>>>;
  const DriverInputInstallations &driver_input_installations() const noexcept {
    return driver_inputs_;
  }

  /// A Mock output whose accepted writes are published as its sample with
  /// source `manyforge_write`.
  void AddMockOutput(std::shared_ptr<MockBooleanOutputProvider> output,
                     const core::IoSignalId &signal) {
    Require(!started_ && output && adapter_signal_sources_.contains(signal),
            "Mock output has no installed in-process source");
    MockEchoChannel &channel = MockChannel(std::move(output));
    channel.written.push_back(signal);
  }
  void AddMockEcho(std::shared_ptr<MockBooleanOutputProvider> output,
                   const core::IoSignalId &signal, core::Duration delay) {
    Require(!started_ && output && mock_inputs_.contains(signal) && delay >= 0,
            "Mock echo has no exact configured input/output");
    for (const auto &channel : mock_echoes_)
      for (const auto &target : channel.targets)
        Require(target.signal != signal, "Mock input repeats an echo source");
    MockChannel(std::move(output)).targets.push_back({signal, delay, 0U, {}});
  }
  std::function<void()>
  cancellation_hook(const std::shared_ptr<IoProviderDriverRecord> &driver) {
    return [weak = weak_from_this(), actual = std::weak_ptr(driver)] {
      if (auto self = weak.lock()) {
        const auto owner = actual.lock();
        if (!owner)
          return;
        // Invoked with that actual record's delivery lock already held.
        std::lock_guard lock(self->mutex_);
        for (auto &[key, entry] : self->node_simulations_)
          for (auto &change : entry.changes) {
            const auto input = self->mock_inputs_.find(change.signal);
            if (input != self->mock_inputs_.end() &&
                input->second.driver == owner)
              self->CancelChangeLocked(change);
          }
      }
    };
  }
  using InputLifecycleProcessor = std::function<core::Result<void>(
      const std::shared_ptr<core::IIoSignalService> &,
      std::span<const core::InterruptSourceId>, bool)>;
  IoProviderDriverRecord::InputLifecycleHook
  input_lifecycle_hook(const std::shared_ptr<IoProviderDriverRecord> &driver) {
    std::vector<core::InterruptSourceId> sources;
    if (const auto found = driver_inputs_.find(driver);
        found != driver_inputs_.end())
      for (const auto &input : found->second)
        sources.push_back(input.rows.front().descriptor.source_id);
    return [weak = weak_from_this(), sources = std::move(sources)](
               bool disabled) -> core::Result<void> {
      const auto self = weak.lock();
      if (!self)
        return core::Result<void>::Err(
            {"invalid_state_transition", "Input lifecycle owner expired", {}});
      // The actual driver delivery barrier is held. Drain eligible prior
      // evidence before declaring planned cessation, without a Kernel callback.
      // Enable must also consume the prior Pause's queued loss while the old
      // arm fence remains in place; a stale pre-Pause sample cannot rearm.
      if (self->ingress_) {
        const auto drained = self->ingress_->Drain(self->drain_budget_);
        if (!drained.ok())
          return core::Result<void>::Err(drained.error());
        if (drained.value().blocked_error)
          return core::Result<void>::Err(*drained.value().blocked_error);
      }
      InputLifecycleProcessor processor;
      {
        std::lock_guard lock(self->mutex_);
        processor = self->input_lifecycle_processor_;
      }
      // A staged, not-yet-Applied service has no armed Kernel catalog.
      return processor ? processor(self, sources, disabled)
                       : core::Result<void>::Ok();
    };
  }
  IoProviderDriverRecord::InputAcquisitionHook
  input_acquisition_hook(
      const std::shared_ptr<IoProviderDriverRecord> &driver) {
    std::vector<core::IoSignalId> expected_signals;
    if (const auto found = driver_inputs_.find(driver);
        found != driver_inputs_.end())
      for (const auto &input : found->second)
        if (input.geometric_contact)
          for (const auto &row : input.rows)
            expected_signals.push_back(row.descriptor.signal_id);
    std::sort(expected_signals.begin(), expected_signals.end());
    return [weak = weak_from_this(),
            expected_signals = std::move(expected_signals)](
               std::span<const IoProviderInputAcquisitionBaseline> baselines)
               -> core::Result<void> {
      if (expected_signals.empty())
        return core::Result<void>::Ok();
      std::vector<core::IoSignalId> actual_signals;
      for (const auto &baseline : baselines)
        actual_signals.insert(actual_signals.end(), baseline.signals.begin(),
                              baseline.signals.end());
      std::sort(actual_signals.begin(), actual_signals.end());
      if (actual_signals != expected_signals)
        return core::Result<void>::Err(
            {"io_source_unavailable",
             "Geometric contact source has no initial acquisition", {}});
      const auto self = weak.lock();
      if (!self)
        return core::Result<void>::Err(
            {"invalid_state_transition", "I/O acquisition owner expired", {}});
      const auto drained = self->DrainPending();
      if (!drained.ok())
        return core::Result<void>::Err(drained.error());
      if (drained.value().blocked_error)
        return core::Result<void>::Err(*drained.value().blocked_error);
      for (const auto &baseline : baselines)
        for (const auto &signal : baseline.signals) {
          const auto observed = self->signals_->ReadSignal(signal);
          const bool matches =
              observed.ok() && observed.value().latest_sample &&
              observed.value().latest_sample->publication.incarnation ==
                  baseline.incarnation &&
              observed.value().latest_sample->publication.source_sequence >=
                  baseline.source_sequence;
          const bool current =
              matches &&
              ((observed.value().latest_sample->publication.validity ==
                    core::IoSampleValidity::kValid &&
                (observed.value().availability ==
                     core::IoSignalAvailability::kFresh ||
                 observed.value().availability ==
                     core::IoSignalAvailability::kStale)) ||
               (observed.value().latest_sample->publication.validity ==
                    core::IoSampleValidity::kUnavailable &&
                observed.value().availability ==
                    core::IoSignalAvailability::kUnavailable));
          if (!current)
            return core::Result<void>::Err(
                {"io_source_unavailable",
                 "Geometric contact source has no initial acquisition",
                 {{"signal_name", signal.name}}});
        }
      return core::Result<void>::Ok();
    };
  }
  core::Result<core::IoIngressDrainResultDTO> DrainPending() {
    Require(ingress_ != nullptr, "I/O runtime has no input ingress");
    auto drained = ingress_->Drain(drain_budget_);
    RequestRuleEvidenceProcessing();
    return drained;
  }

  // Installed once by the actual host. The callback acquires its existing
  // owner call lease; it never borrows a raw replaceable component lifetime.
  void SetRuleEvidenceProcessor(std::function<core::Result<bool>()> processor,
                                InputLifecycleProcessor input_lifecycle = {}) {
    std::lock_guard lock(mutex_);
    Require(processor && !rule_evidence_processor_,
            "I/O rule evidence processor has no unique actual owner");
    rule_evidence_processor_ = std::move(processor);
    input_lifecycle_processor_ = std::move(input_lifecycle);
  }
  [[nodiscard]] bool has_rule_evidence_processor() const {
    std::lock_guard lock(mutex_);
    return static_cast<bool>(rule_evidence_processor_);
  }
  void RequestRuleEvidenceProcessing() {
    std::lock_guard lock(mutex_);
    ScheduleRuleEvidenceLocked(clock_->now());
    ScheduleSampleExpiryLocked();
  }
  void DetachRuleEvidenceProcessor() noexcept {
    std::lock_guard lock(mutex_);
    rule_evidence_processor_ = {};
    input_lifecycle_processor_ = {};
    if (rule_evidence_handle_) scheduler_->cancel(*rule_evidence_handle_);
    rule_evidence_handle_.reset();
    rule_evidence_deadline_.reset();
  }

  ~HostIoSignalRuntime() { Shutdown(); }
  std::shared_ptr<core::IIoSignalService> service() {
    return shared_from_this();
  }
  std::uint64_t generation() const noexcept { return generation_; }

  core::Result<core::IoSignalObservationDTO>
  ReadSignal(const core::IoSignalId &signal) const override {
    return signals_->ReadSignal(signal);
  }
  core::Result<core::IoWatchResultDTO>
  ReadWatch(const core::IoWatchRegistrationDTO &token) const override {
    return signals_->ReadWatch(token);
  }
  core::Result<core::IoWatchRegistrationDTO>
  RegisterLevelWatch(const core::IoLevelWatchRequestDTO &request) override {
    auto delivery_locks = AcquireProviderDeliveries();
    const auto registered = signals_->RegisterLevelWatch(request);
    if (!registered.ok())
      return registered;
    const auto observed = signals_->ReadWatch(registered.value());
    if (!observed.ok()) {
      static_cast<void>(signals_->ReleaseWatch(registered.value()));
      return core::Result<core::IoWatchRegistrationDTO>::Err(observed.error());
    }
    return registered;
  }
  core::Result<void>
  ReleaseWatch(const core::IoWatchRegistrationDTO &token) override {
    auto delivery_locks = AcquireProviderDeliveries();
    return signals_->ReleaseWatch(token);
  }
  core::Result<void>
  ReleaseNodeWatches(const core::IoWatchScopeDTO &scope) override {
    auto delivery_locks = AcquireProviderDeliveries();
    return signals_->ReleaseNodeWatches(scope);
  }
  core::Result<void> ReleaseRunWatches(const core::RequestOwnerEpoch &owner,
                                       const core::ProgramRunId &run) override {
    auto delivery_locks = AcquireProviderDeliveries();
    const auto released = signals_->ReleaseRunWatches(owner, run);
    if (released.ok()) {
      std::lock_guard lock(mutex_);
      for (auto it = node_simulations_.begin();
           it != node_simulations_.end();) {
        if (std::get<0>(it->first) == owner.value() &&
            std::get<1>(it->first) == run.value()) {
          for (auto &change : it->second.changes)
            CancelChangeLocked(change);
          it = node_simulations_.erase(it);
        } else {
          ++it;
        }
      }
      run_simulation_contexts_.erase({owner.value(), run.value()});
    }
    return released;
  }
  core::Result<core::IoRuleRegistrationDTO>
  ArmRule(const core::IoRuleArmRequestDTO &request) override {
    // Retained source evidence alone decides conditions. A rule/watch never
    // asks a Mock provider to manufacture its desired value (132/133).
    if (std::holds_alternative<core::IoActivationRuleScopeDTO>(request.scope)) {
      if (request.search_expectation)
        return core::Result<core::IoRuleRegistrationDTO>::Err(
            {"invalid_request", "A global guard cannot request a Mock search answer",
             {{"reason", "io_global_rule_search_expectation_invalid"}}});
      return signals_->ArmRule(request);
    }
    auto delivery_locks = AcquireProviderDeliveries();
    const auto armed = signals_->ArmRule(request);
    if (armed.ok()) RequestRuleEvidenceProcessing();
    return armed;
  }
  core::Result<core::IoRuleServiceConfigurationDTO>
  RuleServiceConfiguration() const override {
    return signals_->RuleServiceConfiguration();
  }
  core::Result<core::IoRuleConditionSnapshotDTO>
  ReadRuleConditionSnapshot(const core::IoRuleRegistrationDTO &token) const override {
    return signals_->ReadRuleConditionSnapshot(token);
  }

  core::Result<core::IoRuleEvidenceDTO>
  ReadRuleEvidence(const core::IoRuleRegistrationDTO &token) const override {
    return signals_->ReadRuleEvidence(token);
  }
  core::Result<core::IoRuleEvidenceDTO>
  RetireRule(const core::IoRuleRegistrationDTO &token) override {
    if (std::holds_alternative<core::IoActivationRuleScopeDTO>(token.scope))
      return signals_->RetireRule(token);
    auto delivery_locks = AcquireProviderDeliveries();
    const auto retired = signals_->RetireRule(token);
    if (retired.ok())
      RequestRuleEvidenceProcessing();
    return retired;
  }
  core::Result<core::ProgramNodeMockSimulationAcknowledgementDTO>
  StartProgramNodeMockSimulation(
      const core::ProgramNodeMockSimulationRequestDTO &request,
      std::span<const core::ResolvedProgramMockSimulationSignalDTO> resolved,
      std::size_t maximum_node_entries) override {
    using Reply =
        core::Result<core::ProgramNodeMockSimulationAcknowledgementDTO>;
    auto delivery_locks = AcquireProviderDeliveries();
    const auto key = SimulationKey(request.scope);
    if (request.scope.node_start_index == 0U)
      return Reply::Err(SimulationError("mock_simulation_invalid",
                                        "Invalid node simulation shape"));
    {
      std::lock_guard lock(mutex_);
      if (!active_)
        return Reply::Err(SimulationError("io_signal_service_unavailable",
                                          "I/O runtime is retired"));
      const auto previous = node_simulations_.find(key);
      if (previous != node_simulations_.end()) {
        // The original result is historical start evidence, not a current
        // freshness grant. Source proof is not additional caller replay data.
        const auto *started =
            std::get_if<core::ProgramNodeMockSimulationRequestDTO>(
                &previous->second.origin);
        if (started && *started == request && previous->second.result)
          return *previous->second.result;
        const auto &old_scope = SimulationScope(previous->second);
        if (!started && old_scope == request.scope)
          return Reply::Err(
              SimulationError("mock_simulation_ended",
                              "Node attempt already ended before its start"));
        auto expected = old_scope;
        expected.node = request.scope.node;
        expected.node_start_index = request.scope.node_start_index;
        if (request.scope.node_start_index <= old_scope.node_start_index ||
            request.scope.node.node_attempt_id ==
                old_scope.node.node_attempt_id ||
            expected != request.scope)
          return Reply::Err(SimulationError(
              "mock_simulation_stale", "Node start is stale or conflicting"));
      } else {
        const auto in_run =
            std::count_if(node_simulations_.begin(), node_simulations_.end(),
                          [&](const auto &row) {
                            return std::get<0>(row.first) == std::get<0>(key) &&
                                   std::get<1>(row.first) == std::get<1>(key);
                          });
        if (static_cast<std::size_t>(in_run) >= maximum_node_entries)
          return Reply::Err(SimulationError("mock_simulation_invalid",
                                            "Captured node capacity exceeded"));
      }
    }

    if (maximum_node_entries == 0U || resolved.size() != request.signals.size())
      return Reply::Err(SimulationError(
          "mock_simulation_invalid", "Invalid resolved node simulation shape"));
    bool node_paused;
    {
      std::lock_guard lock(mutex_);
      const auto context =
          run_simulation_contexts_.find(RunKey(RunScope(request.scope)));
      if (context == run_simulation_contexts_.end())
        return Reply::Err(SimulationError(
            "mock_simulation_run_context_required",
            "An explicit Run clock receipt is required before node start"));
      if (context->second.scope != RunScope(request.scope))
        return Reply::Err(SimulationError("mock_simulation_stale",
                                          "Run clock authority scope changed"));
      node_paused =
          RunPausesNode(context->second, request.scope.node.node_instance_id);
    }
    const auto anchor = clock_->now();
    if (anchor < 0)
      return Reply::Err(
          SimulationError("mock_simulation_invalid", "Invalid host time"));
    core::ProgramNodeMockSimulationAcknowledgementDTO acknowledgement{
        request.scope, anchor, {}};
    acknowledgement.signals.reserve(request.signals.size());
    struct ActualRow final {
      core::ProgramMockSimulationSignalDTO row;
      std::optional<core::TimePoint> deadline;
      std::optional<core::Duration> delay;
    };
    std::map<core::IoSignalId, ActualRow> actual_rows;
    std::optional<std::pair<std::string, std::string>> previous_selector;
    for (std::size_t index = 0; index < request.signals.size(); ++index) {
      const auto &row = request.signals[index];
      const auto selector = std::make_pair(row.signal.component_id.value(),
                                           row.signal.signal_name);
      if (selector.first.empty() || selector.second.empty() ||
          (previous_selector && selector <= *previous_selector))
        return Reply::Err(
            SimulationError("mock_simulation_invalid",
                            "Selectors are not canonical and unique"));
      previous_selector = selector;
      std::optional<core::TimePoint> deadline;
      std::optional<core::Duration> delay;
      if (row.change) {
        constexpr std::uint64_t ns_per_ms = 1'000'000U;
        if (row.change->after_ms >
            static_cast<std::uint64_t>(
                std::numeric_limits<core::Duration>::max()) /
                ns_per_ms)
          return Reply::Err(SimulationError("mock_simulation_invalid",
                                            "Change offset overflows"));
        const auto offset =
            static_cast<core::Duration>(row.change->after_ms * ns_per_ms);
        delay = offset;
        if (!node_paused) {
          if (anchor > std::numeric_limits<core::TimePoint>::max() - offset)
            return Reply::Err(SimulationError("mock_simulation_invalid",
                                              "Change deadline overflows"));
          deadline = anchor + offset;
        }
      }
      const auto &proof = resolved[index];
      const auto observed = signals_->ReadSignal(proof.signal_id);
      if (!observed.ok())
        return Reply::Err(observed.error());
      const auto digest =
          core::ComputeIoSignalDescriptorDigest(observed.value().descriptor);
      if (!digest.ok())
        return Reply::Err(digest.error());
      const auto profile = input_profiles_.find(proof.signal_id);
      if (profile == input_profiles_.end() ||
          observed.value().descriptor.direction !=
              core::IoSignalDirection::kInput ||
          observed.value().descriptor.value_type !=
              core::IoSignalValueType::kBoolean ||
          observed.value().cursor.owner_epoch !=
              request.scope.request_owner_epoch ||
          observed.value().cursor.catalog_generation != generation_ ||
          proof.catalog_generation != generation_ ||
          proof.signal_descriptor_digest != digest.value() ||
          !proof.source_incarnation ||
          proof.source_incarnation != observed.value().installed_incarnation)
        return Reply::Err(SimulationError(
            "mock_simulation_invalid", "Resolved input source proof changed"));
      const bool mock = profile->second == "mock";
      if (mock && (mock_inputs_.find(proof.signal_id) == mock_inputs_.end() ||
                   observed.value().availability !=
                       core::IoSignalAvailability::kFresh ||
                   !observed.value().latest_sample))
        return Reply::Err(
            SimulationError("io_source_unavailable",
                            "Mock input has no current actual reading"));
      acknowledgement.signals.push_back(
          {row.signal,
           mock ? core::ProgramMockSimulationSignalDisposition::kApplied
                : core::ProgramMockSimulationSignalDisposition::kInactive});
      const auto [actual, inserted] =
          actual_rows.emplace(proof.signal_id, ActualRow{row, deadline, delay});
      if (!inserted && (actual->second.row.start_value != row.start_value ||
                        actual->second.row.change != row.change))
        return Reply::Err(
            SimulationError("mock_simulation_invalid",
                            "Aliases request conflicting signal values"));
    }

    // Allocate request, original result, receipt and all delayed entries before
    // any model mutation. A failed setup remains replayable without effects.
    auto success = Reply::Ok(std::move(acknowledgement));
    NodeSimulation prepared{
        request,
        Reply::Err(SimulationError("mock_simulation_incomplete",
                                   "Node start has not completed")),
        {}};
    prepared.changes.reserve(actual_rows.size());
    for (const auto &[signal, actual] : actual_rows) {
      if (!mock_inputs_.contains(signal) || !actual.row.change ||
          (!node_paused && actual.row.change->after_ms == 0U))
        continue;
      PendingSimulationChange change{
          signal, actual.row.change->value, std::monostate{}, {}};
      if (node_paused)
        change.due = PausedSimulationDelay{*actual.delay};
      else
        change.due = RunningSimulationDeadline{*actual.deadline};
      prepared.changes.push_back(std::move(change));
    }
    NodeSimulation *entry{};
    {
      std::lock_guard lock(mutex_);
      if (!active_)
        return Reply::Err(SimulationError("io_signal_service_unavailable",
                                          "I/O runtime is retired"));
      const auto old = node_simulations_.find(key);
      if (old != node_simulations_.end())
        for (auto &change : old->second.changes)
          CancelChangeLocked(change);
      entry = &node_simulations_.insert_or_assign(key, std::move(prepared))
                   .first->second;
    }
    const auto fail = [&](core::ErrorDTO error) {
      std::lock_guard lock(mutex_);
      entry->result = Reply::Err(std::move(error));
      for (auto &change : entry->changes)
        CancelChangeLocked(change);
      return *entry->result;
    };
    try {
      // Actual scheduler workers may dequeue before schedule returns. They
      // cannot cross these delivery barriers until the retained result commits.
      for (auto &change : entry->changes) {
        if (node_paused)
          break; // 136: even a zero-offset change waits for Resume
        std::optional<core::TimePoint> deadline;
        {
          std::lock_guard lock(mutex_);
          if (active_)
            if (const auto *running =
                    std::get_if<RunningSimulationDeadline>(&change.due))
              deadline = running->value;
        }
        if (!deadline)
          return fail(SimulationError("io_signal_service_unavailable",
                                      "I/O runtime is retired"));
        const auto handle = scheduler_->schedule(
            *deadline,
            [weak = weak_from_this(), key,
             index = request.scope.node_start_index] {
              if (const auto self = weak.lock())
                self->ApplyDueNodeSimulation(key, index);
            },
            core::SchedulerPriority::RuntimeSafety);
        bool active;
        {
          std::lock_guard lock(mutex_);
          change.handle = handle;
          active = active_;
        }
        if (!active)
          return fail(SimulationError("io_signal_service_unavailable",
                                      "I/O runtime is retired"));
      }
      const auto publish = [&](const core::IoSignalId &signal,
                               bool value) -> core::Result<void> {
        {
          std::lock_guard lock(mutex_);
          if (!active_)
            return core::Result<void>::Err(SimulationError(
                "io_signal_service_unavailable", "I/O runtime is retired"));
        }
        const auto result = PublishMockValueLocked(signal, value);
        if (!result.ok())
          return core::Result<void>::Err(result.error());
        if (result.value().publication_error)
          return core::Result<void>::Err(*result.value().publication_error);
        if (!result.value().published_cursor)
          return core::Result<void>::Err(
              SimulationError("io_signal_publication_unconfirmed",
                              "Mock start produced no acquisition"));
        return core::Result<void>::Ok();
      };
      for (const auto &[signal, actual] : actual_rows) {
        if (!mock_inputs_.contains(signal))
          continue;
        const auto published = publish(signal, actual.row.start_value);
        if (!published.ok())
          return fail(published.error());
      }
      // While running, zero offset is an actual second publication after the
      // initial writes. A new paused START keeps it as paused remaining zero.
      for (const auto &[signal, actual] : actual_rows) {
        if (node_paused)
          break;
        if (!mock_inputs_.contains(signal) || !actual.row.change ||
            actual.row.change->after_ms != 0U)
          continue;
        const auto published = publish(signal, actual.row.change->value);
        if (!published.ok())
          return fail(published.error());
      }
      std::lock_guard lock(mutex_);
      if (!active_) {
        entry->result = Reply::Err(SimulationError(
            "io_signal_service_unavailable", "I/O runtime is retired"));
        for (auto &change : entry->changes)
          CancelChangeLocked(change);
      } else {
        entry->result = std::move(success);
      }
      return *entry->result;
    } catch (...) {
      // Previously published model/acquisition facts are not rolled back.
      // A failed entry fences even a callback already dequeued before cancel.
      return fail(SimulationError("mock_simulation_start_failed",
                                  "Node simulation setup failed"));
    }
  }

  core::Result<void> SetProgramRunMockSimulationPaused(
      const core::ProgramRunMockSimulationPauseRequestDTO &request) override {
    auto delivery_locks = AcquireProviderDeliveries();
    if (request.run_revision == 0U)
      return core::Result<void>::Err(SimulationError(
          "mock_simulation_invalid", "Run revision is unnumbered"));
    std::optional<std::string> previous_node;
    for (const auto &node : request.held_node_instance_ids) {
      if (node.value().empty() || node.value().size() > 128U ||
          node.value().find('\0') != std::string::npos ||
          (previous_node && node.value() <= *previous_node))
        return core::Result<void>::Err(SimulationError(
            "mock_simulation_invalid", "Held-node mask is not canonical"));
      previous_node = node.value();
    }
    const auto key = RunKey(request.scope);
    auto accepted =
        request; // allocate exact accepted scope before timer effects
    std::unique_lock lock(mutex_);
    if (!active_)
      return core::Result<void>::Err(SimulationError(
          "io_signal_service_unavailable", "I/O runtime is retired"));
    const auto context = run_simulation_contexts_.find(key);
    if (context == run_simulation_contexts_.end()) {
      // First participation reports the actual Run state, including a Halt
      // accepted before this Run's first simulated node. Never infer a default.
      run_simulation_contexts_.emplace(key, std::move(accepted));
      return core::Result<void>::Ok();
    }
    if (context->second.scope != request.scope)
      return core::Result<void>::Err(SimulationError(
          "mock_simulation_stale", "Run clock authority scope changed"));
    if (request.run_revision < context->second.run_revision)
      return core::Result<void>::Ok();
    if (request.run_revision == context->second.run_revision) {
      if (request.paused != context->second.paused ||
          request.held_node_instance_ids !=
              context->second.held_node_instance_ids)
        return core::Result<void>::Err(
            SimulationError("mock_simulation_stale",
                            "Run revision has conflicting pause snapshot"));
      return core::Result<void>::Ok();
    }
    if (request.paused == context->second.paused &&
        request.held_node_instance_ids ==
            context->second.held_node_instance_ids) {
      context->second = std::move(accepted);
      return core::Result<void>::Ok(); // newer receipt does not refresh any
                                       // delay
    }

    struct ChangePlan final {
      PendingSimulationChange *change;
      NodeSimulationKey node;
      std::uint64_t index;
      core::TimePoint deadline{};
      core::Duration remaining{};
      bool pause;
      std::optional<core::ScheduleHandle> handle;
    };
    std::vector<ChangePlan> plan;
    for (auto &[node, entry] : node_simulations_) {
      if (std::get<0>(node) != key.first || std::get<1>(node) != key.second ||
          !entry.result || !entry.result->ok())
        continue;
      const bool node_paused =
          RunPausesNode(request, SimulationScope(entry).node.node_instance_id);
      for (auto &change : entry.changes) {
        if (node_paused) {
          if (const auto *running =
                  std::get_if<RunningSimulationDeadline>(&change.due))
            plan.push_back({&change,
                            node,
                            SimulationScope(entry).node_start_index,
                            running->value,
                            0,
                            true,
                            {}});
        } else if (const auto *paused =
                       std::get_if<PausedSimulationDelay>(&change.due)) {
          plan.push_back({&change,
                          node,
                          SimulationScope(entry).node_start_index,
                          0,
                          paused->remaining,
                          false,
                          {}});
        }
      }
    }
    if (plan.empty()) {
      context->second = std::move(accepted);
      return core::Result<void>::Ok();
    }
    const auto now = clock_->now();
    if (now < 0)
      return core::Result<void>::Err(
          SimulationError("mock_simulation_invalid", "Invalid host clock"));
    for (auto &item : plan) {
      if (item.pause) {
        item.remaining = item.deadline > now ? item.deadline - now : 0;
      } else {
        if (item.remaining < 0 ||
            now > std::numeric_limits<core::TimePoint>::max() - item.remaining)
          return core::Result<void>::Err(SimulationError(
              "mock_simulation_invalid", "Resumed deadline overflows"));
        item.deadline = now + item.remaining;
      }
    }
    lock.unlock();
    const auto cancel_prepared = [&] {
      for (auto &item : plan)
        if (item.handle)
          scheduler_->cancel(*item.handle);
    };
    try {
      // Prepare every replacement handle before changing accepted clock state.
      // Dequeued callbacks still see the paused due state if setup fails.
      for (auto &item : plan) {
        if (item.pause)
          continue;
        item.handle = scheduler_->schedule(
            item.deadline,
            [weak = weak_from_this(), node = item.node, index = item.index] {
              if (const auto self = weak.lock())
                self->ApplyDueNodeSimulation(node, index);
            },
            core::SchedulerPriority::RuntimeSafety);
      }
    } catch (...) {
      cancel_prepared();
      return core::Result<void>::Err(SimulationError(
          "mock_simulation_resume_not_applied",
          "Resume scheduling failed before clock/timer commit"));
    }
    lock.lock();
    if (!active_) {
      cancel_prepared();
      return core::Result<void>::Err(SimulationError(
          "io_signal_service_unavailable", "I/O runtime is retired"));
    }
    for (auto &item : plan) {
      if (item.pause) {
        CancelChangeLocked(*item.change);
        item.change->due = PausedSimulationDelay{item.remaining};
      } else {
        item.change->due = RunningSimulationDeadline{item.deadline};
        item.change->handle = item.handle;
      }
    }
    context->second = std::move(accepted);
    return core::Result<void>::Ok();
  }

  core::Result<void> EndProgramNodeMockSimulation(
      const core::ProgramNodeMockSimulationScopeDTO &scope,
      std::size_t maximum_node_entries) override {
    auto delivery_locks = AcquireProviderDeliveries();
    if (scope.node_start_index == 0U || maximum_node_entries == 0U)
      return core::Result<void>::Err(
          SimulationError("mock_simulation_invalid", "Invalid node end scope"));
    const auto key = SimulationKey(scope);
    NodeSimulation ended{scope, std::nullopt, {}};
    std::lock_guard lock(mutex_);
    if (!active_)
      return core::Result<void>::Err(SimulationError(
          "io_signal_service_unavailable", "I/O runtime is retired"));
    const auto previous = node_simulations_.find(key);
    if (previous != node_simulations_.end()) {
      const auto &old_scope = SimulationScope(previous->second);
      auto expected = old_scope;
      expected.node = scope.node;
      expected.node_start_index = scope.node_start_index;
      if (expected != scope)
        return core::Result<void>::Err(SimulationError(
            "mock_simulation_stale", "Node end authority scope changed"));
      if (scope.node_start_index < old_scope.node_start_index)
        return core::Result<void>::Ok(); // older END cannot cancel newer work
      if (scope.node_start_index == old_scope.node_start_index) {
        if (scope.node.node_attempt_id != old_scope.node.node_attempt_id)
          return core::Result<void>::Err(SimulationError(
              "mock_simulation_stale", "Node end attempt conflicts"));
        for (auto &change : previous->second.changes)
          CancelChangeLocked(change);
        // The original start result remains historical and inert on replay.
        return core::Result<void>::Ok();
      }
      if (scope.node.node_attempt_id == old_scope.node.node_attempt_id)
        return core::Result<void>::Err(
            SimulationError("mock_simulation_stale",
                            "A higher node end needs a fresh attempt"));
      for (auto &change : previous->second.changes)
        CancelChangeLocked(change);
    } else {
      const auto in_run =
          std::count_if(node_simulations_.begin(), node_simulations_.end(),
                        [&](const auto &row) {
                          return std::get<0>(row.first) == std::get<0>(key) &&
                                 std::get<1>(row.first) == std::get<1>(key);
                        });
      if (static_cast<std::size_t>(in_run) >= maximum_node_entries)
        return core::Result<void>::Err(SimulationError(
            "mock_simulation_invalid", "Captured node capacity exceeded"));
    }
    node_simulations_.insert_or_assign(key, std::move(ended));
    return core::Result<void>::Ok();
  }

  core::Result<void> EvaluateRules() override {
    return signals_->EvaluateRules();
  }
  core::Result<std::optional<core::TimePoint>> NextRuleExpiry() const override {
    return signals_->NextRuleExpiry();
  }

  core::Result<void> InstallProgramBlackboardSource(
      const core::IoProgramBlackboardSourceBindingDTO &binding,
      const std::optional<core::IoSourceIncarnation> &expected) override {
    std::lock_guard delivery_lock(mock_delivery_mutex_);
    return signals_->InstallProgramBlackboardSource(binding, expected);
  }
  core::Result<core::IoProgramBlackboardAcknowledgementDTO>
  PublishProgramBlackboard(
      const core::IoProgramBlackboardPublicationDTO &publication) override {
    std::lock_guard delivery_lock(mock_delivery_mutex_);
    return signals_->PublishProgramBlackboard(publication);
  }
  core::Result<core::IoProgramBlackboardAcknowledgementDTO>
  PublishProgramBlackboardBaseline(
      const core::IoProgramBlackboardSourceBindingDTO &binding,
      const std::optional<core::IoSourceIncarnation> &expected,
      const core::IoProgramBlackboardPublicationDTO &publication) override {
    std::lock_guard delivery_lock(mock_delivery_mutex_);
    return signals_->PublishProgramBlackboardBaseline(binding, expected,
                                                     publication);
  }
  core::Result<void> RetireProgramBlackboardSource(
      const core::IoProgramBlackboardSourceBindingDTO &binding) override {
    std::lock_guard delivery_lock(mock_delivery_mutex_);
    return signals_->RetireProgramBlackboardSource(binding);
  }

  core::Result<bool> WasProgramBlackboardScopeRetired(
      const core::IoProgramBlackboardScopeDTO &scope) const override {
    std::lock_guard delivery_lock(mock_delivery_mutex_);
    return signals_->WasProgramBlackboardScopeRetired(scope);
  }

  void Start() {
    {
      std::lock_guard lock(mutex_);
      Require(!started_, "I/O activation already started");
      started_ = true;
      active_ = true;
    }
    for (std::size_t i = 0; i < mock_echoes_.size(); ++i)
      mock_echoes_[i].output->SetModelObserver(
          [weak = weak_from_this(), i](bool value, core::TimePoint written_at) {
            if (auto self = weak.lock()) {
              self->PublishMockWrite(i, value, written_at);
              self->ScheduleMockEcho(i, value, written_at);
            }
          });
    // The system source has no DriverRecord: its levels outlive compositions
    // in the host store. Starting (also as an Apply candidate) only replays
    // the recorded `true` levels, serialized with every Publish; the
    // store's sink moves here when this composition is committed
    // (`AdoptCarriedObjectLostSignals`).
    if (system_provider_)
      host_carried_object_lost_signals().Seed(
          [this](const std::vector<core::IoSignalId> &lost) {
            for (const auto &signal : lost)
              if (std::find(system_signals_.begin(), system_signals_.end(),
                            signal) != system_signals_.end())
                Require(system_provider_->SetValue(signal, true).ok(),
                        "system signal initial level cannot be set");
            Require(system_provider_->PublishInitial().ok(),
                    "system signal initial acquisition was refused");
          });
    // Initial samples come only from actual DriverRecord Enable.
    if (ingress_) {
      const auto drained = ingress_->Drain(drain_budget_);
      Require(drained.ok() && !drained.value().blocked_error,
              "I/O initial ingress admission failed");
    }
    RequestRuleEvidenceProcessing();
  }

  /// This runtime's composition is committed (terminal publication proof):
  /// the host store delivers every later level here, keeps only the rows of
  /// the grippers it holds, and this runtime takes the store's current levels
  /// (a level published while it was a candidate included), all in one
  /// serialized step. Idempotent.
  void AdoptCarriedObjectLostSignals() {
    host_carried_object_lost_signals().InstallSink(
        [weak = weak_from_this()](const core::IoSignalId &signal, bool value) {
          const auto self = weak.lock();
          return self ? self->PublishSystemSignal(signal, value)
                      : std::optional<core::ErrorDTO>(core::ErrorDTO{
                            "io_signal_service_unavailable",
                            "System signal source is retired", {}});
        },
        system_signals_,
        [this](const std::vector<core::IoSignalId> &lost) {
          if (!system_provider_)
            return;
          for (const auto &signal : system_signals_)
            Require(system_provider_
                        ->SetValue(signal, std::find(lost.begin(), lost.end(),
                                                     signal) != lost.end())
                        .ok(),
                    "system signal level cannot be set");
        });
  }

  /// One level of a compiled system signal of this runtime. The ingress wake
  /// drains it and runs rule evidence on the host scheduler.
  std::optional<core::ErrorDTO> PublishSystemSignal(const core::IoSignalId &signal,
                                                    bool value) {
    {
      std::lock_guard lock(mutex_);
      if (!active_)
        return core::ErrorDTO{"io_signal_service_unavailable",
                              "System signal source is retired", {}};
    }
    if (!system_provider_ ||
        std::find(system_signals_.begin(), system_signals_.end(), signal) ==
            system_signals_.end())
      return core::ErrorDTO{"io_signal_unbound",
                            "No compiled system signal has this identity",
                            {{"signal_name", signal.name}}};
    const auto changed = system_provider_->SetValue(signal, value);
    if (!changed.ok())
      return changed.error();
    return changed.value().publication_error;
  }

  /// I/O update sink: one acquisition of an adapter family's
  /// component, stamped at receipt by the family. Updates of a component
  /// whose signals this catalog doesn't hold are not this runtime's.
  void PublishAdapterUpdates(const std::string &component_id,
                             const std::vector<core::IoSignalUpdateDTO> &updates,
                             const std::int64_t received_at_ns) {
    // One acquisition per source of the updated signals; an update outside
    // this catalog is not this runtime's.
    std::map<core::InterruptSourceId, core::IoSourceAcquisitionDTO> acquisitions;
    {
      std::lock_guard lock(mutex_);
      if (!active_) return;
      for (const auto &update : updates) {
        if (update.signal_id.controllable_component_id.value() != component_id) continue;
        const auto signal = adapter_signal_sources_.find(update.signal_id);
        if (signal == adapter_signal_sources_.end()) continue;
        auto &source = adapter_sources_.at(signal->second);
        auto [acquisition, added] = acquisitions.try_emplace(
            source.source_id, core::IoSourceAcquisitionDTO{source.source_id, source.incarnation,
                                                           0U, received_at_ns, {}});
        if (added) acquisition->second.source_sequence = ++source.sequence;
        acquisition->second.updates.push_back(update);
      }
    }
    // A refused frame records its own loss barrier in the ingress.
    for (const auto &entry : acquisitions)
      static_cast<void>(adapter_ingress_->TryEnqueue(entry.second));
  }

  core::Result<MockIoInputSetResult>
  SetMockInput(const core::IoSignalId &signal,
               const core::IoSourceIncarnation &expected, bool value) {
    using Reply = core::Result<MockIoInputSetResult>;
    auto delivery_locks = AcquireProviderDeliveries();
    {
      std::lock_guard lock(mutex_);
      if (!active_)
        return Reply::Err(
            {"io_signal_service_unavailable", "Mock source is retired", {}});
    }
    const auto input = mock_inputs_.find(signal);
    if (input == mock_inputs_.end())
      return Reply::Err({"io_mock_input_required",
                         "Signal is not an installed Mock input",
                         {}});
    const auto before = signals_->ReadSignal(signal);
    if (!before.ok())
      return Reply::Err(before.error());
    if (before.value().installed_incarnation != expected)
      return Reply::Err(
          {"io_signal_catalog_stale", "Mock source incarnation changed", {}});
    return PublishMockValueLocked(signal, value);
  }

private:
  static void Require(bool condition, const std::string &message) {
    if (!condition)
      throw std::runtime_error(message);
  }
  core::Result<MockIoInputSetResult>
  PublishMockValueLocked(const core::IoSignalId &signal, bool value) {
    using Reply = core::Result<MockIoInputSetResult>;
    const auto input = mock_inputs_.find(signal);
    Require(input != mock_inputs_.end(),
            "Mock publication has no installed input");
    const auto before = signals_->ReadSignal(signal);
    if (!before.ok())
      return Reply::Err(before.error());
    const auto changed = input->second.driver->SetMockValueLocked(signal, value);
    if (!changed.ok())
      return Reply::Err(changed.error());
    auto publication_error = changed.value().publication_error;
    const auto drained = ingress_->Drain(drain_budget_);
    RequestRuleEvidenceProcessing();
    if (!drained.ok() && !publication_error)
      publication_error = drained.error();
    if (drained.ok() && drained.value().blocked_error && !publication_error)
      publication_error = drained.value().blocked_error;
    const auto observed = signals_->ReadSignal(signal);
    if (!observed.ok())
      return Reply::Err(observed.error());
    std::optional<core::IoSourceCursorDTO> published_cursor;
    if (changed.value().enqueued_source_sequence) {
      // Read the actual retained acquisition record, not the current source
      // cursor: later loss records may already follow this admitted sample.
      const auto records = signals_->ReadRecords(before.value().cursor,
                                                 input->second.record_capacity);
      if (records.ok()) {
        for (const auto &record : records.value().records) {
          if (!before.value().installed_incarnation ||
              record.incarnation != *before.value().installed_incarnation ||
              !record.acquisition ||
              record.acquisition->incarnation != record.incarnation ||
              record.acquisition->source_id !=
                  observed.value().descriptor.source_id ||
              record.acquisition->source_sequence !=
                  *changed.value().enqueued_source_sequence)
            continue;
          const auto update = std::find_if(
              record.acquisition->updates.begin(),
              record.acquisition->updates.end(),
              [&](const auto &row) { return row.signal_id == signal; });
          if (update != record.acquisition->updates.end() &&
              update->validity == core::IoSampleValidity::kValid &&
              update->value == value) {
            published_cursor = record.cursor;
            break;
          }
        }
      }
      if (!published_cursor && !publication_error)
        publication_error = core::ErrorDTO{"io_signal_publication_unconfirmed",
                                           "Mock acquisition was not admitted",
                                           {}};
    }
    return Reply::Ok({changed.value().model_value, std::move(published_cursor),
                      observed.value(), std::move(publication_error)});
  }

public:
  void Shutdown() noexcept {
    {
      std::lock_guard lock(mutex_);
      active_ = false;
      rule_evidence_processor_ = {};
      try {
        if (rule_evidence_handle_)
          scheduler_->cancel(*rule_evidence_handle_);
        rule_evidence_handle_.reset();
        rule_evidence_deadline_.reset();
        if (drain_handle_)
          scheduler_->cancel(*drain_handle_);
        drain_handle_.reset();
        if (sample_expiry_handle_)
          scheduler_->cancel(*sample_expiry_handle_);
        sample_expiry_handle_.reset();
        sample_expiry_deadline_.reset();
        // Keep replay results stable until in-flight starts have crossed the
        // same provider barriers. No new callback may publish while inactive.
        for (auto &[key, entry] : node_simulations_)
          for (auto &change : entry.changes)
            CancelChangeLocked(change);
        for (auto &channel : mock_echoes_)
          for (auto &target : channel.targets) {
            if (target.handle)
              scheduler_->cancel(*target.handle);
            target.handle.reset();
          }
      } catch (...) {
      } // Every future callback still observes active_ == false.
    }
    try {
      // No allocation during retirement. With active_ already fenced, waiting
      // through each actual barrier drains every previously selected script
      // publication; no runtime timer mutex is held while waiting for a driver.
      for (const auto &driver : drivers_) {
        auto delivery = driver->AcquireDelivery();
      }
      // Borrowed actual records remain with their component owner. Retiring
      // an activation must never Shutdown a driver retained by its successor.
      if (system_provider_)
        system_provider_->Shutdown();
    } catch (...) {
    } // Clock/ingress remain pinned until callback teardown completes.
  }

private:
  struct MockEchoTarget final {
    core::IoSignalId signal;
    core::Duration delay;
    std::uint64_t serial;
    std::optional<core::ScheduleHandle> handle;
  };
  struct MockEchoChannel final {
    std::shared_ptr<MockBooleanOutputProvider> output;
    std::vector<MockEchoTarget> targets;
    /// The channel's output signals, published back on each accepted write.
    std::vector<core::IoSignalId> written{};
  };
  MockEchoChannel &MockChannel(std::shared_ptr<MockBooleanOutputProvider> output) {
    const auto found = std::find_if(mock_echoes_.begin(), mock_echoes_.end(),
        [&](const auto &channel) { return channel.output == output; });
    if (found != mock_echoes_.end()) return *found;
    return mock_echoes_.emplace_back(MockEchoChannel{std::move(output), {}, {}});
  }
  /// One accepted Mock write: its value is the sample of every output of the
  /// channel, source `manyforge_write`. Called under the output's model lock.
  void PublishMockWrite(std::size_t channel, bool value, core::TimePoint written_at) {
    std::map<core::InterruptSourceId, core::IoSourceAcquisitionDTO> acquisitions;
    {
      std::lock_guard lock(mutex_);
      if (!active_ || channel >= mock_echoes_.size()) return;
      for (const auto &signal : mock_echoes_[channel].written) {
        auto &source = adapter_sources_.at(adapter_signal_sources_.at(signal));
        auto [acquisition, added] = acquisitions.try_emplace(
            source.source_id, core::IoSourceAcquisitionDTO{source.source_id, source.incarnation,
                                                           0U, written_at, {}});
        if (added) acquisition->second.source_sequence = ++source.sequence;
        acquisition->second.updates.push_back({signal, core::IoSampleValidity::kValid, value,
                                               std::nullopt,
                                               core::IoSignalSampleSource::kManyforgeWrite});
      }
    }
    for (const auto &entry : acquisitions)
      static_cast<void>(adapter_ingress_->TryEnqueue(entry.second));
  }
  void ScheduleMockEcho(std::size_t channel, bool value,
                        core::TimePoint written_at) {
    std::lock_guard lock(mutex_);
    if (!active_ || channel >= mock_echoes_.size())
      return;
    auto &targets = mock_echoes_[channel].targets;
    for (std::size_t i = 0; i < targets.size(); ++i) {
      auto &target = targets[i];
      if (target.handle)
        scheduler_->cancel(*target.handle);
      target.handle.reset();
      if (target.serial == std::numeric_limits<std::uint64_t>::max() ||
          written_at > std::numeric_limits<core::TimePoint>::max() - target.delay)
        continue;
      const auto serial = ++target.serial;
      target.handle = scheduler_->schedule(
          written_at + target.delay,
          [weak = weak_from_this(), channel, i, serial, value] {
            if (auto self = weak.lock())
              self->ApplyMockEcho(channel, i, serial, value);
          }, core::SchedulerPriority::Monitoring);
    }
  }
  void ApplyMockEcho(std::size_t channel, std::size_t index,
                     std::uint64_t serial, bool value) {
    auto delivery_locks = AcquireProviderDeliveries();
    // Channels are immutable after Start. This is the same output mutex held
    // across acceptance and ScheduleMockEcho, so a newer accepted write cannot
    // overtake this serial check and then be overwritten by this publication.
    if (channel >= mock_echoes_.size())
      return;
    auto output_delivery = mock_echoes_[channel].output->AcquireModelDelivery();
    std::optional<core::IoSignalId> signal;
    {
      std::lock_guard lock(mutex_);
      if (!active_ || channel >= mock_echoes_.size() ||
          index >= mock_echoes_[channel].targets.size())
        return;
      auto &target = mock_echoes_[channel].targets[index];
      if (target.serial != serial || !target.handle)
        return;
      target.handle.reset();
      signal = target.signal;
    }
    static_cast<void>(PublishMockValueLocked(*signal, value));
  }
  class IngressWake final : public core::IIoSignalIngress {
  public:
    IngressWake(std::shared_ptr<core::IoSignalIngress> ingress,
                std::weak_ptr<HostIoSignalRuntime> runtime)
        : ingress_(std::move(ingress)), runtime_(std::move(runtime)) {}
    bool
    TryEnqueue(const core::IoSourceAcquisitionDTO &value) noexcept override {
      const bool accepted = ingress_->TryEnqueue(value);
      Wake();
      return accepted;
    }
    void NotifyLoss(
        const core::IoIngressLossNotificationDTO &value) noexcept override {
      ingress_->NotifyLoss(value);
      Wake();
    }

  private:
    void Wake() noexcept {
      if (auto runtime = runtime_.lock()) {
        try {
          std::lock_guard lock(runtime->mutex_);
          runtime->ScheduleDrainLocked();
        } catch (...) {
        } // Ingress retains its original sample/loss; no restamping/retry.
      }
    }
    std::shared_ptr<core::IoSignalIngress> ingress_;
    std::weak_ptr<HostIoSignalRuntime> runtime_;
  };
  struct ProviderDeliveryLocks final {
    std::vector<std::unique_lock<std::mutex>> drivers;
  };
  ProviderDeliveryLocks AcquireProviderDeliveries() const {
    ProviderDeliveryLocks result;
    result.drivers.reserve(drivers_.size());
    for (const auto &driver : drivers_)
      result.drivers.push_back(driver->AcquireDelivery());
    return result;
  }
  struct MockInput final {
    std::size_t record_capacity;
    std::shared_ptr<IoProviderDriverRecord> driver{};
  };
  using SimulationReply =
      core::Result<core::ProgramNodeMockSimulationAcknowledgementDTO>;
  using NodeSimulationKey = std::tuple<std::string, std::string, std::string>;
  struct RunningSimulationDeadline final {
    core::TimePoint value;
  };
  struct PausedSimulationDelay final {
    core::Duration remaining;
  };
  struct PendingSimulationChange final {
    core::IoSignalId signal;
    bool value;
    // Exactly one real clock state, or none after completion/END/source
    // retirement.
    std::variant<std::monostate, RunningSimulationDeadline,
                 PausedSimulationDelay>
        due;
    std::optional<core::ScheduleHandle> handle;
  };
  struct NodeSimulation final {
    // An END may arrive before an uncertain START. Retain its actual scope,
    // without inventing rows, a start anchor or a historical start result.
    std::variant<core::ProgramNodeMockSimulationRequestDTO,
                 core::ProgramNodeMockSimulationScopeDTO>
        origin;
    std::optional<SimulationReply> result;
    std::vector<PendingSimulationChange> changes;
  };
  static const core::ProgramNodeMockSimulationScopeDTO &
  SimulationScope(const NodeSimulation &entry) {
    return std::visit(
        [](const auto &origin)
            -> const core::ProgramNodeMockSimulationScopeDTO & {
          if constexpr (std::is_same_v<
                            std::decay_t<decltype(origin)>,
                            core::ProgramNodeMockSimulationRequestDTO>)
            return origin.scope;
          else
            return origin;
        },
        entry.origin);
  }
  using RunSimulationKey = std::pair<std::string, std::string>;
  static RunSimulationKey
  RunKey(const core::ProgramRunMockSimulationScopeDTO &scope) {
    return {scope.program_runner_owner_epoch.value(),
            scope.program_run_id.value()};
  }
  static core::ProgramRunMockSimulationScopeDTO
  RunScope(const core::ProgramNodeMockSimulationScopeDTO &scope) {
    return {scope.request_owner_epoch, scope.program_binding_id,
            scope.program_runner_owner_epoch, scope.claim_generation,
            scope.program_run_id};
  }
  static bool
  RunPausesNode(const core::ProgramRunMockSimulationPauseRequestDTO &snapshot,
                const core::ProgramNodeInstanceId &node) {
    return snapshot.paused ||
           std::binary_search(snapshot.held_node_instance_ids.begin(),
                              snapshot.held_node_instance_ids.end(), node,
                              [](const auto &left, const auto &right) {
                                return left.value() < right.value();
                              });
  }
  static NodeSimulationKey
  SimulationKey(const core::ProgramNodeMockSimulationScopeDTO &scope) {
    return {scope.program_runner_owner_epoch.value(),
            scope.program_run_id.value(), scope.node.node_instance_id.value()};
  }
  static core::ErrorDTO SimulationError(const std::string &reason,
                                        const std::string &message) {
    return {reason, message, {{"reason", reason}}};
  }
  void CancelChangeLocked(PendingSimulationChange &change) {
    if (change.handle)
      scheduler_->cancel(*change.handle);
    change.handle.reset();
    change.due = std::monostate{};
  }
  void ApplyDueNodeSimulation(const NodeSimulationKey &key,
                              std::uint64_t index) {
    // Actual SimpleScheduler and ManualScheduler queue callbacks. A due worker
    // may start before schedule() returns, but waits for these SAME barriers.
    auto delivery_locks = AcquireProviderDeliveries();
    for (;;) {
      std::optional<std::pair<core::IoSignalId, bool>> publication;
      {
        std::lock_guard lock(mutex_);
        const auto entry = node_simulations_.find(key);
        if (!active_ || entry == node_simulations_.end() ||
            SimulationScope(entry->second).node_start_index != index ||
            !entry->second.result || !entry->second.result->ok())
          return;
        const auto context = run_simulation_contexts_.find(
            RunKey(RunScope(SimulationScope(entry->second))));
        if (context == run_simulation_contexts_.end() ||
            RunPausesNode(
                context->second,
                SimulationScope(entry->second).node.node_instance_id) ||
            context->second.scope != RunScope(SimulationScope(entry->second)))
          return;
        const auto now = clock_->now();
        const auto due = std::find_if(
            entry->second.changes.begin(), entry->second.changes.end(),
            [&](const auto &change) {
              const auto *running =
                  std::get_if<RunningSimulationDeadline>(&change.due);
              return running && running->value <= now;
            });
        if (due == entry->second.changes.end())
          return;
        publication.emplace(due->signal, due->value);
        CancelChangeLocked(*due);
      }
      // No runtime mutex during ingress delivery: its wake reenters mutex_.
      // Later source failure remains actual provider/ingress evidence, not a
      // rewritten start acknowledgement or a blind retry.
      static_cast<void>(
          PublishMockValueLocked(publication->first, publication->second));
    }
  }
  HostIoSignalRuntime(std::shared_ptr<core::IClock> clock,
                      std::shared_ptr<core::IScheduler> scheduler,
                      std::shared_ptr<core::IoSignalSubsystem> signals,
                      std::size_t drain_budget, std::uint64_t generation)
      : clock_(std::move(clock)), scheduler_(std::move(scheduler)),
        signals_(std::move(signals)), drain_budget_(drain_budget),
        generation_(generation) {}

  void ScheduleDrainLocked() {
    if (!active_ || drain_handle_)
      return;
    drain_handle_ = scheduler_->schedule(
        clock_->now(),
        [weak = weak_from_this()] {
          if (auto self = weak.lock()) {
            {
              std::lock_guard lock(self->mutex_);
              self->drain_handle_.reset();
              if (!self->active_)
                return;
            }
            const auto drained = self->ingress_->Drain(self->drain_budget_);
            if (!drained.ok() || drained.value().blocked_error)
              std::cerr << "native I/O ingress remains blocked by its original "
                           "admission error\n";
            // Drain has returned: no ingress/provider/runtime lock is held.
            self->RequestRuleEvidenceProcessing();
          }
        },
        core::SchedulerPriority::RuntimeSafety);
  }
  void ScheduleRuleEvidenceLocked(core::TimePoint deadline) {
    if (!active_ || !rule_evidence_processor_)
      return;
    if (rule_evidence_handle_ && rule_evidence_deadline_ &&
        *rule_evidence_deadline_ <= deadline)
      return;
    if (rule_evidence_handle_) scheduler_->cancel(*rule_evidence_handle_);
    rule_evidence_deadline_ = deadline;
    rule_evidence_handle_ = scheduler_->schedule(
        deadline, [weak = weak_from_this(), deadline] {
          const auto self = weak.lock();
          if (!self) return;
          std::function<core::Result<bool>()> processor;
          {
            std::lock_guard lock(self->mutex_);
            if (self->rule_evidence_deadline_ != deadline) return;
            self->rule_evidence_handle_.reset();
            self->rule_evidence_deadline_.reset();
            if (!self->active_) return;
            processor = self->rule_evidence_processor_;
          }
          if (!processor) return;
          // The owner lease/callback runs with no provider or runtime locks.
          const auto processed = processor();
          if (!processed.ok()) {
            std::cerr << "native I/O rule evidence processing failed: "
                      << processed.error().code << " "
                      << processed.error().message << '\n';
            return;
          }
          if (!processed.value()) return; // actual owner was retired/unavailable
          // Query THIS actual service's rules. A staged candidate without
          // registrations cannot create a duplicate predecessor expiry timer.
          const auto next = self->signals_->NextRuleExpiry();
          if (!next.ok()) {
            std::cerr << "native I/O rule expiry is unavailable: "
                      << next.error().code << " " << next.error().message << '\n';
            return;
          }
          if (next.value()) {
            std::lock_guard lock(self->mutex_);
            self->ScheduleRuleEvidenceLocked(*next.value());
          }
        }, core::SchedulerPriority::RuntimeSafety);
  }
  // A fresh sample turns stale with no new acquisition: refresh the published
  // samples at that moment so the event stream sends the change.
  // One deadline at a time, the earliest; no periodic re-read.
  void ScheduleSampleExpiryLocked() {
    if (!active_)
      return;
    const auto next = signals_->NextSampleExpiry();
    if (!next || (sample_expiry_handle_ && sample_expiry_deadline_ &&
                  *sample_expiry_deadline_ <= *next))
      return;
    if (sample_expiry_handle_) scheduler_->cancel(*sample_expiry_handle_);
    sample_expiry_deadline_ = *next;
    sample_expiry_handle_ = scheduler_->schedule(
        *next, [weak = weak_from_this(), deadline = *next] {
          const auto self = weak.lock();
          if (!self) return;
          std::lock_guard lock(self->mutex_);
          if (self->sample_expiry_deadline_ != deadline) return;
          self->sample_expiry_handle_.reset();
          self->sample_expiry_deadline_.reset();
          if (!self->active_) return;
          self->signals_->RefreshSamples();
          self->ScheduleSampleExpiryLocked();
        }, core::SchedulerPriority::Monitoring);
  }
  std::shared_ptr<core::IClock> clock_;
  std::shared_ptr<core::IScheduler> scheduler_;
  std::shared_ptr<core::IoSignalSubsystem> signals_;
  std::shared_ptr<core::IoSignalIngress> ingress_;
  std::size_t drain_budget_;
  std::uint64_t generation_;
  std::map<core::InterruptSourceId, core::IoSourceIncarnation> incarnations_;
  DriverInputInstallations driver_inputs_;
  std::vector<std::shared_ptr<IoProviderDriverRecord>> drivers_;
  std::map<core::IoSignalId, MockInput> mock_inputs_;
  std::vector<core::IoSignalId> system_signals_;
  std::unique_ptr<MockBooleanInputProvider> system_provider_;
  struct AdapterSource final {
    core::InterruptSourceId source_id;
    core::IoSourceIncarnation incarnation;
    std::uint64_t sequence{};
  };
  /// Adapter-fed sources, the source of each of their signals, and the
  /// ingress they publish to.
  std::map<core::InterruptSourceId, AdapterSource> adapter_sources_;
  std::map<core::IoSignalId, core::InterruptSourceId> adapter_signal_sources_;
  std::shared_ptr<core::IIoSignalIngress> adapter_ingress_;
  std::vector<MockEchoChannel> mock_echoes_;
  mutable std::mutex mock_delivery_mutex_;
  mutable std::mutex mutex_;
  bool active_{};
  bool started_{};
  std::optional<core::ScheduleHandle> drain_handle_;
  std::function<core::Result<bool>()> rule_evidence_processor_;
  InputLifecycleProcessor input_lifecycle_processor_;
  std::optional<core::ScheduleHandle> rule_evidence_handle_;
  std::optional<core::TimePoint> rule_evidence_deadline_;
  std::optional<core::ScheduleHandle> sample_expiry_handle_;
  std::optional<core::TimePoint> sample_expiry_deadline_;
  std::map<core::IoSignalId, std::string> input_profiles_;
  std::map<NodeSimulationKey, NodeSimulation> node_simulations_;
  // Actual accepted Run clock notification, never inferred from missing packets
  // or controller/operation/node-held states. Runner revision orders only.
  std::map<RunSimulationKey, core::ProgramRunMockSimulationPauseRequestDTO>
      run_simulation_contexts_;
};

} // namespace manyforge::roboplan_backend::host_detail
