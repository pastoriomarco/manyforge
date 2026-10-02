#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "manyforge_component_manager/manager_apply_fence_release_coordinator.hpp"
#include "manyforge_component_manager/manager_named_apply_coordinator.hpp"
#include "manyforge_component_manager/manager_runtime_convergence_coordinator.hpp"
#include "manyforge_component_manager/manager_selected_bundle_transition_coordinator.hpp"
#include "manyforge_component_manager/manager_selected_runtime_composition_coordinator.hpp"
#include "manyforge_component_manager/reference_runtime_unit_admission.hpp"
#include "manyforge_component_manager/retained_posix_runtime_unit_adapter.hpp"
#include "lite6_controllable_retention_fixture.hpp"
#include "runtime_only_retention_fixture.hpp"

namespace manyforge::component_manager {
namespace {

using namespace manyforge::core;

constexpr std::string_view kDigestOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigestTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kSceneId =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";

class ScopedPackageRoot final {
public:
  ScopedPackageRoot() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-runtime-convergence-test-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("runtime-convergence fixture mkdtemp failed");
    }
    path_ = created;
  }

  ~ScopedPackageRoot() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

struct RuntimePackage final {
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::unique_ptr<deployment::FilesystemVerifiedRetainedBundleSource>
      bundle_source;
  std::unique_ptr<deployment::FilesystemVerifiedRetainedProductArtifactSource>
      artifact_source;
};

class RefusingBundleReloadSource final
    : public deployment::IVerifiedRetainedBundleSource {
public:
  explicit RefusingBundleReloadSource(
      const deployment::IVerifiedRetainedBundleSource &source)
      : source_(source) {}

  [[nodiscard]] std::shared_ptr<const deployment::RetainedBundle>
  get_verified(const RetainedBundleId &id) const override {
    ++read_calls;
    if (refuse_reads) {
      throw std::runtime_error("accepted bundle was reloaded");
    }
    return source_.get_verified(id);
  }

  mutable int read_calls{0};
  bool refuse_reads{false};

private:
  const deployment::IVerifiedRetainedBundleSource &source_;
};

[[nodiscard]] RuntimePackage
MaterializeRuntimePackage(const std::filesystem::path &root,
                          const bool empty_runtime = false) {
  auto fixture = [&] {
    if (!empty_runtime) {
      return deployment::test::make_runtime_only_retention_fixture();
    }
    auto profile = deployment::test::qualify_runtime_only_profile();
    const std::string graph =
        "{\"runtime_units\":[],\"model_instances\":[],"
        "\"controllable_components\":[],\"control_domains\":[],"
        "\"control_bindings\":[],\"component_attachments\":[]}";
    auto deployment = deployment::compute_deployment_artifact_revision(
        deployment::parse_strict_json_model_yaml(
            deployment::test::runtime_only_deployment_document(
                profile, graph, "{\"records\":[]}")),
        std::span<const deployment::QualifiedRuntimeProfile>{});
    const auto composition = deployment::compose_cell_occurrences(
        deployment.graph(),
        std::span<const deployment::SourceVerifiedModelAsset>{},
        std::span<const deployment::QualifiedComponentType>{});
    const auto selected_overlay =
        deployment::select_site_overlay_for_deployment(
            deployment.applicability_inputs().value().site_overlay_ref,
            deployment.applicability_inputs().value().external_resources,
            nullptr);
    const auto applicability = deployment::qualify_cell_applicability(
        composition, deployment.applicability_inputs(),
        std::span<const deployment::QualifiedRuntimeProfile>{}, {}, {}, {},
        {}, {}, {}, {}, {}, selected_overlay);
    auto compiler = deployment::compile_cell_ir(applicability);
    auto qualified =
        deployment::qualify_deployment_for_retention(deployment, compiler);
    return deployment::test::RuntimeOnlyRetentionFixture{
        std::move(profile), std::move(deployment), std::move(compiler),
        std::move(qualified)};
  }();
  const deployment::test::RuntimeOnlyCompilerInputArtifactSource input_source(
      fixture.runtime_profile);
  deployment::FilesystemBuildManifestRepository manifests(root);
  deployment::FilesystemCatalogArtifactStore catalogs(root);
  deployment::FilesystemRawArtifactStore raw_artifacts(root);
  deployment::FilesystemRetainedBundleRepository bundles(root);
  auto bundle = deployment::detail::RetainedBundleMaterializer::
      materialize_store_and_reload_production(fixture.qualified, manifests,
                                              input_source, {}, catalogs,
                                              raw_artifacts, bundles);
  if (bundle == nullptr) {
    throw std::runtime_error(
        "runtime-convergence fixture produced no retained bundle");
  }
  return {
      std::move(bundle),
      std::make_unique<deployment::FilesystemVerifiedRetainedBundleSource>(
          root),
      std::make_unique<
          deployment::FilesystemVerifiedRetainedProductArtifactSource>(root),
  };
}

[[nodiscard]] RuntimePackage
MaterializeNativeRuntimePackage(const std::filesystem::path &root,
                                bool shared_domain = false) {
  static_cast<void>(std::filesystem::create_directories(root));
  auto fixture = shared_domain
      ? deployment::test::make_lite6_controllable_retention_fixture(
            false, false, 2U, false, true, "runtime_only_cell", false,
            false, "resource", false, {}, {}, false, false, {}, false,
            std::nullopt, true)
      : deployment::test::make_lite6_native_motion_retention_fixture_for_cell(
            "runtime_only_cell");
  const deployment::test::Lite6ControllableCompilerInputArtifactSource
      input_source(fixture);
  deployment::FilesystemBuildManifestRepository manifests(root);
  deployment::FilesystemCatalogArtifactStore catalogs(root);
  deployment::FilesystemRawArtifactStore raw_artifacts(root);
  deployment::FilesystemRetainedBundleRepository bundles(root);
  auto bundle = deployment::detail::RetainedBundleMaterializer::
      materialize_store_and_reload_production(
          fixture.qualified, manifests, input_source,
          fixture.structural_preparations(), catalogs, raw_artifacts,
          bundles);
  if (bundle == nullptr) {
    throw std::runtime_error(
        "native runtime-convergence fixture produced no retained bundle");
  }
  return {
      std::move(bundle),
      std::make_unique<deployment::FilesystemVerifiedRetainedBundleSource>(
          root),
      std::make_unique<
          deployment::FilesystemVerifiedRetainedProductArtifactSource>(root),
  };
}

[[nodiscard]] ManagerApplyIntentDTO
ApplyIntent(const deployment::RetainedBundle &bundle) {
  const auto &graph = bundle.value().cell_graph;
  return {
      PublicRequestContextDTO{
          RequestId("request-1"),
          RequestOwnerEpoch("manager-owner-1"),
          "runtime-convergence-test",
          std::nullopt,
      },
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      graph.cell_id,
      ProjectionRevisionDTO{
          bundle.retained_bundle_id(),
          graph.graph_revision,
          SceneRevision{
              SceneId(std::string(kSceneId)),
              1U,
              ContentDigest(std::string(kDigestOne)),
          },
      },
      bundle.retained_bundle_id(),
      graph.graph_revision,
      ManagerApplyPreparedActivationDTO{
          ContentDigest(std::string(kDigestOne)),
          bundle.value().deployment_artifact_id,
          bundle.value().deployment_revision,
          ProgramBindingId(std::string(kDigestOne)),
          ContentDigest(std::string(kDigestTwo)),
          "{}",
      },
      AdmissionGeneration(7U),
      std::nullopt,
  };
}

[[nodiscard]] ActiveBundlePublicationReceiptDTO
PredecessorPublication(const ManagerApplyIntentDTO &apply_intent) {
  return {
      ActiveBundlePublicationRequestDTO{
          BootstrapPublicationTransactionDTO{
              BootstrapId("bootstrap-1"),
              BootstrapGeneration(1U),
          },
          apply_intent.expected_active_projection_revision,
          apply_intent.expected_active_projection_revision.retained_bundle_id,
          apply_intent.expected_active_projection_revision.graph_revision,
          std::nullopt,
          ContentDigest(std::string(kDigestOne)),
      },
      ActiveBundlePublicationGeneration(3U),
      ContentDigest(std::string(kDigestTwo)),
  };
}

[[nodiscard]] ExecutionHostEpochDTO HostEpoch() {
  return {
      RuntimeUnitId("runtime.kernel"),
      RuntimeEpoch("kernel-runtime-1"),
      RequestOwnerEpoch("kernel-owner-1"),
      ContentDigest(std::string(kDigestOne)),
      ActiveBundlePublicationGeneration(3U),
  };
}

class CurrentPublicationReader final : public IActiveBundlePublicationReader {
public:
  explicit CurrentPublicationReader(ActiveBundlePublicationReceiptDTO receipt)
      : receipt_(std::move(receipt)) {}

  [[nodiscard]] Result<ActiveBundlePublicationReceiptDTO>
  ReadActiveBundlePublication(
      const ActiveBundlePublicationGeneration generation) const override {
    ++read_calls;
    if ((fail_on_call > 0 && read_calls == fail_on_call) ||
        generation != receipt_.publication_generation) {
      return Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
          "publication_not_current", "publication is not current", {}});
    }
    return Result<ActiveBundlePublicationReceiptDTO>::Ok(receipt_);
  }

  mutable int read_calls{0};
  int fail_on_call{0};

private:
  ActiveBundlePublicationReceiptDTO receipt_;
};

class ScriptedPublicationStore final : public IActiveBundlePublicationStore {
public:
  explicit ScriptedPublicationStore(
      ActiveBundlePublicationReceiptDTO predecessor,
      SqliteManagerApplyJournal *independent_reader = nullptr)
      : current_(std::move(predecessor)),
        independent_reader_(independent_reader) {}

  [[nodiscard]] ActiveBundlePublicationReceiptDTO PublishActiveBundle(
      const ActiveBundlePublicationRequestDTO &request) override {
    ++publish_calls;
    if (!request.valid() ||
        request.expected_previous_publication_generation !=
            std::optional<ActiveBundlePublicationGeneration>{
                current_.publication_generation}) {
      throw std::runtime_error("publication fixture rejected stale request");
    }
    const auto *named = std::get_if<NamedApplyPublicationTransactionDTO>(
        &request.transaction_ref);
    if (independent_reader_ != nullptr && named != nullptr) {
      const auto materialization =
          independent_reader_->ReadPrivateMaterialization(
              named->apply_id, named->apply_generation);
      saw_prerequisites_before_publish =
          materialization.has_value() &&
          materialization->private_materialization_content_digest ==
              request.private_materialization_content_digest;
    }
    current_ = {
        request,
        ActiveBundlePublicationGeneration(
            current_.publication_generation.value() + 1U),
        ContentDigest(std::string(kDigestOne)),
    };
    if (throw_after_publish_effect) {
      fail_next_read = true;
      throw std::runtime_error("simulated lost publication response");
    }
    return current_;
  }

