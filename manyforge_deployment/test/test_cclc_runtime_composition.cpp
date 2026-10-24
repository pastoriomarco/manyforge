#include <gtest/gtest.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/build_owned_lifecycle_plan_catalog_source.hpp>
#include <manyforge_core/component_lifecycle_admission.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_deployment/cclc_runtime_composition.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "../../manyforge_core/src/component_lifecycle_journal_codec_internal.hpp"
#include "../../manyforge_core/test/cclc_test_admission_port.hpp"
#include "canonical_empty_admission_internal.hpp"
#include "io_retained_fixture.hpp"
#include "lite6_retained_consumer_fixture.hpp"

namespace manyforge::deployment {
namespace {

class ScopedTrustedRoot final {
public:
  ScopedTrustedRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("manyforge-cclc-composition-" +
               std::to_string(static_cast<long long>(::getpid())) + "-" +
               std::to_string(sequence_.fetch_add(1U)))) {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
    if (!std::filesystem::create_directory(path_, error) || error) {
      throw std::runtime_error("cannot create CCLC composition test root");
    }
  }

  ~ScopedTrustedRoot() {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  inline static std::atomic<std::uint64_t> sequence_{0U};
  std::filesystem::path path_;
};

[[nodiscard]] core::ErrorDTO unsupported() {
  return {"test_port_unused", "the composition test port was not invoked", {}};
}

class ActivePublicationReader final
    : public core::IActiveBundlePublicationReader {
public:
  explicit ActivePublicationReader(
      core::ActiveBundlePublicationReceiptDTO receipt)
      : receipt_(std::move(receipt)) {}

  [[nodiscard]] core::Result<core::ActiveBundlePublicationReceiptDTO>
  ReadActiveBundlePublication(
      core::ActiveBundlePublicationGeneration generation) const override {
    if (fail_reads_.load() || generation != receipt_.publication_generation) {
      return core::Result<core::ActiveBundlePublicationReceiptDTO>::Err(
          unsupported());
    }
    return core::Result<core::ActiveBundlePublicationReceiptDTO>::Ok(receipt_);
  }

  void FailReads() { fail_reads_.store(true); }

private:
  core::ActiveBundlePublicationReceiptDTO receipt_;
  std::atomic<bool> fail_reads_{false};
};

// Same exact retained-binding/catalog construction as the verified snapshot
// source fixture; the read-only source never discovers a provider at runtime.
[[nodiscard]] core::BrandLifecyclePlanCatalogDTO
catalog_for(const core::ControlBindingDTO &binding) {
  const core::LifecyclePredicateDTO declared{
      {core::ComponentLifecycleStateDTO::kDeclared}, {}};
  const core::LifecyclePredicateDTO prepared{
      {core::ComponentLifecycleStateDTO::kPrepared}, {}};
  auto result = core::MakeBrandLifecyclePlanCatalog(
      binding.provider_manifest_id, binding.provider_manifest_revision,
      {{core::ComponentLifecycleIntentKindDTO::kPrepare,
        core::ComponentLifecycleStateDTO::kPrepared, std::nullopt, prepared,
        30'000'000'000LL,
        {{core::LifecyclePrimitiveId("prepare"), declared, prepared}}}});
  if (!result.ok()) {
    throw std::logic_error("test could not construct lifecycle catalog: " +
                           result.error().message);
  }
  return std::move(result.value());
}

class CatalogSource final : public core::ICCLCLifecyclePlanCatalogSource {
public:
  explicit CatalogSource(core::BrandLifecyclePlanCatalogDTO catalog)
      : catalog_(std::move(catalog)) {}

  [[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
  LoadExactLifecyclePlanCatalog(
      const core::ProviderManifestId &manifest,
      const core::ProviderManifestRevision &revision,
      const core::ContentDigest &digest) const override {
    if (manifest != catalog_.provider_manifest_id ||
        revision != catalog_.provider_manifest_revision ||
        digest != catalog_.catalog_digest) {
      return core::Result<core::BrandLifecyclePlanCatalogDTO>::Err(
          {"catalog_key_mismatch", "test source requires the exact catalog", {}});
    }
    return core::Result<core::BrandLifecyclePlanCatalogDTO>::Ok(catalog_);
  }

private:
  core::BrandLifecyclePlanCatalogDTO catalog_;
};

class UnusedKernelPorts : public core::ICCLCKernelRouteConfigurationFencePort,
                          public core::IKernelCellAdmissionPort {
public:
  [[nodiscard]] core::AuthoringAdmissionDTO GetAuthoringAdmission()
      const override {
    return {true, core::AdmissionGeneration(1U)};
  }
  [[nodiscard]] core::Result<core::AuthoringAdmissionDTO>
  UpdateCellAdmissionReadiness(
      const core::CellAdmissionReadinessSnapshotDTO &,
      std::shared_ptr<core::IIoSignalService>* actual_retired_service = nullptr) override {
    if (actual_retired_service) actual_retired_service->reset();
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        GetAuthoringAdmission());
  }
  [[nodiscard]] core::Result<core::CellAdmissionClaimDTO>
  AcquireCellAdmissionClaim(
      const core::CellAdmissionClaimIntentDTO &) override {
    return core::Result<core::CellAdmissionClaimDTO>::Err(unsupported());
  }
  [[nodiscard]] core::Result<core::AuthoringAdmissionDTO>
  ReleaseCellAdmissionClaim(
      const core::CellAdmissionClaimReleaseDTO &) override {
    return core::Result<core::AuthoringAdmissionDTO>::Err(unsupported());
  }
  [[nodiscard]] core::Result<core::AuthoringAdmissionDTO>
  ReconcileCellAdmissionClaims(
      const core::CellAdmissionReconcileDTO &) override {
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        GetAuthoringAdmission());
  }
  [[nodiscard]] core::Result<core::CellAdmissionRecoveryReceiptDTO>
  GetCellAdmissionRecoveryReceipt(
      const core::CellAdmissionRecoveryReceiptQueryDTO &) const override {
    return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Ok(
        {core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown,
         core::RequestOwnerEpoch("kernel.1"),
         core::AdmissionGeneration(1U),
         std::nullopt,
         std::nullopt,
         std::nullopt,
         {},
         {}});
  }

  [[nodiscard]]
  core::Result<core::RouteConfigurationFenceAdmissionSnapshotDTO>
  ObserveRouteConfigurationFenceAdmission(
      const core::RouteConfigurationFenceAdmissionQueryDTO &) const override {
    return core::Result<core::RouteConfigurationFenceAdmissionSnapshotDTO>::Err(
        unsupported());
  }

  [[nodiscard]] core::Result<core::RouteConfigurationFenceAcceptedDTO>
  AcquireRouteConfigurationFence(
      const core::RouteConfigurationFenceIntentDTO &) override {
    return core::Result<core::RouteConfigurationFenceAcceptedDTO>::Err(
        unsupported());
  }

  [[nodiscard]]
  core::Result<core::RouteConfigurationFenceAcquireAttemptReconciliationDTO>
  ReconcileRouteConfigurationFenceAcquireAttempt(
      const core::RouteConfigurationFenceIntentDTO &) const override {
    return core::Result<
        core::RouteConfigurationFenceAcquireAttemptReconciliationDTO>::
        Err(unsupported());
  }

  [[nodiscard]] core::Result<core::RouteConfigurationFenceStatusDTO>
  ObserveRouteConfigurationFenceStatus(
      const core::RouteConfigurationFenceStatusQueryDTO &) const override {
    return core::Result<core::RouteConfigurationFenceStatusDTO>::Err(
        unsupported());
  }

  [[nodiscard]] core::Result<core::RouteConfigurationFenceReleasedDTO>
  ReleaseRouteConfigurationFence(
      const core::RouteConfigurationFenceReleaseRequestDTO &) override {
    return core::Result<core::RouteConfigurationFenceReleasedDTO>::Err(
        unsupported());
  }

  [[nodiscard]]
  core::Result<core::RouteConfigurationFenceReleaseAttemptReconciliationDTO>
  ReconcileRouteConfigurationFenceReleaseAttempt(
      const core::RouteConfigurationFenceReleaseRequestDTO &) const override {
    return core::Result<
        core::RouteConfigurationFenceReleaseAttemptReconciliationDTO>::
        Err(unsupported());
  }
};

// The two barriers separate the actual recovered claim release from its
// final successful owner reconciliation, without holding a fixture lock over
// either delegated Kernel operation.
class BlockingSuccessfulReconciliationPorts final : public UnusedKernelPorts {
public:
  void RetainClaim(core::CellAdmissionClaimDTO claim) {
    cell_.RetainClaim(std::move(claim));
  }
  core::Result<core::CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const core::CellAdmissionClaimIntentDTO &intent) override {
    ++acquire_count_;
    return cell_.AcquireCellAdmissionClaim(intent);
  }
  core::Result<core::AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const core::CellAdmissionClaimReleaseDTO &release) override {
    {
      std::unique_lock lock(mutex_);
      release_blocked_ = true;
      changed_.notify_all();
      changed_.wait(lock, [&] { return release_allowed_; });
    }
    return cell_.ReleaseCellAdmissionClaim(release);
  }
  core::Result<core::AuthoringAdmissionDTO> ReconcileCellAdmissionClaims(
      const core::CellAdmissionReconcileDTO &reconcile) override {
    ++reconcile_count_;
    return cell_.ReconcileCellAdmissionClaims(reconcile);
  }
  core::Result<core::CellAdmissionRecoveryReceiptDTO>
  GetCellAdmissionRecoveryReceipt(
      const core::CellAdmissionRecoveryReceiptQueryDTO &query) const override {
    if (query.intent.owner_transaction_id == "cclc-owner-epoch-probe") {
      std::unique_lock lock(mutex_);
      probe_blocked_ = true;
      changed_.notify_all();
      changed_.wait(lock, [&] { return probe_allowed_; });
    }
    return cell_.GetCellAdmissionRecoveryReceipt(query);
  }
  bool WaitForRelease(std::chrono::nanoseconds budget) const {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, budget, [&] { return release_blocked_; });
  }
  bool WaitForProbe(std::chrono::nanoseconds budget) const {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, budget, [&] { return probe_blocked_; });
  }
  void AllowRelease() {
    {
      std::lock_guard lock(mutex_);
      release_allowed_ = true;
    }
    changed_.notify_all();
  }
  void AllowProbe() {
    {
      std::lock_guard lock(mutex_);
      probe_allowed_ = true;
    }
    changed_.notify_all();
  }
  std::size_t acquire_count() const { return acquire_count_.load(); }
  std::size_t reconcile_count() const { return reconcile_count_.load(); }

