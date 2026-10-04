#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <manyforge_core/component_lifecycle_coordinator.hpp>
#include <manyforge_core/controller_lifecycle_evaluator.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../src/component_lifecycle_journal_codec_internal.hpp"
#include "cclc_test_admission_port.hpp"

namespace manyforge::core {
namespace {

class ScopedTemporaryDirectory final {
 public:
  ScopedTemporaryDirectory() {
    static std::atomic<std::uint64_t> next{1U};
    path_ = std::filesystem::temp_directory_path() /
            ("manyforge-route-coordinator-test-" +
             std::to_string(static_cast<std::uint64_t>(::getpid())) + "-" +
             std::to_string(next.fetch_add(1U)));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("failed to create route coordinator test root");
    }
  }
  ~ScopedTemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

class WaitableManualScheduler final : public IScheduler {
 public:
  ScheduleHandle schedule(TimePoint deadline, std::function<void()> callback,
                          SchedulerPriority priority) override {
    const auto handle = scheduler_.schedule(deadline, std::move(callback), priority);
    {
      std::lock_guard lock(mutex_);
      ++schedule_count_;
    }
    scheduled_.notify_all();
    return handle;
  }

  void cancel(ScheduleHandle handle) override { scheduler_.cancel(handle); }

  void drain(SchedulerPriority priority) override { scheduler_.drain(priority); }

  bool drain_for(SchedulerPriority priority, std::chrono::nanoseconds timeout) override {
    return scheduler_.drain_for(priority, timeout);
  }

  [[nodiscard]] bool WaitForScheduleCount(std::size_t expected, std::chrono::nanoseconds timeout) {
    std::unique_lock lock(mutex_);
    return scheduled_.wait_for(lock, timeout, [&] { return schedule_count_ >= expected; });
  }

  void AdvanceTo(TimePoint time) { scheduler_.advance_to(time); }

 private:
  ManualScheduler scheduler_;
  std::mutex mutex_;
  std::condition_variable scheduled_;
  std::size_t schedule_count_{};
};

[[nodiscard]] std::string Sha(char value) {
  return std::string("sha256:") + std::string(64U, value);
}

[[nodiscard]] ControllerTopologyDTO Topology() {
  const ResourceInterfaceClaimDTO position{ResourceRoleId("arm_joints"), "position"};
  return {ControllerTopologyId("topology.arm"),
          TopologyRevision(Sha('1')),
          {{ControllerRoleId("hardware"), {}, {position}, {}}},
          {},
          {{ActivationSetId("activation.direct"), {ControllerRoleId("hardware")}, {}}},
          {{ControlRouteId("route.direct"),
            ControlInterfaceContractId("contract.position"),
            ControlInterfaceRevision(Sha('2')),
            {ControllerRoleId("hardware")},
            {ActivationSetId("activation.direct")}}}};
}

[[nodiscard]] DesiredControlConfigurationDTO Desired() {
  return {ControlDomainId("domain.arm"),
          ControlDomainRevision(Sha('3')),
          ControllerTopologyId("topology.arm"),
          TopologyRevision(Sha('1')),
          ActivationSetId("activation.direct"),
          {{ControlBindingId("binding.arm"), ControlBindingRevision(Sha('4')),
            ControlRouteId("route.direct")}}};
}

[[nodiscard]] ControllerLifecycleExpectedDomainDTO ExpectedDomain() {
  const auto desired = Desired();
  return {desired.control_domain_id,
          desired.control_domain_revision,
          desired.controller_topology_id,
          desired.topology_revision,
          ProductId("product.control.arm"),
          ProductContractId("manyforge.control-domain-control.v2"),
          ProductContractRevision(Sha('6')),
          ContentDigest(Sha('7')),
          ContentDigest(Sha('8')),
          RuntimeUnitId("runtime.controller-manager"),
          RuntimeEpoch("runtime-epoch.1"),
          Topology(),
          desired,
          {{ControlBindingId("binding.arm"),
            ControlBindingRevision(Sha('4')),
            ControlInterfaceContractId("contract.position"),
            ControlInterfaceRevision(Sha('2')),
            {{ResourceId("joint.arm"), "position"}},
            {{ResourceId("joint.arm"), "position"}}}},
          {{ControllerRoleId("hardware"),
            ControllerImplementationId("implementation.hardware"),
            ControllerImplementationRevision(Sha('9')),
            "controller_interface::ControllerInterface",
            ContentDigest(Sha('a')),
            {},
            {},
            {},
            {}}},
          {{ControlBindingId("binding.arm"), ControlBindingRevision(Sha('4')),
            ControlRouteId("route.direct"), EndpointRoleId("endpoint.command"), "/cell/arm/command",
            true, false}}};
}

[[nodiscard]] ConfigurationFenceScopeDTO Scope() {
  const auto desired = Desired();
  return {{ControllableComponentId("component.arm")},
          {{desired.control_domain_id, desired.control_domain_revision,
            desired.control_domain_revision}},
          {{ControlBindingId("binding.arm"), ControlBindingRevision(Sha('4')),
            ControlBindingRevision(Sha('4'))}},
          {ResourceId("joint.arm")}};
}

[[nodiscard]] RouteSelectionCurrentSnapshotDTO CurrentSnapshot() {
  return {RetainedBundleId("bundle.1"),
          GraphRevision("graph.1"),
          {{ControllableComponentId("component.arm"), ComponentLifecycleGeneration(4U),
            ComponentLifecycleStateDTO::kPrepared}},
          std::nullopt,
          ExpectedDomain(),
          Scope()};
}

[[nodiscard]] RouteSelectionIntentDTO Intent(
    RequestOwnerEpoch owner = RequestOwnerEpoch("cclc-owner.1")) {
  return {{RequestId("request.route.1"), std::move(owner), "test", std::nullopt},
          RetainedBundleId("bundle.1"),
          GraphRevision("graph.1"),
          Desired(),
          {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt},
          CurrentSnapshot().component_expectations,
          {RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified, std::nullopt},
          AdmissionGeneration(1U)};
}

[[nodiscard]] ComponentLifecycleCoordinatorConfig Config(
    RequestOwnerEpoch owner = RequestOwnerEpoch("cclc-owner.1")) {
  return {std::move(owner), 8U, 2U, 1'000, 1'000'000};
}

[[nodiscard]] RouteConfigurationFenceAdmissionSnapshotDTO FenceAdmissionSnapshot(
    const RouteConfigurationFenceAdmissionQueryDTO& query) {
  return {query,
          {RuntimeUnitId("runtime.kernel"), RuntimeEpoch("kernel-epoch.1"),
           RequestOwnerEpoch("kernel-owner.1"), ContentDigest(Sha('b')),
           ActiveBundlePublicationGeneration(3U)},
          {query.expected_retained_bundle_id,
           query.expected_graph_revision,
           {SceneId(Sha('c')), 2U, ContentDigest(Sha('d'))}},
          ActiveBundlePublicationGeneration(3U),
          ContentDigest(Sha('e')),
          11U,
          ConfigurationFenceGeneration(4U),
          query.expected_active_control_configuration,
          0U,
          0U,
          ContentDigest(Sha('f'))};
}

class UnusedComponentSnapshotSource final : public ICCLCCurrentComponentSnapshotSource {
 public:
  [[nodiscard]] Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponentSnapshot(
      const ControllableComponentId&) const override {
    return Result<ComponentLifecycleCurrentSnapshotDTO>::Err(
        ErrorDTO{"unused", "component lifecycle path is unused", {}});
  }
};

class UnusedCatalogSource final : public ICCLCLifecyclePlanCatalogSource {
 public:
  [[nodiscard]] Result<BrandLifecyclePlanCatalogDTO> LoadExactLifecyclePlanCatalog(
      const ProviderManifestId&, const ProviderManifestRevision&,
      const ContentDigest&) const override {
    return Result<BrandLifecyclePlanCatalogDTO>::Err(
        ErrorDTO{"unused", "component lifecycle path is unused", {}});
  }
};

class UnusedBrandResolver final : public ICCLCBrandLifecyclePortResolver {
 public:
  [[nodiscard]] Result<std::shared_ptr<IBrandLifecyclePort>> ResolveExactLifecyclePort(
      const ResolvedLifecycleProviderOccurrenceDTO&) const override {
    return Result<std::shared_ptr<IBrandLifecyclePort>>::Err(
        ErrorDTO{"unused", "component lifecycle path is unused", {}});
  }
};

class RecoveryAwareAdmissionPort final : public test::FakeCellAdmissionPort {
 public:
  std::optional<ActiveControlConfigurationDTO> recovery_predecessor;
  std::optional<CellAdmissionClaimIntentDTO> recovery_claim;
  unsigned recovery_acquisitions{};
  AuthoringAdmissionDTO GetAuthoringAdmission() const override {
    return {!recovery_predecessor, AdmissionGeneration(1U)};
  }
  Result<CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent) override {
    if (recovery_predecessor)
      return Result<CellAdmissionClaimDTO>::Err(
          ErrorDTO{"authoring_not_ready", "replacement has no ordinary readiness", {}});
    return test::FakeCellAdmissionPort::AcquireCellAdmissionClaim(intent);
  }
  Result<CellAdmissionClaimDTO> AcquireRouteRecoveryCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent,
      const ActiveControlConfigurationDTO& predecessor) override {
    if (recovery_predecessor != predecessor)
      return Result<CellAdmissionClaimDTO>::Err(
          ErrorDTO{"authoring_not_ready", "foreign recovery predecessor", {}});
    ++recovery_acquisitions;
    recovery_claim = intent;
    return test::FakeCellAdmissionPort::AcquireCellAdmissionClaim(intent);
  }
};

class FakeRouteSnapshotSource final : public ICCLCRouteSelectionCurrentSnapshotSource {
 public:
  void SetComponentState(ComponentLifecycleStateDTO state) {
    std::lock_guard lock(mutex_);
    component_state_ = state;
  }
  void SetActiveConfiguration(std::optional<ActiveControlConfigurationDTO> configuration) {
    std::lock_guard lock(mutex_);
    active_configuration_ = std::move(configuration);
  }

  void SetRuntimeEpoch(RuntimeEpoch runtime_epoch) {
    std::lock_guard lock(mutex_);
    runtime_epoch_ = std::move(runtime_epoch);
  }

