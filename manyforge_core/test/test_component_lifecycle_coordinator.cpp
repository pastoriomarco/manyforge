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
#include <manyforge_core/component_lifecycle_admission.hpp>
#include <manyforge_core/component_lifecycle_coordinator.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
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
          expected.controller_topology_id,
          expected.topology_revision,
          std::nullopt,
          expected.expected_control_bindings,
          {ResourceId("resource.1")}};
}

[[nodiscard]] ActiveControlConfigurationDTO CurrentActiveConfiguration() {
  const auto expected = ExpectedDomain();
  const auto& binding = expected.expected_control_bindings.front();
  return {
      ActiveControlConfigurationId("configuration.1"),
      expected.control_domain_id,
      expected.control_domain_revision,
      {{binding.control_binding_id, binding.control_binding_revision, binding.control_route_id}},
      ActivationSetId("activation.1"),
      expected.controller_topology_id,
      expected.topology_revision,
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
  expected.controller_topology_id = ControllerTopologyId("topology." + suffix);
  expected.topology_revision = TopologyRevision("topology-revision." + suffix);
  expected.expected_control_bindings.front().control_binding_id =
      ControlBindingId("binding." + suffix);
  expected.expected_control_bindings.front().control_binding_revision =
      ControlBindingRevision("binding-revision." + suffix);
  expected.expected_control_bindings.front().control_route_id = ControlRouteId("route." + suffix);

  ResolvedLifecycleControlDomainScopeDTO current{expected.control_domain_id,
                                                 expected.control_domain_revision,
                                                 expected.controller_topology_id,
                                                 expected.topology_revision,
                                                 std::nullopt,
                                                 expected.expected_control_bindings,
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

  [[nodiscard]] std::size_t load_count() const noexcept {
    return load_count_.load(std::memory_order_acquire);
  }

 private:
  BrandLifecyclePlanCatalogDTO catalog_;
  CatalogAvailability availability_;
  mutable std::atomic<std::size_t> load_count_{0U};
};

enum class DispatchMode { kKnownSuccess, kUncertain };

class FakeBrandPort final : public IBrandLifecyclePort {
 public:
  FakeBrandPort(SqliteCCLCJournal& journal, std::vector<ComponentLifecycleStateDTO> observations,
                DispatchMode mode = DispatchMode::kKnownSuccess,
                std::uint64_t first_observation_generation = 1U,
                std::vector<LifecycleFactDTO> facts = {},
                std::function<void()> before_dispatch_result = {})
      : journal_(journal),
        observations_(std::move(observations)),
        mode_(mode),
        first_observation_generation_(first_observation_generation),
        facts_(std::move(facts)),
        before_dispatch_result_(std::move(before_dispatch_result)) {}

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
         observation_generation, 50, 90, 2'000'000, observations_[index], facts_});
  }

  [[nodiscard]] BrandLifecyclePrimitiveResultDTO Dispatch(
      const BrandLifecyclePrimitiveIntentDTO& intent, std::stop_token) override {
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
    if (mode_ == DispatchMode::kUncertain) {
      return {intent.lifecycle_transaction_id,
              intent.lifecycle_transaction_generation,
              intent.controllable_component_id,
              intent.control_domain_id,
              intent.control_domain_revision,
              intent.provider_instance_id,
              intent.provider_manifest_id,
              intent.provider_manifest_revision,
              intent.provider_runtime_unit_id,
              intent.provider_runtime_epoch,
              intent.lifecycle_primitive_id,
              intent.lifecycle_primitive_attempt_id,
              intent.expected_observation_generation,
              LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain,
              LifecyclePrimitiveEffectDTO::kPossible,
              std::nullopt,
              ErrorDTO{"response_lost", "dispatch response lost", {}}};
    }
    return {intent.lifecycle_transaction_id,
            intent.lifecycle_transaction_generation,
            intent.controllable_component_id,
            intent.control_domain_id,
            intent.control_domain_revision,
            intent.provider_instance_id,
            intent.provider_manifest_id,
            intent.provider_manifest_revision,
            intent.provider_runtime_unit_id,
            intent.provider_runtime_epoch,
            intent.lifecycle_primitive_id,
            intent.lifecycle_primitive_attempt_id,
            intent.expected_observation_generation,
            LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown,
            LifecyclePrimitiveEffectDTO::kObserved,
            LifecyclePrimitiveKnownResultDTO::kSucceeded,
            std::nullopt};
  }

  [[nodiscard]] std::size_t dispatch_count() const noexcept {
    return dispatch_count_.load(std::memory_order_acquire);
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
  mutable std::mutex mutex_;
  std::size_t observe_count_ = 0U;
  std::atomic<std::size_t> dispatch_count_{0U};
  std::atomic<bool> saw_prepared_status_{false};
  std::optional<LifecyclePrimitiveId> last_dispatched_primitive_id_;
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

class UnusedKernelPort final : public ICCLCKernelDisableHandoffPort,
                               public FakeCellAdmissionPort {
 public:
  [[nodiscard]] Result<DisableHandoffAdmissionSnapshotDTO> ObserveDisableHandoffAdmission(
      const DisableHandoffAdmissionQueryDTO&) const override {
    return Result<DisableHandoffAdmissionSnapshotDTO>::Err(
        ErrorDTO{"unexpected_kernel_call", "not a Disable test", {}});
  }

  [[nodiscard]] Result<DisableHandoffAcceptedDTO> BeginDisableHandoff(
      const DisableHandoffRequestDTO&) override {
    return Result<DisableHandoffAcceptedDTO>::Err(
        ErrorDTO{"unexpected_kernel_call", "not a Disable test", {}});
  }

  [[nodiscard]] Result<DisableHandoffStatusDTO> DisableHandoffStatus(
      const LifecycleTransactionId&, LifecycleTransactionGeneration) override {
    return Result<DisableHandoffStatusDTO>::Err(
        ErrorDTO{"unexpected_kernel_call", "not a Disable test", {}});
  }

  [[nodiscard]] Result<DisableHandoffAttemptReconciliationDTO> ReconcileDisableHandoffAttempt(
      const DisableHandoffRequestDTO&) override {
    return Result<DisableHandoffAttemptReconciliationDTO>::Err(
        ErrorDTO{"unexpected_kernel_call", "not a Disable test", {}});
  }

  [[nodiscard]] Result<DisableFenceReleasedDTO> ReleaseDisableFence(
      const DisableFenceReleaseRequestDTO&) override {
    return Result<DisableFenceReleasedDTO>::Err(
        ErrorDTO{"unexpected_kernel_call", "not a Disable test", {}});
  }

  [[nodiscard]] Result<DisableFenceReleaseAttemptReconciliationDTO>
  ReconcileDisableFenceReleaseAttempt(const DisableFenceReleaseRequestDTO&) override {
    return Result<DisableFenceReleaseAttemptReconciliationDTO>::Err(
        ErrorDTO{"unexpected_kernel_call", "not a Disable test", {}});
  }
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

  void cancel(ScheduleHandle handle) override { scheduler_.cancel(handle); }

  bool drain_for(SchedulerPriority priority, std::chrono::nanoseconds timeout) override {
    return !fail_drain_ && scheduler_.drain_for(priority, timeout);
  }

  [[nodiscard]] bool WaitForScheduled(std::size_t count, std::chrono::nanoseconds timeout) const {
    std::unique_lock lock(mutex_);
    return scheduled_.wait_for(lock, timeout, [&] { return scheduled_count_ >= count; });
  }

  void AdvanceTo(TimePoint time) { scheduler_.advance_to(time); }

  [[nodiscard]] SchedulerPriority last_priority() const { return scheduler_.last_priority(); }

 private:
  ManualScheduler scheduler_;
  mutable std::mutex mutex_;
  mutable std::condition_variable scheduled_;
  std::size_t scheduled_count_ = 0U;
  bool fail_drain_ = false;
};

class FakeKernelDisablePort final : public ICCLCKernelDisableHandoffPort,
                                    public FakeCellAdmissionPort {
 public:
  explicit FakeKernelDisablePort(SqliteCCLCJournal& journal) : journal_(journal) {}

  [[nodiscard]] Result<DisableHandoffAdmissionSnapshotDTO> ObserveDisableHandoffAdmission(
      const DisableHandoffAdmissionQueryDTO& query) const override {
    if (!query.locally_valid()) {
      return Result<DisableHandoffAdmissionSnapshotDTO>::Err(
          ErrorDTO{"disable_admission_query_invalid", "unexpected Disable admission query", {}});
    }
    const bool full_stop =
        query.request_kind == DisableHandoffScopeRequestKindDTO::kFullStopRequired;
    if (!full_stop && !query.component_expectation.has_value()) {
      return Result<DisableHandoffAdmissionSnapshotDTO>::Err(
          ErrorDTO{"disable_admission_query_invalid", "resolved query has no expectation", {}});
    }
    const auto expectation = full_stop
                                 ? BuildDisableHandoffComponentScopeExpectation(Snapshot()).value()
                                 : *query.component_expectation;
    const auto fence_generation = handoff_begun_.load(std::memory_order_acquire)
                                      ? ConfigurationFenceGeneration(12U)
                                      : ConfigurationFenceGeneration(11U);
    DisableHandoffAdmissionSnapshotDTO snapshot{
        query,
        RequestOwnerEpoch("kernel-owner.1"),
        full_stop ? DisableHandoffScopeResolutionDTO::kPromotedToFull
                  : DisableHandoffScopeResolutionDTO::kExact,
        expectation.component_scope,
        expectation.active_publication_generation,
        expectation.active_public_pointer_digest,
        expectation.retained_bundle_id,
        expectation.graph_revision,
        9U,
        fence_generation,
        expectation.current_active_control_configurations,
        ContentDigest(handoff_begun_.load(std::memory_order_acquire) ? "disable-release-admission.1"
                                                                     : "disable-admission.1")};
    if (!snapshot.locally_valid()) {
      return Result<DisableHandoffAdmissionSnapshotDTO>::Err(
          ErrorDTO{"disable_admission_snapshot_invalid", "invalid fake admission snapshot", {}});
    }
    ++admission_observation_count_;
    return Result<DisableHandoffAdmissionSnapshotDTO>::Ok(std::move(snapshot));
  }

  [[nodiscard]] Result<DisableHandoffAcceptedDTO> BeginDisableHandoff(
      const DisableHandoffRequestDTO& request) override {
    const auto retained = journal_.LoadRetainedLifecycleStatus(request.lifecycle_transaction_id);
    saw_handoff_prepared_.store(
        retained.ok() && retained.value().has_value() &&
            retained.value()->phase == ComponentLifecycleTransactionPhaseDTO::kAcquiringDomains &&
            !retained.value()->disable_handoff.has_value() &&
            !retained.value()->configuration_fence.has_value(),
        std::memory_order_release);
    begin_count_.fetch_add(1U, std::memory_order_acq_rel);
    handoff_begun_.store(true, std::memory_order_release);
    DisableHandoffAcceptedDTO acceptance{request.owner_request_context.request_id,
                                         RequestReplayStatusDTO::kAcceptedNew,
                                         request.lifecycle_transaction_id,
                                         request.lifecycle_transaction_generation,
                                         request.controllable_component_id,
                                         request.scope_resolution,
                                         ContentDigest("scope-digest.1"),
                                         ConfigurationFenceId("configuration-fence.1"),
                                         ConfigurationFenceGeneration(12U),
                                         std::nullopt,
                                         std::nullopt};
    if (!acceptance.matches(request)) {
      return Result<DisableHandoffAcceptedDTO>::Err(
          ErrorDTO{"disable_acceptance_invalid", "invalid fake Disable acceptance", {}});
    }
    {
      std::lock_guard lock(mutex_);
      handoff_request_ = request;
      handoff_acceptance_ = acceptance;
    }
    return Result<DisableHandoffAcceptedDTO>::Ok(std::move(acceptance));
  }

  [[nodiscard]] Result<DisableHandoffStatusDTO> DisableHandoffStatus(
      const LifecycleTransactionId& lifecycle_transaction_id,
      LifecycleTransactionGeneration lifecycle_transaction_generation) override {
    std::lock_guard lock(mutex_);
    if (!handoff_request_.has_value() || !handoff_acceptance_.has_value() ||
        handoff_request_->lifecycle_transaction_id != lifecycle_transaction_id ||
        handoff_request_->lifecycle_transaction_generation != lifecycle_transaction_generation) {
      return Result<DisableHandoffStatusDTO>::Err(
          ErrorDTO{"disable_handoff_unknown", "unknown fake Disable handoff", {}});
    }
    ++status_count_;
    const auto& request = *handoff_request_;
    const auto& acceptance = *handoff_acceptance_;
    DisableHandoffStatusDTO status{
        request.lifecycle_transaction_id,
        request.lifecycle_transaction_generation,
        request.controllable_component_id,
        acceptance.scope_resolution,
        acceptance.component_scope_digest,
        acceptance.configuration_fence_id,
        acceptance.configuration_fence_generation,
        DisableHandoffStateDTO::kReady,
        request.affected_scope,
        {acceptance.configuration_fence_id, acceptance.configuration_fence_generation,
         request.lifecycle_transaction_id, request.lifecycle_transaction_generation,
         request.controllable_component_id, request.affected_scope, "disable containment",
         ConfigurationFenceState::kHeld},
        std::nullopt,
        {},
        std::nullopt};
    if (!status.locally_valid()) {
      return Result<DisableHandoffStatusDTO>::Err(
          ErrorDTO{"disable_handoff_status_invalid", "invalid fake Disable status", {}});
    }
    return Result<DisableHandoffStatusDTO>::Ok(std::move(status));
  }

  [[nodiscard]] Result<DisableHandoffAttemptReconciliationDTO> ReconcileDisableHandoffAttempt(
      const DisableHandoffRequestDTO&) override {
    return Result<DisableHandoffAttemptReconciliationDTO>::Err(
        ErrorDTO{"unexpected_kernel_call", "direct-ready handoff does not reconcile", {}});
  }

  [[nodiscard]] Result<DisableFenceReleasedDTO> ReleaseDisableFence(
      const DisableFenceReleaseRequestDTO& request) override {
    const auto retained = journal_.LoadRetainedLifecycleStatus(request.lifecycle_transaction_id);
    saw_release_prepared_.store(
        retained.ok() && retained.value().has_value() &&
            retained.value()->phase == ComponentLifecycleTransactionPhaseDTO::kVerifying &&
            retained.value()->configuration_fence.has_value() &&
            retained.value()->configuration_fence->state == ConfigurationFenceState::kHeld &&
            !retained.value()->disable_fence_released.has_value() &&
            retained.value()->status_sequence == request.release_precondition_status_sequence + 1U,
        std::memory_order_release);
    release_count_.fetch_add(1U, std::memory_order_acq_rel);
    DisableFenceReleasedDTO receipt{request,
                                    RequestReplayStatusDTO::kAcceptedNew,
                                    700,
                                    request.ready_handoff.configuration_fence_id,
                                    request.ready_handoff.configuration_fence_generation,
                                    request.ready_handoff.component_scope_digest,
                                    ContentDigest("disable-release-receipt.1")};
    if (!receipt.matches(request)) {
      return Result<DisableFenceReleasedDTO>::Err(
          ErrorDTO{"disable_release_receipt_invalid", "invalid fake release receipt", {}});
    }
    if (retained.ok() && retained.value().has_value()) {
      auto candidate = *retained.value();
      ++candidate.status_sequence;
      candidate.configuration_fence->state = ConfigurationFenceState::kReleased;
      candidate.disable_fence_released = receipt;
      candidate.reconciliation_required = false;
      candidate.remaining_duration_ns = 0;
      const bool all_succeeded =
          std::all_of(candidate.domains.begin(), candidate.domains.end(), [](const auto& domain) {
            return domain.action_state == ComponentLifecycleDomainActionStateDTO::kSucceeded;
          });
      if (all_succeeded) {
        candidate.phase = ComponentLifecycleTransactionPhaseDTO::kSucceeded;
        candidate.current_state = candidate.requested_state.state;
        candidate.terminal_result = ComponentLifecycleTerminalResultDTO::kSucceeded;
        candidate.error.reset();
      } else {
        candidate.phase = ComponentLifecycleTransactionPhaseDTO::kFailed;
        bool any_observed = false;
        for (auto& domain : candidate.domains) {
          any_observed = any_observed ||
                         domain.effect == ComponentLifecycleDomainEffectDTO::kEffectObserved ||
                         domain.effect == ComponentLifecycleDomainEffectDTO::kPartialEffectObserved;
          if (domain.action_state != ComponentLifecycleDomainActionStateDTO::kSucceeded) {
            domain.action_state = ComponentLifecycleDomainActionStateDTO::kFailed;
            if (domain.effect == ComponentLifecycleDomainEffectDTO::kNone) {
              domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
            }
            domain.error =
                ErrorDTO{"component_lifecycle_disable_mutation_failed",
                         "Disable containment did not satisfy the lifecycle postcondition",
                         {}};
          }
        }
        candidate.terminal_result = any_observed
                                        ? ComponentLifecycleTerminalResultDTO::kFailedPartialEffect
                                        : ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect;
        candidate.error =
            ErrorDTO{"component_lifecycle_disable_mutation_failed",
                     "Disable containment did not satisfy every lifecycle postcondition",
                     {}};
      }
      terminal_candidate_locally_valid_.store(candidate.locally_valid(), std::memory_order_release);
    }
    return Result<DisableFenceReleasedDTO>::Ok(std::move(receipt));
  }

  [[nodiscard]] Result<DisableFenceReleaseAttemptReconciliationDTO>
  ReconcileDisableFenceReleaseAttempt(const DisableFenceReleaseRequestDTO&) override {
    return Result<DisableFenceReleaseAttemptReconciliationDTO>::Err(
        ErrorDTO{"unexpected_kernel_call", "direct-ready release does not reconcile", {}});
  }

  [[nodiscard]] std::size_t begin_count() const noexcept {
    return begin_count_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t release_count() const noexcept {
    return release_count_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t status_count() const {
    std::lock_guard lock(mutex_);
    return status_count_;
  }

  [[nodiscard]] std::size_t admission_observation_count() const noexcept {
    return admission_observation_count_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool saw_handoff_prepared() const noexcept {
    return saw_handoff_prepared_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool saw_release_prepared() const noexcept {
    return saw_release_prepared_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool terminal_candidate_locally_valid() const noexcept {
    return terminal_candidate_locally_valid_.load(std::memory_order_acquire);
  }

 private:
  SqliteCCLCJournal& journal_;
  mutable std::mutex mutex_;
  mutable std::atomic<std::size_t> admission_observation_count_{0U};
  std::atomic<std::size_t> begin_count_{0U};
  std::atomic<std::size_t> release_count_{0U};
  std::atomic<bool> handoff_begun_{false};
  std::atomic<bool> saw_handoff_prepared_{false};
  std::atomic<bool> saw_release_prepared_{false};
  std::atomic<bool> terminal_candidate_locally_valid_{false};
  std::optional<DisableHandoffRequestDTO> handoff_request_;
  std::optional<DisableHandoffAcceptedDTO> handoff_acceptance_;
  std::size_t status_count_ = 0U;
};

[[nodiscard]] ComponentLifecycleCoordinatorConfig Config() {
  return {RequestOwnerEpoch("cclc-owner.1"), 8U, 2U, 2U, 1'000, 10'000};
}

[[nodiscard]] ComponentLifecycleAdmissionCandidate RecoveryAdmissionCandidate(
    std::string transaction_id, std::optional<ComponentLifecycleCurrentSnapshotDTO> snapshot,
    std::vector<BrandLifecyclePlanCatalogDTO> catalogs,
    std::optional<DisableHandoffAdmissionSnapshotDTO> disable_admission = std::nullopt) {
  return {LifecycleTransactionId(std::move(transaction_id)),
          LifecycleTransactionGeneration(1U),
          ComponentLifecycleGeneration(3U),
          100,
          Config().containment_only_disable_timeout_duration_ns,
          std::move(snapshot),
          std::move(catalogs),
          std::move(disable_admission)};
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
          LifecyclePrimitiveId("prepare"),
          LifecyclePrimitiveAttemptId("recovery-attempt.1"),
          1U,
          status.remaining_duration_ns};
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
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
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

[[nodiscard]] Result<DisableHandoffAdmissionSnapshotDTO> RecoveryDisableAdmission(
    const ComponentLifecycleCurrentSnapshotDTO& snapshot) {
  const auto expectation = BuildDisableHandoffComponentScopeExpectation(snapshot);
  if (!expectation.ok()) {
    return Result<DisableHandoffAdmissionSnapshotDTO>::Err(expectation.error());
  }
  DisableHandoffAdmissionSnapshotDTO admission{
      {snapshot.controllable_component_id,
       DisableHandoffScopeRequestKindDTO::kResolvedComponentScope, expectation.value()},
      RequestOwnerEpoch("kernel-owner.1"),
      DisableHandoffScopeResolutionDTO::kExact,
      expectation.value().component_scope,
      expectation.value().active_publication_generation,
      expectation.value().active_public_pointer_digest,
      expectation.value().retained_bundle_id,
      expectation.value().graph_revision,
      9U,
      ConfigurationFenceGeneration(11U),
      expectation.value().current_active_control_configurations,
      ContentDigest("disable-admission.1")};
  if (!admission.locally_valid()) {
    return Result<DisableHandoffAdmissionSnapshotDTO>::Err(
        ErrorDTO{"disable_admission_invalid", "invalid recovery test admission", {}});
  }
  return Result<DisableHandoffAdmissionSnapshotDTO>::Ok(std::move(admission));
}

[[nodiscard]] DisableHandoffRequestDTO RecoveryDisableRequest(
    const ComponentLifecycleStatusDTO& status,
    const DisableHandoffAdmissionSnapshotDTO& admission) {
  return {{RequestId("recovery-disable-handoff.1"), admission.request_owner_epoch,
           "CCLC Disable handoff", status.accepted_intent.request_context.request_id.value()},
          status.accepted_intent.request_context.request_id,
          status.acceptance.lifecycle_transaction_id,
          status.acceptance.lifecycle_transaction_generation,
          status.accepted_intent.controllable_component_id,
          admission.scope_resolution,
          admission.current_scope,
          admission.snapshot_digest,
          admission.active_publication_generation,
          admission.active_public_pointer_digest,
          admission.graph_revision,
          admission.retained_bundle_id,
          admission.query.component_expectation->component_lifecycle_generation,
          admission.operation_resource_generation,
          admission.configuration_fence_generation,
          admission.current_active_control_configurations};
}

[[nodiscard]] CCLCEffectAttemptDTO PreparedDisableAttempt(const DisableHandoffRequestDTO& request) {
  return {CCLCRequestKindDTO::kComponentLifecycle,
          request.lifecycle_transaction_id,
          request.lifecycle_transaction_generation,
          1U,
          CCLCEffectKindDTO::kKernelDisableHandoff,
          CCLCEffectAttemptStateDTO::kPrepared,
          std::nullopt,
          std::nullopt,
          request,
          std::nullopt,
          std::nullopt,
          std::nullopt};
}

[[nodiscard]] CCLCEffectAttemptDTO CompletedDisableAttempt(
    const DisableHandoffRequestDTO& request, const DisableHandoffAcceptedDTO& acceptance) {
  auto attempt = PreparedDisableAttempt(request);
  attempt.state = CCLCEffectAttemptStateDTO::kCompleted;
  attempt.disable_handoff_result = DisableHandoffEffectResultDTO{
      request, DisableHandoffEffectDispositionDTO::kAccepted, acceptance};
  return attempt;
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
  UnusedKernelPort kernel;
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

TEST(ComponentLifecycleCoordinator,
     MultiDomainPrepareFailureRetainsTheSuccessfulDomainAsPartialEffect) {
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
  second.controller_topology_id = ControllerTopologyId("topology.2");
  second.topology_revision = TopologyRevision("topology-revision.2");
  second.current_control_bindings.front().control_binding_id = ControlBindingId("binding.2");
  second.current_control_bindings.front().control_binding_revision =
      ControlBindingRevision("binding-revision.2");
  second.current_control_bindings.front().control_route_id = ControlRouteId("route.2");
  second.current_resources = {ResourceId("resource.2")};
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
  expected_second.controller_topology_id = second.controller_topology_id;
  expected_second.topology_revision = second.topology_revision;
  expected_second.expected_control_bindings = second.current_control_bindings;
  intent.expected_control_domains.push_back(expected_second);

  FakeSnapshotSource snapshots({snapshot});
  FakeCatalogSource catalogs(catalog);
  auto first_port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  auto second_port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared},
      DispatchMode::kUncertain);
  DomainPortResolver resolver(snapshot.provider_occurrences.front(), first_port,
                              second_provider, second_port);
  UnusedKernelPort kernel;
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
  EXPECT_EQ(status.value()->terminal_result,
            ComponentLifecycleTerminalResultDTO::kFailedPartialEffect);
  ASSERT_EQ(status.value()->domains.size(), 2U);
  EXPECT_EQ(first_port->dispatch_count(), 1U);
  EXPECT_EQ(second_port->dispatch_count(), 1U);
  EXPECT_TRUE(reservations.snapshot().empty());
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

  [[nodiscard]] Result<ComponentCommandReadinessDTO> ReadComponentCommandReadiness(
      const ControllableComponentId&) const override {
    std::lock_guard lock(mutex_);
    return Result<ComponentCommandReadinessDTO>::Ok(state_);
  }

  void FailNextPublications(std::size_t count) {
    std::lock_guard lock(mutex_);
    failures_remaining_ = count;
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
  FakeResolver resolver(port, catalog);
  UnusedKernelPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  FakeReadinessPort readiness;
  readiness.FailNextPublications(failing_publications);
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler, nullptr,
                                            std::nullopt, &readiness);
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

TEST(ComponentLifecycleCoordinator, UnverifiedReadinessPublicationCannotReportSuccess) {
  const auto run = RunEnable(1U);
  ASSERT_TRUE(run.status.has_value());
  // The adapter succeeded but the Kernel result was not verified: the known
  // failure after effects settles command-closed, never Active.
  EXPECT_EQ(run.status->terminal_result, ComponentLifecycleTerminalResultDTO::kFailedPartialEffect);
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
  second.controller_topology_id = ControllerTopologyId("topology.2");
  second.topology_revision = TopologyRevision("topology-revision.2");
  second.current_control_bindings.front().control_binding_id = ControlBindingId("binding.2");
  second.current_control_bindings.front().control_binding_revision =
      ControlBindingRevision("binding-revision.2");
  second.current_control_bindings.front().control_route_id = ControlRouteId("route.2");
  second.current_resources = {ResourceId("resource.2")};
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
  expected_second.controller_topology_id = second.controller_topology_id;
  expected_second.topology_revision = second.topology_revision;
  expected_second.expected_control_bindings = second.current_control_bindings;
  intent.expected_control_domains.push_back(expected_second);

  FakeSnapshotSource snapshots({snapshot});
  FakeCatalogSource catalogs(catalog);
  auto first_port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                       ComponentLifecycleStateDTO::kPrepared});
  auto second_port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared},
      DispatchMode::kUncertain);
  DomainPortResolver resolver(snapshot.provider_occurrences.front(), first_port,
                              second_provider, second_port);
  UnusedKernelPort kernel;
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
  FakeReadinessPort readiness;
  ComponentLifecycleCoordinator coordinator(Config(), journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler, nullptr,
                                            std::nullopt, &readiness);
  ASSERT_TRUE(coordinator.Start().ok());
  intent.request_context.request_owner_epoch = coordinator.request_owner_epoch();
  const auto accepted = coordinator.SubmitLifecycle(intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().acceptance.has_value());
  ASSERT_TRUE(coordinator.DrainFor(std::chrono::seconds(2)));
  const auto status =
      coordinator.LoadLifecycleStatus(accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(status.ok() && status.value().has_value());
  EXPECT_EQ(status.value()->terminal_result,
            ComponentLifecycleTerminalResultDTO::kFailedPartialEffect);
  // The first domain succeeded, yet no domain of the component was ever
  // opened, and the settled result is verified closed before release.
  const auto calls = readiness.calls();
  EXPECT_EQ(std::count(calls.begin(), calls.end(), "publish_open"), 0);
  ASSERT_FALSE(calls.empty());
  EXPECT_EQ(calls.front(), "close");
  EXPECT_EQ(calls.back(), "publish_closed");
  EXPECT_FALSE(readiness.state().command_eligible);
  // The partial effect leaves no durable current state for restart to
  // reconstruct as readiness.
  const auto fact =
      journal.LoadCurrentComponentLifecycleFact(intent.controllable_component_id);
  ASSERT_TRUE(fact.ok());
  EXPECT_FALSE(fact.value().current_state.has_value());

  // A restarted owner reading the same durable journal finds a settled
  // transaction and no current state to reconstruct as command readiness.
  SqliteCCLCJournal reopened(temporary.path() / std::string(kCCLCJournalFilename),
                             CellId("cell.1"), 8U);
  const auto reopened_fact =
      reopened.LoadCurrentComponentLifecycleFact(intent.controllable_component_id);
  ASSERT_TRUE(reopened_fact.ok()) << reopened_fact.error().message;
  EXPECT_TRUE(reopened_fact.value().durable_generation.has_value());
  EXPECT_FALSE(reopened_fact.value().current_state.has_value());
  const auto unsettled = reopened.LoadComponentsWithUnsettledLifecycle();
  ASSERT_TRUE(unsettled.ok()) << unsettled.error().message;
  EXPECT_TRUE(unsettled.value().empty());
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
  UnusedKernelPort kernel;
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
  const auto unknown = coordinator.SubmitLifecycle(intent);
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().code, "owner_unavailable");
  EXPECT_FALSE(coordinator.accepting_new_requests());
  EXPECT_EQ(port->dispatch_count(), 1U);
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
  UnusedKernelPort kernel;
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
  UnusedKernelPort kernel;
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
  UnusedKernelPort kernel;
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
  EXPECT_EQ(status.value()->terminal_result,
            ComponentLifecycleTerminalResultDTO::kFailedPartialEffect);
  ASSERT_TRUE(status.value()->error.has_value());
  EXPECT_EQ(status.value()->error->code, "component_lifecycle_verification_timeout");
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_TRUE(reservations.snapshot().empty());
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
  UnusedKernelPort kernel;
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
  UnusedKernelPort kernel;
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
  UnusedKernelPort kernel;
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
    UnusedKernelPort kernel;
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
  UnusedKernelPort kernel;
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
  UnusedKernelPort kernel;
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

TEST(ComponentLifecycleCoordinator,
     UncertainDispatchReconcilesFromFreshObservationWithoutRedispatch) {
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
      DispatchMode::kUncertain);
  FakeResolver resolver(port);
  UnusedKernelPort kernel;
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
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kSucceeded);
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_TRUE(status.value()->domains.front().reconciliation_evidence.has_value());
}

TEST(ComponentLifecycleCoordinator,
     OrdinaryDisableUsesIdleClaimAndNeverCallsTheKernelStopHandoff) {
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
  UnusedKernelPort kernel;
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
  EXPECT_FALSE(status.value()->disable_handoff.has_value());
  EXPECT_FALSE(status.value()->configuration_fence.has_value());
  EXPECT_FALSE(status.value()->disable_fence_released.has_value());
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_EQ(port->last_dispatched_primitive_id(), LifecyclePrimitiveId("disable"));
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
  under_fence.domains.front().active_control_configuration = CurrentActiveConfiguration();
  ASSERT_TRUE(under_fence.locally_valid());
  FakeSnapshotSource snapshots({before, before, under_fence, under_fence});
  FakeCatalogSource catalogs(DisableCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kActive});
  FakeResolver resolver(port);
  UnusedKernelPort kernel;
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
  EXPECT_EQ(status.value()->terminal_result,
            ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect);
  ASSERT_TRUE(status.value()->error.has_value());
  EXPECT_EQ(status.value()->error->code, "component_lifecycle_snapshot_changed_under_claim");
  EXPECT_EQ(port->observation_count(), 0U);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_FALSE(status.value()->disable_handoff.has_value());
  EXPECT_FALSE(status.value()->configuration_fence.has_value());
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
  FakeKernelDisablePort kernel(journal);
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
  EXPECT_EQ(kernel.begin_count(), 0U);
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
  FakeKernelDisablePort kernel(journal);
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
  EXPECT_EQ(kernel.begin_count(), 0U);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
}
TEST(ComponentLifecycleCoordinator, ShutdownFailsClosedWhenTheLifecycleSchedulerLaneCannotDrain) {
  EXPECT_DEATH(
      {
        ScopedTemporaryDirectory temporary;
        SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename),
                                  CellId("cell.1"), 8U);
        CCLCDomainReservationTable reservations;
        FakeSnapshotSource snapshots({Snapshot(), Snapshot()});
        FakeCatalogSource catalogs(PrepareCatalog());
        auto port = std::make_shared<FakeBrandPort>(
            journal,
            std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                    ComponentLifecycleStateDTO::kDeclared});
        FakeResolver resolver(port);
        UnusedKernelPort kernel;
        FakeClock clock;
        clock.set(100);
        WaitableManualScheduler scheduler(true);
        auto coordinator = std::make_unique<ComponentLifecycleCoordinator>(
            Config(), journal, reservations, snapshots, catalogs, resolver, kernel, clock,
            scheduler);

        if (!coordinator->Start().ok()) {
          ::_exit(0);
        }
        const auto accepted =
            coordinator->SubmitLifecycle(PrepareIntent(coordinator->request_owner_epoch()));
        if (!accepted.ok() || !scheduler.WaitForScheduled(1U, std::chrono::seconds(2))) {
          ::_exit(0);
        }
        coordinator.reset();
        ::_exit(0);
      },
      ".*");
}