private:
  core::test::FakeCellAdmissionPort cell_;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  bool release_allowed_{};
  bool probe_allowed_{};
  mutable bool release_blocked_{};
  mutable bool probe_blocked_{};
  std::atomic<std::size_t> acquire_count_{};
  std::atomic<std::size_t> reconcile_count_{};
};

class UnusedKernelPublicationOwner final
    : public IRouteConfigurationKernelPublicationOwner {
public:
  [[nodiscard]] core::Result<std::vector<core::RuntimeParticipantBinding>>
  ReadCurrentPhysicalParticipants(
      const core::ActiveControlConfigurationDTO &) const override {
    return core::Result<std::vector<core::RuntimeParticipantBinding>>::Err(
        unsupported());
  }

  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  PublishAndReadBackRouteConfiguration(
      const core::ActiveControlConfigurationDTO &,
      std::vector<VerifiedRouteReadinessParticipant>) override {
    return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
        unsupported());
  }
};

[[nodiscard]] core::ActiveBundlePublicationReceiptDTO
publication(const RetainedBundle &bundle) {
  const std::string scene_digest =
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const core::ProjectionRevisionDTO revision{
      bundle.retained_bundle_id(),
      bundle.value().cell_graph.graph_revision,
      {core::SceneId(scene_digest), 1U, core::ContentDigest(scene_digest)}};
  return {{core::BootstrapPublicationTransactionDTO{
               core::BootstrapId("bootstrap.cclc-composition"),
               core::BootstrapGeneration(1U)},
           revision, revision.retained_bundle_id, revision.graph_revision,
           std::nullopt, core::ContentDigest("private-materialization")},
          core::ActiveBundlePublicationGeneration(1U),
          core::ContentDigest("active-public-pointer")};
}