  [[nodiscard]] Result<RouteSelectionCurrentSnapshot> ReadCurrentRouteSelectionSnapshot(
      const RetainedBundleId& bundle_id, const GraphRevision& graph_revision,
      const DesiredControlConfigurationDTO& desired) const override {
    ++read_count_;
    auto snapshot = CurrentSnapshot();
    {
      std::lock_guard lock(mutex_);
      snapshot.active_control_configuration = active_configuration_;
      snapshot.expected_controller_domain.source_runtime_epoch = runtime_epoch_;
      if (component_state_) {
        for (auto& component : snapshot.component_expectations) {
          component.expected_component_lifecycle_state = *component_state_;
        }
      }
    }
    if (snapshot.retained_bundle_id != bundle_id || snapshot.graph_revision != graph_revision ||
        snapshot.expected_controller_domain.desired_configuration != desired) {
      return Result<RouteSelectionCurrentSnapshot>::Err(
          ErrorDTO{"route_snapshot_mismatch", "unexpected route snapshot query", {}});
    }
    return verify_route_selection_current_snapshot(std::move(snapshot));
  }

  [[nodiscard]] std::size_t read_count() const noexcept { return read_count_.load(); }

 private:
  mutable std::atomic<std::size_t> read_count_{0U};
  mutable std::mutex mutex_;
  std::optional<ActiveControlConfigurationDTO> active_configuration_;
  RuntimeEpoch runtime_epoch_{"runtime-epoch.1"};
  std::optional<ComponentLifecycleStateDTO> component_state_;
};

class FakeControllerPort final : public IControllerLifecyclePort {
 public:
  explicit FakeControllerPort(bool initially_active = true) : active_(initially_active) {}
  void SetActive(bool value) { active_.store(value); }
  void SetObservationUnavailable(bool value) { observation_unavailable_.store(value); }

  [[nodiscard]] Result<ControllerLifecycleObservationDTO> Observe(
      const ControllerLifecycleObservationQueryDTO& query) override {
    ++observe_count_;
    if (observation_unavailable_.load()) {
      return Result<ControllerLifecycleObservationDTO>::Err(
          ErrorDTO{"controller_observation_unavailable", "source is unavailable", {}});
    }
    const auto expected = ExpectedDomain();
    const bool active = active_.load();
    std::vector<ActiveControlConfigurationBindingRouteDTO> routes;
    for (const auto& route : expected.desired_configuration.binding_routes) {
      routes.push_back(
          {route.control_binding_id, route.control_binding_revision, route.control_route_id});
    }
    std::optional<ControllerLifecycleObservedConfigurationDTO> active_configuration{
        ControllerLifecycleObservedConfigurationDTO{
            expected.desired_configuration.activation_set_id, std::move(routes)}};
    return Result<ControllerLifecycleObservationDTO>::Ok(
        {query,
         static_cast<std::uint64_t>(observe_count_.load()),
         99,
         100,
         1'000'000,
         std::move(active_configuration),
         {{expected.expected_controllers.front(), active
                                                      ? ControllerLifecycleActivityDTO::kActive
                                                      : ControllerLifecycleActivityDTO::kInactive}},
         {},
         {{expected.expected_endpoints.front(), true}}});
  }

  [[nodiscard]] ControllerLifecycleTransitionResultDTO ApplyTransitionStep(
      const ControllerLifecycleTransitionIntentDTO& intent, std::stop_token) override {
    active_.store(true);
    {
      std::lock_guard lock(mutex_);
      ++dispatch_count_;
    }
    dispatched_.notify_all();
    return {intent, ControllerLifecycleTransitionDispatchStateDTO::kDispatchedKnown,
            ControllerLifecycleTransitionKnownResultDTO::kSucceeded, std::nullopt};
  }

  [[nodiscard]] std::size_t observe_count() const noexcept { return observe_count_.load(); }
  [[nodiscard]] std::size_t dispatch_count() const noexcept { return dispatch_count_.load(); }

  [[nodiscard]] bool WaitForDispatch(std::chrono::nanoseconds timeout) {
    std::unique_lock lock(mutex_);
    return dispatched_.wait_for(lock, timeout, [&] { return dispatch_count_.load() != 0U; });
  }

 private:
  std::atomic<std::size_t> observe_count_{0U};
  std::atomic<std::size_t> dispatch_count_{0U};
  std::atomic<bool> active_;
  std::atomic<bool> observation_unavailable_{false};
  std::mutex mutex_;
  std::condition_variable dispatched_;
};

class FakeControllerResolver final : public ICCLCControllerLifecyclePortResolver {
 public:
  explicit FakeControllerResolver(std::shared_ptr<FakeControllerPort> port)
      : port_(std::move(port)) {}

  [[nodiscard]] Result<std::shared_ptr<IControllerLifecyclePort>>
  ResolveExactControllerLifecyclePort(
      const ControllerLifecycleObservationQueryDTO& query) const override {
    if (query != MakeControllerLifecycleObservationQuery(ExpectedDomain())) {
      return Result<std::shared_ptr<IControllerLifecyclePort>>::Err(
          ErrorDTO{"controller_query_mismatch", "unexpected controller query", {}});
    }
    return Result<std::shared_ptr<IControllerLifecyclePort>>::Ok(port_);
  }

 private:
  std::shared_ptr<FakeControllerPort> port_;
};

class FakeRouteFencePort final : public ICCLCKernelRouteConfigurationFencePort {
 public:
  void LoseAcquireResponse(bool value) noexcept { lose_acquire_response_.store(value); }
  void ReconcileAcquireAsAccepted(bool value) noexcept {
    reconcile_acquire_as_accepted_.store(value);
  }
  void ReconcileAcquireAsNotSeen(bool value) noexcept {
    reconcile_acquire_as_not_seen_.store(value);
  }
  void LoseReleaseResponse(bool value) noexcept { lose_release_response_.store(value); }
  void ReconcileReleaseAsReleased(bool value) noexcept {
    reconcile_release_as_released_.store(value);
  }
  void SetFenceCurrentConfiguration(ExpectedActiveControlConfigurationDTO configuration) {
    status_current_configuration_ = std::move(configuration);
  }

  [[nodiscard]] Result<RouteConfigurationFenceAdmissionSnapshotDTO>
  ObserveRouteConfigurationFenceAdmission(
      const RouteConfigurationFenceAdmissionQueryDTO& query) const override {
    ++admission_count_;
    return Result<RouteConfigurationFenceAdmissionSnapshotDTO>::Ok(FenceAdmissionSnapshot(query));
  }

  [[nodiscard]] Result<RouteConfigurationFenceAcceptedDTO> AcquireRouteConfigurationFence(
      const RouteConfigurationFenceIntentDTO& intent) override {
    accepted_ = RouteConfigurationFenceAcceptedDTO{
        intent.owner_request_context.request_id,
        RequestReplayStatusDTO::kAcceptedNew,
        intent.route_selection_transaction_id,
        intent.route_selection_transaction_generation,
        ConfigurationFenceId("route-fence.1"),
        ConfigurationFenceGeneration(intent.expected_fence_generation.value() + 1U),
        intent.scope,
        intent.candidate_active_control_configuration_id,
        intent.candidate_active_control_configuration_generation,
        100,
        ContentDigest(Sha('1'))};
    {
      std::lock_guard lock(mutex_);
      ++acquire_count_;
    }
    acquired_.notify_all();
    if (lose_acquire_response_.load()) {
      return Result<RouteConfigurationFenceAcceptedDTO>::Err(
          ErrorDTO{"route_fence_acquire_response_lost", "Acquire response was lost", {}});
    }
    return Result<RouteConfigurationFenceAcceptedDTO>::Ok(*accepted_);
  }

  [[nodiscard]] Result<RouteConfigurationFenceAcquireAttemptReconciliationDTO>
  ReconcileRouteConfigurationFenceAcquireAttempt(
      const RouteConfigurationFenceIntentDTO& intent) const override {
    ++acquire_reconciliation_count_;
    if (reconcile_acquire_as_not_seen_.load()) {
      return Result<RouteConfigurationFenceAcquireAttemptReconciliationDTO>::Ok(
          {intent, RouteConfigurationFenceAcquireAttemptDispositionDTO::kNotSeen, std::nullopt,
           std::nullopt, std::nullopt});
    }
    if (!reconcile_acquire_as_accepted_.load() || !accepted_.has_value() ||
        !accepted_->matches(intent)) {
      return Result<RouteConfigurationFenceAcquireAttemptReconciliationDTO>::Err(
          ErrorDTO{"route_fence_acquire_reconciliation_unavailable",
                   "Acquire reconciliation is inconclusive",
                   {}});
    }
    const RouteConfigurationFenceStatusQueryDTO query{
        accepted_->route_selection_transaction_id,
        accepted_->route_selection_transaction_generation, accepted_->configuration_fence_id,
        accepted_->configuration_fence_generation, accepted_->captured_scope};
    const auto status = ObserveRouteConfigurationFenceStatus(query);
    if (!status.ok()) {
      return Result<RouteConfigurationFenceAcquireAttemptReconciliationDTO>::Err(status.error());
    }
    return Result<RouteConfigurationFenceAcquireAttemptReconciliationDTO>::Ok(
        {intent, RouteConfigurationFenceAcquireAttemptDispositionDTO::kAcceptedExact, accepted_,
         status.value(), std::nullopt});
  }

  [[nodiscard]] Result<RouteConfigurationFenceStatusDTO> ObserveRouteConfigurationFenceStatus(
      const RouteConfigurationFenceStatusQueryDTO& query) const override {
    ++status_count_;
    if (!accepted_.has_value()) {
      return Result<RouteConfigurationFenceStatusDTO>::Err(
          ErrorDTO{"route_fence_missing", "route fence is not held", {}});
    }
    const ExpectedActiveControlConfigurationDTO current =
        status_current_configuration_.value_or(ExpectedActiveControlConfigurationDTO{
            ExpectedActiveControlConfigurationPresenceDTO::kPresent,
            accepted_->candidate_active_control_configuration_id,
            accepted_->candidate_active_control_configuration_generation});
    RouteConfigurationFenceReconciliationEvidenceDTO evidence{
        {RuntimeUnitId("runtime.kernel"), RuntimeEpoch("kernel-epoch.1"),
         RequestOwnerEpoch("kernel-owner.1"), ContentDigest(Sha('b')),
         ActiveBundlePublicationGeneration(3U)},
        query.route_selection_transaction_id,
        query.route_selection_transaction_generation,
        query.configuration_fence_id,
        query.configuration_fence_generation,
        query.expected_scope,
        12U,
        RetainedBundleId("bundle.1"),
        GraphRevision("graph.1"),
        {RetainedBundleId("bundle.1"),
         GraphRevision("graph.1"),
         {SceneId(Sha('c')), 2U, ContentDigest(Sha('d'))}},
        ActiveBundlePublicationGeneration(3U),
        ContentDigest(Sha('e')),
        current,
        0U,
        0U,
        ContentDigest(Sha('2'))};
    return Result<RouteConfigurationFenceStatusDTO>::Ok({query,
                                                         std::move(evidence),
                                                         RouteConfigurationFenceStateDTO::kHeld,
                                                         true,
                                                         std::nullopt,
                                                         {},
                                                         ContentDigest(Sha('3')),
                                                         std::nullopt});
  }

