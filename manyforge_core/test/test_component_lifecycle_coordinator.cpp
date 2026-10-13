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
#include <manyforge_core/component_lifecycle_admission.hpp>
#include <manyforge_core/component_lifecycle_coordinator.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
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

  bool drain_for(SchedulerPriority priority, std::chrono::nanoseconds timeout) override {
    return !fail_drain_ && scheduler_.drain_for(priority, timeout);
  }

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
  std::atomic<std::size_t> cancel_count_{0U};
  bool fail_drain_ = false;
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
    return journal_.LoadCurrentComponentLifecycleFact(controllable_component_id);
  }
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
        coordinator, snapshots, catalogs, resolver, kernel,
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
        coordinator, snapshots, catalogs, resolver, kernel,
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
      coordinator, snapshots, catalogs, resolver, kernel,
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

struct ApplyCoordinatorData final {
  ComponentLifecycleIntentDTO intent;
  std::optional<ComponentLifecycleCurrentSnapshotDTO> predecessor;
  BrandLifecyclePlanCatalogDTO catalog;
  std::vector<ResolvedLifecycleControlDomainScopeDTO> domains;
  std::vector<ResolvedLifecycleProviderOccurrenceDTO> providers;
  ApplyComponentBringUpEvidenceDTO evidence;
  ActiveBundlePublicationRequestDTO publication;
  std::vector<ControlDomainId> retired;
};

[[nodiscard]] ApplyCoordinatorData ApplyData(const RequestOwnerEpoch& owner,
                                             ComponentLifecycleIntentKindDTO kind,
                                             bool recovery = false, bool changed = false) {
  auto plans = PrepareCatalog().plans;
  plans.push_back(
      {ComponentLifecycleIntentKindDTO::kEnable,
       ComponentLifecycleStateDTO::kActive,
       std::nullopt,
       Predicate(ComponentLifecycleStateDTO::kActive),
       1'000'000,
       {{LifecyclePrimitiveId("enable"), Predicate(ComponentLifecycleStateDTO::kPrepared),
         Predicate(ComponentLifecycleStateDTO::kActive)}}});
  std::sort(plans.begin(), plans.end(), [](const auto& left, const auto& right) {
    return left.intent < right.intent;
  });
  const auto made = MakeBrandLifecyclePlanCatalog(ProviderManifestId("manifest.1"),
                                                  ProviderManifestRevision(Sha('1')), plans);
  if (!made.ok()) throw std::runtime_error(made.error().message);
  const auto catalog = made.value();
  auto domain = CurrentDomain();
  auto expected = ExpectedDomain();
  if (changed) {
    domain.control_domain_revision = ControlDomainRevision("candidate.domain.1");
    expected.control_domain_revision = domain.control_domain_revision;
    domain.current_control_bindings.front().control_binding_revision =
        ControlBindingRevision("candidate.binding.1");
    expected.expected_control_bindings = domain.current_control_bindings;
  }
  auto provider = Provider(catalog);
  provider.control_domain_revision = domain.control_domain_revision;
  provider.provider_instance_id = ProviderInstanceId("installed.provider.1");
  provider.provider_runtime_epoch = RuntimeEpoch("installed.runtime.1");
  std::optional<ComponentLifecycleCurrentSnapshotDTO> predecessor;
  std::vector<ControlDomainId> retired;
  if (recovery) {
    predecessor = Snapshot("placeholder", ComponentLifecycleStateDTO::kActive,
                           ComponentLifecycleGeneration(3U), catalog);
    if (changed) {
      auto old_only = CurrentDomain();
      old_only.control_domain_id = ControlDomainId("domain.2");
      old_only.control_domain_revision = ControlDomainRevision("old.domain.2");
      old_only.current_control_bindings.front().control_binding_id = ControlBindingId("binding.2");
      auto old_provider = Provider(catalog);
      old_provider.control_domain_id = old_only.control_domain_id;
      old_provider.control_domain_revision = old_only.control_domain_revision;
      predecessor->domains.push_back(old_only);
      predecessor->provider_occurrences.push_back(old_provider);
      retired.push_back(old_only.control_domain_id);
    }
    const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(*predecessor);
    if (!digest.ok()) throw std::runtime_error(digest.error().message);
    predecessor->snapshot_digest = digest.value();
  }
  auto intent = PrepareIntent(owner);
  intent.intent = kind;
  intent.expected_control_domains = {expected};
  ConfigurationFenceScopeDTO scope{
      {intent.controllable_component_id},
      {{domain.control_domain_id,
        predecessor ? std::optional(predecessor->domains.front().control_domain_revision)
                    : std::nullopt,
        domain.control_domain_revision}},
      {{domain.current_control_bindings.front().control_binding_id,
        predecessor ? std::optional(predecessor->domains.front()
                                        .current_control_bindings.front()
                                        .control_binding_revision)
                    : std::nullopt,
        domain.current_control_bindings.front().control_binding_revision}},
      domain.current_resources};
  if (!retired.empty()) {
    const auto& old_only = predecessor->domains.back();
    scope.domains.push_back(
        {old_only.control_domain_id, old_only.control_domain_revision, std::nullopt});
    scope.bindings.push_back({old_only.current_control_bindings.front().control_binding_id,
                              old_only.current_control_bindings.front().control_binding_revision,
                              std::nullopt});
  }
  const ApplyFenceOwnerDTO parent_owner{ApplyId("apply.1"), ApplyGeneration(1U)};
  const ApplyComponentBringUpReferenceDTO parent{parent_owner, ContentDigest(Sha('c'))};
  const CellAdmissionClaimIntentDTO claim_intent{CellAdmissionClaimKindDTO::kManagerApply,
                                                 CellAdmissionOwnerRoleDTO::kManager,
                                                 RequestOwnerEpoch("manager.1"),
                                                 "apply.1",
                                                 RequestId("parent.request.1"),
                                                 parent.plan_digest,
                                                 AdmissionGeneration(1U),
                                                 {},
                                                 std::nullopt,
                                                 std::nullopt};
  const CellAdmissionClaimDTO claim{claim_intent, CellAdmissionClaimGeneration(1U),
                                    AdmissionGeneration(1U), 50};
  const ConfigurationFenceStatusQueryDTO held_fence{parent_owner, ConfigurationFenceId("fence.1"),
                                                    ConfigurationFenceGeneration(1U), scope,
                                                    RuntimeEpoch("manager.runtime.1")};
  const ApplyComponentBringUpAuthorityDTO authority{parent, claim, held_fence};
  auto publication = ActivePublication().request;
  publication.transaction_ref =
      NamedApplyPublicationTransactionDTO{parent_owner.apply_id, parent_owner.apply_generation};
  publication.expected_previous_publication_generation = ActivePublication().publication_generation;
  publication.candidate_projection_revision.retained_bundle_id =
      RetainedBundleId("candidate.bundle.1");
  publication.candidate_retained_bundle_id =
      publication.candidate_projection_revision.retained_bundle_id;
  if (changed) {
    publication.candidate_projection_revision.graph_revision = GraphRevision("candidate.graph.1");
    publication.candidate_graph_revision = publication.candidate_projection_revision.graph_revision;
  }
  ApplyComponentBringUpEvidenceDTO evidence{
      authority,
      scope,
      ContentDigest(Sha('e')),
      publication.candidate_projection_revision,
      ContentDigest(Sha('f')),
      512U,
      {RuntimeUnitId("host.1"), RuntimeEpoch("host.runtime.1")},
      std::nullopt,
      std::nullopt,
      std::nullopt};
  if (predecessor) {
    evidence.recovery = ApplyComponentBringUpRecoveryEvidenceDTO{
        predecessor->snapshot_digest, "incident.1", StopId("stop.1"), StopGeneration(1U)};
    SelectedControllerOccurrenceDTO occurrence{std::nullopt, {}};
    for (const auto& old_domain : predecessor->domains)
      occurrence.domains.push_back({old_domain.control_domain_id, 4U});
    const auto successor = ComputeSelectedControllerSuccessorOccurrence(
        occurrence, std::vector<ControlDomainId>{domain.control_domain_id}, false);
    if (!successor.ok()) throw std::runtime_error(successor.error().message);
    evidence.predecessor_occurrence = occurrence;
    evidence.successor_occurrence_floor = successor.value();
  }
  evidence.original_request_payload_digest = ApplyDelegatedComponentLifecycleSemanticRequestDigest(
      intent, evidence.candidate_projection_revision, evidence.candidate_retained_bundle_digest,
      evidence.candidate_retained_bundle_bytes, evidence.original_execution_host.runtime_unit_id,
      evidence.original_execution_host.runtime_epoch, held_fence.manager_runtime_epoch, parent);
  return {intent, predecessor, catalog, {domain}, {provider}, evidence, publication, retired};
}