[[nodiscard]] CCLCRuntimeCompositionConfig
configuration(const std::filesystem::path &root, const core::CellId &cell_id,
              std::string owner_suffix) {
  const auto state_root = root / "state";
  const auto run_root = root / "run";
  std::filesystem::create_directory(state_root);
  std::filesystem::create_directory(run_root);
  return {state_root,
          run_root,
          cell_id,
          {core::RequestOwnerEpoch("cclc-owner." + owner_suffix), 128U, 1U,
           1'000'000LL, 30'000'000'000LL}};
}

[[nodiscard]] std::vector<ControllerLifecycleSourceOccurrenceDTO>
controller_sources(const RetainedBundle &bundle) {
  std::vector<ControllerLifecycleSourceOccurrenceDTO> result;
  for (const auto &domain : bundle.value().cell_graph.control_domains) {
    if (domain.runtime_unit_ids.empty()) {
      throw std::runtime_error("test control domain has no runtime source");
    }
    result.push_back({domain.control_domain_id, domain.runtime_unit_ids.front(),
                      core::RuntimeEpoch("runtime-epoch.test.1")});
  }
  return result;
}

[[nodiscard]] core::CellObserverIncidentRecordDTO
ObserverLossRecord(const core::CellId &cell_id) {
  using manyforge::content_identity::CanonicalValue;
  const core::CellObserverIncidentTargetDTO target{cell_id, "observer.source", "manager-observer"};
  return {"observer." + std::string(64U, 'a'), target, 0, "observer exited with signal 11",
          CanonicalValue(CanonicalValue::Object{
              {"exit", CanonicalValue(CanonicalValue::Object{
                           {"status", CanonicalValue(nullptr)},
                           {"signal", CanonicalValue(11.0)},
                           {"core_dumped", CanonicalValue(false)},
                           {"observed_utc", CanonicalValue("2026-10-12T14:00:00Z")}})},
              {"context", CanonicalValue(CanonicalValue::Object{
                              {"cell_id", CanonicalValue(target.cell_id.value())},
                              {"source_id", CanonicalValue(target.source_id)},
                              {"source_epoch_base", CanonicalValue(target.source_epoch_base)}})},
              {"settings", CanonicalValue(CanonicalValue::Object{})},
              {"generated", CanonicalValue(CanonicalValue::Object{})},
              {"output", CanonicalValue(CanonicalValue::Object{
                             {"tail", CanonicalValue("fatal observer\n")},
                             {"line_count", CanonicalValue(1.0)},
                             {"truncated", CanonicalValue(false)},
                             {"dropped_forward_lines", CanonicalValue(0.0)},
                             {"dropped_forward_bytes", CanonicalValue(0.0)},
                             {"invalid_text_bytes", CanonicalValue(0.0)}})},
              {"core_dump_link", CanonicalValue(nullptr)}})};
}

[[nodiscard]] std::string Exchange(const std::filesystem::path &socket_path,
                                   const std::string &request) {
  const int descriptor = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (descriptor < 0) {
    throw std::runtime_error("cannot create CCLC test client");
  }
  const auto close_descriptor = [&] { static_cast<void>(::close(descriptor)); };
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto spelling = socket_path.string();
  if (spelling.size() >= sizeof(address.sun_path)) {
    close_descriptor();
    throw std::runtime_error("CCLC test socket path is too long");
  }
  std::memcpy(address.sun_path, spelling.c_str(), spelling.size() + 1U);
  if (::connect(descriptor, reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) != 0 ||
      ::send(descriptor, request.data(), request.size(), MSG_NOSIGNAL) !=
          static_cast<ssize_t>(request.size())) {
    close_descriptor();
    throw std::runtime_error("cannot submit CCLC test request");
  }
  std::vector<char> response(core::kMaximumCCLCOpenResponseBytes);
  const auto count = ::recv(descriptor, response.data(), response.size(), 0);
  close_descriptor();
  if (count <= 0) {
    throw std::runtime_error("CCLC test response is absent");
  }
  return {response.data(), static_cast<std::size_t>(count)};
}


TEST(CCLCRuntimeComposition,
     StartsGenuineCanonicalEmptyCellWithoutControllerSources) {
  ScopedTrustedRoot root;
  const auto package = root.path() / "canonical-empty";
  ASSERT_TRUE(std::filesystem::create_directory(package));
  const auto generated = detail::generate_canonical_empty_package(package);
  const FilesystemCanonicalEmptyRetainedBundleSource admission_source(package);
  const auto admission = admission_source.load_verified();
  EXPECT_EQ(admission.admission_digest(), generated.admission_digest());
  const FilesystemVerifiedRetainedBundleSource bundle_source(package);
  const auto bundle = bundle_source.get_verified(admission.retained_bundle_id());
  ASSERT_NE(bundle, nullptr);
  EXPECT_EQ(bundle->canonical_bytes(), admission.retained_bundle().canonical_bytes());
  const auto &graph = bundle->value().cell_graph;
  ASSERT_TRUE(graph.controllable_components.empty());
  ASSERT_TRUE(graph.control_domains.empty());
  ASSERT_TRUE(graph.control_bindings.empty());
  ASSERT_TRUE(bundle->value().desired_control_configurations.empty());
  const auto receipt = publication(*bundle);
  ActivePublicationReader publication_reader(receipt);
  FilesystemVerifiedRetainedProductArtifactSource artifacts(package);
  auto catalogs = core::BuildOwnedLifecyclePlanCatalogSource::Make({});
  ASSERT_TRUE(catalogs.ok()) << catalogs.error().message;
  UnusedKernelPorts kernel;
  UnusedKernelPublicationOwner publication_owner;
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto composition = CCLCRuntimeComposition::Create(
      configuration(root.path(), graph.cell_id, "empty"), bundle,
      publication_reader, receipt, artifacts, catalogs.value(), {}, {}, {}, {},
      {}, kernel, kernel, publication_owner, clock, scheduler);
  ASSERT_TRUE(composition.ok()) << composition.error().message;
  EXPECT_FALSE(composition.value()->coordinator().accepting_new_requests());
  const auto started = composition.value()->Start();
  ASSERT_TRUE(started.ok()) << started.error().message;
  EXPECT_TRUE(composition.value()->coordinator().accepting_new_requests());
  using manyforge::content_identity::CanonicalValue;
  const auto response = manyforge::content_identity::parse_canonical_json(
      Exchange(composition.value()->open_request_socket_path(),
               manyforge::content_identity::canonical_json(CanonicalValue(
                   CanonicalValue::Object{
                       {"api_version", CanonicalValue(std::string(core::kCCLCOpenRequestApiVersion))},
                       {"command", CanonicalValue("owner_identity")},
                       {"payload", CanonicalValue(CanonicalValue::Object{})}}))));
  const auto *ok = response.find_object_member("ok");
  ASSERT_NE(ok, nullptr);
  ASSERT_TRUE(ok->as_bool());
  const auto *result = response.find_object_member("result");
  ASSERT_NE(result, nullptr);
  const auto *owner = result->find_object_member("request_owner_epoch");
  ASSERT_NE(owner, nullptr);
  EXPECT_EQ(owner->as_string(),
            composition.value()->coordinator().request_owner_epoch().value());
}

TEST(CCLCRuntimeComposition,
     RejectsPopulatedRetainedCellWithoutControllerSources) {
  ScopedTrustedRoot root;
  const auto retained = test::materialize_lite6_retained_consumer_fixture(root.path());
  const auto &graph = retained.bundle->value().cell_graph;
  ASSERT_FALSE(graph.controllable_components.empty());
  ASSERT_FALSE(graph.control_domains.empty());
  ASSERT_FALSE(graph.control_bindings.empty());
  ASSERT_FALSE(retained.bundle->value().desired_control_configurations.empty());
  const auto &binding = graph.control_bindings.front();
  const auto domain = std::find_if(graph.control_domains.begin(), graph.control_domains.end(),
      [&](const auto &row) { return row.control_domain_id == binding.control_domain_id; });
  ASSERT_NE(domain, graph.control_domains.end());
  const auto catalog = catalog_for(binding);
  CatalogSource catalogs(catalog);
  const std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> providers{
      {domain->control_domain_id, domain->control_domain_revision,
       binding.provider_instance_id, binding.provider_manifest_id,
       binding.provider_manifest_revision, binding.provider_runtime_unit_id,
       core::RuntimeEpoch("runtime-epoch.test.1"), catalog.catalog_digest}};
  const auto receipt = publication(*retained.bundle);
  ActivePublicationReader publication_reader(receipt);
  FilesystemVerifiedRetainedProductArtifactSource artifacts(root.path());
  UnusedKernelPorts kernel;
  UnusedKernelPublicationOwner publication_owner;
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto composition = CCLCRuntimeComposition::Create(
      configuration(root.path(), graph.cell_id, "missing-sources"), retained.bundle,
      publication_reader, receipt, artifacts, catalogs, providers, {}, {}, {},
      {}, kernel, kernel, publication_owner, clock, scheduler);
  ASSERT_FALSE(composition.ok());
  EXPECT_EQ(composition.error().message,
            "route current-snapshot controller-source inventory is invalid");
}

TEST(CCLCRuntimeComposition,
     ProviderOnlyRouteSourceHasNoControllerTemplatesAndRejectsPhysicalRoute) {
  ScopedTrustedRoot root;
  const auto retained =
      test::materialize_boolean_io_retained_fixture(root.path());
  const auto &graph = retained.bundle->value().cell_graph;
  ASSERT_FALSE(graph.control_domains.empty());
  ASSERT_TRUE(std::all_of(
      graph.control_domains.begin(), graph.control_domains.end(),
      [](const auto &domain) { return domain.provider() != nullptr; }));
  ASSERT_TRUE(graph.control_bindings.empty());
  ASSERT_TRUE(retained.bundle->value().desired_control_configurations.empty());
  const auto receipt = publication(*retained.bundle);
  ActivePublicationReader reader(receipt);
  FilesystemVerifiedRetainedProductArtifactSource artifacts(root.path());
  core::SqliteCCLCJournal journal(root.path() / "route-source.sqlite",
                                  graph.cell_id);
  VerifiedRouteSelectionCurrentSnapshotSource source(
      *retained.bundle, reader, receipt, journal, artifacts, {});
  const auto templates = source.ReadCurrentRouteSelectionTemplates(
      graph.controllable_components.front().controllable_component_id);
  ASSERT_TRUE(templates.ok()) << templates.error().message;
  EXPECT_TRUE(templates.value().templates.empty());

  // A real qualified physical product supplies this desired row. Its presence
  // cannot turn the provider-only source into a controller/route authority.
  const auto physical_root = root.path() / "physical";
  ASSERT_TRUE(std::filesystem::create_directory(physical_root));
  const auto physical =
      test::materialize_lite6_retained_consumer_fixture(physical_root);
  ASSERT_FALSE(physical.bundle->value().desired_control_configurations.empty());
  const auto denied = source.ReadCurrentRouteSelectionSnapshot(
      retained.bundle->retained_bundle_id(), graph.graph_revision,
      physical.bundle->value().desired_control_configurations.front());
  ASSERT_FALSE(denied.ok());
  EXPECT_EQ(denied.error().details.at("reason"), "desired_domain_not_retained");
}

TEST(CCLCRuntimeComposition,
     OwnsAndStartsTheCompleteDurableDependencySetWithoutDiscovery) {
  ScopedTrustedRoot root;
  const auto retained =
      test::materialize_lite6_retained_consumer_fixture(root.path());
  const auto receipt = publication(*retained.bundle);
  ActivePublicationReader publication_reader(receipt);
  FilesystemVerifiedRetainedProductArtifactSource artifact_source(root.path());
  const auto &graph = retained.bundle->value().cell_graph;
  ASSERT_EQ(graph.controllable_components.size(), 1U);
  const auto binding = std::find_if(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [&](const auto &candidate) {
        return candidate.controllable_component_id ==
               graph.controllable_components.front().controllable_component_id;
      });
  ASSERT_NE(binding, graph.control_bindings.end());
  const auto domain = std::find_if(
      graph.control_domains.begin(), graph.control_domains.end(),
      [&](const auto &candidate) {
        return candidate.control_domain_id == binding->control_domain_id;
      });
  ASSERT_NE(domain, graph.control_domains.end());
  const auto catalog = catalog_for(*binding);
  ASSERT_TRUE(core::VerifyBrandLifecyclePlanCatalog(catalog).ok());
  CatalogSource catalogs(catalog);
  const std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> providers{
      {domain->control_domain_id, domain->control_domain_revision,
       binding->provider_instance_id, binding->provider_manifest_id,
       binding->provider_manifest_revision, binding->provider_runtime_unit_id,
       core::RuntimeEpoch("runtime-epoch.test.1"), catalog.catalog_digest}};
  UnusedKernelPorts kernel_ports;
  UnusedKernelPublicationOwner publication_owner;
  core::FakeClock clock;
  core::ManualScheduler scheduler;

  auto config = configuration(root.path(), retained.bundle->value().cell_graph.cell_id, "1");
  const auto component_id = retained.bundle->value()
                                .cell_graph.controllable_components.front()
                                .controllable_component_id;
  const core::ExpectedActiveControlConfigurationDTO absent_configuration{
      core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
      std::nullopt, std::nullopt};
  const core::ExpectedControlDomainRevisionDTO historical_domain{
      core::ControlDomainId("historical.domain"),
      core::ControlDomainRevision("historical.domain.revision"),
      core::ControllerTopologyId("historical.topology"),
      core::TopologyRevision("historical.topology.revision"),
      absent_configuration,
      {{core::ControlBindingId("historical.binding"),
        core::ControlBindingRevision("historical.binding.revision"),
        core::ControlRouteId("historical.route")}}};
  const core::ErrorDTO rejection{"historical_request_refused", "no effects were started",
                                  {{"effect", "not_started"}}};
  std::optional<core::ComponentLifecycleIntentDTO> historical_request;
  {
    core::SqliteCCLCJournal journal(
        config.state_root / std::string(core::kCCLCJournalFilename), config.cell_id,
        config.exact_result_history_capacity, config.terminal_result_bytes_capacity);
    const auto original = journal.BeginOwnerEpoch(
        core::RequestOwnerEpoch("historical.seed"), clock.now(),
        config.coordinator.request_membership_capacity);
    ASSERT_TRUE(original.ok()) << original.error().message;
    config.coordinator.quiescent_predecessor = original.value().request_owner_epoch;
    historical_request = core::ComponentLifecycleIntentDTO{
        {core::RequestId("historical.request"), original.value().request_owner_epoch,
         "test", std::nullopt},
        component_id, core::ComponentLifecycleIntentKindDTO::kPrepare,
        retained.bundle->value().cell_graph.graph_revision,
        core::ComponentLifecycleGeneration(1U), {historical_domain}, std::nullopt,
        {core::RequestTransportDTO::kInternal, core::RequestAssuranceDTO::kUnverified,
         std::nullopt},
        core::AdmissionGeneration(1U), std::nullopt};
    ASSERT_TRUE(historical_request->locally_valid());
    ASSERT_TRUE(journal.RecordLifecycleRejection(original.value().request_owner_epoch,
                                               *historical_request, rejection, std::nullopt).ok());
  }

  auto composition = CCLCRuntimeComposition::Create(
      config,
      retained.bundle, publication_reader, receipt, artifact_source,
      catalogs, providers, controller_sources(*retained.bundle), {}, {}, {},
      kernel_ports, kernel_ports, publication_owner, clock,
      scheduler);
  ASSERT_TRUE(composition.ok()) << composition.error().message;
  EXPECT_FALSE(composition.value()->coordinator().accepting_new_requests());
  const auto observer = ObserverLossRecord(config.cell_id);
  ASSERT_TRUE(observer.locally_valid());
  const auto unstarted_owner = composition.value()->coordinator().request_owner_epoch();
  const auto absent_observer = composition.value()->LoadOpenCellObserverIncident();
  ASSERT_TRUE(absent_observer.ok()) << absent_observer.error().message;
  EXPECT_FALSE(absent_observer.value());
  const auto stored_observer = composition.value()->RecordCellObserverIncident(observer);
  ASSERT_TRUE(stored_observer.ok()) << stored_observer.error().message;
  EXPECT_EQ(stored_observer.value(), observer);
  const auto open_observer = composition.value()->LoadOpenCellObserverIncident();
  ASSERT_TRUE(open_observer.ok()) << open_observer.error().message;
  ASSERT_TRUE(open_observer.value());
  EXPECT_EQ(*open_observer.value(), stored_observer.value());
  EXPECT_EQ(composition.value()->coordinator().request_owner_epoch(), unstarted_owner);
  EXPECT_FALSE(composition.value()->coordinator().accepting_new_requests());
  EXPECT_FALSE(composition.value()->coordinator().admission_fault_closed());
  auto conflicting_observer = observer;
  conflicting_observer.summary = "conflicting exit evidence for the same occurrence";
  const auto observer_conflict =
      composition.value()->RecordCellObserverIncident(conflicting_observer);
  ASSERT_FALSE(observer_conflict.ok());
  EXPECT_EQ(observer_conflict.error().code, "component_lifecycle_incident_conflict");
  const auto before_start_observer = composition.value()->LoadOpenCellObserverIncident();
  ASSERT_TRUE(before_start_observer.ok());
  ASSERT_TRUE(before_start_observer.value());
  EXPECT_EQ(*before_start_observer.value(), stored_observer.value());
  const auto run_root = root.path() / "run";
  EXPECT_FALSE(std::filesystem::exists(
      run_root / std::string(kCCLCOpenRequestSocketFilename)));
  EXPECT_FALSE(std::filesystem::exists(
      run_root / std::string(kCCLCOpenRequestRosSocketFilename)));
  EXPECT_THROW((void)composition.value()->open_request_socket_path(),
               std::logic_error);
  EXPECT_THROW((void)composition.value()->ros_open_request_socket_path(),
               std::logic_error);
  ASSERT_TRUE(composition.value()->Start().ok());
  const auto first_owner =
      composition.value()->coordinator().request_owner_epoch();
  const auto generation =
      composition.value()->ReadApplyComponentGenerationSelection(component_id);
  ASSERT_TRUE(generation.ok()) << generation.error().message;
  EXPECT_FALSE(generation.value().predecessor_generation);
  ASSERT_TRUE(generation.value().owner_qualified_predecessor);
  const auto actual_predecessor = composition.value()->ReadCurrentComponentSnapshot(component_id);
  ASSERT_TRUE(actual_predecessor.ok()) << actual_predecessor.error().message;
  EXPECT_EQ(actual_predecessor.value().component_lifecycle_generation,
            core::ComponentLifecycleGeneration(1U));
  EXPECT_EQ(actual_predecessor.value().component_lifecycle_state,
            core::ComponentLifecycleStateDTO::kDeclared);
  EXPECT_TRUE(core::VerifyComponentLifecycleCurrentSnapshot(actual_predecessor.value()).ok());
  EXPECT_EQ(generation.value().owner_qualified_predecessor->generation,
            actual_predecessor.value().component_lifecycle_generation);
  EXPECT_EQ(generation.value().owner_qualified_predecessor->snapshot_digest,
            actual_predecessor.value().snapshot_digest);
  EXPECT_EQ(generation.value().candidate_generation, core::ComponentLifecycleGeneration(2U));
  EXPECT_EQ(generation.value().prior_reservation_generation, 0U);
  const auto generation_replay =
      composition.value()->ReadApplyComponentGenerationSelection(component_id);
  ASSERT_TRUE(generation_replay.ok());
  EXPECT_EQ(generation_replay.value(), generation.value());
  EXPECT_TRUE(composition.value()->coordinator().accepting_new_requests());
  struct stat socket_status {};
  ASSERT_EQ(::lstat(composition.value()->open_request_socket_path().c_str(),
                    &socket_status),
            0);
  EXPECT_TRUE(S_ISSOCK(socket_status.st_mode));
  EXPECT_EQ(socket_status.st_mode & 0777, 0600);
  ASSERT_EQ(::lstat(composition.value()->ros_open_request_socket_path().c_str(),
                    &socket_status),
            0);
  EXPECT_TRUE(S_ISSOCK(socket_status.st_mode));
  EXPECT_EQ(socket_status.st_mode & 0777, 0600);
  const auto request = manyforge::content_identity::canonical_json(
      manyforge::content_identity::CanonicalValue(
          manyforge::content_identity::CanonicalValue::Object{
              {"api_version",
               manyforge::content_identity::CanonicalValue(
                   std::string(core::kCCLCOpenRequestApiVersion))},
              {"command", manyforge::content_identity::CanonicalValue(
                              "component_snapshot")},
              {"payload",
               manyforge::content_identity::CanonicalValue(
                   manyforge::content_identity::CanonicalValue::Object{
                       {"controllable_component_id",
                        manyforge::content_identity::CanonicalValue(
                            component_id.value())}})}}));
  const auto response = manyforge::content_identity::parse_canonical_json(
      Exchange(composition.value()->open_request_socket_path(), request));
  const auto *ok = response.find_object_member("ok");
  ASSERT_NE(ok, nullptr);
  EXPECT_TRUE(ok->as_bool());

  const auto old_socket = composition.value()->open_request_socket_path();
  const auto old_ros_socket =
      composition.value()->ros_open_request_socket_path();
  const auto before_stop =
      composition.value()->coordinator().ReadLifecycleRequestResult(*historical_request);
  ASSERT_TRUE(before_stop.ok());
  ASSERT_TRUE(before_stop.value());
  EXPECT_EQ(before_stop.value()->rejection, rejection);
  composition.value()->StopRequestFollowingAndTransports();
  composition.value()->StopRequestFollowingAndTransports();
  composition.value()->lifecycle_target_driver().Wake();
  EXPECT_EQ(scheduler.pending_count(core::SchedulerPriority::Lifecycle), 0U);
  EXPECT_FALSE(std::filesystem::exists(old_socket));
  EXPECT_FALSE(std::filesystem::exists(old_ros_socket));
  EXPECT_THROW((void)composition.value()->open_request_socket_path(), std::logic_error);
  EXPECT_THROW((void)composition.value()->ros_open_request_socket_path(), std::logic_error);
  EXPECT_TRUE(composition.value()->coordinator().accepting_new_requests());
  EXPECT_FALSE(composition.value()->coordinator().admission_fault_closed());
  EXPECT_EQ(composition.value()->coordinator().request_owner_epoch(), first_owner);
  const auto after_stop =
      composition.value()->coordinator().ReadLifecycleRequestResult(*historical_request);
  ASSERT_TRUE(after_stop.ok());
  ASSERT_TRUE(after_stop.value());
  EXPECT_EQ(after_stop.value()->replay_status, core::RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(after_stop.value()->rejection, rejection);
  EXPECT_FALSE(after_stop.value()->acceptance);
  EXPECT_FALSE(after_stop.value()->status);
  const auto stopped_open_observer = composition.value()->LoadOpenCellObserverIncident();
  ASSERT_TRUE(stopped_open_observer.ok()) << stopped_open_observer.error().message;
  ASSERT_TRUE(stopped_open_observer.value());
  EXPECT_EQ(*stopped_open_observer.value(), stored_observer.value());
  const auto retained_observer = composition.value()->RecordCellObserverIncident(observer);
  ASSERT_TRUE(retained_observer.ok()) << retained_observer.error().message;
  EXPECT_EQ(retained_observer.value(), stored_observer.value());
  EXPECT_EQ(composition.value()->coordinator().request_owner_epoch(), first_owner);
  EXPECT_TRUE(composition.value()->coordinator().accepting_new_requests());
  EXPECT_FALSE(composition.value()->coordinator().admission_fault_closed());
  ASSERT_TRUE(composition.value()->ReadQuiescentOwner(first_owner).ok());
  const auto foreign_quiescence =
      composition.value()->ReadQuiescentOwner(core::RequestOwnerEpoch("foreign.owner"));
  ASSERT_FALSE(foreign_quiescence.ok());
  EXPECT_EQ(foreign_quiescence.error().code, "component_lifecycle_journal_owner_epoch_stale");
  EXPECT_EQ(composition.value()->coordinator().request_owner_epoch(), first_owner);
  EXPECT_TRUE(composition.value()->coordinator().accepting_new_requests());
  EXPECT_FALSE(composition.value()->coordinator().admission_fault_closed());
  composition.value()->coordinator().CloseAdmission();
  EXPECT_FALSE(composition.value()->coordinator().accepting_new_requests());
  composition.value().reset();
  EXPECT_FALSE(std::filesystem::exists(old_socket));
  EXPECT_FALSE(std::filesystem::exists(old_ros_socket));

  auto reused = CCLCRuntimeComposition::Create(
      configuration(root.path(), retained.bundle->value().cell_graph.cell_id,
                    "1"),
      retained.bundle, publication_reader, receipt, artifact_source,
      catalogs, providers, controller_sources(*retained.bundle), {}, {}, {},
      kernel_ports, kernel_ports, publication_owner, clock,
      scheduler);
  ASSERT_TRUE(reused.ok());
  ASSERT_TRUE(reused.value()->Start().ok());
  EXPECT_NE(reused.value()->coordinator().request_owner_epoch(), first_owner);
  reused.value().reset();

  auto replacement = CCLCRuntimeComposition::Create(
      configuration(root.path(), retained.bundle->value().cell_graph.cell_id,
                    "2"),
      retained.bundle, publication_reader, receipt, artifact_source,
      catalogs, providers, controller_sources(*retained.bundle), {}, {}, {},
      kernel_ports, kernel_ports, publication_owner, clock,
      scheduler);
  ASSERT_TRUE(replacement.ok()) << replacement.error().message;
  EXPECT_FALSE(replacement.value()->coordinator().accepting_new_requests());
  ASSERT_TRUE(replacement.value()->Start().ok());
}

TEST(CCLCRuntimeComposition, RejectsMissingTrustedRootsWithoutCreatingThem) {
  ScopedTrustedRoot root;
  const auto retained =
      test::materialize_lite6_retained_consumer_fixture(root.path());
  const auto receipt = publication(*retained.bundle);
  ActivePublicationReader publication_reader(receipt);
  FilesystemVerifiedRetainedProductArtifactSource artifact_source(root.path());
  auto catalogs = core::BuildOwnedLifecyclePlanCatalogSource::Make({});
  ASSERT_TRUE(catalogs.ok());
  UnusedKernelPorts kernel_ports;
  UnusedKernelPublicationOwner publication_owner;
  core::FakeClock clock;
  core::ManualScheduler scheduler;

  auto config = configuration(
      root.path(), retained.bundle->value().cell_graph.cell_id, "missing");
  const auto missing_run_root = root.path() / "absent-run-root";
  config.run_root = missing_run_root;
  auto composition = CCLCRuntimeComposition::Create(
      std::move(config), retained.bundle, publication_reader, receipt,
      artifact_source, catalogs.value(), {},
      controller_sources(*retained.bundle), {}, {}, {}, kernel_ports,
      kernel_ports, publication_owner, clock, scheduler);
  EXPECT_FALSE(composition.ok());
  EXPECT_FALSE(std::filesystem::exists(missing_run_root));
}

TEST(CCLCRuntimeComposition, RejectsAliasedStateAndRunCapabilities) {
  ScopedTrustedRoot root;
  const auto retained =
      test::materialize_lite6_retained_consumer_fixture(root.path());
  const auto receipt = publication(*retained.bundle);
  ActivePublicationReader publication_reader(receipt);
  FilesystemVerifiedRetainedProductArtifactSource artifact_source(root.path());
  auto catalogs = core::BuildOwnedLifecyclePlanCatalogSource::Make({});
  ASSERT_TRUE(catalogs.ok());
  UnusedKernelPorts kernel_ports;
  UnusedKernelPublicationOwner publication_owner;
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = configuration(
      root.path(), retained.bundle->value().cell_graph.cell_id, "aliased");
  config.run_root = config.state_root / ".";

  auto composition = CCLCRuntimeComposition::Create(
      std::move(config), retained.bundle, publication_reader, receipt,
      artifact_source, catalogs.value(), {},
      controller_sources(*retained.bundle), {}, {}, {}, kernel_ports,
      kernel_ports, publication_owner, clock, scheduler);
  EXPECT_FALSE(composition.ok());
}

TEST(CCLCRuntimeComposition, RejectsNestedStateAndRunCapabilities) {
  ScopedTrustedRoot root;
  const auto retained =
      test::materialize_lite6_retained_consumer_fixture(root.path());
  const auto receipt = publication(*retained.bundle);
  ActivePublicationReader publication_reader(receipt);
  FilesystemVerifiedRetainedProductArtifactSource artifact_source(root.path());
  auto catalogs = core::BuildOwnedLifecyclePlanCatalogSource::Make({});
  ASSERT_TRUE(catalogs.ok());
  UnusedKernelPorts kernel_ports;
  UnusedKernelPublicationOwner publication_owner;
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = configuration(
      root.path(), retained.bundle->value().cell_graph.cell_id, "nested");
  config.run_root = config.state_root / "disposable";
  ASSERT_TRUE(std::filesystem::create_directory(config.run_root));

  auto composition = CCLCRuntimeComposition::Create(
      std::move(config), retained.bundle, publication_reader, receipt,
      artifact_source, catalogs.value(), {},
      controller_sources(*retained.bundle), {}, {}, {}, kernel_ports,
      kernel_ports, publication_owner, clock, scheduler);
  EXPECT_FALSE(composition.ok());
}

TEST(CCLCRuntimeComposition,
     RejectsSymlinkedOrUnsafeTrustedRootsBeforeStateMutation) {
  for (const bool symlinked : {false, true}) {
    ScopedTrustedRoot root;
    const auto retained =
        test::materialize_lite6_retained_consumer_fixture(root.path());
    const auto receipt = publication(*retained.bundle);
    ActivePublicationReader publication_reader(receipt);
    FilesystemVerifiedRetainedProductArtifactSource artifact_source(
        root.path());
    auto catalogs = core::BuildOwnedLifecyclePlanCatalogSource::Make({});
    ASSERT_TRUE(catalogs.ok());
    UnusedKernelPorts kernel_ports;
    UnusedKernelPublicationOwner publication_owner;
    core::FakeClock clock;
    core::ManualScheduler scheduler;
    auto config = configuration(
        root.path(), retained.bundle->value().cell_graph.cell_id,
        symlinked ? "symlinked" : "unsafe-mode");
    const auto state_root = config.state_root;
    const auto real_run_root = config.run_root;
    if (symlinked) {
      const auto run_link = root.path() / "run-link";
      std::filesystem::create_directory_symlink(real_run_root, run_link);
      config.run_root = run_link;
    } else {
      std::filesystem::permissions(
          real_run_root, std::filesystem::perms::owner_all |
                             std::filesystem::perms::group_write,
          std::filesystem::perm_options::replace);
    }

    auto composition = CCLCRuntimeComposition::Create(
        std::move(config), retained.bundle, publication_reader, receipt,
        artifact_source, catalogs.value(), {},
        controller_sources(*retained.bundle), {}, {}, {}, kernel_ports,
        kernel_ports, publication_owner, clock, scheduler);
    EXPECT_FALSE(composition.ok());
    EXPECT_TRUE(std::filesystem::is_empty(state_root));
    EXPECT_TRUE(std::filesystem::is_empty(real_run_root));
  }
}

// The prepared pin owns every borrowed candidate product dependency. Kernel,
// clock, scheduler and trusted roots remain the existing cell lifetime.
struct InstalledContextFixture final {
  InstalledContextFixture(std::shared_ptr<const RetainedBundle> retained,
                          core::ActiveBundlePublicationReceiptDTO receipt,
                          const std::filesystem::path &root,
                          core::BrandLifecyclePlanCatalogDTO value)
      : bundle(std::move(retained)), reader(std::move(receipt)),
        artifacts(root), catalogs(std::move(value)) {}
  std::shared_ptr<const RetainedBundle> bundle;
  ActivePublicationReader reader;
  FilesystemVerifiedRetainedProductArtifactSource artifacts;
  CatalogSource catalogs;
  UnusedKernelPublicationOwner publication_owner;
};

struct InstalledEmptyContextFixture final {
  InstalledEmptyContextFixture(std::shared_ptr<const RetainedBundle> retained,
                               core::ActiveBundlePublicationReceiptDTO receipt,
                               const std::filesystem::path& root,
                               core::BuildOwnedLifecyclePlanCatalogSource source)
      : bundle(std::move(retained)),
        reader(std::move(receipt)),
        artifacts(root),
        catalogs(std::move(source)) {}
  std::shared_ptr<const RetainedBundle> bundle;
  ActivePublicationReader reader;
  FilesystemVerifiedRetainedProductArtifactSource artifacts;
  core::BuildOwnedLifecyclePlanCatalogSource catalogs;
  UnusedKernelPublicationOwner publication_owner;
};

TEST(CCLCRuntimeComposition,
     PositionResetExceptionReleasesActualProductPinAndScope) {
  ScopedTrustedRoot root;
  const auto retained =
      test::materialize_lite6_retained_consumer_fixture(root.path());
  auto previous = publication(*retained.bundle);
  previous.request.transaction_ref = core::NamedApplyPublicationTransactionDTO{
      core::ApplyId("reset.initial.apply"), core::ApplyGeneration(1U)};
  previous.request.expected_previous_publication_generation =
      previous.publication_generation;
  previous.publication_generation = core::ActiveBundlePublicationGeneration(2U);
  const auto &graph = retained.bundle->value().cell_graph;
  ASSERT_EQ(graph.controllable_components.size(), 1U);
  ASSERT_EQ(graph.control_domains.size(), 1U);
  const auto component =
      graph.controllable_components.front().controllable_component_id;
  const auto &binding = graph.control_bindings.front();
  const auto &domain = graph.control_domains.front();
  const auto &desired =
      retained.bundle->value().desired_control_configurations.front();
  const auto catalog = catalog_for(binding);
  const std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> providers{
      {domain.control_domain_id, domain.control_domain_revision,
       binding.provider_instance_id, binding.provider_manifest_id,
       binding.provider_manifest_revision, binding.provider_runtime_unit_id,
       core::RuntimeEpoch("reset.product.epoch"), catalog.catalog_digest}};
  auto original = std::make_shared<InstalledContextFixture>(
      retained.bundle, previous, root.path(), catalog);
  UnusedKernelPorts kernel;
  UnusedKernelPublicationOwner publication_owner;
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  const auto config =
      configuration(root.path(), graph.cell_id, "position-reset-exception");
  auto composition = CCLCRuntimeComposition::Create(
      config, retained.bundle, original->reader, previous, original->artifacts,
      original->catalogs, providers, controller_sources(*retained.bundle), {},
      {}, {}, kernel, kernel, publication_owner, clock, scheduler);
  ASSERT_TRUE(composition.ok()) << composition.error().message;
  ASSERT_TRUE(composition.value()->Start().ok());
  composition.value()->lifecycle_target_driver().Stop();
  const auto owner = composition.value()->coordinator().request_owner_epoch();
  const auto plan = composition.value()->PlanNativeApply(
      {std::get<core::NamedApplyPublicationTransactionDTO>(
           previous.request.transaction_ref),
       retained.bundle->retained_bundle_id(),
       graph.graph_revision,
       {component},
       {{domain.control_domain_id,
         core::ActiveControlConfigurationGeneration(1U)}},
       {},
       {},
       {},
       {}});
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  core::ActiveControlConfigurationDTO route{
      core::ActiveControlConfigurationId("reset.active.configuration"),
      domain.control_domain_id,
      domain.control_domain_revision,
      {},
      desired.activation_set_id,
      domain.controller().controller_topology_id,
      domain.controller().topology_revision,
      binding.provider_runtime_unit_id,
      providers.front().provider_runtime_epoch,
      1U,
      {},
      plan.value().routes.front().generation,
      core::ActiveControlConfigurationStateDTO::kReconciled};
  for (const auto &row : desired.binding_routes)
    route.binding_routes.push_back({row.control_binding_id,
                                    row.control_binding_revision,
                                    row.control_route_id});
  ASSERT_TRUE(route.locally_valid());
  ASSERT_TRUE(
      composition.value()
          ->AdoptNativeApplyFacts({previous, plan.value().components, {route}})
          .ok());
  core::SqliteCCLCJournal journal(
      config.state_root / std::string(core::kCCLCJournalFilename),
      graph.cell_id, config.exact_result_history_capacity,
      config.terminal_result_bytes_capacity);
  const auto target = journal.SetLifecycleTarget(
      component, core::LifecycleTargetStateDTO::kActive,
      core::LifecycleTargetOriginDTO::kStartup);
  ASSERT_TRUE(target.ok()) << target.error().message;
  ASSERT_TRUE(journal
                  .RecordLifecycleTargetReached(
                      {component, target.value().target_generation}, true, 0U)
                  .ok());
  const auto current =
      composition.value()->ReadCurrentComponentSnapshot(component);
  ASSERT_TRUE(current.ok()) << current.error().message;
  core::CCLCProgramResetPositionScopeRequest request{
      core::RequestOwnerEpoch("runner.reset"),
      1U,
      core::RequestOwnerEpoch("browser.reset"),
      core::RequestId("reset.position.1"),
      core::ContentDigest("reset.payload"),
      previous,
      component,
      current.value().provider_occurrences.front(),
      current.value().domains.front()};
  bool invoked{};
  const auto reset = composition.value()->WithProgramResetPositionScope(
      request, [&](const auto &) -> core::Result<void> {
        invoked = true;
        EXPECT_TRUE(composition.value()
                        ->lifecycle_target_driver()
                        .ReadReadinessFacts(component)
                        .value()
                        .blocker.has_value());
        throw std::runtime_error("position reset action failed");
      });
  EXPECT_TRUE(invoked);
  EXPECT_FALSE(reset.ok());
  EXPECT_FALSE(composition.value()
                   ->lifecycle_target_driver()
                   .ReadReadinessFacts(component)
                   .value()
                   .blocker.has_value());

  // A real exchange must complete: acquiring another pin alone would not
  // detect a leaked operation pin from the failed synchronous action.
  auto successor = previous;
  successor.request.transaction_ref = core::NamedApplyPublicationTransactionDTO{
      core::ApplyId("reset.successor"), core::ApplyGeneration(1U)};
  successor.request.expected_previous_publication_generation =
      previous.publication_generation;
  successor.publication_generation = core::ActiveBundlePublicationGeneration(
      previous.publication_generation.value() + 1U);
  successor.durable_public_pointer_content_digest =
      core::ContentDigest("reset.successor.pointer");
  auto installed = std::make_shared<InstalledContextFixture>(
      retained.bundle, successor, root.path(), catalog);
  const auto prepared = composition.value()->PrepareProductContext(
      {installed->bundle,
       installed->reader,
       successor,
       installed->artifacts,
       installed->catalogs,
       providers,
       controller_sources(*installed->bundle),
       {},
       {},
       {},
       {},
       nullptr,
       installed->publication_owner,
       installed});
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  const auto exchanged = composition.value()->TryExchangeProductContext(
      *prepared.value(), owner, previous);
  ASSERT_TRUE(exchanged.ok()) << exchanged.error().message;
  EXPECT_EQ(exchanged.value().phase,
            core::ComponentLifecycleProductExchangePhase::kCompleted)
      << exchanged.value().reason;
}

TEST(CCLCRuntimeComposition, ExchangesVerifiedNonemptyToEmptyOnSameFacadeAndRetainsDependencies) {
  ScopedTrustedRoot root;
  const auto empty_package = root.path() / "canonical-empty";
  ASSERT_TRUE(std::filesystem::create_directory(empty_package));
  const auto generated = detail::generate_canonical_empty_package(empty_package);
  const FilesystemCanonicalEmptyRetainedBundleSource admission_source(empty_package);
  const auto admission = admission_source.load_verified();
  EXPECT_EQ(admission.admission_digest(), generated.admission_digest());
  const FilesystemVerifiedRetainedBundleSource empty_source(empty_package);
  const auto empty_bundle = empty_source.get_verified(admission.retained_bundle_id());
  ASSERT_NE(empty_bundle, nullptr);
  EXPECT_EQ(empty_bundle->canonical_bytes(), admission.retained_bundle().canonical_bytes());
  const auto& empty_graph = empty_bundle->value().cell_graph;
  ASSERT_TRUE(empty_graph.controllable_components.empty());
  ASSERT_TRUE(empty_graph.control_domains.empty());
  ASSERT_TRUE(empty_graph.control_bindings.empty());
  ASSERT_TRUE(empty_bundle->value().desired_control_configurations.empty());

  // Materialize a real populated predecessor for the same cell; no graph or
  // retained identity is spliced to manufacture an empty successor.
  const auto retained = test::materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
      root.path(), empty_graph.cell_id.value());
  const auto previous = publication(*retained.bundle);
  const auto& graph = retained.bundle->value().cell_graph;
  ASSERT_FALSE(graph.controllable_components.empty());
  ASSERT_FALSE(graph.control_bindings.empty());
  const auto& binding = graph.control_bindings.front();
  const auto domain = std::find_if(
      graph.control_domains.begin(), graph.control_domains.end(),
      [&](const auto& row) { return row.control_domain_id == binding.control_domain_id; });
  ASSERT_NE(domain, graph.control_domains.end());
  const auto catalog = catalog_for(binding);
  const std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> providers{
      {domain->control_domain_id, domain->control_domain_revision, binding.provider_instance_id,
       binding.provider_manifest_id, binding.provider_manifest_revision,
       binding.provider_runtime_unit_id, core::RuntimeEpoch("context.nonempty.epoch"),
       catalog.catalog_digest}};
  auto original =
      std::make_shared<InstalledContextFixture>(retained.bundle, previous, root.path(), catalog);
  UnusedKernelPorts kernel;
  UnusedKernelPublicationOwner publication_owner;
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto composition = CCLCRuntimeComposition::Create(
      configuration(root.path(), graph.cell_id, "empty-exchange"), retained.bundle,
      original->reader, previous, original->artifacts, original->catalogs, providers,
      controller_sources(*retained.bundle), {}, {}, {}, kernel, kernel, publication_owner, clock,
      scheduler);
  ASSERT_TRUE(composition.ok()) << composition.error().message;
  ASSERT_TRUE(composition.value()->Start().ok());
  // Join the unrelated startup follower so only this test's explicit pin
  // controls pending exchange; owner, journal and transports stay running.
  composition.value()->lifecycle_target_driver().Stop();
  const auto owner = composition.value()->coordinator().request_owner_epoch();
  const auto socket_path = composition.value()->open_request_socket_path();
  struct stat socket_before {};
  ASSERT_EQ(::stat(socket_path.c_str(), &socket_before), 0);

  auto successor = publication(*empty_bundle);
  successor.request.transaction_ref = core::NamedApplyPublicationTransactionDTO{
      core::ApplyId("context.empty.apply"), core::ApplyGeneration(1U)};
  successor.request.expected_previous_publication_generation = previous.publication_generation;
  successor.publication_generation =
      core::ActiveBundlePublicationGeneration(previous.publication_generation.value() + 1U);
  successor.durable_public_pointer_content_digest = core::ContentDigest("context.empty.pointer");
  ASSERT_TRUE(successor.valid());
  auto catalogs = core::BuildOwnedLifecyclePlanCatalogSource::Make({});
  ASSERT_TRUE(catalogs.ok()) << catalogs.error().message;
  auto installed = std::make_shared<InstalledEmptyContextFixture>(
      empty_bundle, successor, empty_package, std::move(catalogs.value()));
  const std::weak_ptr<InstalledEmptyContextFixture> lifetime = installed;
  const auto inputs = [&] {
    return CCLCProductContextInputs{installed->bundle,
                                    installed->reader,
                                    successor,
                                    installed->artifacts,
                                    installed->catalogs,
                                    {},
                                    {},
                                    {},
                                    {},
                                    {},
                                    {},
                                    nullptr,
                                    installed->publication_owner,
                                    installed};
  };
  auto unowned = inputs();
  unowned.installed_product_lifetime.reset();
  EXPECT_FALSE(composition.value()->PrepareProductContext(std::move(unowned)).ok());
  auto wrong_predecessor = inputs();
  wrong_predecessor.expected_publication.request.expected_previous_publication_generation =
      core::ActiveBundlePublicationGeneration(previous.publication_generation.value() + 1U);
  wrong_predecessor.expected_publication.publication_generation =
      core::ActiveBundlePublicationGeneration(successor.publication_generation.value() + 1U);
  ASSERT_TRUE(wrong_predecessor.expected_publication.valid());
  const auto predecessor_refused =
      composition.value()->PrepareProductContext(std::move(wrong_predecessor));
  ASSERT_FALSE(predecessor_refused.ok());
  EXPECT_EQ(predecessor_refused.error().details.at("reason"),
            "prepared_product_predecessor_mismatch");
  auto wrong_head = inputs();
  wrong_head.expected_publication.durable_public_pointer_content_digest =
      core::ContentDigest("foreign.empty.pointer");
  const auto head_refused = composition.value()->PrepareProductContext(std::move(wrong_head));
  ASSERT_FALSE(head_refused.ok());
  EXPECT_EQ(head_refused.error().details.at("reason"),
            "prepared_product_publication_mismatch");
  auto wrong_bundle = inputs();
  wrong_bundle.retained_bundle = retained.bundle;
  EXPECT_FALSE(composition.value()->PrepareProductContext(std::move(wrong_bundle)).ok());

  auto prepared = composition.value()->PrepareProductContext(inputs());
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  installed.reset();
  ASSERT_FALSE(lifetime.expired());
  {
    auto old_pin = composition.value()->coordinator().AcquireProductPin();
    ASSERT_TRUE(old_pin.ok());
    EXPECT_FALSE(old_pin.value().context().identity->configured_components.empty());
    const auto pending =
        composition.value()->TryExchangeProductContext(*prepared.value(), owner, previous);
    ASSERT_TRUE(pending.ok()) << pending.error().message;
    EXPECT_EQ(pending.value().phase, core::ComponentLifecycleProductExchangePhase::kPending)
        << pending.value().reason;
    EXPECT_FALSE(composition.value()->coordinator().AcquireProductPin().ok());
    EXPECT_FALSE(lifetime.expired());
    EXPECT_EQ(old_pin.value().context().identity->publication, previous);
    EXPECT_TRUE(old_pin.value()
                    .context()
                    .current_snapshot_source
                    .ReadCurrentComponentSnapshot(
                        graph.controllable_components.front().controllable_component_id)
                    .ok());
    const auto foreign_owner = composition.value()->TryExchangeProductContext(
        *prepared.value(), core::RequestOwnerEpoch("foreign.empty.owner"), previous);
    ASSERT_FALSE(foreign_owner.ok());
    EXPECT_EQ(foreign_owner.error().details.at("reason"),
              "prepared_product_predecessor_foreign");
    auto foreign_previous = previous;
    foreign_previous.durable_public_pointer_content_digest =
        core::ContentDigest("foreign.predecessor.pointer");
    EXPECT_FALSE(composition.value()
                     ->TryExchangeProductContext(*prepared.value(), owner, foreign_previous)
                     .ok());
    EXPECT_TRUE(composition.value()->coordinator().AcquireProductPin().ok());
  }
  const auto completed =
      composition.value()->TryExchangeProductContext(*prepared.value(), owner, previous);
  ASSERT_TRUE(completed.ok()) << completed.error().message;
  EXPECT_EQ(completed.value().phase, core::ComponentLifecycleProductExchangePhase::kCompleted)
      << completed.value().reason;
  EXPECT_EQ(completed.value().owner_epoch, owner);
  EXPECT_EQ(completed.value().previous_publication, previous);
  EXPECT_EQ(completed.value().current_publication, successor);
  prepared.value().reset();
  ASSERT_FALSE(lifetime.expired());
  {
    auto empty_pin = composition.value()->coordinator().AcquireProductPin();
    ASSERT_TRUE(empty_pin.ok());
    ASSERT_TRUE(empty_pin.value().context().identity);
    EXPECT_EQ(empty_pin.value().context().identity->publication, successor);
    EXPECT_TRUE(empty_pin.value().context().identity->configured_components.empty());
    // Exercise the borrowed sources after both caller-owned lifetime handles
    // have been released: removed membership is refused by the actual source.
    EXPECT_FALSE(empty_pin.value()
                     .context()
                     .current_snapshot_source
                     .ReadCurrentComponentSnapshot(
                         graph.controllable_components.front().controllable_component_id)
                     .ok());
    EXPECT_FALSE(empty_pin.value()
                     .context()
                     .lifecycle_plan_catalog_source
                     .LoadExactLifecyclePlanCatalog(binding.provider_manifest_id,
                                                    binding.provider_manifest_revision,
                                                    catalog.catalog_digest)
                     .ok());
  }
  EXPECT_EQ(composition.value()->coordinator().request_owner_epoch(), owner);
  EXPECT_TRUE(composition.value()->coordinator().accepting_new_requests());
  EXPECT_TRUE(composition.value()->LoadOpenCellObserverIncident().ok());
  EXPECT_EQ(composition.value()->open_request_socket_path(), socket_path);
  struct stat socket_after {};
  ASSERT_EQ(::stat(socket_path.c_str(), &socket_after), 0);
  EXPECT_EQ(socket_after.st_dev, socket_before.st_dev);
  EXPECT_EQ(socket_after.st_ino, socket_before.st_ino);
  using manyforge::content_identity::CanonicalValue;
  const auto response = manyforge::content_identity::parse_canonical_json(
      Exchange(socket_path,
               manyforge::content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
                   {"api_version", CanonicalValue(std::string(core::kCCLCOpenRequestApiVersion))},
                   {"command", CanonicalValue("owner_identity")},
                   {"payload", CanonicalValue(CanonicalValue::Object{})}}))));
  const auto* ok = response.find_object_member("ok");
  ASSERT_NE(ok, nullptr);
  ASSERT_TRUE(ok->as_bool());
  const auto* result = response.find_object_member("result");
  ASSERT_NE(result, nullptr);
  const auto* observed_owner = result->find_object_member("request_owner_epoch");
  ASSERT_NE(observed_owner, nullptr);
  EXPECT_EQ(observed_owner->as_string(), owner.value());
  composition.value().reset();
  EXPECT_TRUE(lifetime.expired());
}