TEST(ComponentLifecycleCoordinator,
     RecoveryWithoutPreparedEffectFailsKnownNoEffectAndReopensAdmission) {
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
  UnusedKernelPort kernel;
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
  EXPECT_EQ(status.value()->terminal_result,
            ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect);
  ASSERT_TRUE(status.value()->error.has_value());
  EXPECT_EQ(status.value()->error->code, "component_lifecycle_owner_replaced");
  EXPECT_EQ(port->observation_count(), 0U);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_TRUE(coordinator.accepting_new_requests());
}

TEST(ComponentLifecycleCoordinator,
     RecoveryOfPreparedBrandAttemptObservesFreshStateWithoutRedispatch) {
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
  UnusedKernelPort kernel;
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
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kSucceeded);
  EXPECT_EQ(status.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
  EXPECT_EQ(catalogs.load_count(), 1U);
  EXPECT_EQ(resolver.resolve_count(), 1U);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_EQ(port->observation_count(), 1U);
  ASSERT_EQ(status.value()->domains.front().primitive_attempts.size(), 1U);
  const auto& synthetic_result = status.value()->domains.front().primitive_attempts.front();
  EXPECT_EQ(synthetic_result.dispatch_state,
            LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain);
  ASSERT_TRUE(synthetic_result.error.has_value());
  const auto reason = synthetic_result.error->details.find("reason");
  ASSERT_NE(reason, synthetic_result.error->details.end());
  EXPECT_EQ(reason->second, "owner_replaced_after_effect_prepare");
  ASSERT_TRUE(status.value()->domains.front().reconciliation_evidence.has_value());
  EXPECT_EQ(status.value()->domains.front().reconciliation_evidence->postcondition_result,
            LifecyclePostconditionResultDTO::kSatisfied);
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_TRUE(coordinator.accepting_new_requests());
}

