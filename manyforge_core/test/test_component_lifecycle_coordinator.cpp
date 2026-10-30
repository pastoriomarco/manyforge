#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <manyforge_core/cclc_lifecycle_target_driver.hpp>
#include <manyforge_core/cclc_open_request.hpp>
#include <manyforge_core/component_lifecycle_admission.hpp>
#include <manyforge_core/component_lifecycle_coordinator.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/io_output_participant_engine.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "../src/component_lifecycle_journal_codec_internal.hpp"
#include "cclc_test_admission_port.hpp"
#include "close_retirement_sqlite_faults.hpp"

namespace manyforge::core {
namespace {

[[nodiscard]] Result<CellAdmissionClaimDTO> SeedLifecycleClaim(
    SqliteCCLCJournal& journal, const RequestOwnerEpoch& owner,
    const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleAcceptedDTO& acceptance) {
  const CellAdmissionClaimIntentDTO claim_intent{
      CellAdmissionClaimKindDTO::kComponentLifecycle,
      CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, owner,
      acceptance.lifecycle_transaction_id.value(), intent.request_context.request_id,
      ContentDigest(detail::ComponentLifecycleSemanticIntentDigest(intent)),
      AdmissionGeneration(1U), {}, std::nullopt, std::nullopt};
  auto recorded = journal.RecordCellClaimAttempt(owner, claim_intent);
  if (!recorded.ok()) {
    return Result<CellAdmissionClaimDTO>::Err(recorded.error());
  }
  CellAdmissionClaimDTO claim{claim_intent, CellAdmissionClaimGeneration(1U),
                              AdmissionGeneration(1U), 0};
  auto acquired = journal.RecordCellClaimAcquired(owner, claim);
  return acquired.ok() ? Result<CellAdmissionClaimDTO>::Ok(std::move(claim))
                       : Result<CellAdmissionClaimDTO>::Err(acquired.error());
}

class ScopedTemporaryDirectory final {
 public:
  ScopedTemporaryDirectory() {
    static std::atomic<std::uint64_t> next{1U};
    path_ = std::filesystem::temp_directory_path() /
            ("manyforge-cclc-coordinator-test-" +
             std::to_string(static_cast<std::uint64_t>(::getpid())) + "-" +
             std::to_string(next.fetch_add(1U)));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("failed to create coordinator test root");
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

[[nodiscard]] std::string Sha(char value) {
  return std::string("sha256:") + std::string(64U, value);
}

[[nodiscard]] ActiveBundlePublicationReceiptDTO ActivePublication() {
  const ProjectionRevisionDTO revision{RetainedBundleId("bundle.1"),
                                       GraphRevision("graph.1"),
                                       {SceneId(Sha('a')), 1U, ContentDigest(Sha('b'))}};
  return {{ActiveBundlePublicationTransactionRefDTO{BootstrapPublicationTransactionDTO{
               BootstrapId("bootstrap.1"), BootstrapGeneration(1U)}},
           revision, revision.retained_bundle_id, revision.graph_revision, std::nullopt,
           ContentDigest("private.1")},
          ActiveBundlePublicationGeneration(2U),
          ContentDigest("public-pointer.1")};
}

[[nodiscard]] ExpectedControlDomainRevisionDTO ExpectedDomain() {
  return {ControlDomainId("domain.1"),
          ControlDomainRevision("domain-revision.1"),
          ControllerTopologyId("topology.1"),
          TopologyRevision("topology-revision.1"),
          {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt},
          {{ControlBindingId("binding.1"), ControlBindingRevision("binding-revision.1"),
            ControlRouteId("route.1")}}};
}

[[nodiscard]] ResolvedLifecycleControlDomainScopeDTO CurrentDomain() {
  const auto expected = ExpectedDomain();
  return {expected.control_domain_id,
          expected.control_domain_revision,
          expected.controller().controller_topology_id,
          expected.controller().topology_revision,
          std::nullopt,
          expected.controller().expected_control_bindings,
          {ResourceId("resource.1")}};
}

[[nodiscard]] ActiveControlConfigurationDTO CurrentActiveConfiguration() {
  const auto expected = ExpectedDomain();
  const auto& binding = expected.controller().expected_control_bindings.front();
  return {
      ActiveControlConfigurationId("configuration.1"),
      expected.control_domain_id,
      expected.control_domain_revision,
      {{binding.control_binding_id, binding.control_binding_revision, binding.control_route_id}},
      ActivationSetId("activation.1"),
      expected.controller().controller_topology_id,
      expected.controller().topology_revision,
      RuntimeUnitId("runtime.1"),
      RuntimeEpoch("runtime-epoch.1"),
      1U,
      {},
      ActiveControlConfigurationGeneration(7U),
      ActiveControlConfigurationStateDTO::kReconciled};
}

[[nodiscard]] BrandLifecyclePlanCatalogDTO PrepareCatalog();

[[nodiscard]] ResolvedLifecycleProviderOccurrenceDTO Provider(
    const BrandLifecyclePlanCatalogDTO& catalog) {
  const auto domain = CurrentDomain();
  return {domain.control_domain_id,           domain.control_domain_revision,
          ProviderInstanceId("provider.1"),   catalog.provider_manifest_id,
          catalog.provider_manifest_revision, RuntimeUnitId("runtime.1"),
          RuntimeEpoch("runtime-epoch.1"),    catalog.catalog_digest};
}

[[nodiscard]] ComponentLifecycleCurrentSnapshotDTO Snapshot(
    std::string digest = "snapshot.1",
    ComponentLifecycleStateDTO state = ComponentLifecycleStateDTO::kDeclared,
    ComponentLifecycleGeneration generation = ComponentLifecycleGeneration(3U),
    const BrandLifecyclePlanCatalogDTO& catalog = PrepareCatalog()) {
  return {ContentDigest(std::move(digest)),
          ControllableComponentId("component.1"),
          generation,
          state,
          ActivePublication(),
          {CurrentDomain()},
          {Provider(catalog)}};
}

[[nodiscard]] std::vector<ComponentLifecycleCurrentSnapshotDTO> ResolvedDisableSnapshots() {
  const auto catalog = PrepareCatalog();
  return {Snapshot("snapshot.before.1", ComponentLifecycleStateDTO::kActive,
                   ComponentLifecycleGeneration(3U), catalog),
          Snapshot("snapshot.before.1", ComponentLifecycleStateDTO::kActive,
                   ComponentLifecycleGeneration(3U), catalog),
          Snapshot("snapshot.under-fence.1", ComponentLifecycleStateDTO::kDisabling,
                   ComponentLifecycleGeneration(4U), catalog)};
}

[[nodiscard]] std::vector<ComponentLifecycleCurrentSnapshotDTO> OrdinaryDisableSnapshots() {
  const auto catalog = PrepareCatalog();
  const auto current = Snapshot("snapshot.before.1",
                                ComponentLifecycleStateDTO::kActive,
                                ComponentLifecycleGeneration(3U), catalog);
  auto accepted = current;
  accepted.component_lifecycle_generation = ComponentLifecycleGeneration(4U);
  const auto accepted_digest = ComputeComponentLifecycleCurrentSnapshotDigest(accepted);
  if (!accepted_digest.ok()) {
    ADD_FAILURE() << accepted_digest.error().code << ": "
                  << accepted_digest.error().message;
    throw std::runtime_error("failed to construct accepted Disable snapshot");
  }
  accepted.snapshot_digest = accepted_digest.value();
  return {current, current, accepted};
}

[[nodiscard]] LifecyclePredicateDTO Predicate(ComponentLifecycleStateDTO state) {
  return {{state}, {}};
}

[[nodiscard]] BrandLifecyclePlanCatalogDTO PrepareCatalog() {
  auto catalog = MakeBrandLifecyclePlanCatalog(
      ProviderManifestId("manifest.1"), ProviderManifestRevision(Sha('1')),
      {{ComponentLifecycleIntentKindDTO::kPrepare,
        ComponentLifecycleStateDTO::kPrepared,
        std::nullopt,
        Predicate(ComponentLifecycleStateDTO::kPrepared),
        1'000'000,
        {{LifecyclePrimitiveId("prepare"), Predicate(ComponentLifecycleStateDTO::kDeclared),
          Predicate(ComponentLifecycleStateDTO::kPrepared)}}},
       {ComponentLifecycleIntentKindDTO::kDisable,
        ComponentLifecycleStateDTO::kDisabled,
        std::nullopt,
        Predicate(ComponentLifecycleStateDTO::kDisabled),
        1'000'000,
        {{LifecyclePrimitiveId("disable"), Predicate(ComponentLifecycleStateDTO::kActive),
          Predicate(ComponentLifecycleStateDTO::kDisabled)}}}});
  if (!catalog.ok()) {
    ADD_FAILURE() << catalog.error().code << ": " << catalog.error().message;
    throw std::runtime_error("failed to construct canonical lifecycle catalog");
  }
  return std::move(catalog.value());
}

[[nodiscard]] LifecycleFactValueDTO TokenFactValue(std::string value) {
  return {LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt, std::move(value)};
}

[[nodiscard]] BrandLifecyclePlanCatalogDTO FactBearingPrepareCatalog() {
  auto plans = PrepareCatalog().plans;
  plans.front().transitions.front().postcondition_predicate.fact_predicates = {
      {LifecycleFactId("mode"), TokenFactValue("automatic")}};
  auto catalog = MakeBrandLifecyclePlanCatalog(
      ProviderManifestId("manifest.1"), ProviderManifestRevision(Sha('1')), std::move(plans));
  if (!catalog.ok()) {
    ADD_FAILURE() << catalog.error().code << ": " << catalog.error().message;
    throw std::runtime_error("failed to construct fact-bearing lifecycle catalog");
  }
  return std::move(catalog.value());
}

[[nodiscard]] BrandLifecyclePlanCatalogDTO DisableCatalog() {
  return PrepareCatalog();
}

[[nodiscard]] ComponentLifecycleIntentDTO PrepareIntent(const RequestOwnerEpoch& owner) {
  return {{RequestId("request.1"), owner, "test", std::nullopt},
          ControllableComponentId("component.1"),
          ComponentLifecycleIntentKindDTO::kPrepare,
          GraphRevision("graph.1"),
          ComponentLifecycleGeneration(3U),
          {ExpectedDomain()},
          std::nullopt,
          {RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified, std::nullopt}};
}

[[nodiscard]] ComponentLifecycleIntentDTO DisableIntent(const RequestOwnerEpoch& owner) {
  return {{RequestId("disable-request.1"), owner, "test", std::nullopt},
          ControllableComponentId("component.1"),
          ComponentLifecycleIntentKindDTO::kDisable,
          GraphRevision("graph.1"),
          ComponentLifecycleGeneration(3U),
          {ExpectedDomain()},
          std::nullopt,
          {RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified, std::nullopt}};
}

struct DisjointComponentFixture final {
  ComponentLifecycleCurrentSnapshotDTO snapshot;
  ComponentLifecycleIntentDTO intent;
};

[[nodiscard]] DisjointComponentFixture DisjointComponent(std::string suffix,
                                                         std::string request_id) {
  const auto catalog = PrepareCatalog();
  auto expected = ExpectedDomain();
  expected.control_domain_id = ControlDomainId("domain." + suffix);
  expected.control_domain_revision = ControlDomainRevision("domain-revision." + suffix);
  expected.controller().controller_topology_id = ControllerTopologyId("topology." + suffix);
  expected.controller().topology_revision = TopologyRevision("topology-revision." + suffix);
  expected.controller().expected_control_bindings.front().control_binding_id =
      ControlBindingId("binding." + suffix);
  expected.controller().expected_control_bindings.front().control_binding_revision =
      ControlBindingRevision("binding-revision." + suffix);
  expected.controller().expected_control_bindings.front().control_route_id = ControlRouteId("route." + suffix);

  ResolvedLifecycleControlDomainScopeDTO current{expected.control_domain_id,
                                                 expected.control_domain_revision,
                                                 expected.controller().controller_topology_id,
                                                 expected.controller().topology_revision,
                                                 std::nullopt,
                                                 expected.controller().expected_control_bindings,
                                                 {ResourceId("resource." + suffix)}};
  ResolvedLifecycleProviderOccurrenceDTO provider{current.control_domain_id,
                                                  current.control_domain_revision,
                                                  ProviderInstanceId("provider." + suffix),
                                                  catalog.provider_manifest_id,
                                                  catalog.provider_manifest_revision,
                                                  RuntimeUnitId("runtime." + suffix),
                                                  RuntimeEpoch("runtime-epoch." + suffix),
                                                  catalog.catalog_digest};
  const ControllableComponentId component_id("component." + suffix);
  ComponentLifecycleCurrentSnapshotDTO snapshot{ContentDigest("snapshot." + suffix),
                                                component_id,
                                                ComponentLifecycleGeneration(3U),
                                                ComponentLifecycleStateDTO::kPrepared,
                                                ActivePublication(),
                                                {current},
                                                {provider}};
  ComponentLifecycleIntentDTO intent{
      {RequestId(std::move(request_id)), RequestOwnerEpoch("cclc-owner.1"), "test", std::nullopt},
      component_id,
      ComponentLifecycleIntentKindDTO::kPrepare,
      GraphRevision("graph.1"),
      ComponentLifecycleGeneration(3U),
      {expected},
      std::nullopt,
      {RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified, std::nullopt}};
  return {std::move(snapshot), std::move(intent)};
}

class FakeSnapshotSource final : public ICCLCCurrentComponentSnapshotSource {
 public:
  explicit FakeSnapshotSource(std::vector<ComponentLifecycleCurrentSnapshotDTO> snapshots,
                              std::size_t initially_unavailable_reads = 0U)
      : snapshots_(std::move(snapshots)),
        initially_unavailable_reads_(initially_unavailable_reads) {}

  [[nodiscard]] Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponentSnapshot(
      const ControllableComponentId& component_id) const override {
    std::lock_guard lock(mutex_);
    ++read_count_;
    if (read_count_ <= initially_unavailable_reads_ || snapshots_.empty() ||
        snapshots_.front().controllable_component_id != component_id) {
      return Result<ComponentLifecycleCurrentSnapshotDTO>::Err(
          ErrorDTO{"component_snapshot_unavailable", "snapshot unavailable", {}});
    }
    const auto index = std::min(read_count_ - 1U, snapshots_.size() - 1U);
    return Result<ComponentLifecycleCurrentSnapshotDTO>::Ok(snapshots_[index]);
  }

  [[nodiscard]] std::size_t read_count() const {
    std::lock_guard lock(mutex_);
    return read_count_;
  }

  void SetSnapshot(ComponentLifecycleCurrentSnapshotDTO snapshot) {
    std::lock_guard lock(mutex_);
    snapshots_ = {std::move(snapshot)};
  }

 private:
  mutable std::mutex mutex_;
  mutable std::size_t read_count_ = 0U;
  std::vector<ComponentLifecycleCurrentSnapshotDTO> snapshots_;
  std::size_t initially_unavailable_reads_ = 0U;
};

class RoutedSnapshotSource final : public ICCLCCurrentComponentSnapshotSource {
 public:
  explicit RoutedSnapshotSource(std::vector<ComponentLifecycleCurrentSnapshotDTO> snapshots)
      : snapshots_(std::move(snapshots)) {}

  [[nodiscard]] Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponentSnapshot(
      const ControllableComponentId& component_id) const override {
    std::lock_guard lock(mutex_);
    const auto snapshot = std::find_if(
        snapshots_.begin(), snapshots_.end(),
        [&](const auto& candidate) { return candidate.controllable_component_id == component_id; });
    if (snapshot == snapshots_.end()) {
      return Result<ComponentLifecycleCurrentSnapshotDTO>::Err(
          ErrorDTO{"component_snapshot_unavailable", "snapshot unavailable", {}});
    }
    return Result<ComponentLifecycleCurrentSnapshotDTO>::Ok(*snapshot);
  }

 private:
  mutable std::mutex mutex_;
  std::vector<ComponentLifecycleCurrentSnapshotDTO> snapshots_;
};

class BlockingRereadSnapshotSource final : public ICCLCCurrentComponentSnapshotSource {
 public:
  explicit BlockingRereadSnapshotSource(ComponentLifecycleCurrentSnapshotDTO snapshot)
      : snapshot_(std::move(snapshot)) {}

  [[nodiscard]] Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponentSnapshot(
      const ControllableComponentId& component_id) const override {
    std::unique_lock lock(mutex_);
    if (snapshot_.controllable_component_id != component_id) {
      return Result<ComponentLifecycleCurrentSnapshotDTO>::Err(
          ErrorDTO{"component_snapshot_unavailable", "snapshot unavailable", {}});
    }
    ++read_count_;
    if (read_count_ == 2U) {
      reread_blocked_ = true;
      changed_.notify_all();
      changed_.wait(lock, [&] { return reread_released_; });
    }
    return Result<ComponentLifecycleCurrentSnapshotDTO>::Ok(snapshot_);
  }

  [[nodiscard]] bool WaitForBlockedReread(std::chrono::nanoseconds timeout) const {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return reread_blocked_; });
  }

  void ReleaseReread() const {
    {
      std::lock_guard lock(mutex_);
      reread_released_ = true;
    }
    changed_.notify_all();
  }

 private:
  ComponentLifecycleCurrentSnapshotDTO snapshot_;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  mutable std::size_t read_count_ = 0U;
  mutable bool reread_blocked_ = false;
  mutable bool reread_released_ = false;
};

enum class CatalogAvailability { kAvailable, kUnavailable, kUnavailableOnce };

class FakeCatalogSource final : public ICCLCLifecyclePlanCatalogSource {
 public:
  explicit FakeCatalogSource(BrandLifecyclePlanCatalogDTO catalog,
                             CatalogAvailability availability = CatalogAvailability::kAvailable)
      : catalog_(std::move(catalog)), availability_(availability) {}

  [[nodiscard]] Result<BrandLifecyclePlanCatalogDTO> LoadExactLifecyclePlanCatalog(
      const ProviderManifestId& manifest_id, const ProviderManifestRevision& manifest_revision,
      const ContentDigest& catalog_digest) const override {
    const auto load_number = load_count_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    if (availability_ == CatalogAvailability::kUnavailable ||
        (availability_ == CatalogAvailability::kUnavailableOnce && load_number == 1U)) {
      return Result<BrandLifecyclePlanCatalogDTO>::Err(
          ErrorDTO{"lifecycle_catalog_unavailable", "catalog unavailable", {}});
    }
    if (catalog_.provider_manifest_id != manifest_id ||
        catalog_.provider_manifest_revision != manifest_revision ||
        catalog_.catalog_digest != catalog_digest) {
      return Result<BrandLifecyclePlanCatalogDTO>::Err(
          ErrorDTO{"lifecycle_catalog_mismatch", "catalog mismatch", {}});
    }
    return Result<BrandLifecyclePlanCatalogDTO>::Ok(catalog_);
  }

  [[nodiscard]] const BrandLifecyclePlanCatalogDTO& catalog() const noexcept { return catalog_; }

  [[nodiscard]] std::size_t load_count() const noexcept {
    return load_count_.load(std::memory_order_acquire);
  }

 private:
  BrandLifecyclePlanCatalogDTO catalog_;
  CatalogAvailability availability_;
  mutable std::atomic<std::size_t> load_count_{0U};
};

// V-B2: what the adapter's transition action reports. kResponseLost is a
// failed action with a typed reason (no CCLC reconciliation); a foreign-domain
// result is a mismatched one.
enum class DispatchMode {
  kKnownSuccess,
  kResponseLost,
  kKnownFailure,
  kForeignDomainFailure,
  kConnectionLost
};

class FakeBrandPort final : public IBrandLifecyclePort {
 public:
  FakeBrandPort(SqliteCCLCJournal& journal, std::vector<ComponentLifecycleStateDTO> observations,
                DispatchMode mode = DispatchMode::kKnownSuccess,
                std::uint64_t first_observation_generation = 1U,
                std::vector<LifecycleFactDTO> facts = {},
                std::function<void()> before_dispatch_result = {},
                std::optional<ErrorDTO> dispatch_error = std::nullopt)
      : journal_(journal),
        observations_(std::move(observations)),
        mode_(mode),
        first_observation_generation_(first_observation_generation),
        facts_(std::move(facts)),
        before_dispatch_result_(std::move(before_dispatch_result)),
        dispatch_error_(std::move(dispatch_error)),
        admission_(std::make_shared<CatalogLifecycleAdmission>(PrepareCatalog())) {}

  [[nodiscard]] Result<BrandLifecycleObservationDTO> Observe(
      const BrandLifecycleObservationQueryDTO& query) override {
    std::lock_guard lock(mutex_);
    const auto index = std::min(observe_count_, observations_.size() - 1U);
    ++observe_count_;
    const auto observation_generation = first_observation_generation_ + observe_count_ - 1U;
    return Result<BrandLifecycleObservationDTO>::Ok(
        {query.lifecycle_transaction_id, query.lifecycle_transaction_generation,
         query.controllable_component_id, query.control_domain_id, query.control_domain_revision,
         query.provider_instance_id, query.provider_manifest_id, query.provider_manifest_revision,
         query.provider_runtime_unit_id, query.provider_runtime_epoch, observation_generation,
         observation_generation, 50, 90, fresh_until_ns_.load(), observations_[index], facts_});
  }

  /// The adapter's own admission: the test's catalog (Prepare by default),
  /// or any other answer the test installs.
  [[nodiscard]] const ILifecycleAdmission& Admission() const override { return *admission_; }

  void set_catalog(const BrandLifecyclePlanCatalogDTO& catalog) {
    admission_ = std::make_shared<CatalogLifecycleAdmission>(catalog);
  }
  void set_admission(std::shared_ptr<const ILifecycleAdmission> admission) {
    admission_ = std::move(admission);
  }
  /// Started/finished events for the one substep go to the feedback sink.
  void set_emit_events(bool emit) noexcept { emit_events_ = emit; }
  /// Without, a succeeded action lists no substep (it found nothing to do).
  void set_report_substeps(bool report) noexcept { report_substeps_ = report; }

  [[nodiscard]] BrandLifecyclePrimitiveResultDTO Dispatch(
      const BrandLifecyclePrimitiveIntentDTO& intent, const LifecycleSubstepFeedbackSink& feedback,
      std::stop_token) override {
    ++dispatch_count_;
    {
      std::lock_guard lock(mutex_);
      last_dispatched_primitive_id_ = intent.lifecycle_primitive_id;
    }
    const auto retained = journal_.LoadRetainedLifecycleStatus(intent.lifecycle_transaction_id);
    saw_prepared_status_.store(
        retained.ok() && retained.value().has_value() &&
            retained.value()->phase == ComponentLifecycleTransactionPhaseDTO::kExecuting &&
            retained.value()->domains.front().primitive_attempts.empty(),
        std::memory_order_release);
    if (before_dispatch_result_) {
      before_dispatch_result_();
    }
    const LifecyclePrimitiveId substep("fake.substep");
    if (emit_events_ && feedback) {
      feedback({intent.lifecycle_primitive_attempt_id, substep, std::nullopt,
                LifecycleSubstepEventDTO::Phase::kStarted, 60, std::nullopt, std::nullopt});
    }
    auto outcome = LifecycleTransitionOutcomeDTO::kSucceeded;
    std::optional<ErrorDTO> error;
    switch (mode_) {
      case DispatchMode::kKnownSuccess:
        break;
      case DispatchMode::kResponseLost:
        outcome = LifecycleTransitionOutcomeDTO::kFailed;
        error = ErrorDTO{"response_lost", "dispatch response lost", {}};
        break;
      case DispatchMode::kKnownFailure:
      case DispatchMode::kForeignDomainFailure:
        outcome = LifecycleTransitionOutcomeDTO::kFailed;
        error = dispatch_error_;
        break;
      case DispatchMode::kConnectionLost:
        outcome = LifecycleTransitionOutcomeDTO::kConnectionLost;
        error = ErrorDTO{"robot_controller_unresponsive", "no answer", {}};
        break;
    }
    if (emit_events_ && feedback) {
      feedback({intent.lifecycle_primitive_attempt_id, substep, std::nullopt,
                LifecycleSubstepEventDTO::Phase::kFinished, 70, outcome,
                error ? std::optional(error->code) : std::nullopt});
    }
    return {intent.lifecycle_transaction_id,
            intent.lifecycle_transaction_generation,
            intent.controllable_component_id,
            mode_ == DispatchMode::kForeignDomainFailure ? ControlDomainId("foreign.domain")
                                                       : intent.control_domain_id,
            intent.control_domain_revision,
            intent.provider_instance_id,
            intent.provider_manifest_id,
            intent.provider_manifest_revision,
            intent.provider_runtime_unit_id,
            intent.provider_runtime_epoch,
            intent.lifecycle_primitive_id,
            intent.lifecycle_primitive_attempt_id,
            intent.expected_observation_generation,
            outcome,
            error,
            report_substeps_ ? std::vector<LifecycleSubstepResultDTO>{
                                   {substep, std::nullopt, outcome, 60, 70, error}}
                             : std::vector<LifecycleSubstepResultDTO>{}};
  }

  [[nodiscard]] std::size_t dispatch_count() const noexcept {
    return dispatch_count_.load(std::memory_order_acquire);
  }

  /// Every later observation stays fresh until this time.
  void set_fresh_until_ns(std::int64_t fresh_until_ns) noexcept {
    fresh_until_ns_.store(fresh_until_ns);
  }

  [[nodiscard]] std::size_t observation_count() const {
    std::lock_guard lock(mutex_);
    return observe_count_;
  }

  [[nodiscard]] bool saw_prepared_status() const noexcept {
    return saw_prepared_status_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::optional<LifecyclePrimitiveId> last_dispatched_primitive_id() const {
    std::lock_guard lock(mutex_);
    return last_dispatched_primitive_id_;
  }

 private:
  SqliteCCLCJournal& journal_;
  std::vector<ComponentLifecycleStateDTO> observations_;
  DispatchMode mode_;
  std::uint64_t first_observation_generation_;
  std::vector<LifecycleFactDTO> facts_;
  std::function<void()> before_dispatch_result_;
  std::optional<ErrorDTO> dispatch_error_;
  std::shared_ptr<const ILifecycleAdmission> admission_;
  bool emit_events_ = false;
  std::atomic<bool> report_substeps_{true};
  mutable std::mutex mutex_;
  std::size_t observe_count_ = 0U;
  std::atomic<std::size_t> dispatch_count_{0U};
  std::atomic<bool> saw_prepared_status_{false};
  std::optional<LifecyclePrimitiveId> last_dispatched_primitive_id_;
  std::atomic<std::int64_t> fresh_until_ns_{2'000'000};
};

class FakeResolver final : public ICCLCBrandLifecyclePortResolver {
 public:
  explicit FakeResolver(std::shared_ptr<IBrandLifecyclePort> port,
                        BrandLifecyclePlanCatalogDTO catalog = PrepareCatalog())
      : port_(std::move(port)), expected_provider_(Provider(catalog)) {}

  [[nodiscard]] Result<std::shared_ptr<IBrandLifecyclePort>> ResolveExactLifecyclePort(
      const ResolvedLifecycleProviderOccurrenceDTO& provider) const override {
    resolve_count_.fetch_add(1U, std::memory_order_acq_rel);
    if (provider != expected_provider_) {
      return Result<std::shared_ptr<IBrandLifecyclePort>>::Err(
          ErrorDTO{"lifecycle_port_mismatch", "provider mismatch", {}});
    }
    return Result<std::shared_ptr<IBrandLifecyclePort>>::Ok(port_);
  }

  [[nodiscard]] std::size_t resolve_count() const noexcept {
    return resolve_count_.load(std::memory_order_acquire);
  }

 private:
  std::shared_ptr<IBrandLifecyclePort> port_;
  ResolvedLifecycleProviderOccurrenceDTO expected_provider_;
  mutable std::atomic<std::size_t> resolve_count_{0U};
};

class RoutedResolver final : public ICCLCBrandLifecyclePortResolver {
 public:
  RoutedResolver(std::shared_ptr<IBrandLifecyclePort> port,
                 std::vector<ResolvedLifecycleProviderOccurrenceDTO> providers)
      : port_(std::move(port)), providers_(std::move(providers)) {}

  [[nodiscard]] Result<std::shared_ptr<IBrandLifecyclePort>> ResolveExactLifecyclePort(
      const ResolvedLifecycleProviderOccurrenceDTO& provider) const override {
    if (std::find(providers_.begin(), providers_.end(), provider) == providers_.end()) {
      return Result<std::shared_ptr<IBrandLifecyclePort>>::Err(
          ErrorDTO{"lifecycle_port_mismatch", "provider mismatch", {}});
    }
    return Result<std::shared_ptr<IBrandLifecyclePort>>::Ok(port_);
  }

 private:
  std::shared_ptr<IBrandLifecyclePort> port_;
  std::vector<ResolvedLifecycleProviderOccurrenceDTO> providers_;
};

class DomainPortResolver final : public ICCLCBrandLifecyclePortResolver {
 public:
  DomainPortResolver(
      ResolvedLifecycleProviderOccurrenceDTO first,
      std::shared_ptr<IBrandLifecyclePort> first_port,
      ResolvedLifecycleProviderOccurrenceDTO second,
      std::shared_ptr<IBrandLifecyclePort> second_port)
      : first_(std::move(first)), first_port_(std::move(first_port)),
        second_(std::move(second)), second_port_(std::move(second_port)) {}

  [[nodiscard]] Result<std::shared_ptr<IBrandLifecyclePort>> ResolveExactLifecyclePort(
      const ResolvedLifecycleProviderOccurrenceDTO& provider) const override {
    if (provider == first_) {
      return Result<std::shared_ptr<IBrandLifecyclePort>>::Ok(first_port_);
    }
    if (provider == second_) {
      return Result<std::shared_ptr<IBrandLifecyclePort>>::Ok(second_port_);
    }
    return Result<std::shared_ptr<IBrandLifecyclePort>>::Err(
        ErrorDTO{"lifecycle_port_mismatch", "provider mismatch", {}});
  }

 private:
  ResolvedLifecycleProviderOccurrenceDTO first_;
  std::shared_ptr<IBrandLifecyclePort> first_port_;
  ResolvedLifecycleProviderOccurrenceDTO second_;
  std::shared_ptr<IBrandLifecyclePort> second_port_;
};

using test::FakeCellAdmissionPort;

class CountingAdmissionReadPort final : public FakeCellAdmissionPort {
 public:
  AuthoringAdmissionDTO GetAuthoringAdmission() const override {
    ++admission_reads;
    return FakeCellAdmissionPort::GetAuthoringAdmission();
  }
  Result<CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent) override {
    ++claim_acquires;
    return FakeCellAdmissionPort::AcquireCellAdmissionClaim(intent);
  }

  mutable std::atomic<std::size_t> admission_reads{0U};
  std::atomic<std::size_t> claim_acquires{0U};
};

class BlockingOwnerReconciliationPort final : public FakeCellAdmissionPort {
 public:
  Result<CellAdmissionRecoveryReceiptDTO> GetCellAdmissionRecoveryReceipt(
      const CellAdmissionRecoveryReceiptQueryDTO& query) const override {
    if (query.intent.owner_transaction_id != "cclc-owner-epoch-probe")
      return FakeCellAdmissionPort::GetCellAdmissionRecoveryReceipt(query);
    std::unique_lock lock(mutex_);
    probe_blocked_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return probe_released_; });
    return Result<CellAdmissionRecoveryReceiptDTO>::Err(
        {"late_owner_receipt_unavailable", "final owner reconciliation failed", {}});
  }

  [[nodiscard]] bool WaitForBlockedProbe(std::chrono::nanoseconds timeout) const {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return probe_blocked_; });
  }

  void ReleaseProbe() const {
    {
      std::lock_guard lock(mutex_);
      probe_released_ = true;
    }
    changed_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  mutable bool probe_blocked_ = false;
  mutable bool probe_released_ = false;
};

class WaitableManualScheduler final : public IScheduler {
 public:
  explicit WaitableManualScheduler(bool fail_drain = false) : fail_drain_(fail_drain) {}

  ScheduleHandle schedule(TimePoint deadline, std::function<void()> callback,
                          SchedulerPriority priority = SchedulerPriority::Lifecycle) override {
    const auto handle = scheduler_.schedule(deadline, std::move(callback), priority);
    {
      std::lock_guard lock(mutex_);
      ++scheduled_count_;
    }
    scheduled_.notify_all();
    return handle;
  }

  void cancel(ScheduleHandle handle) override {
    ++cancel_count_;
    scheduler_.cancel(handle);
  }

  bool cancel_before_dequeue(ScheduleHandle handle) override {
    ++cancel_count_;
    const bool removed = scheduler_.cancel_before_dequeue(handle);
    if (removed) {
      {
        std::lock_guard lock(mutex_);
        ++proven_cancel_count_;
      }
      scheduled_.notify_all();
    }
    return removed;
  }

  [[nodiscard]] bool WaitForProvenCancel(std::chrono::nanoseconds timeout) const {
    std::unique_lock lock(mutex_);
    return scheduled_.wait_for(lock, timeout, [&] { return proven_cancel_count_ > 0U; });
  }

  // A failing drain misses its bound once, then completes.
  bool drain_for(SchedulerPriority priority, std::chrono::nanoseconds timeout) override {
    ++drain_count_;
    return !fail_drain_.exchange(false) && scheduler_.drain_for(priority, timeout);
  }

  [[nodiscard]] std::size_t drain_count() const noexcept { return drain_count_.load(); }

  [[nodiscard]] bool WaitForScheduled(std::size_t count, std::chrono::nanoseconds timeout) const {
    std::unique_lock lock(mutex_);
    return scheduled_.wait_for(lock, timeout, [&] { return scheduled_count_ >= count; });
  }

  void AdvanceTo(TimePoint time) { scheduler_.advance_to(time); }

  [[nodiscard]] SchedulerPriority last_priority() const { return scheduler_.last_priority(); }

  [[nodiscard]] std::size_t pending_count(SchedulerPriority priority) const {
    return scheduler_.pending_count(priority);
  }

  [[nodiscard]] std::size_t cancel_count() const noexcept { return cancel_count_.load(); }

 private:
  ManualScheduler scheduler_;
  mutable std::mutex mutex_;
  mutable std::condition_variable scheduled_;
  std::size_t scheduled_count_ = 0U;
  std::size_t proven_cancel_count_ = 0U;
  std::atomic<std::size_t> cancel_count_{0U};
  std::atomic<std::size_t> drain_count_{0U};
  std::atomic<bool> fail_drain_{false};
};

// Loses every callback due before `keep_from` (the observation polls), so
// only the independent lifecycle deadline can wake a waiting transaction.
class PollLosingScheduler final : public IScheduler {
 public:
  explicit PollLosingScheduler(TimePoint keep_from) : keep_from_(keep_from) {}

  ScheduleHandle schedule(TimePoint deadline, std::function<void()> callback,
                          SchedulerPriority priority = SchedulerPriority::Lifecycle) override {
    if (deadline < keep_from_) {
      {
        std::lock_guard lock(mutex_);
        ++lost_count_;
      }
      lost_.notify_all();
      // A lost poll is never delivered: it stays queued (never dequeued), so
      // DrainFor can still prove its removal with cancel_before_dequeue.
      return scheduler_.schedule(std::numeric_limits<TimePoint>::max(), [] {}, priority);
    }
    return scheduler_.schedule(deadline, std::move(callback), priority);
  }

  void cancel(ScheduleHandle handle) override { scheduler_.cancel(handle); }

  bool cancel_before_dequeue(ScheduleHandle handle) override {
    return scheduler_.cancel_before_dequeue(handle);
  }

  [[nodiscard]] bool WaitForLost(std::size_t count, std::chrono::nanoseconds timeout) const {
    std::unique_lock lock(mutex_);
    return lost_.wait_for(lock, timeout, [&] { return lost_count_ >= count; });
  }

  void AdvanceTo(TimePoint time) { scheduler_.advance_to(time); }

 private:
  ManualScheduler scheduler_;
  const TimePoint keep_from_;
  mutable std::mutex mutex_;
  mutable std::condition_variable lost_;
  std::size_t lost_count_ = 0U;
};

// Holds a dequeued callback due at or after `pause_from` (the lifecycle
// deadline) inside the scheduler until Release(), like a scheduler worker that
// has dequeued the callback but not yet returned from it.
class PausingDeadlineScheduler final : public IScheduler {
 public:
  explicit PausingDeadlineScheduler(TimePoint pause_from) : pause_from_(pause_from) {}

  ScheduleHandle schedule(TimePoint deadline, std::function<void()> callback,
                          SchedulerPriority priority = SchedulerPriority::Lifecycle) override {
    if (deadline >= pause_from_) {
      callback = [this, inner = std::move(callback)] {
        {
          std::unique_lock lock(mutex_);
          paused_ = true;
          in_flight_ = true;
          changed_.notify_all();
          changed_.wait(lock, [&] { return released_; });
        }
        inner();
        {
          std::lock_guard lock(mutex_);
          in_flight_ = false;
        }
        changed_.notify_all();
      };
    }
    const auto handle = scheduler_.schedule(deadline, std::move(callback), priority);
    {
      std::lock_guard lock(mutex_);
      ++scheduled_count_;
    }
    changed_.notify_all();
    return handle;
  }