TEST(CCLCRuntimeComposition,
     PreparesOneOwnedProductAndExchangesWithoutOwnerOrTransportRotation) {
  ScopedTrustedRoot root;
  const auto retained =
      test::materialize_lite6_retained_consumer_fixture(root.path());
  const auto previous = publication(*retained.bundle);
  const auto &graph = retained.bundle->value().cell_graph;
  ASSERT_FALSE(graph.control_bindings.empty());
  const auto &binding = graph.control_bindings.front();
  const auto domain =
      std::find_if(graph.control_domains.begin(), graph.control_domains.end(),
                   [&](const auto &row) {
                     return row.control_domain_id == binding.control_domain_id;
                   });
  ASSERT_NE(domain, graph.control_domains.end());
  const auto catalog = catalog_for(binding);
  const std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> providers{
      {domain->control_domain_id, domain->control_domain_revision,
       binding.provider_instance_id, binding.provider_manifest_id,
       binding.provider_manifest_revision, binding.provider_runtime_unit_id,
       core::RuntimeEpoch("context.product.epoch"), catalog.catalog_digest}};
  auto original = std::make_shared<InstalledContextFixture>(
      retained.bundle, previous, root.path(), catalog);
  UnusedKernelPorts kernel;
  UnusedKernelPublicationOwner publication_owner;
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto composition = CCLCRuntimeComposition::Create(
      configuration(root.path(), graph.cell_id, "exchange"), retained.bundle,
      original->reader, previous, original->artifacts, original->catalogs,
      providers, controller_sources(*retained.bundle), {}, {}, {}, kernel,
      kernel, publication_owner, clock, scheduler);
  ASSERT_TRUE(composition.ok()) << composition.error().message;
  ASSERT_TRUE(composition.value()->Start().ok());
  // Join the unrelated startup follower before testing explicit pin drain.
  composition.value()->lifecycle_target_driver().Stop();
  const auto owner = composition.value()->coordinator().request_owner_epoch();
  auto successor = previous;
  successor.request.transaction_ref = core::NamedApplyPublicationTransactionDTO{
      core::ApplyId("context.apply.1"), core::ApplyGeneration(1U)};
  successor.request.expected_previous_publication_generation =
      previous.publication_generation;
  successor.publication_generation = core::ActiveBundlePublicationGeneration(
      previous.publication_generation.value() + 1U);
  successor.durable_public_pointer_content_digest =
      core::ContentDigest("context.successor.pointer");
  ASSERT_TRUE(successor.valid());
  auto installed = std::make_shared<InstalledContextFixture>(
      retained.bundle, successor, root.path(), catalog);
  const auto inputs = [&] {
    return CCLCProductContextInputs{installed->bundle,
                                    installed->reader,
                                    successor,
                                    installed->artifacts,
                                    installed->catalogs,
                                    providers,
                                    controller_sources(*installed->bundle),
                                    {},
                                    {},
                                    {},
                                    {},
                                    nullptr,
                                    installed->publication_owner,
                                    installed};
  };
  auto unowned = inputs();
  unowned.installed_product_lifetime.reset();
  EXPECT_FALSE(
      composition.value()->PrepareProductContext(std::move(unowned)).ok());
  const auto prepared = composition.value()->PrepareProductContext(inputs());
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  {
    auto pinned = composition.value()->coordinator().AcquireProductPin();
    ASSERT_TRUE(pinned.ok());
    const auto pending = composition.value()->TryExchangeProductContext(
        *prepared.value(), owner, previous);
    ASSERT_TRUE(pending.ok()) << pending.error().message;
    EXPECT_EQ(pending.value().phase,
              core::ComponentLifecycleProductExchangePhase::kPending)
        << pending.value().reason;
    EXPECT_FALSE(composition.value()->coordinator().AcquireProductPin().ok());
    const auto refused = composition.value()->TryExchangeProductContext(
        *prepared.value(), core::RequestOwnerEpoch("foreign.context.owner"),
        previous);
    EXPECT_FALSE(refused.ok());
    EXPECT_TRUE(composition.value()->coordinator().AcquireProductPin().ok());
    EXPECT_EQ(pinned.value().context().identity->publication, previous);
  }
  const auto completed = composition.value()->TryExchangeProductContext(
      *prepared.value(), owner, previous);
  ASSERT_TRUE(completed.ok()) << completed.error().message;
  EXPECT_EQ(completed.value().phase,
            core::ComponentLifecycleProductExchangePhase::kCompleted)
      << completed.value().reason;
  EXPECT_EQ(composition.value()->coordinator().request_owner_epoch(), owner);
  const auto pin = composition.value()->coordinator().AcquireProductPin();
  ASSERT_TRUE(pin.ok());
  EXPECT_EQ(pin.value().context().identity->publication, successor);
  ASSERT_TRUE(pin.value().context().identity);
  EXPECT_EQ(pin.value().context().identity->configured_components.size(),
            graph.controllable_components.size());
  EXPECT_TRUE(composition.value()->coordinator().accepting_new_requests());
  const auto replay = composition.value()->TryExchangeProductContext(
      *prepared.value(), owner, previous);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().phase,
            core::ComponentLifecycleProductExchangePhase::kCompleted);
  // Exact journal reads remain attached to the same stable owner.
  EXPECT_TRUE(composition.value()->LoadOpenCellObserverIncident().ok());
}