struct RecoveredBrandClassificationCase final {
  std::string_view name;
  ComponentLifecycleStateDTO observed_state;
  BrandLifecyclePlanCatalogDTO catalog;
  std::vector<LifecycleFactDTO> facts;
  ComponentLifecycleTransactionPhaseDTO expected_phase;
  ComponentLifecycleTerminalResultDTO expected_terminal;
  ComponentLifecycleDomainEffectDTO expected_effect;
};

class RecoveredBrandClassificationTest
    : public ::testing::TestWithParam<RecoveredBrandClassificationCase> {};

TEST_P(RecoveredBrandClassificationTest, ClassifiesFreshEvidenceWithoutRedispatch) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("cclc-owner.1"), 1, 8U);
  ASSERT_TRUE(begun.ok());
  const auto first_owner = begun.value().request_owner_epoch;
  const auto seed =
      SeedPreparedBrandRecovery(journal, first_owner, GetParam().catalog, "lifecycle.1");

  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots({Snapshot()});
  FakeCatalogSource catalogs(GetParam().catalog);
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{GetParam().observed_state},
      DispatchMode::kKnownSuccess, 2U, GetParam().facts);
  FakeResolver resolver(port);
  UnusedKernelPort kernel;
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
  EXPECT_EQ(status.value()->phase, GetParam().expected_phase);
  EXPECT_EQ(status.value()->terminal_result, GetParam().expected_terminal);
  EXPECT_EQ(status.value()->domains.front().effect, GetParam().expected_effect);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_EQ(port->observation_count(), 1U);
  ASSERT_EQ(status.value()->domains.front().primitive_attempts.size(), 1U);
  const auto& synthetic_result = status.value()->domains.front().primitive_attempts.front();
  EXPECT_EQ(synthetic_result.dispatch_state,
            LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain);
  ASSERT_TRUE(synthetic_result.error.has_value());
  const auto reason = synthetic_result.error->details.find("reason");
  ASSERT_NE(reason, synthetic_result.error->details.end());
  EXPECT_EQ(reason->second, "owner_replaced_after_effect_prepare");
  ASSERT_TRUE(status.value()->domains.front().reconciliation_evidence.has_value());
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_TRUE(coordinator.accepting_new_requests());
}