  void cancel(ScheduleHandle handle) override { scheduler_.cancel(handle); }

  bool cancel_before_dequeue(ScheduleHandle handle) override {
    const bool removed = scheduler_.cancel_before_dequeue(handle);
    {
      std::lock_guard lock(mutex_);
      cancel_attempted_ = true;
    }
    changed_.notify_all();
    return removed;
  }

  // A held callback is a stuck lane: the bounded drain fails at once.
  bool drain_for(SchedulerPriority, std::chrono::nanoseconds) override {
    std::lock_guard lock(mutex_);
    drain_attempted_ = true;
    return !in_flight_;
  }

  [[nodiscard]] bool drain_attempted() {
    std::lock_guard lock(mutex_);
    return drain_attempted_;
  }

  [[nodiscard]] bool WaitForCancelAttempt(std::chrono::nanoseconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return cancel_attempted_; });
  }

  [[nodiscard]] bool WaitForScheduled(std::size_t count, std::chrono::nanoseconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return scheduled_count_ >= count; });
  }

  [[nodiscard]] bool WaitForPaused(std::chrono::nanoseconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return paused_; });
  }

  void Release() {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    changed_.notify_all();
  }

  void AdvanceTo(TimePoint time) { scheduler_.advance_to(time); }

 private:
  ManualScheduler scheduler_;
  const TimePoint pause_from_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t scheduled_count_ = 0U;
  bool paused_ = false;
  bool released_ = false;
  bool in_flight_ = false;
  bool cancel_attempted_ = false;
  bool drain_attempted_ = false;
};

[[nodiscard]] ComponentLifecycleCoordinatorConfig Config() {
  return {RequestOwnerEpoch("cclc-owner.1"), 8U, 2U, 1'000};
}

[[nodiscard]] ComponentLifecycleAdmissionCandidate RecoveryAdmissionCandidate(
    std::string transaction_id, std::optional<ComponentLifecycleCurrentSnapshotDTO> snapshot,
    std::vector<BrandLifecyclePlanCatalogDTO> catalogs) {
  return {LifecycleTransactionId(std::move(transaction_id)),
          LifecycleTransactionGeneration(1U),
          ComponentLifecycleGeneration(3U),
          100,
          std::move(snapshot),
          std::move(catalogs)};
}

struct PreparedBrandRecoverySeed final {
  ComponentLifecycleAcceptedDTO acceptance;
  BrandLifecyclePrimitiveIntentDTO primitive;
  CellAdmissionClaimDTO claim;
};

[[nodiscard]] BrandLifecyclePrimitiveIntentDTO RecoveryPrimitiveIntent(
    const ComponentLifecycleStatusDTO& status) {
  const auto& domain = status.domains.front();
  const auto& provider = *domain.resolved_provider_occurrence;
  return {status.acceptance.lifecycle_transaction_id,
          status.acceptance.lifecycle_transaction_generation,
          status.accepted_intent.controllable_component_id,
          domain.control_domain_id,
          provider.control_domain_revision,
          provider.provider_instance_id,
          provider.provider_manifest_id,
          provider.provider_manifest_revision,
          provider.provider_runtime_unit_id,
          provider.provider_runtime_epoch,
          LifecycleTransitionActionId(status.accepted_intent.intent),
          LifecyclePrimitiveAttemptId("recovery-attempt.1"),
          1U,
          status.remaining_duration_ns,
          *status.requested_state.state};
}

[[nodiscard]] CCLCEffectAttemptDTO PreparedPrimitiveAttempt(
    const BrandLifecyclePrimitiveIntentDTO& intent) {
  return {CCLCRequestKindDTO::kComponentLifecycle,
          intent.lifecycle_transaction_id,
          intent.lifecycle_transaction_generation,
          1U,
          CCLCEffectKindDTO::kBrandLifecyclePrimitive,
          CCLCEffectAttemptStateDTO::kPrepared,
          intent,
          std::nullopt};
}

[[nodiscard]] PreparedBrandRecoverySeed SeedPreparedBrandRecovery(
    SqliteCCLCJournal& journal, const RequestOwnerEpoch& owner,
    const BrandLifecyclePlanCatalogDTO& catalog, std::string transaction_id) {
  const auto intent = PrepareIntent(owner);
  const auto snapshot = Snapshot("snapshot.1", ComponentLifecycleStateDTO::kDeclared,
                                 ComponentLifecycleGeneration(3U), catalog);
  const auto prepared = PrepareComponentLifecycleAdmission(
      intent,
      RecoveryAdmissionCandidate(std::move(transaction_id), std::move(snapshot), {catalog}));
  if (!prepared.ok()) {
    throw std::runtime_error(prepared.error().code + ": " + prepared.error().message);
  }
  auto claim = SeedLifecycleClaim(journal, owner, intent, prepared.value().acceptance);
  if (!claim.ok()) {
    throw std::runtime_error(claim.error().code + ": " + claim.error().message);
  }
  const auto accepted = journal.AcceptOrReplayLifecycle(owner, intent, prepared.value().acceptance,
                                                        prepared.value().initial_status);
  if (!accepted.ok()) {
    throw std::runtime_error(accepted.error().code + ": " + accepted.error().message);
  }
  const auto primitive = RecoveryPrimitiveIntent(prepared.value().initial_status);
  auto prepared_status = prepared.value().initial_status;
  ++prepared_status.status_sequence;
  prepared_status.phase = ComponentLifecycleTransactionPhaseDTO::kExecuting;
  prepared_status.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kExecuting;
  prepared_status.latest_updated_at_ns += 10;
  prepared_status.remaining_duration_ns -= 10;
  const auto retained =
      journal.RecordEffectPrepared(owner, PreparedPrimitiveAttempt(primitive), prepared_status);
  if (!retained.ok()) {
    throw std::runtime_error(retained.error().code + ": " + retained.error().message);
  }
  return {prepared.value().acceptance, primitive, claim.value()};
}

TEST(ComponentLifecycleCoordinator, QuiescentPredecessorConfigurationUsesExistingIdentityBounds) {
  auto config = Config();
  EXPECT_FALSE(config.quiescent_predecessor);
  EXPECT_TRUE(config.locally_valid());
  config.quiescent_predecessor = RequestOwnerEpoch("predecessor.1");
  EXPECT_TRUE(config.locally_valid());
  config.quiescent_predecessor = RequestOwnerEpoch("");
  EXPECT_FALSE(config.locally_valid());
  config.quiescent_predecessor =
      RequestOwnerEpoch(std::string(kMaxLifecycleIdentityBytes + 1U, 'p'));
  EXPECT_FALSE(config.locally_valid());
}

TEST(ComponentLifecycleCoordinator, InitialStartRequiresTheExactQuiescentPredecessor) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  const auto original = journal.BeginOwnerEpoch(RequestOwnerEpoch("original.1"), 100, 8U);
  ASSERT_TRUE(original.ok()) << original.error().message;
  const auto original_owner = original.value().request_owner_epoch;
  const auto intent = PrepareIntent(original_owner);
  const ErrorDTO rejection{"not_qualified", "original request did not start effects",
                           {{"effect", "not_started"}}};
  ASSERT_TRUE(journal.RecordLifecycleRejection(original_owner, intent, rejection, std::nullopt).ok());

  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(port);
  CountingAdmissionReadPort kernel;
  FakeClock clock;
  clock.set(200);
  ManualScheduler scheduler;
  {
    auto config = Config();
    config.quiescent_predecessor = RequestOwnerEpoch("foreign.predecessor");
    ComponentLifecycleCoordinator coordinator(config, journal, reservations, snapshots, catalogs,
                                              resolver, kernel, clock, scheduler);
    EXPECT_FALSE(coordinator.admission_fault_closed());
    const auto started = coordinator.Start();
    ASSERT_FALSE(started.ok());
    EXPECT_EQ(started.error().code, "component_lifecycle_checkpoint_not_quiescent");
    EXPECT_FALSE(coordinator.accepting_new_requests());
    // A failed initial Start does not set the existing sticky runtime flag.
    EXPECT_FALSE(coordinator.admission_fault_closed());
    coordinator.CloseAdmission();
    EXPECT_TRUE(coordinator.admission_fault_closed());
    EXPECT_TRUE(coordinator.admission_fault_closed());
  }
  {
    auto config = Config();
    config.quiescent_predecessor = original_owner;
    ComponentLifecycleCoordinator coordinator(config, journal, reservations, snapshots, catalogs,
                                              resolver, kernel, clock, scheduler);
    ASSERT_TRUE(coordinator.Start().ok());
    EXPECT_NE(coordinator.request_owner_epoch(), original_owner);
    EXPECT_TRUE(coordinator.accepting_new_requests());
    EXPECT_FALSE(coordinator.admission_fault_closed());
    const auto retained = coordinator.ReadLifecycleRequestResult(intent);
    ASSERT_TRUE(retained.ok());
    ASSERT_TRUE(retained.value());
    EXPECT_EQ(retained.value()->replay_status, RequestReplayStatusDTO::kReplayedExact);
    EXPECT_EQ(retained.value()->rejection, rejection);
    EXPECT_FALSE(coordinator.admission_fault_closed());
  }
  EXPECT_EQ(snapshots.read_count(), 0U);
  EXPECT_EQ(catalogs.load_count(), 0U);
  EXPECT_EQ(resolver.resolve_count(), 0U);
  EXPECT_EQ(kernel.claim_acquires.load(), 0U);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

TEST(ComponentLifecycleCoordinator, PreconditionRunsAfterExactReplayAndItsRefusalIsRetained) {
  // M6 (SX-507): a Recover without incident acknowledgement checks the
  // incident/stream hold inside admission, after exact replay.
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());

  int calls = 0;
  const ComponentLifecycleCoordinator::LifecycleAdmissionPrecondition refuse = [&] {
    ++calls;
    ErrorDTO error{"cclc_incident_open", "a component incident is open", {}};
    error.details["reason"] = "incident_open";
    return Result<void>::Err(std::move(error));
  };
  const ComponentLifecycleCoordinator::LifecycleAdmissionPrecondition allow = [&] {
    ++calls;
    return Result<void>::Ok();
  };

  // A refusal: no effect, retained as this request's result.
  auto refused_intent = PrepareIntent(coordinator.request_owner_epoch());
  refused_intent.request_context.request_id = RequestId("request.refused");
  const auto refused = coordinator.SubmitLifecycle(refused_intent, refuse);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "cclc_incident_open");
  EXPECT_EQ(refused.error().details.at("effect"), "not_started");
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_TRUE(coordinator.accepting_new_requests());
  // Its exact retry replays the refusal; the precondition is not consulted.
  const auto refused_again = coordinator.SubmitLifecycle(refused_intent, allow);
  ASSERT_FALSE(refused_again.ok());
  EXPECT_EQ(refused_again.error().code, "cclc_incident_open");
  EXPECT_EQ(calls, 1);

  // An accepted request, then an incident: its exact retry replays the
  // acceptance instead of being refused.
  const auto intent = PrepareIntent(coordinator.request_owner_epoch());
  const auto accepted = coordinator.SubmitLifecycle(intent, allow);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(accepted.value().replay_status, RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_EQ(calls, 2);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto replay = coordinator.SubmitLifecycle(intent, refuse);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  // The replayed acceptance is the original one with its own replay status.
  auto replayed_acceptance = accepted.value().acceptance;
  ASSERT_TRUE(replayed_acceptance);
  replayed_acceptance->replay_status = RequestReplayStatusDTO::kReplayedExact;
  EXPECT_EQ(replay.value().acceptance, replayed_acceptance);
  EXPECT_EQ(calls, 2);
}

TEST(ComponentLifecycleCoordinator, PersistsBeforeDispatchAndReconcilesLostReleaseReply) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  kernel.LoseNextReleaseReply();
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));

  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kSucceeded);
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_TRUE(port->saw_prepared_status());
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_TRUE(coordinator.accepting_new_requests());
}

// V2-4: the second domain's failed action fails the transaction with its own
// typed reason; the succeeded domain keeps its attempt; current status settles.
TEST(ComponentLifecycleCoordinator, MultiDomainPrepareFailureRetainsTheSuccessfulDomain) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  const auto catalog = PrepareCatalog();
  auto snapshot = Snapshot("snapshot.two-domains", ComponentLifecycleStateDTO::kDeclared,
                           ComponentLifecycleGeneration(3U), catalog);
  auto second = CurrentDomain();
  second.control_domain_id = ControlDomainId("domain.2");
  second.control_domain_revision = ControlDomainRevision("domain-revision.2");
  second.controller().controller_topology_id = ControllerTopologyId("topology.2");
  second.controller().topology_revision = TopologyRevision("topology-revision.2");
  second.controller().current_control_bindings.front().control_binding_id = ControlBindingId("binding.2");
  second.controller().current_control_bindings.front().control_binding_revision =
      ControlBindingRevision("binding-revision.2");
  second.controller().current_control_bindings.front().control_route_id = ControlRouteId("route.2");
  second.controller().current_resources = {ResourceId("resource.2")};
  auto second_provider = Provider(catalog);
  second_provider.control_domain_id = second.control_domain_id;
  second_provider.control_domain_revision = second.control_domain_revision;
  second_provider.provider_instance_id = ProviderInstanceId("provider.2");
  snapshot.domains.push_back(second);
  snapshot.provider_occurrences.push_back(second_provider);
  auto intent = PrepareIntent(RequestOwnerEpoch("cclc-owner.1"));
  auto expected_second = ExpectedDomain();
  expected_second.control_domain_id = second.control_domain_id;
  expected_second.control_domain_revision = second.control_domain_revision;
  expected_second.controller().controller_topology_id = second.controller().controller_topology_id;
  expected_second.controller().topology_revision = second.controller().topology_revision;
  expected_second.controller().expected_control_bindings = second.controller().current_control_bindings;
  intent.expected_control_domains.push_back(expected_second);

  FakeSnapshotSource snapshots({snapshot});
  FakeCatalogSource catalogs(catalog);
  auto first_port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  auto second_port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared},
      DispatchMode::kResponseLost);
  DomainPortResolver resolver(snapshot.provider_occurrences.front(), first_port,
                              second_provider, second_port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  intent.request_context.request_owner_epoch = coordinator.request_owner_epoch();
  const auto accepted = coordinator.SubmitLifecycle(intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance.has_value());
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status = coordinator.LoadLifecycleStatus(
      accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  ASSERT_EQ(status.value()->domains.size(), 2U);
  EXPECT_EQ(first_port->dispatch_count(), 1U);
  EXPECT_EQ(second_port->dispatch_count(), 1U);
  EXPECT_FALSE(status.value()->domains.front().error);
  ASSERT_EQ(status.value()->domains.front().primitive_attempts.size(), 1U);
  EXPECT_EQ(status.value()->domains.front().primitive_attempts.front().outcome,
            LifecycleTransitionOutcomeDTO::kSucceeded);
  ASSERT_TRUE(status.value()->error);
  EXPECT_EQ(*status.value()->error, (ErrorDTO{"response_lost", "dispatch response lost", {}}));
  // VB2-O1: the second domain's fresh resting observation is current status.
  EXPECT_EQ(status.value()->domains.back().current_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_EQ(status.value()->current_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_TRUE(reservations.snapshot().empty());
}

// C12: a refusal before any effect names each domain's own cause; a domain
// with no refusal of its own names the refusing domain, not its error.
TEST(ComponentLifecycleCoordinator, MultiDomainRefusalReportsEachDomainsOwnError) {
  struct Case {
    ComponentLifecycleStateDTO first;
    ComponentLifecycleStateDTO second;
  };
  for (const auto [first_state, second_state] :
       {Case{ComponentLifecycleStateDTO::kDeclared, ComponentLifecycleStateDTO::kActive},
        Case{ComponentLifecycleStateDTO::kFaulted, ComponentLifecycleStateDTO::kActive}}) {
    SCOPED_TRACE(std::string(to_string(first_state)));
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename),
                              CellId("cell.1"), 8U);
    CCLCDomainReservationTable reservations;
    const auto catalog = PrepareCatalog();
    auto snapshot = Snapshot("snapshot.two-domains", ComponentLifecycleStateDTO::kDeclared,
                             ComponentLifecycleGeneration(3U), catalog);
    auto second = CurrentDomain();
    second.control_domain_id = ControlDomainId("domain.2");
    second.control_domain_revision = ControlDomainRevision("domain-revision.2");
    second.controller().controller_topology_id = ControllerTopologyId("topology.2");
    second.controller().topology_revision = TopologyRevision("topology-revision.2");
    second.controller().current_control_bindings.front().control_binding_id =
        ControlBindingId("binding.2");
    second.controller().current_control_bindings.front().control_binding_revision =
        ControlBindingRevision("binding-revision.2");
    second.controller().current_control_bindings.front().control_route_id =
        ControlRouteId("route.2");
    second.controller().current_resources = {ResourceId("resource.2")};
    auto second_provider = Provider(catalog);
    second_provider.control_domain_id = second.control_domain_id;
    second_provider.control_domain_revision = second.control_domain_revision;
    second_provider.provider_instance_id = ProviderInstanceId("provider.2");
    snapshot.domains.push_back(second);
    snapshot.provider_occurrences.push_back(second_provider);
    auto intent = PrepareIntent(RequestOwnerEpoch("cclc-owner.1"));
    auto expected_second = ExpectedDomain();
    expected_second.control_domain_id = second.control_domain_id;
    expected_second.control_domain_revision = second.control_domain_revision;
    expected_second.controller().controller_topology_id =
        second.controller().controller_topology_id;
    expected_second.controller().topology_revision = second.controller().topology_revision;
    expected_second.controller().expected_control_bindings =
        second.controller().current_control_bindings;
    intent.expected_control_domains.push_back(expected_second);

    FakeSnapshotSource snapshots({snapshot});
    FakeCatalogSource catalogs(catalog);
    auto first_port = std::make_shared<FakeBrandPort>(
        journal, std::vector<ComponentLifecycleStateDTO>{first_state});
    auto second_port = std::make_shared<FakeBrandPort>(
        journal, std::vector<ComponentLifecycleStateDTO>{second_state});
    DomainPortResolver resolver(snapshot.provider_occurrences.front(), first_port,
                                second_provider, second_port);
    FakeCellAdmissionPort kernel;
    FakeClock clock;
    clock.set(100);
    ManualScheduler scheduler;
    ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots,
                                              catalogs, resolver, kernel, clock, scheduler);

    ASSERT_TRUE(coordinator.Start().ok());
    intent.request_context.request_owner_epoch = coordinator.request_owner_epoch();
    const auto accepted = coordinator.SubmitLifecycle(intent);
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    ASSERT_TRUE(accepted.value().acceptance.has_value());
    ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
    const auto status = coordinator.LoadLifecycleStatus(
        accepted.value().acceptance->lifecycle_transaction_id);
    ASSERT_TRUE(status.ok());
    ASSERT_TRUE(status.value().has_value());
    EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
    EXPECT_EQ(first_port->dispatch_count(), 0U);
    EXPECT_EQ(second_port->dispatch_count(), 0U);
    ASSERT_EQ(status.value()->domains.size(), 2U);
    const auto& first_error = status.value()->domains.front().error;
    const auto& second_error = status.value()->domains.back().error;
    ASSERT_TRUE(first_error);
    ASSERT_TRUE(second_error);
    // The refusing second domain carries its own cause.
    EXPECT_EQ(second_error->code, "component_lifecycle_transition_missing");
    EXPECT_EQ(second_error->details.at("observed_state"), "active");
    EXPECT_EQ(second_error->details.at("decisive_fact"), "state=active");
    if (first_state == ComponentLifecycleStateDTO::kDeclared) {
      // The eligible first domain names the refusing domain only.
      EXPECT_EQ(first_error->code, "component_lifecycle_other_domain_refused");
      EXPECT_EQ(first_error->details.at("refused_control_domain_id"), "domain.2");
      EXPECT_EQ(status.value()->error, second_error);
    } else {
      // Both refuse: each keeps its own cause; the transaction names the first.
      EXPECT_EQ(first_error->code, "component_lifecycle_transition_missing");
      EXPECT_EQ(first_error->details.at("observed_state"), "faulted");
      EXPECT_EQ(first_error->details.at("decisive_fact"), "state=faulted");
      EXPECT_EQ(status.value()->error, first_error);
    }
    EXPECT_TRUE(reservations.snapshot().empty());
  }
}

/// Records the coordinator's Kernel readiness calls in order.
class FakeReadinessPort final : public ICCLCComponentCommandReadinessPort {
 public:
  [[nodiscard]] Result<ComponentCommandReadinessDTO> CloseComponentCommandEligibility(
      const LifecycleTransactionId&, const ControllableComponentId& component) override {
    std::lock_guard lock(mutex_);
    calls_.push_back("close");
    state_ = {component, state_.component_lifecycle_generation, false};
    return Result<ComponentCommandReadinessDTO>::Ok(state_);
  }

  [[nodiscard]] Result<ComponentCommandReadinessDTO> PublishComponentCommandReadiness(
      const ComponentCommandReadinessPublicationDTO& publication) override {
    std::lock_guard lock(mutex_);
    calls_.push_back(publication.command_eligible ? "publish_open" : "publish_closed");
    publications_.push_back(publication);
    if (failures_remaining_ > 0U) {
      --failures_remaining_;
      return Result<ComponentCommandReadinessDTO>::Err(
          {"invalid_state_transition", "Kernel readback unavailable",
           {{"reason", "injected_readiness_failure"}}});
    }
    state_ = {publication.controllable_component_id, publication.component_lifecycle_generation,
              publication.command_eligible};
    return Result<ComponentCommandReadinessDTO>::Ok(state_);
  }

  Result<ComponentCommandReadinessDTO> PublishComponentCommandReadiness(
      const ComponentCommandReadinessPublicationDTO& publication,
      const ComponentLifecycleCurrentSnapshotDTO& snapshot,
      const std::vector<BrandLifecycleObservationDTO>& observations) override {
    {
      std::lock_guard lock(mutex_);
      provider_proof_domain_count_ = snapshot.domains.size();
      provider_proof_observations_ = observations;
    }
    return PublishComponentCommandReadiness(publication);
  }
  std::pair<std::size_t, std::vector<BrandLifecycleObservationDTO>> provider_proof() const {
    std::lock_guard lock(mutex_);
    return {provider_proof_domain_count_, provider_proof_observations_};
  }

  [[nodiscard]] Result<ComponentCommandReadinessDTO> ReadComponentCommandReadiness(
      const ControllableComponentId&) const override {
    std::lock_guard lock(mutex_);
    return Result<ComponentCommandReadinessDTO>::Ok(state_);
  }

  void FailNextPublications(std::size_t count) {
    std::lock_guard lock(mutex_);
    failures_remaining_ = count;
  }
  void ApplyPreparedClose() noexcept {
    std::lock_guard lock(mutex_);
    state_.command_eligible = false;
  }
  [[nodiscard]] std::vector<std::string> calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
  }
  [[nodiscard]] std::vector<ComponentCommandReadinessPublicationDTO> publications() const {
    std::lock_guard lock(mutex_);
    return publications_;
  }
  [[nodiscard]] ComponentCommandReadinessDTO state() const {
    std::lock_guard lock(mutex_);
    return state_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> calls_;
  std::vector<ComponentCommandReadinessPublicationDTO> publications_;
  ComponentCommandReadinessDTO state_{ControllableComponentId("component.1"), std::nullopt, true};
  std::size_t provider_proof_domain_count_{};
  std::vector<BrandLifecycleObservationDTO> provider_proof_observations_;
  std::size_t failures_remaining_{};
};

[[nodiscard]] BrandLifecyclePlanCatalogDTO EnableCatalog() {
  auto plans = PrepareCatalog().plans;
  plans.push_back({ComponentLifecycleIntentKindDTO::kEnable,
                   ComponentLifecycleStateDTO::kActive,
                   std::nullopt,
                   Predicate(ComponentLifecycleStateDTO::kActive),
                   1'000'000,
                   {{LifecyclePrimitiveId("enable"), Predicate(ComponentLifecycleStateDTO::kPrepared),
                     Predicate(ComponentLifecycleStateDTO::kActive)}}});
  std::sort(plans.begin(), plans.end(), [](const auto& lhs, const auto& rhs) {
    return static_cast<int>(lhs.intent) < static_cast<int>(rhs.intent);
  });
  auto catalog = MakeBrandLifecyclePlanCatalog(
      ProviderManifestId("manifest.1"), ProviderManifestRevision(Sha('1')), std::move(plans));
  if (!catalog.ok()) {
    ADD_FAILURE() << catalog.error().code << ": " << catalog.error().message;
    throw std::runtime_error("failed to construct enable lifecycle catalog");
  }
  return std::move(catalog.value());
}

struct EnableRun final {
  std::optional<ComponentLifecycleStatusDTO> status;
  std::vector<std::string> calls;
  std::vector<ComponentCommandReadinessPublicationDTO> publications;
  ComponentLifecycleGeneration accepted_generation{1U};
  bool accepting_after{};
};

[[nodiscard]] EnableRun RunEnable(std::size_t failing_publications) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  const auto catalog = EnableCatalog();
  FakeSnapshotSource snapshots({Snapshot("snapshot.enable", ComponentLifecycleStateDTO::kPrepared,
                                         ComponentLifecycleGeneration(3U), catalog)});
  FakeCatalogSource catalogs(catalog);
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared,
                                                       ComponentLifecycleStateDTO::kActive});
  port->set_catalog(catalog);
  FakeResolver resolver(port, catalog);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  FakeReadinessPort readiness;
  readiness.FailNextPublications(failing_publications);
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler, std::nullopt, &readiness);
  EnableRun run;
  if (!coordinator.Start().ok()) return run;
  auto intent = PrepareIntent(coordinator.request_owner_epoch());
  intent.intent = ComponentLifecycleIntentKindDTO::kEnable;
  const auto accepted = coordinator.SubmitLifecycle(intent);
  if (!accepted.ok() || !accepted.value().acceptance.has_value()) return run;
  static_cast<void>(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  if (status.ok() && status.value().has_value()) run.status = *status.value();
  run.calls = readiness.calls();
  run.publications = readiness.publications();
  run.accepted_generation = accepted.value().acceptance->component_lifecycle_generation;
  run.accepting_after = coordinator.accepting_new_requests();
  return run;
}

TEST(ComponentLifecycleCoordinator, EnableOpensEligibilityOnlyAfterVerifiedCompletion) {
  const auto run = RunEnable(0U);
  ASSERT_TRUE(run.status.has_value());
  EXPECT_EQ(run.status->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded)
      << (run.status->error ? run.status->error->code + ": " + run.status->error->message + " " +
                                  (run.status->error->details.contains("reason")
                                       ? run.status->error->details.at("reason")
                                       : std::string{})
                            : std::string{});
  // Closed before any adapter work; opened once, at the accepted generation.
  EXPECT_EQ(run.calls, (std::vector<std::string>{"close", "publish_open"}));
  ASSERT_EQ(run.publications.size(), 1U);
  EXPECT_EQ(run.publications.front().authority, ComponentReadinessAuthorityDTO::kLifecycleClaim);
  EXPECT_EQ(run.publications.front().component_lifecycle_generation, run.accepted_generation);
  EXPECT_TRUE(run.accepting_after);
}

class IoRecoveryJournalSnapshotSource final : public ICCLCCurrentComponentSnapshotSource {
 public:
  IoRecoveryJournalSnapshotSource(SqliteCCLCJournal& journal,
                                  ComponentLifecycleCurrentSnapshotDTO installed)
      : journal_(journal), installed_(std::move(installed)) {}
  Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponentSnapshot(
      const ControllableComponentId& component) const override {
    if (component != installed_.controllable_component_id)
      return Result<ComponentLifecycleCurrentSnapshotDTO>::Err(
          {"snapshot_unavailable", "Foreign component", {}});
    auto result = installed_;
    const auto actual = journal_.LoadCurrentComponentLifecycleFact(component);
    if (!actual.ok()) return Result<ComponentLifecycleCurrentSnapshotDTO>::Err(actual.error());
    if (actual.value().durable_generation) {
      if (!actual.value().current_state)
        return Result<ComponentLifecycleCurrentSnapshotDTO>::Err(
            {"snapshot_unavailable", "Current journal state is unknown", {}});
      result.component_lifecycle_generation = *actual.value().durable_generation;
      result.component_lifecycle_state = *actual.value().current_state;
    }
    const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(result);
    if (!digest.ok()) return Result<ComponentLifecycleCurrentSnapshotDTO>::Err(digest.error());
    result.snapshot_digest = digest.value();
    return Result<ComponentLifecycleCurrentSnapshotDTO>::Ok(std::move(result));
  }

 private:
  SqliteCCLCJournal& journal_;
  ComponentLifecycleCurrentSnapshotDTO installed_;
};

class IoRecoveryTimedBrandPort final : public IBrandLifecyclePort {
 public:
  IoRecoveryTimedBrandPort(std::shared_ptr<FakeBrandPort> port, IClock& clock)
      : port_(std::move(port)), clock_(clock) {}
  Result<BrandLifecycleObservationDTO> Observe(
      const BrandLifecycleObservationQueryDTO& query) override {
    auto actual = port_->Observe(query);
    if (actual.ok()) {
      actual.value().source_timestamp_ns = clock_.now();
      actual.value().received_timestamp_ns = clock_.now();
      actual.value().fresh_until_ns = clock_.now() + 1'000'000;
    }
    return actual;
  }
  const ILifecycleAdmission& Admission() const override { return port_->Admission(); }
  BrandLifecyclePrimitiveResultDTO Dispatch(const BrandLifecyclePrimitiveIntentDTO& intent,
                                            const LifecycleSubstepFeedbackSink& feedback,
                                            std::stop_token cancellation) override {
    return port_->Dispatch(intent, feedback, cancellation);
  }

 private:
  std::shared_ptr<FakeBrandPort> port_;
  IClock& clock_;
};

class IoRecoveryPendingOutput final : public IIoOutputProvider {
 public:
  explicit IoRecoveryPendingOutput(IClock& clock) : clock_(clock) {}
  IoOutputEvidence StrongestOutputEvidence() const noexcept override {
    return IoOutputEvidence::kSourceAccepted;
  }
  Result<IoOutputObservationDTO> SubmitOutput(const IoOutputWriteRequestDTO& request) override {
    if (request_) {
      if (*request_ == request) return Result<IoOutputObservationDTO>::Ok(*observation_);
      return Result<IoOutputObservationDTO>::Err({"busy", "Existing correlation retained", {}});
    }
    ++submissions;
    request_ = request;
    observation_ =
        IoOutputObservationDTO{request.identity, IoOutputEvidence::kSubmitted, clock_.now(), {}};
    return Result<IoOutputObservationDTO>::Ok(*observation_);
  }
  Result<IoOutputObservationDTO> ObserveOutputResult(
      const IoOutputCommandIdentityDTO& identity) const override {
    if (!request_ || request_->identity != identity)
      return Result<IoOutputObservationDTO>::Err({"foreign", "No exact correlation", {}});
    return Result<IoOutputObservationDTO>::Ok(*observation_);
  }
  Result<void> RetireOutput(const IoOutputCommandIdentityDTO& identity) override {
    if (!request_ || request_->identity != identity)
      return Result<void>::Err({"foreign", "No exact correlation", {}});
    ++retirements;
    request_.reset();
    observation_.reset();
    return Result<void>::Ok();
  }
  unsigned submissions{};
  unsigned retirements{};

 private:
  IClock& clock_;
  std::optional<IoOutputWriteRequestDTO> request_;
  std::optional<IoOutputObservationDTO> observation_;
};

class IoRecoveryAdmissionGate final : public IResourceOperationAdmissionGate {
 public:
  std::shared_ptr<std::recursive_mutex> shared_admission_mutex() const override { return mutex_; }
  Result<void> reserve(ResourceSlotTable& table, const ResourceSlotOwnerDTO& owner,
                       const std::vector<ResourceId>& resources) override {
    std::lock_guard lock(*mutex_);
    return table.reserve(owner, resources);
  }
  Result<void> reserve_batch(ResourceSlotTable& table,
                             const std::vector<ResourceSlotReservationOwnerDTO>& owners) override {
    std::lock_guard lock(*mutex_);
    return table.reserve_batch(owners);
  }
  std::vector<ResourceId> current_fenced_resources(const std::vector<ResourceId>&) const override {
    return {};
  }
  bool has_held_configuration_fences() const override { return false; }

 private:
  std::shared_ptr<std::recursive_mutex> mutex_ = std::make_shared<std::recursive_mutex>();
};

class IoRecoveryKernelReadinessPort final : public ICCLCComponentCommandReadinessPort {
 public:
  explicit IoRecoveryKernelReadinessPort(ResourceAddressedExecutionKernel& kernel)
      : kernel_(kernel) {}
  Result<ComponentCommandReadinessDTO> CloseComponentCommandEligibility(
      const LifecycleTransactionId& transaction,
      const ControllableComponentId& component) override {
    return kernel_.close_component_command_eligibility(transaction, component);
  }
  Result<ComponentCommandReadinessDTO> PublishComponentCommandReadiness(
      const ComponentCommandReadinessPublicationDTO& publication) override {
    return kernel_.publish_component_command_readiness(publication);
  }
  Result<ComponentCommandReadinessDTO> PublishComponentCommandReadiness(
      const ComponentCommandReadinessPublicationDTO& publication,
      const ComponentLifecycleCurrentSnapshotDTO& snapshot,
      const std::vector<BrandLifecycleObservationDTO>& observations) override {
    const auto installed =
        kernel_.publish_runtime_provider_configuration(publication, snapshot, observations);
    if (!installed.ok()) return Result<ComponentCommandReadinessDTO>::Err(installed.error());
    return kernel_.publish_component_command_readiness(publication);
  }
  Result<ComponentCommandReadinessDTO> ReadComponentCommandReadiness(
      const ControllableComponentId& component) const override {
    return kernel_.component_command_readiness(component);
  }

 private:
  ResourceAddressedExecutionKernel& kernel_;
};