  [[nodiscard]] Result<RouteConfigurationFenceReleasedDTO> ReleaseRouteConfigurationFence(
      const RouteConfigurationFenceReleaseRequestDTO& request) override {
    release_receipt_ =
        RouteConfigurationFenceReleasedDTO{request,
                                           RequestReplayStatusDTO::kAcceptedNew,
                                           100,
                                           request.accepted_fence.configuration_fence_id,
                                           request.accepted_fence.configuration_fence_generation,
                                           ContentDigest(Sha('4')),
                                           ContentDigest(Sha('5'))};
    {
      std::lock_guard lock(mutex_);
      ++release_count_;
    }
    release_called_.notify_all();
    if (lose_release_response_.load()) {
      return Result<RouteConfigurationFenceReleasedDTO>::Err(
          ErrorDTO{"route_fence_release_response_lost", "Release response was lost", {}});
    }
    return Result<RouteConfigurationFenceReleasedDTO>::Ok(*release_receipt_);
  }

  [[nodiscard]] Result<RouteConfigurationFenceReleaseAttemptReconciliationDTO>
  ReconcileRouteConfigurationFenceReleaseAttempt(
      const RouteConfigurationFenceReleaseRequestDTO& request) const override {
    ++release_reconciliation_count_;
    if (!reconcile_release_as_released_.load() || !release_receipt_.has_value() ||
        release_receipt_->request != request) {
      return Result<RouteConfigurationFenceReleaseAttemptReconciliationDTO>::Err(
          ErrorDTO{"route_fence_release_reconciliation_unavailable",
                   "Release reconciliation is inconclusive",
                   {}});
    }
    return Result<RouteConfigurationFenceReleaseAttemptReconciliationDTO>::Ok(
        {request, RouteConfigurationFenceReleaseAttemptDispositionDTO::kReleasedExact,
         release_receipt_, std::nullopt, std::nullopt});
  }

  [[nodiscard]] std::size_t acquire_count() const noexcept { return acquire_count_.load(); }
  [[nodiscard]] std::size_t release_count() const noexcept { return release_count_.load(); }
  [[nodiscard]] std::size_t acquire_reconciliation_count() const noexcept {
    return acquire_reconciliation_count_.load();
  }
  [[nodiscard]] std::size_t release_reconciliation_count() const noexcept {
    return release_reconciliation_count_.load();
  }
  [[nodiscard]] bool WaitForAcquire(std::chrono::nanoseconds timeout) {
    std::unique_lock lock(mutex_);
    return acquired_.wait_for(lock, timeout, [&] { return acquire_count_.load() != 0U; });
  }
  [[nodiscard]] bool WaitForRelease(std::chrono::nanoseconds timeout) {
    std::unique_lock lock(mutex_);
    return release_called_.wait_for(lock, timeout, [&] { return release_count_.load() != 0U; });
  }

 private:
  mutable std::atomic<std::size_t> admission_count_{0U};
  std::atomic<std::size_t> acquire_count_{0U};
  mutable std::atomic<std::size_t> status_count_{0U};
  std::atomic<std::size_t> release_count_{0U};
  mutable std::atomic<std::size_t> acquire_reconciliation_count_{0U};
  mutable std::atomic<std::size_t> release_reconciliation_count_{0U};
  std::atomic<bool> lose_acquire_response_{false};
  std::atomic<bool> reconcile_acquire_as_accepted_{false};
  std::atomic<bool> reconcile_acquire_as_not_seen_{false};
  std::atomic<bool> lose_release_response_{false};
  std::atomic<bool> reconcile_release_as_released_{false};
  std::optional<RouteConfigurationFenceAcceptedDTO> accepted_;
  std::optional<RouteConfigurationFenceReleasedDTO> release_receipt_;
  std::optional<ExpectedActiveControlConfigurationDTO> status_current_configuration_;
  std::mutex mutex_;
  std::condition_variable acquired_;
  std::condition_variable release_called_;
};

class FakePublicationPort final : public ICCLCRouteConfigurationPublicationPort {
 public:
  explicit FakePublicationPort(SqliteCCLCJournal& journal) : journal_(journal) {}

  [[nodiscard]] Result<ActiveControlConfigurationDTO> ResolveResultingActiveControlConfiguration(
      const RouteSelectionAcceptedDTO& acceptance,
      const ControllerLifecycleExpectedDomain& expected_controller_domain,
      const ControllerLifecycleObservationDTO& observation,
      const ControllerLifecycleReconciliationDTO& reconciliation,
      RouteSelectionEffectDTO transaction_effect) const override {
    if (expected_controller_domain.value() != ExpectedDomain()) {
      return Result<ActiveControlConfigurationDTO>::Err(
          ErrorDTO{"publication_input_mismatch", "unexpected route publication input", {}});
    }
    return ResolveRouteSelectionActiveControlConfiguration(acceptance, expected_controller_domain,
                                                           observation, reconciliation,
                                                           transaction_effect, {});
  }

  [[nodiscard]] Result<RouteReadinessPublicationReceiptDTO> PublishAndReadBack(
      const RouteSelectionAcceptedDTO& acceptance,
      const ControllerLifecycleExpectedDomain& expected_controller_domain,
      const ActiveControlConfigurationDTO& durable_configuration) override {
    const auto current =
        journal_.LoadCurrentActiveControlConfiguration(durable_configuration.control_domain_id);
    saw_durable_configuration_.store(current.ok() && current.value() == durable_configuration);
    if (!current.ok() || current.value() != durable_configuration ||
        expected_controller_domain.value() != ExpectedDomain()) {
      return Result<RouteReadinessPublicationReceiptDTO>::Err(
          ErrorDTO{"publication_readback_failed", "durable configuration is not readable", {}});
    }
    ++publication_count_;
    if (fail_publication_.load()) {
      return Result<RouteReadinessPublicationReceiptDTO>::Err(
          ErrorDTO{"invalid_request",
                   "candidate exceeds Stop isolation",
                   {{"reason", "stop_stage_attempt_prequalification_exceeded"}}});
    }
    std::vector<RouteReadinessPublicationRowDTO> readiness;
    const bool reconciled =
        durable_configuration.state == ActiveControlConfigurationStateDTO::kReconciled;
    if (reconciled) {
      const auto& component = acceptance.component_expectations.front();
      const auto& binding = acceptance.desired_configuration.binding_routes.front();
      readiness.push_back({component.controllable_component_id,
                           component.expected_component_lifecycle_generation,
                           binding.control_binding_id, binding.control_binding_revision,
                           ResolvedStopBasePlanId(Sha('9'))});
    }
    return Result<RouteReadinessPublicationReceiptDTO>::Ok(
        {acceptance.route_selection_transaction_id,
         acceptance.route_selection_transaction_generation,
         durable_configuration.active_control_configuration_id,
         durable_configuration.active_control_configuration_generation, std::move(readiness),
         reconciled, 100, ContentDigest(Sha('8'))});
  }

  [[nodiscard]] bool saw_durable_configuration() const noexcept {
    return saw_durable_configuration_.load();
  }
  [[nodiscard]] std::size_t publication_count() const noexcept { return publication_count_.load(); }
  void FailPublication(bool fail) { fail_publication_.store(fail); }