  [[nodiscard]] Result<ActiveBundlePublicationReceiptDTO>
  ReadActiveBundlePublication(
      const ActiveBundlePublicationGeneration generation) const override {
    ++read_calls;
    if (fail_next_read) {
      fail_next_read = false;
      return Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
          "publication_read_uncertain",
          "simulated unavailable publication read",
          {},
      });
    }
    if (generation != current_.publication_generation) {
      return Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
          "publication_not_current", "publication is not current", {}});
    }
    return Result<ActiveBundlePublicationReceiptDTO>::Ok(current_);
  }

  bool throw_after_publish_effect{false};
  bool saw_prerequisites_before_publish{false};
  mutable bool fail_next_read{false};
  int publish_calls{0};
  mutable int read_calls{0};

private:
  ActiveBundlePublicationReceiptDTO current_;
  SqliteManagerApplyJournal *independent_reader_;
};

class FixedActiveRuntimePlanSource final
    : public IManagerActiveRuntimePlanSource {
public:
  FixedActiveRuntimePlanSource(
      ActiveBundlePublicationReceiptDTO publication,
      std::vector<RuntimeUnitConvergencePlanRowDTO> runtime_units)
      : plan_{std::move(publication), ContentDigest(std::string(kDigestOne)),
              std::move(runtime_units)} {
    if (!plan_.valid()) {
      throw std::runtime_error("fixed active runtime plan is invalid");
    }
  }

  [[nodiscard]] ManagerActiveRuntimePlanDTO ReadCurrentPlan() const override {
    ++read_calls;
    return plan_;
  }

  mutable int read_calls{0};

private:
  ManagerActiveRuntimePlanDTO plan_;
};

class AlwaysIdleMaintenanceGate final : public IPlannedMaintenanceGate {
public:
  [[nodiscard]] ManagementIdleState ReadIdleState() const override {
    return ManagementIdleState::kIdle;
  }

  [[nodiscard]] bool CloseAdmissionAndProveClosed() override { return true; }
};

class ExactFencePort final : public IManagerKernelApplyFencePort {
public:
  [[nodiscard]] ConfigurationFenceAdmissionSnapshotDTO
  ObserveAdmission(const ConfigurationFenceAdmissionQueryDTO &) const override {
    throw std::runtime_error("admission observation is outside this fixture");
  }

  [[nodiscard]] ConfigurationFenceAcceptedDTO
  Acquire(const ApplyConfigurationFenceIntentDTO &intent) override {
    ++acquire_calls;
    return {
        ConfigurationFenceId("fence-1"),
        ConfigurationFenceGeneration(intent.expected_fence_generation.value() +
                                     1U),
        intent.scope,
        100,
        RequestReplayStatusDTO::kAcceptedNew,
    };
  }

  [[nodiscard]] ApplyFenceAcquireAttemptReconcileResultDTO
  ReconcileAcquireAttempt(
      const ApplyFenceAcquireAttemptQueryDTO &query) const override {
    ++reconcile_calls;
    return {
        HostEpoch(),
        query,
        ApplyFenceAcquireAttemptReconcileState::kNotSeen,
        std::nullopt,
        false,
        "",
        ContentDigest(std::string(kDigestOne)),
        std::nullopt,
    };
  }

  [[nodiscard]] ApplyFenceReconcileResultDTO
  Rehydrate(const ApplyFenceRehydrateDTO &request) override {
    ++rehydrate_calls;
    return {
        HostEpoch(),
        {ApplyFenceReconcileEntryDTO{
            request.persisted_configuration_fence_id,
            request.persisted_configuration_fence_generation,
            ApplyFenceReconcileState::kMatched,
            std::nullopt,
            std::nullopt,
        }},
        false,
        "",
        ContentDigest(std::string(kDigestOne)),
        std::nullopt,
    };
  }

  [[nodiscard]] ConfigurationFenceStatusDTO
  QueryStatus(const ConfigurationFenceStatusQueryDTO &) const override {
    throw std::runtime_error("release status is outside this fixture");
  }

  [[nodiscard]] ConfigurationFenceReleasedDTO
  Release(const ConfigurationFenceReleaseDTO &) override {
    throw std::runtime_error("release is outside this fixture");
  }

  int acquire_calls{0};
  mutable int reconcile_calls{0};
  int rehydrate_calls{0};
};

class ScriptedFenceReleasePort final : public IManagerKernelApplyFencePort {
public:
  explicit ScriptedFenceReleasePort(
      SqliteManagerApplyJournal &independent_reader)
      : independent_reader_(independent_reader) {}

  [[nodiscard]] ConfigurationFenceAdmissionSnapshotDTO
  ObserveAdmission(const ConfigurationFenceAdmissionQueryDTO &) const override {
    throw std::runtime_error("admission observation is outside this fixture");
  }

  [[nodiscard]] ConfigurationFenceAcceptedDTO
  Acquire(const ApplyConfigurationFenceIntentDTO &) override {
    throw std::runtime_error("Acquire is outside the release fixture");
  }

  [[nodiscard]] ApplyFenceAcquireAttemptReconcileResultDTO
  ReconcileAcquireAttempt(
      const ApplyFenceAcquireAttemptQueryDTO &) const override {
    throw std::runtime_error(
        "Acquire reconciliation is outside the release fixture");
  }

  [[nodiscard]] ApplyFenceReconcileResultDTO
  Rehydrate(const ApplyFenceRehydrateDTO &) override {
    throw std::runtime_error("rehydration is outside the release fixture");
  }

  [[nodiscard]] ConfigurationFenceStatusDTO
  QueryStatus(const ConfigurationFenceStatusQueryDTO &query) const override {
    ++status_calls;
    const auto publication = independent_reader_.ReadPublicationReceipt(
        query.owner_transaction.apply_id,
        query.owner_transaction.apply_generation);
    if (!publication.has_value()) {
      throw std::runtime_error("release fixture lacks completed publication");
    }
    const auto &receipt = publication->publication.publication_receipt;
    const auto &candidate = receipt.request.candidate_projection_revision;
    ConfigurationFenceReconciliationEvidenceDTO evidence{
        ExecutionHostEpochDTO{
            RuntimeUnitId("runtime.kernel"),
            RuntimeEpoch("kernel-runtime-release-1"),
            RequestOwnerEpoch("kernel-owner-release-1"),
            ContentDigest(std::string(kDigestOne)),
            receipt.publication_generation,
        },
        query.owner_transaction,
        query.configuration_fence_id,
        query.configuration_fence_generation,
        query.expected_scope,
        1U,
        candidate.graph_revision,
        candidate,
        receipt.publication_generation,
        foreign_public_pointer ? ContentDigest(std::string(kDigestTwo))
                               : receipt.durable_public_pointer_content_digest,
        {},
        0U,
        0U,
        ContentDigest(std::string(kDigestTwo)),
    };
    if (state == ConfigurationFenceState::kReleased && !released_.has_value()) {
      throw std::runtime_error("released fixture lacks its exact receipt");
    }
    return {
        query,
        std::move(evidence),
        state,
        state != ConfigurationFenceState::kReleased,
        state == ConfigurationFenceState::kReleased ? released_ : std::nullopt,
        state == ConfigurationFenceState::kReleasePending
            ? "complete explicit Kernel release recovery"
            : "",
        ContentDigest(std::string(kDigestOne)),
        state == ConfigurationFenceState::kReleasePending
            ? std::optional<ErrorDTO>{ErrorDTO{
                  "fence_release_pending",
                  "Kernel cannot prove the release outcome",
                  {},
              }}
            : std::nullopt,
    };
  }

  [[nodiscard]] ConfigurationFenceReleasedDTO
  Release(const ConfigurationFenceReleaseDTO &request) override {
    ++release_calls;
    const auto &owner = request.reconciliation_evidence.owner_transaction;
    const auto intent = independent_reader_.ReadFenceReleaseIntent(
        owner.apply_id, owner.apply_generation);
    saw_durable_intent_before_release =
        intent.has_value() &&
        intent->fence_release_intent.release_request == request;
    released_ = ConfigurationFenceReleasedDTO{
        request,
        request.reconciliation_evidence.execution_host_epoch,
        1'234'567LL,
        RequestReplayStatusDTO::kAcceptedNew,
        ContentDigest(std::string(kDigestTwo)),
    };
    state = ConfigurationFenceState::kReleased;
    if (throw_after_release_effect) {
      throw std::runtime_error("simulated lost Release response");
    }
    return *released_;
  }

  ConfigurationFenceState state{ConfigurationFenceState::kHeld};
  bool throw_after_release_effect{false};
  bool foreign_public_pointer{false};
  bool saw_durable_intent_before_release{false};
  mutable int status_calls{0};
  int release_calls{0};

private:
  SqliteManagerApplyJournal &independent_reader_;
  std::optional<ConfigurationFenceReleasedDTO> released_;
};

class AggregateFencePort final : public IManagerKernelApplyFencePort {
public:
  AggregateFencePort(SqliteManagerApplyJournal &independent_reader,
                     ActiveBundlePublicationReceiptDTO predecessor)
      : independent_reader_(independent_reader),
        predecessor_(std::move(predecessor)) {}

  [[nodiscard]] ConfigurationFenceAdmissionSnapshotDTO ObserveAdmission(
      const ConfigurationFenceAdmissionQueryDTO &query) const override {
    ++observe_admission_calls;
    return {
        query,
        ExecutionHostEpochDTO{
            RuntimeUnitId("runtime.kernel"),
            RuntimeEpoch("kernel-runtime-aggregate-1"),
            RequestOwnerEpoch("kernel-owner-aggregate-1"),
            ContentDigest(std::string(kDigestOne)),
            predecessor_.publication_generation,
        },
        predecessor_.request.candidate_projection_revision,
        predecessor_.durable_public_pointer_content_digest,
        0U,
        ConfigurationFenceGeneration(0U),
        {},
        0U,
        0U,
        ContentDigest(std::string(kDigestTwo)),
    };
  }

  [[nodiscard]] ConfigurationFenceAcceptedDTO
  Acquire(const ApplyConfigurationFenceIntentDTO &intent) override {
    ++acquire_calls;
    accepted_ = ConfigurationFenceAcceptedDTO{
        ConfigurationFenceId("fence.aggregate.1"),
        ConfigurationFenceGeneration(intent.expected_fence_generation.value() +
                                     1U),
        intent.scope,
        100U,
        RequestReplayStatusDTO::kAcceptedNew,
    };
    return *accepted_;
  }