INSTANTIATE_TEST_SUITE_P(
    EvidenceClasses, RecoveredBrandClassificationTest,
    ::testing::Values(
        RecoveredBrandClassificationCase{"same-state",
                                         ComponentLifecycleStateDTO::kDeclared,
                                         PrepareCatalog(),
                                         {},
                                         ComponentLifecycleTransactionPhaseDTO::kFailed,
                                         ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect,
                                         ComponentLifecycleDomainEffectDTO::kKnownNoEffect},
        RecoveredBrandClassificationCase{
            "changed-state",
            ComponentLifecycleStateDTO::kActive,
            PrepareCatalog(),
            {},
            ComponentLifecycleTransactionPhaseDTO::kFailed,
            ComponentLifecycleTerminalResultDTO::kFailedPartialEffect,
            ComponentLifecycleDomainEffectDTO::kPartialEffectObserved}),
    [](const ::testing::TestParamInfo<RecoveredBrandClassificationCase>& info) {
      std::string result(info.param.name);
      std::replace(result.begin(), result.end(), '-', '_');
      return result;
    });

TEST(ComponentLifecycleCoordinator,
     RecoveryFactBearingUnsatisfiedWithoutBaselineRemainsUncertainAndAdmissionClosed) {
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
  FakeResolver resolver(port, catalog);
  UnusedKernelPort kernel;
  kernel.RetainClaim(seed.claim);
  FakeClock clock;
  clock.set(500);
  ManualScheduler scheduler;
  auto config = Config();
  config.request_owner_epoch = RequestOwnerEpoch("cclc-owner.2");
  ComponentLifecycleCoordinator coordinator(config, journal, reservations, snapshots, catalogs,
                                            resolver, kernel, clock, scheduler);

  ASSERT_TRUE(coordinator.Start().ok());
  EXPECT_FALSE(coordinator.DrainFor(std::chrono::milliseconds(50)));
  const auto status = coordinator.LoadLifecycleStatus(seed.acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().has_value());
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kReconciling);
  EXPECT_FALSE(status.value()->terminal_result.has_value());
  EXPECT_TRUE(status.value()->reconciliation_required);
  EXPECT_EQ(status.value()->domains.front().effect,
            ComponentLifecycleDomainEffectDTO::kEffectUncertain);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_EQ(port->observation_count(), 1U);
  EXPECT_FALSE(reservations.snapshot().empty());
  EXPECT_FALSE(coordinator.accepting_new_requests());
}