 private:
  SqliteCCLCJournal& journal_;
  std::atomic<bool> saw_durable_configuration_{false};
  std::atomic<std::size_t> publication_count_{0U};
  std::atomic<bool> fail_publication_{false};
};

[[nodiscard]] RouteSelectionTransactionId SeedInterruptedControllerTransition(
    SqliteCCLCJournal& journal, FakeRouteFencePort& route_fence, FakeControllerPort& controller,
    test::FakeCellAdmissionPort& cell_admission, const RequestOwnerEpoch& seed) {
  const auto begun = journal.BeginOwnerEpoch(seed, 100, 8U);
  if (!begun.ok()) {
    throw std::runtime_error(begun.error().message);
  }
  const auto owner = begun.value().request_owner_epoch;
  const auto intent = Intent(owner);
  const RouteSelectionTransactionId transaction_id("route-selection.1");
  const RouteSelectionTransactionGeneration transaction_generation(1U);
  const RouteConfigurationFenceAdmissionQueryDTO fence_query{
      transaction_id,
      transaction_generation,
      Scope(),
      RetainedBundleId("bundle.1"),
      GraphRevision("graph.1"),
      {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt}};
  const auto fence_snapshot = FenceAdmissionSnapshot(fence_query);
  const RouteSelectionAdmissionCandidate candidate{
      transaction_id,
      transaction_generation,
      ActiveControlConfigurationId("active-configuration.1"),
      ActiveControlConfigurationGeneration(1U),
      ActiveControlConfigurationGeneration(0U),
      100,
      1'000'000,
      RetainedBundleId("bundle.1"),
      GraphRevision("graph.1"),
      CurrentSnapshot().component_expectations,
      std::nullopt,
      ExpectedDomain(),
      Scope(),
      fence_snapshot};
  const auto prepared = PrepareRouteSelectionAdmission(intent, candidate);
  if (!prepared.ok()) {
    throw std::runtime_error(prepared.error().message);
  }
  const CellAdmissionClaimIntentDTO claim_intent{
      CellAdmissionClaimKindDTO::kComponentLifecycle,
      CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
      owner,
      transaction_id.value(),
      intent.request_context.request_id,
      ContentDigest(detail::RouteSelectionSemanticIntentDigest(intent)),
      *intent.expected_admission_generation,
      {},
      {},
      {},
      intent.program_node_evidence};
  if (!journal.RecordCellClaimAttempt(owner, claim_intent).ok())
    throw std::runtime_error("claim attempt failed");
  const auto claim = cell_admission.AcquireCellAdmissionClaim(claim_intent);
  if (!claim.ok() || !journal.RecordCellClaimAcquired(owner, claim.value()).ok()) {
    throw std::runtime_error("claim acquisition failed");
  }
  const auto accepted = journal.AcceptOrReplayRouteSelection(
      owner, intent, prepared.value().acceptance, prepared.value().initial_status);
  if (!accepted.ok()) {
    throw std::runtime_error(accepted.error().message);
  }

  const RouteConfigurationFenceIntentDTO fence_intent{
      {RequestId("seed.route.acquire.1"), fence_snapshot.execution_host_epoch.request_owner_epoch,
       "route recovery fixture", std::nullopt},
      intent.request_context.request_id,
      transaction_id,
      transaction_generation,
      Scope(),
      fence_snapshot.snapshot_digest,
      RouteConfigurationFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      RetainedBundleId("bundle.1"),
      GraphRevision("graph.1"),
      fence_snapshot.operation_resource_generation,
      fence_snapshot.configuration_fence_generation,
      fence_snapshot.current_active_control_configuration,
      candidate.candidate_active_control_configuration_id,
      candidate.candidate_active_control_configuration_generation,
      "complete-domain route selection"};
  auto acquiring = prepared.value().initial_status;
  ++acquiring.status_sequence;
  acquiring.phase = RouteSelectionTransactionPhaseDTO::kAcquiringFence;
  const CCLCRouteEffectAttemptDTO acquire_prepared{
      transaction_id,
      transaction_generation,
      1U,
      CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceAcquire,
      CCLCRouteEffectAttemptStateDTO::kPrepared,
      fence_intent,
      std::nullopt};
  const auto acquire_prepared_row =
      journal.RecordRouteEffectPrepared(owner, acquire_prepared, acquiring);
  if (!acquire_prepared_row.ok()) {
    throw std::runtime_error(acquire_prepared_row.error().message);
  }
  const auto fence = route_fence.AcquireRouteConfigurationFence(fence_intent);
  if (!fence.ok()) {
    throw std::runtime_error(fence.error().message);
  }
  auto observing = acquiring;
  ++observing.status_sequence;
  observing.phase = RouteSelectionTransactionPhaseDTO::kObserving;
  observing.accepted_fence = fence.value();
  auto acquire_completed = acquire_prepared;
  acquire_completed.state = CCLCRouteEffectAttemptStateDTO::kCompleted;
  acquire_completed.result = RouteConfigurationFenceAcquireEffectResultDTO{
      fence_intent, RouteConfigurationFenceAcquireEffectDispositionDTO::kAccepted, fence.value()};
  const auto acquire_completed_row =
      journal.RecordRouteEffectCompleted(owner, acquire_completed, observing);
  if (!acquire_completed_row.ok()) {
    throw std::runtime_error(acquire_completed_row.error().message);
  }

  const auto observation =
      controller.Observe(prepared.value().acceptance.controller_lifecycle_observation_query);
  if (!observation.ok()) {
    throw std::runtime_error(observation.error().message);
  }
  const auto reconciliation =
      ReconcileControllerLifecycle(ExpectedDomain(), observation.value(), 100);
  const auto plan =
      BuildControllerLifecycleTransitionPlan(ExpectedDomain(), observation.value(), 100);
  if (!reconciliation.ok() || !plan.ok() || plan.value().steps.empty()) {
    throw std::runtime_error(
        reconciliation.ok() ? (plan.ok() ? "missing route transition step" : plan.error().message)
                            : reconciliation.error().message);
  }
  auto activating = observing;
  ++activating.status_sequence;
  activating.phase = RouteSelectionTransactionPhaseDTO::kActivating;
  activating.controller_observation = observation.value();
  activating.controller_reconciliation = reconciliation.value();
  activating.controller_transition_plan = plan.value();
  const auto observed_row = journal.RecordRouteSelectionStatus(owner, activating);
  if (!observed_row.ok()) {
    throw std::runtime_error(observed_row.error().message);
  }

  const ControllerLifecycleTransitionIntentDTO transition{
      prepared.value().acceptance.controller_lifecycle_observation_query,
      observation.value().source_sequence, 0U, plan.value().steps.front(),
      activating.remaining_duration_ns};
  auto transition_prepared_status = activating;
  ++transition_prepared_status.status_sequence;
  const CCLCRouteEffectAttemptDTO transition_prepared{
      transaction_id,
      transaction_generation,
      2U,
      CCLCRouteEffectKindDTO::kControllerLifecycleTransition,
      CCLCRouteEffectAttemptStateDTO::kPrepared,
      transition,
      std::nullopt};
  const auto transition_row =
      journal.RecordRouteEffectPrepared(owner, transition_prepared, transition_prepared_status);
  if (!transition_row.ok()) {
    throw std::runtime_error(transition_row.error().message);
  }
  return transaction_id;
}

TEST(RouteSelectionCoordinator, ObservationDoesNotNeedAdmissionOrRepairMismatchedControllers) {
  for (const bool active : {false, true}) {
    SCOPED_TRACE(active);
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename),
                              CellId("cell.1"), 8U);
    CCLCDomainReservationTable reservations;
    UnusedComponentSnapshotSource component_snapshots;
    UnusedCatalogSource catalogs;
    UnusedBrandResolver brand_resolver;
    RecoveryAwareAdmissionPort admission;
    FakeRouteSnapshotSource snapshots;
    auto controller = std::make_shared<FakeControllerPort>(active);
    FakeControllerResolver resolver(controller);
    FakeClock clock;
    clock.set(100);
    WaitableManualScheduler scheduler;
    ComponentLifecycleCoordinator coordinator(
        Config(), journal, reservations, component_snapshots, catalogs, brand_resolver, admission,
        clock, scheduler, ComponentLifecycleCoordinatorRouteServices{&snapshots, &resolver, nullptr, nullptr});
    // No Start, owner rotation, worker or request admission is needed for reads.
    EXPECT_FALSE(coordinator.accepting_new_requests());
    const auto observed = coordinator.ObserveRouteConfiguration(
        RetainedBundleId("bundle.1"), GraphRevision("graph.1"), Desired());
    ASSERT_TRUE(observed.ok()) << observed.error().message;
    EXPECT_EQ(
        observed.value().second.disposition,
        active ? ControllerLifecycleReconciliationDispositionDTO::kReconciled
               : ControllerLifecycleReconciliationDispositionDTO::kExternalOrUnknownUnreconciled);
    EXPECT_EQ(observed.value().first.query,
              MakeControllerLifecycleObservationQuery(ExpectedDomain()));
    EXPECT_EQ(observed.value().first.source_sequence, observed.value().second.source_sequence);
    EXPECT_TRUE(coordinator
                    .ObserveRouteConfiguration(
                        RetainedBundleId("bundle.1"), GraphRevision("graph.1"), Desired(),
                        ExpectedActiveControlConfigurationDTO{
                            ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt,
                            std::nullopt})
                    .ok());
    const auto changed_configuration = coordinator.ObserveRouteConfiguration(
        RetainedBundleId("bundle.1"), GraphRevision("graph.1"), Desired(),
        ExpectedActiveControlConfigurationDTO{
            ExpectedActiveControlConfigurationPresenceDTO::kPresent,
            ActiveControlConfigurationId("configuration.old"),
            ActiveControlConfigurationGeneration(1U)});
    ASSERT_FALSE(changed_configuration.ok());
    EXPECT_EQ(changed_configuration.error().code, "route_observation_configuration_changed");
    EXPECT_EQ(observed.value().first.controllers.front().activity,
              active ? ControllerLifecycleActivityDTO::kActive
                     : ControllerLifecycleActivityDTO::kInactive);

    controller->SetObservationUnavailable(true);
    const auto unavailable = coordinator.ObserveRouteConfiguration(
        RetainedBundleId("bundle.1"), GraphRevision("graph.1"), Desired());
    ASSERT_FALSE(unavailable.ok());
    EXPECT_EQ(unavailable.error().code, "controller_observation_unavailable");
    controller->SetObservationUnavailable(false);
    clock.set(1'000'001);
    EXPECT_FALSE(coordinator
                     .ObserveRouteConfiguration(RetainedBundleId("bundle.1"),
                                                GraphRevision("graph.1"), Desired())
                     .ok());
    clock.set(100);
    snapshots.SetRuntimeEpoch(RuntimeEpoch("runtime-epoch.foreign"));
    EXPECT_FALSE(coordinator
                     .ObserveRouteConfiguration(RetainedBundleId("bundle.1"),
                                                GraphRevision("graph.1"), Desired())
                     .ok());
    snapshots.SetRuntimeEpoch(RuntimeEpoch("runtime-epoch.1"));
    EXPECT_FALSE(coordinator
                     .ObserveRouteConfiguration(RetainedBundleId("bundle.foreign"),
                                                GraphRevision("graph.1"), Desired())
                     .ok());
    EXPECT_EQ(controller->dispatch_count(), 0U);
    EXPECT_EQ(admission.recovery_acquisitions, 0U);
    EXPECT_TRUE(reservations.snapshot().empty());
    const auto publication =
        journal.LoadCurrentActiveControlConfiguration(Desired().control_domain_id);
    ASSERT_TRUE(publication.ok());
    EXPECT_FALSE(publication.value());
    EXPECT_FALSE(coordinator.accepting_new_requests());
  }
}