class ApplyPublicationReader final : public IActiveBundlePublicationReader {
 public:
  Result<ActiveBundlePublicationReceiptDTO> ReadActiveBundlePublication(
      ActiveBundlePublicationGeneration generation) const override {
    std::lock_guard lock(mutex_);
    if (fail_ || generation != head_.publication_generation)
      return Result<ActiveBundlePublicationReceiptDTO>::Err(
          {"not_current", "not the current head", {}});
    return Result<ActiveBundlePublicationReceiptDTO>::Ok(head_);
  }
  void SetHead(ActiveBundlePublicationReceiptDTO head) {
    std::lock_guard lock(mutex_);
    head_ = std::move(head);
  }
  void FailReads() {
    std::lock_guard lock(mutex_);
    fail_ = true;
  }

 private:
  mutable std::mutex mutex_;
  ActiveBundlePublicationReceiptDTO head_{ActivePublication()};
  bool fail_ = false;
};

class PinnedBrandPort final : public IBrandLifecyclePort {
 public:
  PinnedBrandPort(std::shared_ptr<FakeBrandPort> port, const std::atomic<unsigned>& depth)
      : port_(std::move(port)), depth_(depth) {}
  Result<BrandLifecycleObservationDTO> Observe(
      const BrandLifecycleObservationQueryDTO& query) override {
    if (depth_.load() == 0U) ++outside_pin;
    return port_->Observe(query);
  }
  BrandLifecyclePrimitiveResultDTO Dispatch(const BrandLifecyclePrimitiveIntentDTO& intent,
                                            std::stop_token cancellation) override {
    if (depth_.load() == 0U) ++outside_pin;
    return port_->Dispatch(intent, cancellation);
  }
  std::atomic<unsigned> outside_pin{0U};

 private:
  std::shared_ptr<FakeBrandPort> port_;
  const std::atomic<unsigned>& depth_;
};

class PinnedReadinessPort final : public ICCLCComponentCommandReadinessPort {
 public:
  explicit PinnedReadinessPort(const std::atomic<unsigned>& depth) : depth_(depth) {}
  Result<ComponentCommandReadinessDTO> CloseComponentCommandEligibility(
      const LifecycleTransactionId& transaction,
      const ControllableComponentId& component) override {
    ++independent_closes;
    return port_.CloseComponentCommandEligibility(transaction, component);
  }
  Result<ComponentCommandReadinessDTO> PublishComponentCommandReadiness(
      const ComponentCommandReadinessPublicationDTO& publication) override {
    if (depth_.load() == 0U) ++outside_pin;
    return port_.PublishComponentCommandReadiness(publication);
  }
  Result<ComponentCommandReadinessDTO> ReadComponentCommandReadiness(
      const ControllableComponentId& component) const override {
    return port_.ReadComponentCommandReadiness(component);
  }
  std::vector<std::string> calls() const { return port_.calls(); }
  std::vector<ComponentCommandReadinessPublicationDTO> publications() const {
    return port_.publications();
  }
  std::atomic<unsigned> outside_pin{0U};
  std::atomic<unsigned> independent_closes{0U};

 private:
  FakeReadinessPort port_;
  const std::atomic<unsigned>& depth_;
};

class ApplyCoordinatorFixture final {
 public:
  explicit ApplyCoordinatorFixture(
      ComponentLifecycleIntentKindDTO kind = ComponentLifecycleIntentKindDTO::kPrepare,
      bool recovery = false, bool changed = false, DispatchMode mode = DispatchMode::kKnownSuccess,
      bool empty_global_readiness = false)
      : journal(temporary.path() / std::string(kCCLCJournalFilename), CellId("cell.1"), 8U),
        snapshots({}),
        catalogs(PrepareCatalog()),
        port(std::make_shared<FakeBrandPort>(
            journal,
            kind == ComponentLifecycleIntentKindDTO::kPrepare
                ? std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                          ComponentLifecycleStateDTO::kPrepared}
            : kind == ComponentLifecycleIntentKindDTO::kEnable
                ? std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared,
                                                          ComponentLifecycleStateDTO::kActive}
                : std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDisabled},
            mode)),
        pinned_port(std::make_shared<PinnedBrandPort>(port, pin_depth)),
        resolver(port),
        readiness(pin_depth),
        data(ApplyData(RequestOwnerEpoch("placeholder"), kind, recovery, changed)),
        coordinator(Config(), journal, reservations, snapshots, catalogs, resolver, kernel, clock,
                    scheduler, std::nullopt, empty_global_readiness ? nullptr : &readiness) {
    clock.set(100);
    if (recovery) {
      // Establish the predecessor through the existing NativeApply owner,
      // rather than allowing the caller's expected generation to seed W.
      const NamedApplyPublicationTransactionDTO seeded_apply{
          ApplyId("predecessor.apply"), ApplyGeneration(1U)};
      const auto seeded = journal.PlanNativeApply(
          {seeded_apply, previous_publication.request.candidate_retained_bundle_id,
           previous_publication.request.candidate_graph_revision,
           {data.intent.controllable_component_id}, {}, {}, {}, {}, {},
           ComponentLifecycleStateDTO::kActive});
      if (!seeded.ok()) throw std::runtime_error(seeded.error().message);
      previous_publication.request.transaction_ref = seeded_apply;
      previous_publication.request.expected_previous_publication_generation =
          ActiveBundlePublicationGeneration(1U);
      const auto adopted =
          journal.AdoptNativeApplyFacts({previous_publication, seeded.value().components, {}});
      if (!adopted.ok()) throw std::runtime_error(adopted.error().message);
      publication_reader.SetHead(previous_publication);
    }
    const auto started = coordinator.Start();
    if (!started.ok()) throw std::runtime_error(started.error().message);
    data = ApplyData(coordinator.request_owner_epoch(), kind, recovery, changed);
    const auto selection = journal.ReadApplyComponentGenerationSelection(
        coordinator.request_owner_epoch(), data.intent.controllable_component_id);
    if (!selection.ok()) throw std::runtime_error(selection.error().message);
    data.intent.apply_generation_selection = selection.value();
    data.intent.expected_component_lifecycle_generation = ComponentLifecycleGeneration(
        selection.value().predecessor_generation
            ? selection.value().predecessor_generation->value()
            : 0U);
    if (data.predecessor) {
      data.predecessor->component_lifecycle_generation =
          *selection.value().predecessor_generation;
      data.predecessor->active_publication_receipt = previous_publication;
      const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(*data.predecessor);
      if (!digest.ok()) throw std::runtime_error(digest.error().message);
      data.predecessor->snapshot_digest = digest.value();
      data.evidence.recovery->predecessor_snapshot_digest = digest.value();
      snapshots.SetSnapshot(*data.predecessor);
      const auto qualified_selection = journal.ReadApplyComponentGenerationSelection(
          coordinator.request_owner_epoch(), data.intent.controllable_component_id,
          ApplyComponentQualifiedPredecessorDTO{
              data.predecessor->component_lifecycle_generation, data.predecessor->snapshot_digest});
      if (!qualified_selection.ok()) throw std::runtime_error(qualified_selection.error().message);
      data.intent.apply_generation_selection = qualified_selection.value();
    }
    data.evidence.original_request_payload_digest =
        ApplyDelegatedComponentLifecycleSemanticRequestDigest(
            data.intent, data.evidence.candidate_projection_revision,
            data.evidence.candidate_retained_bundle_digest,
            data.evidence.candidate_retained_bundle_bytes,
            data.evidence.original_execution_host.runtime_unit_id,
            data.evidence.original_execution_host.runtime_epoch,
            data.evidence.parent_authority.held_fence_query.manager_runtime_epoch,
            data.evidence.parent_authority.parent);
    kernel.RetainClaim(data.evidence.parent_authority.parent_claim);
    if (data.evidence.recovery) {
      const auto& recovery_evidence = *data.evidence.recovery;
      ComponentLifecycleIncidentDTO incident{
          recovery_evidence.incident_id, data.intent.controllable_component_id,
          LifecycleBlockingCauseClassDTO::kProviderLost, {}, 20, "controller exited",
          std::nullopt, {}, ComponentIncidentContainmentStateDTO::kPending,
          std::nullopt, std::nullopt, false, std::nullopt, std::nullopt, std::nullopt};
      const auto policy = LifecycleIncidentRecoveryPolicyFor(
          DefaultLifecycleIncidentRecoveryPolicy(), incident.cause_class);
      const auto original = ComponentIncidentEventPayload(incident, policy,
                                                          kComponentIncidentEventKind);
      const auto opened = journal.OpenComponentIncident({incident, original}, 0U);
      if (!opened.ok()) throw std::runtime_error(opened.error().message);
      const auto contained = ComponentIncidentRecordWithContainment(
          original, ComponentIncidentContainmentStateDTO::kRequested,
          ComponentIncidentContainmentReceiptDTO{
              recovery_evidence.containment_stop_id,
              recovery_evidence.containment_stop_generation, {}, std::nullopt},
          std::nullopt);
      const auto recorded = journal.RecordComponentIncidentContainment(
          incident.incident_id, ComponentIncidentContainmentStateDTO::kRequested, contained, 30);
      if (!recorded.ok() || !recorded.value())
        throw std::runtime_error("failed to record fixture containment");
    }
  }
  ComponentLifecycleCoordinatorApplyBringUpServices Services() {
    return {
        [this](const ComponentLifecycleStatusDTO& status,
               const std::function<Result<void>()>& action) {
          std::lock_guard lock(pin_mutex);
          ++pin_calls;
          if (!live ||
              status.accepted_intent.request_context.request_owner_epoch !=
                  coordinator.request_owner_epoch() ||
              status.apply_bring_up != std::optional(data.evidence))
            return Result<void>::Err({"original_host_replaced", "original host unavailable", {}});
          ++pin_depth;
          struct Guard {
            ApplyCoordinatorFixture& fixture;
            ~Guard() {
              --fixture.pin_depth;
              ++fixture.finished_pin_actions;
              fixture.pin_changed.notify_all();
            }
          } guard{*this};
          return action();
        },
        &publication_reader, data.publication, previous_publication};
  }
  Result<CCLCLifecycleRequestResultDTO> Accept(
      ComponentLifecycleCoordinatorApplyBringUpServices services) {
    return coordinator.AcceptApplyComponentBringUp(data.intent, data.evidence, data.predecessor,
                                                   {data.catalog}, data.domains, data.providers,
                                                   std::move(services));
  }
  Result<CCLCLifecycleRequestResultDTO> Continue() {
    return coordinator.ContinueApplyComponentBringUp(
        data.intent, data.evidence.original_request_payload_digest, data.domains, data.providers,
        {{data.providers.front(), pinned_port}}, data.retired);
  }
  ActiveBundlePublicationReceiptDTO Published() const {
    return {data.publication, ActiveBundlePublicationGeneration(3U),
            ContentDigest("public.pointer.3")};
  }
  bool WaitForPinnedActions(unsigned count) {
    std::unique_lock lock(pin_mutex);
    return pin_changed.wait_for(lock, std::chrono::seconds(2),
                                [&] { return finished_pin_actions >= count; });
  }
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal;
  CCLCDomainReservationTable reservations;
  FakeSnapshotSource snapshots;
  FakeCatalogSource catalogs;
  std::atomic<unsigned> pin_depth{0U};
  std::shared_ptr<FakeBrandPort> port;
  std::shared_ptr<PinnedBrandPort> pinned_port;
  FakeResolver resolver;
  CountingAdmissionReadPort kernel;
  FakeClock clock;
  WaitableManualScheduler scheduler;
  PinnedReadinessPort readiness;
  ActiveBundlePublicationReceiptDTO previous_publication{ActivePublication()};
  ApplyPublicationReader publication_reader;
  std::mutex pin_mutex;
  std::condition_variable pin_changed;
  unsigned finished_pin_actions = 0U;
  std::atomic<unsigned> pin_calls{0U};
  bool live = true;
  ApplyCoordinatorData data;
  ComponentLifecycleCoordinator coordinator;
};