// VB2-O5: the proof needs succeeded `transition.disable` / `transition.enable`
// actions that each ran at least one substep; an Enable that found nothing to
// do is no proof of a restart.
void VerifyOutputResynchronization(bool enable_runs_a_substep) {
  ScopedTemporaryDirectory temporary;
  const auto lifecycle_path =
      temporary.path() / std::string(kCCLCJournalFilename);
  SqliteCCLCJournal journal(lifecycle_path, CellId("cell.1"), 8U);
  CCLCDomainReservationTable reservations;
  auto plans = EnableCatalog().plans;
  for (auto& plan : plans) {
    for (auto& transition : plan.transitions)
      transition.lifecycle_primitive_id =
          LifecyclePrimitiveId("io.provider." + std::string(to_string(plan.intent)));
    if (plan.intent == ComponentLifecycleIntentKindDTO::kPrepare)
      plan.transitions.front().eligibility_predicate.allowed_states = {
          ComponentLifecycleStateDTO::kDeclared, ComponentLifecycleStateDTO::kDisabled};
  }
  const auto made = MakeBrandLifecyclePlanCatalog(ProviderManifestId("manifest.io"),
                                                  ProviderManifestRevision(Sha('1')), plans);
  ASSERT_TRUE(made.ok());
  const auto catalog = made.value();
  auto base = Snapshot("io.base", ComponentLifecycleStateDTO::kActive,
                       ComponentLifecycleGeneration(3U), catalog);
  const ProviderControlDomainDTO provider{base.controllable_component_id,
                                          RuntimeUnitId("runtime.1"),
                                          ProviderInstanceId("provider.io"),
                                          catalog.provider_manifest_id,
                                          catalog.provider_manifest_revision,
                                          catalog.catalog_digest,
                                          ContentDigest(Sha('8')),
                                          {"valve"},
                                          Duration(1'000'000)};
  base.domains = {
      {ControlDomainId("domain.io"), ControlDomainRevision(Sha('9')),
       ResolvedProviderDomainScopeDTO{
           provider, RuntimeEpoch("runtime-epoch.1"), {}, {ResourceId("output.channel")}}}};
  base.provider_occurrences = {
      {base.domains.front().control_domain_id, base.domains.front().control_domain_revision,
       provider.provider_instance_id, provider.provider_manifest_id,
       provider.provider_manifest_revision, provider.provider_runtime_unit_id,
       RuntimeEpoch("runtime-epoch.1"), catalog.catalog_digest}};
  IoRecoveryJournalSnapshotSource snapshots(journal, base);
  FakeCatalogSource catalogs(catalog);
  FakeClock clock;
  clock.set(100);
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{
                   ComponentLifecycleStateDTO::kActive, ComponentLifecycleStateDTO::kActive,
                   ComponentLifecycleStateDTO::kDisabled, ComponentLifecycleStateDTO::kDisabled,
                   ComponentLifecycleStateDTO::kPrepared, ComponentLifecycleStateDTO::kPrepared,
                   ComponentLifecycleStateDTO::kActive});
  port->set_catalog(catalog);
  auto timed_port = std::make_shared<IoRecoveryTimedBrandPort>(port, clock);
  RoutedResolver resolver(timed_port, base.provider_occurrences);
  ManualScheduler scheduler;
  const auto* definition = find_builtin_capability_definition("io.gpio.set_output");
  ASSERT_NE(definition, nullptr);
  const auto schema = builtin_io_signal_write_command_schema();
  RuntimeParticipantBindingDTO captured{
      ProgramId("program.io"),
      ProgramRevision("revision.io"),
      ProgramBindingId("binding.io"),
      base.active_publication_receipt.request.candidate_retained_bundle_id,
      base.active_publication_receipt.request.candidate_graph_revision,
      ComponentRoleId("io"),
      TargetRoleId("io"),
      base.controllable_component_id,
      ComponentTypeId("io.type"),
      ComponentTypeRevision("io.type.revision"),
      CommandTargetId("gpio"),
      CapabilityId("io.gpio.set_output"),
      CapabilityDefinitionRevision(
          std::string(definition->capability_definition_revision)),
      std::nullopt,
      {},
      {},
      {},
      {},
      {},
      std::nullopt,
      std::nullopt,
      {},
      {},
      {},
      std::nullopt};
  captured.command_schema =
      ProgramCommandSchemaRefDTO{schema.command_schema_id, schema.command_schema_revision};
  const auto& occurrence = base.provider_occurrences.front();
  captured.capability_provider_path =
      RuntimeCapabilityProviderPathDTO{captured.capability_id,
                                       captured.capability_definition_revision,
                                       CapabilityProviderBindingId("io.binding"),
                                       CapabilityProviderBindingRevision(Sha('4')),
                                       occurrence.provider_instance_id,
                                       occurrence.provider_manifest_id,
                                       occurrence.provider_manifest_revision,
                                       occurrence.provider_runtime_unit_id,
                                       occurrence.provider_runtime_epoch};
  captured.runtime_unit_epochs = {
      {occurrence.provider_runtime_unit_id, occurrence.provider_runtime_epoch}};
  const IoSignalDescriptorDTO descriptor{{base.controllable_component_id, "valve"},
                                         InterruptSourceId("output"),
                                         IoSignalValueType::kBoolean,
                                         IoSignalDirection::kOutput,
                                         IoSignalUpdateMode::kChangeOnly,
                                         IoTransitionCapability::kSampledOnly,
                                         {IoFreshnessMode::kProviderValidity, {}},
                                         ResourceId("output.channel")};
  const auto descriptor_digest = ComputeIoSignalDescriptorDigest(descriptor);
  ASSERT_TRUE(descriptor_digest.ok());
  captured.io_output_binding =
      RuntimeIoOutputBindingDTO{ResourceRoleId("valve"),
                                descriptor,
                                descriptor_digest.value(),
                                ComponentLifecycleGeneration(3U),
                                occurrence,
                                provider.signal_configuration_digest,
                                IoOutputCompletionPolicyDTO(IoOutputSourceAckBudgetDTO{100})};
  auto sealed = verify_runtime_participant_binding(captured);
  ASSERT_TRUE(sealed.ok());
  SqliteOperationJournal operation_journal(temporary.path() / "operations.sqlite",
                                           CellId("cell.1"));
  IoRecoveryPendingOutput output(clock);
  IoOutputParticipantEngine engine(output, clock);
  RuntimeParticipantExecutionRegistration registration{
      sealed.value(),
      {},
      {},
      MotionInterruptNodeCategory::kGeneric,
      &engine,
      nullptr,
      {},
      &engine,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      false,
      {},
      std::nullopt,
      {}};
  registration.intrinsic_direct_control = true;
  IoRecoveryAdmissionGate admission_gate;
  ResourceAddressedExecutionKernelConfig kernel_config{RequestOwnerEpoch("kernel.io"),
                                                       CellId("cell.1"), 16U};
  kernel_config.operation_admission_gate = &admission_gate;
  ManagerLivenessRequiredSetDTO required{CellId("cell.1"),
                                         captured.retained_bundle_id,
                                         ComputeRequiredRuntimeSetDigest({}),
                                         RuntimeUnitId("manager"),
                                         RuntimeEpoch("manager.1"),
                                         RequestOwnerEpoch("manager.owner"),
                                         {}};
  kernel_config.manager_liveness_required_set = required;
  // Use the same actual durable owner readers as the host. Startup epoch
  // reconciliation and later lifecycle settlement cannot use an accept-all
  // gate.
  kernel_config.cell_admission_reconcile_validator =
      [lifecycle_path](
          const std::vector<CellAdmissionClaimDTO> &held,
          const std::vector<CellAdmissionCurrentOwnerEpochDTO> &current,
          const CellAdmissionReconcileDTO &request) {
        return ValidateCCLCCellAdmissionReconciliationReadOnly(
            lifecycle_path, CellId("cell.1"), held, current, request);
      };
  kernel_config.cell_admission_settlement_validator =
      [lifecycle_path](const CellAdmissionClaimDTO &claim,
                       const CellAdmissionClaimReleaseDTO &release) {
        return ValidateCCLCCellAdmissionSettlementReadOnly(
            lifecycle_path, CellId("cell.1"), claim, release);
      };
  ComponentLifecycleCoordinator* proof_owner = nullptr;
  kernel_config.io_output_local_resynchronization_source =
      [&](const RuntimeParticipantBindingDTO& value) {
        return proof_owner->ReadIoOutputLocalResynchronization(value);
      };
  ResourceAddressedExecutionKernel kernel({registration}, clock, scheduler, operation_journal,
                                          std::move(kernel_config));
  IoRecoveryKernelReadinessPort readiness(kernel);
  const auto context =
      std::make_shared<const ComponentLifecycleProductContext>(ComponentLifecycleProductContext{
          snapshots,
          catalogs,
          resolver,
          {},
          &readiness,
          ComponentLifecycleProductIdentity{base.active_publication_receipt,
                                            {base.controllable_component_id}}});
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, context, kernel, clock,
                                            scheduler);
  proof_owner = &coordinator;
  const auto started = coordinator.Start();
  ASSERT_TRUE(started.ok())
      << started.error().code << ": " << started.error().message;
  const auto initial = snapshots.ReadCurrentComponentSnapshot(base.controllable_component_id);
  ASSERT_TRUE(initial.ok());
  const BrandLifecycleObservationQueryDTO initial_query{
      LifecycleTransactionId("initial"),   LifecycleTransactionGeneration(1U),
      base.controllable_component_id,      occurrence.control_domain_id,
      occurrence.control_domain_revision,  occurrence.provider_instance_id,
      occurrence.provider_manifest_id,     occurrence.provider_manifest_revision,
      occurrence.provider_runtime_unit_id, occurrence.provider_runtime_epoch};
  const auto initial_observation = timed_port->Observe(initial_query);
  ASSERT_TRUE(initial_observation.ok());
  const ComponentCommandReadinessPublicationDTO bootstrap{
      ComponentReadinessAuthorityDTO::kRestartExclusion,
      {},
      {},
      base.controllable_component_id,
      ComponentLifecycleGeneration(3U),
      true};
  ASSERT_TRUE(kernel.SetRouteRecoveryPending(true).ok());
  ASSERT_TRUE(kernel
                  .publish_runtime_provider_configuration(bootstrap, initial.value(),
                                                          {initial_observation.value()})
                  .ok());
  ASSERT_TRUE(kernel.publish_component_command_readiness(bootstrap).ok());
  ASSERT_TRUE(kernel.SetRouteRecoveryPending(false).ok());
  const CellAdmissionReadinessSnapshotDTO idle{
      true,
      ProgramCycleStatusDTO{
          RequestOwnerEpoch("runner"), 1U, ProgramCycleStateDTO::kIdleAfterReset, {}, {}, {}},
      true,
      1U,
      1U,
      true,
      true,
      0U,
      0U,
      0U};
  ASSERT_TRUE(kernel.UpdateCellAdmissionReadiness(idle).ok());
  const OperationIntentDTO write{
      {RequestId("unknown.write"), RequestOwnerEpoch("kernel.io"), "operator", {}},
      captured.program_id,
      captured.program_revision,
      captured.program_binding_id,
      ProgramRunId("manual.output"),
      captured.graph_revision,
      captured.retained_bundle_id,
      {},
      {{captured.component_role_id,
        captured.target_role_id,
        captured.capability_id,
        {},
        CapabilityCommandIntentDTO{schema.command_schema_id,
                                   schema.command_schema_revision,
                                   IoSignalWriteCommandValueDTO{ResourceRoleId("valve"), true},
                                   {},
                                   {}}}}};
  const auto accepted_write = kernel.execute(write);
  ASSERT_TRUE(accepted_write.ok()) << accepted_write.error().message;
  clock.advance(100);
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  const auto unknown = kernel.get_operation_state(accepted_write.value().operation_id);
  ASSERT_TRUE(unknown.ok());
  ASSERT_EQ(unknown.value().state, OperationStateKind::kInterventionRequired);
  ASSERT_EQ(unknown.value().participants.front().output_evidence,
            IoOutputEvidence::kAcceptanceUnknown);
  const auto retained_slots = kernel.resource_snapshot();
  OperationControlRequestDTO reset{
      {RequestId("operator.reset.unknown"), RequestOwnerEpoch("kernel.io"), "operator", {}},
      unknown.value().operation_id,
      unknown.value().operation_revision,
      std::string("Operator accepts remaining remote queued-write uncertainty")};
  reset.acknowledged_uncertain_outputs = std::vector<UncertainOutputAcknowledgementDTO>{
      {unknown.value().operation_id, unknown.value().operation_revision}};
  EXPECT_FALSE(kernel.reset(reset).ok());  // actual durable restart proof still absent
  EXPECT_EQ(kernel.resource_snapshot(), retained_slots);
  EXPECT_FALSE(coordinator.ReadIoOutputLocalResynchronization(captured).ok());
  std::vector<LifecycleTransactionId> completed;
  for (const auto intent_kind :
       {ComponentLifecycleIntentKindDTO::kDisable, ComponentLifecycleIntentKindDTO::kPrepare,
        ComponentLifecycleIntentKindDTO::kEnable}) {
    const auto current = snapshots.ReadCurrentComponentSnapshot(base.controllable_component_id);
    ASSERT_TRUE(current.ok());
    ComponentLifecycleIntentDTO intent{
        {RequestId("recovery." + std::string(to_string(intent_kind))),
         coordinator.request_owner_epoch(),
         "test",
         {}},
        base.controllable_component_id,
        intent_kind,
        captured.graph_revision,
        current.value().component_lifecycle_generation,
        {{base.domains.front().control_domain_id, base.domains.front().control_domain_revision,
          ExpectedProviderDomainScopeDTO{
              provider, {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, {}, {}}}}},
        {}};
    intent.expected_admission_generation = kernel.GetAuthoringAdmission().admission_generation;
    port->set_report_substeps(intent_kind != ComponentLifecycleIntentKindDTO::kEnable ||
                              enable_runs_a_substep);
    const auto accepted = coordinator.SubmitLifecycle(intent);
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    ASSERT_TRUE(accepted.value().acceptance);
    ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
    const auto actual =
        coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
    ASSERT_TRUE(actual.ok());
    ASSERT_TRUE(actual.value());
    ASSERT_EQ(actual.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
    ASSERT_FALSE(actual.value()->domains.front().primitive_attempts.empty());
    EXPECT_EQ(actual.value()->domains.front().primitive_attempts.front().lifecycle_primitive_id,
              LifecycleTransitionActionId(intent_kind));
    completed.push_back(actual.value()->acceptance.lifecycle_transaction_id);
    EXPECT_EQ(kernel.get_operation_state(unknown.value().operation_id).value(), unknown.value());
    EXPECT_EQ(kernel.resource_snapshot().front().owner, retained_slots.front().owner);
    EXPECT_EQ(output.submissions, 1U);
    EXPECT_EQ(output.retirements, 0U);
    if (intent_kind != ComponentLifecycleIntentKindDTO::kEnable) {
      EXPECT_FALSE(coordinator.ReadIoOutputLocalResynchronization(captured).ok());
    }
  }
  EXPECT_EQ(port->dispatch_count(), 3U);
  if (!enable_runs_a_substep) {
    EXPECT_FALSE(coordinator.ReadIoOutputLocalResynchronization(captured).ok());
    return;
  }
  const auto proof = coordinator.ReadIoOutputLocalResynchronization(captured);
  ASSERT_TRUE(proof.ok()) << proof.error().message;
  EXPECT_EQ(proof.value().provider, occurrence);
  EXPECT_EQ(proof.value().disable_transaction_id, completed.front());
  EXPECT_EQ(proof.value().enable_transaction_id, completed.back());
  EXPECT_EQ(proof.value().disabled_generation, ComponentLifecycleGeneration(4U));
  EXPECT_EQ(proof.value().enabled_generation, ComponentLifecycleGeneration(6U));
  auto foreign = captured;
  foreign.retained_bundle_id = RetainedBundleId("foreign.bundle");
  EXPECT_FALSE(coordinator.ReadIoOutputLocalResynchronization(foreign).ok());
  auto later_write = captured;
  later_write.io_output_binding->component_lifecycle_generation = ComponentLifecycleGeneration(4U);
  EXPECT_FALSE(coordinator.ReadIoOutputLocalResynchronization(later_write).ok());
  EXPECT_EQ(port->dispatch_count(), 3U);  // proof queries never issue effects
  auto absent_ack = reset;
  absent_ack.request_context.request_id = RequestId("operator.reset.no.ack");
  absent_ack.acknowledged_uncertain_outputs.reset();
  EXPECT_FALSE(kernel.reset(absent_ack).ok());
  EXPECT_EQ(kernel.resource_snapshot().front().owner, retained_slots.front().owner);
  const auto reset_accepted = kernel.reset(reset);
  ASSERT_TRUE(reset_accepted.ok()) << reset_accepted.error().message;
  EXPECT_EQ(reset_accepted.value().control, OperationControlKind::kReset);
  const auto reset_again = kernel.reset(reset);
  ASSERT_TRUE(reset_again.ok());
  EXPECT_EQ(reset_again.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(reset_again.value().operation_revision_after_acceptance,
            reset_accepted.value().operation_revision_after_acceptance);
  auto conflicting_reset = reset;
  conflicting_reset.acknowledged_uncertain_outputs->front().operation_revision =
      OperationRevision(unknown.value().operation_revision.value() + 1U);
  const auto conflict = kernel.reset(conflicting_reset);
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().code, "request_id_conflict");
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < end &&
         kernel.get_operation_projection(unknown.value().operation_id)
             .value()
             .resident) {
    // The real host drives this owner checkpoint after asynchronous Reset:
    // residency includes the settled manual admission claim until it releases.
    const auto reconciled = kernel.reconcile_manual_operation_admission();
    ASSERT_TRUE(reconciled.ok())
        << reconciled.error().code << ": " << reconciled.error().message;
    std::this_thread::yield();
  }
  const auto retired = kernel.get_operation_projection(unknown.value().operation_id);
  ASSERT_TRUE(retired.ok());
  ASSERT_FALSE(retired.value().resident);
  EXPECT_EQ(retired.value().operation.participants.front().output_evidence,
            IoOutputEvidence::kAcceptanceUnknown);
  ASSERT_TRUE(retired.value().operation.participants.front().terminal_fact);
  EXPECT_EQ(retired.value().operation.participants.front().terminal_fact->effect,
            ParticipantEffectKnowledge::kEffectUncertain);
  EXPECT_EQ(output.submissions, 1U);
  EXPECT_EQ(output.retirements, 1U);
}

TEST(ComponentLifecycleCoordinator,
     OutputResynchronizationRequiresActualCompletedDisableThenEnableInExistingJournal) {
  VerifyOutputResynchronization(true);
}

TEST(ComponentLifecycleCoordinator, OutputResynchronizationRefusesAnEnableThatRanNoSubstep) {
  VerifyOutputResynchronization(false);
}

TEST(ComponentLifecycleCoordinator,
     MixedDomainTerminalProofKeepsPhysicalReadinessAndFiltersProviderObservations) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  const auto catalog = EnableCatalog();
  auto before = Snapshot("mixed.before", ComponentLifecycleStateDTO::kPrepared,
                         ComponentLifecycleGeneration(3U), catalog);
  const ProviderControlDomainDTO provider{before.controllable_component_id,
                                          RuntimeUnitId("runtime.io"),
                                          ProviderInstanceId("provider.io"),
                                          catalog.provider_manifest_id,
                                          catalog.provider_manifest_revision,
                                          catalog.catalog_digest,
                                          ContentDigest(Sha('8')),
                                          {"input.ready"},
                                          Duration(1'000'000)};
  const ControlDomainId generic_id("domain.io");
  const ControlDomainRevision generic_revision(Sha('9'));
  before.domains.push_back(
      {generic_id, generic_revision,
       ResolvedProviderDomainScopeDTO{provider, RuntimeEpoch("runtime.io.epoch"), std::nullopt}});
  before.provider_occurrences.push_back(
      {generic_id, generic_revision, provider.provider_instance_id, provider.provider_manifest_id,
       provider.provider_manifest_revision, provider.provider_runtime_unit_id,
       RuntimeEpoch("runtime.io.epoch"), provider.lifecycle_plan_catalog_digest});
  const auto before_digest = ComputeComponentLifecycleCurrentSnapshotDigest(before);
  ASSERT_TRUE(before_digest.ok());
  before.snapshot_digest = before_digest.value();
  auto accepted_snapshot = before;
  accepted_snapshot.component_lifecycle_generation = ComponentLifecycleGeneration(4U);
  const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(accepted_snapshot);
  ASSERT_TRUE(digest.ok());
  accepted_snapshot.snapshot_digest = digest.value();
  FakeSnapshotSource snapshots({before, before, accepted_snapshot});
  FakeCatalogSource catalogs(catalog);
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared,
                                                       ComponentLifecycleStateDTO::kPrepared,
                                                       ComponentLifecycleStateDTO::kActive});
  port->set_catalog(catalog);
  RoutedResolver resolver(port, before.provider_occurrences);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  FakeReadinessPort readiness;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler, std::nullopt,
                                            &readiness);
  ASSERT_TRUE(coordinator.Start().ok());
  auto intent = PrepareIntent(coordinator.request_owner_epoch());
  intent.intent = ComponentLifecycleIntentKindDTO::kEnable;
  intent.expected_control_domains.push_back(
      {generic_id, generic_revision,
       ExpectedProviderDomainScopeDTO{
           provider,
           {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt}}});
  const auto accepted = coordinator.SubmitLifecycle(intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  EXPECT_EQ(readiness.calls(), (std::vector<std::string>{"close", "publish_open"}));
  const auto [all_domains, observations] = readiness.provider_proof();
  EXPECT_EQ(all_domains, 2U);          // Actual snapshot still carries physical closure.
  ASSERT_EQ(observations.size(), 1U);  // Generic proof excludes physical observations.
  EXPECT_EQ(observations.front().control_domain_id, generic_id);
  EXPECT_EQ(observations.front().observed_state, ComponentLifecycleStateDTO::kActive);
}

TEST(ComponentLifecycleCoordinator, UnverifiedReadinessPublicationCannotReportSuccess) {
  const auto run = RunEnable(1U);
  ASSERT_TRUE(run.status.has_value());
  // The adapter succeeded but the Kernel result was not verified: the
  // failure after effects settles command-closed, never Active.
  EXPECT_EQ(run.status->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  EXPECT_EQ(run.calls, (std::vector<std::string>{"close", "publish_open", "publish_closed"}));
  EXPECT_TRUE(run.accepting_after);
}

TEST(ComponentLifecycleCoordinator, UnverifiedContainmentRetainsTheClaimAndClosesAdmission) {
  const auto run = RunEnable(2U);
  // Neither the enabled nor the closed result was read back: no terminal
  // settlement or claim release is recorded.
  ASSERT_TRUE(run.status.has_value());
  EXPECT_FALSE(run.status->terminal_result.has_value());
  EXPECT_EQ(run.calls, (std::vector<std::string>{"close", "publish_open", "publish_closed"}));
  EXPECT_FALSE(run.accepting_after);
}

TEST(ComponentLifecycleCoordinator,
     MultiDomainPartialFailureSettlesTheWholeComponentCommandClosed) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  const auto catalog = PrepareCatalog();
  auto snapshot = Snapshot("snapshot.two-domains.readiness", ComponentLifecycleStateDTO::kDeclared,
                           ComponentLifecycleGeneration(3U), catalog);
  auto second = CurrentDomain();
  second.control_domain_id = ControlDomainId("domain.2");
  second.control_domain_revision = ControlDomainRevision("domain-revision.2");
  second.controller().controller_topology_id = ControllerTopologyId("topology.2");
  second.controller().topology_revision = TopologyRevision("topology-revision.2");
  second.controller().current_control_bindings.front().control_binding_id = ControlBindingId("binding.2");
  second.controller().current_control_bindings.front().control_binding_revision =
      ControlBindingRevision("binding-revision.2");
  second.controller().current_control_bindings.front().control_route_id = ControlRouteId("route.2");
  second.controller().current_resources = {ResourceId("resource.2")};
  auto second_provider = Provider(catalog);
  second_provider.control_domain_id = second.control_domain_id;
  second_provider.control_domain_revision = second.control_domain_revision;
  second_provider.provider_instance_id = ProviderInstanceId("provider.2");
  snapshot.domains.push_back(second);
  snapshot.provider_occurrences.push_back(second_provider);
  auto intent = PrepareIntent(RequestOwnerEpoch("cclc-owner.1"));
  auto expected_second = ExpectedDomain();
  expected_second.control_domain_id = second.control_domain_id;
  expected_second.control_domain_revision = second.control_domain_revision;
  expected_second.controller().controller_topology_id = second.controller().controller_topology_id;
  expected_second.controller().topology_revision = second.controller().topology_revision;
  expected_second.controller().expected_control_bindings = second.controller().current_control_bindings;
  intent.expected_control_domains.push_back(expected_second);

  FakeSnapshotSource snapshots({snapshot});
  FakeCatalogSource catalogs(catalog);
  auto first_port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  auto second_port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared},
      DispatchMode::kResponseLost);
  DomainPortResolver resolver(snapshot.provider_occurrences.front(), first_port,
                              second_provider, second_port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  FakeReadinessPort readiness;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler, std::nullopt, &readiness);
  ASSERT_TRUE(coordinator.Start().ok());
  intent.request_context.request_owner_epoch = coordinator.request_owner_epoch();
  const auto accepted = coordinator.SubmitLifecycle(intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance.has_value());
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok() && status.value().has_value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  // The first domain succeeded, yet no domain of the component was ever
  // opened, and the settled result is verified closed before release.
  const auto calls = readiness.calls();
  EXPECT_EQ(std::count(calls.begin(), calls.end(), "publish_open"), 0);
  ASSERT_FALSE(calls.empty());
  EXPECT_EQ(calls.front(), "close");
  EXPECT_EQ(calls.back(), "publish_closed");
  EXPECT_FALSE(readiness.state().command_eligible);
  // The dispatched failure (the M55 Isaac Lite6 wedge: arm Prepared, gripper
  // not) records each domain's observed state and the component's least advanced
  // one, so the next Prepare, Disable or Close can still read the component.
  ASSERT_EQ(status.value()->domains.size(), 2U);
  EXPECT_EQ(status.value()->domains.front().current_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(status.value()->domains.back().current_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_EQ(status.value()->current_state, ComponentLifecycleStateDTO::kDeclared);
  ASSERT_TRUE(status.value()->error);
  EXPECT_FALSE(status.value()->error->details.contains("component_state"));
  const auto fact =
      journal.LoadCurrentComponentLifecycleFact(intent.controllable_component_id);
  ASSERT_TRUE(fact.ok());
  EXPECT_EQ(fact.value().durable_generation,
            accepted.value().acceptance->component_lifecycle_generation);
  EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kDeclared);

  // A restarted owner reading the same durable journal finds a settled
  // transaction and a state that is not command readiness.
  SqliteCCLCJournal reopened(temporary.path() / std::string(kCCLCJournalFilename),
                             CellId("cell.1"), 8U);
  const auto reopened_fact =
      reopened.LoadCurrentComponentLifecycleFact(intent.controllable_component_id);
  ASSERT_TRUE(reopened_fact.ok()) << reopened_fact.error().message;
  EXPECT_TRUE(reopened_fact.value().durable_generation.has_value());
  EXPECT_EQ(reopened_fact.value().current_state, ComponentLifecycleStateDTO::kDeclared);
  const auto unsettled = reopened.LoadComponentsWithUnsettledLifecycle();
  ASSERT_TRUE(unsettled.ok()) << unsettled.error().message;
  EXPECT_TRUE(unsettled.value().empty());
}

struct TwoDomainLifecycleFixture final {
  ComponentLifecycleCurrentSnapshotDTO snapshot;
  ResolvedLifecycleProviderOccurrenceDTO second_provider;
  ComponentLifecycleIntentDTO intent;
};

// One component with two control domains, like an arm and its gripper.
[[nodiscard]] TwoDomainLifecycleFixture TwoDomainLifecycle(
    ComponentLifecycleStateDTO state, const BrandLifecyclePlanCatalogDTO& catalog,
    ComponentLifecycleIntentKindDTO intent_kind) {
  auto snapshot = Snapshot("snapshot.two-domains.state", state,
                           ComponentLifecycleGeneration(3U), catalog);
  auto second = CurrentDomain();
  second.control_domain_id = ControlDomainId("domain.2");
  second.control_domain_revision = ControlDomainRevision("domain-revision.2");
  second.controller().controller_topology_id = ControllerTopologyId("topology.2");
  second.controller().topology_revision = TopologyRevision("topology-revision.2");
  auto& binding = second.controller().current_control_bindings.front();
  binding.control_binding_id = ControlBindingId("binding.2");
  binding.control_binding_revision = ControlBindingRevision("binding-revision.2");
  binding.control_route_id = ControlRouteId("route.2");
  second.controller().current_resources = {ResourceId("resource.2")};
  auto second_provider = Provider(catalog);
  second_provider.control_domain_id = second.control_domain_id;
  second_provider.control_domain_revision = second.control_domain_revision;
  second_provider.provider_instance_id = ProviderInstanceId("provider.2");
  snapshot.domains.push_back(second);
  snapshot.provider_occurrences.push_back(second_provider);
  auto intent = PrepareIntent(RequestOwnerEpoch("cclc-owner.1"));
  intent.intent = intent_kind;
  auto expected_second = ExpectedDomain();
  expected_second.control_domain_id = second.control_domain_id;
  expected_second.control_domain_revision = second.control_domain_revision;
  expected_second.controller().controller_topology_id = second.controller().controller_topology_id;
  expected_second.controller().topology_revision = second.controller().topology_revision;
  expected_second.controller().expected_control_bindings =
      second.controller().current_control_bindings;
  intent.expected_control_domains.push_back(expected_second);
  return {std::move(snapshot), std::move(second_provider), std::move(intent)};
}

TEST(ComponentLifecycleCoordinator, MultiDomainPartialEnableNeverRetainsActive) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  const auto catalog = EnableCatalog();
  auto fixture = TwoDomainLifecycle(ComponentLifecycleStateDTO::kPrepared, catalog,
                                    ComponentLifecycleIntentKindDTO::kEnable);
  FakeSnapshotSource snapshots({fixture.snapshot});
  FakeCatalogSource catalogs(catalog);
  auto first_port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared,
                                                       ComponentLifecycleStateDTO::kActive});
  auto second_port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared},
      DispatchMode::kResponseLost);
  first_port->set_catalog(catalog);
  second_port->set_catalog(catalog);
  DomainPortResolver resolver(fixture.snapshot.provider_occurrences.front(), first_port,
                              fixture.second_provider, second_port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  fixture.intent.request_context.request_owner_epoch = coordinator.request_owner_epoch();
  const auto accepted = coordinator.SubmitLifecycle(fixture.intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance.has_value());
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok() && status.value().has_value());
  ASSERT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  ASSERT_EQ(status.value()->domains.front().current_state, ComponentLifecycleStateDTO::kActive);
  EXPECT_EQ(status.value()->domains.back().current_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(status.value()->current_state, ComponentLifecycleStateDTO::kPrepared);
  // One domain is Active, the other still Prepared: the component is not
  // command-ready, and the next Enable can retry from Prepared.
  const auto fact =
      journal.LoadCurrentComponentLifecycleFact(fixture.intent.controllable_component_id);
  ASSERT_TRUE(fact.ok()) << fact.error().message;
  EXPECT_EQ(fact.value().durable_generation,
            accepted.value().acceptance->component_lifecycle_generation);
  EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kPrepared);
}

// VB2-O1 fallback: no fresh resting observation after a dispatched failure.
TEST(ComponentLifecycleCoordinator, DispatchedFailureWithoutFaultFreeObservationIsReconstructedDeclared) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  const auto catalog = PrepareCatalog();
  auto fixture = TwoDomainLifecycle(ComponentLifecycleStateDTO::kDeclared, catalog,
                                    ComponentLifecycleIntentKindDTO::kPrepare);
  FakeSnapshotSource snapshots({fixture.snapshot});
  FakeCatalogSource catalogs(catalog);
  auto first_port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  // The second domain's action fails and its one fresh observation after the
  // failure is faulted: no fault-free state to record.
  auto second_port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kFaulted},
      DispatchMode::kResponseLost);
  DomainPortResolver resolver(fixture.snapshot.provider_occurrences.front(), first_port,
                              fixture.second_provider, second_port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  fixture.intent.request_context.request_owner_epoch = coordinator.request_owner_epoch();
  testing::internal::CaptureStderr();
  const auto accepted = coordinator.SubmitLifecycle(fixture.intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance.has_value());
  static_cast<void>(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto output = testing::internal::GetCapturedStderr();
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok() && status.value().has_value());
  ASSERT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed)
      << (status.value()->error ? status.value()->error->code : std::string("no error"));
  EXPECT_EQ(status.value()->domains.front().current_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(status.value()->domains.back().current_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_EQ(status.value()->current_state, ComponentLifecycleStateDTO::kDeclared);
  ASSERT_TRUE(status.value()->domains.back().observed_after);
  EXPECT_EQ(status.value()->domains.back().observed_after->observed_state,
            ComponentLifecycleStateDTO::kFaulted);
  ASSERT_TRUE(status.value()->error);
  ASSERT_TRUE(status.value()->error->details.contains("component_state"))
      << status.value()->error->code << " " << status.value()->error->details.size();
  EXPECT_EQ(status.value()->error->details.at("component_state"),
            kComponentLifecycleStateReconstructedCode);
  EXPECT_NE(output.find("manyforge_cclc_lifecycle_state_reconstructed"), std::string::npos)
      << output;
  const auto fact =
      journal.LoadCurrentComponentLifecycleFact(fixture.intent.controllable_component_id);
  ASSERT_TRUE(fact.ok()) << fact.error().message;
  EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kDeclared);
}

// The first domain's success expires before the second domain's failure
// settles: its Active no longer counts, so the component is reconstructed
// Declared instead of the second domain's Prepared.
TEST(ComponentLifecycleCoordinator, ExpiredSucceededDomainAtSettlementIsReconstructedDeclared) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  const auto catalog = EnableCatalog();
  auto fixture = TwoDomainLifecycle(ComponentLifecycleStateDTO::kPrepared, catalog,
                                    ComponentLifecycleIntentKindDTO::kEnable);
  FakeSnapshotSource snapshots({fixture.snapshot});
  FakeCatalogSource catalogs(catalog);
  FakeClock clock;
  clock.set(100);
  auto first_port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared,
                                                       ComponentLifecycleStateDTO::kActive});
  // Time passes past the first domain's freshness (2'000'000) during the
  // second dispatch; the second domain's own evidence stays fresh.
  auto second_port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared},
      DispatchMode::kResponseLost, 1U, std::vector<LifecycleFactDTO>{},
      [&clock] { clock.set(3'000'000); });
  second_port->set_fresh_until_ns(10'000'000);
  first_port->set_catalog(catalog);
  second_port->set_catalog(catalog);
  DomainPortResolver resolver(fixture.snapshot.provider_occurrences.front(), first_port,
                              fixture.second_provider, second_port);
  FakeCellAdmissionPort kernel;
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  fixture.intent.request_context.request_owner_epoch = coordinator.request_owner_epoch();
  const auto accepted = coordinator.SubmitLifecycle(fixture.intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance.has_value());
  static_cast<void>(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok() && status.value().has_value());
  ASSERT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed)
      << (status.value()->error ? status.value()->error->code : std::string("no error"));
  EXPECT_EQ(status.value()->domains.front().action_state,
            ComponentLifecycleDomainActionStateDTO::kSucceeded);
  EXPECT_EQ(status.value()->domains.front().current_state, ComponentLifecycleStateDTO::kActive);
  EXPECT_EQ(status.value()->domains.back().current_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(status.value()->current_state, ComponentLifecycleStateDTO::kDeclared);
  ASSERT_TRUE(status.value()->error);
  ASSERT_TRUE(status.value()->error->details.contains("component_state"));
  EXPECT_EQ(status.value()->error->details.at("component_state"),
            kComponentLifecycleStateReconstructedCode);
  const auto fact =
      journal.LoadCurrentComponentLifecycleFact(fixture.intent.controllable_component_id);
  ASSERT_TRUE(fact.ok()) << fact.error().message;
  EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kDeclared);
}

TEST(ComponentLifecycleCoordinator, EnableRefusedBeforeDispatchKeepsTheDeclaredFact) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  const auto catalog = EnableCatalog();
  FakeSnapshotSource snapshots({Snapshot("snapshot.enable.declared",
                                         ComponentLifecycleStateDTO::kDeclared,
                                         ComponentLifecycleGeneration(3U), catalog)});
  FakeCatalogSource catalogs(catalog);
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
  port->set_catalog(catalog);
  FakeResolver resolver(port, catalog);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  auto intent = PrepareIntent(coordinator.request_owner_epoch());
  intent.intent = ComponentLifecycleIntentKindDTO::kEnable;
  const auto accepted = coordinator.SubmitLifecycle(intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance.has_value());
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok() && status.value().has_value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  ASSERT_TRUE(status.value()->error);
  EXPECT_EQ(status.value()->error->code, "component_lifecycle_transition_missing");
  EXPECT_EQ(port->dispatch_count(), 0U);
  // Refused before any dispatch: the Declared state stays current at the new
  // generation.
  const auto fact = journal.LoadCurrentComponentLifecycleFact(intent.controllable_component_id);
  ASSERT_TRUE(fact.ok()) << fact.error().message;
  EXPECT_EQ(fact.value().durable_generation,
            accepted.value().acceptance->component_lifecycle_generation);
  EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kDeclared);
}

// Uses the real journal's target CAS and the existing exact Kernel claim
// receipt seam. A release reply can be lost without losing its durable proof.
class TargetRefusalAdmissionPort final : public FakeCellAdmissionPort {
 public:
  Result<CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent) override {
    ++claim_acquires;
    auto result = FakeCellAdmissionPort::AcquireCellAdmissionClaim(intent);
    if (result.ok()) last_claim = result.value();
    return result;
  }

  Result<AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const CellAdmissionClaimReleaseDTO& release) override {
    ++claim_releases;
    last_release = release;
    if (throw_release) throw std::runtime_error("injected release exception");
    if (refuse_release) {
      return Result<AuthoringAdmissionDTO>::Err(
          {"owner_unavailable", "release was not performed", {{"reason", "release_unavailable"}}});
    }
    return FakeCellAdmissionPort::ReleaseCellAdmissionClaim(release);
  }

  std::size_t claim_acquires{};
  std::size_t claim_releases{};
  bool refuse_release{};
  bool throw_release{};
  std::optional<CellAdmissionClaimDTO> last_claim;
  std::optional<CellAdmissionClaimReleaseDTO> last_release;
};