  [[nodiscard]] ApplyFenceAcquireAttemptReconcileResultDTO
  ReconcileAcquireAttempt(
      const ApplyFenceAcquireAttemptQueryDTO &query) const override {
    ++reconcile_calls;
    return {
        HostEpoch(),
        query,
        accepted_.has_value()
            ? ApplyFenceAcquireAttemptReconcileState::kAcceptedExact
            : ApplyFenceAcquireAttemptReconcileState::kNotSeen,
        accepted_,
        false,
        "",
        ContentDigest(std::string(kDigestOne)),
        std::nullopt,
    };
  }

  [[nodiscard]] ApplyFenceReconcileResultDTO
  Rehydrate(const ApplyFenceRehydrateDTO &request) override {
    ++rehydrate_calls;
    if (!accepted_.has_value()) {
      throw std::runtime_error(
          "aggregate fence cannot rehydrate before Acquire");
    }
    return {
        HostEpoch(),
        {ApplyFenceReconcileEntryDTO{
            request.persisted_configuration_fence_id,
            request.persisted_configuration_fence_generation,
            ApplyFenceReconcileState::kMatched,
            std::nullopt,
            std::nullopt,
        }},
        false,
        "",
        ContentDigest(std::string(kDigestOne)),
        std::nullopt,
    };
  }

  [[nodiscard]] ConfigurationFenceStatusDTO
  QueryStatus(const ConfigurationFenceStatusQueryDTO &query) const override {
    ++status_calls;
    const auto publication = independent_reader_.ReadPublicationReceipt(
        query.owner_transaction.apply_id,
        query.owner_transaction.apply_generation);
    if (!publication.has_value()) {
      throw std::runtime_error(
          "aggregate fence status lacks completed publication");
    }
    const auto &receipt = publication->publication.publication_receipt;
    const auto &candidate = receipt.request.candidate_projection_revision;
    ConfigurationFenceReconciliationEvidenceDTO evidence{
        ExecutionHostEpochDTO{
            RuntimeUnitId("runtime.kernel"),
            RuntimeEpoch("kernel-runtime-aggregate-1"),
            RequestOwnerEpoch("kernel-owner-aggregate-1"),
            ContentDigest(std::string(kDigestOne)),
            receipt.publication_generation,
        },
        query.owner_transaction,
        query.configuration_fence_id,
        query.configuration_fence_generation,
        query.expected_scope,
        1U,
        candidate.graph_revision,
        candidate,
        receipt.publication_generation,
        receipt.durable_public_pointer_content_digest,
        {},
        0U,
        0U,
        ContentDigest(std::string(kDigestTwo)),
    };
    return {
        query,
        std::move(evidence),
        released_.has_value() ? ConfigurationFenceState::kReleased
                              : ConfigurationFenceState::kHeld,
        !released_.has_value(),
        released_,
        "",
        ContentDigest(std::string(kDigestOne)),
        std::nullopt,
    };
  }

  [[nodiscard]] ConfigurationFenceReleasedDTO
  Release(const ConfigurationFenceReleaseDTO &request) override {
    ++release_calls;
    released_ = ConfigurationFenceReleasedDTO{
        request,
        request.reconciliation_evidence.execution_host_epoch,
        1'234'567LL,
        RequestReplayStatusDTO::kAcceptedNew,
        ContentDigest(std::string(kDigestTwo)),
    };
    return *released_;
  }

  mutable int observe_admission_calls{0};
  int acquire_calls{0};
  mutable int reconcile_calls{0};
  int rehydrate_calls{0};
  mutable int status_calls{0};
  int release_calls{0};

private:
  SqliteManagerApplyJournal &independent_reader_;
  ActiveBundlePublicationReceiptDTO predecessor_;
  std::optional<ConfigurationFenceAcceptedDTO> accepted_;
  std::optional<ConfigurationFenceReleasedDTO> released_;
};

class ScriptedRuntimePorts final : public IRuntimeUnitProcessPort,
                                   public IRuntimeUnitProgressPort {
public:
  explicit ScriptedRuntimePorts(SqliteManagerApplyJournal &independent_reader)
      : independent_reader_(independent_reader) {}

  [[nodiscard]] ProcessPresenceObservation
  ObserveProcess(const RuntimeUnitConvergenceQueryDTO &query) override {
    ++observe_calls;
    last_query = query;
    return Presence(query);
  }

  [[nodiscard]] ProcessPresenceObservation
  EnsureRunning(const RuntimeUnitConvergenceQueryDTO &query) override {
    ++ensure_calls;
    const auto *owner =
        std::get_if<RuntimeUnitConvergenceApplyOwnerDTO>(&query.owner);
    if (owner == nullptr) {
      throw std::invalid_argument(
          "named Apply fixture received a bootstrap runtime query");
    }
    const auto durable = independent_reader_.ReadRuntimeConvergenceIntent(
        owner->apply_id, owner->apply_generation);
    saw_durable_intent_before_ensure =
        durable.has_value() &&
        durable->runtime_convergence_intent_digest ==
            query.runtime_convergence_intent_digest &&
        durable->runtime_convergence_intent.runtime_units.size() == 1U &&
        durable->runtime_convergence_intent.runtime_units.front() == query.plan;
    if (state_ != ProcessPresenceState::kPresent) {
      ++start_effect_calls;
    }
    state_ = ProcessPresenceState::kPresent;
    ++observation_sequence_;
    if (throw_after_ensure_effect) {
      throw std::runtime_error("simulated lost process-start response");
    }
    return Presence(query);
  }

  [[nodiscard]] ProcessPresenceObservation
  EnsureStopped(const RuntimeUnitConvergenceQueryDTO &query) override {
    ++stop_calls;
    if (!stop_remains_present) {
      state_ = ProcessPresenceState::kAbsent;
    }
    ++observation_sequence_;
    return Presence(query);
  }

  [[nodiscard]] std::optional<RuntimeUnitSourceReport>
  ObserveProgress(const RuntimeUnitConvergenceQueryDTO &query,
                  const RuntimeEpoch &runtime_epoch) override {
    ++progress_calls;
    if (!report_available) {
      return std::nullopt;
    }
    if (runtime_epoch.value() != process_incarnation_) {
      throw std::runtime_error("progress query has the wrong runtime epoch");
    }
    std::vector<std::string> roles;
    for (const auto &role : query.plan.required_runtime_role_ids) {
      roles.push_back(role.value());
    }
    last_report = RuntimeUnitSourceReport(
        query.plan.runtime_unit_id.value(),
        progress_report_incarnation.value_or(process_incarnation_), 1U, 1U,
        RuntimeSourcePhase::kServing, RuntimeSourceHealth::kHealthy,
        query.plan.product_identity.value(),
        progress_configuration_identity.value_or(
            query.plan.configuration_identity.value()),
        std::move(roles), ManagedCommandSurfaceState::kClosedNoActiveEffect, {},
        progress_offer_digest.value_or(
            query.plan.runtime_contract_offer_digest.value()));
    return last_report;
  }

  void ReplaceProcess(std::string process_incarnation) {
    process_incarnation_ = std::move(process_incarnation);
    state_ = ProcessPresenceState::kPresent;
    ++observation_sequence_;
  }

  void LoseProcess() {
    state_ = ProcessPresenceState::kAbsent;
    ++observation_sequence_;
  }

  void RefreshCurrentProcessObservation() { ++observation_sequence_; }

  bool throw_after_ensure_effect{false};
  bool stop_remains_present{false};
  bool report_available{true};
  bool saw_durable_intent_before_ensure{false};
  std::optional<std::string> progress_report_incarnation;
  std::optional<std::string> progress_configuration_identity;
  std::optional<std::string> progress_offer_digest;
  std::optional<RuntimeUnitSourceReport> last_report;
  int observe_calls{0};
  int ensure_calls{0};
  int start_effect_calls{0};
  int progress_calls{0};
  int stop_calls{0};
  std::optional<RuntimeUnitConvergenceQueryDTO> last_query;

private:
  [[nodiscard]] ProcessPresenceObservation
  Presence(const RuntimeUnitConvergenceQueryDTO &query) const {
    return ProcessPresenceObservation(
        query.plan.runtime_unit_id.value(), "process-observer-1",
        observation_sequence_, state_,
        state_ == ProcessPresenceState::kPresent ? process_incarnation_ : "");
  }

  SqliteManagerApplyJournal &independent_reader_;
  ProcessPresenceState state_{ProcessPresenceState::kAbsent};
  std::uint64_t observation_sequence_{1U};
  std::string process_incarnation_{"process-epoch-1"};
};

class ScriptedRuntimeCompositionPort final
    : public IManagerSelectedRuntimeCompositionPort {
public:
  [[nodiscard]] ManagerSelectedRuntimeCompositionResultDTO InstallOrObserve(
      const ManagerSelectedRuntimeCompositionRequestDTO &request) override {
    ++calls;
    phases.push_back(request.phase);
    last_request = request;
    auto response_request = request;
    if (foreign_response) {
      response_request.manager_runtime_epoch = RuntimeEpoch("foreign-manager");
    }
    const auto preparing = prepare_once && calls == 1;
    if (preparing) {
      return {std::move(response_request),
              ManagerSelectedRuntimeCompositionState::kPreparing,
              {}};
    }
    return {std::move(response_request),
            ManagerSelectedRuntimeCompositionState::kInstalled,
            omit_receipts ? std::vector<ManagerRuntimeComponentReceiptDTO>{}
                          : Receipts(request)};
  }

  // What a conforming host reports: one live-owner receipt per retained,
  // added or replaced component, re-sealing retained controllers when the
  // product changed.
  [[nodiscard]] static std::vector<ManagerRuntimeComponentReceiptDTO>
  Receipts(const ManagerSelectedRuntimeCompositionRequestDTO &request) {
    const auto product_changed =
        request.predecessor_projection_revision.retained_bundle_id !=
            request.candidate_projection_revision.retained_bundle_id ||
        request.predecessor_projection_revision.graph_revision !=
            request.candidate_projection_revision.graph_revision;
    std::vector<ManagerRuntimeComponentReceiptDTO> receipts;
    for (const auto &change : request.component_delta.components) {
      if (change.disposition ==
          deployment::ApplyComponentDisposition::kRemove) {
        continue;
      }
      const auto retained =
          change.disposition == deployment::ApplyComponentDisposition::kRetain;
      receipts.push_back({change.controllable_component_id,
                          change.disposition,
                          change.candidate_runtime_identity,
                          {{request.runtime_unit_id, request.runtime_epoch}},
                          retained,
                          retained && product_changed});
    }
    return receipts;
  }

  bool prepare_once{false};
  bool foreign_response{false};
  bool omit_receipts{false};
  int calls{0};
  std::vector<ManagerSelectedRuntimeCompositionPhase> phases;
  std::optional<ManagerSelectedRuntimeCompositionRequestDTO> last_request;
};