TEST(ComponentLifecycleCoordinator, ApplyAcceptIsNoEffectAndContinueHoldsUntilExactPublication) {
  for (const auto kind :
       {ComponentLifecycleIntentKindDTO::kPrepare, ComponentLifecycleIntentKindDTO::kEnable,
        ComponentLifecycleIntentKindDTO::kDisable}) {
    ApplyCoordinatorFixture fixture(kind);
    const auto accepted = fixture.Accept(fixture.Services());
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    EXPECT_EQ(accepted.value().replay_status, RequestReplayStatusDTO::kAcceptedNew);
    EXPECT_EQ(fixture.port->observation_count(), 0U);
    EXPECT_EQ(fixture.port->dispatch_count(), 0U);
    EXPECT_TRUE(fixture.readiness.calls().empty());
    EXPECT_EQ(fixture.kernel.claim_acquires.load(), 0U);
    EXPECT_FALSE(accepted.value().status->current_state);
    auto replay_services = fixture.Services();
    replay_services.with_original_live_parent = {};
    const auto replay = fixture.Accept(std::move(replay_services));
    ASSERT_TRUE(replay.ok());
    EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
    const auto continued = fixture.Continue();
    ASSERT_TRUE(continued.ok()) << continued.error().message << " "
                               << testing::PrintToString(continued.error().details);
    ASSERT_TRUE(fixture.scheduler.WaitForScheduled(1U, std::chrono::seconds(2)));
    const auto holding = fixture.coordinator.LoadLifecycleStatus(
        accepted.value().acceptance->lifecycle_transaction_id);
    ASSERT_TRUE(holding.ok() && holding.value());
    EXPECT_EQ(holding.value()->phase, ComponentLifecycleTransactionPhaseDTO::kVerifying);
    EXPECT_EQ(holding.value()->current_state, holding.value()->requested_state.state);
    EXPECT_FALSE(holding.value()->terminal_result);
    ASSERT_EQ(fixture.readiness.publications().size(), 1U);
    EXPECT_EQ(fixture.readiness.publications().front().authority,
              ComponentReadinessAuthorityDTO::kParentApplyFence);
    EXPECT_FALSE(fixture.readiness.publications().front().command_eligible);
    EXPECT_TRUE(fixture.reservations.snapshot().empty());
    EXPECT_FALSE(fixture.Continue().ok());
    fixture.publication_reader.SetHead(fixture.Published());
    fixture.clock.set(1'100);
    fixture.scheduler.AdvanceTo(1'100);
    ASSERT_TRUE(fixture.coordinator.DrainFor(std::chrono::seconds(2)));
    const auto terminal = fixture.coordinator.ReadApplyComponentBringUpRequestResult(
        fixture.data.intent, fixture.data.evidence.original_request_payload_digest);
    ASSERT_TRUE(terminal.ok() && terminal.value() && terminal.value()->status);
    EXPECT_EQ(terminal.value()->status->terminal_result,
              ComponentLifecycleTerminalResultDTO::kSucceeded);
    EXPECT_TRUE(fixture.reservations.snapshot().empty());
    EXPECT_EQ(fixture.pinned_port->outside_pin.load(), 0U);
    EXPECT_EQ(fixture.readiness.outside_pin.load(), 0U);
    EXPECT_EQ(fixture.readiness.independent_closes.load(), 0U);
    // Acceptance probes the original owner source before the journal absence CAS.
    EXPECT_EQ(fixture.snapshots.read_count(), 1U);
    EXPECT_EQ(fixture.catalogs.load_count(), 0U);
    EXPECT_EQ(fixture.resolver.resolve_count(), 0U);
    const auto& claim = fixture.data.evidence.parent_authority.parent_claim;
    const auto parent =
        fixture.kernel.GetCellAdmissionRecoveryReceipt({claim.intent, claim, std::nullopt});
    ASSERT_TRUE(parent.ok());
    EXPECT_EQ(parent.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
  }
}

TEST(ComponentLifecycleCoordinator,
     ApplyRecoveryUsesActualCandidateAndRetiredKnownNoEffectHistory) {
  ApplyCoordinatorFixture fixture(ComponentLifecycleIntentKindDTO::kPrepare, true, true);
  const auto accepted = fixture.Accept(fixture.Services());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_EQ(accepted.value().status->domains.size(), 2U);
  EXPECT_EQ(accepted.value().status->current_state, ComponentLifecycleStateDTO::kActive);
  EXPECT_TRUE(fixture.reservations.snapshot().empty());
  const auto continued = fixture.Continue();
    ASSERT_TRUE(continued.ok()) << continued.error().message << " "
                               << testing::PrintToString(continued.error().details);
  ASSERT_TRUE(fixture.scheduler.WaitForScheduled(1U, std::chrono::seconds(2)));
  const auto holding = fixture.coordinator.LoadLifecycleStatus(
      accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(holding.ok() && holding.value());
  EXPECT_EQ(holding.value()->domains.front().resolved_provider_occurrence,
            fixture.data.providers.front());
  EXPECT_EQ(holding.value()->domains.front().current_scope, fixture.data.domains.front());
  const auto& retired = holding.value()->domains.back();
  EXPECT_FALSE(retired.expected_domain);
  EXPECT_EQ(retired.current_scope, fixture.data.predecessor->domains.back());
  EXPECT_EQ(retired.current_state, ComponentLifecycleStateDTO::kActive);
  EXPECT_EQ(retired.effect, ComponentLifecycleDomainEffectDTO::kKnownNoEffect);
  EXPECT_TRUE(retired.primitive_attempts.empty());
  EXPECT_EQ(fixture.port->dispatch_count(), 1U);
  fixture.publication_reader.SetHead(fixture.Published());
  fixture.clock.set(1'100);
  fixture.scheduler.AdvanceTo(1'100);
  ASSERT_TRUE(fixture.coordinator.DrainFor(std::chrono::seconds(2)));
  EXPECT_EQ(fixture.pinned_port->outside_pin.load(), 0U);
}

TEST(ComponentLifecycleCoordinator,
     ApplyMixedInitialObservationsKeepAggregateAbsentUntilTargetHolding) {
  ApplyCoordinatorFixture fixture;
  auto second = fixture.data.domains.front();
  second.control_domain_id = ControlDomainId("domain.2");
  second.control_domain_revision = ControlDomainRevision("domain.revision.2");
  second.current_control_bindings.front().control_binding_id = ControlBindingId("binding.2");
  second.current_resources = {ResourceId("resource.2")};
  auto expected = fixture.data.intent.expected_control_domains.front();
  expected.control_domain_id = second.control_domain_id;
  expected.control_domain_revision = second.control_domain_revision;
  expected.expected_control_bindings = second.current_control_bindings;
  auto provider = fixture.data.providers.front();
  provider.control_domain_id = second.control_domain_id;
  provider.control_domain_revision = second.control_domain_revision;
  provider.provider_instance_id = ProviderInstanceId("installed.provider.2");
  fixture.data.domains.push_back(second);
  fixture.data.providers.push_back(provider);
  fixture.data.intent.expected_control_domains.push_back(expected);
  auto& evidence = fixture.data.evidence;
  evidence.selected_scope.domains.push_back(
      {second.control_domain_id, std::nullopt, second.control_domain_revision});
  evidence.selected_scope.bindings.push_back(
      {second.current_control_bindings.front().control_binding_id, std::nullopt,
       second.current_control_bindings.front().control_binding_revision});
  evidence.selected_scope.resources.push_back(ResourceId("resource.2"));
  evidence.parent_authority.held_fence_query.expected_scope = evidence.selected_scope;
  evidence.original_request_payload_digest = ApplyDelegatedComponentLifecycleSemanticRequestDigest(
      fixture.data.intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      evidence.original_execution_host.runtime_unit_id,
      evidence.original_execution_host.runtime_epoch,
      evidence.parent_authority.held_fence_query.manager_runtime_epoch,
      evidence.parent_authority.parent);
  const auto accepted = fixture.coordinator.AcceptApplyComponentBringUp(
      fixture.data.intent, evidence, std::nullopt, {fixture.data.catalog, fixture.data.catalog},
      fixture.data.domains, fixture.data.providers, fixture.Services());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  auto satisfied = std::make_shared<FakeBrandPort>(
      fixture.journal,
      std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kPrepared});
  auto pinned_satisfied = std::make_shared<PinnedBrandPort>(satisfied, fixture.pin_depth);
  const auto continued = fixture.coordinator.ContinueApplyComponentBringUp(
      fixture.data.intent, evidence.original_request_payload_digest, fixture.data.domains,
      fixture.data.providers,
      {{fixture.data.providers.front(), fixture.pinned_port}, {provider, pinned_satisfied}}, {});
  ASSERT_TRUE(continued.ok()) << continued.error().message << " "
                             << testing::PrintToString(continued.error().details);
  ASSERT_TRUE(continued.value().status);
  EXPECT_FALSE(continued.value().status->previous_state);
  EXPECT_FALSE(continued.value().status->current_state);
  ASSERT_EQ(continued.value().status->domains.size(), 2U);
  EXPECT_EQ(continued.value().status->domains.front().current_state,
            ComponentLifecycleStateDTO::kDeclared);
  EXPECT_EQ(continued.value().status->domains.back().current_state,
            ComponentLifecycleStateDTO::kPrepared);
  ASSERT_TRUE(fixture.scheduler.WaitForScheduled(1U, std::chrono::seconds(2)));
  const auto holding = fixture.coordinator.LoadLifecycleStatus(
      accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(holding.ok() && holding.value());
  EXPECT_FALSE(holding.value()->previous_state);
  EXPECT_EQ(holding.value()->current_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(fixture.port->dispatch_count(), 1U);
  EXPECT_EQ(satisfied->dispatch_count(), 0U);
  fixture.publication_reader.SetHead(fixture.Published());
  fixture.clock.set(1'100);
  fixture.scheduler.AdvanceTo(1'100);
  ASSERT_TRUE(fixture.coordinator.DrainFor(std::chrono::seconds(2)));
  EXPECT_EQ(pinned_satisfied->outside_pin.load(), 0U);
}

TEST(ComponentLifecycleCoordinator, ApplyForeignAuthorityAndInstallClosureNeverDispatch) {
  ApplyCoordinatorFixture fixture;
  auto absent_services = fixture.Services();
  absent_services.with_original_live_parent = {};
  EXPECT_FALSE(fixture.Accept(std::move(absent_services)).ok());
  EXPECT_TRUE(fixture.reservations.snapshot().empty());
  auto services = fixture.Services();
  services.with_original_live_parent = [](const auto&, const auto&) { return Result<void>::Ok(); };
  EXPECT_FALSE(fixture.Accept(std::move(services)).ok());
  EXPECT_TRUE(fixture.reservations.snapshot().empty());
  services = fixture.Services();
  services.expected_publication.private_materialization_content_digest = ContentDigest("");
  EXPECT_FALSE(fixture.Accept(std::move(services)).ok());
  const auto accepted = fixture.Accept(fixture.Services());
  ASSERT_TRUE(accepted.ok());
  auto foreign = fixture.data.providers;
  foreign.front().provider_runtime_epoch = RuntimeEpoch("foreign.runtime");
  EXPECT_FALSE(fixture.coordinator
                   .ContinueApplyComponentBringUp(
                       fixture.data.intent, fixture.data.evidence.original_request_payload_digest,
                       fixture.data.domains, foreign, {{foreign.front(), fixture.pinned_port}}, {})
                   .ok());
  EXPECT_FALSE(fixture.coordinator
                   .ContinueApplyComponentBringUp(
                       fixture.data.intent, fixture.data.evidence.original_request_payload_digest,
                       fixture.data.domains, fixture.data.providers,
                       {{foreign.front(), fixture.pinned_port}}, {})
                   .ok());
  EXPECT_FALSE(fixture.coordinator
                   .ContinueApplyComponentBringUp(
                       fixture.data.intent, fixture.data.evidence.original_request_payload_digest,
                       fixture.data.domains, fixture.data.providers,
                       {{fixture.data.providers.front(), fixture.pinned_port}},
                       {ControlDomainId("invented.retirement")})
                   .ok());
  fixture.live = false;
  EXPECT_FALSE(fixture.Continue().ok());
  EXPECT_EQ(fixture.port->observation_count(), 0U);
  EXPECT_EQ(fixture.port->dispatch_count(), 0U);
  EXPECT_TRUE(fixture.reservations.snapshot().empty());
}

TEST(ComponentLifecycleCoordinator, ApplyForeignFullParentClaimRefusesBeforeAcceptance) {
  ApplyCoordinatorFixture fixture;
  auto foreign = fixture.data.evidence.parent_authority.parent_claim;
  foreign.intent.request_payload_digest = ContentDigest(Sha('0'));
  fixture.kernel.RetainClaim(foreign);
  EXPECT_FALSE(fixture.Accept(fixture.Services()).ok());
  EXPECT_EQ(fixture.port->observation_count(), 0U);
  EXPECT_TRUE(fixture.reservations.snapshot().empty());
  EXPECT_EQ(fixture.kernel.claim_acquires.load(), 0U);
}

TEST(ComponentLifecycleCoordinator,
     ApplyGenerationReservationRaceRefusesBeforeAcceptanceOrPhysicalObservation) {
  ApplyCoordinatorFixture fixture;
  auto services = fixture.Services();
  const auto original_pin = services.with_original_live_parent;
  services.with_original_live_parent =
      [&](const ComponentLifecycleStatusDTO& status,
          const std::function<Result<void>()>& action) {
        return original_pin(status, [&]() -> Result<void> {
          // The owner preview was already compared before entering the pin.
          // A real NativeApply reservation now changes R without seeding W.
          const auto reserved = fixture.journal.PlanNativeApply(
              {{fixture.data.evidence.parent_authority.parent.owner.apply_id,
                fixture.data.evidence.parent_authority.parent.owner.apply_generation},
               fixture.data.evidence.candidate_projection_revision.retained_bundle_id,
               fixture.data.evidence.candidate_projection_revision.graph_revision,
               {fixture.data.intent.controllable_component_id}, {}, {}, {}, {}, {},
               ComponentLifecycleStateDTO::kDeclared});
          if (!reserved.ok()) return Result<void>::Err(reserved.error());
          return action();
        });
      };
  const auto accepted = fixture.Accept(std::move(services));
  ASSERT_FALSE(accepted.ok());
  const auto retained = fixture.journal.ReadLifecycleRequestResult(fixture.data.intent);
  ASSERT_TRUE(retained.ok());
  if (retained.value()) {
    EXPECT_FALSE(retained.value()->acceptance);
  }
  const auto generation = fixture.journal.ReadApplyComponentGenerationSelection(
      fixture.coordinator.request_owner_epoch(), fixture.data.intent.controllable_component_id);
  ASSERT_TRUE(generation.ok());
  EXPECT_FALSE(generation.value().predecessor_generation);
  EXPECT_EQ(generation.value().candidate_generation, ComponentLifecycleGeneration(2U));
  EXPECT_EQ(fixture.port->observation_count(), 0U);
  EXPECT_EQ(fixture.port->dispatch_count(), 0U);
  EXPECT_TRUE(fixture.readiness.calls().empty());
  // The Coordinator conservatively retains parent exclusion after a failed journal
  // invocation; refusal does not grant settlement or another effect attempt.
  EXPECT_FALSE(fixture.coordinator.accepting_new_requests());
  EXPECT_TRUE(fixture.reservations.snapshot().empty());
  EXPECT_EQ(fixture.kernel.claim_acquires.load(), 0U);
}

TEST(ComponentLifecycleCoordinator,
     ApplyRepeatedLiveActionKeepsOneDurableAcceptanceAndCannotContinue) {
  ApplyCoordinatorFixture fixture;
  auto services = fixture.Services();
  services.with_original_live_parent = [](const auto&,
                                          const std::function<Result<void>()>& action) {
    const auto first = action();
    if (!first.ok()) return first;
    return action();
  };
  const auto accepted = fixture.Accept(std::move(services));
  // The first journal commit is authoritative even when the live callback
  // violates its synchronous single-action contract after that commit.
  ASSERT_TRUE(accepted.ok());
  EXPECT_EQ(accepted.value().replay_status, RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_FALSE(fixture.coordinator.accepting_new_requests());
  EXPECT_FALSE(fixture.Continue().ok());
  auto replay_services = fixture.Services();
  replay_services.with_original_live_parent = {};
  const auto replay = fixture.Accept(std::move(replay_services));
  ASSERT_TRUE(replay.ok());
  ASSERT_TRUE(replay.value().acceptance);
  auto original_replayed = *accepted.value().acceptance;
  original_replayed.replay_status = RequestReplayStatusDTO::kReplayedExact;
  EXPECT_EQ(replay.value().acceptance, std::optional(original_replayed));
  EXPECT_EQ(replay.value().status, accepted.value().status);
  EXPECT_EQ(fixture.port->observation_count(), 0U);
  EXPECT_EQ(fixture.port->dispatch_count(), 0U);
  EXPECT_TRUE(fixture.reservations.snapshot().empty());
}

TEST(ComponentLifecycleCoordinator,
     ApplyUncertainDispatchStopsWithoutAnotherObservationOrPublication) {
  ApplyCoordinatorFixture fixture(ComponentLifecycleIntentKindDTO::kPrepare, false, false,
                                  DispatchMode::kUncertain);
  const auto accepted = fixture.Accept(fixture.Services());
  ASSERT_TRUE(accepted.ok());
  const auto continued = fixture.Continue();
    ASSERT_TRUE(continued.ok()) << continued.error().message << " "
                               << testing::PrintToString(continued.error().details);
  ASSERT_TRUE(fixture.WaitForPinnedActions(3U));
  EXPECT_FALSE(fixture.coordinator.DrainFor(std::chrono::milliseconds(20)));
  const auto retained = fixture.coordinator.LoadLifecycleStatus(
      accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(retained.ok() && retained.value());
  EXPECT_EQ(retained.value()->phase, ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown);
  EXPECT_TRUE(retained.value()->reconciliation_required);
  EXPECT_FALSE(retained.value()->terminal_result);
  EXPECT_EQ(fixture.port->dispatch_count(), 1U);
  EXPECT_EQ(fixture.port->observation_count(), 1U);
  EXPECT_TRUE(fixture.readiness.publications().empty());
  EXPECT_TRUE(fixture.reservations.snapshot().empty());
  EXPECT_FALSE(fixture.Continue().ok());
  EXPECT_EQ(fixture.pinned_port->outside_pin.load(), 0U);
}

TEST(ComponentLifecycleCoordinator,
     ApplyMissingOrForeignPublicationRetainsUnknownParentWithoutChildReservations) {
  for (const auto kind :
       {ComponentLifecycleIntentKindDTO::kPrepare, ComponentLifecycleIntentKindDTO::kDisable}) {
    for (const bool foreign : {false, true}) {
      ApplyCoordinatorFixture fixture(kind);
      const auto accepted = fixture.Accept(fixture.Services());
      ASSERT_TRUE(accepted.ok());
      const auto continued = fixture.Continue();
    ASSERT_TRUE(continued.ok()) << continued.error().message << " "
                               << testing::PrintToString(continued.error().details);
      ASSERT_TRUE(fixture.scheduler.WaitForScheduled(1U, std::chrono::seconds(2)));
      if (foreign) {
        auto wrong = fixture.Published();
        wrong.request.private_materialization_content_digest =
            ContentDigest("foreign.materialization");
        fixture.publication_reader.SetHead(wrong);
        fixture.clock.set(1'100);
        fixture.scheduler.AdvanceTo(1'100);
      } else {
        fixture.publication_reader.FailReads();
        fixture.clock.set(1'000'100);
        fixture.scheduler.AdvanceTo(1'000'100);
      }
      ASSERT_TRUE(fixture.WaitForPinnedActions(4U));
      // Drain cannot complete while the unknown original child remains active.
      EXPECT_FALSE(fixture.coordinator.DrainFor(std::chrono::milliseconds(20)));
      const auto retained = fixture.coordinator.LoadLifecycleStatus(
          accepted.value().acceptance->lifecycle_transaction_id);
      ASSERT_TRUE(retained.ok() && retained.value());
      EXPECT_EQ(retained.value()->phase, ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown);
      EXPECT_TRUE(retained.value()->reconciliation_required);
      EXPECT_FALSE(retained.value()->terminal_result);
      EXPECT_TRUE(fixture.reservations.snapshot().empty());
      EXPECT_EQ(fixture.port->dispatch_count(),
                kind == ComponentLifecycleIntentKindDTO::kPrepare ? 1U : 0U);
      EXPECT_FALSE(fixture.Continue().ok());
    }
  }
}

TEST(ComponentLifecycleCoordinator, ApplyReplacementCannotRecreateLiveContinuationOrReleaseParent) {
  ApplyCoordinatorFixture fixture;
  const auto accepted = fixture.Accept(fixture.Services());
  ASSERT_TRUE(accepted.ok());
  fixture.coordinator.CloseAdmission();
  // Q98 replacement retains global observation-only closure without child reservations.
  CCLCDomainReservationTable replacement_reservations;
  auto replacement_config = Config();
  replacement_config.request_owner_epoch = RequestOwnerEpoch("replacement.seed.1");
  ComponentLifecycleCoordinator actual_replacement(
      replacement_config, fixture.journal, replacement_reservations, fixture.snapshots,
      fixture.catalogs, fixture.resolver, fixture.kernel, fixture.clock, fixture.scheduler,
      std::nullopt, &fixture.readiness);
  ASSERT_TRUE(actual_replacement.Start().ok());
  EXPECT_FALSE(actual_replacement
                   .ContinueApplyComponentBringUp(
                       fixture.data.intent, fixture.data.evidence.original_request_payload_digest,
                       fixture.data.domains, fixture.data.providers,
                       {{fixture.data.providers.front(), fixture.pinned_port}}, {})
                   .ok());
  auto replay_services = fixture.Services();
  replay_services.with_original_live_parent = {};
  const auto replay = actual_replacement.AcceptApplyComponentBringUp(
      fixture.data.intent, fixture.data.evidence, fixture.data.predecessor, {fixture.data.catalog},
      fixture.data.domains, fixture.data.providers, std::move(replay_services));
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(fixture.port->observation_count(), 0U);
  EXPECT_EQ(fixture.port->dispatch_count(), 0U);
  EXPECT_TRUE(fixture.readiness.publications().empty());
  EXPECT_TRUE(replacement_reservations.snapshot().empty());
  const auto& claim = fixture.data.evidence.parent_authority.parent_claim;
  const auto parent =
      fixture.kernel.GetCellAdmissionRecoveryReceipt({claim.intent, claim, std::nullopt});
  ASSERT_TRUE(parent.ok());
  EXPECT_EQ(parent.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
}

TEST(ComponentLifecycleCoordinator,
     ApplyQualifiedPredecessorIsRereadUnderTheOriginalPinBeforeAcceptance) {
  ApplyCoordinatorFixture fixture(ComponentLifecycleIntentKindDTO::kPrepare, true);
  ASSERT_TRUE(fixture.data.predecessor);
  ASSERT_TRUE(fixture.data.intent.apply_generation_selection->owner_qualified_predecessor);
  auto changed = *fixture.data.predecessor;
  changed.component_lifecycle_state = ComponentLifecycleStateDTO::kDisabled;
  const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(changed);
  ASSERT_TRUE(digest.ok()) << digest.error().message;
  changed.snapshot_digest = digest.value();
  ASSERT_TRUE(VerifyComponentLifecycleCurrentSnapshot(changed).ok());
  fixture.snapshots.SetSnapshot(changed);
  const auto accepted = fixture.Accept(fixture.Services());
  ASSERT_FALSE(accepted.ok());
  EXPECT_EQ(accepted.error().details.at("reason"), "predecessor_snapshot_changed");
  const auto stored = fixture.journal.ReadLifecycleRequestResult(fixture.data.intent);
  ASSERT_TRUE(stored.ok());
  if (stored.value()) {
    EXPECT_FALSE(stored.value()->acceptance);
  }
  EXPECT_EQ(fixture.port->observation_count(), 0U);
  EXPECT_EQ(fixture.port->dispatch_count(), 0U);
  EXPECT_TRUE(fixture.readiness.publications().empty());
  EXPECT_TRUE(fixture.reservations.snapshot().empty());
  const auto& claim = fixture.data.evidence.parent_authority.parent_claim;
  const auto held = fixture.kernel.GetCellAdmissionRecoveryReceipt({claim.intent, claim, std::nullopt});
  ASSERT_TRUE(held.ok());
  EXPECT_EQ(held.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
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
      EXPECT_EQ(terminal.value()->terminal_result,
                ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect);
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
  ASSERT_TRUE(scheduler.WaitForScheduled(1U, std::chrono::seconds(2)));
  EXPECT_EQ(scheduler.pending_count(SchedulerPriority::Lifecycle), 1U);
  EXPECT_FALSE(coordinator.DrainFor(std::chrono::nanoseconds::zero()));
  EXPECT_EQ(scheduler.cancel_count(), 0U);
  EXPECT_EQ(scheduler.pending_count(SchedulerPriority::Lifecycle), 1U);
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
  EXPECT_EQ(status.value()->phase, ComponentLifecycleTransactionPhaseDTO::kSucceeded);
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_TRUE(status.value()->domains.front().reconciliation_evidence.has_value());
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
  EXPECT_EQ(status.value()->terminal_result,
            ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect);
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
        FakeCellAdmissionPort kernel;
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
  EXPECT_EQ(status.value()->terminal_result,
            ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect);
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
  status.domains.front().current_scope->active_control_configuration = configuration;
  const ExpectedActiveControlConfigurationDTO expected{
      ExpectedActiveControlConfigurationPresenceDTO::kPresent,
      configuration.active_control_configuration_id,
      configuration.active_control_configuration_generation};
  status.accepted_intent.expected_control_domains.front().expected_active_control_configuration =
      expected;
  status.domains.front().expected_domain->expected_active_control_configuration = expected;
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

// Q98 uses one immutable verified parent, not one reservation per child.
class SharedDomainApplyFixture final {
 public:
  explicit SharedDomainApplyFixture(DispatchMode mode = DispatchMode::kKnownSuccess,
                                    bool empty_global_readiness = false)
      : base(ComponentLifecycleIntentKindDTO::kPrepare, false, false, mode,
             empty_global_readiness),
        second(ApplyData(base.coordinator.request_owner_epoch(),
                         ComponentLifecycleIntentKindDTO::kPrepare)),
        shared_port(std::make_shared<FakeBrandPort>(
            base.journal,
            std::vector<ComponentLifecycleStateDTO>{ComponentLifecycleStateDTO::kDeclared,
                                                    ComponentLifecycleStateDTO::kPrepared,
                                                    ComponentLifecycleStateDTO::kPrepared}, mode)),
        pinned_port(std::make_shared<PinnedBrandPort>(shared_port, base.pin_depth)) {
    second.intent.controllable_component_id = ControllableComponentId("component.2");
    second.intent.request_context.request_id = RequestId("request.2");
    second.domains.front().current_resources = {ResourceId("resource.2")};
    second.evidence.selected_scope.controllable_component_ids =
        {second.intent.controllable_component_id};
    second.evidence.selected_scope.resources = second.domains.front().current_resources;
    const auto selected = base.journal.ReadApplyComponentGenerationSelection(
        base.coordinator.request_owner_epoch(), second.intent.controllable_component_id);
    if (!selected.ok()) throw std::runtime_error(selected.error().message);
    second.intent.apply_generation_selection = selected.value();
    second.intent.expected_component_lifecycle_generation = ComponentLifecycleGeneration(
        selected.value().predecessor_generation
            ? selected.value().predecessor_generation->value() : 0U);
    auto& parent_scope = base.data.evidence.parent_authority.held_fence_query.expected_scope;
    parent_scope.controllable_component_ids.push_back(second.intent.controllable_component_id);
    parent_scope.resources.push_back(second.domains.front().current_resources.front());
    second.evidence.parent_authority = base.data.evidence.parent_authority;
    for (auto* child : {&base.data, &second}) {
      child->evidence.original_request_payload_digest =
          ApplyDelegatedComponentLifecycleSemanticRequestDigest(
              child->intent, child->evidence.candidate_projection_revision,
              child->evidence.candidate_retained_bundle_digest,
              child->evidence.candidate_retained_bundle_bytes,
              child->evidence.original_execution_host.runtime_unit_id,
              child->evidence.original_execution_host.runtime_epoch,
              child->evidence.parent_authority.held_fence_query.manager_runtime_epoch,
              child->evidence.parent_authority.parent);
    }
  }

  ComponentLifecycleCoordinatorApplyBringUpServices Services(const ApplyCoordinatorData& data) {
    auto services = base.Services();
    services.with_original_live_parent =
        [this, expected = data.evidence, intent = data.intent](
            const ComponentLifecycleStatusDTO& status,
            const std::function<Result<void>()>& action) {
          std::lock_guard lock(base.pin_mutex);
          ++base.pin_calls;
          if (!base.live ||
              status.accepted_intent.request_context.request_owner_epoch !=
                  base.coordinator.request_owner_epoch() ||
              status.accepted_intent != intent ||
              status.apply_bring_up != std::optional(expected))
            return Result<void>::Err({"original_host_replaced", "original host unavailable", {}});
          ++base.pin_depth;
          struct Guard {
            ApplyCoordinatorFixture& fixture;
            ~Guard() {
              --fixture.pin_depth;
              ++fixture.finished_pin_actions;
              fixture.pin_changed.notify_all();
            }
          } guard{base};
          return action();
        };
    return services;
  }

  Result<CCLCLifecycleRequestResultDTO> Accept(const ApplyCoordinatorData& data) {
    return base.coordinator.AcceptApplyComponentBringUp(
        data.intent, data.evidence, data.predecessor, {data.catalog}, data.domains,
        data.providers, Services(data));
  }

  Result<CCLCLifecycleRequestResultDTO> Continue(const ApplyCoordinatorData& data) {
    return base.coordinator.ContinueApplyComponentBringUp(
        data.intent, data.evidence.original_request_payload_digest, data.domains,
        data.providers, {{data.providers.front(), pinned_port}}, data.retired, &base.readiness);
  }

  ApplyCoordinatorFixture base;
  ApplyCoordinatorData second;
  std::shared_ptr<FakeBrandPort> shared_port;
  std::shared_ptr<PinnedBrandPort> pinned_port;
};

TEST(ComponentLifecycleCoordinator,
     SharedDomainApplyChildrenRequirePreviousPositiveHoldingUnderTheSameParent) {
  SharedDomainApplyFixture fixture(DispatchMode::kKnownSuccess, true);
  const auto& first = fixture.base.data;
  const auto& second = fixture.second;
  ASSERT_NE(first.intent.controllable_component_id, second.intent.controllable_component_id);
  ASSERT_EQ(first.domains.front().control_domain_id, second.domains.front().control_domain_id);
  ASSERT_EQ(first.domains.front().current_control_bindings,
            second.domains.front().current_control_bindings);
  ASSERT_EQ(first.providers, second.providers);
  ASSERT_NE(first.domains.front().current_resources, second.domains.front().current_resources);
  ASSERT_EQ(first.evidence.parent_authority, second.evidence.parent_authority);
  const auto& parent_scope = first.evidence.parent_authority.held_fence_query.expected_scope;
  ASSERT_TRUE(parent_scope.valid());
  EXPECT_EQ(parent_scope.controllable_component_ids,
            (std::vector<ControllableComponentId>{first.intent.controllable_component_id,
                                                 second.intent.controllable_component_id}));
  EXPECT_EQ(parent_scope.resources,
            (std::vector<ResourceId>{ResourceId("resource.1"), ResourceId("resource.2")}));
  ASSERT_EQ(parent_scope.domains.size(), 1U);
  ASSERT_EQ(parent_scope.bindings.size(), 1U);
  EXPECT_TRUE(first.evidence.parent_authority.parent_claim.intent.scope.resources.empty());
  EXPECT_TRUE(first.evidence.parent_authority.parent_claim.intent.scope.configurations.empty());
  ASSERT_TRUE(first.intent.apply_generation_selection);
  ASSERT_TRUE(second.intent.apply_generation_selection);
  ASSERT_FALSE(first.intent.apply_generation_selection->predecessor_generation);
  ASSERT_FALSE(second.intent.apply_generation_selection->predecessor_generation);
  const auto first_accepted = fixture.Accept(first);
  ASSERT_TRUE(first_accepted.ok()) << first_accepted.error().message;
  const auto second_accepted = fixture.Accept(second);
  ASSERT_TRUE(second_accepted.ok()) << second_accepted.error().message;
  EXPECT_EQ(first.intent.expected_component_lifecycle_generation, ComponentLifecycleGeneration(0U));
  EXPECT_EQ(second.intent.expected_component_lifecycle_generation, ComponentLifecycleGeneration(0U));
  EXPECT_EQ(first_accepted.value().acceptance->component_lifecycle_generation,
            ComponentLifecycleGeneration(1U));
  EXPECT_EQ(second_accepted.value().acceptance->component_lifecycle_generation,
            ComponentLifecycleGeneration(1U));
  ASSERT_LT(first_accepted.value().acceptance->lifecycle_transaction_generation.value(),
            second_accepted.value().acceptance->lifecycle_transaction_generation.value());
  EXPECT_EQ(fixture.shared_port->observation_count(), 0U);
  EXPECT_EQ(fixture.shared_port->dispatch_count(), 0U);
  EXPECT_TRUE(fixture.base.readiness.publications().empty());
  EXPECT_TRUE(fixture.base.reservations.snapshot().empty());
  // Empty-facade acceptance is inert. Installation must supply its actual
  // readiness port before even the first provider observation is scheduled.
  const auto without_readiness = fixture.base.coordinator.ContinueApplyComponentBringUp(
      first.intent, first.evidence.original_request_payload_digest, first.domains,
      first.providers, {{first.providers.front(), fixture.pinned_port}}, first.retired);
  ASSERT_FALSE(without_readiness.ok());
  EXPECT_EQ(without_readiness.error().details.at("reason"), "installed_readiness_port_absent");
  EXPECT_EQ(fixture.shared_port->observation_count(), 0U);
  EXPECT_EQ(fixture.shared_port->dispatch_count(), 0U);
  EXPECT_TRUE(fixture.base.readiness.publications().empty());
  EXPECT_TRUE(fixture.base.reservations.snapshot().empty());
  const auto premature = fixture.Continue(second);
  ASSERT_FALSE(premature.ok());
  EXPECT_EQ(premature.error().details.at("reason"), "previous_apply_child_not_holding");
  EXPECT_EQ(fixture.shared_port->observation_count(), 0U);
  EXPECT_EQ(fixture.shared_port->dispatch_count(), 0U);
  EXPECT_TRUE(fixture.base.reservations.snapshot().empty());

  const auto first_continued = fixture.Continue(first);
  ASSERT_TRUE(first_continued.ok()) << first_continued.error().message;
  ASSERT_TRUE(fixture.base.scheduler.WaitForScheduled(1U, std::chrono::seconds(2)));
  const auto first_holding = fixture.base.journal.LoadRetainedLifecycleStatus(
      first_accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(first_holding.ok() && first_holding.value());
  EXPECT_EQ(first_holding.value()->phase, ComponentLifecycleTransactionPhaseDTO::kVerifying);
  EXPECT_EQ(first_holding.value()->current_state, ComponentLifecycleStateDTO::kPrepared);
  const auto& dispatched = first_holding.value()->domains.front();
  EXPECT_EQ(dispatched.action_state, ComponentLifecycleDomainActionStateDTO::kSucceeded);
  EXPECT_EQ(dispatched.effect, ComponentLifecycleDomainEffectDTO::kEffectObserved);
  EXPECT_EQ(dispatched.current_state, ComponentLifecycleStateDTO::kPrepared);
  ASSERT_EQ(dispatched.primitive_attempts.size(), 1U);
  EXPECT_EQ(dispatched.primitive_attempts.front().dispatch_state,
            LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown);
  EXPECT_EQ(dispatched.primitive_attempts.front().known_result,
            LifecyclePrimitiveKnownResultDTO::kSucceeded);
  // A known-success primitive is verified directly; uncertainty reconciliation is absent.
  EXPECT_FALSE(dispatched.reconciliation_evidence);
  EXPECT_FALSE(first_holding.value()->terminal_result);
  ASSERT_EQ(fixture.base.readiness.publications().size(), 1U);
  EXPECT_EQ(fixture.base.readiness.publications().front().controllable_component_id,
            first.intent.controllable_component_id);
  EXPECT_FALSE(fixture.base.readiness.publications().front().command_eligible);
  EXPECT_TRUE(fixture.base.reservations.snapshot().empty());

  const auto second_continued = fixture.Continue(second);
  ASSERT_TRUE(second_continued.ok()) << second_continued.error().message;
  ASSERT_TRUE(fixture.base.scheduler.WaitForScheduled(2U, std::chrono::seconds(2)));
  const auto second_holding = fixture.base.journal.LoadRetainedLifecycleStatus(
      second_accepted.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(second_holding.ok() && second_holding.value());
  EXPECT_EQ(second_holding.value()->current_state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(second_holding.value()->phase, ComponentLifecycleTransactionPhaseDTO::kVerifying);
  const auto& satisfied = second_holding.value()->domains.front();
  EXPECT_EQ(satisfied.action_state, ComponentLifecycleDomainActionStateDTO::kSucceeded);
  EXPECT_EQ(satisfied.effect, ComponentLifecycleDomainEffectDTO::kKnownNoEffect);
  EXPECT_TRUE(satisfied.primitive_attempts.empty());
  EXPECT_FALSE(satisfied.reconciliation_evidence);
  EXPECT_FALSE(second_holding.value()->terminal_result);
  EXPECT_EQ(fixture.shared_port->observation_count(), 3U);
  EXPECT_EQ(fixture.shared_port->dispatch_count(), 1U);
  ASSERT_EQ(fixture.base.readiness.publications().size(), 2U);
  for (const auto& publication : fixture.base.readiness.publications()) {
    EXPECT_EQ(publication.authority, ComponentReadinessAuthorityDTO::kParentApplyFence);
    EXPECT_FALSE(publication.command_eligible);
    EXPECT_EQ(publication.parent_apply_fence_id,
              first.evidence.parent_authority.held_fence_query.configuration_fence_id);
  }
  EXPECT_EQ(fixture.base.readiness.publications().back().controllable_component_id,
            second.intent.controllable_component_id);
  EXPECT_TRUE(fixture.base.reservations.snapshot().empty());
  EXPECT_EQ(first.publication, second.publication);
  fixture.base.publication_reader.SetHead(fixture.base.Published());
  fixture.base.clock.set(1'100);
  fixture.base.scheduler.AdvanceTo(1'100);
  ASSERT_TRUE(fixture.base.coordinator.DrainFor(std::chrono::seconds(2)));
  for (const auto* child : {&first, &second}) {
    const auto terminal = fixture.base.coordinator.ReadApplyComponentBringUpRequestResult(
        child->intent, child->evidence.original_request_payload_digest);
    ASSERT_TRUE(terminal.ok() && terminal.value() && terminal.value()->status);
    EXPECT_EQ(terminal.value()->status->terminal_result,
              ComponentLifecycleTerminalResultDTO::kSucceeded);
    EXPECT_EQ(terminal.value()->status->current_state,
              ComponentLifecycleStateDTO::kPrepared);
  }
  EXPECT_TRUE(fixture.base.reservations.snapshot().empty());
  EXPECT_EQ(fixture.base.kernel.claim_acquires.load(), 0U);
  EXPECT_EQ(fixture.pinned_port->outside_pin.load(), 0U);
  EXPECT_EQ(fixture.base.readiness.outside_pin.load(), 0U);
  const auto& parent_claim = first.evidence.parent_authority.parent_claim;
  const auto parent = fixture.base.kernel.GetCellAdmissionRecoveryReceipt(
      {parent_claim.intent, parent_claim, std::nullopt});
  ASSERT_TRUE(parent.ok());
  EXPECT_EQ(parent.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
}

TEST(ComponentLifecycleCoordinator, SharedDomainApplyUnknownFirstChildBlocksTheNextChild) {
  SharedDomainApplyFixture fixture(DispatchMode::kUncertain);
  const auto first = fixture.Accept(fixture.base.data);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(fixture.Accept(fixture.second).ok());
  ASSERT_TRUE(fixture.Continue(fixture.base.data).ok());
  ASSERT_TRUE(fixture.base.WaitForPinnedActions(4U));
  const auto unknown = fixture.base.journal.LoadRetainedLifecycleStatus(
      first.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(unknown.ok() && unknown.value());
  EXPECT_EQ(unknown.value()->phase, ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown);
  EXPECT_TRUE(unknown.value()->reconciliation_required);
  EXPECT_FALSE(unknown.value()->terminal_result);
  EXPECT_EQ(fixture.shared_port->observation_count(), 1U);
  EXPECT_EQ(fixture.shared_port->dispatch_count(), 1U);
  EXPECT_TRUE(fixture.base.readiness.publications().empty());
  EXPECT_FALSE(fixture.Continue(fixture.second).ok());
  EXPECT_EQ(fixture.shared_port->observation_count(), 1U);
  EXPECT_EQ(fixture.shared_port->dispatch_count(), 1U);
  EXPECT_TRUE(fixture.base.reservations.snapshot().empty());
  const auto& parent_claim = fixture.base.data.evidence.parent_authority.parent_claim;
  const auto parent = fixture.base.kernel.GetCellAdmissionRecoveryReceipt(
      {parent_claim.intent, parent_claim, std::nullopt});
  ASSERT_TRUE(parent.ok());
  EXPECT_EQ(parent.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
}

TEST(ComponentLifecycleCoordinator,
     SharedDomainApplyRestartRestoresObservationOnlyWithoutChildReservations) {
  for (const auto phase : {ComponentLifecycleTransactionPhaseDTO::kAccepted,
                           ComponentLifecycleTransactionPhaseDTO::kVerifying,
                           ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown}) {
    SCOPED_TRACE(static_cast<int>(phase));
    SharedDomainApplyFixture fixture(
        phase == ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown
            ? DispatchMode::kUncertain : DispatchMode::kKnownSuccess);
    const auto first = fixture.Accept(fixture.base.data);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(fixture.Accept(fixture.second).ok());
    if (phase != ComponentLifecycleTransactionPhaseDTO::kAccepted) {
      ASSERT_TRUE(fixture.Continue(fixture.base.data).ok());
      if (phase == ComponentLifecycleTransactionPhaseDTO::kVerifying) {
        ASSERT_TRUE(fixture.base.scheduler.WaitForScheduled(1U, std::chrono::seconds(2)));
        ASSERT_TRUE(fixture.base.WaitForPinnedActions(4U));
      } else {
        ASSERT_TRUE(fixture.base.WaitForPinnedActions(4U));
      }
    }
    const auto before = fixture.base.journal.LoadRetainedLifecycleStatus(
        first.value().acceptance->lifecycle_transaction_id);
    ASSERT_TRUE(before.ok() && before.value());
    ASSERT_EQ(before.value()->phase, phase);
    if (phase == ComponentLifecycleTransactionPhaseDTO::kVerifying) {
      EXPECT_EQ(before.value()->current_state, ComponentLifecycleStateDTO::kPrepared);
      ASSERT_EQ(fixture.base.readiness.publications().size(), 1U);
      EXPECT_FALSE(fixture.base.readiness.publications().front().command_eligible);
    }
    const auto observations_before = fixture.shared_port->observation_count();
    const auto dispatches_before = fixture.shared_port->dispatch_count();
    const auto publications_before = fixture.base.readiness.publications().size();
    const auto readiness_calls_before = fixture.base.readiness.calls();
    const auto child_claims_before = fixture.base.kernel.claim_acquires.load();
    EXPECT_TRUE(fixture.base.reservations.snapshot().empty());
    fixture.base.coordinator.CloseAdmission();
    CCLCDomainReservationTable replacement_reservations;
    auto config = Config();
    config.request_owner_epoch = RequestOwnerEpoch("replacement.shared.seed");
    ComponentLifecycleCoordinator replacement(
        config, fixture.base.journal, replacement_reservations, fixture.base.snapshots,
        fixture.base.catalogs, fixture.base.resolver, fixture.base.kernel, fixture.base.clock,
        fixture.base.scheduler, std::nullopt, &fixture.base.readiness);
    ASSERT_TRUE(replacement.Start().ok());
    // Recovery rows stay unresolved, but the existing drain waits for the
    // asynchronous recovery workers before refusing those active rows.
    EXPECT_FALSE(replacement.DrainFor(std::chrono::seconds(2)));
    EXPECT_TRUE(replacement_reservations.snapshot().empty());
    for (const auto* child : {&fixture.base.data, &fixture.second}) {
      EXPECT_FALSE(replacement.ContinueApplyComponentBringUp(
          child->intent, child->evidence.original_request_payload_digest, child->domains,
          child->providers, {{child->providers.front(), fixture.pinned_port}}, {}).ok());
      auto services = fixture.Services(*child);
      services.with_original_live_parent = {};
      const auto replay = replacement.AcceptApplyComponentBringUp(
          child->intent, child->evidence, child->predecessor, {child->catalog}, child->domains,
          child->providers, std::move(services));
      ASSERT_TRUE(replay.ok()) << replay.error().message;
      EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
      ASSERT_TRUE(replay.value().status);
      EXPECT_EQ(replay.value().status->status_owner_epoch,
                replacement.request_owner_epoch());
      EXPECT_EQ(replay.value().status->phase,
                ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown);
      EXPECT_TRUE(replay.value().status->reconciliation_required);
    }
    EXPECT_EQ(fixture.shared_port->observation_count(), observations_before);
    EXPECT_EQ(fixture.shared_port->dispatch_count(), dispatches_before);
    EXPECT_EQ(fixture.base.readiness.publications().size(), publications_before);
    EXPECT_EQ(fixture.base.readiness.calls(), readiness_calls_before);
    EXPECT_EQ(fixture.base.kernel.claim_acquires.load(), child_claims_before);
    EXPECT_TRUE(replacement_reservations.snapshot().empty());
    const auto& parent_claim = fixture.base.data.evidence.parent_authority.parent_claim;
    const auto parent = fixture.base.kernel.GetCellAdmissionRecoveryReceipt(
        {parent_claim.intent, parent_claim, std::nullopt});
    ASSERT_TRUE(parent.ok());
    EXPECT_EQ(parent.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
    EXPECT_EQ(parent.value().matched_claim, std::optional(parent_claim));
  }
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
  const auto complete =
      coordinator.TryExchangeProductContext(old_context, new_context, owner, previous);
  ASSERT_TRUE(complete.ok()) << complete.error().message;
  EXPECT_EQ(complete.value().phase, ComponentLifecycleProductExchangePhase::kCompleted);
  EXPECT_EQ(port->dispatch_count(), 1U);
  EXPECT_EQ(new_snapshots.read_count(), 0U);
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
  ASSERT_TRUE(scheduler.WaitForScheduled(1U, std::chrono::seconds(2)));
  const auto pending =
      coordinator.TryExchangeProductContext(old_context, new_context, owner, previous);
  ASSERT_TRUE(pending.ok());
  EXPECT_EQ(pending.value().phase, ComponentLifecycleProductExchangePhase::kPending);
  EXPECT_EQ(scheduler.cancel_count(), 0U);
  EXPECT_EQ(scheduler.pending_count(SchedulerPriority::Lifecycle), 1U);
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
      EXPECT_EQ(terminal.value()->terminal_result,
                ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect);
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