// This test adapter delegates the owner, target acceptance, and claim
// operations exercised here to the genuine journal. Only its read-return
// boundary is paused, after the real snapshot committed.
class SettlementBarrierJournal final : public ICCLCJournal {
 public:
  explicit SettlementBarrierJournal(SqliteCCLCJournal& journal) : journal_(journal) {}
  Result<CCLCCurrentFactsReadbackDTO> LoadCurrentSnapshotFacts(
      const std::vector<ControllableComponentId>& canonical_controllable_component_ids,
      const std::vector<ControlDomainId>& canonical_control_domain_ids) override {
    return journal_.LoadCurrentSnapshotFacts(canonical_controllable_component_ids,
                                             canonical_control_domain_ids);
  }
  Result<std::optional<NamedApplyPublicationTransactionDTO>> ReadUnsettledNativeApplyPlan()
      override {
    return journal_.ReadUnsettledNativeApplyPlan();
  }
  Result<CCLCJournalRecoveryDTO> BeginOwnerEpoch(
      const RequestOwnerEpoch& unique_seed, std::int64_t started_at_ns,
      std::size_t request_membership_capacity,
      const std::optional<RequestOwnerEpoch>& quiescent_predecessor) override {
    return journal_.BeginOwnerEpoch(unique_seed, started_at_ns, request_membership_capacity,
                                    quiescent_predecessor);
  }
  Result<void> ReadQuiescentOwner(const RequestOwnerEpoch& arg0) override {
    auto result = journal_.ReadQuiescentOwner(arg0);
    std::unique_lock lock(mutex_);
    if (block_after_read_ && result.ok()) {
      blocked_ = true;
      changed_.notify_all();
      changed_.wait(lock, [&] { return released_; });
    }
    return result;
  }
  Result<std::optional<CCLCLifecycleRequestResultDTO>> InspectLifecycleRequest(
      const RequestOwnerEpoch& active_owner_epoch,
      const ComponentLifecycleIntentDTO& intent) override {
    return journal_.InspectLifecycleRequest(active_owner_epoch, intent);
  }
  Result<std::optional<CCLCLifecycleRequestResultDTO>> ReadLifecycleRequestResult(
      const ComponentLifecycleIntentDTO& arg0) override {
    return journal_.ReadLifecycleRequestResult(arg0);
  }
  Result<CCLCLifecycleRequestResultDTO> RecordLifecycleRejection(
      const RequestOwnerEpoch& arg0, const ComponentLifecycleIntentDTO& arg1, const ErrorDTO& arg2,
      const std::optional<CellAdmissionClaimIntentDTO>& arg3) override {
    return journal_.RecordLifecycleRejection(arg0, arg1, arg2, arg3);
  }
  Result<CCLCLifecycleRequestResultDTO> AcceptOrReplayLifecycle(
      const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
      const ComponentLifecycleAcceptedDTO& candidate_acceptance,
      const ComponentLifecycleStatusDTO& candidate_initial_status) override {
    return journal_.AcceptOrReplayLifecycle(active_owner_epoch, intent, candidate_acceptance,
                                            candidate_initial_status);
  }
  Result<CCLCLifecycleRequestResultDTO> AcceptOrReplayTargetLifecycle(
      const RequestOwnerEpoch& arg0, const ComponentLifecycleIntentDTO& arg1,
      const ComponentLifecycleAcceptedDTO& arg2, const ComponentLifecycleStatusDTO& arg3,
      const ComponentLifecycleTargetExpectationDTO& arg4,
      const CellAdmissionClaimDTO& arg5) override {
    return journal_.AcceptOrReplayTargetLifecycle(arg0, arg1, arg2, arg3, arg4, arg5);
  }
  Result<void> RecordCellClaimAttempt(const RequestOwnerEpoch& active_owner_epoch,
                                      const CellAdmissionClaimIntentDTO& intent) override {
    return journal_.RecordCellClaimAttempt(active_owner_epoch, intent);
  }
  Result<void> RecordCellClaimAcquired(const RequestOwnerEpoch& active_owner_epoch,
                                       const CellAdmissionClaimDTO& claim) override {
    return journal_.RecordCellClaimAcquired(active_owner_epoch, claim);
  }
  Result<void> RecordCellClaimReleaseReceipt(
      const RequestOwnerEpoch& active_owner_epoch, const CellAdmissionClaimReleaseDTO& release,
      const CellAdmissionRecoveryReceiptDTO& receipt) override {
    return journal_.RecordCellClaimReleaseReceipt(active_owner_epoch, release, receipt);
  }
  Result<std::uint64_t> RecordLifecycleTerminalAndClaimRelease(
      const RequestOwnerEpoch& owner, const ComponentLifecycleStatusDTO& status,
      const CellAdmissionClaimReleaseDTO& release) override {
    return journal_.RecordLifecycleTerminalAndClaimRelease(owner, status, release);
  }
  Result<std::uint64_t> RecordEffectPrepared(
      const RequestOwnerEpoch& active_owner_epoch, const CCLCEffectAttemptDTO& prepared_attempt,
      const ComponentLifecycleStatusDTO& successor_status) override {
    return journal_.RecordEffectPrepared(active_owner_epoch, prepared_attempt, successor_status);
  }
  Result<std::uint64_t> RecordEffectCompleted(
      const RequestOwnerEpoch& active_owner_epoch, const CCLCEffectAttemptDTO& completed_attempt,
      const ComponentLifecycleStatusDTO& successor_status) override {
    return journal_.RecordEffectCompleted(active_owner_epoch, completed_attempt, successor_status);
  }
  Result<std::uint64_t> RecordLifecycleStatus(const RequestOwnerEpoch& active_owner_epoch,
                                              const ComponentLifecycleStatusDTO& status) override {
    return journal_.RecordLifecycleStatus(active_owner_epoch, status);
  }
  Result<std::optional<ComponentLifecycleStatusDTO>> LoadRetainedLifecycleStatus(
      const LifecycleTransactionId& lifecycle_transaction_id) override {
    return journal_.LoadRetainedLifecycleStatus(lifecycle_transaction_id);
  }
  Result<ComponentLifecycleCurrentFactReadbackDTO> LoadCurrentComponentLifecycleFact(
      const ControllableComponentId& controllable_component_id) override {
    auto result = journal_.LoadCurrentComponentLifecycleFact(controllable_component_id);
    if (result.ok() && on_component_fact_read) on_component_fact_read(result.value());
    return result;
  }
  std::function<void(ComponentLifecycleCurrentFactReadbackDTO&)> on_component_fact_read{};
  Result<std::optional<CCLCRouteSelectionRequestResultDTO>> InspectRouteSelectionRequest(
      const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionIntentDTO& intent) override {
    return journal_.InspectRouteSelectionRequest(active_owner_epoch, intent);
  }
  Result<CCLCRouteSelectionRequestResultDTO> AcceptOrReplayRouteSelection(
      const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionIntentDTO& intent,
      const RouteSelectionAcceptedDTO& candidate_acceptance,
      const RouteSelectionStatusDTO& candidate_initial_status) override {
    return journal_.AcceptOrReplayRouteSelection(active_owner_epoch, intent, candidate_acceptance,
                                                 candidate_initial_status);
  }
  Result<std::uint64_t> RecordRouteEffectPrepared(
      const RequestOwnerEpoch& active_owner_epoch,
      const CCLCRouteEffectAttemptDTO& prepared_attempt,
      const RouteSelectionStatusDTO& successor_status) override {
    return journal_.RecordRouteEffectPrepared(active_owner_epoch, prepared_attempt,
                                              successor_status);
  }
  Result<std::uint64_t> RecordRouteEffectCompleted(
      const RequestOwnerEpoch& active_owner_epoch,
      const CCLCRouteEffectAttemptDTO& completed_attempt,
      const RouteSelectionStatusDTO& successor_status) override {
    return journal_.RecordRouteEffectCompleted(active_owner_epoch, completed_attempt,
                                               successor_status);
  }
  Result<std::uint64_t> RecordRouteSelectionStatus(const RequestOwnerEpoch& active_owner_epoch,
                                                   const RouteSelectionStatusDTO& status) override {
    return journal_.RecordRouteSelectionStatus(active_owner_epoch, status);
  }
  Result<std::optional<RouteSelectionStatusDTO>> LoadRetainedRouteSelectionStatus(
      const RouteSelectionTransactionId& route_selection_transaction_id) override {
    return journal_.LoadRetainedRouteSelectionStatus(route_selection_transaction_id);
  }
  Result<std::optional<ActiveControlConfigurationDTO>> LoadCurrentActiveControlConfiguration(
      const ControlDomainId& control_domain_id) override {
    return journal_.LoadCurrentActiveControlConfiguration(control_domain_id);
  }

  void ArmReadBarrier() {
    std::lock_guard lock(mutex_);
    block_after_read_ = true;
  }
  bool WaitForReadBarrier(std::chrono::nanoseconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return blocked_; });
  }
  void ReleaseReadBarrier() {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    changed_.notify_all();
  }

 private:
  SqliteCCLCJournal& journal_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool block_after_read_{};
  bool blocked_{};
  bool released_{};
};

TEST(ComponentLifecycleCoordinator, TargetRefusalSettlesActualClaimWithoutConsumingGeneration) {
  for (const bool lose_reply : {false, true}) {
    SCOPED_TRACE(lose_reply ? "lost release reply" : "ordinary release reply");
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename),
                              CellId("cell.1"), 8U);
    CCLCDomainReservationTable reservations;
    FakeSnapshotSource snapshots({Snapshot()});
    FakeCatalogSource catalogs(PrepareCatalog());
    auto port = std::make_shared<FakeBrandPort>(
        journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                         ComponentLifecycleStateDTO::kPrepared});
    FakeResolver resolver(port);
    TargetRefusalAdmissionPort kernel;
    FakeClock clock;
    clock.set(100);
    ManualScheduler scheduler;
    ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                              resolver, kernel, clock, scheduler);
    ASSERT_TRUE(coordinator.Start().ok());
    auto intent = PrepareIntent(coordinator.request_owner_epoch());
    intent.request_context.source_label = std::string(kLifecycleTargetSourceLabel);
    const ComponentLifecycleTargetExpectationDTO retired{intent.controllable_component_id, 1U};
    if (lose_reply) kernel.LoseNextReleaseReply();
    const auto refused = coordinator.SubmitTargetLifecycle(intent, retired);
    ASSERT_TRUE(refused.ok()) << refused.error().message;
    ASSERT_TRUE(refused.value().rejection);
    EXPECT_EQ(refused.value().rejection->code, "component_lifecycle_target_stale");
    EXPECT_EQ(refused.value().rejection->details.at("reason"), "target_retired");
    EXPECT_EQ(refused.value().rejection->details.at("effect"), "not_started");
    EXPECT_FALSE(refused.value().acceptance);
    EXPECT_FALSE(refused.value().status);
    ASSERT_TRUE(kernel.last_claim);
    ASSERT_TRUE(kernel.last_release);
    EXPECT_EQ(kernel.claim_acquires, 1U);
    EXPECT_EQ(kernel.claim_releases, 1U);
    EXPECT_EQ(kernel.last_release->settlement.settlement_kind,
              CellAdmissionSettlementKindDTO::kProvenNoEffect);
    EXPECT_EQ(kernel.last_release->settlement.evidence_digest,
              kernel.last_claim->intent.request_payload_digest);
    const auto receipt = kernel.GetCellAdmissionRecoveryReceipt(
        {kernel.last_claim->intent, kernel.last_claim, kernel.last_release});
    ASSERT_TRUE(receipt.ok());
    EXPECT_EQ(receipt.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact);
    ASSERT_TRUE(journal.ReadQuiescentOwner(coordinator.request_owner_epoch()).ok());
    EXPECT_TRUE(reservations.snapshot().empty());
    EXPECT_EQ(port->dispatch_count(), 0U);
    EXPECT_TRUE(coordinator.accepting_new_requests());
    CoordinatorLifecycleTargetDriverPorts production_ports(
        coordinator, snapshots, resolver, kernel,
        [&] { return journal.LoadComponentsWithUnsettledLifecycle(); });
    EXPECT_TRUE(production_ports.RetiredTargetWorkSettled(intent.controllable_component_id));

    // The retained request replays before both target and product checks.
    coordinator.CloseAdmission();
    const auto replay = coordinator.SubmitTargetLifecycle(
        intent, {ControllableComponentId("foreign.component"), 0U});
    ASSERT_FALSE(replay.ok());
    EXPECT_EQ(replay.error(), *refused.value().rejection);
    const auto retained = coordinator.ReadLifecycleRequestResult(intent);
    ASSERT_TRUE(retained.ok()) << retained.error().message;
    ASSERT_TRUE(retained.value());
    EXPECT_EQ(retained.value()->replay_status, RequestReplayStatusDTO::kReplayedExact);
    EXPECT_EQ(retained.value()->rejection, refused.value().rejection);
    EXPECT_FALSE(retained.value()->acceptance);
    EXPECT_FALSE(retained.value()->status);
    EXPECT_EQ(kernel.claim_acquires, 1U);
    EXPECT_EQ(kernel.claim_releases, 1U);
    EXPECT_EQ(port->dispatch_count(), 0U);
  }
}

TEST(ComponentLifecycleCoordinator, TargetRefusalLeavesFirstGenuineLifecycleGenerationAvailable) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  TargetRefusalAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  auto intent = PrepareIntent(coordinator.request_owner_epoch());
  intent.request_context.source_label = std::string(kLifecycleTargetSourceLabel);
  const auto refused =
      coordinator.SubmitTargetLifecycle(intent, {intent.controllable_component_id, 1U});
  ASSERT_TRUE(refused.ok() && refused.value().rejection);
  intent.request_context.request_id = RequestId("prepare.after-target-refusal");
  intent.request_context.source_label = "test";
  const auto accepted = coordinator.SubmitLifecycle(intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance);
  EXPECT_EQ(accepted.value().acceptance->lifecycle_transaction_id,
            LifecycleTransactionId("lifecycle.1"));
  EXPECT_EQ(accepted.value().acceptance->lifecycle_transaction_generation.value(), 1U);
  EXPECT_EQ(accepted.value().acceptance->component_lifecycle_generation.value(), 4U);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

TEST(ComponentLifecycleCoordinator, UnknownTargetRefusalReleaseRetainsClaimAndClosesAdmission) {
  for (const bool throw_release : {false, true}) {
    SCOPED_TRACE(throw_release ? "release exception" : "release refusal");
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename),
                              CellId("cell.1"), 8U);
    CCLCDomainReservationTable reservations;
    FakeSnapshotSource snapshots({Snapshot()});
    FakeCatalogSource catalogs(PrepareCatalog());
    auto port = std::make_shared<FakeBrandPort>(
        journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
    FakeResolver resolver(port);
    TargetRefusalAdmissionPort kernel;
    FakeClock clock;
    clock.set(100);
    ManualScheduler scheduler;
    ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                              resolver, kernel, clock, scheduler);
    ASSERT_TRUE(coordinator.Start().ok());
    kernel.refuse_release = !throw_release;
    kernel.throw_release = throw_release;
    auto intent = PrepareIntent(coordinator.request_owner_epoch());
    intent.request_context.source_label = std::string(kLifecycleTargetSourceLabel);
    const auto unresolved =
        coordinator.SubmitTargetLifecycle(intent, {intent.controllable_component_id, 1U});
    ASSERT_FALSE(unresolved.ok());
    EXPECT_EQ(unresolved.error().code, "component_lifecycle_claim_release_unknown");
    EXPECT_EQ(unresolved.error().details.at("reason"), "target_refusal_claim_release_unresolved");
    EXPECT_FALSE(coordinator.accepting_new_requests());
    EXPECT_TRUE(coordinator.admission_fault_closed());
    ASSERT_TRUE(kernel.last_claim && kernel.last_release);
    const auto held = kernel.GetCellAdmissionRecoveryReceipt(
        {kernel.last_claim->intent, kernel.last_claim, kernel.last_release});
    ASSERT_TRUE(held.ok());
    EXPECT_EQ(held.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
    EXPECT_FALSE(journal.ReadQuiescentOwner(coordinator.request_owner_epoch()).ok());
    CoordinatorLifecycleTargetDriverPorts production_ports(
        coordinator, snapshots, resolver, kernel,
        [&] { return journal.LoadComponentsWithUnsettledLifecycle(); });
    EXPECT_FALSE(production_ports.RetiredTargetWorkSettled(intent.controllable_component_id));
    const auto retained = coordinator.ReadLifecycleRequestResult(intent);
    ASSERT_TRUE(retained.ok() && retained.value() && retained.value()->rejection);
    EXPECT_EQ(retained.value()->rejection->details.at("reason"), "target_retired");
    EXPECT_EQ(port->dispatch_count(), 0U);
    EXPECT_TRUE(reservations.snapshot().empty());
    intent.request_context.request_id = RequestId("prepare.after-unresolved-target");
    const auto fresh = coordinator.SubmitLifecycle(intent);
    ASSERT_FALSE(fresh.ok());
    EXPECT_EQ(fresh.error().code, "component_lifecycle_admission_closed");
    EXPECT_EQ(kernel.claim_acquires, 1U);
  }
}

TEST(ComponentLifecycleCoordinator,
     RetiredOwnerSettlementRechecksGateAfterUnacceptedClaimReleaseFailure) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal durable(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  SettlementBarrierJournal journal(durable);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      durable, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(port);
  TargetRefusalAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  ASSERT_TRUE(coordinator.ReadLifecycleOwnerSettlement().ok());
  journal.ArmReadBarrier();
  auto sampled =
      std::async(std::launch::async, [&] { return coordinator.ReadLifecycleOwnerSettlement(); });
  struct ReleaseReadOnExit {
    SettlementBarrierJournal& journal;
    ~ReleaseReadOnExit() { journal.ReleaseReadBarrier(); }
  } release{journal};
  ASSERT_TRUE(journal.WaitForReadBarrier(std::chrono::seconds(2)));

  // The durable quiescence snapshot has ended; a new unaccepted claim can
  // now become unresolved before the reader takes its final memory fence.
  kernel.refuse_release = true;
  auto intent = PrepareIntent(coordinator.request_owner_epoch());
  intent.request_context.source_label = std::string(kLifecycleTargetSourceLabel);
  const auto refusal =
      coordinator.SubmitTargetLifecycle(intent, {intent.controllable_component_id, 1U});
  ASSERT_FALSE(refusal.ok());
  EXPECT_EQ(refusal.error().code, "component_lifecycle_claim_release_unknown");
  EXPECT_TRUE(coordinator.admission_fault_closed());
  EXPECT_FALSE(coordinator.accepting_new_requests());
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());

  journal.ReleaseReadBarrier();
  ASSERT_EQ(sampled.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  const auto stale_proof = sampled.get();
  ASSERT_FALSE(stale_proof.ok());
  EXPECT_EQ(stale_proof.error().code, "component_lifecycle_owner_settlement_unknown");
  ASSERT_TRUE(kernel.last_claim && kernel.last_release);
  const auto held = kernel.GetCellAdmissionRecoveryReceipt(
      {kernel.last_claim->intent, kernel.last_claim, kernel.last_release});
  ASSERT_TRUE(held.ok());
  EXPECT_EQ(held.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
  EXPECT_FALSE(durable.ReadQuiescentOwner(coordinator.request_owner_epoch()).ok());
  const auto terminal_membership = durable.LoadComponentsWithUnsettledLifecycle();
  ASSERT_TRUE(terminal_membership.ok());
  EXPECT_TRUE(terminal_membership.value().empty());
  CoordinatorLifecycleTargetDriverPorts production_ports(
      coordinator, snapshots, resolver, kernel,
      [&] { return durable.LoadComponentsWithUnsettledLifecycle(); });
  EXPECT_FALSE(production_ports.RetiredTargetWorkSettled(intent.controllable_component_id));
}

TEST(ComponentLifecycleCoordinator, KernelClaimRejectionDoesNotPoisonOwnerOrConsumeGeneration) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot(), Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  auto intent = PrepareIntent(coordinator.request_owner_epoch());
  kernel.RejectNextClaim(
      {"authoring_not_ready",
       "Not started — the cell is not idle",
       {{"reason", "component_lifecycle_program_not_idle_after_reset"},
        {"selection_revision", "9"}}});
  const auto rejected = coordinator.SubmitLifecycle(intent);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("selection_revision"), "9");
  EXPECT_TRUE(coordinator.accepting_new_requests());
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
  const auto absent = coordinator.ReadLifecycleRequestResult(intent);
  ASSERT_TRUE(absent.ok());
  ASSERT_TRUE(absent.value());
  ASSERT_TRUE(absent.value()->rejection);
  EXPECT_EQ(absent.value()->rejection->details.at("selection_revision"), "9");

  // Once the cell is idle a fresh request is admitted, not a replay of the
  // refused one. The first real transaction still has generation 1.
  intent.request_context.request_id = RequestId("prepare.after-release");
  const auto accepted = coordinator.SubmitLifecycle(intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  EXPECT_EQ(accepted.value().acceptance->lifecycle_transaction_generation.value(), 1U);
  const auto result = coordinator.ReadLifecycleRequestResult(intent);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(result.value());
  ASSERT_TRUE(result.value()->status);
  EXPECT_EQ(result.value()->status->terminal_result,
            ComponentLifecycleTerminalResultDTO::kSucceeded);
  EXPECT_EQ(port->dispatch_count(), 1U);
  // The same request identity with different content is a conflict, not a replay.
  auto conflict = intent;
  conflict.expected_component_lifecycle_generation = ComponentLifecycleGeneration(99U);
  const auto wrong = coordinator.ReadLifecycleRequestResult(conflict);
  ASSERT_TRUE(wrong.ok());
  ASSERT_TRUE(wrong.value());
  EXPECT_EQ(wrong.value()->replay_status, RequestReplayStatusDTO::kRequestIdConflict);
  EXPECT_EQ(port->dispatch_count(), 1U);
  intent.request_context.request_id = RequestId("prepare.unknown-acquire");
  intent.expected_component_lifecycle_generation =
      accepted.value().acceptance->component_lifecycle_generation;
  snapshots.SetSnapshot(Snapshot("after.prepare", ComponentLifecycleStateDTO::kPrepared,
                                 intent.expected_component_lifecycle_generation));
  kernel.RejectNextClaim({"owner_unavailable", "acquire reply unknown", {}});
  EXPECT_FALSE(coordinator.admission_fault_closed());
  const auto unknown = coordinator.SubmitLifecycle(intent);
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().code, "owner_unavailable");
  EXPECT_FALSE(coordinator.accepting_new_requests());
  EXPECT_TRUE(coordinator.admission_fault_closed());
  coordinator.CloseAdmission();
  EXPECT_TRUE(coordinator.admission_fault_closed());
  EXPECT_EQ(port->dispatch_count(), 1U);
}

TEST(ComponentLifecycleCoordinator, BringUpReadbackPreservesReplacedOwnerResultsWithoutEffects) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            1U);
  const auto original = journal.BeginOwnerEpoch(RequestOwnerEpoch("original.1"), 100, 2U);
  ASSERT_TRUE(original.ok());
  auto expired_intent = PrepareIntent(original.value().request_owner_epoch);
  expired_intent.intent = ComponentLifecycleIntentKindDTO::kEnable;
  const ContentDigest stable_digest(Sha('a'));
  const ErrorDTO rejection{"not_qualified", "original owner refused before effects", {{"effect", "not_started"}}};
  ASSERT_TRUE(journal
                  .RecordSelectedComponentBringUpRejection(original.value().request_owner_epoch,
                                                           expired_intent, stable_digest, rejection,
                                                           std::nullopt)
                  .ok());
  auto retained_intent = expired_intent;
  retained_intent.request_context.request_id = RequestId("retained.1");
  ASSERT_TRUE(journal
                  .RecordSelectedComponentBringUpRejection(original.value().request_owner_epoch,
                                                           retained_intent, stable_digest,
                                                           rejection, std::nullopt)
                  .ok());

  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(port);
  CountingAdmissionReadPort kernel;
  FakeClock clock;
  clock.set(200);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto receiver_epoch = coordinator.request_owner_epoch();
  ASSERT_NE(receiver_epoch, original.value().request_owner_epoch);
  coordinator.CloseAdmission();
  const auto admission_reads = kernel.admission_reads.load();
  const auto claim_acquires = kernel.claim_acquires.load();

  // No-admission results share the journal's immutable request membership.
  // Both private read paths must preserve its exact original classification.
  for (const bool delegated : {false, true}) {
    const auto read = [&](const ComponentLifecycleIntentDTO& intent, const ContentDigest& digest) {
      return delegated ? coordinator.ReadApplyComponentBringUpRequestResult(intent, digest)
                       : coordinator.ReadSelectedComponentBringUpRequestResult(intent, digest);
    };
    const auto retained = read(retained_intent, stable_digest);
    ASSERT_TRUE(retained.ok());
    ASSERT_TRUE(retained.value());
    EXPECT_EQ(retained.value()->replay_status, RequestReplayStatusDTO::kReplayedExact);
    EXPECT_EQ(retained.value()->rejection, rejection);
    EXPECT_FALSE(retained.value()->acceptance);
    const auto expired = read(expired_intent, stable_digest);
    ASSERT_TRUE(expired.ok());
    ASSERT_TRUE(expired.value());
    EXPECT_EQ(expired.value()->replay_status, RequestReplayStatusDTO::kRequestResultExpired);
    const auto conflict = read(retained_intent, ContentDigest(Sha('b')));
    ASSERT_TRUE(conflict.ok());
    ASSERT_TRUE(conflict.value());
    EXPECT_EQ(conflict.value()->replay_status, RequestReplayStatusDTO::kRequestIdConflict);
    auto absent_intent = retained_intent;
    absent_intent.request_context.request_id = RequestId("unseen.1");
    const auto full = read(absent_intent, stable_digest);
    ASSERT_TRUE(full.ok());
    ASSERT_TRUE(full.value());
    EXPECT_EQ(full.value()->replay_status, RequestReplayStatusDTO::kRequestLedgerFull);
    absent_intent.request_context.request_owner_epoch = receiver_epoch;
    const auto absent = read(absent_intent, stable_digest);
    ASSERT_TRUE(absent.ok());
    EXPECT_FALSE(absent.value());
  }
  EXPECT_EQ(coordinator.request_owner_epoch(), receiver_epoch);
  EXPECT_FALSE(coordinator.accepting_new_requests());
  EXPECT_EQ(kernel.admission_reads.load(), admission_reads);
  EXPECT_EQ(kernel.claim_acquires.load(), claim_acquires);
  EXPECT_EQ(snapshots.read_count(), 0U);
  EXPECT_EQ(catalogs.load_count(), 0U);
  EXPECT_EQ(resolver.resolve_count(), 0U);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

TEST(ComponentLifecycleCoordinator, BringUpReadbackValidatesInputsWithoutStartingAnOwner) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(port);
  CountingAdmissionReadPort kernel;
  FakeClock clock;
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  const auto intent = PrepareIntent(RequestOwnerEpoch("original.1"));
  for (const bool delegated : {false, true}) {
    const auto read = [&](const ComponentLifecycleIntentDTO& request, const ContentDigest& digest) {
      return delegated ? coordinator.ReadApplyComponentBringUpRequestResult(request, digest)
                       : coordinator.ReadSelectedComponentBringUpRequestResult(request, digest);
    };
    const auto absent = read(intent, ContentDigest(Sha('a')));
    ASSERT_TRUE(absent.ok());
    EXPECT_FALSE(absent.value());
    EXPECT_FALSE(read(intent, ContentDigest("")).ok());
    EXPECT_FALSE(read(intent, ContentDigest(Sha('A'))).ok());
    auto invalid = intent;
    invalid.controllable_component_id = ControllableComponentId("");
    const auto rejected = read(invalid, ContentDigest(Sha('a')));
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, "component_lifecycle_intent_invalid");
  }
  EXPECT_FALSE(coordinator.accepting_new_requests());
  EXPECT_EQ(kernel.admission_reads.load(), 0U);
  EXPECT_EQ(kernel.claim_acquires.load(), 0U);
  EXPECT_EQ(snapshots.read_count(), 0U);
  EXPECT_EQ(catalogs.load_count(), 0U);
  EXPECT_EQ(resolver.resolve_count(), 0U);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

TEST(ComponentLifecycleCoordinator, FullSettledLedgerRotatesTwiceAndJoinsLostReconcileReply) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            1U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  auto config = Config();
  config.request_membership_capacity = 2U;
  ComponentLifecycleCoordinator coordinator(config, journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  std::vector<RequestOwnerEpoch> namespaces{coordinator.request_owner_epoch()};
  for (std::uint64_t index = 0; index != 6U; ++index) {
    snapshots.SetSnapshot(Snapshot("snapshot." + std::to_string(index),
                                   ComponentLifecycleStateDTO::kPrepared,
                                   ComponentLifecycleGeneration(3U + index)));
    auto intent = PrepareIntent(coordinator.request_owner_epoch());
    intent.request_context.request_id = RequestId("request." + std::to_string(index));
    intent.expected_component_lifecycle_generation = ComponentLifecycleGeneration(3U + index);
    if (index == 2U) kernel.LoseNextReconcileReply();
    if (index == 4U) kernel.RejectNextReconcileInventory();
    auto accepted = coordinator.SubmitLifecycle(intent);
    if (index == 2U || index == 4U) {
      ASSERT_FALSE(accepted.ok());
      EXPECT_EQ(accepted.error().code, "cclc_request_namespace_refreshed");
      EXPECT_EQ(accepted.error().details.at("effect"), "not_started");
      EXPECT_TRUE(coordinator.accepting_new_requests());
      EXPECT_NE(coordinator.request_owner_epoch(), namespaces.back());
      namespaces.push_back(coordinator.request_owner_epoch());
      // Only the explicit no-effect result permits a fresh receiver-bound
      // request. An unknown response must not take this branch.
      intent.request_context.request_owner_epoch = coordinator.request_owner_epoch();
      accepted = coordinator.SubmitLifecycle(intent);
    }
    ASSERT_TRUE(accepted.ok()) << accepted.error().code << ": " << accepted.error().message;
    ASSERT_TRUE(accepted.value().acceptance);
    EXPECT_EQ(accepted.value().acceptance->lifecycle_transaction_generation.value(), index + 1U);
    ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
    EXPECT_TRUE(reservations.snapshot().empty());
  }
  EXPECT_EQ(namespaces.size(), 3U);
  EXPECT_EQ(port->dispatch_count(), 0U);  // Already-satisfied Prepare, not fabricated motion.
}

TEST(ComponentLifecycleCoordinator, FullButRunningIsProvenUnstartedNotAnUnknownEffect) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            1U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  auto port = std::make_shared<FakeBrandPort>(
      journal,
      std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                              ComponentLifecycleStateDTO::kPrepared},
      DispatchMode::kKnownSuccess, 1U, std::vector<LifecycleFactDTO>{}, [&] {
        entered.set_value();
        released.wait();
      });
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  auto config = Config();
  config.request_membership_capacity = 1U;
  ComponentLifecycleCoordinator coordinator(config, journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto old = coordinator.request_owner_epoch();
  const auto first = coordinator.SubmitLifecycle(PrepareIntent(old));
  const auto running = entered.get_future().wait_for(std::chrono::seconds(2));
  auto waiting = PrepareIntent(old);
  waiting.request_context.request_id = RequestId("capacity.waiting");
  const auto deferred = coordinator.SubmitLifecycle(waiting);
  const auto proof = coordinator.ReadLifecycleRequestResult(waiting);
  release.set_value();  // Always release the fixture worker before assertions can return.
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(running, std::future_status::ready);
  ASSERT_FALSE(deferred.ok());
  EXPECT_EQ(deferred.error().code, "cclc_request_admission_deferred");
  EXPECT_EQ(deferred.error().details.at("effect"), "not_started");
  ASSERT_FALSE(proof.ok());
  EXPECT_EQ(proof.error().code, "cclc_request_admission_deferred");
  EXPECT_EQ(coordinator.request_owner_epoch(), old);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto cutover = coordinator.SubmitLifecycle(waiting);
  ASSERT_FALSE(cutover.ok());
  EXPECT_EQ(cutover.error().code, "cclc_request_namespace_refreshed");
  const auto after = coordinator.ReadLifecycleRequestResult(waiting);
  ASSERT_FALSE(after.ok());
  EXPECT_EQ(after.error().details.at("effect"), "not_started");
  EXPECT_NE(coordinator.request_owner_epoch(), old);
  EXPECT_EQ(port->dispatch_count(), 1U);
}

TEST(ComponentLifecycleCoordinator, EvidenceAtExactDeadlineCannotCompleteLifecycleTransition) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  FakeClock clock;
  clock.set(100);
  auto port = std::make_shared<FakeBrandPort>(
      journal,
      std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                              ComponentLifecycleStateDTO::kPrepared},
      DispatchMode::kKnownSuccess, 1U, std::vector<LifecycleFactDTO>{},
      [&clock] { clock.set(1'000'100); });
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));

  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kFailed);
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  ASSERT_TRUE(status.value()->error.has_value());
  EXPECT_EQ(status.value()->error->code, "component_lifecycle_verification_timeout");
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

// Per-component time budget: with the product's 120 s budget for the
// component, a Prepare whose plan allows 1 ms is accepted with 120 s and is
// not timed out when its evidence arrives 30 s later.
TEST(ComponentLifecycleCoordinator, ComponentBudgetFromTheProductBoundsThePrepareDeadline) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  FakeClock clock;
  clock.set(100);
  auto port = std::make_shared<FakeBrandPort>(
      journal,
      std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                              ComponentLifecycleStateDTO::kPrepared},
      DispatchMode::kKnownSuccess, 1U, std::vector<LifecycleFactDTO>{},
      [&clock] { clock.set(30'000'000'100); });
  // The evidence that arrives 30 s later is a fresh observation then.
  port->set_fresh_until_ns(60'000'000'000LL);
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  ManualScheduler scheduler;
  const auto context =
      std::make_shared<const ComponentLifecycleProductContext>(ComponentLifecycleProductContext{
          snapshots, catalogs, resolver, std::nullopt, nullptr, std::nullopt, nullptr, nullptr,
          {}, {{"component.1", 120'000'000'000LL}}});
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, context, kernel,
                                            clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance);
  EXPECT_EQ(accepted.value().acceptance->timeout_duration_ns, 120'000'000'000LL);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));

  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded)
      << (status.value()->error ? status.value()->error->code : std::string());
  EXPECT_EQ(port->dispatch_count(), 1U);
}

TEST(ComponentLifecycleCoordinator, LostVerifyPollTimesOutAtTheIndependentDeadline) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  // The postcondition never holds, so verification waits on its poll.
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(port);
  TargetRefusalAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  constexpr TimePoint kDeadline = 1'000'100;  // accepted_at + plan timeout
  PollLosingScheduler scheduler(kDeadline);
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance);
  const auto& id = accepted.value().acceptance->lifecycle_transaction_id;
  ASSERT_TRUE(scheduler.WaitForLost(1U, std::chrono::seconds(2)));
  const auto waiting = coordinator.LoadLifecycleStatus(id);
  ASSERT_TRUE(waiting.ok() && waiting.value());
  EXPECT_EQ(waiting.value()->phase, ComponentLifecycleTransactionPhaseDTO::kVerifying);
  EXPECT_FALSE(waiting.value()->terminal_result);

  clock.set(kDeadline);
  scheduler.AdvanceTo(kDeadline);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status = coordinator.LoadLifecycleStatus(id);
  ASSERT_TRUE(status.ok() && status.value());
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kFailed);
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  ASSERT_TRUE(status.value()->error);
  EXPECT_EQ(status.value()->error->code, "component_lifecycle_verification_timeout");
  // Its post-dispatch observation is still fresh: the observed state counts.
  EXPECT_EQ(status.value()->current_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_FALSE(status.value()->error->details.contains("component_state"));
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_EQ(kernel.claim_releases, 1U);
  ASSERT_TRUE(kernel.last_claim && kernel.last_release);
  const auto receipt = kernel.GetCellAdmissionRecoveryReceipt(
      {kernel.last_claim->intent, kernel.last_claim, kernel.last_release});
  ASSERT_TRUE(receipt.ok());
  EXPECT_EQ(receipt.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact);
  EXPECT_TRUE(coordinator.accepting_new_requests());
}

// The failure settles after the last observation expired (fresh_until_ns
// 2'000'000 in FakeBrandPort): it is not the current state any more.
TEST(ComponentLifecycleCoordinator, ExpiredEvidenceAtSettlementIsReconstructedDeclared) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(port);
  TargetRefusalAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  constexpr TimePoint kDeadline = 1'000'100;
  PollLosingScheduler scheduler(kDeadline);
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok() && accepted.value().acceptance);
  const auto& id = accepted.value().acceptance->lifecycle_transaction_id;
  ASSERT_TRUE(scheduler.WaitForLost(1U, std::chrono::seconds(2)));

  clock.set(3'000'000);
  testing::internal::CaptureStderr();
  scheduler.AdvanceTo(kDeadline);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto output = testing::internal::GetCapturedStderr();
  const auto status = coordinator.LoadLifecycleStatus(id);
  ASSERT_TRUE(status.ok() && status.value());
  ASSERT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  EXPECT_EQ(status.value()->current_state, ComponentLifecycleStateDTO::kDeclared);
  ASSERT_TRUE(status.value()->error);
  ASSERT_TRUE(status.value()->error->details.contains("component_state"));
  EXPECT_EQ(status.value()->error->details.at("component_state"),
            kComponentLifecycleStateReconstructedCode);
  EXPECT_NE(output.find("manyforge_cclc_lifecycle_state_reconstructed"), std::string::npos)
      << output;
  const auto fact = journal.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.1"));
  ASSERT_TRUE(fact.ok()) << fact.error().message;
  EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kDeclared);
}

TEST(ComponentLifecycleCoordinator, CompletedTransactionLeavesNoPendingDeadline) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  WaitableManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  // Complete itself removes the never-dequeued deadline (before any drain),
  // so owner settlement (read by the target driver) does not wait for it.
  ASSERT_TRUE(scheduler.WaitForProvenCancel(std::chrono::seconds(2)));
  EXPECT_EQ(scheduler.pending_count(SchedulerPriority::Lifecycle), 0U);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  EXPECT_TRUE(coordinator.ReadLifecycleOwnerSettlement().ok());
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok() && status.value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
}

// The Real xArm arm's Recover shape: target `disabled`, a postcondition that
// admits either unarmed resting state, and one vendor clear.
[[nodiscard]] BrandLifecyclePlanCatalogDTO UnarmedRecoverCatalog(
    LifecyclePredicateDTO eligibility = Predicate(ComponentLifecycleStateDTO::kFaulted)) {
  auto plans = PrepareCatalog().plans;
  const LifecyclePredicateDTO unarmed{
      {ComponentLifecycleStateDTO::kDeclared, ComponentLifecycleStateDTO::kDisabled}, {}};
  plans.push_back({ComponentLifecycleIntentKindDTO::kRecover,
                   ComponentLifecycleStateDTO::kDisabled,
                   std::move(eligibility),
                   unarmed,
                   1'000'000,
                   {{LifecyclePrimitiveId("clear_error"),
                     Predicate(ComponentLifecycleStateDTO::kFaulted), unarmed}}});
  auto catalog = MakeBrandLifecyclePlanCatalog(
      ProviderManifestId("manifest.1"), ProviderManifestRevision(Sha('1')), std::move(plans));
  if (!catalog.ok()) {
    ADD_FAILURE() << catalog.error().code << ": " << catalog.error().message;
    throw std::runtime_error("failed to construct unarmed Recover catalog");
  }
  return std::move(catalog.value());
}