TEST(CCLCRuntimeComposition,
     FailedExchangeProofStaysClosedAfterSuccessfulCompletionReconciliation) {
  ScopedTrustedRoot root;
  const auto retained =
      test::materialize_lite6_retained_consumer_fixture(root.path());
  const auto previous = publication(*retained.bundle);
  const auto &graph = retained.bundle->value().cell_graph;
  ASSERT_FALSE(graph.control_bindings.empty());
  const auto &binding = graph.control_bindings.front();
  const auto domain =
      std::find_if(graph.control_domains.begin(), graph.control_domains.end(),
                   [&](const auto &row) {
                     return row.control_domain_id == binding.control_domain_id;
                   });
  ASSERT_NE(domain, graph.control_domains.end());
  const auto catalog = catalog_for(binding);
  const std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> providers{
      {domain->control_domain_id, domain->control_domain_revision,
       binding.provider_instance_id, binding.provider_manifest_id,
       binding.provider_manifest_revision, binding.provider_runtime_unit_id,
       core::RuntimeEpoch("context.product.epoch"), catalog.catalog_digest}};
  auto original = std::make_shared<InstalledContextFixture>(
      retained.bundle, previous, root.path(), catalog);
  BlockingSuccessfulReconciliationPorts kernel;
  UnusedKernelPublicationOwner publication_owner;
  core::FakeClock clock;
  clock.set(100);
  core::ManualScheduler scheduler;
  const auto config = configuration(root.path(), graph.cell_id, "proof-fault");
  auto composition = CCLCRuntimeComposition::Create(
      config, retained.bundle, original->reader, previous, original->artifacts,
      original->catalogs, providers, controller_sources(*retained.bundle), {},
      {}, {}, kernel, kernel, publication_owner, clock, scheduler);
  ASSERT_TRUE(composition.ok()) << composition.error().message;
  // Any failed assertion still releases both real worker barriers before the
  // composition destructor attempts its existing drain.
  const auto release_on_exit = std::shared_ptr<void>(&kernel, [&](void *) {
    kernel.AllowRelease();
    kernel.AllowProbe();
  });
  const auto component =
      graph.controllable_components.front().controllable_component_id;
  std::optional<core::ComponentLifecycleCurrentSnapshotDTO> snapshot;
  {
    auto pin = composition.value()->coordinator().AcquireProductPin();
    ASSERT_TRUE(pin.ok());
    const auto actual =
        pin.value()
            .context()
            .current_snapshot_source.ReadCurrentComponentSnapshot(component);
    ASSERT_TRUE(actual.ok()) << actual.error().message;
    snapshot = actual.value();
  }
  std::vector<core::ExpectedControlDomainRevisionDTO> expected_domains;
  for (const auto &scope : snapshot->domains) {
    ASSERT_FALSE(scope.controller().active_control_configuration);
    expected_domains.push_back(
        {scope.control_domain_id,
         scope.control_domain_revision,
         scope.controller().controller_topology_id,
         scope.controller().topology_revision,
         {core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
          std::nullopt, std::nullopt},
         scope.controller().current_control_bindings});
  }
  std::optional<core::ComponentLifecycleIntentDTO> original_intent;
  std::optional<core::ComponentLifecycleAcceptedDTO> original_acceptance;
  {
    core::SqliteCCLCJournal seed(
        config.state_root / std::string(core::kCCLCJournalFilename),
        graph.cell_id, config.exact_result_history_capacity,
        config.terminal_result_bytes_capacity);
    const auto begun = seed.BeginOwnerEpoch(
        core::RequestOwnerEpoch("proof.predecessor.seed"), 100, 128U);
    ASSERT_TRUE(begun.ok()) << begun.error().message;
    original_intent = core::ComponentLifecycleIntentDTO{
        {core::RequestId("proof.predecessor.prepare"),
         begun.value().request_owner_epoch, "test", std::nullopt},
        component,
        core::ComponentLifecycleIntentKindDTO::kPrepare,
        graph.graph_revision,
        snapshot->component_lifecycle_generation,
        expected_domains,
        std::nullopt};
    const auto admitted = core::PrepareComponentLifecycleAdmission(
        *original_intent, {core::LifecycleTransactionId("lifecycle.1"),
                           core::LifecycleTransactionGeneration(1U),
                           snapshot->component_lifecycle_generation,
                           100,
                           snapshot,
                           {catalog}});
    ASSERT_TRUE(admitted.ok()) << admitted.error().message;
    original_acceptance = admitted.value().acceptance;
    const core::CellAdmissionClaimIntentDTO claim_intent{
        core::CellAdmissionClaimKindDTO::kComponentLifecycle,
        core::CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
        begun.value().request_owner_epoch,
        original_acceptance->lifecycle_transaction_id.value(),
        original_intent->request_context.request_id,
        core::ContentDigest(
            core::detail::ComponentLifecycleSemanticIntentDigest(
                *original_intent)),
        core::AdmissionGeneration(1U),
        {},
        std::nullopt,
        std::nullopt};
    const core::CellAdmissionClaimDTO claim{
        claim_intent, core::CellAdmissionClaimGeneration(1U),
        core::AdmissionGeneration(1U), 100};
    ASSERT_TRUE(seed.RecordCellClaimAttempt(begun.value().request_owner_epoch,
                                            claim_intent)
                    .ok());
    ASSERT_TRUE(
        seed.RecordCellClaimAcquired(begun.value().request_owner_epoch, claim)
            .ok());
    const auto accepted = seed.AcceptOrReplayLifecycle(
        begun.value().request_owner_epoch, *original_intent,
        *original_acceptance, admitted.value().initial_status);
    ASSERT_TRUE(accepted.ok())
        << accepted.error().code << ": " << accepted.error().message;
    kernel.RetainClaim(claim);
  }
  ASSERT_TRUE(composition.value()->Start().ok());
  ASSERT_TRUE(kernel.WaitForRelease(std::chrono::seconds(2)));
  const auto owner = composition.value()->coordinator().request_owner_epoch();
  auto successor = previous;
  successor.request.transaction_ref = core::NamedApplyPublicationTransactionDTO{
      core::ApplyId("proof.context.apply"), core::ApplyGeneration(1U)};
  successor.request.expected_previous_publication_generation =
      previous.publication_generation;
  successor.publication_generation = core::ActiveBundlePublicationGeneration(
      previous.publication_generation.value() + 1U);
  successor.durable_public_pointer_content_digest =
      core::ContentDigest("proof.successor.pointer");
  auto installed = std::make_shared<InstalledContextFixture>(
      retained.bundle, successor, root.path(), catalog);
  const auto prepared = composition.value()->PrepareProductContext(
      {installed->bundle,
       installed->reader,
       successor,
       installed->artifacts,
       installed->catalogs,
       providers,
       controller_sources(*installed->bundle),
       {},
       {},
       {},
       {},
       nullptr,
       installed->publication_owner,
       installed});
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  kernel.AllowRelease();
  ASSERT_TRUE(kernel.WaitForProbe(std::chrono::seconds(2)));
  installed->reader.FailReads();
  const auto refused = composition.value()->TryExchangeProductContext(
      *prepared.value(), owner, previous);
  EXPECT_FALSE(refused.ok());
  EXPECT_FALSE(composition.value()->coordinator().accepting_new_requests());
  kernel.AllowProbe();
  ASSERT_TRUE(
      composition.value()->coordinator().DrainFor(std::chrono::seconds(2)));
  EXPECT_EQ(kernel.reconcile_count(), 1U);
  EXPECT_TRUE(composition.value()->coordinator().admission_fault_closed());
  EXPECT_FALSE(composition.value()->coordinator().accepting_new_requests());
  auto fresh = *original_intent;
  fresh.request_context.request_owner_epoch = owner;
  fresh.request_context.request_id = core::RequestId("proof.fresh.after.fault");
  fresh.expected_component_lifecycle_generation =
      original_acceptance->component_lifecycle_generation;
  const auto submitted =
      composition.value()->coordinator().SubmitLifecycle(fresh);
  EXPECT_FALSE(submitted.ok());
  if (!submitted.ok()) {
    EXPECT_EQ(submitted.error().code, "component_lifecycle_admission_closed");
    EXPECT_EQ(submitted.error().details.at("reason"),
              "coordinator_not_accepting");
  }
  EXPECT_EQ(kernel.acquire_count(), 0U);
  const auto pin = composition.value()->coordinator().AcquireProductPin();
  ASSERT_TRUE(pin.ok());
  EXPECT_EQ(pin.value().context().identity->publication, previous);
}