TEST(ComponentLifecycleCoordinator,
     RecoveryOfReadyHeldDisableClearsOwnerMarkerAndDurablyReleasesFence) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"),
                            8U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("cclc-owner.1"), 1, 8U);
  ASSERT_TRUE(begun.ok());
  const auto first_owner = begun.value().request_owner_epoch;
  const auto snapshot = Snapshot("snapshot.before.1", ComponentLifecycleStateDTO::kActive,
                                 ComponentLifecycleGeneration(3U));
  const auto disable_admission = RecoveryDisableAdmission(snapshot);
  ASSERT_TRUE(disable_admission.ok()) << disable_admission.error().message;
  const auto intent = DisableIntent(first_owner);
  const auto prepared = PrepareComponentLifecycleAdmission(
      intent, RecoveryAdmissionCandidate("lifecycle.1", snapshot, {DisableCatalog()}));
  ASSERT_TRUE(prepared.ok()) << prepared.error().code << ": " << prepared.error().message;
  const auto seeded_claim =
      SeedLifecycleClaim(journal, first_owner, intent, prepared.value().acceptance);
  ASSERT_TRUE(seeded_claim.ok());
  ASSERT_TRUE(journal
                  .AcceptOrReplayLifecycle(first_owner, intent, prepared.value().acceptance,
                                           prepared.value().initial_status)
                  .ok());

  FakeKernelDisablePort kernel(journal);
  kernel.RetainClaim(seeded_claim.value());
  const auto handoff_request =
      RecoveryDisableRequest(prepared.value().initial_status, disable_admission.value());
  ASSERT_TRUE(handoff_request.locally_valid());
  auto handoff_prepared_status = prepared.value().initial_status;
  ++handoff_prepared_status.status_sequence;
  handoff_prepared_status.phase = ComponentLifecycleTransactionPhaseDTO::kAcquiringDomains;
  handoff_prepared_status.latest_updated_at_ns += 10;
  handoff_prepared_status.remaining_duration_ns -= 10;
  ASSERT_TRUE(handoff_prepared_status.locally_valid());
  ASSERT_TRUE(journal
                  .RecordEffectPrepared(first_owner, PreparedDisableAttempt(handoff_request),
                                        handoff_prepared_status)
                  .ok());
  const auto handoff_acceptance = kernel.BeginDisableHandoff(handoff_request);
  ASSERT_TRUE(handoff_acceptance.ok()) << handoff_acceptance.error().message;
  const auto handoff = kernel.DisableHandoffStatus(
      handoff_request.lifecycle_transaction_id, handoff_request.lifecycle_transaction_generation);
  ASSERT_TRUE(handoff.ok()) << handoff.error().message;
  ASSERT_EQ(handoff.value().state, DisableHandoffStateDTO::kReady);

  auto held_status = handoff_prepared_status;
  ++held_status.status_sequence;
  held_status.phase = ComponentLifecycleTransactionPhaseDTO::kContaining;
  held_status.disable_handoff = handoff.value();
  held_status.configuration_fence = handoff.value().configuration_fence_handle;
  held_status.latest_updated_at_ns += 10;
  held_status.remaining_duration_ns -= 10;
  ASSERT_TRUE(held_status.locally_valid());
  ASSERT_TRUE(journal
                  .RecordEffectCompleted(
                      first_owner,
                      CompletedDisableAttempt(handoff_request, handoff_acceptance.value()),
                      held_status)
                  .ok());

  const ErrorDTO contained_without_mutation{
      "component_lifecycle_disable_mutation_not_eligible",
      "Disable containment completed without vendor lifecycle mutation",
      {}};
  auto resolved_status = held_status;
  ++resolved_status.status_sequence;
  resolved_status.phase = ComponentLifecycleTransactionPhaseDTO::kVerifying;
  resolved_status.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kFailed;
  resolved_status.domains.front().effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
  resolved_status.domains.front().error = contained_without_mutation;
  resolved_status.error = contained_without_mutation;
  resolved_status.latest_updated_at_ns += 10;
  resolved_status.remaining_duration_ns -= 10;
  ASSERT_TRUE(resolved_status.locally_valid());
  ASSERT_TRUE(journal.RecordLifecycleStatus(first_owner, resolved_status).ok());

  CCLCDomainReservationTable reservations;
  const auto under_fence_snapshot =
      Snapshot("snapshot.under-fence.1", ComponentLifecycleStateDTO::kDisabling,
               ComponentLifecycleGeneration(4U));
  FakeSnapshotSource snapshots({under_fence_snapshot});
  FakeCatalogSource catalogs(DisableCatalog());
  auto port = std::make_shared<FakeBrandPort>(
      journal, std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kActive});
  FakeResolver resolver(port);
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
  EXPECT_EQ(snapshots.read_count(), 1U);
  EXPECT_EQ(kernel.admission_observation_count(), 1U);
  EXPECT_EQ(kernel.release_count(), 1U);
  EXPECT_TRUE(kernel.saw_release_prepared());
  EXPECT_EQ(status.value()->status_owner_epoch, coordinator.request_owner_epoch());
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kFailed);
  EXPECT_EQ(status.value()->terminal_result,
            ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect);
  EXPECT_FALSE(status.value()->reconciliation_required);
  ASSERT_TRUE(status.value()->configuration_fence.has_value());
  EXPECT_EQ(status.value()->configuration_fence->state, ConfigurationFenceState::kReleased);
  ASSERT_TRUE(status.value()->disable_fence_released.has_value());
  EXPECT_TRUE(status.value()->disable_fence_released->matches(
      status.value()->disable_fence_released->request));
  EXPECT_EQ(kernel.begin_count(), 1U);
  EXPECT_TRUE(kernel.saw_handoff_prepared());
  EXPECT_EQ(port->observation_count(), 0U);
  EXPECT_EQ(port->dispatch_count(), 0U);
  EXPECT_TRUE(reservations.snapshot().empty());
  EXPECT_TRUE(coordinator.accepting_new_requests());
}

}  // namespace
}  // namespace manyforge::core