struct UnarmedRecoverRun final {
  std::optional<ComponentLifecycleStatusDTO> status;
  std::size_t dispatches = 0U;
  std::size_t claim_releases = 0U;
  bool reservations_empty = false;
  bool accepting = false;
  std::string stderr_output;
  std::optional<ComponentLifecycleStateDTO> persisted_state;
};

// Live 2026-10-28 (Real Lite 6): a Prepare blocked by an emergency-stop error
// left the arm Declared; after the operator's Recover the target driver's
// Recover cleared the error and the arm reported Declared again.
[[nodiscard]] UnarmedRecoverRun RunUnarmedRecover(
    DispatchMode mode,
    std::vector<ComponentLifecycleStateDTO> observations = {ComponentLifecycleStateDTO::kFaulted,
                                                           ComponentLifecycleStateDTO::kDeclared},
    const BrandLifecyclePlanCatalogDTO& catalog = UnarmedRecoverCatalog(),
    std::shared_ptr<const ILifecycleAdmission> admission = nullptr) {
  UnarmedRecoverRun run;
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot("snapshot.recover", ComponentLifecycleStateDTO::kDeclared,
                                         ComponentLifecycleGeneration(3U), catalog)});
  FakeCatalogSource catalogs(catalog);
  auto port = std::make_shared<FakeBrandPort>(journal, std::move(observations), mode);
  port->set_catalog(catalog);
  if (admission) port->set_admission(std::move(admission));
  FakeResolver resolver(port, catalog);
  TargetRefusalAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  if (!coordinator.Start().ok()) {
    ADD_FAILURE() << "coordinator did not start";
    return run;
  }
  auto intent = PrepareIntent(coordinator.request_owner_epoch());
  intent.intent = ComponentLifecycleIntentKindDTO::kRecover;
  testing::internal::CaptureStderr();
  const auto accepted = coordinator.SubmitLifecycle(intent);
  if (!accepted.ok() || !accepted.value().acceptance) {
    run.stderr_output = testing::internal::GetCapturedStderr();
    ADD_FAILURE() << (accepted.ok() ? "no acceptance" : accepted.error().message);
    return run;
  }
  const auto& id = accepted.value().acceptance->lifecycle_transaction_id;
  static_cast<void>(coordinator.DrainFor(std::chrono::milliseconds(500)));
  // The fixed deadline passes: a transaction left pending would stay so.
  clock.set(1'000'100);
  scheduler.advance_to(1'000'100);
  static_cast<void>(coordinator.DrainFor(std::chrono::milliseconds(500)));
  run.stderr_output = testing::internal::GetCapturedStderr();
  const auto status = coordinator.LoadLifecycleStatus(id);
  if (status.ok() && status.value()) run.status = *status.value();
  run.dispatches = port->dispatch_count();
  run.claim_releases = kernel.claim_releases;
  run.reservations_empty = reservations.snapshot().empty();
  run.accepting = coordinator.accepting_new_requests();
  const auto fact = journal.LoadCurrentComponentLifecycleFact(intent.controllable_component_id);
  if (fact.ok()) run.persisted_state = fact.value().current_state;
  return run;
}

TEST(ComponentLifecycleCoordinator, RecoverFromDeclaredRecordsTheObservedRestingState) {
  const auto run = RunUnarmedRecover(DispatchMode::kKnownSuccess);
  EXPECT_EQ(run.stderr_output.find("manyforge_cclc_lifecycle_stall"), std::string::npos)
      << run.stderr_output;
  ASSERT_TRUE(run.status);
  EXPECT_EQ(run.status->phase, ComponentLifecycleTransactionPhaseDTO::kSucceeded);
  EXPECT_EQ(run.status->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  EXPECT_EQ(run.status->requested_state.state, ComponentLifecycleStateDTO::kDisabled);
  ASSERT_EQ(run.status->domains.size(), 1U);
  // The provider's observation, not the requested target, in the domain,
  // the component aggregate and the durable current fact.
  EXPECT_EQ(run.status->domains.front().current_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_EQ(run.status->current_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_EQ(run.persisted_state, ComponentLifecycleStateDTO::kDeclared);
  ASSERT_EQ(run.status->domains.front().primitive_attempts.size(), 1U);
  EXPECT_EQ(run.status->domains.front().primitive_attempts.front().outcome,
            LifecycleTransitionOutcomeDTO::kSucceeded);
  EXPECT_EQ(run.dispatches, 1U);
  EXPECT_EQ(run.claim_releases, 1U);
  EXPECT_TRUE(run.reservations_empty);
  EXPECT_TRUE(run.accepting);
}

// Already satisfied at Initialize: no primitive runs, and the observed
// resting state (Declared, accepted by the plan) is kept, not the target.
TEST(ComponentLifecycleCoordinator, AlreadySatisfiedRecoverKeepsTheObservedRestingState) {
  const auto run = RunUnarmedRecover(
      DispatchMode::kKnownSuccess, {ComponentLifecycleStateDTO::kDeclared},
      UnarmedRecoverCatalog({{ComponentLifecycleStateDTO::kDeclared,
                              ComponentLifecycleStateDTO::kFaulted,
                              ComponentLifecycleStateDTO::kDisabled},
                             {}}));
  ASSERT_TRUE(run.status);
  EXPECT_EQ(run.status->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  EXPECT_EQ(run.status->requested_state.state, ComponentLifecycleStateDTO::kDisabled);
  ASSERT_EQ(run.status->domains.size(), 1U);
  EXPECT_EQ(run.status->domains.front().current_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_TRUE(run.status->domains.front().primitive_attempts.empty());
  EXPECT_EQ(run.status->current_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_EQ(run.persisted_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_EQ(run.dispatches, 0U);
  EXPECT_EQ(run.claim_releases, 1U);
}

// The Real xArm gripper's Recover plan accepts any healthy state: a gripper
// still Active is already satisfied. Its domain keeps Active; the component
// records Prepared (a Recover never makes it command-ready).
TEST(ComponentLifecycleCoordinator, AlreadySatisfiedRecoverAtActiveRecordsPreparedComponent) {
  auto plans = PrepareCatalog().plans;
  const LifecyclePredicateDTO healthy{
      {ComponentLifecycleStateDTO::kDeclared, ComponentLifecycleStateDTO::kPrepared,
       ComponentLifecycleStateDTO::kActive, ComponentLifecycleStateDTO::kDisabled},
      {}};
  plans.push_back({ComponentLifecycleIntentKindDTO::kRecover,
                   ComponentLifecycleStateDTO::kDisabled, healthy, healthy, 1'000'000, {}});
  auto made = MakeBrandLifecyclePlanCatalog(ProviderManifestId("manifest.1"),
                                            ProviderManifestRevision(Sha('1')), std::move(plans));
  ASSERT_TRUE(made.ok()) << made.error().message;
  const auto run = RunUnarmedRecover(DispatchMode::kKnownSuccess,
                                     {ComponentLifecycleStateDTO::kActive}, made.value());
  EXPECT_EQ(run.stderr_output.find("manyforge_cclc_lifecycle_stall"), std::string::npos)
      << run.stderr_output;
  ASSERT_TRUE(run.status);
  EXPECT_EQ(run.status->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  ASSERT_EQ(run.status->domains.size(), 1U);
  EXPECT_EQ(run.status->domains.front().current_state, ComponentLifecycleStateDTO::kActive);
  EXPECT_EQ(run.status->current_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(run.persisted_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(run.dispatches, 0U);
}

// D8 at transition level: an action that lost its connection ends the
// transaction as connection lost, with the adapter's typed error.
TEST(ComponentLifecycleCoordinator, ConnectionLostActionEndsTheTransactionAsConnectionLost) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared},
      DispatchMode::kConnectionLost);
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok() && status.value());
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kFailed);
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kConnectionLost);
  ASSERT_TRUE(status.value()->error);
  EXPECT_EQ(status.value()->error->code, "robot_controller_unresponsive");
  ASSERT_EQ(status.value()->domains.front().primitive_attempts.size(), 1U);
  EXPECT_EQ(status.value()->domains.front().primitive_attempts.front().outcome,
            LifecycleTransitionOutcomeDTO::kConnectionLost);
  EXPECT_TRUE(status.value()->domains.front().observed_after);
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

// Feedback events go to the domain status while the action runs, stay in the
// terminal row and history, and each finished substep logs one line.
TEST(ComponentLifecycleCoordinator, FeedbackEventsAreJournaledInTheDomainStatusAndHistory) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  port->set_emit_events(true);
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  testing::internal::CaptureStderr();
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto output = testing::internal::GetCapturedStderr();
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok() && status.value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  const auto& domain = status.value()->domains.front();
  ASSERT_EQ(domain.primitive_attempts.size(), 1U);
  const auto& attempt_id = domain.primitive_attempts.front().lifecycle_primitive_attempt_id;
  ASSERT_EQ(domain.substep_events.size(), 2U);
  EXPECT_EQ(domain.substep_events.front().phase, LifecycleSubstepEventDTO::Phase::kStarted);
  EXPECT_EQ(domain.substep_events.back().phase, LifecycleSubstepEventDTO::Phase::kFinished);
  EXPECT_EQ(domain.substep_events.back().outcome, LifecycleTransitionOutcomeDTO::kSucceeded);
  for (const auto& event : domain.substep_events) EXPECT_EQ(event.attempt_id, attempt_id);
  ASSERT_TRUE(domain.observed_before);
  ASSERT_TRUE(domain.observed_after);
  EXPECT_EQ(domain.observed_after->observed_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_NE(output.find("manyforge_lifecycle_substep transaction="), std::string::npos) << output;
  EXPECT_EQ(output.find("manyforge_lifecycle_substep_event_refused"), std::string::npos) << output;
  const auto history =
      journal.LoadRetainedComponentLifecycleHistory(ControllableComponentId("component.1"));
  ASSERT_TRUE(history.ok()) << history.error().message;
  const auto retained = std::find_if(
      history.value().begin(), history.value().end(), [&](const auto& row) {
        return row.acceptance.lifecycle_transaction_id ==
               accepted.value().acceptance->lifecycle_transaction_id;
      });
  ASSERT_NE(retained, history.value().end());
  EXPECT_EQ(retained->domains.front().substep_events, domain.substep_events);
}

/// An adapter whose own answer differs from the catalog the CCLC product
/// serves: already done for any intent, and Prepared as the only Recover
/// target.
class AlreadyDoneAdmission final : public ILifecycleAdmission {
 public:
  Result<LifecycleAdmissionDTO> Evaluate(ComponentLifecycleIntentKindDTO, ComponentLifecycleStateDTO,
                                         const BrandLifecycleObservationDTO&) const override {
    return Result<LifecycleAdmissionDTO>::Ok(
        {LifecyclePostconditionResultDTO::kSatisfied, std::nullopt});
  }
  Result<LifecycleAdmissionDTO> Evaluate(ComponentLifecycleIntentKindDTO, ComponentLifecycleStateDTO,
                                         const LifecycleDomainHealthDTO&) const override {
    return Result<LifecycleAdmissionDTO>::Ok(
        {LifecyclePostconditionResultDTO::kSatisfied, std::nullopt});
  }
  Result<std::array<LifecyclePostconditionResultDTO, 3>> RecoverTargets(
      const BrandLifecycleObservationDTO&) const override {
    return Result<std::array<LifecyclePostconditionResultDTO, 3>>::Ok(
        {LifecyclePostconditionResultDTO::kNotSatisfied,
         LifecyclePostconditionResultDTO::kSatisfied,
         LifecyclePostconditionResultDTO::kNotSatisfied});
  }
};

// V2-1: CCLC asks the adapter, not the catalog: its already-done answer
// succeeds without a dispatch where the catalog would select a step.
TEST(ComponentLifecycleCoordinator, InitializationUsesTheAdaptersOwnAdmission) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
  port->set_admission(std::make_shared<AlreadyDoneAdmission>());
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok() && status.value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_TRUE(status.value()->domains.front().primitive_attempts.empty());
}

// The Recover target comes from the adapter's per-target answers: Prepared,
// which the served catalog has no Recover plan for.
TEST(ComponentLifecycleCoordinator, RecoverTargetUsesTheAdaptersOwnAnswers) {
  const auto run = RunUnarmedRecover(DispatchMode::kKnownSuccess,
                                     {ComponentLifecycleStateDTO::kDeclared},
                                     UnarmedRecoverCatalog(),
                                     std::make_shared<AlreadyDoneAdmission>());
  ASSERT_TRUE(run.status);
  EXPECT_EQ(run.status->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  EXPECT_EQ(run.status->requested_state.state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(run.dispatches, 0U);
}

struct DisableEligibilityRun final {
  std::optional<ComponentLifecycleStatusDTO> status;
  std::vector<std::string> calls;
  bool command_eligible{};
  std::size_t dispatches{};
};

// A Disable of an Active component whose action fails; the port answers with
// the Enable-capable catalog so "fully Active" can be judged.
[[nodiscard]] DisableEligibilityRun RunFailedDisable(
    std::vector<ComponentLifecycleStateDTO> observations) {
  DisableEligibilityRun run;
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots(OrdinaryDisableSnapshots());
  FakeCatalogSource catalogs(DisableCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::move(observations), DispatchMode::kKnownFailure, 1U,
      std::vector<LifecycleFactDTO>{}, std::function<void()>{},
      ErrorDTO{"execution_rejected", "the controller manager refused the switch",
               {{"reason", "ros2_control_deactivation_refused"}}});
  port->set_catalog(EnableCatalog());
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  FakeReadinessPort readiness;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler, std::nullopt,
                                            &readiness);
  if (!coordinator.Start().ok()) {
    ADD_FAILURE() << "coordinator did not start";
    return run;
  }
  const auto accepted =
      coordinator.SubmitLifecycle(DisableIntent(coordinator.request_owner_epoch()));
  if (!accepted.ok() || !accepted.value().acceptance) {
    ADD_FAILURE() << (accepted.ok() ? "no acceptance" : accepted.error().message);
    return run;
  }
  static_cast<void>(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  if (status.ok() && status.value()) run.status = *status.value();
  run.calls = readiness.calls();
  run.command_eligible = readiness.state().command_eligible;
  run.dispatches = port->dispatch_count();
  return run;
}

// VB2-O1/O2: after a failed action, current status decides the recorded
// state and Kernel eligibility: still fully Active is recorded Active.
TEST(ComponentLifecycleCoordinator, FailedDisableLeavingTheComponentFullyActiveStaysCommandable) {
  const auto run = RunFailedDisable(
      {ComponentLifecycleStateDTO::kActive, ComponentLifecycleStateDTO::kActive});
  ASSERT_TRUE(run.status);
  EXPECT_EQ(run.status->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  EXPECT_EQ(run.dispatches, 1U);
  EXPECT_EQ(run.calls, (std::vector<std::string>{"close", "publish_open"}));
  EXPECT_TRUE(run.command_eligible);
  EXPECT_EQ(run.status->current_state, ComponentLifecycleStateDTO::kActive);
  EXPECT_EQ(run.status->domains.front().current_state, ComponentLifecycleStateDTO::kActive);
  ASSERT_TRUE(run.status->error);
  EXPECT_FALSE(run.status->error->details.contains("component_state"));
}

TEST(ComponentLifecycleCoordinator, FailedDisableLeavingTheComponentNotActiveStaysClosed) {
  const auto run = RunFailedDisable(
      {ComponentLifecycleStateDTO::kActive, ComponentLifecycleStateDTO::kPrepared});
  ASSERT_TRUE(run.status);
  EXPECT_EQ(run.status->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  EXPECT_EQ(run.dispatches, 1U);
  EXPECT_EQ(run.calls, (std::vector<std::string>{"close", "publish_closed"}));
  EXPECT_FALSE(run.command_eligible);
  EXPECT_EQ(run.status->current_state, ComponentLifecycleStateDTO::kPrepared);
}

// A refusal before any dispatch keeps its predecessor state (VB2-O1) and
// today's eligibility rule: the Active component reopens.
TEST(ComponentLifecycleCoordinator, PreDispatchRefusalKeepsThePredecessorStateAndEligibility) {
  const auto run = RunFailedDisable({ComponentLifecycleStateDTO::kFaulted});
  ASSERT_TRUE(run.status);
  EXPECT_EQ(run.status->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  ASSERT_TRUE(run.status->error);
  EXPECT_EQ(run.status->error->code, "component_lifecycle_transition_missing");
  EXPECT_EQ(run.dispatches, 0U);
  EXPECT_TRUE(run.status->domains.front().primitive_attempts.empty());
  EXPECT_FALSE(run.status->domains.front().observed_after);
  EXPECT_EQ(run.status->current_state, ComponentLifecycleStateDTO::kActive);
  EXPECT_EQ(run.calls, (std::vector<std::string>{"close", "publish_open"}));
  EXPECT_TRUE(run.command_eligible);
}

TEST(ComponentLifecycleStatus, OnlyRecoverSucceedsAtAnotherRestingState) {
  const auto run = RunUnarmedRecover(DispatchMode::kKnownSuccess);
  ASSERT_TRUE(run.status);
  EXPECT_TRUE(run.status->locally_valid());
  // The same succeeded status under a Disable: valid only at Disabled.
  auto disable = *run.status;
  disable.accepted_intent.intent = ComponentLifecycleIntentKindDTO::kDisable;
  EXPECT_FALSE(disable.locally_valid());
  disable.domains.front().current_state = ComponentLifecycleStateDTO::kDisabled;
  EXPECT_TRUE(disable.locally_valid());
  // A healthy domain may settle Active (Real xArm gripper); never faulted.
  auto active = *run.status;
  active.domains.front().current_state = ComponentLifecycleStateDTO::kActive;
  EXPECT_TRUE(active.locally_valid());
  auto faulted = *run.status;
  faulted.domains.front().current_state = ComponentLifecycleStateDTO::kFaulted;
  EXPECT_FALSE(faulted.locally_valid());
}

// A Prepare whose plan postcondition admits a state other than Prepared (or
// Active): the verified progress is not a valid status. It fails typed and
// releases its claim instead of a journal refusal that leaves it pending.
TEST(ComponentLifecycleCoordinator, UnrecordableProgressFailsClosedInsteadOfStayingPending) {
  auto plans = PrepareCatalog().plans;
  const LifecyclePredicateDTO armed{
      {ComponentLifecycleStateDTO::kPrepared, ComponentLifecycleStateDTO::kDisabled}, {}};
  plans.front().requested_postcondition = armed;
  plans.front().transitions.front().postcondition_predicate = armed;
  auto made = MakeBrandLifecyclePlanCatalog(ProviderManifestId("manifest.1"),
                                            ProviderManifestRevision(Sha('1')), std::move(plans));
  ASSERT_TRUE(made.ok()) << made.error().message;
  const auto catalog = made.value();
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot("snapshot.prepare", ComponentLifecycleStateDTO::kDeclared,
                                         ComponentLifecycleGeneration(3U), catalog)});
  FakeCatalogSource catalogs(catalog);
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kDisabled});
  port->set_catalog(catalog);
  FakeResolver resolver(port, catalog);
  TargetRefusalAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  testing::internal::CaptureStderr();
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok() && accepted.value().acceptance);
  static_cast<void>(coordinator.DrainFor(std::chrono::milliseconds(500)));
  const auto output = testing::internal::GetCapturedStderr();
  EXPECT_EQ(output.find("reason=progress_journal_refused"), std::string::npos) << output;
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok() && status.value());
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kFailed);
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  ASSERT_TRUE(status.value()->error);
  EXPECT_EQ(status.value()->error->code, "component_lifecycle_progress_status_invalid");
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_EQ(kernel.claim_releases, 1U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

TEST(ComponentLifecycleCoordinator, DequeuedDeadlineCallbackKeepsOwnerSettlementOpenUntilItReturns) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  // First verification is unsatisfied (poll); the polled observation succeeds.
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  constexpr TimePoint kDeadline = 1'000'100;
  PausingDeadlineScheduler scheduler(kDeadline);
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok() && accepted.value().acceptance);
  const auto& id = accepted.value().acceptance->lifecycle_transaction_id;
  ASSERT_TRUE(scheduler.WaitForScheduled(2U, std::chrono::seconds(2)));  // deadline + poll

  // One advance dequeues both callbacks before running either: the poll
  // completes the transaction while the deadline callback is held dequeued.
  auto advancer = std::async(std::launch::async, [&] { scheduler.AdvanceTo(kDeadline); });
  ASSERT_TRUE(scheduler.WaitForPaused(std::chrono::seconds(2)));
  // Barrier: Complete attempted the cancel after the callback was dequeued.
  ASSERT_TRUE(scheduler.WaitForCancelAttempt(std::chrono::seconds(2)));
  // The held callback stays tracked, so settlement is unavailable on every
  // read while it is held (also once the worker tail has returned).
  for (int read = 0; read < 10'000; ++read) {
    ASSERT_FALSE(coordinator.ReadLifecycleOwnerSettlement().ok()) << read;
    std::this_thread::yield();
  }

  scheduler.Release();
  advancer.get();
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  EXPECT_TRUE(coordinator.ReadLifecycleOwnerSettlement().ok());
  const auto status = coordinator.LoadLifecycleStatus(id);
  ASSERT_TRUE(status.ok() && status.value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  EXPECT_EQ(port->dispatch_count(), 1U);
}

TEST(ComponentLifecycleCoordinator, FailedDrainKeepsADequeuedDeadlineCallbackTracked) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  constexpr TimePoint kDeadline = 1'000'100;
  PausingDeadlineScheduler scheduler(kDeadline);
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok() && accepted.value().acceptance);
  ASSERT_TRUE(scheduler.WaitForScheduled(2U, std::chrono::seconds(2)));  // deadline + poll
  auto advancer = std::async(std::launch::async, [&] { scheduler.AdvanceTo(kDeadline); });
  ASSERT_TRUE(scheduler.WaitForPaused(std::chrono::seconds(2)));
  ASSERT_TRUE(scheduler.WaitForCancelAttempt(std::chrono::seconds(2)));

  // DrainFor reaches its cancel and Lifecycle-lane drain (worker idle, no
  // active transaction); the held callback makes the drain fail.
  EXPECT_FALSE(coordinator.DrainFor(std::chrono::seconds(2)));
  ASSERT_TRUE(scheduler.drain_attempted());
  // The failed drain must not have forgotten the dequeued callback.
  EXPECT_FALSE(coordinator.ReadLifecycleOwnerSettlement().ok());

  scheduler.Release();
  advancer.get();
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  EXPECT_TRUE(coordinator.ReadLifecycleOwnerSettlement().ok());
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok() && status.value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
}

TEST(ComponentLifecycleCoordinator, RefusedTerminalReadinessIsLoggedAndTheDeadlineDoesNotAct) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  const auto catalog = EnableCatalog();
  FakeSnapshotSource snapshots({Snapshot("snapshot.enable", ComponentLifecycleStateDTO::kPrepared,
                                         ComponentLifecycleGeneration(3U), catalog)});
  FakeCatalogSource catalogs(catalog);
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared,
                                                       ComponentLifecycleStateDTO::kActive});
  port->set_catalog(catalog);
  FakeResolver resolver(port, catalog);
  TargetRefusalAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  FakeReadinessPort readiness;
  // Neither the enabled nor the closed terminal result is read back.
  readiness.FailNextPublications(2U);
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler, std::nullopt,
                                            &readiness);
  ASSERT_TRUE(coordinator.Start().ok());
  auto intent = PrepareIntent(coordinator.request_owner_epoch());
  intent.intent = ComponentLifecycleIntentKindDTO::kEnable;
  testing::internal::CaptureStderr();
  const auto accepted = coordinator.SubmitLifecycle(intent);
  ASSERT_TRUE(accepted.ok() && accepted.value().acceptance);
  const auto& id = accepted.value().acceptance->lifecycle_transaction_id;
  // The worker goes idle; the unverified transaction stays active.
  EXPECT_FALSE(coordinator.DrainFor(std::chrono::milliseconds(200)));
  const auto stalled = coordinator.LoadLifecycleStatus(id);
  ASSERT_TRUE(stalled.ok() && stalled.value());
  clock.set(1'000'100);
  scheduler.advance_to(1'000'100);
  EXPECT_FALSE(coordinator.DrainFor(std::chrono::milliseconds(200)));
  const auto output = testing::internal::GetCapturedStderr();

  const std::string line = "manyforge_cclc_lifecycle_stall transaction=" + id.value() +
                           " generation=" +
                           std::to_string(
                               accepted.value().acceptance->lifecycle_transaction_generation.value()) +
                           " reason=terminal_readiness_refused error=invalid_state_transition\n";
  const auto first = output.find(line);
  ASSERT_NE(first, std::string::npos) << output;
  EXPECT_EQ(output.find(line, first + line.size()), std::string::npos) << output;
  // Fail closed: no terminal row, claim held, admission closed; the deadline
  // pass does not act on the refused, unrecorded result.
  const auto after = coordinator.LoadLifecycleStatus(id);
  ASSERT_TRUE(after.ok() && after.value());
  EXPECT_FALSE(after.value()->terminal_result);
  EXPECT_EQ(after.value()->status_sequence, stalled.value()->status_sequence);
  EXPECT_EQ(readiness.calls(), (std::vector<std::string>{"close", "publish_open", "publish_closed"}));
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_EQ(kernel.claim_releases, 0U);
  EXPECT_FALSE(coordinator.accepting_new_requests());
}

TEST(ComponentLifecycleCoordinator, UnrepresentableDeadlineFailsBeforeDurableAcceptance) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(std::numeric_limits<std::int64_t>::max() - 500'000);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  const auto intent = PrepareIntent(coordinator.request_owner_epoch());
  const auto accepted = coordinator.SubmitLifecycle(intent);
  ASSERT_FALSE(accepted.ok());
  EXPECT_EQ(accepted.error().code, "component_lifecycle_deadline_overflow");
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_EQ(port->dispatch_count(), 0U);
  const auto inspected = journal.InspectLifecycleRequest(coordinator.request_owner_epoch(), intent);
  ASSERT_TRUE(inspected.ok());
  ASSERT_TRUE(inspected.value());
  EXPECT_FALSE(inspected.value()->acceptance);
  ASSERT_TRUE(inspected.value()->rejection);
  EXPECT_EQ(inspected.value()->rejection->details.at("effect"), "not_started");
}

TEST(ComponentLifecycleCoordinator,
     ExactReplayStillWorksAfterNewAdmissionClosesWithoutSecondDispatch) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  const auto first = coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  coordinator.CloseAdmission();
  const auto replay = coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.value().acceptance->lifecycle_transaction_id,
            first.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(replay.value().status.has_value());
  EXPECT_EQ(replay.value().status->phase, ComponentLifecycleTransactionPhaseDTO::kSucceeded);
  EXPECT_EQ(port->dispatch_count(), 0U);
}

TEST(ComponentLifecycleCoordinator, SnapshotChangeAfterReservationRejectsAndReleasesWholeScope) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot("snapshot.1"), Snapshot("snapshot.changed")});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  const auto result = coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "component_lifecycle_snapshot_changed_during_admission");
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_EQ(port->dispatch_count(), 0U);
}

TEST(ComponentLifecycleCoordinator,
     ConcurrentDisjointAdmissionsMintGapFreeTransactionsAndExactComponentWatermarks) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  auto first = DisjointComponent("a", "concurrent-request.a");
  auto second = DisjointComponent("b", "concurrent-request.b");
  ASSERT_TRUE(first.snapshot.locally_valid());
  ASSERT_TRUE(first.intent.locally_valid());
  ASSERT_TRUE(second.snapshot.locally_valid());
  ASSERT_TRUE(second.intent.locally_valid());

  {
    CCLCDomainReservationTable reservations;
    RoutedSnapshotSource snapshots({first.snapshot, second.snapshot});
    FakeCatalogSource catalogs(PrepareCatalog());
    auto port = std::make_shared<FakeBrandPort>(
        journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared});
    RoutedResolver resolver(port, {first.snapshot.provider_occurrences.front(),
                                   second.snapshot.provider_occurrences.front()});
    FakeCellAdmissionPort kernel;
    FakeClock clock;
    clock.set(100);
    ManualScheduler scheduler;
    ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                              resolver, kernel, clock, scheduler);

    ASSERT_TRUE(coordinator.Start().ok());
    first.intent.request_context.request_owner_epoch = coordinator.request_owner_epoch();
    second.intent.request_context.request_owner_epoch = coordinator.request_owner_epoch();
    std::optional<Result<CCLCLifecycleRequestResultDTO>> first_result;
    std::optional<Result<CCLCLifecycleRequestResultDTO>> second_result;
    std::thread first_submitter(
        [&] { first_result.emplace(coordinator.SubmitLifecycle(first.intent)); });
    std::thread second_submitter(
        [&] { second_result.emplace(coordinator.SubmitLifecycle(second.intent)); });
    first_submitter.join();
    second_submitter.join();

    ASSERT_TRUE(first_result.has_value());
    ASSERT_TRUE(first_result->ok())
        << first_result->error().code << ": " << first_result->error().message;
    ASSERT_TRUE(first_result->value().acceptance.has_value());
    ASSERT_TRUE(second_result.has_value());
    ASSERT_TRUE(second_result->ok())
        << second_result->error().code << ": " << second_result->error().message;
    ASSERT_TRUE(second_result->value().acceptance.has_value());
    EXPECT_EQ(first_result->value().acceptance->component_lifecycle_generation,
              ComponentLifecycleGeneration(4U));
    EXPECT_EQ(second_result->value().acceptance->component_lifecycle_generation,
              ComponentLifecycleGeneration(4U));
    std::vector<std::uint64_t> transaction_generations{
        first_result->value().acceptance->lifecycle_transaction_generation.value(),
        second_result->value().acceptance->lifecycle_transaction_generation.value()};
    std::sort(transaction_generations.begin(), transaction_generations.end());
    EXPECT_EQ(transaction_generations, (std::vector<std::uint64_t>{1U, 2U}));
    ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
    EXPECT_TRUE(reservations.snapshot().empty());
  }

  const auto recovery = journal.BeginOwnerEpoch(RequestOwnerEpoch("cclc-owner.2"), 200,
                                                Config().request_membership_capacity);
  ASSERT_TRUE(recovery.ok()) << recovery.error().code << ": " << recovery.error().message;
  EXPECT_EQ(recovery.value().highest_lifecycle_transaction_generation, 2U);
  auto watermarks = recovery.value().component_watermarks;
  std::sort(watermarks.begin(), watermarks.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.controllable_component_id < rhs.controllable_component_id;
  });
  ASSERT_EQ(watermarks.size(), 2U);
  EXPECT_EQ(watermarks[0].controllable_component_id, ControllableComponentId("component.a"));
  EXPECT_EQ(watermarks[0].component_lifecycle_generation, ComponentLifecycleGeneration(4U));
  EXPECT_EQ(watermarks[1].controllable_component_id, ControllableComponentId("component.b"));
  EXPECT_EQ(watermarks[1].component_lifecycle_generation, ComponentLifecycleGeneration(4U));
}

TEST(ComponentLifecycleCoordinator,
     CloseAdmissionDuringPostReservationRereadPreventsPersistenceAndReleasesReservation) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  BlockingRereadSnapshotSource snapshots(Snapshot());
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  const auto intent = PrepareIntent(coordinator.request_owner_epoch());
  std::optional<Result<CCLCLifecycleRequestResultDTO>> submitted;
  std::thread submitter([&] { submitted.emplace(coordinator.SubmitLifecycle(intent)); });
  const bool reread_blocked = snapshots.WaitForBlockedReread(std::chrono::seconds(2));
  EXPECT_TRUE(reread_blocked);
  if (reread_blocked) {
    EXPECT_FALSE(reservations.snapshot().empty());
  }
  coordinator.CloseAdmission();
  snapshots.ReleaseReread();
  submitter.join();

  ASSERT_TRUE(submitted.has_value());
  ASSERT_FALSE(submitted->ok());
  EXPECT_EQ(submitted->error().code, "component_lifecycle_admission_closed");
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_EQ(port->dispatch_count(), 0U);
  const auto inspected = journal.InspectLifecycleRequest(coordinator.request_owner_epoch(), intent);
  ASSERT_TRUE(inspected.ok()) << inspected.error().code << ": " << inspected.error().message;
  ASSERT_TRUE(inspected.value());
  EXPECT_FALSE(inspected.value()->acceptance);
  ASSERT_TRUE(inspected.value()->rejection);
  EXPECT_EQ(inspected.value()->rejection->details.at("effect"), "not_started");
}

TEST(ComponentLifecycleCoordinator, DrainIncludesDirectResolverBeforeAnyRuntimeExists) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  BlockingRereadSnapshotSource snapshots(Snapshot());
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto intent = PrepareIntent(coordinator.request_owner_epoch());
  auto submitted = std::async(std::launch::async, [&] { return coordinator.SubmitLifecycle(intent); });
  const bool blocked = snapshots.WaitForBlockedReread(std::chrono::seconds(2));
  EXPECT_TRUE(blocked);
  if (blocked) {
    const auto durable = journal.ReadLifecycleRequestResult(intent);
    EXPECT_TRUE(durable.ok());
    if (durable.ok()) {
      EXPECT_FALSE(durable.value());
    }
    EXPECT_FALSE(reservations.snapshot().empty());
    EXPECT_EQ(port->dispatch_count(), 0U);
    EXPECT_FALSE(coordinator.ReadLifecycleOwnerSettlement().ok());
    EXPECT_FALSE(coordinator.DrainFor(std::chrono::nanoseconds::zero()));
  }
  coordinator.CloseAdmission();
  snapshots.ReleaseReread();
  const auto refused = submitted.get();
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_lifecycle_admission_closed");
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_EQ(clock.now(), 100);
}

TEST(ComponentLifecycleCoordinator, DrainReturnsOnlyAfterLateOwnerReconciliationFaultIsVisible) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("original.seed"), 1, 8U);
  ASSERT_TRUE(begun.ok());
  const auto owner = begun.value().request_owner_epoch;
  const auto intent = PrepareIntent(owner);
  const auto prepared = PrepareComponentLifecycleAdmission(
      intent, RecoveryAdmissionCandidate("lifecycle.1", Snapshot(), {PrepareCatalog()}));
  ASSERT_TRUE(prepared.ok());
  const auto claim = SeedLifecycleClaim(journal, owner, intent, prepared.value().acceptance);
  ASSERT_TRUE(claim.ok());
  ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, intent, prepared.value().acceptance,
                                            prepared.value().initial_status).ok());
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  BlockingOwnerReconciliationPort kernel;
  kernel.RetainClaim(claim.value());
  FakeClock clock;
  clock.set(500);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const bool blocked = kernel.WaitForBlockedProbe(std::chrono::seconds(2));
  EXPECT_TRUE(blocked);
  std::promise<void> drain_entered;
  auto drained = std::async(std::launch::async, [&] {
    drain_entered.set_value();
    return coordinator.DrainFor(std::chrono::seconds(2));
  });
  drain_entered.get_future().wait();
  if (blocked) {
    // This probe is reached by Complete only after reservation release and
    // active-row removal. The genuine worker still owns the final fault path.
    EXPECT_TRUE(reservations.snapshot().empty());
    const auto terminal = journal.LoadRetainedLifecycleStatus(
        prepared.value().acceptance.lifecycle_transaction_id);
    EXPECT_TRUE(terminal.ok());
    if (terminal.ok() && terminal.value()) {
      EXPECT_EQ(terminal.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
    } else {
      ADD_FAILURE() << "recovered terminal was not retained before final reconciliation";
    }
    EXPECT_FALSE(coordinator.ReadLifecycleOwnerSettlement().ok());
    EXPECT_EQ(drained.wait_for(std::chrono::nanoseconds::zero()), std::future_status::timeout);
  }
  kernel.ReleaseProbe();
  EXPECT_TRUE(drained.get());
  EXPECT_TRUE(coordinator.admission_fault_closed());
  EXPECT_FALSE(coordinator.accepting_new_requests());
  EXPECT_EQ(port->observation_count(), 0U);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_EQ(clock.now(), 500);
}

TEST(ComponentLifecycleCoordinator, DrainDoesNotCancelAnActivePollOrAdvanceTheOwnerClock) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  WaitableManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted = coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok());
  ASSERT_TRUE(accepted.value().acceptance);
  ASSERT_TRUE(scheduler.WaitForScheduled(2U, std::chrono::seconds(2)));
  EXPECT_EQ(scheduler.pending_count(SchedulerPriority::Lifecycle), 2U);
  EXPECT_FALSE(coordinator.DrainFor(std::chrono::nanoseconds::zero()));
  EXPECT_EQ(scheduler.cancel_count(), 0U);
  EXPECT_EQ(scheduler.pending_count(SchedulerPriority::Lifecycle), 2U);
  EXPECT_FALSE(reservations.snapshot().empty());
  const auto pending = coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(pending.ok());
  ASSERT_TRUE(pending.value());
  EXPECT_FALSE(pending.value()->terminal_result);
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_EQ(clock.now(), 100);
}