class UnobservedCandidateBrandPort final : public core::IBrandLifecyclePort {
public:
  core::Result<core::BrandLifecycleObservationDTO>
  Observe(const core::BrandLifecycleObservationQueryDTO &) override {
    ++observations;
    return core::Result<core::BrandLifecycleObservationDTO>::Err(unsupported());
  }
  core::BrandLifecyclePrimitiveResultDTO
  Dispatch(const core::BrandLifecyclePrimitiveIntentDTO &,
           std::stop_token) override {
    ++dispatches;
    throw std::logic_error("candidate source must never dispatch");
  }
  unsigned observations{};
  unsigned dispatches{};
};

TEST(
    CCLCRuntimeComposition,
    NativeCandidateFactsUseExactRecordedTransactionWithoutPublicSnapshotOrEffects) {
  ScopedTrustedRoot root;
  const auto retained =
      test::materialize_boolean_io_retained_fixture(root.path());
  const auto &graph = retained.bundle->value().cell_graph;
  ASSERT_EQ(graph.control_domains.size(), 1U);
  const auto &domain = graph.control_domains.front();
  ASSERT_NE(domain.provider(), nullptr);
  const auto &provider = *domain.provider();
  FilesystemVerifiedRetainedProviderManifestSource manifests(root.path());
  const auto manifest = manifests.get_verified(
      {retained.bundle->retained_bundle_id(), provider.provider_manifest_id,
       provider.provider_manifest_revision});
  ASSERT_TRUE(manifest);
  const auto catalog = core::MakeIoProviderLifecycleCatalog(manifest->value());
  ASSERT_TRUE(catalog.ok());
  CatalogSource catalogs(catalog.value());
  const auto receipt = publication(*retained.bundle);
  ActivePublicationReader reader(receipt);
  FilesystemVerifiedRetainedProductArtifactSource artifacts(root.path());
  const core::RuntimeEpoch epoch("candidate.actual.epoch");
  const core::ResolvedLifecycleProviderOccurrenceDTO occurrence{
      domain.control_domain_id,
      domain.control_domain_revision,
      provider.provider_instance_id,
      provider.provider_manifest_id,
      provider.provider_manifest_revision,
      provider.provider_runtime_unit_id,
      epoch,
      catalog.value().catalog_digest};
  const core::ResolvedLifecycleControlDomainScopeDTO scope{
      domain.control_domain_id, domain.control_domain_revision,
      core::ResolvedProviderDomainScopeDTO{provider, epoch, std::nullopt, {}}};
  auto port = std::make_shared<UnobservedCandidateBrandPort>();
  const std::vector<core::BrandLifecyclePortRegistrationDTO> ports{
      {occurrence, port}};
  UnusedKernelPorts kernel;
  UnusedKernelPublicationOwner publication_owner;
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto composition = CCLCRuntimeComposition::Create(
      configuration(root.path(), graph.cell_id, "candidate.facts"),
      retained.bundle, reader, receipt, artifacts, catalogs, {occurrence}, {},
      ports, {}, {}, kernel, kernel, publication_owner, clock, scheduler);
  ASSERT_TRUE(composition.ok()) << composition.error().message;
  ASSERT_TRUE(composition.value()->Start().ok());
  composition.value()->lifecycle_target_driver().Stop();
  const core::NamedApplyPublicationTransactionDTO apply{
      core::ApplyId("actual.pending.apply"), core::ApplyGeneration(1U)};
  const auto plan = composition.value()->PlanNativeApply(
      {apply,
       retained.bundle->retained_bundle_id(),
       graph.graph_revision,
       {provider.controllable_component_id},
       {},
       {},
       {},
       {},
       {},
       core::ComponentLifecycleStateDTO::kDeclared});
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  const auto observations = port->observations;
  const auto dispatches = port->dispatches;
  // No candidate public pointer is supplied or fabricated. The existing
  // snapshot source fails, while the actual recorded-plan source still works.
  reader.FailReads();
  EXPECT_FALSE(
      composition.value()
          ->ReadCurrentComponentSnapshot(provider.controllable_component_id)
          .ok());
  const auto facts =
      composition.value()->ReadInstalledNativeApplyCandidateFacts(
          plan.value(), *retained.bundle, catalogs,
          provider.controllable_component_id, {scope}, {occurrence}, ports);
  ASSERT_TRUE(facts.ok()) << facts.error().message;
  EXPECT_EQ(facts.value().component_lifecycle_generation,
            plan.value().components.front().generation);
  EXPECT_EQ(std::get<core::InstalledNativeApplyCandidateSourceDTO>(
                facts.value().source)
                .apply,
            apply);
  EXPECT_EQ(facts.value().domains, std::vector{scope});
  EXPECT_EQ(port->observations, observations);
  EXPECT_EQ(port->dispatches, dispatches);
  auto wrong_plan = plan.value();
  wrong_plan.components.front().generation = core::ComponentLifecycleGeneration(
      wrong_plan.components.front().generation.value() + 1U);
  EXPECT_FALSE(composition.value()
                   ->ReadInstalledNativeApplyCandidateFacts(
                       wrong_plan, *retained.bundle, catalogs,
                       provider.controllable_component_id, {scope},
                       {occurrence}, ports)
                   .ok());
  auto wrong_transaction = plan.value();
  wrong_transaction.apply.apply_id = core::ApplyId("foreign.apply");
  EXPECT_FALSE(composition.value()
                   ->ReadInstalledNativeApplyCandidateFacts(
                       wrong_transaction, *retained.bundle, catalogs,
                       provider.controllable_component_id, {scope},
                       {occurrence}, ports)
                   .ok());
  EXPECT_FALSE(composition.value()
                   ->ReadInstalledNativeApplyCandidateFacts(
                       plan.value(), *retained.bundle, catalogs,
                       provider.controllable_component_id, {}, {}, {})
                   .ok());
  auto wrong_ports = ports;
  wrong_ports.front().provider_occurrence.provider_runtime_epoch =
      core::RuntimeEpoch("foreign.epoch");
  EXPECT_FALSE(composition.value()
                   ->ReadInstalledNativeApplyCandidateFacts(
                       plan.value(), *retained.bundle, catalogs,
                       provider.controllable_component_id, {scope},
                       {occurrence}, wrong_ports)
                   .ok());
  composition.value()->coordinator().CloseAdmission();
  EXPECT_FALSE(composition.value()
                   ->ReadInstalledNativeApplyCandidateFacts(
                       plan.value(), *retained.bundle, catalogs,
                       provider.controllable_component_id, {scope},
                       {occurrence}, ports)
                   .ok());
}

} // namespace
} // namespace manyforge::deployment