class ScriptedCellAdmissionPort final : public core::IKernelCellAdmissionPort {
public:
  [[nodiscard]] core::AuthoringAdmissionDTO
  GetAuthoringAdmission() const override {
    return {held_claim_.has_value() ? false : true,
            core::AdmissionGeneration(generation_)};
  }

  [[nodiscard]] core::Result<core::AuthoringAdmissionDTO>
  UpdateCellAdmissionReadiness(
      const core::CellAdmissionReadinessSnapshotDTO &) override {
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        GetAuthoringAdmission());
  }

  [[nodiscard]] core::Result<core::CellAdmissionClaimDTO>
  AcquireCellAdmissionClaim(
      const core::CellAdmissionClaimIntentDTO &intent) override {
    if (!held_claim_.has_value()) {
      held_claim_ = core::CellAdmissionClaimDTO{
          intent, core::CellAdmissionClaimGeneration(1U),
          intent.expected_admission_generation, 1};
      ++generation_;
    }
    return core::Result<core::CellAdmissionClaimDTO>::Ok(*held_claim_);
  }

  [[nodiscard]] core::Result<core::AuthoringAdmissionDTO>
  ReleaseCellAdmissionClaim(
      const core::CellAdmissionClaimReleaseDTO &release) override {
    last_release = release;
    released_claim_ = held_claim_;
    held_claim_.reset();
    ++generation_;
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        GetAuthoringAdmission());
  }

  [[nodiscard]] core::Result<core::AuthoringAdmissionDTO>
  ReconcileCellAdmissionClaims(
      const core::CellAdmissionReconcileDTO &) override {
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        GetAuthoringAdmission());
  }

  [[nodiscard]] core::Result<core::CellAdmissionRecoveryReceiptDTO>
  GetCellAdmissionRecoveryReceipt(
      const core::CellAdmissionRecoveryReceiptQueryDTO &query) const override {
    core::CellAdmissionRecoveryReceiptDTO receipt{
        core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown,
        core::RequestOwnerEpoch("kernel-owner.test"),
        core::AdmissionGeneration(generation_),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        {},
        {},
    };
    if (held_claim_.has_value() && held_claim_->intent == query.intent) {
      receipt.status = core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact;
      receipt.matched_claim = held_claim_;
    } else if (released_claim_.has_value() &&
               released_claim_->intent == query.intent) {
      receipt.status =
          core::CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact;
      receipt.matched_claim = released_claim_;
    }
    return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Ok(
        std::move(receipt));
  }

  std::optional<core::CellAdmissionClaimReleaseDTO> last_release;

private:
  std::uint64_t generation_{7U};
  std::optional<core::CellAdmissionClaimDTO> held_claim_;
  std::optional<core::CellAdmissionClaimDTO> released_claim_;
};

class MaintenanceSceneOwner final
    : public core::IManagerSceneProjectionMaintenancePort {
public:
  explicit MaintenanceSceneOwner(core::ProjectionRevisionDTO current)
      : current_(std::move(current)) {}

  [[nodiscard]] core::ProjectionRevisionDTO
  CurrentProjectionRevision() const override {
    return current_;
  }

  [[nodiscard]] core::ProjectionMaintenanceInitializationStatusDTO
  BeginMaintenanceInitialization(
      const core::ProjectionMaintenanceInitializationRequestDTO &request)
      override {
    ++begin_calls;
    last_request = request;
    if (!request.valid()) {
      throw std::runtime_error("scene fixture received an invalid request");
    }
    const core::ProjectionRevisionDTO target{
        request.candidate_retained_bundle.retained_bundle_id,
        request.candidate_retained_bundle.cell_graph.graph_revision,
        request.expected_active_projection_revision.scene_revision,
    };
    status_ = core::ProjectionMaintenanceInitializationStatusDTO{
        request.apply_id,
        request.apply_generation,
        core::ProjectionMaintenanceInitializationState::kInitialized,
        request.expected_active_projection_revision,
        target,
        core::LogicalSceneSnapshotIdentityDTO{
            target.scene_revision,
            request.candidate_retained_bundle.cell_graph.cell_id,
        },
        request.native_runtime_bindings,
        std::nullopt,
        std::nullopt,
        false,
        false,
    };
    return *status_;
  }

  [[nodiscard]] core::ProjectionMaintenanceInitializationStatusDTO
  InstallInitialScene(
      const core::ProjectionMaintenanceInitializationRequestDTO &request)
      override {
    ++install_scene_calls;
    if (!status_.has_value() || !last_request.has_value() ||
        request != *last_request || status_->initial_scene_installed) {
      throw std::runtime_error("scene fixture received foreign baseline");
    }
    ++status_->target_projection_revision.scene_revision.revision_id;
    status_->authoritative_snapshot_identity.revision =
        status_->target_projection_revision.scene_revision;
    status_->initial_scene_installed = true;
    return *status_;
  }

  [[nodiscard]] core::ProjectionMaintenanceInitializationStatusDTO
  CompleteMaintenanceInitialization(
      const core::ProjectionMaintenanceCompletionDTO &completion) override {
    ++complete_calls;
    if (!status_.has_value() || !completion.valid() ||
        completion.apply_id != status_->apply_id ||
        completion.apply_generation != status_->apply_generation ||
        completion.target_projection_revision !=
            status_->target_projection_revision ||
        completion.authoritative_snapshot_identity !=
            status_->authoritative_snapshot_identity) {
      throw std::runtime_error("scene fixture received foreign completion");
    }
    status_->state =
        core::ProjectionMaintenanceInitializationState::kCompleted;
    status_->publication_receipt = completion.publication_receipt;
    current_ = status_->target_projection_revision;
    return *status_;
  }

  [[nodiscard]] std::optional<
      core::ProjectionMaintenanceInitializationStatusDTO>
  ReadMaintenanceInitialization(
      const core::ProjectionMaintenanceInitializationQueryDTO &query) const
      override {
    ++read_calls;
    if (!status_.has_value() || status_->apply_id != query.apply_id ||
        status_->apply_generation != query.apply_generation) {
      return std::nullopt;
    }
    return status_;
  }

  [[nodiscard]] core::ProjectionAggregateHealthDTO
  ActiveProjectionHealth() const override {
    if (!status_.has_value()) {
      throw std::runtime_error(
          "scene fixture has no maintenance projection health");
    }
    return {
        core::MaintenanceProjectionSetSourceDTO{
            status_->apply_id, status_->apply_generation,
            status_->target_projection_revision},
        core::ProjectionHealthState::kHealthy,
        {},
        std::nullopt,
    };
  }

  int begin_calls{0};
  int install_scene_calls{0};
  int complete_calls{0};
  mutable int read_calls{0};
  std::optional<core::ProjectionMaintenanceInitializationRequestDTO>
      last_request;

private:
  core::ProjectionRevisionDTO current_;
  std::optional<core::ProjectionMaintenanceInitializationStatusDTO> status_;
};

struct CoordinatorFixture final {
  explicit CoordinatorFixture(const std::filesystem::path &root)
      : package(MaterializeRuntimePackage(root)),
        apply_intent(ApplyIntent(*package.bundle)),
        journal(root / "apply.sqlite3",
                package.bundle->value().cell_graph.cell_id),
        independent_reader(root / "apply.sqlite3",
                           package.bundle->value().cell_graph.cell_id),
        publication(PredecessorPublication(apply_intent)),
        runtime_ports(independent_reader),
        fence_coordinator(journal, kernel, RuntimeEpoch("manager-runtime-1")),
        coordinator(journal, publication,
                    *package.artifact_source, runtime_ports, runtime_ports,
                    "linux", "posix.1-2017", [] { return 1'000U; }) {}

  [[nodiscard]] ManagerApplyRuntimeConvergenceResult Run() {
    if (!journal.ReadFenceAcceptance(apply_intent.apply_id,
                                     apply_intent.apply_generation)
             .has_value()) {
      static_cast<void>(fence_coordinator.AcquireOrRecoverFence(
          apply_intent, *package.bundle, *package.bundle,
          ManagerApplyFenceExpectationDTO{
              0U, ConfigurationFenceGeneration(0U), {}}));
    }
    return coordinator.ConvergeOrRecover(apply_intent, *package.bundle,
                                         *package.bundle,
                                         ActiveBundlePublicationGeneration(3U));
  }

  RuntimePackage package;
  ManagerApplyIntentDTO apply_intent;
  SqliteManagerApplyJournal journal;
  SqliteManagerApplyJournal independent_reader;
  CurrentPublicationReader publication;
  ExactFencePort kernel;
  ScriptedRuntimePorts runtime_ports;
  ManagerApplyFenceCoordinator fence_coordinator;
  ManagerApplyRuntimeConvergenceCoordinator coordinator;
};

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     MissingAcceptedFenceRejectsBeforeRuntimeEffect) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());

  EXPECT_THROW(static_cast<void>(fixture.coordinator.ConvergeOrRecover(
                   fixture.apply_intent, *fixture.package.bundle,
                   *fixture.package.bundle,
                   ActiveBundlePublicationGeneration(3U))),
               ManagerRuntimeConvergenceCoordinatorError);
  EXPECT_EQ(fixture.kernel.acquire_calls, 0);
  EXPECT_EQ(fixture.kernel.rehydrate_calls, 0);
  EXPECT_EQ(fixture.runtime_ports.observe_calls, 0);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 0);
  EXPECT_FALSE(fixture.journal
                   .ReadRuntimeConvergenceIntent(ApplyId("apply-1"),
                                                 ApplyGeneration(1U))
                   .has_value());
}