TEST(ComponentLifecycleCoordinator, ConcurrentDuplicateJoinsBeforeAnotherPreflight) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  BlockingRereadSnapshotSource snapshots(Snapshot());
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto intent = PrepareIntent(coordinator.request_owner_epoch());
  auto first = std::async(std::launch::async, [&] { return coordinator.SubmitLifecycle(intent); });
  const bool crossing = snapshots.WaitForBlockedReread(std::chrono::seconds(2));
  EXPECT_TRUE(crossing);
  std::promise<void> duplicate_started;
  auto duplicate = std::async(std::launch::async, [&] {
    duplicate_started.set_value();
    return coordinator.SubmitLifecycle(intent);
  });
  // Preflight and its outcome now share one gate with checkpointing.
  duplicate_started.get_future().wait();
  EXPECT_EQ(duplicate.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
  snapshots.ReleaseReread();
  const auto accepted = first.get();
  const auto joined = duplicate.get();
  ASSERT_TRUE(accepted.ok());
  ASSERT_TRUE(joined.ok()) << joined.error().message;
  ASSERT_TRUE(accepted.value().acceptance);
  ASSERT_TRUE(joined.value().acceptance);
  EXPECT_EQ(accepted.value().acceptance->lifecycle_transaction_id,
            joined.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  EXPECT_TRUE(coordinator.accepting_new_requests());
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_EQ(port->dispatch_count(), 1U);
}

// V2-4: a failed action fails the transaction with the adapter's own typed
// error, unchanged; CCLC observes once before and once after (L3).
TEST(ComponentLifecycleCoordinator, FailedActionCauseIsTheTerminalErrorAndSurvivesDurableStatus) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  const ErrorDTO refusal{
      "execution_rejected", "the controller manager refused the controller switch",
      {{"reason", "ros2_control_activation_refused"},
       {"controller_response_message", "controller requires its hardware interface"},
       {"controller_name", "gripper_controller"}, {"switch_action", "activate"},
       {"controller_service", "/cell/controller_manager/switch_controller"}}};
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared},
      DispatchMode::kKnownFailure, 1U, std::vector<LifecycleFactDTO>{}, std::function<void()>{},
      refusal);
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value());
  const auto& final = *status.value();
  EXPECT_EQ(final.phase, ComponentLifecycleTransactionPhaseDTO::kFailed);
  EXPECT_EQ(final.terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  EXPECT_FALSE(final.reconciliation_required);
  EXPECT_EQ(final.error, refusal);
  ASSERT_EQ(final.domains.size(), 1U);
  const auto& domain = final.domains.front();
  EXPECT_EQ(domain.action_state, ComponentLifecycleDomainActionStateDTO::kFailed);
  EXPECT_EQ(domain.error, refusal);
  ASSERT_EQ(domain.primitive_attempts.size(), 1U);
  const auto& attempt = domain.primitive_attempts.front();
  EXPECT_EQ(attempt.lifecycle_primitive_id,
            LifecycleTransitionActionId(ComponentLifecycleIntentKindDTO::kPrepare));
  EXPECT_EQ(attempt.outcome, LifecycleTransitionOutcomeDTO::kFailed);
  EXPECT_EQ(attempt.error, refusal);
  ASSERT_EQ(attempt.substeps.size(), 1U);
  EXPECT_EQ(attempt.substeps.front().error, refusal);
  // One observation before dispatch and one fresh observation after it.
  EXPECT_EQ(port->observation_count(), 2U);
  ASSERT_TRUE(domain.observed_before);
  EXPECT_EQ(domain.observed_before->observation_generation, 1U);
  ASSERT_TRUE(domain.observed_after);
  EXPECT_EQ(domain.observed_after->observation_generation, 2U);
  EXPECT_EQ(domain.observed_after->observed_state, ComponentLifecycleStateDTO::kDeclared);
  const auto retained = journal.LoadRetainedLifecycleStatus(final.acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(retained.ok());
  ASSERT_TRUE(retained.value());
  EXPECT_EQ(*retained.value(), final);
  EXPECT_EQ(detail::DecodeComponentLifecycleStatus(detail::EncodeComponentLifecycleStatus(final)),
            final);
  EXPECT_TRUE(final.locally_valid());
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

// An adapter result for another domain is not this action's result: failed
// with the typed mismatch reason, never redispatched.
TEST(ComponentLifecycleCoordinator, MismatchedAdapterResultFailsWithItsTypedReason) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  const ErrorDTO foreign{"execution_rejected", "foreign controller refused",
                         {{"reason", "foreign_refusal"}}};
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared},
      DispatchMode::kForeignDomainFailure, 1U, std::vector<LifecycleFactDTO>{},
      std::function<void()>{}, foreign);
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  ASSERT_TRUE(status.value()->error);
  EXPECT_EQ(status.value()->error->code, "component_lifecycle_adapter_result_invalid");
  ASSERT_EQ(status.value()->domains.front().primitive_attempts.size(), 1U);
  const auto& result = status.value()->domains.front().primitive_attempts.front();
  EXPECT_EQ(result.outcome, LifecycleTransitionOutcomeDTO::kFailed);
  EXPECT_EQ(result.control_domain_id, status.value()->domains.front().control_domain_id);
  EXPECT_EQ(port->dispatch_count(), 1U);
}

// V2-4 + VB2-O1: a failed action is failed even when the fresh observation
// after it shows the requested state; nothing is reconciled or redispatched,
// and that resting observation is the recorded current status.
TEST(ComponentLifecycleCoordinator, FailedDispatchSettlesCurrentStatusWithoutReconciliation) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal,
      std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                              ComponentLifecycleStateDTO::kPrepared},
      DispatchMode::kResponseLost);
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted =
      coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kFailed);
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  ASSERT_TRUE(status.value()->error);
  EXPECT_EQ(status.value()->error->code, "response_lost");
  EXPECT_FALSE(status.value()->error->details.contains("component_state"));
  EXPECT_EQ(port->dispatch_count(), 1U);
  const auto& domain = status.value()->domains.front();
  ASSERT_TRUE(domain.observed_after);
  EXPECT_EQ(domain.observed_after->observed_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(domain.current_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(status.value()->current_state, ComponentLifecycleStateDTO::kPrepared);
  const auto fact = journal.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.1"));
  ASSERT_TRUE(fact.ok()) << fact.error().message;
  EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kPrepared);
}

TEST(ComponentLifecycleCoordinator,
     OrdinaryDisableUsesTheIdleClaimAndNeverInvokesKernelStop) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots(OrdinaryDisableSnapshots());
  FakeCatalogSource catalogs(DisableCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kActive,
                                                       ComponentLifecycleStateDTO::kDisabled});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted =
      coordinator.SubmitLifecycle(DisableIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().code << ": " << accepted.error().message;
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));

  ASSERT_TRUE(accepted.value().acceptance.has_value());
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kSucceeded)
      << (status.value()->error.has_value()
              ? status.value()->error->code + ": " + status.value()->error->message
              : "no terminal error recorded");
  EXPECT_EQ(status.value()->current_state, ComponentLifecycleStateDTO::kDisabled);
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_EQ(port->last_dispatched_primitive_id(),
            LifecycleTransitionActionId(ComponentLifecycleIntentKindDTO::kDisable));
  EXPECT_TRUE(port->saw_prepared_status());
  EXPECT_TRUE(reservations.snapshot().empty());
}

TEST(ComponentLifecycleCoordinator,
     ClaimedSnapshotMismatchNeverDispatchesAndReleasesTheIdleClaim) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  const auto before = Snapshot("snapshot.before.1", ComponentLifecycleStateDTO::kActive,
                               ComponentLifecycleGeneration(3U));
  auto under_fence =
      Snapshot("snapshot.under-fence.changed.1", ComponentLifecycleStateDTO::kDisabling,
               ComponentLifecycleGeneration(4U));
  under_fence.domains.front().controller().active_control_configuration =
      CurrentActiveConfiguration();
  ASSERT_TRUE(under_fence.locally_valid());
  FakeSnapshotSource snapshots({before, before, under_fence, under_fence});
  FakeCatalogSource catalogs(DisableCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kActive});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  auto intent = DisableIntent(coordinator.request_owner_epoch());
  intent.request_context.request_id = RequestId("disable-under-fence-config-mismatch.1");
  const auto accepted = coordinator.SubmitLifecycle(intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().code << ": " << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance.has_value());
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));

  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kFailed);
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  ASSERT_TRUE(status.value()->error.has_value());
  EXPECT_EQ(status.value()->error->code, "component_lifecycle_snapshot_changed_under_claim");
  EXPECT_EQ(port->observation_count(), 0U);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

TEST(ComponentLifecycleCoordinator, StaleDisableRejectsBeforeFenceOrVendorMutation) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots(ResolvedDisableSnapshots());
  FakeCatalogSource catalogs(DisableCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kActive});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  auto intent = DisableIntent(coordinator.request_owner_epoch());
  intent.request_context.request_id = RequestId("disable-stale.1");
  intent.expected_component_lifecycle_generation = ComponentLifecycleGeneration(2U);
  const auto rejected = coordinator.SubmitLifecycle(intent);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, "component_lifecycle_current_snapshot_mismatch");
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

TEST(ComponentLifecycleCoordinator, MissingDisableCatalogRejectsBeforeFence) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots(ResolvedDisableSnapshots());
  FakeCatalogSource catalogs(DisableCatalog(), CatalogAvailability::kUnavailable);
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kActive});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  auto intent = DisableIntent(coordinator.request_owner_epoch());
  intent.request_context.request_id = RequestId("disable-catalog-unavailable.1");
  const auto rejected = coordinator.SubmitLifecycle(intent);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, "lifecycle_catalog_unavailable");
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
}
TEST(ComponentLifecycleCoordinator,
     ShutdownWaitsForAStuckLifecycleSchedulerLaneInsteadOfTerminating) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  WaitableManualScheduler scheduler(true);
  auto coordinator = std::make_unique<ComponentLifecycleCoordinator>(
      Config(), journal, reservations, snapshots, catalogs, resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator->Start().ok());
  const auto accepted =
      coordinator->SubmitLifecycle(PrepareIntent(coordinator->request_owner_epoch()));
  ASSERT_TRUE(accepted.ok());
  ASSERT_TRUE(scheduler.WaitForScheduled(2U, std::chrono::seconds(2)));

  testing::internal::CaptureStderr();
  coordinator.reset();
  const auto output = testing::internal::GetCapturedStderr();

  EXPECT_EQ(scheduler.drain_count(), 2U);
  EXPECT_NE(output.find("manyforge_fault code=drain_stuck "
                        "drain=lifecycle_coordinator.scheduler_lifecycle_lane bound_ms=5000 "
                        "state=waiting_for_operator"),
            std::string::npos)
      << output;
  EXPECT_NE(output.find("drain=lifecycle_coordinator.scheduler_lifecycle_lane state=drained_late"),
            std::string::npos)
      << output;
}

TEST(ComponentLifecycleCoordinator,
     RecoveryWithoutPreparedEffectFailsAndReopensAdmission) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("cclc-owner.1"), 1, 8U);
  ASSERT_TRUE(begun.ok());
  const auto first_owner = begun.value().request_owner_epoch;
  const auto intent = PrepareIntent(first_owner);
  const auto prepared = PrepareComponentLifecycleAdmission(
      intent, RecoveryAdmissionCandidate("lifecycle.1", Snapshot(), {PrepareCatalog()}));
  ASSERT_TRUE(prepared.ok()) << prepared.error().code << ": " << prepared.error().message;
  const auto seeded_claim =
      SeedLifecycleClaim(journal, first_owner, intent, prepared.value().acceptance);
  ASSERT_TRUE(seeded_claim.ok());
  ASSERT_TRUE(journal
                  .AcceptOrReplayLifecycle(first_owner, intent, prepared.value().acceptance,
                                           prepared.value().initial_status)
                  .ok());

  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  kernel.RetainClaim(seeded_claim.value());
  FakeClock clock;
  clock.set(500);
  ManualScheduler scheduler;
  auto config = Config();
  config.request_owner_epoch = RequestOwnerEpoch("cclc-owner.2");
  ComponentLifecycleCoordinator coordinator(config, journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  EXPECT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(prepared.value().acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->status_owner_epoch, coordinator.request_owner_epoch());
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kFailed);
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  ASSERT_TRUE(status.value()->error.has_value());
  EXPECT_EQ(status.value()->error->code, "component_lifecycle_owner_replaced");
  EXPECT_EQ(port->observation_count(), 0U);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_TRUE(coordinator.accepting_new_requests());
}

TEST(ComponentLifecycleCoordinator, SelectedOwnerReplacementKeepsClaimAndNeverDispatches) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("cclc-owner.1"), 1, 8U);
  ASSERT_TRUE(begun.ok());
  const auto first_owner = begun.value().request_owner_epoch;
  auto intent = PrepareIntent(first_owner);
  intent.intent = ComponentLifecycleIntentKindDTO::kEnable;
  const auto catalog = EnableCatalog();
  const auto snapshot = Snapshot("snapshot.selected", ComponentLifecycleStateDTO::kPrepared,
                                 ComponentLifecycleGeneration(3U), catalog);
  const auto prepared = PrepareComponentLifecycleAdmission(
      intent, RecoveryAdmissionCandidate("lifecycle.1", snapshot, {catalog}));
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  auto status = prepared.value().initial_status;
  status.acceptance.lifecycle_transaction_id = LifecycleTransactionId("selected-lifecycle.1");
  const auto configuration = CurrentActiveConfiguration();
  status.domains.front().current_scope->controller().active_control_configuration = configuration;
  const ExpectedActiveControlConfigurationDTO expected{
      ExpectedActiveControlConfigurationPresenceDTO::kPresent,
      configuration.active_control_configuration_id,
      configuration.active_control_configuration_generation};
  status.accepted_intent.expected_control_domains.front()
      .controller()
      .expected_active_control_configuration = expected;
  status.domains.front()
      .expected_domain->controller()
      .expected_active_control_configuration = expected;
  const ContentDigest digest(Sha('a'));
  const CellAdmissionClaimIntentDTO claim_intent{
      CellAdmissionClaimKindDTO::kInterventionOrRecovery,
      CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
      first_owner,
      status.acceptance.lifecycle_transaction_id.value(),
      status.acceptance.request_id,
      digest,
      status.accepted_intent.expected_admission_generation,
      {{ResourceId("resource.1")},
       {{configuration.control_domain_id, configuration.active_control_configuration_id,
         configuration.active_control_configuration_generation}}},
      std::nullopt,
      CellAdmissionInterventionEvidenceDTO{
          CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, first_owner,
          status.acceptance.lifecycle_transaction_id.value(), digest, std::nullopt},
      std::nullopt};
  const CellAdmissionClaimDTO claim{claim_intent, CellAdmissionClaimGeneration(15U),
                                    AdmissionGeneration(1U), 80};
  status.selected_bring_up = SelectedComponentBringUpEvidenceDTO{
      digest,
      digest,
      "incident.selected",
      StopId("stop.selected"),
      StopGeneration(7U),
      {RetainedBundleId("candidate.bundle"),
       GraphRevision("candidate.graph"),
       {SceneId(digest.value()), 1U, digest}},
      digest,
      1024U,
      {ExecutionBindingEpoch(12U), {{ControlDomainId("domain.1"), 30U}}},
      {ExecutionBindingEpoch(13U), {{ControlDomainId("domain.1"), 31U}}},
      claim,
      {ConfigurationFenceId("fence.selected"),
       ConfigurationFenceGeneration(22U),
       {{ControllableComponentId("component.1")},
        {{ControlDomainId("domain.1"), ControlDomainRevision("domain-revision.1"),
          ControlDomainRevision("domain-revision.1")}},
        {{ControlBindingId("binding.1"), ControlBindingRevision("binding-revision.1"),
          ControlBindingRevision("binding-revision.1")}},
        {ResourceId("resource.1")}},
       90,
       RequestReplayStatusDTO::kAcceptedNew},
      ContentDigest("sha256:" + std::string(64U, 'c'))};
  ASSERT_TRUE(status.locally_valid());
  const auto& evidence = *status.selected_bring_up;
  ComponentLifecycleIncidentDTO incident{evidence.incident_id,
                                         status.accepted_intent.controllable_component_id,
                                         LifecycleBlockingCauseClassDTO::kProviderLost,
                                         {},
                                         20,
                                         "controller exited",
                                         std::nullopt,
                                         {},
                                         ComponentIncidentContainmentStateDTO::kPending,
                                         std::nullopt,
                                         std::nullopt,
                                         false,
                                         std::nullopt,
                                         std::nullopt,
                                         std::nullopt};
  const auto policy = LifecycleIncidentRecoveryPolicyFor(DefaultLifecycleIncidentRecoveryPolicy(),
                                                         incident.cause_class);
  const auto original =
      ComponentIncidentEventPayload(incident, policy, kComponentIncidentEventKind);
  ASSERT_TRUE(journal.OpenComponentIncident({incident, original}, 0U).ok());
  const auto contained = ComponentIncidentRecordWithContainment(
      original, ComponentIncidentContainmentStateDTO::kRequested,
      ComponentIncidentContainmentReceiptDTO{
          evidence.containment_stop_id, evidence.containment_stop_generation, {}, std::nullopt},
      std::nullopt);
  ASSERT_TRUE(
      journal
          .RecordComponentIncidentContainment(
              incident.incident_id, ComponentIncidentContainmentStateDTO::kRequested, contained, 30)
          .value());
  ASSERT_TRUE(
      journal
          .RecordSelectedCellClaimAttempt(first_owner, claim.intent,
                                          status.selected_bring_up->original_request_payload_digest)
          .ok());
  ASSERT_TRUE(journal.RecordCellClaimAcquired(first_owner, claim).ok());
  ASSERT_TRUE(journal
                  .AcceptOrReplaySelectedComponentBringUp(
                      first_owner, status.accepted_intent, status.acceptance, status,
                      status.selected_bring_up->original_request_payload_digest)
                  .ok());

  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({snapshot});
  FakeCatalogSource catalogs(catalog);
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  kernel.RetainClaim(claim);
  FakeClock clock;
  clock.set(500);
  ManualScheduler scheduler;
  auto config = Config();
  config.request_owner_epoch = RequestOwnerEpoch("cclc-owner.2");
  ComponentLifecycleCoordinator coordinator(config, journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  const auto started = coordinator.Start();
  ASSERT_TRUE(started.ok()) << started.error().message;
  EXPECT_FALSE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto read = coordinator.LoadLifecycleStatus(status.acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(read.ok());
  ASSERT_TRUE(read.value());
  EXPECT_EQ(read.value()->phase, ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown);
  EXPECT_TRUE(read.value()->reconciliation_required);
  EXPECT_EQ(read.value()->selected_bring_up, status.selected_bring_up);
  EXPECT_EQ(read.value()->remaining_duration_ns, 0);
  EXPECT_FALSE(read.value()->terminal_result);
  EXPECT_EQ(read.value()->status_owner_epoch, coordinator.request_owner_epoch());
  EXPECT_EQ(port->observation_count(), 0U);
  EXPECT_EQ(port->dispatch_count(), 0U);
  const auto retained = kernel.GetCellAdmissionRecoveryReceipt({claim.intent, claim, std::nullopt});
  ASSERT_TRUE(retained.ok());
  EXPECT_EQ(retained.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
  EXPECT_FALSE(coordinator.accepting_new_requests());
  EXPECT_FALSE(reservations.snapshot().empty());
  const auto fact =
      journal.LoadCurrentComponentLifecycleFact(status.accepted_intent.controllable_component_id);
  ASSERT_TRUE(fact.ok());
  EXPECT_EQ(fact.value().durable_generation, status.acceptance.component_lifecycle_generation);
  EXPECT_EQ(fact.value().current_state, status.current_state);
  EXPECT_EQ(journal.LoadComponentsWithUnsettledLifecycle().value(),
            std::vector<ControllableComponentId>{status.accepted_intent.controllable_component_id});
  EXPECT_TRUE(
      journal.LoadOpenComponentIncident(status.accepted_intent.controllable_component_id).value());
}

// V2-4 owner recovery: a prepared, never completed action is completed as
// failed, nothing is re-sent, and one fresh observation settles the state
// (VB2-O1). No reconciliation wait and no admission gate.
TEST(ComponentLifecycleCoordinator,
     RecoveryCompletesAPreparedAttemptAsFailedAndSettlesFreshStatus) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("cclc-owner.1"), 1, 8U);
  ASSERT_TRUE(begun.ok());
  const auto first_owner = begun.value().request_owner_epoch;
  const auto intent = PrepareIntent(first_owner);
  const auto prepared = PrepareComponentLifecycleAdmission(
      intent, RecoveryAdmissionCandidate("lifecycle.1", Snapshot(), {PrepareCatalog()}));
  ASSERT_TRUE(prepared.ok()) << prepared.error().code << ": " << prepared.error().message;
  const auto seeded_claim =
      SeedLifecycleClaim(journal, first_owner, intent, prepared.value().acceptance);
  ASSERT_TRUE(seeded_claim.ok());
  ASSERT_TRUE(journal
                  .AcceptOrReplayLifecycle(first_owner, intent, prepared.value().acceptance,
                                           prepared.value().initial_status)
                  .ok());
  const auto primitive = RecoveryPrimitiveIntent(prepared.value().initial_status);
  ASSERT_TRUE(primitive.locally_valid());
  auto prepared_status = prepared.value().initial_status;
  ++prepared_status.status_sequence;
  prepared_status.phase = ComponentLifecycleTransactionPhaseDTO::kExecuting;
  prepared_status.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kExecuting;
  prepared_status.latest_updated_at_ns += 10;
  prepared_status.remaining_duration_ns -= 10;
  ASSERT_TRUE(prepared_status.locally_valid());
  ASSERT_TRUE(
      journal
          .RecordEffectPrepared(first_owner, PreparedPrimitiveAttempt(primitive), prepared_status)
          .ok());

  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared},
      DispatchMode::kKnownSuccess, 2U);
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  kernel.RetainClaim(seeded_claim.value());
  FakeClock clock;
  clock.set(500);
  ManualScheduler scheduler;
  auto config = Config();
  config.request_owner_epoch = RequestOwnerEpoch("cclc-owner.2");
  ComponentLifecycleCoordinator coordinator(config, journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  EXPECT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(prepared.value().acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->status_owner_epoch, coordinator.request_owner_epoch());
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kFailed);
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  ASSERT_TRUE(status.value()->error);
  EXPECT_EQ(status.value()->error->code, "component_lifecycle_owner_replaced");
  EXPECT_EQ(status.value()->error->details.at("reason"), "owner_replaced_after_effect_prepare");
  EXPECT_EQ(catalogs.load_count(), 0U);
  EXPECT_EQ(resolver.resolve_count(), 1U);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_EQ(port->observation_count(), 1U);
  const auto& domain = status.value()->domains.front();
  ASSERT_EQ(domain.primitive_attempts.size(), 1U);
  const auto& completed = domain.primitive_attempts.front();
  EXPECT_EQ(completed.lifecycle_primitive_attempt_id, primitive.lifecycle_primitive_attempt_id);
  EXPECT_EQ(completed.outcome, LifecycleTransitionOutcomeDTO::kFailed);
  EXPECT_TRUE(completed.substeps.empty());
  ASSERT_TRUE(completed.error.has_value());
  EXPECT_EQ(completed.error->code, "component_lifecycle_owner_replaced");
  EXPECT_EQ(completed.error->details.at("reason"), "owner_replaced_after_effect_prepare");
  // The fresh resting observation is the recorded state.
  ASSERT_TRUE(domain.observed_after);
  EXPECT_EQ(domain.observed_after->observed_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(domain.current_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(status.value()->current_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_FALSE(status.value()->error->details.contains("component_state"));
  const auto fact =
      journal.LoadCurrentComponentLifecycleFact(intent.controllable_component_id);
  ASSERT_TRUE(fact.ok()) << fact.error().message;
  EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_TRUE(coordinator.accepting_new_requests());
}

TEST(ComponentLifecycleCoordinator, RecoveryRetainsTheActualFailedPrimitiveCauseWithoutRedispatch) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("cclc-owner.1"), 1, 8U);
  ASSERT_TRUE(begun.ok());
  const auto owner = begun.value().request_owner_epoch;
  const auto seed = SeedPreparedBrandRecovery(journal, owner, PrepareCatalog(), "lifecycle.1");
  const auto& intent = seed.primitive;
  const ErrorDTO refusal{"execution_rejected", "the controller manager refused the controller switch",
                         {{"reason", "ros2_control_activation_refused"},
                          {"controller_response_message", "controller unavailable"},
                          {"controller_name", "gripper_controller"}, {"switch_action", "activate"}}};
  const BrandLifecyclePrimitiveResultDTO failed{
      intent.lifecycle_transaction_id, intent.lifecycle_transaction_generation,
      intent.controllable_component_id, intent.control_domain_id, intent.control_domain_revision,
      intent.provider_instance_id, intent.provider_manifest_id, intent.provider_manifest_revision,
      intent.provider_runtime_unit_id, intent.provider_runtime_epoch, intent.lifecycle_primitive_id,
      intent.lifecycle_primitive_attempt_id, intent.expected_observation_generation,
      LifecycleTransitionOutcomeDTO::kFailed, refusal};
  ASSERT_TRUE(failed.matches(intent));
  const auto prepared = journal.LoadRetainedLifecycleStatus(seed.acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(prepared.ok());
  ASSERT_TRUE(prepared.value());
  auto completed = *prepared.value();
  ++completed.status_sequence;
  completed.phase = ComponentLifecycleTransactionPhaseDTO::kVerifying;
  completed.error = refusal;
  auto& domain = completed.domains.front();
  domain.primitive_attempts.push_back(failed);
  domain.action_state = ComponentLifecycleDomainActionStateDTO::kFailed;
  domain.error = refusal;
  ASSERT_TRUE(completed.locally_valid());
  auto attempt = PreparedPrimitiveAttempt(intent);
  attempt.state = CCLCEffectAttemptStateDTO::kCompleted;
  attempt.lifecycle_primitive_result = failed;
  ASSERT_TRUE(journal.RecordEffectCompleted(owner, attempt, completed).ok());

  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared},
      DispatchMode::kKnownSuccess, 2U);
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  kernel.RetainClaim(seed.claim);
  FakeClock clock;
  clock.set(500);
  ManualScheduler scheduler;
  auto config = Config();
  config.request_owner_epoch = RequestOwnerEpoch("cclc-owner.2");
  ComponentLifecycleCoordinator coordinator(config, journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status = coordinator.LoadLifecycleStatus(seed.acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value());
  // The interrupted transaction fails with the adapter's actual cause,
  // unchanged, in the terminal, the domain and the completed attempt.
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  EXPECT_EQ(status.value()->error, refusal);
  EXPECT_EQ(status.value()->domains.front().error, refusal);
  EXPECT_EQ(status.value()->domains.front().primitive_attempts,
            std::vector<BrandLifecyclePrimitiveResultDTO>{failed});
  EXPECT_EQ(status.value()->domains.front().current_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_EQ(port->dispatch_count(), 0U);
  const auto retained = journal.LoadRetainedLifecycleStatus(seed.acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(retained.ok());
  EXPECT_EQ(retained.value(), status.value());
}

struct RecoveredBrandSettlementCase final {
  std::string_view name;
  ComponentLifecycleStateDTO observed_state;
  ComponentLifecycleStateDTO expected_domain_state;
  ComponentLifecycleStateDTO expected_component_state;
  bool reconstructed;
};

class RecoveredBrandSettlementTest
    : public ::testing::TestWithParam<RecoveredBrandSettlementCase> {};

// VB2-O1 after owner recovery: whatever the fresh observation shows, the
// interrupted transition is failed; a resting observation is recorded as is
// (all-Active as Active: the completed attempt failed, VB2-core rev 2 / Q2),
// anything else as Declared with the warning.
TEST_P(RecoveredBrandSettlementTest, SettlesFromFreshObservationWithoutRedispatch) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("cclc-owner.1"), 1, 8U);
  ASSERT_TRUE(begun.ok());
  const auto first_owner = begun.value().request_owner_epoch;
  const auto seed = SeedPreparedBrandRecovery(journal, first_owner, PrepareCatalog(), "lifecycle.1");

  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{GetParam().observed_state},
      DispatchMode::kKnownSuccess, 2U);
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  kernel.RetainClaim(seed.claim);
  FakeClock clock;
  clock.set(500);
  ManualScheduler scheduler;
  auto config = Config();
  config.request_owner_epoch = RequestOwnerEpoch("cclc-owner.2");
  ComponentLifecycleCoordinator coordinator(config, journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status = coordinator.LoadLifecycleStatus(seed.acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kFailed);
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  EXPECT_EQ(status.value()->domains.front().current_state, GetParam().expected_domain_state);
  EXPECT_EQ(status.value()->current_state, GetParam().expected_component_state);
  ASSERT_TRUE(status.value()->error);
  EXPECT_EQ(status.value()->error->details.at("reason"), "owner_replaced_after_effect_prepare");
  EXPECT_EQ(status.value()->error->details.contains("component_state"), GetParam().reconstructed);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_EQ(port->observation_count(), 1U);
  ASSERT_EQ(status.value()->domains.front().primitive_attempts.size(), 1U);
  EXPECT_EQ(status.value()->domains.front().primitive_attempts.front().outcome,
            LifecycleTransitionOutcomeDTO::kFailed);
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_TRUE(coordinator.accepting_new_requests());
}

INSTANTIATE_TEST_SUITE_P(
    FreshObservations, RecoveredBrandSettlementTest,
    ::testing::Values(
        RecoveredBrandSettlementCase{"same-state", ComponentLifecycleStateDTO::kDeclared,
                                     ComponentLifecycleStateDTO::kDeclared,
                                     ComponentLifecycleStateDTO::kDeclared, false},
        RecoveredBrandSettlementCase{"changed-state", ComponentLifecycleStateDTO::kActive,
                                     ComponentLifecycleStateDTO::kActive,
                                     ComponentLifecycleStateDTO::kActive, false},
        RecoveredBrandSettlementCase{"not-resting", ComponentLifecycleStateDTO::kFaulted,
                                     ComponentLifecycleStateDTO::kDeclared,
                                     ComponentLifecycleStateDTO::kDeclared, true}),
    [](const ::testing::TestParamInfo<RecoveredBrandSettlementCase>& info) {
      std::string result(info.param.name);
      std::replace(result.begin(), result.end(), '-', '_');
      return result;
    });

// V2-4: a fact-bearing plan no longer waits for conclusive evidence after
// owner replacement; the interrupted action fails and admission reopens.
TEST(ComponentLifecycleCoordinator, RecoveryOfAFactBearingPlanFailsWithoutWaitingForEvidence) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("cclc-owner.1"), 1, 8U);
  ASSERT_TRUE(begun.ok());
  const auto first_owner = begun.value().request_owner_epoch;
  const auto catalog = FactBearingPrepareCatalog();
  const auto seed = SeedPreparedBrandRecovery(journal, first_owner, catalog, "lifecycle.1");

  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(catalog);
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared},
      DispatchMode::kKnownSuccess, 2U,
      std::vector<LifecycleFactDTO>{{LifecycleFactId("mode"), TokenFactValue("manual")}});
  port->set_catalog(catalog);
  FakeResolver resolver(port, catalog);
  FakeCellAdmissionPort kernel;
  kernel.RetainClaim(seed.claim);
  FakeClock clock;
  clock.set(500);
  ManualScheduler scheduler;
  auto config = Config();
  config.request_owner_epoch = RequestOwnerEpoch("cclc-owner.2");
  ComponentLifecycleCoordinator coordinator(config, journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status = coordinator.LoadLifecycleStatus(seed.acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kFailed);
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  EXPECT_FALSE(status.value()->reconciliation_required);
  EXPECT_EQ(status.value()->current_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_EQ(port->observation_count(), 1U);
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_TRUE(coordinator.accepting_new_requests());
}

[[nodiscard]] NativeApplyPlanRequestDTO DeclaredNativeApplyPlanRequest() {
  return {{ApplyId("apply.ros"), ApplyGeneration(1U)},
          RetainedBundleId("bundle.1"),
          GraphRevision("graph.1"),
          {ControllableComponentId("component.1")},
          {},
          {},
          {},
          {},
          {},
          ComponentLifecycleStateDTO::kDeclared};
}

TEST(ComponentLifecycleCoordinator, ObservedControllerApplyReopensPrepareOnlyAfterDeclaredAdoption) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  // A ROS-style parent Apply records a plan that only declares its component.
  const auto plan = journal.PlanNativeApply(DeclaredNativeApplyPlanRequest());
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  ASSERT_EQ(plan.value().components.size(), 1U);
  ASSERT_EQ(plan.value().components.front().state, ComponentLifecycleStateDTO::kDeclared);
  const auto declared_generation = plan.value().components.front().generation;

  const auto catalog = PrepareCatalog();
  auto intent_at = [&](const RequestOwnerEpoch& owner, std::string request_id) {
    auto intent = PrepareIntent(owner);
    intent.request_context.request_id = RequestId(std::move(request_id));
    intent.expected_component_lifecycle_generation = declared_generation;
    return intent;
  };
  {
    CCLCDomainReservationTable reservations;
    FakeSnapshotSource snapshots({Snapshot("snapshot.declared", ComponentLifecycleStateDTO::kDeclared,
                                           declared_generation, catalog)});
    FakeCatalogSource catalogs(catalog);
    auto port = std::make_shared<FakeBrandPort>(
        journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                         ComponentLifecycleStateDTO::kPrepared});
    FakeResolver resolver(port, catalog);
    FakeCellAdmissionPort kernel;
    FakeClock clock;
    clock.set(100);
    ManualScheduler scheduler;
    ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                              resolver, kernel, clock, scheduler);
    ASSERT_TRUE(coordinator.Start().ok());
    // While the plan is unadopted, ordinary Prepare stays closed.
    const auto refused = coordinator.SubmitLifecycle(
        intent_at(coordinator.request_owner_epoch(), "request.before-adoption"));
    const auto code = !refused.ok() ? refused.error().code
                      : refused.value().rejection.has_value()
                          ? refused.value().rejection->code
                          : std::string{};
    EXPECT_EQ(code, "component_lifecycle_native_apply_unsettled");
    EXPECT_EQ(port->dispatch_count(), 0U);
  }

  // The host adopts exactly the Declared plan under its held Apply fence.
  const auto publication = [] {
    auto receipt = ActivePublication();
    receipt.request.transaction_ref =
        NamedApplyPublicationTransactionDTO{ApplyId("apply.ros"), ApplyGeneration(1U)};
    receipt.request.expected_previous_publication_generation =
        ActiveBundlePublicationGeneration(1U);
    return receipt;
  }();
  ASSERT_TRUE(publication.valid());
  const auto adopted =
      journal.AdoptNativeApplyFacts({publication, plan.value().components, {}});
  ASSERT_TRUE(adopted.ok()) << adopted.error().message;

  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot("snapshot.declared", ComponentLifecycleStateDTO::kDeclared,
                                         declared_generation, catalog)});
  FakeCatalogSource catalogs(catalog);
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port, catalog);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(200);
  ManualScheduler scheduler;
  auto config = Config();
  config.request_owner_epoch = RequestOwnerEpoch("cclc-owner.2");
  ComponentLifecycleCoordinator coordinator(config, journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted = coordinator.SubmitLifecycle(
      intent_at(coordinator.request_owner_epoch(), "request.after-adoption"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().code << ": " << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance.has_value())
      << (accepted.value().rejection ? accepted.value().rejection->code : std::string{});
  // The transition continues past the adopted generation; nothing is reused.
  EXPECT_GT(accepted.value().acceptance->component_lifecycle_generation.value(),
            declared_generation.value());
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  EXPECT_EQ(port->dispatch_count(), 1U);
  const auto fact = journal.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.1"));
  ASSERT_TRUE(fact.ok());
  EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kPrepared);
}

TEST(ComponentLifecycleCoordinator, OrdinaryChildDomainReservationsStillRejectOverlap) {
  CCLCDomainReservationTable reservations;
  const CCLCDomainReservationOwnerDTO first{ComponentLifecycleDomainReservationOwnerDTO{
      ControllableComponentId("ordinary.component.1"), LifecycleTransactionId("ordinary.1"),
      LifecycleTransactionGeneration(1U)}};
  const CCLCDomainReservationOwnerDTO second{ComponentLifecycleDomainReservationOwnerDTO{
      ControllableComponentId("ordinary.component.2"), LifecycleTransactionId("ordinary.2"),
      LifecycleTransactionGeneration(2U)}};
  const std::vector<ControlDomainId> shared{ControlDomainId("domain.1")};
  ASSERT_TRUE(reservations.reserve(first, shared).ok());
  const auto before = reservations.snapshot();
  const auto overlap = reservations.reserve(second, shared);
  ASSERT_FALSE(overlap.ok());
  EXPECT_EQ(overlap.error().code, "cclc_domain_busy");
  EXPECT_EQ(reservations.snapshot(), before);
  ASSERT_TRUE(reservations.release(first, shared).ok());
  ASSERT_TRUE(reservations.reserve(second, shared).ok());
  ASSERT_EQ(reservations.snapshot().size(), 1U);
  EXPECT_EQ(reservations.snapshot().front().owner, second);
}

