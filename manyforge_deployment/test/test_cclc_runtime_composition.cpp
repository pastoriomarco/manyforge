#include <gtest/gtest.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/build_owned_lifecycle_plan_catalog_source.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_deployment/cclc_runtime_composition.hpp>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

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
    if (generation != receipt_.publication_generation) {
      return core::Result<core::ActiveBundlePublicationReceiptDTO>::Err(
          unsupported());
    }
    return core::Result<core::ActiveBundlePublicationReceiptDTO>::Ok(receipt_);
  }

private:
  core::ActiveBundlePublicationReceiptDTO receipt_;
};

class UnusedKernelPorts final
    : public core::ICCLCKernelDisableHandoffPort,
      public core::ICCLCKernelRouteConfigurationFencePort,
      public core::IKernelCellAdmissionPort {
public:
  [[nodiscard]] core::AuthoringAdmissionDTO GetAuthoringAdmission()
      const override {
    return {true, core::AdmissionGeneration(1U)};
  }
  [[nodiscard]] core::Result<core::AuthoringAdmissionDTO>
  UpdateCellAdmissionReadiness(
      const core::CellAdmissionReadinessSnapshotDTO &) override {
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
  [[nodiscard]] core::Result<core::DisableHandoffAdmissionSnapshotDTO>
  ObserveDisableHandoffAdmission(
      const core::DisableHandoffAdmissionQueryDTO &) const override {
    return core::Result<core::DisableHandoffAdmissionSnapshotDTO>::Err(
        unsupported());
  }

  [[nodiscard]] core::Result<core::DisableHandoffAcceptedDTO>
  BeginDisableHandoff(const core::DisableHandoffRequestDTO &) override {
    return core::Result<core::DisableHandoffAcceptedDTO>::Err(unsupported());
  }

  [[nodiscard]] core::Result<core::DisableHandoffStatusDTO>
  DisableHandoffStatus(const core::LifecycleTransactionId &,
                       core::LifecycleTransactionGeneration) override {
    return core::Result<core::DisableHandoffStatusDTO>::Err(unsupported());
  }

  [[nodiscard]] core::Result<core::DisableHandoffAttemptReconciliationDTO>
  ReconcileDisableHandoffAttempt(
      const core::DisableHandoffRequestDTO &) override {
    return core::Result<core::DisableHandoffAttemptReconciliationDTO>::Err(
        unsupported());
  }

  [[nodiscard]] core::Result<core::DisableFenceReleasedDTO>
  ReleaseDisableFence(const core::DisableFenceReleaseRequestDTO &) override {
    return core::Result<core::DisableFenceReleasedDTO>::Err(unsupported());
  }

  [[nodiscard]]
  core::Result<core::DisableFenceReleaseAttemptReconciliationDTO>
  ReconcileDisableFenceReleaseAttempt(
      const core::DisableFenceReleaseRequestDTO &) override {
    return core::Result<core::DisableFenceReleaseAttemptReconciliationDTO>::Err(
        unsupported());
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
          {core::RequestOwnerEpoch("cclc-owner." + owner_suffix), 128U, 1U, 1U,
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
     OwnsAndStartsTheCompleteDurableDependencySetWithoutDiscovery) {
  ScopedTrustedRoot root;
  const auto retained =
      test::materialize_lite6_retained_consumer_fixture(root.path());
  const auto receipt = publication(*retained.bundle);
  ActivePublicationReader publication_reader(receipt);
  FilesystemVerifiedRetainedProductArtifactSource artifact_source(root.path());
  auto catalogs = core::BuildOwnedLifecyclePlanCatalogSource::Make({});
  ASSERT_TRUE(catalogs.ok()) << catalogs.error().message;
  UnusedKernelPorts kernel_ports;
  UnusedKernelPublicationOwner publication_owner;
  core::FakeClock clock;
  core::ManualScheduler scheduler;

  auto composition = CCLCRuntimeComposition::Create(
      configuration(root.path(), retained.bundle->value().cell_graph.cell_id,
                    "1"),
      retained.bundle, publication_reader, receipt, artifact_source,
      catalogs.value(), {}, controller_sources(*retained.bundle), {}, {}, {},
      kernel_ports, kernel_ports, kernel_ports, publication_owner, clock,
      scheduler);
  ASSERT_TRUE(composition.ok()) << composition.error().message;
  EXPECT_FALSE(composition.value()->coordinator().accepting_new_requests());
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
  const auto component_id = retained.bundle->value()
                                .cell_graph.controllable_components.front()
                                .controllable_component_id;
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
  EXPECT_FALSE(ok->as_bool());

  composition.value()->coordinator().CloseAdmission();
  EXPECT_FALSE(composition.value()->coordinator().accepting_new_requests());
  const auto old_socket = composition.value()->open_request_socket_path();
  const auto old_ros_socket =
      composition.value()->ros_open_request_socket_path();
  composition.value().reset();
  EXPECT_FALSE(std::filesystem::exists(old_socket));
  EXPECT_FALSE(std::filesystem::exists(old_ros_socket));

  auto reused = CCLCRuntimeComposition::Create(
      configuration(root.path(), retained.bundle->value().cell_graph.cell_id,
                    "1"),
      retained.bundle, publication_reader, receipt, artifact_source,
      catalogs.value(), {}, controller_sources(*retained.bundle), {}, {}, {},
      kernel_ports, kernel_ports, kernel_ports, publication_owner, clock,
      scheduler);
  ASSERT_TRUE(reused.ok());
  ASSERT_TRUE(reused.value()->Start().ok());
  EXPECT_NE(reused.value()->coordinator().request_owner_epoch(), first_owner);
  reused.value().reset();

  auto replacement = CCLCRuntimeComposition::Create(
      configuration(root.path(), retained.bundle->value().cell_graph.cell_id,
                    "2"),
      retained.bundle, publication_reader, receipt, artifact_source,
      catalogs.value(), {}, controller_sources(*retained.bundle), {}, {}, {},
      kernel_ports, kernel_ports, kernel_ports, publication_owner, clock,
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
      kernel_ports, kernel_ports, publication_owner, clock, scheduler);
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
      kernel_ports, kernel_ports, publication_owner, clock, scheduler);
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
      kernel_ports, kernel_ports, publication_owner, clock, scheduler);
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
        kernel_ports, kernel_ports, publication_owner, clock, scheduler);
    EXPECT_FALSE(composition.ok());
    EXPECT_TRUE(std::filesystem::is_empty(state_root));
    EXPECT_TRUE(std::filesystem::is_empty(real_run_root));
  }
}

} // namespace
} // namespace manyforge::deployment