TEST(ManagerApplyRuntimeConvergenceCoordinator,
     PersistsBeforeStartStagesAndReplaysWithoutASecondEffect) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());

  const auto first = fixture.Run();
  ASSERT_EQ(first.state, ManagerRuntimeConvergenceResultState::kStagedReady);
  EXPECT_TRUE(first.durable_runtime_intent.inserted);
  ASSERT_TRUE(first.durable_staged_ready.has_value());
  EXPECT_TRUE(first.durable_staged_ready->inserted);
  EXPECT_EQ(
      first.durable_staged_ready->record.staged_ready.staged_ready_generation,
      1U);
  ASSERT_EQ(first.durable_runtime_intent.record.runtime_convergence_intent
                .runtime_units.size(),
            1U);
  EXPECT_EQ(first.durable_runtime_intent.record.runtime_convergence_intent
                .runtime_units.front()
                .candidate_disposition,
            RuntimeUnitCandidateDisposition::kRetainedExact);
  EXPECT_TRUE(fixture.runtime_ports.saw_durable_intent_before_ensure);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 1);
  EXPECT_EQ(fixture.runtime_ports.start_effect_calls, 1);
  EXPECT_EQ(fixture.kernel.acquire_calls, 1);
  EXPECT_EQ(fixture.publication.read_calls, 3);

  const auto replay = fixture.Run();
  ASSERT_EQ(replay.state, ManagerRuntimeConvergenceResultState::kStagedReady);
  EXPECT_FALSE(replay.durable_runtime_intent.inserted);
  ASSERT_TRUE(replay.durable_staged_ready.has_value());
  EXPECT_FALSE(replay.durable_staged_ready->inserted);
  EXPECT_EQ(replay.durable_staged_ready->record,
            first.durable_staged_ready->record);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 1);
  EXPECT_EQ(fixture.runtime_ports.start_effect_calls, 1);
  EXPECT_EQ(fixture.kernel.acquire_calls, 1);
  EXPECT_EQ(fixture.kernel.rehydrate_calls, 1);
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     LostStartResponseRecoversByObservationWithoutDuplicateStart) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  fixture.runtime_ports.throw_after_ensure_effect = true;

  try {
    static_cast<void>(fixture.Run());
    FAIL() << "expected an effect-uncertain start result";
  } catch (const ManagerRuntimeProcessEffectUncertainError &error) {
    EXPECT_TRUE(error.recovery_query().valid());
    const auto *owner = std::get_if<RuntimeUnitConvergenceApplyOwnerDTO>(
        &error.recovery_query().owner);
    ASSERT_NE(owner, nullptr);
    EXPECT_EQ(owner->apply_id, fixture.apply_intent.apply_id);
  }
  EXPECT_TRUE(fixture.runtime_ports.saw_durable_intent_before_ensure);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 1);
  EXPECT_EQ(fixture.runtime_ports.start_effect_calls, 1);
  EXPECT_FALSE(
      fixture.journal
          .ReadLatestRuntimeStagedReady(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());

  fixture.runtime_ports.throw_after_ensure_effect = false;
  const auto recovered = fixture.Run();
  EXPECT_EQ(recovered.state,
            ManagerRuntimeConvergenceResultState::kStagedReady);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 2);
  EXPECT_EQ(fixture.runtime_ports.start_effect_calls, 1);
  EXPECT_EQ(fixture.kernel.acquire_calls, 1);
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     MissingProgressRemainsPendingWithoutStagedReadyEvidence) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  fixture.runtime_ports.report_available = false;

  const auto result = fixture.Run();
  EXPECT_EQ(result.state, ManagerRuntimeConvergenceResultState::kPending);
  EXPECT_FALSE(result.durable_staged_ready.has_value());
  EXPECT_EQ(fixture.publication.read_calls, 2);
  EXPECT_FALSE(
      fixture.journal
          .ReadLatestRuntimeStagedReady(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     StaleRuntimeEpochEvidenceFailsAfterObservedStartWithoutStagedReady) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  fixture.runtime_ports.progress_report_incarnation = "stale-process-epoch";

  EXPECT_THROW(static_cast<void>(fixture.Run()),
               ManagerRuntimeConvergenceCoordinatorError);
  ASSERT_TRUE(fixture.runtime_ports.last_report.has_value());
  EXPECT_EQ(fixture.runtime_ports.last_report->process_incarnation(),
            "stale-process-epoch");
  EXPECT_EQ(fixture.runtime_ports.last_report->command_surface_state(),
            ManagedCommandSurfaceState::kClosedNoActiveEffect);
  EXPECT_EQ(fixture.kernel.acquire_calls, 1);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 1);
  EXPECT_EQ(fixture.runtime_ports.start_effect_calls, 1);
  EXPECT_EQ(fixture.runtime_ports.progress_calls, 1);
  EXPECT_TRUE(
      fixture.journal
          .ReadRuntimeConvergenceIntent(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
  EXPECT_FALSE(
      fixture.journal
          .ReadLatestRuntimeStagedReady(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
  const auto report = fixture.journal.VerifyIntegrity();
  EXPECT_EQ(report.fence_acceptance_count, 1);
  EXPECT_EQ(report.runtime_convergence_intent_count, 1);
  EXPECT_EQ(report.runtime_staged_ready_count, 0);
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     StaleRetainedEpochFailsClosedWithoutReplacementOrRelaunch) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  const auto first = fixture.Run();
  ASSERT_EQ(first.state, ManagerRuntimeConvergenceResultState::kStagedReady);
  ASSERT_TRUE(first.durable_staged_ready.has_value());
  ASSERT_TRUE(fixture.runtime_ports.last_report.has_value());
  const auto prior_report = *fixture.runtime_ports.last_report;
  fixture.runtime_ports.ReplaceProcess("process-epoch-2");

  EXPECT_THROW(static_cast<void>(fixture.Run()),
               ManagerRuntimeConvergenceCoordinatorError);
  EXPECT_EQ(prior_report.process_incarnation(), "process-epoch-1");
  EXPECT_EQ(prior_report.source_report_sequence(), 1U);
  EXPECT_EQ(
      prior_report.offered_runtime_contract_offer_digest(),
      first.durable_staged_ready->record.staged_ready.runtime_units.front()
          .observed_runtime_contract_offer_digest.value());
  EXPECT_EQ(fixture.kernel.acquire_calls, 1);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 1);
  EXPECT_EQ(fixture.runtime_ports.start_effect_calls, 1);
  EXPECT_EQ(fixture.runtime_ports.progress_calls, 1);
  const auto latest = fixture.journal.ReadLatestRuntimeStagedReady(
      ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(*latest, first.durable_staged_ready->record);
  ASSERT_EQ(latest->staged_ready.runtime_units.size(), 1U);
  EXPECT_EQ(latest->staged_ready.runtime_units.front().command_surface_state,
            ManagedCommandSurfaceState::kClosedNoActiveEffect);
  const auto report = fixture.journal.VerifyIntegrity();
  EXPECT_EQ(report.runtime_convergence_intent_count, 1);
  EXPECT_EQ(report.runtime_staged_ready_count, 1);
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     MissingRetainedEpochFailsClosedWithoutRelaunch) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  const auto first = fixture.Run();
  ASSERT_EQ(first.state, ManagerRuntimeConvergenceResultState::kStagedReady);
  fixture.runtime_ports.LoseProcess();

  try {
    static_cast<void>(fixture.Run());
    FAIL() << "expected terminal retained component loss";
  } catch (const ManagerRuntimeConvergenceCoordinatorError &error) {
    EXPECT_NE(std::string(error.what()).find("not currently present"),
              std::string::npos);
    EXPECT_NE(std::string(error.what()).find("process-epoch-1"),
              std::string::npos);
  }
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 1);
  EXPECT_EQ(fixture.runtime_ports.start_effect_calls, 1);
  EXPECT_EQ(fixture.journal.VerifyIntegrity().runtime_staged_ready_count, 1);
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     InjectedChildOfferMismatchFailsBeforeNewStagedReadyOrEligibility) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  // This is injected progress-port evidence. The retained POSIX/native
  // adapters derive the child echo from the sealed package and cannot
  // honestly emit this mismatch while package integrity holds.
  fixture.runtime_ports.progress_offer_digest = std::string(kDigestTwo);

  EXPECT_THROW(static_cast<void>(fixture.Run()),
               ManagerRuntimeConvergenceCoordinatorError);
  EXPECT_THROW(static_cast<void>(fixture.Run()),
               ManagerRuntimeConvergenceCoordinatorError);
  ASSERT_TRUE(fixture.runtime_ports.last_report.has_value());
  EXPECT_EQ(fixture.runtime_ports.last_report
                ->offered_runtime_contract_offer_digest(),
            std::string(kDigestTwo));
  ASSERT_TRUE(fixture.runtime_ports.last_query.has_value());
  EXPECT_NE(fixture.runtime_ports.last_report
                ->offered_runtime_contract_offer_digest(),
            fixture.runtime_ports.last_query->plan.runtime_contract_offer_digest
                .value());
  EXPECT_EQ(fixture.runtime_ports.last_report->command_surface_state(),
            ManagedCommandSurfaceState::kClosedNoActiveEffect);
  EXPECT_EQ(fixture.kernel.acquire_calls, 1);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 2);
  EXPECT_EQ(fixture.runtime_ports.start_effect_calls, 1);
  EXPECT_EQ(fixture.runtime_ports.progress_calls, 2);
  const auto intent = fixture.journal.ReadRuntimeConvergenceIntent(
      ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(intent.has_value());
  EXPECT_EQ(intent->runtime_convergence_intent.runtime_units.size(), 1U);
  EXPECT_EQ(intent->runtime_convergence_intent.runtime_units.front()
                .runtime_contract_offer_digest.value(),
            fixture.runtime_ports.last_query->plan.runtime_contract_offer_digest
                .value());
  EXPECT_EQ(intent->runtime_convergence_intent.runtime_units.front(),
            fixture.runtime_ports.last_query->plan);
  EXPECT_FALSE(
      fixture.journal
          .ReadLatestRuntimeStagedReady(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
  EXPECT_EQ(fixture.journal.VerifyIntegrity().runtime_staged_ready_count, 0);
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     ConfigurationIdentityMismatchRemainsUnavailableWithoutStagedReady) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  fixture.runtime_ports.progress_configuration_identity = std::string(
      "sha256:"
      "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

  const auto result = fixture.Run();
  EXPECT_EQ(result.state, ManagerRuntimeConvergenceResultState::kPending);
  ASSERT_TRUE(fixture.runtime_ports.last_query.has_value());
  const auto runtime_unit_id =
      fixture.runtime_ports.last_query->plan.runtime_unit_id.value();
  ASSERT_NE(result.observations.observation(runtime_unit_id), nullptr);
  const auto *observation = result.observations.observation(runtime_unit_id);
  EXPECT_FALSE(observation->converged());
  EXPECT_EQ(
      observation->reason_codes(),
      std::vector<std::string>{"runtime_configuration_identity_mismatch"});
  EXPECT_EQ(observation->command_surface_state(),
            ManagedCommandSurfaceState::kClosedNoActiveEffect);
  EXPECT_EQ(fixture.runtime_ports.start_effect_calls, 1);
  EXPECT_TRUE(
      fixture.journal
          .ReadRuntimeConvergenceIntent(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
  EXPECT_FALSE(
      fixture.journal
          .ReadLatestRuntimeStagedReady(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
  EXPECT_EQ(fixture.journal.VerifyIntegrity().runtime_staged_ready_count, 0);
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     PublicationDriftAfterStartPreventsStagedReadyCommit) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  fixture.publication.fail_on_call = 3;

  EXPECT_THROW(static_cast<void>(fixture.Run()),
               ManagerRuntimeConvergenceCoordinatorError);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 1);
  EXPECT_FALSE(
      fixture.journal
          .ReadLatestRuntimeStagedReady(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     IncompatibleSelectedLaunchContractRejectsBeforeFenceOrProcessEffect) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  // The sealed product is valid, but the selected host launch contract is
  // not offered by that product. This must fail before any owner effect.
  ManagerApplyRuntimeConvergenceCoordinator incompatible(
      fixture.journal, fixture.publication,
      *fixture.package.artifact_source, fixture.runtime_ports,
      fixture.runtime_ports, "linux", "foreign.shell", [] { return 1'000U; });

  EXPECT_THROW(static_cast<void>(incompatible.ConvergeOrRecover(
                   fixture.apply_intent, *fixture.package.bundle,
                   *fixture.package.bundle,
                   ActiveBundlePublicationGeneration(3U))),
               ManagerRuntimeConvergenceCoordinatorError);
  EXPECT_EQ(fixture.kernel.acquire_calls, 0);
  EXPECT_EQ(fixture.kernel.reconcile_calls, 0);
  EXPECT_EQ(fixture.kernel.rehydrate_calls, 0);
  EXPECT_EQ(fixture.runtime_ports.observe_calls, 0);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 0);
  EXPECT_EQ(fixture.runtime_ports.progress_calls, 0);
  EXPECT_FALSE(fixture.runtime_ports.last_query.has_value());
  EXPECT_FALSE(
      fixture.journal.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
  EXPECT_FALSE(
      fixture.journal.ReadAffectedScope(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
  EXPECT_FALSE(
      fixture.journal.ReadFenceAttempt(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
  EXPECT_FALSE(fixture.journal
                   .ReadFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U))
                   .has_value());
  EXPECT_FALSE(
      fixture.journal
          .ReadRuntimeConvergenceIntent(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
  EXPECT_FALSE(
      fixture.journal
          .ReadLatestRuntimeStagedReady(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     NativeExecutionHostRemovalRejectsBeforeRuntimeEffect) {
  ScopedPackageRoot native_root;
  ScopedPackageRoot candidate_root;
  ScopedPackageRoot state_root;
  auto predecessor =
      MaterializeNativeRuntimePackage(native_root.path());
  auto candidate = MaterializeRuntimePackage(candidate_root.path());
  ASSERT_EQ(predecessor.bundle->value().cell_graph.cell_id,
            candidate.bundle->value().cell_graph.cell_id);
  ASSERT_EQ(predecessor.bundle->value().cell_graph.runtime_units.size(), 1U);
  ASSERT_EQ(predecessor.bundle->value().cell_graph.runtime_units.front()
                .runtime_profile_id.value(),
            deployment::native_execution_runtime_contract::
                kRuntimeProfileId);

  auto intent = ApplyIntent(*predecessor.bundle);
  intent.candidate_retained_bundle_id = candidate.bundle->retained_bundle_id();
  intent.candidate_graph_revision =
      candidate.bundle->value().cell_graph.graph_revision;
  const auto continuity = VerifyNativeExecutionHostContinuity(
      *predecessor.bundle, *candidate.bundle, *predecessor.artifact_source);
  ASSERT_FALSE(continuity.ok());
  EXPECT_EQ(continuity.error().code,
            "native_execution_host_incompatible");
  EXPECT_EQ(continuity.error().details.at("expected_runtime_unit_id"),
            predecessor.bundle->value().cell_graph.runtime_units.front()
                .runtime_unit_id.value());
  EXPECT_EQ(continuity.error().details.at("candidate_runtime_unit_id"),
            "<missing>");
  EXPECT_EQ(continuity.error().details.at("reason"),
            "candidate_native_host_missing");
  const auto database = state_root.path() / "native-host-removal.sqlite3";
  SqliteManagerApplyJournal journal(
      database, predecessor.bundle->value().cell_graph.cell_id);
  SqliteManagerApplyJournal independent_reader(
      database, predecessor.bundle->value().cell_graph.cell_id);
  CurrentPublicationReader publication(PredecessorPublication(intent));
  ScriptedRuntimePorts runtime_ports(independent_reader);
  ManagerApplyRuntimeConvergenceCoordinator coordinator(
      journal, publication, *predecessor.artifact_source, runtime_ports,
      runtime_ports, "linux",
      {std::string(deployment::native_execution_runtime_contract::
                       kLaunchEnvironmentContract)},
      [] { return 1'000U; });

  EXPECT_THROW(static_cast<void>(coordinator.ConvergeOrRecover(
                   intent, *predecessor.bundle, *candidate.bundle,
                   ActiveBundlePublicationGeneration(3U))),
               ManagerRuntimeConvergenceCoordinatorError);
  EXPECT_EQ(publication.read_calls, 0);
  EXPECT_EQ(runtime_ports.observe_calls, 0);
  EXPECT_EQ(runtime_ports.ensure_calls, 0);
  EXPECT_FALSE(journal.ReadFenceAttempt(intent.apply_id,
                                        intent.apply_generation)
                   .has_value());
  EXPECT_FALSE(journal.ReadFenceAcceptance(intent.apply_id,
                                           intent.apply_generation)
                   .has_value());
  EXPECT_FALSE(journal.ReadRuntimeConvergenceIntent(intent.apply_id,
                                                   intent.apply_generation)
                   .has_value());
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     ConcreteRetainedProcessReachesDurableStagedReadyWithoutEpochReplacement) {
  ScopedPackageRoot root;
  auto package = MaterializeRuntimePackage(root.path());
  const auto runtime_state_root = root.path() / "execution-state";
  const auto runtime_run_root = root.path() / "execution-run";
  ASSERT_TRUE(std::filesystem::create_directory(runtime_state_root));
  ASSERT_TRUE(std::filesystem::create_directory(runtime_run_root));
  const auto apply_intent = ApplyIntent(*package.bundle);
  SqliteManagerApplyJournal journal(root.path() / "concrete-apply.sqlite3",
                                    package.bundle->value().cell_graph.cell_id);
  CurrentPublicationReader publication(PredecessorPublication(apply_intent));
  ExactFencePort kernel;
  ManagerApplyFenceCoordinator fence_coordinator(
      journal, kernel, RuntimeEpoch("manager-runtime-1"));
  RetainedPosixRuntimeUnitAdapter runtime_adapter(
      runtime_state_root, runtime_run_root, *package.bundle_source,
      *package.artifact_source, "linux", "posix.1-2017");
  std::uint64_t now_ns = 1'000U;
  ManagerApplyRuntimeConvergenceCoordinator coordinator(
      journal, publication, *package.artifact_source,
      runtime_adapter, runtime_adapter, "linux", "posix.1-2017",
      [&now_ns] { return now_ns++; });
  const auto run = [&] {
    return coordinator.ConvergeOrRecover(apply_intent, *package.bundle,
                                         *package.bundle,
                                         ActiveBundlePublicationGeneration(3U));
  };

  static_cast<void>(fence_coordinator.AcquireOrRecoverFence(
      apply_intent, *package.bundle, *package.bundle,
      ManagerApplyFenceExpectationDTO{
          0U, ConfigurationFenceGeneration(0U), {}}));
  const auto first = run();
  ASSERT_EQ(first.state, ManagerRuntimeConvergenceResultState::kStagedReady);
  ASSERT_TRUE(first.durable_staged_ready.has_value());
  ASSERT_EQ(
      first.durable_staged_ready->record.staged_ready.runtime_units.size(), 1U);
  const auto first_evidence =
      first.durable_staged_ready->record.staged_ready.runtime_units.front();
  EXPECT_EQ(first_evidence.source_report_sequence, 1U);

  const auto second = run();
  ASSERT_EQ(second.state, ManagerRuntimeConvergenceResultState::kStagedReady);
  ASSERT_TRUE(second.durable_staged_ready.has_value());
  ASSERT_EQ(
      second.durable_staged_ready->record.staged_ready.runtime_units.size(),
      1U);
  const auto &second_evidence =
      second.durable_staged_ready->record.staged_ready.runtime_units.front();
  EXPECT_EQ(second_evidence.runtime_epoch, first_evidence.runtime_epoch);
  EXPECT_EQ(second_evidence.source_report_sequence, 1U);
  EXPECT_EQ(
      second.durable_staged_ready->record.staged_ready.staged_ready_generation,
      1U);
  EXPECT_FALSE(second.durable_staged_ready->inserted);
  EXPECT_EQ(kernel.acquire_calls, 1);
  const auto &durable_runtime = second.durable_runtime_intent.record;
  const RuntimeUnitConvergenceQueryDTO cleanup_query{
      apply_intent.apply_id,
      apply_intent.apply_generation,
      package.bundle->retained_bundle_id(),
      package.bundle->value().cell_graph.graph_revision,
      durable_runtime.runtime_convergence_intent_digest,
      durable_runtime.runtime_convergence_intent.runtime_units.front(),
  };
  EXPECT_EQ(runtime_adapter.EnsureStopped(cleanup_query).state(),
            ProcessPresenceState::kAbsent);
}

TEST(ManagerNamedApplyCoordinator,
     CapturesOnceProgressesWithoutBundleReloadAndReplaysWithoutEffects) {
  ScopedPackageRoot package_root;
  ScopedPackageRoot state_root;
  auto package = MaterializeRuntimePackage(package_root.path());
  RefusingBundleReloadSource bundle_source(*package.bundle_source);
  const auto intent_template = ApplyIntent(*package.bundle);
  const auto predecessor = PredecessorPublication(intent_template);
  SqliteManagerApplyJournal journal(state_root.path() / "named-apply.sqlite3",
                                    package.bundle->value().cell_graph.cell_id);
  SqliteManagerApplyJournal independent_reader(
      state_root.path() / "named-apply.sqlite3",
      package.bundle->value().cell_graph.cell_id);
  ScriptedPublicationStore publication_store(predecessor, &independent_reader);
  FixedActiveRuntimePlanSource active_plan_source(
      predecessor, BuildVerifiedRuntimeConvergencePlan(
                       *package.bundle, *package.artifact_source, "linux",
                       {"posix.1-2017"}));
  ScriptedRuntimePorts runtime_ports(independent_reader);
  MaintenanceSceneOwner scene_owner(
      predecessor.request.candidate_projection_revision);
  AggregateFencePort kernel_fence(independent_reader, predecessor);
  ScriptedRuntimeCompositionPort runtime_composition;
  ScriptedCellAdmissionPort cell_admission;
  ManagerActiveRuntimeCoordinator active_runtime(
      active_plan_source, runtime_ports, runtime_ports, [] { return 1'000U; });
  AlwaysIdleMaintenanceGate maintenance_gate;
  ManagerPlannedMaintenanceCoordinator planned_maintenance(
      active_plan_source, maintenance_gate);
  bool invalid_initial_scene = true;
  ManagerNamedApplyCoordinator coordinator(
      journal, active_plan_source, bundle_source,
      *package.artifact_source, publication_store, scene_owner, nullptr,
      kernel_fence, cell_admission, runtime_composition, runtime_ports,
      runtime_ports, planned_maintenance,
      RuntimeEpoch("manager-runtime.aggregate.1"), "linux",
      {"posix.1-2017"}, [] { return 1'000U; },
      [] { return ApplyId("apply.aggregate.1"); },
      [&](const auto &, const auto &) {
        if (invalid_initial_scene) {
          return std::vector<core::AddObjectRequestDTO>{
              {core::SceneResourceId("invalid"), core::SphereShapeDTO{-0.1},
               core::Pose3DTO{},
               core::CellSceneFrameRefDTO{
                   package.bundle->value().cell_graph.cell_id}, {}}};
        }
        return std::vector<core::AddObjectRequestDTO>{};
      });
  const ManagerNamedApplyRequestDTO request{
      intent_template.request_context,
      package.bundle->value().deployment_artifact_id,
      package.bundle->value().deployment_revision,
      predecessor.request.candidate_retained_bundle_id,
      predecessor.request.candidate_graph_revision,
      package.bundle->retained_bundle_id(),
      intent_template.prepared_activation,
      core::AdmissionGeneration(7U),
      std::nullopt,
  };

  EXPECT_THROW(coordinator.AcceptOrReplay(request),
               ManagerNamedApplyCoordinatorError);
  EXPECT_FALSE(journal.ReadRequest(request.request_context.request_owner_epoch,
                                   request.request_context.request_id)
                   .has_value());
  EXPECT_EQ(runtime_ports.ensure_calls, 0);
  EXPECT_EQ(scene_owner.begin_calls, 0);
  invalid_initial_scene = false;
  const auto accepted = coordinator.AcceptOrReplay(request);
  EXPECT_EQ(accepted.state, ManagerNamedApplyResultState::kPending);
  EXPECT_FALSE(accepted.request_replayed);
  EXPECT_EQ(runtime_ports.ensure_calls, 0);
  EXPECT_EQ(scene_owner.begin_calls, 0);

  // Continue must use the accepted bundles, not look them up again. Retained
  // executable products remain available for their separate verification and
  // process initialization.
  const auto accepted_bundle_reads = bundle_source.read_calls;
  bundle_source.refuse_reads = true;
  const auto completed = coordinator.RecoverIncompleteApply();
  ASSERT_TRUE(completed.has_value());
  const auto failure_detail =
      completed->terminal_outcome.has_value() &&
              completed->terminal_outcome->failure.has_value()
          ? completed->terminal_outcome->failure->failure_code + ": " +
                completed->terminal_outcome->failure->failure_message
          : std::string("no terminal failure");
  SCOPED_TRACE(failure_detail);
  EXPECT_EQ(completed->state, ManagerNamedApplyResultState::kSucceeded);
  EXPECT_TRUE(completed->request_replayed);
  ASSERT_TRUE(completed->terminal_outcome.has_value());
  EXPECT_EQ(runtime_ports.ensure_calls, 1);
  EXPECT_EQ(scene_owner.begin_calls, 1);
  EXPECT_EQ(scene_owner.install_scene_calls, 1);
  EXPECT_EQ(scene_owner.complete_calls, 1);
  // Finalize before publication, then once more after it under the held
  // fence so the runtime owner adopts the published candidate (UA7W.L L6).
  EXPECT_EQ(runtime_composition.calls, 3);
  EXPECT_EQ(runtime_composition.phases,
            (std::vector<ManagerSelectedRuntimeCompositionPhase>{
                ManagerSelectedRuntimeCompositionPhase::kPrepare,
                ManagerSelectedRuntimeCompositionPhase::kFinalize,
                ManagerSelectedRuntimeCompositionPhase::kFinalize}));
  ASSERT_TRUE(runtime_composition.last_request.has_value());
  EXPECT_EQ(runtime_composition.last_request->phase,
            ManagerSelectedRuntimeCompositionPhase::kFinalize);
  // The host executes the exact durable decision, never a re-derived one.
  const auto affected = journal.ReadAffectedScope(
      completed->terminal_outcome->apply_id,
      completed->terminal_outcome->apply_generation);
  ASSERT_TRUE(affected.has_value());
  EXPECT_EQ(runtime_composition.last_request->component_delta,
            affected->affected_scope.component_delta);
  EXPECT_TRUE(runtime_composition.last_request->component_delta.valid());
  EXPECT_EQ(runtime_composition.last_request->predecessor_projection_revision
                .retained_bundle_id,
            predecessor.request.candidate_retained_bundle_id);
  EXPECT_EQ(publication_store.publish_calls, 1);
  EXPECT_EQ(kernel_fence.acquire_calls, 1);
  EXPECT_EQ(kernel_fence.release_calls, 1);
  EXPECT_EQ(bundle_source.read_calls, accepted_bundle_reads);

  const auto replayed = coordinator.AcceptOrReplay(request);
  EXPECT_EQ(replayed.state, ManagerNamedApplyResultState::kSucceeded);
  EXPECT_TRUE(replayed.request_replayed);
  EXPECT_EQ(replayed.terminal_outcome, completed->terminal_outcome);
  EXPECT_EQ(runtime_ports.ensure_calls, 1);
  EXPECT_EQ(scene_owner.begin_calls, 1);
  EXPECT_EQ(scene_owner.install_scene_calls, 1);
  EXPECT_EQ(scene_owner.complete_calls, 1);
  EXPECT_EQ(runtime_composition.calls, 3);  // Replay repeats no effect.
  EXPECT_EQ(publication_store.publish_calls, 1);
  EXPECT_EQ(kernel_fence.acquire_calls, 1);
  EXPECT_EQ(kernel_fence.release_calls, 1);
  EXPECT_FALSE(coordinator.RecoverIncompleteApply().has_value());
}

TEST(ManagerNamedApplyCoordinator,
     EmptyDeltaInstallationDoesNotRequireComponentReceipts) {
  ScopedPackageRoot package_root;
  ScopedPackageRoot state_root;
  auto package = MaterializeRuntimePackage(package_root.path());
  const auto intent_template = ApplyIntent(*package.bundle);
  const auto predecessor = PredecessorPublication(intent_template);
  const auto database = state_root.path() / "receipt-apply.sqlite3";
  SqliteManagerApplyJournal journal(database,
                                    package.bundle->value().cell_graph.cell_id);
  SqliteManagerApplyJournal independent_reader(
      database, package.bundle->value().cell_graph.cell_id);
  ScriptedPublicationStore publication_store(predecessor, &independent_reader);
  FixedActiveRuntimePlanSource active_plan_source(
      predecessor, BuildVerifiedRuntimeConvergencePlan(
                       *package.bundle, *package.artifact_source, "linux",
                       {"posix.1-2017"}));
  ScriptedRuntimePorts runtime_ports(independent_reader);
  MaintenanceSceneOwner scene_owner(
      predecessor.request.candidate_projection_revision);
  AggregateFencePort kernel_fence(independent_reader, predecessor);
  ScriptedRuntimeCompositionPort runtime_composition;
  runtime_composition.omit_receipts = true;
  ScriptedCellAdmissionPort cell_admission;
  ManagerActiveRuntimeCoordinator active_runtime(
      active_plan_source, runtime_ports, runtime_ports, [] { return 1'000U; });
  AlwaysIdleMaintenanceGate maintenance_gate;
  ManagerPlannedMaintenanceCoordinator planned_maintenance(
      active_plan_source, maintenance_gate);
  ManagerNamedApplyCoordinator coordinator(
      journal, active_plan_source, *package.bundle_source,
      *package.artifact_source, publication_store, scene_owner, nullptr,
      kernel_fence, cell_admission, runtime_composition, runtime_ports,
      runtime_ports, planned_maintenance,
      RuntimeEpoch("manager-runtime.receipts.1"), "linux", {"posix.1-2017"},
      [] { return 1'000U; }, [] { return ApplyId("apply.receipts.1"); },
      [](const auto &, const auto &) {
        return std::vector<core::AddObjectRequestDTO>{};
      });
  const ManagerNamedApplyRequestDTO request{
      intent_template.request_context,
      package.bundle->value().deployment_artifact_id,
      package.bundle->value().deployment_revision,
      predecessor.request.candidate_retained_bundle_id,
      predecessor.request.candidate_graph_revision,
      package.bundle->retained_bundle_id(),
      intent_template.prepared_activation,
      core::AdmissionGeneration(7U),
      std::nullopt,
  };
  EXPECT_EQ(coordinator.AcceptOrReplay(request).state,
            ManagerNamedApplyResultState::kPending);
  const auto outcome = coordinator.RecoverIncompleteApply();
  ASSERT_TRUE(outcome.has_value());
  // This runtime-only candidate has no controllable components, so an empty
  // receipt list is the exact complete receipt set and installation may
  // publish. Non-empty deltas' missing-receipt refusal is tested directly by
  // ComponentReceiptsMatchRequest.
  EXPECT_EQ(outcome->state, ManagerNamedApplyResultState::kSucceeded);
  EXPECT_EQ(runtime_composition.phases,
            (std::vector<ManagerSelectedRuntimeCompositionPhase>{
                ManagerSelectedRuntimeCompositionPhase::kPrepare,
                ManagerSelectedRuntimeCompositionPhase::kFinalize,
                ManagerSelectedRuntimeCompositionPhase::kFinalize}));
  ASSERT_TRUE(runtime_composition.last_request.has_value());
  EXPECT_TRUE(runtime_composition.last_request->component_delta.components.empty());
  EXPECT_EQ(publication_store.publish_calls, 1);
}

TEST(ManagerNamedApplyCoordinator,
     StalePredecessorIsRefusedBeforeAnyDurableRecordOrEffect) {
  ScopedPackageRoot package_root;
  ScopedPackageRoot state_root;
  auto package = MaterializeRuntimePackage(package_root.path());
  const auto intent_template = ApplyIntent(*package.bundle);
  const auto predecessor = PredecessorPublication(intent_template);
  const auto database = state_root.path() / "stale-predecessor.sqlite3";
  SqliteManagerApplyJournal journal(database,
                                    package.bundle->value().cell_graph.cell_id);
  SqliteManagerApplyJournal independent_reader(
      database, package.bundle->value().cell_graph.cell_id);
  ScriptedPublicationStore publication_store(predecessor, &independent_reader);
  FixedActiveRuntimePlanSource active_plan_source(
      predecessor, BuildVerifiedRuntimeConvergencePlan(
                       *package.bundle, *package.artifact_source, "linux",
                       {"posix.1-2017"}));
  ScriptedRuntimePorts runtime_ports(independent_reader);
  MaintenanceSceneOwner scene_owner(
      predecessor.request.candidate_projection_revision);
  AggregateFencePort kernel_fence(independent_reader, predecessor);
  ScriptedRuntimeCompositionPort runtime_composition;
  ScriptedCellAdmissionPort cell_admission;
  ManagerActiveRuntimeCoordinator active_runtime(
      active_plan_source, runtime_ports, runtime_ports, [] { return 1'000U; });
  AlwaysIdleMaintenanceGate maintenance_gate;
  ManagerPlannedMaintenanceCoordinator planned_maintenance(
      active_plan_source, maintenance_gate);
  ManagerNamedApplyCoordinator coordinator(
      journal, active_plan_source, *package.bundle_source,
      *package.artifact_source, publication_store, scene_owner, nullptr,
      kernel_fence, cell_admission, runtime_composition, runtime_ports,
      runtime_ports, planned_maintenance,
      RuntimeEpoch("manager-runtime.stale.1"), "linux", {"posix.1-2017"},
      [] { return 1'000U; }, [] { return ApplyId("apply.stale.1"); },
      [](const auto &, const auto &) {
        return std::vector<core::AddObjectRequestDTO>{};
      });
  // The caller previewed against a predecessor that is no longer active.
  auto stale_bundle =
      predecessor.request.candidate_retained_bundle_id.value();
  stale_bundle.back() = stale_bundle.back() == '0' ? '1' : '0';
  const ManagerNamedApplyRequestDTO request{
      intent_template.request_context,
      package.bundle->value().deployment_artifact_id,
      package.bundle->value().deployment_revision,
      RetainedBundleId(stale_bundle),
      predecessor.request.candidate_graph_revision,
      package.bundle->retained_bundle_id(),
      intent_template.prepared_activation,
      core::AdmissionGeneration(7U),
      std::nullopt,
  };

  EXPECT_THROW(static_cast<void>(coordinator.AcceptOrReplay(request)),
               ManagerNamedApplyCoordinatorError);
  EXPECT_FALSE(journal.ReadRequest(request.request_context.request_owner_epoch,
                                   request.request_context.request_id)
                   .has_value());
  EXPECT_EQ(runtime_ports.ensure_calls, 0);
  EXPECT_EQ(scene_owner.begin_calls, 0);
  EXPECT_EQ(kernel_fence.acquire_calls, 0);
  EXPECT_EQ(runtime_composition.calls, 0);
  EXPECT_EQ(publication_store.publish_calls, 0);
}

TEST(ManagerNamedApplyCoordinator,
     SharedNativeDomainIsRefusedBeforeFenceOrPredecessorEffects) {
  ScopedPackageRoot package_root;
  ScopedPackageRoot state_root;
  auto previous = MaterializeRuntimePackage(package_root.path());
  auto candidate = MaterializeNativeRuntimePackage(package_root.path(), true);
  const auto predecessor = PredecessorPublication(ApplyIntent(*previous.bundle));
  const auto intent = ApplyIntent(*candidate.bundle);
  const auto database = state_root.path() / "unsupported-topology.sqlite3";
  SqliteManagerApplyJournal journal(database, intent.cell_id);
  SqliteManagerApplyJournal reader(database, intent.cell_id);
  ScriptedPublicationStore publication_store(predecessor, &reader);
  FixedActiveRuntimePlanSource active_plan_source(
      predecessor, BuildVerifiedRuntimeConvergencePlan(
          *previous.bundle, *previous.artifact_source, "linux", {"posix.1-2017"}));
  ScriptedRuntimePorts runtime_ports(reader);
  MaintenanceSceneOwner scene_owner(predecessor.request.candidate_projection_revision);
  AggregateFencePort kernel_fence(reader, predecessor);
  ScriptedRuntimeCompositionPort runtime_composition;
  ScriptedCellAdmissionPort cell_admission;
  AlwaysIdleMaintenanceGate maintenance_gate;
  ManagerPlannedMaintenanceCoordinator maintenance(active_plan_source, maintenance_gate);
  ManagerNamedApplyCoordinator coordinator(
      journal, active_plan_source, *candidate.bundle_source,
      *candidate.artifact_source, publication_store, scene_owner, nullptr,
      kernel_fence, cell_admission, runtime_composition, runtime_ports,
      runtime_ports, maintenance, RuntimeEpoch("manager.topology.1"),
      "linux", {"posix.1-2017"}, [] { return 1'000U; },
      [] { return ApplyId("apply.topology.1"); },
      [](const auto &, const auto &) { return std::vector<AddObjectRequestDTO>{}; });
  const ManagerNamedApplyRequestDTO request{
      intent.request_context, candidate.bundle->value().deployment_artifact_id,
      candidate.bundle->value().deployment_revision,
      predecessor.request.candidate_retained_bundle_id,
      predecessor.request.candidate_graph_revision, candidate.bundle->retained_bundle_id(),
      intent.prepared_activation, AdmissionGeneration(7U), std::nullopt};
  const auto accepted = coordinator.AcceptOrReplay(request);
  ASSERT_EQ(accepted.state, ManagerNamedApplyResultState::kPending);
  const auto failed = coordinator.RecoverIncompleteApply();
  ASSERT_TRUE(failed.has_value());
  ASSERT_EQ(failed->state, ManagerNamedApplyResultState::kFailed);
  ASSERT_TRUE(failed->terminal_outcome.has_value());
  ASSERT_TRUE(failed->terminal_outcome->failure.has_value());
  EXPECT_EQ(failed->terminal_outcome->failure->failure_code,
            "native_shared_domain_unsupported");
  EXPECT_EQ(failed->terminal_outcome->failure->effect_disposition,
            ManagerApplyEffectDisposition::kKnown);
  EXPECT_FALSE(journal.ReadFenceAttempt(accepted.apply_id, accepted.apply_generation).has_value());
  EXPECT_EQ(kernel_fence.acquire_calls, 0);
  EXPECT_EQ(kernel_fence.release_calls, 0);
  EXPECT_EQ(runtime_ports.stop_calls, 0);
  EXPECT_EQ(runtime_ports.ensure_calls, 0);
  EXPECT_EQ(runtime_composition.calls, 0);
  EXPECT_EQ(scene_owner.begin_calls, 0);
  EXPECT_EQ(scene_owner.CurrentProjectionRevision(), predecessor.request.candidate_projection_revision);
  EXPECT_EQ(publication_store.publish_calls, 0);
  EXPECT_TRUE(cell_admission.GetAuthoringAdmission().authoring_allowed);
  ASSERT_TRUE(cell_admission.last_release.has_value());
  const auto replay = coordinator.AcceptOrReplay(request);
  EXPECT_TRUE(replay.request_replayed);
  EXPECT_EQ(replay.state, ManagerNamedApplyResultState::kFailed);
  EXPECT_EQ(kernel_fence.acquire_calls, 0);
  EXPECT_EQ(runtime_ports.stop_calls, 0);
}

TEST(ManagerNamedApplyCoordinator,
     ReopenedManagerMarksNonterminalCaptureInterruptedWithoutEffects) {
  ScopedPackageRoot package_root;
  ScopedPackageRoot state_root;
  auto package = MaterializeRuntimePackage(package_root.path());
  const auto intent_template = ApplyIntent(*package.bundle);
  const auto predecessor = PredecessorPublication(intent_template);
  const auto database = state_root.path() / "reopened-apply.sqlite3";
  SqliteManagerApplyJournal journal(database,
                                    package.bundle->value().cell_graph.cell_id);
  SqliteManagerApplyJournal independent_reader(
      database, package.bundle->value().cell_graph.cell_id);
  ScriptedPublicationStore publication_store(predecessor, &independent_reader);
  FixedActiveRuntimePlanSource active_plan_source(
      predecessor, BuildVerifiedRuntimeConvergencePlan(
                       *package.bundle, *package.artifact_source, "linux",
                       {"posix.1-2017"}));
  ScriptedRuntimePorts runtime_ports(independent_reader);
  MaintenanceSceneOwner scene_owner(
      predecessor.request.candidate_projection_revision);
  AggregateFencePort kernel_fence(independent_reader, predecessor);
  ScriptedRuntimeCompositionPort runtime_composition;
  ScriptedCellAdmissionPort cell_admission;
  ManagerActiveRuntimeCoordinator active_runtime(
      active_plan_source, runtime_ports, runtime_ports, [] { return 1'000U; });
  AlwaysIdleMaintenanceGate maintenance_gate;
  ManagerPlannedMaintenanceCoordinator planned_maintenance(
      active_plan_source, maintenance_gate);
  const ManagerNamedApplyRequestDTO request{
      intent_template.request_context,
      package.bundle->value().deployment_artifact_id,
      package.bundle->value().deployment_revision,
      predecessor.request.candidate_retained_bundle_id,
      predecessor.request.candidate_graph_revision,
      package.bundle->retained_bundle_id(),
      intent_template.prepared_activation,
      core::AdmissionGeneration(7U),
      std::nullopt,
  };
  {
    ManagerNamedApplyCoordinator first(
        journal, active_plan_source, *package.bundle_source,
        *package.artifact_source, publication_store, scene_owner, nullptr,
        kernel_fence, cell_admission, runtime_composition, runtime_ports,
        runtime_ports, planned_maintenance,
        RuntimeEpoch("manager-runtime.aggregate.1"), "linux",
        {"posix.1-2017"}, [] { return 1'000U; },
        [] { return ApplyId("apply.reopened.1"); },
        [](const auto &, const auto &) {
          return std::vector<core::AddObjectRequestDTO>{};
        });
    EXPECT_EQ(first.AcceptOrReplay(request).state,
              ManagerNamedApplyResultState::kPending);
  }

  ManagerNamedApplyCoordinator reopened(
      journal, active_plan_source, *package.bundle_source,
      *package.artifact_source, publication_store, scene_owner, nullptr,
      kernel_fence, cell_admission, runtime_composition, runtime_ports,
      runtime_ports, planned_maintenance,
      RuntimeEpoch("manager-runtime.aggregate.2"), "linux",
      {"posix.1-2017"}, [] { return 2'000U; },
      [] { return ApplyId("must-not-be-minted"); },
      [](const auto &, const auto &) {
        return std::vector<core::AddObjectRequestDTO>{};
      });
  const auto interrupted = reopened.RecoverIncompleteApply();
  ASSERT_TRUE(interrupted.has_value());
  EXPECT_EQ(interrupted->state, ManagerNamedApplyResultState::kUnknownEffect);
  EXPECT_EQ(runtime_ports.ensure_calls, 0);
  EXPECT_EQ(scene_owner.begin_calls, 0);
  EXPECT_EQ(runtime_composition.calls, 0);
  EXPECT_EQ(publication_store.publish_calls, 0);
  EXPECT_EQ(kernel_fence.acquire_calls, 0);
  EXPECT_EQ(kernel_fence.release_calls, 0);
}


} // namespace
} // namespace manyforge::component_manager