// Exact publication metadata for an owner-local context handoff. This fixture
// tests CCLC lifetime/accounting, not physical host publication authority.
[[nodiscard]] ActiveBundlePublicationReceiptDTO NextProductPublication(
    const ActiveBundlePublicationReceiptDTO& previous) {
  auto next = previous;
  next.request.transaction_ref =
      NamedApplyPublicationTransactionDTO{ApplyId("product.apply.1"), ApplyGeneration(1U)};
  next.request.expected_previous_publication_generation = previous.publication_generation;
  next.publication_generation =
      ActiveBundlePublicationGeneration(previous.publication_generation.value() + 1U);
  next.durable_public_pointer_content_digest = ContentDigest("product.successor.pointer");
  return next;
}

[[nodiscard]] std::shared_ptr<const ComponentLifecycleProductContext> ProductContextFor(
    const ICCLCCurrentComponentSnapshotSource& snapshots, const FakeCatalogSource& catalogs,
    const FakeResolver& resolver, const ActiveBundlePublicationReceiptDTO& publication) {
  return std::make_shared<const ComponentLifecycleProductContext>(ComponentLifecycleProductContext{
      snapshots, catalogs, resolver, std::nullopt, nullptr,
      ComponentLifecycleProductIdentity{publication, {ControllableComponentId("component.1")}}});
}

Result<void> SeedCloseComponentFact(SqliteCCLCJournal& journal, ActiveBundlePublicationReceiptDTO& publication) {
  const auto plan = journal.PlanNativeApply(DeclaredNativeApplyPlanRequest());
  if (!plan.ok()) return Result<void>::Err(plan.error());
  publication.request.transaction_ref = NamedApplyPublicationTransactionDTO{ApplyId("apply.ros"), ApplyGeneration(1U)};
  publication.request.expected_previous_publication_generation = ActiveBundlePublicationGeneration(1U);
  return journal.AdoptNativeApplyFacts({publication, plan.value().components, {}});
}

class CloseLeaseAdmissionPort final : public FakeCellAdmissionPort {
 public:
  class Lease final : public ICellCloseAdmissionLease {
   public:
    explicit Lease(CloseLeaseAdmissionPort& owner) : owner_(owner) {}
    Result<void> ValidatePrerequisites() const override {
      ++owner_.checks;
      if (owner_.fail_at != 0 && owner_.checks == owner_.fail_at)
        return Result<void>::Err({"cell_close_stop_fault", "actual test guard changed", {{"reason", "active_stop_fault"}}});
      return Result<void>::Ok();
    }
    Result<void> PrepareCommandEligibilityClosure(
        const CCLCLifecycleTargetRetirementIntentDTO& intent) override {
      prepared_ = intent;
      return Result<void>::Ok();
    }
    bool CloseCommandEligibilityAfterRetirement(
        const CCLCLifecycleTargetRetirementIntentDTO& intent,
        const CCLCLifecycleTargetRetirementResultDTO& result) noexcept override {
      if (!prepared_ || *prepared_ != intent ||
          result.replay_status != RequestReplayStatusDTO::kAcceptedNew ||
          result.request_context != prepared_->request_context ||
          result.scope != prepared_->scope || result.retired_targets != prepared_->expected_targets)
        return false;
      ++owner_.closures;
      if (owner_.readiness) owner_.readiness->ApplyPreparedClose();
      return true;
    }
    bool CloseCommandEligibilityForUnknownRetirement(
        const CCLCLifecycleTargetRetirementIntentDTO& intent) noexcept override {
      if (!prepared_ || *prepared_ != intent) return false;
      ++owner_.unknown_closures;
      if (owner_.readiness) owner_.readiness->ApplyPreparedClose();
      return true;
    }
   private:
    CloseLeaseAdmissionPort& owner_;
    std::optional<CCLCLifecycleTargetRetirementIntentDTO> prepared_;
  };
  Result<std::unique_ptr<ICellCloseAdmissionLease>> AcquireCellCloseAdmissionLease() override {
    ++acquires;
    std::unique_ptr<ICellCloseAdmissionLease> lease = std::make_unique<Lease>(*this);
    return Result<std::unique_ptr<ICellCloseAdmissionLease>>::Ok(std::move(lease));
  }
  int checks{};
  int acquires{};
  int fail_at{};
  int closures{};
  int unknown_closures{};
  FakeReadinessPort* readiness{};
};

// Reproduce the exact final scoped-settlement lock ordering with a real
// exclusion and barriers. The test-only timed acquisition bounds a regressed
// blocking implementation; no production timeout or guard changes.
class ContendedCloseLeaseAdmissionPort final : public FakeCellAdmissionPort {
 public:
  class Lease final : public ICellCloseAdmissionLease {
   public:
    Lease(ContendedCloseLeaseAdmissionPort& owner,
          std::unique_lock<std::timed_mutex> lock, bool first)
        : owner_(owner), lock_(std::move(lock)), first_(first) {}
    Result<void> ValidatePrerequisites() const override {
      if (first_ && ++checks_ == 2) {
        std::unique_lock barrier(owner_.barrier_);
        owner_.at_final_validation_ = true;
        owner_.changed_.notify_all();
        if (!owner_.changed_.wait_for(barrier, std::chrono::seconds(2),
                                     [&] { return owner_.competing_acquisition_; }))
          return Result<void>::Err({"test_barrier_timeout", "competing offer did not acquire CCLC", {}});
      }
      return Result<void>::Ok();
    }
   private:
    ContendedCloseLeaseAdmissionPort& owner_;
    std::unique_lock<std::timed_mutex> lock_;
    bool first_;
    mutable int checks_{};
  };
  Result<std::unique_ptr<ICellCloseAdmissionLease>> AcquireCellCloseAdmissionLease() override {
    const bool first = acquires_.fetch_add(1) == 0;
    if (!first) {
      std::lock_guard barrier(barrier_);
      // The competing Offer calls this only while retaining CCLC exclusion.
      competing_acquisition_ = true;
      changed_.notify_all();
    }
    std::unique_lock lock(admission_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(2)))
      return Result<std::unique_ptr<ICellCloseAdmissionLease>>::Err(
          {"test_kernel_lease_timeout", "scoped settlement retained a reversed blocking lock", {}});
    std::unique_ptr<ICellCloseAdmissionLease> lease =
        std::make_unique<Lease>(*this, std::move(lock), first);
    return Result<std::unique_ptr<ICellCloseAdmissionLease>>::Ok(std::move(lease));
  }
  bool WaitForFinalValidation() {
    std::unique_lock barrier(barrier_);
    return changed_.wait_for(barrier, std::chrono::seconds(2),
                             [&] { return at_final_validation_; });
  }
 private:
  std::timed_mutex admission_;
  std::mutex barrier_;
  std::condition_variable changed_;
  std::atomic<int> acquires_{};
  bool at_final_validation_{};
  bool competing_acquisition_{};
};

TEST(ComponentLifecycleCoordinator,
     CloseScopedSettlementDoesNotWaitForCCLCWhileRetainingKernelAdmission) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"), 8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto brand = std::make_shared<FakeBrandPort>(journal,
      std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(brand);
  ContendedCloseLeaseAdmissionPort kernel;
  FakeClock clock;
  ManualScheduler scheduler;
  auto publication = ActivePublication();
  publication.durable_public_pointer_content_digest = ContentDigest(Sha('c'));
  ASSERT_TRUE(SeedCloseComponentFact(journal, publication).ok());
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations,
      ProductContextFor(snapshots, catalogs, resolver, publication), kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const CCLCLifecycleTargetRetirementScopeDTO scope{
      coordinator.request_owner_epoch(), publication.publication_generation,
      publication.durable_public_pointer_content_digest};
  auto settlement = std::async(std::launch::async, [&] {
    return coordinator.ReadLifecycleOwnerSettlement(scope);
  });
  ASSERT_TRUE(kernel.WaitForFinalValidation());
  auto offer = std::async(std::launch::async, [&] {
    return coordinator.ReadLifecycleTargetRetirementOffer();
  });
  const auto settled = settlement.get();
  ASSERT_FALSE(settled.ok());
  EXPECT_EQ(settled.error().code, "component_lifecycle_owner_settlement_unknown");
  const auto offered = offer.get();
  ASSERT_TRUE(offered.ok()) << offered.error().message;
  EXPECT_EQ(offered.value().scope, scope);
  EXPECT_EQ(brand->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_TRUE(journal.LoadLifecycleTargets().value().empty());
}

TEST(ComponentLifecycleCoordinator, CloseRetirementUsesActualScopedJournalAndReplaysBeforeSuccessorPublication) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"), 8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto brand = std::make_shared<FakeBrandPort>(journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(brand);
  CloseLeaseAdmissionPort kernel;
  FakeClock clock;
  ManualScheduler scheduler;
  auto previous = ActivePublication();
  previous.durable_public_pointer_content_digest = ContentDigest(Sha('c'));
  ASSERT_TRUE(SeedCloseComponentFact(journal, previous).ok());
  const auto old_context = ProductContextFor(snapshots, catalogs, resolver, previous);
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, old_context, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  ASSERT_TRUE(journal.SetLifecycleTarget(ControllableComponentId("component.1"), LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kOperator).ok());
  ComponentLifecycleCoordinatorOpenRequestPort open(coordinator);
  const auto offer = open.ReadLifecycleTargetRetirementOffer();
  ASSERT_TRUE(offer.ok()) << offer.error().message;
  ASSERT_EQ(offer.value().targets.size(), 1U);
  const CCLCLifecycleTargetRetirementIntentDTO intent{
      {RequestId("close.owner.1"), coordinator.request_owner_epoch(), "operator", std::string("close.correlation")},
      offer.value().scope, offer.value().targets};
  auto stale = intent;
  stale.scope.publication_generation = ActiveBundlePublicationGeneration(3U);
  const auto refused = open.RetireLifecycleTargetsForClose(stale);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_lifecycle_target_retirement_scope_stale");
  EXPECT_EQ(journal.LoadLifecycleTargets().value(), intent.expected_targets);
  const auto retired = open.RetireLifecycleTargetsForClose(intent);
  ASSERT_TRUE(retired.ok()) << retired.error().message;
  EXPECT_EQ(retired.value().request_context, intent.request_context);
  EXPECT_EQ(retired.value().retired_targets, intent.expected_targets);
  EXPECT_TRUE(journal.LoadLifecycleTargets().value().empty());
  EXPECT_TRUE(open.ReadLifecycleOwnerSettlement(intent.scope).ok());
  auto next = NextProductPublication(previous);
  next.durable_public_pointer_content_digest = ContentDigest(Sha('d'));
  const auto fact = journal.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.1"));
  ASSERT_TRUE(fact.ok()) << fact.error().message;
  ASSERT_TRUE(fact.value().durable_generation.has_value());
  auto successor_snapshot = Snapshot("close.successor.snapshot", ComponentLifecycleStateDTO::kDeclared,
                                     *fact.value().durable_generation);
  successor_snapshot.active_publication_receipt = next;
  const auto snapshot_digest = ComputeComponentLifecycleCurrentSnapshotDigest(successor_snapshot);
  ASSERT_TRUE(snapshot_digest.ok()) << snapshot_digest.error().message;
  successor_snapshot.snapshot_digest = snapshot_digest.value();
  snapshots.SetSnapshot(std::move(successor_snapshot));
  const auto successor = ProductContextFor(snapshots, catalogs, resolver, next);
  const auto exchange = coordinator.TryExchangeProductContext(old_context, successor, coordinator.request_owner_epoch(), previous);
  ASSERT_TRUE(exchange.ok()) << exchange.error().message;
  EXPECT_EQ(exchange.value().phase, ComponentLifecycleProductExchangePhase::kCompleted);
  ASSERT_TRUE(journal.SetLifecycleTarget(ControllableComponentId("component.1"), LifecycleTargetStateDTO::kPrepared, LifecycleTargetOriginDTO::kOperator).ok());
  const auto new_targets = journal.LoadLifecycleTargets().value();
  kernel.fail_at = kernel.checks + 1;
  const auto replay = open.RetireLifecycleTargetsForClose(intent);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(journal.LoadLifecycleTargets().value(), new_targets);
  EXPECT_EQ(open.ReadLifecycleTargetRetirementResult(intent).value()->request_context, intent.request_context);
  EXPECT_FALSE(open.ReadLifecycleOwnerSettlement(intent.scope).ok());
  EXPECT_EQ(brand->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

TEST(ComponentLifecycleCoordinator, CloseRetirementActualJournalRollsBackGuardChangeBeforeCommit) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"), 8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto brand = std::make_shared<FakeBrandPort>(journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(brand);
  CloseLeaseAdmissionPort kernel;
  FakeClock clock;
  ManualScheduler scheduler;
  auto publication = ActivePublication();
  publication.durable_public_pointer_content_digest = ContentDigest(Sha('c'));
  ASSERT_TRUE(SeedCloseComponentFact(journal, publication).ok());
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations,
      ProductContextFor(snapshots, catalogs, resolver, publication), kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  ASSERT_TRUE(journal.SetLifecycleTarget(ControllableComponentId("component.1"), LifecycleTargetStateDTO::kDisabled, LifecycleTargetOriginDTO::kOperator).ok());
  const auto offer = coordinator.ReadLifecycleTargetRetirementOffer();
  ASSERT_TRUE(offer.ok());
  const CCLCLifecycleTargetRetirementIntentDTO intent{
      {RequestId("close.guard.owner"), coordinator.request_owner_epoch(), "operator", std::nullopt},
      offer.value().scope, offer.value().targets};
  kernel.fail_at = kernel.checks + 2; // SQL acceptance succeeds; precommit changes.
  const auto refused = coordinator.RetireLifecycleTargetsForClose(intent);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "cell_close_stop_fault");
  EXPECT_EQ(journal.LoadLifecycleTargets().value(), intent.expected_targets);
  EXPECT_FALSE(coordinator.ReadLifecycleTargetRetirementResult(intent).value());
  EXPECT_EQ(brand->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

namespace {
enum class HeldCloseOrder { kRetireFirst, kPublishFirst, kUnreadable };

// Hold the actual accepted primitive response, after its claim and SQL
// acceptance, so retirement races the original completion rather than a
// guessed worker delay. The guard releases before coordinator destruction.
class HeldCloseDispatch final {
 public:
  void Enter() {
    std::unique_lock lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return released_; });
  }
  bool Wait() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(2), [&] { return entered_; });
  }
  void Release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }
 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_{};
  bool released_{};
};

void VerifyHeldCloseOrder(HeldCloseOrder order) {
  ScopedTemporaryDirectory temporary;
  const auto path = temporary.path() / std::string(kCCLCJournalFilename);
  SqliteCCLCJournal journal(path, CellId("cell.1"), 8U);
  auto plan_request = DeclaredNativeApplyPlanRequest();
  const auto plan = journal.PlanNativeApply(plan_request);
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  ASSERT_EQ(plan.value().components.size(), 1U);
  auto publication = ActivePublication();
  publication.durable_public_pointer_content_digest = ContentDigest(Sha('c'));
  publication.request.transaction_ref = NamedApplyPublicationTransactionDTO{ApplyId("apply.ros"), ApplyGeneration(1U)};
  publication.request.expected_previous_publication_generation = ActiveBundlePublicationGeneration(1U);
  ASSERT_TRUE(journal.AdoptNativeApplyFacts({publication, plan.value().components, {}}).ok());
  const auto generation = plan.value().components.front().generation;
  const auto catalog = EnableCatalog();
  auto snapshot = Snapshot("snapshot.close.held", ComponentLifecycleStateDTO::kDeclared,
                           generation, catalog);
  snapshot.active_publication_receipt = publication;
  FakeSnapshotSource snapshots({snapshot});
  FakeCatalogSource catalogs(catalog);
  HeldCloseDispatch dispatch;
  std::atomic<int> dispatched{};
  auto brand = std::make_shared<FakeBrandPort>(journal,
      std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                              ComponentLifecycleStateDTO::kPrepared,
                                              ComponentLifecycleStateDTO::kPrepared,
                                              ComponentLifecycleStateDTO::kActive},
      DispatchMode::kKnownSuccess, 1U, std::vector<LifecycleFactDTO>{}, [&] {
        if (++dispatched == 2) dispatch.Enter();
      });
  brand->set_catalog(catalog);
  FakeResolver resolver(brand, catalog);
  CloseLeaseAdmissionPort kernel;
  FakeReadinessPort readiness;
  kernel.readiness = &readiness;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  CCLCDomainReservationTable reservations;
  const auto context = std::make_shared<const ComponentLifecycleProductContext>(
      ComponentLifecycleProductContext{snapshots, catalogs, resolver, std::nullopt, &readiness,
        ComponentLifecycleProductIdentity{publication, {ControllableComponentId("component.1")}}});
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, context, kernel, clock, scheduler);
  struct ReleaseOnExit {
    HeldCloseDispatch& dispatch;
    ~ReleaseOnExit() { dispatch.Release(); }
  } guard{dispatch};
  ASSERT_TRUE(coordinator.Start().ok());
  auto prepare = PrepareIntent(coordinator.request_owner_epoch());
  prepare.expected_component_lifecycle_generation = generation;
  const auto prepared = coordinator.SubmitLifecycle(prepare);
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  ASSERT_TRUE(prepared.value().acceptance);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto prepared_status = journal.LoadRetainedLifecycleStatus(
      prepared.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(prepared_status.ok());
  ASSERT_TRUE(prepared_status.value());
  ASSERT_EQ(prepared_status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  snapshot.component_lifecycle_state = ComponentLifecycleStateDTO::kPrepared;
  snapshot.component_lifecycle_generation = prepared.value().acceptance->component_lifecycle_generation;
  snapshots.SetSnapshot(snapshot);
  auto enable = PrepareIntent(coordinator.request_owner_epoch());
  enable.request_context.request_id = RequestId("close.held.enable");
  enable.intent = ComponentLifecycleIntentKindDTO::kEnable;
  enable.expected_component_lifecycle_generation = snapshot.component_lifecycle_generation;
  const auto accepted = coordinator.SubmitLifecycle(enable);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance);
  ASSERT_TRUE(dispatch.Wait());
  const auto transaction = accepted.value().acceptance->lifecycle_transaction_id;
  const auto held = journal.LoadRetainedLifecycleStatus(transaction);
  ASSERT_TRUE(held.ok());
  ASSERT_TRUE(held.value());
  ASSERT_FALSE(held.value()->terminal_result);
  ASSERT_EQ(journal.LoadLifecycleTargets().value().size(), 1U);
  const auto offer = coordinator.ReadLifecycleTargetRetirementOffer();
  ASSERT_TRUE(offer.ok()) << offer.error().message;
  const CCLCLifecycleTargetRetirementIntentDTO retirement{
      {RequestId("close.held.retire"), coordinator.request_owner_epoch(), "operator", std::nullopt},
      offer.value().scope, offer.value().targets};
  if (order == HeldCloseOrder::kPublishFirst) {
    dispatch.Release();
    ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
    EXPECT_TRUE(readiness.state().command_eligible);
    ASSERT_TRUE(coordinator.RetireLifecycleTargetsForClose(retirement).ok());
  } else if (order == HeldCloseOrder::kUnreadable) {
    journal.Close();
  } else {
    ASSERT_TRUE(coordinator.RetireLifecycleTargetsForClose(retirement).ok());
    EXPECT_FALSE(readiness.state().command_eligible);
    // The original work remains held; Close does not invent a retirement
    // refusal or rearrange the subsequent owner settlement step.
    EXPECT_FALSE(journal.LoadRetainedLifecycleStatus(transaction).value()->terminal_result);
  }
  dispatch.Release();
  const bool drained = coordinator.DrainFor(std::chrono::seconds(2));
  if (order == HeldCloseOrder::kUnreadable) {
    EXPECT_FALSE(drained); // The original live claim is deliberately retained.
    EXPECT_FALSE(coordinator.accepting_new_requests());
    ASSERT_EQ(readiness.publications().size(), 1U); // Only completed Prepare.
    EXPECT_FALSE(readiness.publications().front().command_eligible);
    SqliteCCLCJournal reopened(path, CellId("cell.1"), 8U);
    const auto unfinished = reopened.LoadRetainedLifecycleStatus(transaction);
    ASSERT_TRUE(unfinished.ok());
    ASSERT_TRUE(unfinished.value());
    EXPECT_FALSE(unfinished.value()->terminal_result);
    return;
  }
  ASSERT_TRUE(drained);
  const auto status = journal.LoadRetainedLifecycleStatus(transaction);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded)
      << (status.value()->error ? status.value()->error->message : "");
  EXPECT_EQ(brand->dispatch_count(), 2U);
  const auto publications = readiness.publications();
  ASSERT_EQ(publications.size(), 2U);
  EXPECT_FALSE(publications.front().command_eligible);
  const bool expected_positive = order == HeldCloseOrder::kPublishFirst;
  EXPECT_EQ(publications.back().command_eligible, expected_positive);
  EXPECT_FALSE(readiness.state().command_eligible);
  EXPECT_EQ(kernel.closures, 1);
  EXPECT_TRUE(coordinator.ReadLifecycleOwnerSettlement(retirement.scope).ok());
  EXPECT_TRUE(reservations.snapshot().empty());
}
}  // namespace

TEST(ComponentLifecycleCoordinator, CloseRetirementSuppressesOriginalHeldEnablePublication) {
  VerifyHeldCloseOrder(HeldCloseOrder::kRetireFirst);
}
TEST(ComponentLifecycleCoordinator, CloseRetirementClosesOriginalEnableAlreadyPublished) {
  VerifyHeldCloseOrder(HeldCloseOrder::kPublishFirst);
}
TEST(ComponentLifecycleCoordinator, UnreadableTargetRetirementProofRetainsOriginalHeldClaim) {
  VerifyHeldCloseOrder(HeldCloseOrder::kUnreadable);
}

TEST(ComponentLifecycleCoordinator, CloseCommitUnknownClosesPreparedEligibilityButProvenRollbackDoesNot) {
  for (const bool unreadable : {false, true}) {
    SCOPED_TRACE(unreadable);
    test::CloseRetirementCommitFailure failure;
    test::ScopedCloseRetirementCommitFailure hook(failure);
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"), 8U);
    CCLCDomainReservationTable reservations;
    FakeSnapshotSource snapshots({Snapshot()});
    FakeCatalogSource catalogs(PrepareCatalog());
    auto brand = std::make_shared<FakeBrandPort>(journal,
        std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
    FakeResolver resolver(brand);
    CloseLeaseAdmissionPort kernel;
    FakeReadinessPort readiness;
    kernel.readiness = &readiness;
    FakeClock clock;
    ManualScheduler scheduler;
    auto publication = ActivePublication();
    publication.durable_public_pointer_content_digest = ContentDigest(Sha('c'));
    ASSERT_TRUE(SeedCloseComponentFact(journal, publication).ok());
    ComponentLifecycleCoordinator coordinator(Config(), journal, reservations,
        ProductContextFor(snapshots, catalogs, resolver, publication), kernel, clock, scheduler);
    ASSERT_TRUE(coordinator.Start().ok());
    ASSERT_TRUE(journal.SetLifecycleTarget(ControllableComponentId("component.1"),
        LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kOperator).ok());
    const auto offer = coordinator.ReadLifecycleTargetRetirementOffer();
    ASSERT_TRUE(offer.ok());
    const CCLCLifecycleTargetRetirementIntentDTO intent{
        {RequestId("close.owner.commit"), coordinator.request_owner_epoch(), "operator", std::nullopt},
        offer.value().scope, offer.value().targets};
    failure.fail_readback = unreadable;
    failure.armed = true;
    const auto result = coordinator.RetireLifecycleTargetsForClose(intent);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(failure.commit_failures, 1);
    EXPECT_EQ(result.error().code == "persistence_outcome_unknown", unreadable);
    EXPECT_EQ(coordinator.accepting_new_requests(), !unreadable);
    EXPECT_EQ(readiness.state().command_eligible, !unreadable);
    EXPECT_EQ(kernel.unknown_closures, unreadable ? 1 : 0);
    EXPECT_EQ(kernel.closures, 0);
    EXPECT_EQ(brand->dispatch_count(), 0U);
    failure.deny_reads = false;
    EXPECT_EQ(journal.LoadLifecycleTargets().value(), intent.expected_targets);
    EXPECT_FALSE(journal.ReadLifecycleTargetRetirementResult(intent).value());
    EXPECT_TRUE(reservations.snapshot().empty());
  }
}

// The grouped-read fixture has a qualified non-robot component. Its empty
// geometry inventory says nothing about a physical robot's measured readiness.
class ReadinessHealthResolver final : public ICCLCBrandLifecyclePortResolver {
 public:
  explicit ReadinessHealthResolver(std::shared_ptr<FakeBrandPort> port,
                                   BrandLifecyclePlanCatalogDTO catalog = PrepareCatalog())
      : port_(std::move(port)), provider_(Provider(catalog)) {}

  Result<std::shared_ptr<IBrandLifecyclePort>> ResolveExactLifecyclePort(
      const ResolvedLifecycleProviderOccurrenceDTO&) const override {
    ++execution_resolves;
    return Result<std::shared_ptr<IBrandLifecyclePort>>::Ok(port_);
  }

  Result<LifecycleDomainHealthDTO> ReadExactLifecycleHealth(
      const ResolvedLifecycleProviderOccurrenceDTO& provider,
      std::int64_t now_ns) const override {
    ++health_reads;
    last_now = now_ns;
    if (provider != provider_)
      return Result<LifecycleDomainHealthDTO>::Err(
          {"lifecycle_port_mismatch", "provider mismatch", {}});
    if (on_health_read) on_health_read();
    if (health_error)
      return Result<LifecycleDomainHealthDTO>::Err(*health_error);
    return Result<LifecycleDomainHealthDTO>::Ok(health);
  }

  LifecycleDomainHealthDTO health{ComponentLifecycleStateDTO::kDeclared, {}, std::nullopt};
  std::optional<ErrorDTO> health_error;
  std::function<void()> on_health_read;
  mutable std::size_t health_reads{};
  mutable std::size_t execution_resolves{};
  mutable std::int64_t last_now{};
 private:
  std::shared_ptr<FakeBrandPort> port_;
  ResolvedLifecycleProviderOccurrenceDTO provider_;
};

[[nodiscard]] ComponentLifecycleCurrentSnapshotDTO VerifiedReadinessSnapshot() {
  auto value = Snapshot();
  const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(value);
  if (!digest.ok()) throw std::runtime_error(digest.error().message);
  value.snapshot_digest = digest.value();
  return value;
}

struct GroupedReadinessFixture final {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal{temporary.path() / std::string(kCCLCJournalFilename),
                           CellId("cell.1"), 8U};
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots{{VerifiedReadinessSnapshot()}};
  FakeCatalogSource catalogs{PrepareCatalog()};
  std::shared_ptr<FakeBrandPort> port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
  ReadinessHealthResolver resolver{port};
  CountingAdmissionReadPort kernel;
  FakeClock clock;
  ManualScheduler scheduler;
  bool association_available = true;
  bool association_current = true;
  std::size_t validations{};
  std::size_t target_fact_reads{};
  ComponentLifecycleReadinessTargetFactsDTO target_facts{{}, std::nullopt};
  std::function<void()> before_target_facts;
  std::shared_ptr<const ComponentLifecycleProductContext> context;
  std::unique_ptr<ComponentLifecycleCoordinator> coordinator;

  explicit GroupedReadinessFixture(bool measured_state_required = false) {
    std::vector<ConfiguredRobotMeasuredStateRequirement> measured_robots;
    if (measured_state_required) {
      measured_robots.push_back({ControllableComponentId("component.1"),
                                 {ResourceId("joint.1")}, {}});
    }
    context = std::make_shared<const ComponentLifecycleProductContext>(
        ComponentLifecycleProductContext{
            snapshots, catalogs, resolver, std::nullopt, nullptr,
            ComponentLifecycleProductIdentity{ActivePublication(),
                                               {ControllableComponentId("component.1")}},
            nullptr,
            std::make_shared<const ConfiguredRobotMeasuredStateInventory>(
                std::move(measured_robots)),
            [this, measured_state_required](const ComponentLifecycleCurrentSnapshotDTO&) {
              if (!association_available)
                return Result<ComponentLifecycleReadinessSourceView>::Err(
                    {"route_source_unavailable", "route source is unavailable", {}});
              return Result<ComponentLifecycleReadinessSourceView>::Ok(
                  {measured_state_required
                       ? std::optional<RobotMeasuredStateSourceView>(
                             RobotMeasuredStateSourceView{
                                 ControllableComponentId("component.1"), {}})
                       : std::nullopt,
                   [this] {
                    ++validations;
                    return association_current ? Result<void>::Ok()
                        : Result<void>::Err({"route_source_changed", "route source changed", {}});
                  }});
            }});
    coordinator = std::make_unique<ComponentLifecycleCoordinator>(
        Config(), journal, reservations, context, kernel, clock, scheduler);
    const auto started = coordinator->Start();
    if (!started.ok()) throw std::runtime_error(started.error().message);
  }

  Result<CCLCComponentReadDTO> Read() {
    return coordinator->ReadCurrentComponentView(ControllableComponentId("component.1"),
        [this] { return Result<ComponentLifecycleTargetViewDTO>::Ok(
                     {target_facts.facts.target, std::nullopt, std::nullopt, std::nullopt}); },
        [this] {
          ++target_fact_reads;
          if (before_target_facts) before_target_facts();
          return Result<ComponentLifecycleReadinessTargetFactsDTO>::Ok(target_facts);
        });
  }

  // Readiness resolves the port only for its pure admission query (V2-1).
  void ExpectNoExecutionEffects() const {
    EXPECT_EQ(port->observation_count(), 0U);
    EXPECT_EQ(port->dispatch_count(), 0U);
    EXPECT_EQ(kernel.claim_acquires.load(), 0U);
    EXPECT_TRUE(reservations.snapshot().empty());
  }
};

TEST(ComponentLifecycleCoordinator, GroupedReadinessUsesExactPureHealthWithoutIntentOrObservation) {
  GroupedReadinessFixture fixture;
  const auto owner = fixture.coordinator->request_owner_epoch();
  for (const auto now : {123, 456}) {
    fixture.clock.set(now);
    auto read = fixture.Read();
    ASSERT_TRUE(read.ok()) << read.error().message;
    EXPECT_TRUE(read.value().readiness.ready);
    EXPECT_EQ(read.value().readiness.request_owner_epoch, owner);
    EXPECT_EQ(read.value().readiness.evaluated_at_ns, now);
    EXPECT_EQ(read.value().readiness.next_intent, ComponentLifecycleIntentKindDTO::kPrepare);
    EXPECT_EQ(read.value().readiness.requested_state, ComponentLifecycleStateDTO::kPrepared);
    ASSERT_EQ(read.value().readiness.domains.size(), 1U);
    EXPECT_EQ(read.value().readiness.domains.front().provider_occurrence,
              Provider(PrepareCatalog()));
    EXPECT_EQ(read.value().readiness.domains.front().decision_kind,
              LifecycleDomainPlanDecisionKind::kDispatchPrimitive);
    EXPECT_EQ(read.value().readiness.measured_state.required, false);
    EXPECT_FALSE(read.value().readiness.measured_state.available);
    EXPECT_EQ(fixture.resolver.last_now, now);
    EXPECT_EQ(read.value().snapshot, VerifiedReadinessSnapshot());
  }
  EXPECT_EQ(fixture.resolver.health_reads, 2U);
  EXPECT_EQ(fixture.validations, 4U);
  EXPECT_EQ(fixture.target_fact_reads, 4U);
  EXPECT_EQ(fixture.coordinator->request_owner_epoch(), owner);
  EXPECT_TRUE(fixture.journal.ReadQuiescentOwner(owner).ok());
  fixture.ExpectNoExecutionEffects();
}

// A journal written before partial effects kept a state: numbered, no fact.
TEST(ComponentLifecycleCoordinator, MissingCurrentFactReadsHealthOrWarnsReconstructed) {
  GroupedReadinessFixture fixture;
  fixture.clock.set(123);
  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open((fixture.temporary.path() / std::string(kCCLCJournalFilename)).c_str(),
                         &raw),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw,
                         "INSERT INTO lifecycle_component_watermarks("
                         "controllable_component_id,component_lifecycle_generation) "
                         "VALUES('component.1',3)",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
  const auto fact =
      fixture.journal.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.1"));
  ASSERT_TRUE(fact.ok() && fact.value().durable_generation && !fact.value().current_state);

  // Fresh fault-free health: the read carries no warning.
  auto read = fixture.Read();
  ASSERT_TRUE(read.ok()) << read.error().message;
  EXPECT_TRUE(read.value().readiness.ready);
  EXPECT_FALSE(read.value().readiness.error);

  // No health: Declared with the typed warning, never a failed read.
  fixture.resolver.health_error = ErrorDTO{"lifecycle_health_unavailable", "no health", {}};
  read = fixture.Read();
  ASSERT_TRUE(read.ok()) << read.error().message;
  EXPECT_FALSE(read.value().readiness.ready);
  ASSERT_TRUE(read.value().readiness.error);
  EXPECT_EQ(read.value().readiness.error->code, kComponentLifecycleStateReconstructedCode);
  EXPECT_EQ(read.value().readiness.error->details.at("reason"), "current_state_fact_missing");
  EXPECT_EQ(read.value().readiness.error->details.at("blocking_code"),
            "lifecycle_health_unavailable");
  fixture.ExpectNoExecutionEffects();
}

TEST(ComponentLifecycleCoordinator,
     MissingMeasuredStateWarnsWithoutBlockingLifecycleReadiness) {
  GroupedReadinessFixture fixture(true);
  fixture.clock.set(123);
  const auto read = fixture.Read();
  ASSERT_TRUE(read.ok()) << read.error().message;
  EXPECT_TRUE(read.value().readiness.ready);
  EXPECT_FALSE(read.value().readiness.error);
  EXPECT_EQ(read.value().readiness.next_intent,
            ComponentLifecycleIntentKindDTO::kPrepare);
  EXPECT_EQ(read.value().readiness.measured_state.required, true);
  EXPECT_EQ(read.value().readiness.measured_state.available, false);
  ASSERT_TRUE(read.value().readiness.measured_state.error);
  EXPECT_EQ(read.value().readiness.measured_state.error->code,
            "configured_robot_measured_state_unavailable");
  EXPECT_EQ(read.value().readiness.measured_state.error->details.at("reason"),
            "measured_geometry_coordinates_missing");
  fixture.ExpectNoExecutionEffects();
}

TEST(ComponentLifecycleCoordinator, EndpointConflictsRefusePrepareBeforePrimitiveEffects) {
  for (const auto code : {"duplicate_component_instance",
                          "component_instance_graph_unsettled"}) {
    SCOPED_TRACE(code);
    GroupedReadinessFixture fixture;
    fixture.clock.set(100);
    fixture.resolver.health_error = ErrorDTO{code, "selected endpoint conflict",
                                            {{"topic", "/selected/activity"}}};
    const auto read = fixture.Read();
    ASSERT_TRUE(read.ok());
    EXPECT_FALSE(read.value().readiness.ready);
    ASSERT_TRUE(read.value().readiness.error);
    EXPECT_EQ(read.value().readiness.error->code, code);
    const auto accepted = fixture.coordinator->SubmitLifecycle(
        PrepareIntent(fixture.coordinator->request_owner_epoch()));
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    ASSERT_TRUE(accepted.value().acceptance);
    ASSERT_TRUE(fixture.coordinator->DrainFor(std::chrono::seconds(2)));
    const auto status = fixture.coordinator->LoadLifecycleStatus(
        accepted.value().acceptance->lifecycle_transaction_id);
    ASSERT_TRUE(status.ok());
    ASSERT_TRUE(status.value());
    EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
    ASSERT_TRUE(status.value()->error);
    EXPECT_EQ(status.value()->error->code, code);
    EXPECT_EQ(fixture.port->dispatch_count(), 0U);
    EXPECT_TRUE(fixture.reservations.snapshot().empty());
  }
}

TEST(ComponentLifecycleCoordinator, EndpointConflictRefusesEnableAfterGenuineCompletedPrepare) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"), 8U);
  CCLCDomainReservationTable reservations;
  const auto catalog = EnableCatalog();
  auto snapshot = Snapshot("endpoint.enable", ComponentLifecycleStateDTO::kDeclared,
                           ComponentLifecycleGeneration(3U), catalog);
  FakeSnapshotSource snapshots({snapshot});
  FakeCatalogSource catalogs(catalog);
  auto port = std::make_shared<FakeBrandPort>(journal,
      std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                              ComponentLifecycleStateDTO::kPrepared,
                                              ComponentLifecycleStateDTO::kPrepared});
  port->set_catalog(catalog);
  ReadinessHealthResolver resolver(port, catalog);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto prepared = coordinator.SubmitLifecycle(PrepareIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  ASSERT_TRUE(prepared.value().acceptance);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto prepared_status = coordinator.LoadLifecycleStatus(
      prepared.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(prepared_status.ok());
  ASSERT_TRUE(prepared_status.value());
  ASSERT_EQ(prepared_status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  ASSERT_EQ(port->dispatch_count(), 1U);
  snapshot.component_lifecycle_state = ComponentLifecycleStateDTO::kPrepared;
  snapshot.component_lifecycle_generation = prepared.value().acceptance->component_lifecycle_generation;
  snapshots.SetSnapshot(snapshot);
  auto enable = PrepareIntent(coordinator.request_owner_epoch());
  enable.request_context.request_id = RequestId("endpoint.enable");
  enable.intent = ComponentLifecycleIntentKindDTO::kEnable;
  enable.expected_component_lifecycle_generation = snapshot.component_lifecycle_generation;
  resolver.health_error = ErrorDTO{"duplicate_component_instance", "selected endpoint conflict", {}};
  const auto accepted = coordinator.SubmitLifecycle(enable);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status = coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
  ASSERT_TRUE(status.value()->error);
  EXPECT_EQ(status.value()->error->code, "duplicate_component_instance");
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_TRUE(reservations.snapshot().empty());
}

TEST(ComponentLifecycleCoordinator, EndpointConflictDoesNotBlockOrdinaryDisable) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"), 8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots(OrdinaryDisableSnapshots());
  FakeCatalogSource catalogs(DisableCatalog());
  auto port = std::make_shared<FakeBrandPort>(journal,
      std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kActive,
                                              ComponentLifecycleStateDTO::kDisabled});
  ReadinessHealthResolver resolver(port);
  resolver.health_error = ErrorDTO{"duplicate_component_instance", "selected endpoint conflict", {}};
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto accepted = coordinator.SubmitLifecycle(DisableIntent(coordinator.request_owner_epoch()));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance);
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status = coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value());
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_EQ(resolver.health_reads, 0U);
}