TEST(RouteSelectionCoordinator, ObservationRejectsMissingReadServicesBeforeStart) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  UnusedComponentSnapshotSource component_snapshots;
  UnusedCatalogSource catalogs;
  UnusedBrandResolver brand_resolver;
  RecoveryAwareAdmissionPort admission;
  FakeRouteSnapshotSource snapshots;
  auto controller = std::make_shared<FakeControllerPort>(true);
  FakeControllerResolver resolver(controller);
  FakeClock clock;
  WaitableManualScheduler scheduler;
  const std::vector<std::optional<ComponentLifecycleCoordinatorRouteServices>> cases{
      std::nullopt,
      ComponentLifecycleCoordinatorRouteServices{nullptr, &resolver, nullptr, nullptr},
      ComponentLifecycleCoordinatorRouteServices{&snapshots, nullptr, nullptr, nullptr}};
  for (std::size_t index = 0; index < cases.size(); ++index) {
    SCOPED_TRACE(index);
    ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, component_snapshots,
                                              catalogs, brand_resolver, admission, clock, scheduler,
                                              cases[index]);
    const auto result = coordinator.ObserveRouteConfiguration(RetainedBundleId("bundle.1"),
                                                              GraphRevision("graph.1"), Desired());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, "route_observation_unavailable");
    EXPECT_FALSE(coordinator.accepting_new_requests());
  }
  EXPECT_EQ(controller->dispatch_count(), 0U);
  EXPECT_EQ(admission.recovery_acquisitions, 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

void CheckPrivateRecoveryUsesExactSavedRoute(bool disabled) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  UnusedComponentSnapshotSource component_snapshots;
  UnusedCatalogSource catalogs;
  UnusedBrandResolver brand_resolver;
  RecoveryAwareAdmissionPort admission;
  FakeRouteSnapshotSource snapshots;
  auto controller = std::make_shared<FakeControllerPort>();
  FakeControllerResolver resolver(controller);
  FakeRouteFencePort fence;
  FakePublicationPort publisher(journal);
  FakeClock clock;
  clock.set(100);
  WaitableManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(
      Config(), journal, reservations, component_snapshots, catalogs, brand_resolver, admission,
      clock, scheduler, ComponentLifecycleCoordinatorRouteServices{&snapshots, &resolver, &fence, &publisher});
  ASSERT_TRUE(coordinator.Start().ok());
  ASSERT_TRUE(coordinator.SubmitRouteSelection(Intent(coordinator.request_owner_epoch())).ok());
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto saved = journal.LoadCurrentActiveControlConfiguration(Desired().control_domain_id);
  ASSERT_TRUE(saved.ok() && saved.value());
  snapshots.SetActiveConfiguration(saved.value());
  admission.recovery_predecessor = saved.value();
  const ExpectedActiveControlConfigurationDTO expected{
      ExpectedActiveControlConfigurationPresenceDTO::kPresent,
      saved.value()->active_control_configuration_id,
      saved.value()->active_control_configuration_generation};
  auto intent = Intent(coordinator.request_owner_epoch());
  intent.expected_active_control_configuration = expected;
  if (disabled) {
    snapshots.SetComponentState(ComponentLifecycleStateDTO::kDisabled);
    for (auto& component : intent.component_expectations) {
      component.expected_component_lifecycle_state = ComponentLifecycleStateDTO::kDisabled;
    }
  }
  intent.request_context.request_id = RequestId("ordinary.closed");
  const auto ordinary = coordinator.SubmitRouteSelection(intent);
  ASSERT_FALSE(ordinary.ok());
  if (disabled) {
    EXPECT_EQ(ordinary.error().code, "route_selection_component_not_ready");
  }
  EXPECT_EQ(admission.recovery_acquisitions, 0U);
  intent.request_context.request_id = RequestId("recovery.foreign");
  auto foreign = *saved.value();
  foreign.active_control_configuration_generation = ActiveControlConfigurationGeneration(99U);
  EXPECT_FALSE(coordinator.SubmitRouteRecovery(intent, foreign).ok());
  EXPECT_EQ(admission.recovery_acquisitions, 0U);
  intent.request_context.request_id = RequestId("recovery.current");
  const auto accepted = coordinator.SubmitRouteRecovery(intent, *saved.value());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  EXPECT_EQ(admission.recovery_acquisitions, 1U);
  ASSERT_TRUE(admission.recovery_claim);
  EXPECT_EQ(admission.recovery_claim->claim_kind,
            CellAdmissionClaimKindDTO::kInterventionOrRecovery);
  EXPECT_EQ(admission.recovery_claim->scope.configurations.front().active_configuration_id,
            saved.value()->active_control_configuration_id);
  const auto status = coordinator.LoadRouteSelectionStatus(
      accepted.value().acceptance->route_selection_transaction_id);
  ASSERT_TRUE(status.ok() && status.value());
  EXPECT_EQ(status.value()->terminal_result, RouteSelectionTerminalResultDTO::kSucceeded);
  EXPECT_EQ(status.value()->effect, RouteSelectionEffectDTO::kKnownNoEffect);
  EXPECT_EQ(status.value()->acceptance.component_expectations, intent.component_expectations);
  EXPECT_TRUE(status.value()->controller_transition_results.empty());
  EXPECT_EQ(controller->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_FALSE(admission.GetAuthoringAdmission().authoring_allowed);
  EXPECT_TRUE(coordinator.SubmitRouteRecovery(intent, *saved.value()).ok());
  EXPECT_EQ(admission.recovery_acquisitions, 1U);
  EXPECT_EQ(controller->dispatch_count(), 0U);

  // An inactive controller is recoverable through ordinary route selection,
  // but private owner recovery must expose that mismatch, not activate it.
  const auto current = status.value()->resulting_active_control_configuration;
  ASSERT_TRUE(current);
  snapshots.SetActiveConfiguration(current);
  admission.recovery_predecessor = current;
  controller->SetActive(false);
  intent.request_context.request_id = RequestId("recovery.controller-inactive");
  intent.expected_active_control_configuration = {
      ExpectedActiveControlConfigurationPresenceDTO::kPresent,
      current->active_control_configuration_id, current->active_control_configuration_generation};
  const auto mismatched = coordinator.SubmitRouteRecovery(intent, *current);
  ASSERT_TRUE(mismatched.ok()) << mismatched.error().message;
  ASSERT_TRUE(mismatched.value().acceptance);
  const bool drained = coordinator.DrainFor(std::chrono::seconds(2));
  const auto failed = coordinator.LoadRouteSelectionStatus(
      mismatched.value().acceptance->route_selection_transaction_id);
  ASSERT_TRUE(failed.ok() && failed.value());
  ASSERT_TRUE(drained) << "phase=" << static_cast<int>(failed.value()->phase) << "; error="
                       << (failed.value()->error ? failed.value()->error->message : "none");
  EXPECT_EQ(failed.value()->terminal_result, RouteSelectionTerminalResultDTO::kFailedPartialEffect);
  ASSERT_TRUE(failed.value()->resulting_active_control_configuration);
  EXPECT_NE(failed.value()->resulting_active_control_configuration->state,
            ActiveControlConfigurationStateDTO::kReconciled);
  EXPECT_TRUE(failed.value()->controller_transition_results.empty());
  EXPECT_FALSE(failed.value()->controller_transition_plan);
  ASSERT_TRUE(failed.value()->error);
  EXPECT_EQ(failed.value()->error->code, "route_selection_unreconciled");
  EXPECT_EQ(controller->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_FALSE(admission.GetAuthoringAdmission().authoring_allowed);
}

TEST(RouteSelectionCoordinator, PrivateRecoveryUsesExactSavedRouteWithoutOrdinaryAdmission) {
  CheckPrivateRecoveryUsesExactSavedRoute(false);
}

TEST(RouteSelectionCoordinator, DisabledRecoveryObservesWithoutCommandsAndRefusesOrdinarySelection) {
  CheckPrivateRecoveryUsesExactSavedRoute(true);
}

TEST(RouteSelectionCoordinator, RetainsPublicationFailureWithoutFloodingAndClearsOnRecovery) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  UnusedComponentSnapshotSource component_snapshots;
  UnusedCatalogSource catalogs;
  UnusedBrandResolver brand_resolver;
  RecoveryAwareAdmissionPort admission_port;
  FakeRouteSnapshotSource route_snapshots;
  auto controller = std::make_shared<FakeControllerPort>();
  FakeControllerResolver controller_resolver(controller);
  FakeRouteFencePort route_fence;
  FakePublicationPort publisher(journal);
  publisher.FailPublication(true);
  FakeClock clock;
  clock.set(100);
  WaitableManualScheduler scheduler;
  ComponentLifecycleCoordinatorRouteServices route_services{&route_snapshots, &controller_resolver,
                                                            &route_fence, &publisher};
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, component_snapshots,
                                            catalogs, brand_resolver, admission_port, clock,
                                            scheduler, route_services);
  ASSERT_TRUE(coordinator.Start().ok());
  auto intent = Intent(coordinator.request_owner_epoch());
  admission_port.RejectNextClaim(
      {"authoring_not_ready",
       "Not started — the cell is not idle",
       {{"reason", "component_lifecycle_program_not_idle_after_reset"},
        {"selection_revision", "9"}}});
  const auto stopped = coordinator.SubmitRouteSelection(intent);
  ASSERT_FALSE(stopped.ok());
  EXPECT_EQ(stopped.error().details.at("effect"), "not_started");
  EXPECT_TRUE(coordinator.accepting_new_requests());
  EXPECT_EQ(route_fence.acquire_count(), 0U);
  EXPECT_EQ(controller->observe_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
  const auto rejected = coordinator.ReadRouteSelectionRequestResult(intent);
  ASSERT_TRUE(rejected.ok() && rejected.value() && rejected.value()->rejection);
  EXPECT_EQ(rejected.value()->rejection, stopped.error());
  intent.request_context.request_id = RequestId("route.after-stop");
  admission_port.LoseNextReleaseReply();
  const auto accepted = coordinator.SubmitRouteSelection(intent);
  ASSERT_TRUE(accepted.ok());
  ASSERT_TRUE(accepted.value().acceptance);
  const auto id = accepted.value().acceptance->route_selection_transaction_id;
  ASSERT_TRUE(scheduler.WaitForScheduleCount(1U, std::chrono::seconds(2)));
  const auto failed = coordinator.LoadRouteSelectionStatus(id);
  ASSERT_TRUE(failed.ok() && failed.value() && failed.value()->error);
  EXPECT_EQ(failed.value()->error->details.at("reason"),
            "stop_stage_attempt_prequalification_exceeded");
  EXPECT_EQ(failed.value()->phase, RouteSelectionTransactionPhaseDTO::kPublishing);
  EXPECT_EQ(route_fence.release_count(), 0U);
  clock.set(2'000);
  scheduler.AdvanceTo(2'000);
  ASSERT_TRUE(scheduler.WaitForScheduleCount(2U, std::chrono::seconds(2)));
  const auto repeated = coordinator.LoadRouteSelectionStatus(id);
  ASSERT_TRUE(repeated.ok() && repeated.value());
  EXPECT_EQ(repeated.value()->status_sequence, failed.value()->status_sequence);
  EXPECT_EQ(publisher.publication_count(), 2U);
  publisher.FailPublication(false);
  clock.set(4'000);
  scheduler.AdvanceTo(4'000);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto recovered = coordinator.LoadRouteSelectionStatus(id);
  ASSERT_TRUE(recovered.ok() && recovered.value());
  EXPECT_EQ(recovered.value()->phase, RouteSelectionTransactionPhaseDTO::kSucceeded);
  EXPECT_FALSE(recovered.value()->error);
  EXPECT_EQ(route_fence.release_count(), 1U);
  EXPECT_TRUE(coordinator.accepting_new_requests());
  intent.request_context.request_id = RequestId("route.unknown-acquire");
  const auto& configuration = *recovered.value()->resulting_active_control_configuration;
  route_snapshots.SetActiveConfiguration(configuration);
  intent.expected_active_control_configuration = {
      ExpectedActiveControlConfigurationPresenceDTO::kPresent,
      configuration.active_control_configuration_id,
      configuration.active_control_configuration_generation};
  admission_port.RejectNextClaim({"owner_unavailable", "acquire reply unknown", {}});
  const auto unknown = coordinator.SubmitRouteSelection(intent);
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().code, "owner_unavailable");
  EXPECT_FALSE(coordinator.accepting_new_requests());
  EXPECT_EQ(route_fence.acquire_count(), 1U);
}

TEST(RouteSelectionCoordinator, CompletesSatisfiedRouteThroughDurablePublicationAndFenceRelease) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            1U);
  CCLCDomainReservationTable reservations;
  UnusedComponentSnapshotSource component_snapshots;
  UnusedCatalogSource catalogs;
  UnusedBrandResolver brand_resolver;
  RecoveryAwareAdmissionPort admission_port;
  FakeRouteSnapshotSource route_snapshots;
  auto controller = std::make_shared<FakeControllerPort>();
  FakeControllerResolver controller_resolver(controller);
  FakeRouteFencePort route_fence;
  FakePublicationPort publisher(journal);
  FakeClock clock;
  clock.set(100);
  WaitableManualScheduler scheduler;
  ComponentLifecycleCoordinatorRouteServices route_services{&route_snapshots, &controller_resolver,
                                                            &route_fence, &publisher};
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, component_snapshots,
                                            catalogs, brand_resolver, admission_port, clock,
                                            scheduler, route_services);

  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted = coordinator.SubmitRouteSelection(Intent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().code << ": " << accepted.error().message;
  EXPECT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  ASSERT_TRUE(accepted.value().acceptance.has_value());
  const auto status = coordinator.LoadRouteSelectionStatus(
      accepted.value().acceptance->route_selection_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(controller->observe_count(), 1U);
  EXPECT_EQ(controller->dispatch_count(), 0U);
  EXPECT_EQ(route_fence.acquire_count(), 1U);
  EXPECT_EQ(route_fence.release_count(), 1U);
  EXPECT_EQ(publisher.publication_count(), 1U);
  EXPECT_EQ(status.value()->phase, RouteSelectionTransactionPhaseDTO::kSucceeded);
  EXPECT_EQ(status.value()->terminal_result, RouteSelectionTerminalResultDTO::kSucceeded);
  ASSERT_TRUE(status.value()->resulting_active_control_configuration.has_value());
  EXPECT_EQ(status.value()->resulting_active_control_configuration->state,
            ActiveControlConfigurationStateDTO::kReconciled);
  EXPECT_EQ(route_snapshots.read_count(), 2U);
  EXPECT_EQ(route_fence.acquire_count(), 1U);
  EXPECT_EQ(route_fence.release_count(), 1U);
  EXPECT_EQ(controller->dispatch_count(), 0U);
  EXPECT_TRUE(publisher.saw_durable_configuration());
  EXPECT_EQ(publisher.publication_count(), 1U);
  EXPECT_TRUE(reservations.snapshot().empty());

  const auto replay = coordinator.SubmitRouteSelection(Intent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(route_fence.acquire_count(), 1U);
  EXPECT_EQ(route_fence.release_count(), 1U);

  // Current configuration outlives its bounded route result. Check the
  // journal's real FK/guard retention through compaction and owner cutover.
  coordinator.CloseAdmission();
  const auto desired = Desired();
  const ComponentLifecycleIntentDTO rejected{
      {RequestId("history.pressure"), coordinator.request_owner_epoch(), "test", std::nullopt},
      ControllableComponentId("component.arm"),
      ComponentLifecycleIntentKindDTO::kPrepare,
      GraphRevision("graph.1"),
      ComponentLifecycleGeneration(4U),
      {{desired.control_domain_id,
        desired.control_domain_revision,
        desired.controller_topology_id,
        desired.topology_revision,
        {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt},
        {{ControlBindingId("binding.arm"), ControlBindingRevision(Sha('4')),
          ControlRouteId("route.direct")}}}},
      std::nullopt,
      {RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified, std::nullopt}};
  ASSERT_TRUE(rejected.locally_valid());
  ASSERT_TRUE(journal
                  .RecordLifecycleRejection(
                      coordinator.request_owner_epoch(), rejected,
                      {"invalid_state_transition", "unstarted", {{"effect", "not_started"}}},
                      std::nullopt)
                  .ok());
  const auto expired = coordinator.LoadRouteSelectionStatus(
      accepted.value().acceptance->route_selection_transaction_id);
  ASSERT_TRUE(expired.ok());
  EXPECT_FALSE(expired.value());
  const auto expired_request =
      coordinator.ReadRouteSelectionRequestResult(Intent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(expired_request.ok() && expired_request.value());
  EXPECT_EQ(expired_request.value()->replay_status, RequestReplayStatusDTO::kRequestResultExpired);
  const auto checkpoint = journal.BeginOwnerEpoch(RequestOwnerEpoch("next.seed"), 200, 8U,
                                                  coordinator.request_owner_epoch());
  ASSERT_TRUE(checkpoint.ok()) << checkpoint.error().message;
  journal.Close();
  SqliteCCLCJournal reopened(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                             1U);
  const auto configuration =
      reopened.LoadCurrentActiveControlConfiguration(desired.control_domain_id);
  ASSERT_TRUE(configuration.ok());
  EXPECT_EQ(configuration.value(), status.value()->resulting_active_control_configuration);
}

TEST(RouteSelectionCoordinator,
     PersistsOneControllerWaveThenReobservesBeforePublicationAndRelease) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  UnusedComponentSnapshotSource component_snapshots;
  UnusedCatalogSource catalogs;
  UnusedBrandResolver brand_resolver;
  RecoveryAwareAdmissionPort admission_port;
  FakeRouteSnapshotSource route_snapshots;
  auto controller = std::make_shared<FakeControllerPort>(false);
  FakeControllerResolver controller_resolver(controller);
  FakeRouteFencePort route_fence;
  FakePublicationPort publisher(journal);
  FakeClock clock;
  clock.set(100);
  WaitableManualScheduler scheduler;
  ComponentLifecycleCoordinatorRouteServices route_services{&route_snapshots, &controller_resolver,
                                                            &route_fence, &publisher};
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, component_snapshots,
                                            catalogs, brand_resolver, admission_port, clock,
                                            scheduler, route_services);

  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted = coordinator.SubmitRouteSelection(Intent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().code << ": " << accepted.error().message;
  ASSERT_TRUE(controller->WaitForDispatch(std::chrono::seconds(2)));
  ASSERT_TRUE(scheduler.WaitForScheduleCount(1U, std::chrono::seconds(2)));
  scheduler.AdvanceTo(2'000);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));

  ASSERT_TRUE(accepted.value().acceptance.has_value());
  const auto status = coordinator.LoadRouteSelectionStatus(
      accepted.value().acceptance->route_selection_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->phase, RouteSelectionTransactionPhaseDTO::kSucceeded);
  ASSERT_EQ(status.value()->controller_transition_results.size(), 1U);
  EXPECT_EQ(status.value()->controller_transition_results.front().dispatch_state,
            ControllerLifecycleTransitionDispatchStateDTO::kDispatchedKnown);
  EXPECT_EQ(status.value()->controller_transition_results.front().known_result,
            ControllerLifecycleTransitionKnownResultDTO::kSucceeded);
  const auto original =
      coordinator.ReadRouteSelectionRequestResult(Intent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(original.ok() && original.value());
  EXPECT_EQ(original.value()->replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(original.value()->status, status.value());
  auto unknown = Intent(coordinator.request_owner_epoch());
  unknown.request_context.request_id = RequestId("unknown.route");
  const auto absent = coordinator.ReadRouteSelectionRequestResult(unknown);
  ASSERT_TRUE(absent.ok());
  EXPECT_FALSE(absent.value());
  EXPECT_EQ(controller->dispatch_count(), 1U);
  EXPECT_EQ(controller->observe_count(), 2U);
  EXPECT_EQ(route_fence.acquire_count(), 1U);
  EXPECT_EQ(route_fence.release_count(), 1U);
  EXPECT_EQ(publisher.publication_count(), 1U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

TEST(RouteSelectionCoordinator,
     ReplacementReconcilesPreparedAcquireWithoutRedispatchAndCompletesRoute) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  UnusedComponentSnapshotSource component_snapshots;
  UnusedCatalogSource catalogs;
  UnusedBrandResolver brand_resolver;
  RecoveryAwareAdmissionPort admission_port;
  FakeRouteSnapshotSource route_snapshots;
  auto controller = std::make_shared<FakeControllerPort>();
  FakeControllerResolver controller_resolver(controller);
  FakeRouteFencePort route_fence;
  route_fence.LoseAcquireResponse(true);
  FakePublicationPort publisher(journal);
  FakeClock clock;
  clock.set(100);
  std::optional<RouteSelectionTransactionId> transaction_id;

  {
    WaitableManualScheduler scheduler;
    ComponentLifecycleCoordinatorRouteServices route_services{
        &route_snapshots, &controller_resolver, &route_fence, &publisher};
    ComponentLifecycleCoordinator coordinator(
        Config(RequestOwnerEpoch("cclc-owner.1")), journal, reservations, component_snapshots,
        catalogs, brand_resolver, admission_port, clock, scheduler, route_services);
    ASSERT_TRUE(coordinator.Start().ok());
    const auto accepted =
        coordinator.SubmitRouteSelection(Intent(coordinator.request_owner_epoch()));
    ASSERT_TRUE(accepted.ok()) << accepted.error().code << ": " << accepted.error().message;
    ASSERT_TRUE(accepted.value().acceptance.has_value());
    transaction_id = accepted.value().acceptance->route_selection_transaction_id;
    ASSERT_TRUE(route_fence.WaitForAcquire(std::chrono::seconds(2)));
    EXPECT_FALSE(coordinator.DrainFor(std::chrono::milliseconds(100)));
    const auto unresolved = coordinator.LoadRouteSelectionStatus(*transaction_id);
    ASSERT_TRUE(unresolved.ok());
    ASSERT_TRUE(unresolved.value().has_value());
    EXPECT_EQ(unresolved.value()->phase, RouteSelectionTransactionPhaseDTO::kOutcomeUnknown);
    EXPECT_EQ(route_fence.acquire_count(), 1U);
    EXPECT_EQ(route_fence.acquire_reconciliation_count(), 1U);
  }

  route_fence.LoseAcquireResponse(false);
  route_fence.ReconcileAcquireAsAccepted(true);
  {
    WaitableManualScheduler scheduler;
    ComponentLifecycleCoordinatorRouteServices route_services{
        &route_snapshots, &controller_resolver, &route_fence, &publisher};
    ComponentLifecycleCoordinator coordinator(
        Config(RequestOwnerEpoch("cclc-owner.2")), journal, reservations, component_snapshots,
        catalogs, brand_resolver, admission_port, clock, scheduler, route_services);
    const auto started = coordinator.Start();
    ASSERT_TRUE(started.ok()) << started.error().code << ": " << started.error().message;
    ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
    ASSERT_TRUE(transaction_id.has_value());
    const auto status = coordinator.LoadRouteSelectionStatus(*transaction_id);
    ASSERT_TRUE(status.ok());
    ASSERT_TRUE(status.value().has_value());
    EXPECT_EQ(status.value()->phase, RouteSelectionTransactionPhaseDTO::kSucceeded);
    EXPECT_EQ(status.value()->status_owner_epoch, coordinator.request_owner_epoch());
    EXPECT_EQ(route_fence.acquire_count(), 1U);
    EXPECT_EQ(route_fence.acquire_reconciliation_count(), 2U);
    EXPECT_EQ(route_fence.release_count(), 1U);
    EXPECT_EQ(controller->dispatch_count(), 0U);
    EXPECT_EQ(publisher.publication_count(), 1U);
    EXPECT_TRUE(coordinator.accepting_new_requests());
    EXPECT_TRUE(reservations.snapshot().empty());
  }
}

TEST(RouteSelectionCoordinator, ReplacementSettlesFenceOnlyRouteWhenControllerSourceEpochChanged) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  UnusedComponentSnapshotSource component_snapshots;
  UnusedCatalogSource catalogs;
  UnusedBrandResolver brand_resolver;
  RecoveryAwareAdmissionPort admission_port;
  FakeRouteSnapshotSource route_snapshots;
  auto controller = std::make_shared<FakeControllerPort>();
  FakeControllerResolver controller_resolver(controller);
  FakeRouteFencePort route_fence;
  route_fence.LoseAcquireResponse(true);
  FakePublicationPort publisher(journal);
  FakeClock clock;
  clock.set(100);
  std::optional<RouteSelectionTransactionId> transaction_id;

  {
    WaitableManualScheduler scheduler;
    ComponentLifecycleCoordinatorRouteServices route_services{
        &route_snapshots, &controller_resolver, &route_fence, &publisher};
    ComponentLifecycleCoordinator coordinator(
        Config(RequestOwnerEpoch("cclc-owner.1")), journal, reservations, component_snapshots,
        catalogs, brand_resolver, admission_port, clock, scheduler, route_services);
    ASSERT_TRUE(coordinator.Start().ok());
    const auto accepted =
        coordinator.SubmitRouteSelection(Intent(coordinator.request_owner_epoch()));
    ASSERT_TRUE(accepted.ok()) << accepted.error().code << ": " << accepted.error().message;
    ASSERT_TRUE(accepted.value().acceptance.has_value());
    transaction_id = accepted.value().acceptance->route_selection_transaction_id;
    ASSERT_TRUE(route_fence.WaitForAcquire(std::chrono::seconds(2)));
    EXPECT_FALSE(coordinator.DrainFor(std::chrono::milliseconds(100)));
  }

  route_snapshots.SetRuntimeEpoch(RuntimeEpoch("runtime-epoch.2"));
  route_fence.LoseAcquireResponse(false);
  route_fence.ReconcileAcquireAsAccepted(true);
  route_fence.SetFenceCurrentConfiguration(
      {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt});
  {
    WaitableManualScheduler scheduler;
    ComponentLifecycleCoordinatorRouteServices route_services{
        &route_snapshots, &controller_resolver, &route_fence, &publisher};
    ComponentLifecycleCoordinator coordinator(
        Config(RequestOwnerEpoch("cclc-owner.2")), journal, reservations, component_snapshots,
        catalogs, brand_resolver, admission_port, clock, scheduler, route_services);
    const auto started = coordinator.Start();
    ASSERT_TRUE(started.ok()) << started.error().code << ": " << started.error().message;
    ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
    ASSERT_TRUE(transaction_id.has_value());
    const auto status = coordinator.LoadRouteSelectionStatus(*transaction_id);
    ASSERT_TRUE(status.ok());
    ASSERT_TRUE(status.value().has_value());
    EXPECT_EQ(status.value()->phase, RouteSelectionTransactionPhaseDTO::kFailed);
    EXPECT_EQ(status.value()->effect, RouteSelectionEffectDTO::kKnownNoEffect);
    EXPECT_EQ(status.value()->terminal_result,
              RouteSelectionTerminalResultDTO::kFailedKnownNoEffect);
    ASSERT_TRUE(status.value()->error.has_value());
    EXPECT_EQ(status.value()->error->details.at("reason"),
              "controller_source_replaced_before_transition");
    EXPECT_EQ(status.value()->status_owner_epoch, coordinator.request_owner_epoch());
    EXPECT_EQ(route_fence.acquire_count(), 1U);
    EXPECT_EQ(route_fence.acquire_reconciliation_count(), 2U);
    EXPECT_EQ(route_fence.release_count(), 1U);
    EXPECT_EQ(controller->observe_count(), 0U);
    EXPECT_EQ(controller->dispatch_count(), 0U);
    EXPECT_EQ(publisher.publication_count(), 0U);
    EXPECT_TRUE(coordinator.accepting_new_requests());
    EXPECT_TRUE(reservations.snapshot().empty());
  }
}

TEST(RouteSelectionCoordinator,
     ReplacementProvesFenceOnlyRouteNotSeenAfterControllerSourceChanged) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  UnusedComponentSnapshotSource component_snapshots;
  UnusedCatalogSource catalogs;
  UnusedBrandResolver brand_resolver;
  RecoveryAwareAdmissionPort admission_port;
  FakeRouteSnapshotSource route_snapshots;
  auto controller = std::make_shared<FakeControllerPort>();
  FakeControllerResolver controller_resolver(controller);
  FakeRouteFencePort route_fence;
  route_fence.LoseAcquireResponse(true);
  FakePublicationPort publisher(journal);
  FakeClock clock;
  clock.set(100);
  std::optional<RouteSelectionTransactionId> transaction_id;

  {
    WaitableManualScheduler scheduler;
    ComponentLifecycleCoordinatorRouteServices route_services{
        &route_snapshots, &controller_resolver, &route_fence, &publisher};
    ComponentLifecycleCoordinator coordinator(
        Config(RequestOwnerEpoch("cclc-owner.1")), journal, reservations, component_snapshots,
        catalogs, brand_resolver, admission_port, clock, scheduler, route_services);
    ASSERT_TRUE(coordinator.Start().ok());
    const auto accepted =
        coordinator.SubmitRouteSelection(Intent(coordinator.request_owner_epoch()));
    ASSERT_TRUE(accepted.ok());
    ASSERT_TRUE(accepted.value().acceptance.has_value());
    transaction_id = accepted.value().acceptance->route_selection_transaction_id;
    ASSERT_TRUE(route_fence.WaitForAcquire(std::chrono::seconds(2)));
  }

  route_snapshots.SetRuntimeEpoch(RuntimeEpoch("runtime-epoch.2"));
  route_fence.LoseAcquireResponse(false);
  route_fence.ReconcileAcquireAsNotSeen(true);
  {
    WaitableManualScheduler scheduler;
    ComponentLifecycleCoordinatorRouteServices route_services{
        &route_snapshots, &controller_resolver, &route_fence, &publisher};
    ComponentLifecycleCoordinator coordinator(
        Config(RequestOwnerEpoch("cclc-owner.2")), journal, reservations, component_snapshots,
        catalogs, brand_resolver, admission_port, clock, scheduler, route_services);
    const auto started = coordinator.Start();
    ASSERT_TRUE(started.ok()) << started.error().code << ": " << started.error().message;
    ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
    ASSERT_TRUE(transaction_id.has_value());
    const auto status = coordinator.LoadRouteSelectionStatus(*transaction_id);
    ASSERT_TRUE(status.ok());
    ASSERT_TRUE(status.value().has_value());
    EXPECT_EQ(status.value()->phase, RouteSelectionTransactionPhaseDTO::kFailed);
    EXPECT_EQ(status.value()->terminal_result,
              RouteSelectionTerminalResultDTO::kFailedKnownNoEffect);
    EXPECT_EQ(route_fence.release_count(), 0U);
    EXPECT_EQ(controller->observe_count(), 0U);
    EXPECT_EQ(publisher.publication_count(), 0U);
    EXPECT_TRUE(coordinator.accepting_new_requests());
  }
}

TEST(RouteSelectionCoordinator, ReplacementReconcilesFenceOnlyReleaseAfterControllerSourceChanged) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  UnusedComponentSnapshotSource component_snapshots;
  UnusedCatalogSource catalogs;
  UnusedBrandResolver brand_resolver;
  RecoveryAwareAdmissionPort admission_port;
  FakeRouteSnapshotSource route_snapshots;
  auto controller = std::make_shared<FakeControllerPort>();
  FakeControllerResolver controller_resolver(controller);
  FakeRouteFencePort route_fence;
  route_fence.LoseAcquireResponse(true);
  FakePublicationPort publisher(journal);
  FakeClock clock;
  clock.set(100);
  std::optional<RouteSelectionTransactionId> transaction_id;

  {
    WaitableManualScheduler scheduler;
    ComponentLifecycleCoordinatorRouteServices route_services{
        &route_snapshots, &controller_resolver, &route_fence, &publisher};
    ComponentLifecycleCoordinator coordinator(
        Config(RequestOwnerEpoch("cclc-owner.1")), journal, reservations, component_snapshots,
        catalogs, brand_resolver, admission_port, clock, scheduler, route_services);
    ASSERT_TRUE(coordinator.Start().ok());
    const auto accepted =
        coordinator.SubmitRouteSelection(Intent(coordinator.request_owner_epoch()));
    ASSERT_TRUE(accepted.ok());
    ASSERT_TRUE(accepted.value().acceptance.has_value());
    transaction_id = accepted.value().acceptance->route_selection_transaction_id;
    ASSERT_TRUE(route_fence.WaitForAcquire(std::chrono::seconds(2)));
  }

  route_snapshots.SetRuntimeEpoch(RuntimeEpoch("runtime-epoch.2"));
  route_fence.LoseAcquireResponse(false);
  route_fence.ReconcileAcquireAsAccepted(true);
  route_fence.SetFenceCurrentConfiguration(
      {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt});
  route_fence.LoseReleaseResponse(true);
  {
    WaitableManualScheduler scheduler;
    ComponentLifecycleCoordinatorRouteServices route_services{
        &route_snapshots, &controller_resolver, &route_fence, &publisher};
    ComponentLifecycleCoordinator coordinator(
        Config(RequestOwnerEpoch("cclc-owner.2")), journal, reservations, component_snapshots,
        catalogs, brand_resolver, admission_port, clock, scheduler, route_services);
    const auto started = coordinator.Start();
    ASSERT_TRUE(started.ok()) << started.error().code << ": " << started.error().message;
    ASSERT_TRUE(route_fence.WaitForRelease(std::chrono::seconds(2)));
    EXPECT_FALSE(coordinator.DrainFor(std::chrono::milliseconds(100)));
    ASSERT_TRUE(transaction_id.has_value());
    const auto unresolved = coordinator.LoadRouteSelectionStatus(*transaction_id);
    ASSERT_TRUE(unresolved.ok());
    ASSERT_TRUE(unresolved.value().has_value());
    EXPECT_EQ(unresolved.value()->phase, RouteSelectionTransactionPhaseDTO::kOutcomeUnknown);
  }

  route_fence.LoseReleaseResponse(false);
  route_fence.ReconcileReleaseAsReleased(true);
  {
    WaitableManualScheduler scheduler;
    ComponentLifecycleCoordinatorRouteServices route_services{
        &route_snapshots, &controller_resolver, &route_fence, &publisher};
    ComponentLifecycleCoordinator coordinator(
        Config(RequestOwnerEpoch("cclc-owner.3")), journal, reservations, component_snapshots,
        catalogs, brand_resolver, admission_port, clock, scheduler, route_services);
    const auto started = coordinator.Start();
    ASSERT_TRUE(started.ok()) << started.error().code << ": " << started.error().message;
    ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
    ASSERT_TRUE(transaction_id.has_value());
    const auto status = coordinator.LoadRouteSelectionStatus(*transaction_id);
    ASSERT_TRUE(status.ok());
    ASSERT_TRUE(status.value().has_value());
    EXPECT_EQ(status.value()->phase, RouteSelectionTransactionPhaseDTO::kFailed);
    EXPECT_EQ(status.value()->effect, RouteSelectionEffectDTO::kKnownNoEffect);
    EXPECT_EQ(status.value()->terminal_result,
              RouteSelectionTerminalResultDTO::kFailedKnownNoEffect);
    ASSERT_TRUE(status.value()->error.has_value());
    EXPECT_EQ(status.value()->error->details.at("reason"),
              "controller_source_replaced_before_transition");
    EXPECT_EQ(status.value()->status_owner_epoch, coordinator.request_owner_epoch());
    EXPECT_EQ(route_fence.acquire_count(), 1U);
    EXPECT_EQ(route_fence.release_count(), 1U);
    EXPECT_EQ(route_fence.release_reconciliation_count(), 2U);
    EXPECT_EQ(controller->observe_count(), 0U);
    EXPECT_EQ(controller->dispatch_count(), 0U);
    EXPECT_EQ(publisher.publication_count(), 0U);
    EXPECT_TRUE(coordinator.accepting_new_requests());
    EXPECT_TRUE(reservations.snapshot().empty());
  }
}

TEST(RouteSelectionCoordinator,
     ReplacementReconcilesPreparedReleaseWithoutRepublishingOrRedispatch) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  UnusedComponentSnapshotSource component_snapshots;
  UnusedCatalogSource catalogs;
  UnusedBrandResolver brand_resolver;
  RecoveryAwareAdmissionPort admission_port;
  FakeRouteSnapshotSource route_snapshots;
  auto controller = std::make_shared<FakeControllerPort>();
  FakeControllerResolver controller_resolver(controller);
  FakeRouteFencePort route_fence;
  route_fence.LoseReleaseResponse(true);
  FakePublicationPort publisher(journal);
  FakeClock clock;
  clock.set(100);
  std::optional<RouteSelectionTransactionId> transaction_id;

  {
    WaitableManualScheduler scheduler;
    ComponentLifecycleCoordinatorRouteServices route_services{
        &route_snapshots, &controller_resolver, &route_fence, &publisher};
    ComponentLifecycleCoordinator coordinator(
        Config(RequestOwnerEpoch("cclc-owner.1")), journal, reservations, component_snapshots,
        catalogs, brand_resolver, admission_port, clock, scheduler, route_services);
    ASSERT_TRUE(coordinator.Start().ok());
    const auto accepted =
        coordinator.SubmitRouteSelection(Intent(coordinator.request_owner_epoch()));
    ASSERT_TRUE(accepted.ok()) << accepted.error().code << ": " << accepted.error().message;
    ASSERT_TRUE(accepted.value().acceptance.has_value());
    transaction_id = accepted.value().acceptance->route_selection_transaction_id;
    ASSERT_TRUE(route_fence.WaitForRelease(std::chrono::seconds(2)));
    EXPECT_FALSE(coordinator.DrainFor(std::chrono::milliseconds(100)));
    const auto unresolved = coordinator.LoadRouteSelectionStatus(*transaction_id);
    ASSERT_TRUE(unresolved.ok());
    ASSERT_TRUE(unresolved.value().has_value());
    EXPECT_EQ(unresolved.value()->phase, RouteSelectionTransactionPhaseDTO::kOutcomeUnknown);
    EXPECT_EQ(route_fence.release_count(), 1U);
    EXPECT_EQ(route_fence.release_reconciliation_count(), 1U);
  }

  ASSERT_TRUE(transaction_id.has_value());
  const auto interrupted = journal.LoadRetainedRouteSelectionStatus(*transaction_id);
  ASSERT_TRUE(interrupted.ok());
  ASSERT_TRUE(interrupted.value().has_value());
  ASSERT_TRUE(interrupted.value()->resulting_active_control_configuration.has_value());

  {
    WaitableManualScheduler scheduler;
    ComponentLifecycleCoordinatorRouteServices route_services{
        &route_snapshots, &controller_resolver, &route_fence, &publisher};
    ComponentLifecycleCoordinator coordinator(
        Config(RequestOwnerEpoch("cclc-owner.2")), journal, reservations, component_snapshots,
        catalogs, brand_resolver, admission_port, clock, scheduler, route_services);
    const auto started = coordinator.Start();
    ASSERT_FALSE(started.ok());
    EXPECT_EQ(started.error().code, "route_selection_recovery_current_configuration_mismatch");
  }

  route_snapshots.SetActiveConfiguration(
      interrupted.value()->resulting_active_control_configuration);
  route_fence.LoseReleaseResponse(false);
  route_fence.ReconcileReleaseAsReleased(true);

  {
    WaitableManualScheduler scheduler;
    ComponentLifecycleCoordinatorRouteServices route_services{
        &route_snapshots, &controller_resolver, &route_fence, &publisher};
    ComponentLifecycleCoordinator coordinator(
        Config(RequestOwnerEpoch("cclc-owner.3")), journal, reservations, component_snapshots,
        catalogs, brand_resolver, admission_port, clock, scheduler, route_services);
    const auto started = coordinator.Start();
    ASSERT_TRUE(started.ok()) << started.error().code << ": " << started.error().message;
    ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
    const auto status = coordinator.LoadRouteSelectionStatus(*transaction_id);
    ASSERT_TRUE(status.ok());
    ASSERT_TRUE(status.value().has_value());
    EXPECT_EQ(status.value()->phase, RouteSelectionTransactionPhaseDTO::kSucceeded);
    EXPECT_EQ(status.value()->status_owner_epoch, coordinator.request_owner_epoch());
    EXPECT_EQ(route_fence.acquire_count(), 1U);
    EXPECT_EQ(route_fence.release_count(), 1U);
    EXPECT_EQ(route_fence.release_reconciliation_count(), 2U);
    EXPECT_EQ(controller->observe_count(), 1U);
    EXPECT_EQ(controller->dispatch_count(), 0U);
    EXPECT_EQ(publisher.publication_count(), 1U);
    EXPECT_TRUE(coordinator.accepting_new_requests());
    EXPECT_TRUE(reservations.snapshot().empty());
  }
}

TEST(RouteSelectionCoordinator,
     ReplacementClassifiesPreparedControllerTransitionFromFreshTruthWithoutRedispatch) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  UnusedComponentSnapshotSource component_snapshots;
  UnusedCatalogSource catalogs;
  UnusedBrandResolver brand_resolver;
  RecoveryAwareAdmissionPort admission_port;
  FakeRouteSnapshotSource route_snapshots;
  auto controller = std::make_shared<FakeControllerPort>(false);
  FakeControllerResolver controller_resolver(controller);
  FakeRouteFencePort route_fence;
  FakePublicationPort publisher(journal);
  FakeClock clock;
  clock.set(100);
  const auto transaction_id = SeedInterruptedControllerTransition(
      journal, route_fence, *controller, admission_port, RequestOwnerEpoch("cclc-owner.1"));

  WaitableManualScheduler scheduler;
  ComponentLifecycleCoordinatorRouteServices route_services{&route_snapshots, &controller_resolver,
                                                            &route_fence, &publisher};
  ComponentLifecycleCoordinator coordinator(
      Config(RequestOwnerEpoch("cclc-owner.2")), journal, reservations, component_snapshots,
      catalogs, brand_resolver, admission_port, clock, scheduler, route_services);
  const auto started = coordinator.Start();
  ASSERT_TRUE(started.ok()) << started.error().code << ": " << started.error().message;
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));

  const auto status = coordinator.LoadRouteSelectionStatus(transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->phase, RouteSelectionTransactionPhaseDTO::kFailed);
  EXPECT_EQ(status.value()->terminal_result, RouteSelectionTerminalResultDTO::kFailedPartialEffect);
  EXPECT_EQ(status.value()->effect, RouteSelectionEffectDTO::kPartialEffectObserved);
  EXPECT_EQ(status.value()->status_owner_epoch, coordinator.request_owner_epoch());
  ASSERT_EQ(status.value()->controller_transition_results.size(), 1U);
  EXPECT_EQ(status.value()->controller_transition_results.front().dispatch_state,
            ControllerLifecycleTransitionDispatchStateDTO::kDispatchedUncertain);
  ASSERT_TRUE(status.value()->controller_transition_results.front().error.has_value());
  EXPECT_EQ(status.value()->controller_transition_results.front().error->details.at("reason"),
            "owner_replaced_after_effect_prepare");
  ASSERT_TRUE(status.value()->resulting_active_control_configuration.has_value());
  EXPECT_EQ(status.value()->resulting_active_control_configuration->state,
            ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled);
  EXPECT_EQ(controller->observe_count(), 2U);
  EXPECT_EQ(controller->dispatch_count(), 0U);
  EXPECT_EQ(route_fence.acquire_count(), 1U);
  EXPECT_EQ(route_fence.release_count(), 1U);
  EXPECT_EQ(publisher.publication_count(), 1U);
  EXPECT_TRUE(reservations.snapshot().empty());

  const auto current = journal.LoadCurrentActiveControlConfiguration(Desired().control_domain_id);
  ASSERT_TRUE(current.ok());
  ASSERT_TRUE(current.value().has_value());
  EXPECT_EQ(current.value()->state,
            ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled);
}

}  // namespace
}  // namespace manyforge::core