TEST(ComponentLifecycleCoordinator, GroupedReadinessForwardsActivationStepWithoutChangingAdmission) {
  GroupedReadinessFixture fixture;
  for (const auto metadata : {std::optional<bool>{}, std::optional<bool>{false},
                              std::optional<bool>{true}}) {
    fixture.resolver.health.has_activation_step = metadata;
    const auto read = fixture.Read();
    ASSERT_TRUE(read.ok()) << read.error().message;
    ASSERT_EQ(read.value().readiness.domains.size(), 1U);
    const auto& row = read.value().readiness.domains.front();
    EXPECT_EQ(row.has_activation_step, metadata);
    EXPECT_TRUE(read.value().readiness.ready);
    EXPECT_EQ(read.value().readiness.next_intent, ComponentLifecycleIntentKindDTO::kPrepare);
    EXPECT_EQ(row.decision_kind, LifecycleDomainPlanDecisionKind::kDispatchPrimitive);
    EXPECT_EQ(row.health->facts, fixture.resolver.health.facts);
  }
  fixture.ExpectNoExecutionEffects();
}

// V2-1: readiness asks the adapter's own admission over the same health.
TEST(ComponentLifecycleCoordinator, GroupedReadinessUsesTheAdaptersOwnAdmission) {
  GroupedReadinessFixture fixture;
  fixture.port->set_admission(std::make_shared<AlreadyDoneAdmission>());
  const auto read = fixture.Read();
  ASSERT_TRUE(read.ok()) << read.error().message;
  ASSERT_EQ(read.value().readiness.domains.size(), 1U);
  EXPECT_EQ(read.value().readiness.domains.front().decision_kind,
            LifecycleDomainPlanDecisionKind::kAlreadySatisfied);
  fixture.ExpectNoExecutionEffects();
}

TEST(ComponentLifecycleCoordinator, GroupedReadinessDoesNotUsePredecessorHealthWhenRouteSourceUnavailable) {
  GroupedReadinessFixture fixture;
  fixture.association_available = false;
  const auto read = fixture.Read();
  ASSERT_TRUE(read.ok()) << read.error().message;
  EXPECT_FALSE(read.value().readiness.ready);
  ASSERT_TRUE(read.value().readiness.error);
  EXPECT_EQ(read.value().readiness.error->code, "route_source_unavailable");
  ASSERT_EQ(read.value().readiness.domains.size(), 1U);
  EXPECT_FALSE(read.value().readiness.domains.front().health);
  EXPECT_FALSE(read.value().readiness.domains.front().decision_kind);
  EXPECT_EQ(fixture.resolver.health_reads, 0U);
  fixture.ExpectNoExecutionEffects();
}

TEST(ComponentLifecycleCoordinator, GroupedReadinessRejectsRouteAssociationChangingDuringHealthRead) {
  GroupedReadinessFixture fixture;
  fixture.resolver.on_health_read = [&] { fixture.association_current = false; };
  const auto read = fixture.Read();
  ASSERT_FALSE(read.ok());
  EXPECT_EQ(read.error().code, "route_source_changed");
  EXPECT_EQ(fixture.validations, 2U);
  EXPECT_EQ(fixture.resolver.health_reads, 1U);
  fixture.ExpectNoExecutionEffects();
}

TEST(ComponentLifecycleCoordinator, GroupedReadinessRejectsRecoveryFactsChangingDuringRead) {
  GroupedReadinessFixture fixture;
  fixture.before_target_facts = [&] {
    if (fixture.target_fact_reads == 2U) ++fixture.target_facts.facts.recovery_generation;
  };
  const auto read = fixture.Read();
  ASSERT_FALSE(read.ok());
  EXPECT_EQ(read.error().code, "component_lifecycle_facts_stale");
  EXPECT_EQ(read.error().details.at("reason"), "component_read_changed");
  fixture.ExpectNoExecutionEffects();
}

TEST(ComponentLifecycleCoordinator, GroupedReadinessDiagnosticDoesNotInventAnotherPlanPredicate) {
  GroupedReadinessFixture fixture;
  fixture.resolver.health.diagnostic = ErrorDTO{
      "transport_note", "availability diagnostic", {{"reason", "availability"}}};
  fixture.resolver.health.facts = {{LifecycleFactId(std::string(kLifecycleBlockingCauseFactId)),
                                  TokenFactValue("availability")}};
  const auto read = fixture.Read();
  ASSERT_TRUE(read.ok()) << read.error().message;
  EXPECT_TRUE(read.value().readiness.ready);
  ASSERT_TRUE(read.value().readiness.domains.front().health);
  EXPECT_EQ(read.value().readiness.domains.front().health->diagnostic,
            fixture.resolver.health.diagnostic);
  EXPECT_FALSE(read.value().readiness.domains.front().error);
  fixture.ExpectNoExecutionEffects();
}

TEST(ComponentLifecycleCoordinator, GroupedReadinessHonorsActualRecoveredCauseWithoutClearingIt) {
  GroupedReadinessFixture fixture;
  fixture.resolver.health.facts = {{LifecycleFactId(std::string(kLifecycleBlockingCauseFactId)),
                                  TokenFactValue("protective_stop")}};
  const auto blocked = fixture.Read();
  ASSERT_TRUE(blocked.ok()) << blocked.error().message;
  EXPECT_FALSE(blocked.value().readiness.ready);
  ASSERT_TRUE(blocked.value().readiness.error);
  EXPECT_EQ(blocked.value().readiness.error->details.at("cause_class"), "protective_stop");
  fixture.target_facts.facts.target = ComponentLifecycleTargetDTO{
      ControllableComponentId("component.1"), LifecycleTargetStateDTO::kActive,
      LifecycleTargetOriginDTO::kOperator, 1U, false,
      LifecycleBlockingCauseClassDTO::kProtectiveStop};
  const auto target_before = fixture.target_facts;
  const auto allowed = fixture.Read();
  ASSERT_TRUE(allowed.ok()) << allowed.error().message;
  EXPECT_TRUE(allowed.value().readiness.ready);
  EXPECT_EQ(fixture.target_facts, target_before);
  fixture.ExpectNoExecutionEffects();
}

[[nodiscard]] ComponentLifecycleCurrentSnapshotDTO ExchangeSnapshot(
    const ActiveBundlePublicationReceiptDTO& publication, std::uint64_t generation,
    ComponentLifecycleStateDTO state = ComponentLifecycleStateDTO::kDeclared) {
  auto snapshot = Snapshot("exchange.snapshot", state, ComponentLifecycleGeneration(generation));
  snapshot.active_publication_receipt = publication;
  const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(snapshot);
  if (!digest.ok()) throw std::runtime_error(digest.error().message);
  snapshot.snapshot_digest = digest.value();
  return snapshot;
}

class ExchangeGenerationFixture final {
 public:
  ExchangeGenerationFixture()
      : journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"), 8U),
        boundary(journal),
        previous(SeedFour()),
        successor(NextProductPublication(previous)),
        old_snapshots({ExchangeSnapshot(previous, 4U)}),
        new_snapshots({ExchangeSnapshot(successor, 5U)}),
        catalogs(PrepareCatalog()),
        port(std::make_shared<FakeBrandPort>(
            journal,
            std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                    ComponentLifecycleStateDTO::kPrepared})),
        resolver(port),
        old_context(ProductContextFor(old_snapshots, catalogs, resolver, previous)),
        new_context(ProductContextFor(new_snapshots, catalogs, resolver, successor)),
        coordinator(Config(), boundary, reservations, old_context, kernel, clock, scheduler) {
    clock.set(100);
    const auto started = coordinator.Start();
    if (!started.ok()) throw std::runtime_error(started.error().message);
  }

  void AdoptSuccessor() {
    auto request = DeclaredNativeApplyPlanRequest();
    request.apply =
        std::get<NamedApplyPublicationTransactionDTO>(successor.request.transaction_ref);
    request.candidate_retained_bundle_id = successor.request.candidate_retained_bundle_id;
    request.candidate_graph_revision = successor.request.candidate_graph_revision;
    const auto plan = journal.PlanNativeApply(request);
    if (!plan.ok()) throw std::runtime_error(plan.error().message);
    if (plan.value().components.front().generation != ComponentLifecycleGeneration(5U))
      throw std::runtime_error("replacement did not reserve the genuine fifth generation");
    const auto adopted = journal.AdoptNativeApplyFacts({successor, plan.value().components, {}});
    if (!adopted.ok()) throw std::runtime_error(adopted.error().message);
  }

  auto Exchange() {
    return coordinator.TryExchangeProductContext(old_context, new_context,
                                                 coordinator.request_owner_epoch(), previous);
  }
  void ExpectOldProduct() {
    const auto current = coordinator.AcquireProductPin();
    ASSERT_TRUE(current.ok());
    EXPECT_EQ(&current.value().context(), old_context.get());
    EXPECT_EQ(port->dispatch_count(), 0U);
  }

  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal;
  SettlementBarrierJournal boundary;
  ActiveBundlePublicationReceiptDTO previous;
  ActiveBundlePublicationReceiptDTO successor;
  FakeSnapshotSource old_snapshots;
  FakeSnapshotSource new_snapshots;
  FakeCatalogSource catalogs;
  std::shared_ptr<FakeBrandPort> port;
  FakeResolver resolver;
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  ManualScheduler scheduler;
  CCLCDomainReservationTable reservations;
  std::shared_ptr<const ComponentLifecycleProductContext> old_context;
  std::shared_ptr<const ComponentLifecycleProductContext> new_context;
  ComponentLifecycleCoordinator coordinator;

 private:
  ActiveBundlePublicationReceiptDTO SeedFour() {
    auto publication = ActivePublication();
    for (std::uint64_t generation = 1U; generation <= 4U; ++generation) {
      publication = NextProductPublication(publication);
      publication.request.transaction_ref = NamedApplyPublicationTransactionDTO{
          ApplyId("seed.apply." + std::to_string(generation)), ApplyGeneration(generation)};
      auto request = DeclaredNativeApplyPlanRequest();
      request.apply =
          std::get<NamedApplyPublicationTransactionDTO>(publication.request.transaction_ref);
      const auto plan = journal.PlanNativeApply(request);
      if (!plan.ok()) throw std::runtime_error(plan.error().message);
      const auto adopted =
          journal.AdoptNativeApplyFacts({publication, plan.value().components, {}});
      if (!adopted.ok()) throw std::runtime_error(adopted.error().message);
    }
    return publication;
  }
};

TEST(ComponentLifecycleCoordinator, ProductExchangeSynchronizesAdoptedGenerationBeforePrepare) {
  std::string stage = "construct durable generation fixture";
  try {
    ExchangeGenerationFixture fixture;
    stage = "adopt successor";
    fixture.AdoptSuccessor();
    stage = "exchange";
    const auto exchanged = fixture.Exchange();
    ASSERT_TRUE(exchanged.ok()) << exchanged.error().message;
    EXPECT_EQ(exchanged.value().phase, ComponentLifecycleProductExchangePhase::kCompleted);
    const auto reads = fixture.new_snapshots.read_count();
    stage = "replay";
    ASSERT_TRUE(fixture.Exchange().ok());
    EXPECT_EQ(fixture.new_snapshots.read_count(), reads);
    auto intent = PrepareIntent(fixture.coordinator.request_owner_epoch());
    intent.expected_component_lifecycle_generation = ComponentLifecycleGeneration(5U);
    stage = "prepare generation five";
    const auto prepared = fixture.coordinator.SubmitLifecycle(intent);
    ASSERT_TRUE(prepared.ok()) << prepared.error().message;
    ASSERT_TRUE(prepared.value().acceptance);
    EXPECT_EQ(prepared.value().acceptance->component_lifecycle_generation,
              ComponentLifecycleGeneration(6U));
    stage = "drain preparation";
    EXPECT_TRUE(fixture.coordinator.DrainFor(std::chrono::seconds(2)));
  } catch (const std::exception& error) {
    FAIL() << stage << ": " << error.what();
  }
}

TEST(ComponentLifecycleCoordinator, ProductExchangeRejectsSnapshotGenerationAndForeignScope) {
  for (const unsigned variant : {0U, 1U, 2U, 3U}) {
    SCOPED_TRACE(variant);
    ExchangeGenerationFixture fixture;
    fixture.AdoptSuccessor();
    auto snapshot = ExchangeSnapshot(fixture.successor, variant == 0U ? 4U : 5U);
    if (variant == 1U) snapshot.controllable_component_id = ControllableComponentId("foreign");
    if (variant == 2U) snapshot.active_publication_receipt = fixture.previous;
    if (variant == 3U) snapshot.snapshot_digest = ContentDigest(Sha('f'));
    fixture.new_snapshots.SetSnapshot(std::move(snapshot));
    EXPECT_FALSE(fixture.Exchange().ok());
    fixture.ExpectOldProduct();
  }
}

TEST(ComponentLifecycleCoordinator, ProductExchangeRefusesMissingAndLowerExistingFloor) {
  for (const bool missing : {false, true}) {
    SCOPED_TRACE(missing);
    ExchangeGenerationFixture fixture;
    fixture.AdoptSuccessor();
    fixture.new_snapshots.SetSnapshot(ExchangeSnapshot(fixture.successor, 3U));
    fixture.boundary.on_component_fact_read = [&](auto& fact) {
      fact.durable_generation =
          missing ? std::nullopt : std::optional(ComponentLifecycleGeneration(3U));
      if (missing) fact.current_state.reset();
    };
    const auto refused = fixture.Exchange();
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().details.at("reason"), "product_generation_floor_regressed");
    fixture.ExpectOldProduct();
  }
}

TEST(ComponentLifecycleCoordinator, ProductExchangeRefusesFactsChangingBeforeCommit) {
  for (const bool change_snapshot : {false, true}) {
    SCOPED_TRACE(change_snapshot);
    ExchangeGenerationFixture fixture;
    fixture.AdoptSuccessor();
    unsigned reads = 0U;
    fixture.boundary.on_component_fact_read = [&](auto& fact) {
      if (++reads == 2U) {
        if (change_snapshot)
          fixture.new_snapshots.SetSnapshot(
              ExchangeSnapshot(fixture.successor, 5U, ComponentLifecycleStateDTO::kPrepared));
        else
          fact.current_state = ComponentLifecycleStateDTO::kPrepared;
      }
    };
    const auto refused = fixture.Exchange();
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().details.at("reason"), "product_generation_proof_changed");
    fixture.ExpectOldProduct();
  }
}

TEST(ComponentLifecycleCoordinator, ProductExchangeRefusesOwnerClosingDuringGenerationProof) {
  ExchangeGenerationFixture fixture;
  fixture.AdoptSuccessor();
  fixture.boundary.on_component_fact_read = [&](auto&) { fixture.coordinator.CloseAdmission(); };
  EXPECT_FALSE(fixture.Exchange().ok());
  fixture.ExpectOldProduct();
}

TEST(ComponentLifecycleCoordinator, ProductExchangeEmptyMembershipPreservesOutsideFloor) {
  ExchangeGenerationFixture fixture;
  // Remove through the genuine plan/adoption: no current component remains,
  // while its durable generation floor must survive the empty product.
  auto removal = DeclaredNativeApplyPlanRequest();
  removal.apply =
      std::get<NamedApplyPublicationTransactionDTO>(fixture.successor.request.transaction_ref);
  removal.components.clear();
  removal.removed_components = {ControllableComponentId("component.1")};
  const auto plan = fixture.journal.PlanNativeApply(removal);
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  ASSERT_TRUE(fixture.journal.AdoptNativeApplyFacts({fixture.successor, {}, {}}).ok());
  fixture.new_context =
      std::make_shared<const ComponentLifecycleProductContext>(ComponentLifecycleProductContext{
          fixture.new_snapshots, fixture.catalogs, fixture.resolver, std::nullopt, nullptr,
          ComponentLifecycleProductIdentity{fixture.successor, {}}});
  const auto exchanged = fixture.Exchange();
  ASSERT_TRUE(exchanged.ok()) << exchanged.error().message;
  EXPECT_EQ(fixture.new_snapshots.read_count(), 0U);
  auto reintroduced = NextProductPublication(fixture.successor);
  fixture.new_snapshots.SetSnapshot(ExchangeSnapshot(reintroduced, 3U));
  fixture.boundary.on_component_fact_read = [](auto& fact) {
    fact.durable_generation = ComponentLifecycleGeneration(3U);
  };
  const auto next =
      ProductContextFor(fixture.new_snapshots, fixture.catalogs, fixture.resolver, reintroduced);
  const auto refused = fixture.coordinator.TryExchangeProductContext(
      fixture.new_context, next, fixture.coordinator.request_owner_epoch(), fixture.successor);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"), "product_generation_floor_regressed");
}

TEST(ComponentLifecycleCoordinator, ProductExchangeWaitsForWholePassAndWithdrawsForeignRequest) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource old_snapshots({Snapshot()});
  FakeSnapshotSource new_snapshots({Snapshot("successor.snapshot")});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  ManualScheduler scheduler;
  const auto previous = ActivePublication();
  const auto successor = NextProductPublication(previous);
  ASSERT_TRUE(previous.valid());
  ASSERT_TRUE(successor.valid());
  const auto old_context = ProductContextFor(old_snapshots, catalogs, resolver, previous);
  const auto new_context = ProductContextFor(new_snapshots, catalogs, resolver, successor);
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, old_context, kernel,
                                            clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto owner = coordinator.request_owner_epoch();
  {
    auto pass = coordinator.AcquireProductPin();
    ASSERT_TRUE(pass.ok());
    const auto pending =
        coordinator.TryExchangeProductContext(old_context, new_context, owner, previous);
    ASSERT_TRUE(pending.ok());
    EXPECT_EQ(pending.value().phase, ComponentLifecycleProductExchangePhase::kPending);
    EXPECT_EQ(&pass.value().context().current_snapshot_source, &old_snapshots);
    EXPECT_FALSE(coordinator.AcquireProductPin().ok());
    // The same journal remains readable while product operations are pending.
    EXPECT_TRUE(coordinator.ReadLifecycleRequestResult(PrepareIntent(owner)).ok());
    const auto refused = coordinator.TryExchangeProductContext(
        old_context, new_context, RequestOwnerEpoch("foreign.owner"), previous);
    EXPECT_FALSE(refused.ok());
    EXPECT_TRUE(coordinator.AcquireProductPin().ok());
  }
  const auto complete =
      coordinator.TryExchangeProductContext(old_context, new_context, owner, previous);
  ASSERT_TRUE(complete.ok()) << complete.error().message;
  EXPECT_EQ(complete.value().phase, ComponentLifecycleProductExchangePhase::kCompleted);
  const auto current = coordinator.AcquireProductPin();
  ASSERT_TRUE(current.ok());
  EXPECT_EQ(&current.value().context().current_snapshot_source, &new_snapshots);
  EXPECT_EQ(coordinator.request_owner_epoch(), owner);
  EXPECT_EQ(port->dispatch_count(), 0U);
  const auto replay =
      coordinator.TryExchangeProductContext(old_context, new_context, owner, previous);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value().phase, ComponentLifecycleProductExchangePhase::kCompleted);
}

TEST(ComponentLifecycleCoordinator, ProductExchangePinsAdmissionRereadThroughDurableAcceptance) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  BlockingRereadSnapshotSource old_snapshots(Snapshot());
  FakeSnapshotSource new_snapshots({Snapshot("successor.snapshot")});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  const auto previous = ActivePublication();
  const auto old_context = ProductContextFor(old_snapshots, catalogs, resolver, previous);
  const auto new_context =
      ProductContextFor(new_snapshots, catalogs, resolver, NextProductPublication(previous));
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, old_context, kernel,
                                            clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto owner = coordinator.request_owner_epoch();
  const auto intent = PrepareIntent(owner);
  auto submitted =
      std::async(std::launch::async, [&] { return coordinator.SubmitLifecycle(intent); });
  const bool blocked = old_snapshots.WaitForBlockedReread(std::chrono::seconds(2));
  EXPECT_TRUE(blocked);
  if (blocked) {
    const auto pending =
        coordinator.TryExchangeProductContext(old_context, new_context, owner, previous);
    EXPECT_TRUE(pending.ok());
    if (pending.ok()) {
      EXPECT_EQ(pending.value().phase, ComponentLifecycleProductExchangePhase::kPending);
    }
    EXPECT_EQ(new_snapshots.read_count(), 0U);
    EXPECT_EQ(port->dispatch_count(), 0U);
  }
  old_snapshots.ReleaseReread();
  const auto accepted = submitted.get();
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto replay = coordinator.SubmitLifecycle(intent);
  ASSERT_TRUE(replay.ok());
  ASSERT_TRUE(replay.value().acceptance.has_value());
  EXPECT_EQ(replay.value().acceptance->replay_status, RequestReplayStatusDTO::kReplayedExact);
  auto original_acknowledgement = *replay.value().acceptance;
  original_acknowledgement.replay_status = accepted.value().acceptance->replay_status;
  EXPECT_EQ(original_acknowledgement, *accepted.value().acceptance);
  new_snapshots.SetSnapshot(ExchangeSnapshot(new_context->identity->publication, 4U,
                                             ComponentLifecycleStateDTO::kPrepared));
  const auto complete =
      coordinator.TryExchangeProductContext(old_context, new_context, owner, previous);
  ASSERT_TRUE(complete.ok()) << complete.error().message;
  EXPECT_EQ(complete.value().phase, ComponentLifecycleProductExchangePhase::kCompleted);
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_EQ(new_snapshots.read_count(), 2U);
}

TEST(ComponentLifecycleCoordinator, ProductExchangeWaitsForAcceptedWorkerAfterAdmissionPinEnds) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  std::promise<void> entered;
  std::promise<void> release;
  const auto released = release.get_future().share();
  auto port = std::make_shared<FakeBrandPort>(
      journal,
      std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                              ComponentLifecycleStateDTO::kPrepared},
      DispatchMode::kKnownSuccess, 1U, std::vector<LifecycleFactDTO>{}, [&] {
        entered.set_value();
        released.wait();
      });
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  const auto previous = ActivePublication();
  auto old_context = ProductContextFor(snapshots, catalogs, resolver, previous);
  const std::weak_ptr<const ComponentLifecycleProductContext> old_lifetime = old_context;
  const auto new_context =
      ProductContextFor(snapshots, catalogs, resolver, NextProductPublication(previous));
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, old_context, kernel,
                                            clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto owner = coordinator.request_owner_epoch();
  const auto intent = PrepareIntent(owner);
  const auto accepted = coordinator.SubmitLifecycle(intent);
  ASSERT_TRUE(accepted.ok());
  const bool blocked =
      entered.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  EXPECT_TRUE(blocked);
  if (blocked) {
    const auto pending =
        coordinator.TryExchangeProductContext(old_context, new_context, owner, previous);
    EXPECT_TRUE(pending.ok());
    if (pending.ok()) {
      EXPECT_EQ(pending.value().reason, "product_work_pending");
    }
    const auto replay = coordinator.SubmitLifecycle(intent);
    EXPECT_TRUE(replay.ok());
    if (replay.ok()) {
      EXPECT_TRUE(replay.value().acceptance.has_value());
      if (replay.value().acceptance) {
        EXPECT_EQ(replay.value().acceptance->replay_status, RequestReplayStatusDTO::kReplayedExact);
        auto original_acknowledgement = *replay.value().acceptance;
        original_acknowledgement.replay_status = accepted.value().acceptance->replay_status;
        EXPECT_EQ(original_acknowledgement, *accepted.value().acceptance);
      }
    }
    EXPECT_FALSE(coordinator.AcquireProductPin().ok());
    EXPECT_FALSE(old_lifetime.expired());
  }
  release.set_value();
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  snapshots.SetSnapshot(ExchangeSnapshot(new_context->identity->publication, 4U,
                                         ComponentLifecycleStateDTO::kPrepared));
  const auto complete =
      coordinator.TryExchangeProductContext(old_context, new_context, owner, previous);
  ASSERT_TRUE(complete.ok()) << complete.error().message;
  EXPECT_EQ(complete.value().phase, ComponentLifecycleProductExchangePhase::kCompleted);
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_EQ(coordinator.request_owner_epoch(), owner);
  old_context.reset();
  EXPECT_TRUE(old_lifetime.expired());
}

TEST(ComponentLifecycleCoordinator, ProductExchangeDoesNotCancelActiveObservationPoll) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  WaitableManualScheduler scheduler;
  const auto previous = ActivePublication();
  const auto old_context = ProductContextFor(snapshots, catalogs, resolver, previous);
  const auto new_context =
      ProductContextFor(snapshots, catalogs, resolver, NextProductPublication(previous));
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, old_context, kernel,
                                            clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto owner = coordinator.request_owner_epoch();
  const auto accepted = coordinator.SubmitLifecycle(PrepareIntent(owner));
  ASSERT_TRUE(accepted.ok());
  ASSERT_TRUE(scheduler.WaitForScheduled(2U, std::chrono::seconds(2)));
  const auto pending =
      coordinator.TryExchangeProductContext(old_context, new_context, owner, previous);
  ASSERT_TRUE(pending.ok());
  EXPECT_EQ(pending.value().phase, ComponentLifecycleProductExchangePhase::kPending);
  EXPECT_EQ(scheduler.cancel_count(), 0U);
  EXPECT_EQ(scheduler.pending_count(SchedulerPriority::Lifecycle), 2U);
  EXPECT_EQ(clock.now(), 100);
}

TEST(ComponentLifecycleCoordinator, ProductExchangeRefusesDurableResidualWithoutReplacingContext) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  ManualScheduler scheduler;
  const auto previous = ActivePublication();
  const auto old_context = ProductContextFor(snapshots, catalogs, resolver, previous);
  const auto new_context =
      ProductContextFor(snapshots, catalogs, resolver, NextProductPublication(previous));
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, old_context, kernel,
                                            clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto owner = coordinator.request_owner_epoch();
  const auto intent = PrepareIntent(owner);
  const auto prepared = PrepareComponentLifecycleAdmission(
      intent, RecoveryAdmissionCandidate("lifecycle.1", Snapshot(), {PrepareCatalog()}));
  ASSERT_TRUE(prepared.ok());
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, intent, prepared.value().acceptance).ok());
  const auto residual_accepted = journal.AcceptOrReplayLifecycle(
      owner, intent, prepared.value().acceptance, prepared.value().initial_status);
  ASSERT_TRUE(residual_accepted.ok())
      << residual_accepted.error().code << ": " << residual_accepted.error().message;
  const auto refused =
      coordinator.TryExchangeProductContext(old_context, new_context, owner, previous);
  ASSERT_FALSE(refused.ok());
  EXPECT_TRUE(coordinator.admission_fault_closed());
  const auto current = coordinator.AcquireProductPin();
  ASSERT_TRUE(current.ok());
  EXPECT_EQ(&current.value().context(), old_context.get());
  const auto retained = coordinator.ReadLifecycleRequestResult(intent);
  ASSERT_TRUE(retained.ok());
  ASSERT_TRUE(retained.value());
  ASSERT_TRUE(retained.value()->acceptance.has_value());
  EXPECT_EQ(retained.value()->acceptance->replay_status, RequestReplayStatusDTO::kReplayedExact);
  auto original_acknowledgement = *retained.value()->acceptance;
  original_acknowledgement.replay_status = prepared.value().acceptance.replay_status;
  EXPECT_EQ(original_acknowledgement, prepared.value().acceptance);
  ASSERT_TRUE(retained.value()->status.has_value());
  EXPECT_EQ(retained.value()->status->acceptance, prepared.value().acceptance);
  EXPECT_EQ(port->dispatch_count(), 0U);
}
TEST(ComponentLifecycleCoordinator, ProductExchangeWaitsForPostRemovalOwnerReconciliationTail) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("original.seed"), 1, 8U);
  ASSERT_TRUE(begun.ok());
  const auto owner = begun.value().request_owner_epoch;
  const auto intent = PrepareIntent(owner);
  const auto prepared = PrepareComponentLifecycleAdmission(
      intent, RecoveryAdmissionCandidate("lifecycle.1", Snapshot(), {PrepareCatalog()}));
  ASSERT_TRUE(prepared.ok());
  const auto claim = SeedLifecycleClaim(journal, owner, intent, prepared.value().acceptance);
  ASSERT_TRUE(claim.ok());
  ASSERT_TRUE(journal
                  .AcceptOrReplayLifecycle(owner, intent, prepared.value().acceptance,
                                           prepared.value().initial_status)
                  .ok());
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  BlockingOwnerReconciliationPort kernel;
  kernel.RetainClaim(claim.value());
  FakeClock clock;
  clock.set(500);
  ManualScheduler scheduler;
  const auto previous = ActivePublication();
  const auto old_context = ProductContextFor(snapshots, catalogs, resolver, previous);
  const auto new_context =
      ProductContextFor(snapshots, catalogs, resolver, NextProductPublication(previous));
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, old_context, kernel,
                                            clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const bool blocked = kernel.WaitForBlockedProbe(std::chrono::seconds(2));
  EXPECT_TRUE(blocked);
  if (blocked) {
    // This probe is reached by Complete only after reservation release and
    // active-row removal. The genuine worker still owns the final fault path.
    EXPECT_TRUE(reservations.snapshot().empty());
    const auto terminal =
        journal.LoadRetainedLifecycleStatus(prepared.value().acceptance.lifecycle_transaction_id);
    EXPECT_TRUE(terminal.ok());
    if (terminal.ok() && terminal.value()) {
      EXPECT_EQ(terminal.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kFailed);
    } else {
      ADD_FAILURE() << "recovered terminal was not retained before final reconciliation";
    }
    const auto attempted = coordinator.TryExchangeProductContext(
        old_context, new_context, coordinator.request_owner_epoch(), previous);
    EXPECT_TRUE(attempted.ok());
    if (attempted.ok()) {
      EXPECT_EQ(attempted.value().phase, ComponentLifecycleProductExchangePhase::kPending);
      EXPECT_EQ(attempted.value().reason, "product_owner_exclusion_busy");
    }
  }
  kernel.ReleaseProbe();
  EXPECT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  EXPECT_FALSE(coordinator
                   .TryExchangeProductContext(old_context, new_context,
                                              coordinator.request_owner_epoch(), previous)
                   .ok());
  const auto current = coordinator.AcquireProductPin();
  ASSERT_TRUE(current.ok());
  EXPECT_EQ(&current.value().context(), old_context.get());
  EXPECT_TRUE(coordinator.admission_fault_closed());
  EXPECT_FALSE(coordinator.accepting_new_requests());
  EXPECT_EQ(port->observation_count(), 0U);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_EQ(clock.now(), 500);
}

TEST(ComponentLifecycleCoordinator, ProductExchangeKeepsOneBoundFollowerPassAcrossSubmission) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  auto verified_snapshot = Snapshot();
  const auto snapshot_digest = ComputeComponentLifecycleCurrentSnapshotDigest(verified_snapshot);
  ASSERT_TRUE(snapshot_digest.ok()) << snapshot_digest.error().message;
  verified_snapshot.snapshot_digest = snapshot_digest.value();
  ASSERT_TRUE(VerifyComponentLifecycleCurrentSnapshot(verified_snapshot).ok());
  FakeSnapshotSource snapshots({verified_snapshot});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  const auto previous = ActivePublication();
  const auto old_context = ProductContextFor(snapshots, catalogs, resolver, previous);
  const auto new_context =
      ProductContextFor(snapshots, catalogs, resolver, NextProductPublication(previous));
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, old_context, kernel,
                                            clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto owner = coordinator.request_owner_epoch();
  CoordinatorLifecycleTargetDriverPorts follower(
      coordinator, kernel, [] { return Result<std::vector<ControllableComponentId>>::Ok({}); });
  std::promise<void> captured;
  std::promise<void> resume;
  const auto resumed = resume.get_future().share();
  auto passed = std::async(std::launch::async, [&] {
    bool accepted = false;
    const auto result = follower.WithPinnedProduct([&](ICCLCLifecycleTargetDriverPorts& bound) {
      const auto observed = bound.ReadCurrentComponent(ControllableComponentId("component.1"));
      EXPECT_TRUE(observed.ok()) << observed.error().code << ": " << observed.error().message;
      captured.set_value();
      resumed.wait();
      const auto submitted = bound.SubmitLifecycle(PrepareIntent(owner));
      EXPECT_TRUE(submitted.ok());
      accepted = submitted.ok() && submitted.value().acceptance.has_value();
    });
    return result.ok() && accepted;
  });
  const bool blocked =
      captured.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  EXPECT_TRUE(blocked);
  if (blocked) {
    const auto pending =
        coordinator.TryExchangeProductContext(old_context, new_context, owner, previous);
    EXPECT_TRUE(pending.ok());
    if (pending.ok()) {
      EXPECT_EQ(pending.value().reason, "product_operations_pending");
    }
    EXPECT_FALSE(coordinator.AcquireProductPin().ok());
  }
  resume.set_value();
  EXPECT_TRUE(passed.get());
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  snapshots.SetSnapshot(ExchangeSnapshot(new_context->identity->publication, 4U,
                                         ComponentLifecycleStateDTO::kPrepared));
  const auto complete =
      coordinator.TryExchangeProductContext(old_context, new_context, owner, previous);
  ASSERT_TRUE(complete.ok()) << complete.error().message;
  EXPECT_EQ(complete.value().phase, ComponentLifecycleProductExchangePhase::kCompleted);
  EXPECT_EQ(port->dispatch_count(), 1U);
}

TEST(ComponentLifecycleCoordinator, ProductExchangeWithdrawsOwnedPendingProductOutsideExclusion) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(PrepareCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared});
  FakeResolver resolver(port);
  FakeCellAdmissionPort kernel;
  FakeClock clock;
  ManualScheduler scheduler;
  const auto previous = ActivePublication();
  const auto old_context = ProductContextFor(snapshots, catalogs, resolver, previous);
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, old_context, kernel,
                                            clock, scheduler);
  ASSERT_TRUE(coordinator.Start().ok());
  const auto owner = coordinator.request_owner_epoch();
  bool destroyed = false;
  bool reentrant_read_ok = false;
  auto pending_context = std::shared_ptr<const ComponentLifecycleProductContext>(
      new ComponentLifecycleProductContext{
          snapshots, catalogs, resolver, std::nullopt, nullptr,
          ComponentLifecycleProductIdentity{NextProductPublication(previous),
                                            {ControllableComponentId("component.1")}}},
      [&](const ComponentLifecycleProductContext* context) {
        reentrant_read_ok = coordinator.AcquireProductPin().ok();
        destroyed = true;
        delete context;
      });
  const auto competing =
      ProductContextFor(snapshots, catalogs, resolver, NextProductPublication(previous));
  auto pass = coordinator.AcquireProductPin();
  ASSERT_TRUE(pass.ok());
  const auto pending =
      coordinator.TryExchangeProductContext(old_context, pending_context, owner, previous);
  ASSERT_TRUE(pending.ok());
  EXPECT_EQ(pending.value().phase, ComponentLifecycleProductExchangePhase::kPending);
  pending_context.reset();
  EXPECT_FALSE(destroyed);
  const auto refused =
      coordinator.TryExchangeProductContext(old_context, competing, owner, previous);
  EXPECT_FALSE(refused.ok());
  EXPECT_TRUE(destroyed);
  EXPECT_TRUE(reentrant_read_ok);
  EXPECT_TRUE(coordinator.AcquireProductPin().ok());
  EXPECT_EQ(&pass.value().context(), old_context.get());
  EXPECT_EQ(port->dispatch_count(), 0U);
}

}  // namespace
}  // namespace manyforge::core
