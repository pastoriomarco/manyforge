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
#include "manyforge_component_manager/manager_scene_activation_coordinator.hpp"
#include "manyforge_component_manager/manager_scene_stage_coordinator.hpp"
#include "manyforge_component_manager/manager_selected_bundle_transition_coordinator.hpp"
#include "manyforge_component_manager/manager_selected_runtime_composition_coordinator.hpp"
#include "manyforge_component_manager/reference_runtime_unit_admission.hpp"
#include "manyforge_component_manager/retained_posix_runtime_unit_adapter.hpp"
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

[[nodiscard]] RuntimePackage
MaterializeRuntimePackage(const std::filesystem::path &root) {
  const auto fixture = deployment::test::make_runtime_only_retention_fixture();
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
          ContentDigest(std::string(kDigestTwo)),
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
      const auto retired =
          independent_reader_->ReadObsoleteCommandSurfacesRetired(
              named->apply_id, named->apply_generation);
      saw_prerequisites_before_publish =
          materialization.has_value() && retired.has_value() &&
          materialization->private_materialization_content_digest ==
              request.private_materialization_content_digest &&
          retired->obsolete_managed_command_surfaces_retired_evidence_digest ==
              request.obsolete_managed_command_surfaces_retired_evidence_digest;
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
    const auto finalized = independent_reader_.ReadSceneFinalized(
        query.owner_transaction.apply_id,
        query.owner_transaction.apply_generation);
    const auto publication = independent_reader_.ReadPublicationReceipt(
        query.owner_transaction.apply_id,
        query.owner_transaction.apply_generation);
    if (!finalized.has_value() || !publication.has_value()) {
      throw std::runtime_error("release fixture lacks finalized publication");
    }
    const auto &candidate = finalized->scene_finalized.finalized_result
                                .candidate_projection_revision;
    const auto &receipt = publication->publication.publication_receipt;
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
    const auto finalized = independent_reader_.ReadSceneFinalized(
        query.owner_transaction.apply_id,
        query.owner_transaction.apply_generation);
    const auto publication = independent_reader_.ReadPublicationReceipt(
        query.owner_transaction.apply_id,
        query.owner_transaction.apply_generation);
    if (!finalized.has_value() || !publication.has_value()) {
      throw std::runtime_error(
          "aggregate fence status lacks finalized publication");
    }
    const auto &candidate = finalized->scene_finalized.finalized_result
                                .candidate_projection_revision;
    const auto &receipt = publication->publication.publication_receipt;
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
    state_ = ProcessPresenceState::kAbsent;
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
    if (replayed_report.has_value()) {
      last_report = replayed_report;
      return replayed_report;
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
        std::move(roles),
        ManagedCommandSurfaceState::kClosedNoActiveEffect, {},
        progress_offer_digest.value_or(
            query.plan.runtime_contract_offer_digest.value()));
    return last_report;
  }

  void ReplaceProcess(std::string process_incarnation) {
    process_incarnation_ = std::move(process_incarnation);
    state_ = ProcessPresenceState::kPresent;
    ++observation_sequence_;
  }

  bool throw_after_ensure_effect{false};
  bool report_available{true};
  bool saw_durable_intent_before_ensure{false};
  std::optional<std::string> progress_report_incarnation;
  std::optional<std::string> progress_configuration_identity;
  std::optional<std::string> progress_offer_digest;
  std::optional<RuntimeUnitSourceReport> replayed_report;
  std::optional<RuntimeUnitSourceReport> last_report;
  int observe_calls{0};
  int ensure_calls{0};
  int start_effect_calls{0};
  int progress_calls{0};
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
    last_request = request;
    auto response_request = request;
    if (foreign_response) {
      response_request.manager_runtime_epoch = RuntimeEpoch("foreign-manager");
    }
    const auto preparing = prepare_once && calls == 1;
    return {std::move(response_request),
            preparing ? ManagerSelectedRuntimeCompositionState::kPreparing
                      : ManagerSelectedRuntimeCompositionState::kInstalled};
  }

  bool prepare_once{false};
  bool foreign_response{false};
  int calls{0};
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
      receipt.status =
          core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact;
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

class ScriptedSceneOwner final : public IManagerSceneProjectionHandoffPort {
public:
  explicit ScriptedSceneOwner(SqliteManagerApplyJournal &independent_reader)
      : independent_reader_(independent_reader) {}

  [[nodiscard]] ProjectionApplyStageResultDTO
  StageCandidateProjection(const ProjectionApplyStageDTO &request) override {
    ++stage_calls;
    last_stage_request = request;
    const auto durable = independent_reader_.ReadSceneStageIntent(
        request.apply_id, request.apply_generation);
    saw_durable_intent_before_stage =
        durable.has_value() &&
        durable->scene_stage_intent.native_transition_rows ==
            request.native_transition_rows &&
        durable->scene_stage_intent.expected_active_projection_revision ==
            request.expected_active_projection_revision;
    if (!request.valid() || !request.native_transition_rows.empty()) {
      throw std::runtime_error("scene fixture received an invalid request");
    }
    const ProjectionRevisionDTO candidate{
        request.candidate_retained_bundle.retained_bundle_id,
        request.candidate_retained_bundle.cell_graph.graph_revision,
        SceneRevision{
            request.expected_active_projection_revision.scene_revision.scene_id,
            request.expected_active_projection_revision.scene_revision
                    .revision_id +
                1U,
            ContentDigest(std::string(kDigestTwo)),
        },
    };
    const ProjectionStageGeneration stage_generation(9U);
    ProjectionTransitionEvidenceDTO evidence{
        request.apply_id,
        request.apply_generation,
        stage_generation,
        request.expected_active_projection_revision,
        candidate,
        ProjectionTransitionEvidenceState::kStaged,
        {},
        std::nullopt,
    };
    staged_result_ = ProjectionApplyStageResultDTO{
        request.apply_id,
        request.apply_generation,
        request.expected_active_projection_revision,
        candidate,
        stage_generation,
        std::move(evidence),
        ProjectionApplyStageResultState::kStaged,
        std::nullopt,
    };
    if (throw_after_stage_effect) {
      throw std::runtime_error("simulated lost scene Stage response");
    }
    return *staged_result_;
  }

  [[nodiscard]] ProjectionApplyActivationResultDTO ActivateStagedProjection(
      const ProjectionApplyActivationDTO &request) override {
    ++activation_calls;
    last_activation_request = request;
    const auto durable = independent_reader_.ReadActivationSelection(
        request.apply_id, request.apply_generation);
    saw_durable_selection_before_activation =
        durable.has_value() &&
        durable->activation_selection.apply_id == request.apply_id &&
        durable->activation_selection.apply_generation ==
            request.apply_generation &&
        durable->activation_selection.projection_stage_generation ==
            request.projection_stage_generation &&
        durable->activation_selection.candidate_projection_revision ==
            request.candidate_projection_revision;
    if (!staged_result_.has_value() || !request.valid() ||
        staged_result_->apply_id != request.apply_id ||
        staged_result_->apply_generation != request.apply_generation ||
        staged_result_->expected_active_projection_revision !=
            request.expected_active_projection_revision ||
        staged_result_->projection_stage_generation !=
            std::optional<ProjectionStageGeneration>{
                request.projection_stage_generation} ||
        staged_result_->candidate_projection_revision !=
            std::optional<ProjectionRevisionDTO>{
                request.candidate_projection_revision} ||
        !staged_result_->transition_evidence->transition_rows.empty()) {
      throw std::runtime_error(
          "activation fixture received a foreign or nonempty stage");
    }
    auto evidence = *staged_result_->transition_evidence;
    evidence.state = ProjectionTransitionEvidenceState::kActivatedAndRetired;
    activation_result_ = ProjectionApplyActivationResultDTO{
        request.apply_id,
        request.apply_generation,
        request.expected_active_projection_revision,
        request.projection_stage_generation,
        request.candidate_projection_revision,
        ProjectionApplyActivationResultState::kActivated,
        request.candidate_projection_revision,
        std::move(evidence),
        std::nullopt,
    };
    if (throw_after_activation_effect) {
      throw std::runtime_error("simulated lost activation response");
    }
    return *activation_result_;
  }

  [[nodiscard]] ProjectionApplyDiscardResultDTO
  DiscardStagedProjection(const ProjectionApplyDiscardDTO &) override {
    throw std::runtime_error("discard is outside the scene-stage fixture");
  }

  [[nodiscard]] ProjectionApplyFinalizeResultDTO FinalizeProjectionTransition(
      const ProjectionApplyFinalizeDTO &request) override {
    ++finalize_calls;
    last_finalize_request = request;
    const auto durable = independent_reader_.ReadPublicationReceipt(
        request.apply_id, request.apply_generation);
    saw_durable_publication_before_finalize =
        durable.has_value() &&
        durable->publication.publication_receipt == request.publication_receipt;
    if (!activation_result_.has_value() || !request.valid() ||
        request.apply_id != activation_result_->apply_id ||
        request.apply_generation != activation_result_->apply_generation ||
        request.projection_stage_generation !=
            activation_result_->projection_stage_generation ||
        request.candidate_projection_revision !=
            activation_result_->candidate_projection_revision) {
      throw std::runtime_error("finalize fixture received a foreign request");
    }
    auto evidence = activation_result_->transition_evidence;
    evidence.state = ProjectionTransitionEvidenceState::kFinalized;
    ProjectionAggregateHealthDTO health{
        NamedApplyProjectionSetSourceDTO{
            request.apply_id,
            request.apply_generation,
            request.projection_stage_generation,
            request.candidate_projection_revision,
        },
        ProjectionHealthState::kHealthy,
        {},
        std::nullopt,
    };
    finalization_result_ = ProjectionApplyFinalizeResultDTO{
        request.apply_id,
        request.apply_generation,
        request.projection_stage_generation,
        request.candidate_projection_revision,
        request.publication_receipt,
        ProjectionApplyFinalizeResultState::kFinalized,
        request.candidate_projection_revision,
        std::move(evidence),
        std::move(health),
        std::nullopt,
    };
    if (throw_after_finalize_effect) {
      throw std::runtime_error("simulated lost Finalize response");
    }
    return *finalization_result_;
  }

  [[nodiscard]] ProjectionApplyStatusDTO ProjectionApplyStatus(
      const ProjectionApplyStatusQueryDTO &query) const override {
    ++status_calls;
    if (throw_status) {
      throw std::runtime_error("simulated scene status transport failure");
    }
    if (foreign_status) {
      return {
          ApplyId("foreign-apply"),
          query.apply_generation,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          ProjectionApplyStatusState::kAbsent,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
      };
    }
    if (finalization_result_.has_value()) {
      const auto &result = *finalization_result_;
      return {
          result.apply_id,
          result.apply_generation,
          activation_result_->expected_active_projection_revision,
          result.projection_stage_generation,
          result.candidate_projection_revision,
          ProjectionApplyStatusState::kFinalized,
          result.transition_evidence,
          result.publication_receipt,
          result.candidate_aggregate_health,
          std::nullopt,
      };
    }
    if (activation_result_.has_value()) {
      const auto &result = *activation_result_;
      return {
          result.apply_id,
          result.apply_generation,
          result.expected_active_projection_revision,
          result.projection_stage_generation,
          result.candidate_projection_revision,
          ProjectionApplyStatusState::kActivated,
          result.transition_evidence,
          std::nullopt,
          std::nullopt,
          std::nullopt,
      };
    }
    if (staged_result_.has_value()) {
      const auto &result = *staged_result_;
      return {
          result.apply_id,
          result.apply_generation,
          result.expected_active_projection_revision,
          result.projection_stage_generation,
          result.candidate_projection_revision,
          ProjectionApplyStatusState::kStaged,
          result.transition_evidence,
          std::nullopt,
          std::nullopt,
          std::nullopt,
      };
    }
    if (quiescing) {
      const auto durable = independent_reader_.ReadSceneStageIntent(
          query.apply_id, query.apply_generation);
      if (!durable.has_value()) {
        throw std::runtime_error(
            "quiescing fixture requires an expected predecessor");
      }
      return {
          query.apply_id,
          query.apply_generation,
          durable->scene_stage_intent.expected_active_projection_revision,
          ProjectionStageGeneration(9U),
          std::nullopt,
          ProjectionApplyStatusState::kQuiescing,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
      };
    }
    return {
        query.apply_id, query.apply_generation,
        std::nullopt,   std::nullopt,
        std::nullopt,   ProjectionApplyStatusState::kAbsent,
        std::nullopt,   std::nullopt,
        std::nullopt,   std::nullopt,
    };
  }

  [[nodiscard]] ActiveProjectionRuntimeRecoveryResultDTO
  ReconcileActiveProjectionRuntimes(
      const ActiveProjectionRuntimeSetDTO &runtime_set) override {
    return {runtime_set, ActiveProjectionRuntimeRecoveryState::kCurrent,
            std::nullopt};
  }

  [[nodiscard]] ProjectionAggregateHealthDTO
  ActiveProjectionHealth() const override {
    if (!finalization_result_.has_value()) {
      throw std::runtime_error(
          "active projection health is absent before finalization");
    }
    return finalization_result_->candidate_aggregate_health;
  }

  bool throw_after_stage_effect{false};
  bool throw_after_activation_effect{false};
  bool throw_after_finalize_effect{false};
  bool throw_status{false};
  bool foreign_status{false};
  bool quiescing{false};
  bool saw_durable_intent_before_stage{false};
  bool saw_durable_selection_before_activation{false};
  bool saw_durable_publication_before_finalize{false};
  int stage_calls{0};
  int activation_calls{0};
  int finalize_calls{0};
  mutable int status_calls{0};
  std::optional<ProjectionApplyStageDTO> last_stage_request;
  std::optional<ProjectionApplyActivationDTO> last_activation_request;
  std::optional<ProjectionApplyFinalizeDTO> last_finalize_request;

private:
  SqliteManagerApplyJournal &independent_reader_;
  std::optional<ProjectionApplyStageResultDTO> staged_result_;
  std::optional<ProjectionApplyActivationResultDTO> activation_result_;
  std::optional<ProjectionApplyFinalizeResultDTO> finalization_result_;
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
        coordinator(fence_coordinator, journal, publication,
                    *package.artifact_source, runtime_ports, runtime_ports,
                    "linux", "posix.1-2017", [] { return 1'000U; }) {}

  [[nodiscard]] ManagerApplyRuntimeConvergenceResult Run() {
    return coordinator.ConvergeOrRecover(apply_intent, *package.bundle,
                                         *package.bundle,
                                         ManagerApplyFenceExpectationDTO{
                                             0U,
                                             ConfigurationFenceGeneration(0U),
                                             {},
                                         },
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

void ReachDurableSceneActivation(CoordinatorFixture &fixture,
                                 ScriptedSceneOwner &scene_owner) {
  ASSERT_EQ(fixture.Run().state,
            ManagerRuntimeConvergenceResultState::kStagedReady);
  ManagerSceneStageCoordinator stage_coordinator(
      fixture.journal, *fixture.package.bundle_source, fixture.publication,
      scene_owner);
  ASSERT_EQ(
      stage_coordinator.StageOrRecover(ApplyId("apply-1"), ApplyGeneration(1U))
          .path,
      ManagerSceneStagePath::kStagedFromNewCall);
  ManagerSceneActivationCoordinator activation_coordinator(fixture.journal,
                                                           scene_owner);
  ASSERT_EQ(activation_coordinator
                .ActivateOrRecover(ApplyId("apply-1"), ApplyGeneration(1U))
                .path,
            ManagerSceneActivationPath::kActivatedFromNewCall);
}

void ReachDurableRuntimeComposition(CoordinatorFixture &fixture,
                                    ScriptedRuntimeCompositionPort &port) {
  ManagerSelectedRuntimeCompositionCoordinator coordinator(
      fixture.journal, *fixture.package.bundle_source, port,
      RuntimeEpoch("manager-runtime-1"));
  const auto installed =
      coordinator.InstallOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_EQ(installed.state,
            ManagerSelectedRuntimeCompositionResultState::kInstalled);
  ASSERT_TRUE(installed.durable_materialization.has_value());
}

void ReachDurableSceneFinalize(CoordinatorFixture &fixture,
                               ScriptedSceneOwner &scene_owner,
                               ScriptedPublicationStore &publication_store) {
  ReachDurableSceneActivation(fixture, scene_owner);
  ScriptedRuntimeCompositionPort runtime_composition;
  ReachDurableRuntimeComposition(fixture, runtime_composition);
  ManagerSelectedBundleTransitionCoordinator coordinator(
      fixture.journal, *fixture.package.bundle_source, publication_store,
      scene_owner);
  ASSERT_EQ(
      coordinator.AdvanceOrRecover(ApplyId("apply-1"), ApplyGeneration(1U))
          .path,
      ManagerSelectedBundleTransitionPath::kPublishedFromNewCall);
  ASSERT_EQ(
      coordinator.AdvanceOrRecover(ApplyId("apply-1"), ApplyGeneration(1U))
          .path,
      ManagerSelectedBundleTransitionPath::kFinalizedFromNewCall);
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
  EXPECT_EQ(fixture.kernel.rehydrate_calls, 2);
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
  EXPECT_TRUE(fixture.journal
                  .ReadRuntimeConvergenceIntent(ApplyId("apply-1"),
                                                 ApplyGeneration(1U))
                  .has_value());
  EXPECT_FALSE(
      fixture.journal.ReadLatestRuntimeStagedReady(ApplyId("apply-1"),
                                                   ApplyGeneration(1U))
          .has_value());
  const auto report = fixture.journal.VerifyIntegrity();
  EXPECT_EQ(report.fence_acceptance_count, 1);
  EXPECT_EQ(report.runtime_convergence_intent_count, 1);
  EXPECT_EQ(report.runtime_staged_ready_count, 0);
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     ReplayedPriorEpochSequenceAndOfferRemainUnavailableAfterReplacement) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  const auto first = fixture.Run();
  ASSERT_EQ(first.state, ManagerRuntimeConvergenceResultState::kStagedReady);
  ASSERT_TRUE(first.durable_staged_ready.has_value());
  ASSERT_TRUE(fixture.runtime_ports.last_report.has_value());
  const auto prior_report = *fixture.runtime_ports.last_report;
  fixture.runtime_ports.replayed_report = prior_report;
  fixture.runtime_ports.ReplaceProcess("process-epoch-2");

  EXPECT_THROW(static_cast<void>(fixture.Run()),
               ManagerRuntimeConvergenceCoordinatorError);
  EXPECT_EQ(prior_report.process_incarnation(), "process-epoch-1");
  EXPECT_EQ(prior_report.source_report_sequence(), 1U);
  EXPECT_EQ(prior_report.offered_runtime_contract_offer_digest(),
            first.durable_staged_ready->record.staged_ready.runtime_units
                .front()
                .observed_runtime_contract_offer_digest.value());
  EXPECT_EQ(fixture.kernel.acquire_calls, 1);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 2);
  EXPECT_EQ(fixture.runtime_ports.start_effect_calls, 1);
  EXPECT_EQ(fixture.runtime_ports.progress_calls, 2);
  const auto latest = fixture.journal.ReadLatestRuntimeStagedReady(
      ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(*latest, first.durable_staged_ready->record);
  ASSERT_EQ(latest->staged_ready.runtime_units.size(), 1U);
  EXPECT_EQ(
      latest->staged_ready.runtime_units.front().command_surface_state,
      ManagedCommandSurfaceState::kClosedNoActiveEffect);
  const auto report = fixture.journal.VerifyIntegrity();
  EXPECT_EQ(report.runtime_convergence_intent_count, 1);
  EXPECT_EQ(report.runtime_staged_ready_count, 1);
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
            fixture.runtime_ports.last_query->plan
                .runtime_contract_offer_digest.value());
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
      fixture.journal.ReadLatestRuntimeStagedReady(ApplyId("apply-1"),
                                                   ApplyGeneration(1U))
          .has_value());
  EXPECT_EQ(fixture.journal.VerifyIntegrity().runtime_staged_ready_count, 0);
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     ConfigurationIdentityMismatchRemainsUnavailableWithoutStagedReady) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  fixture.runtime_ports.progress_configuration_identity =
      std::string("sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

  const auto result = fixture.Run();
  EXPECT_EQ(result.state, ManagerRuntimeConvergenceResultState::kPending);
  ASSERT_TRUE(fixture.runtime_ports.last_query.has_value());
  const auto runtime_unit_id =
      fixture.runtime_ports.last_query->plan.runtime_unit_id.value();
  ASSERT_NE(result.observations.observation(runtime_unit_id), nullptr);
  const auto *observation = result.observations.observation(runtime_unit_id);
  EXPECT_FALSE(observation->converged());
  EXPECT_EQ(observation->reason_codes(),
            std::vector<std::string>{
                "runtime_configuration_identity_mismatch"});
  EXPECT_EQ(observation->command_surface_state(),
            ManagedCommandSurfaceState::kClosedNoActiveEffect);
  EXPECT_EQ(fixture.runtime_ports.start_effect_calls, 1);
  EXPECT_TRUE(fixture.journal
                  .ReadRuntimeConvergenceIntent(ApplyId("apply-1"),
                                                 ApplyGeneration(1U))
                  .has_value());
  EXPECT_FALSE(
      fixture.journal.ReadLatestRuntimeStagedReady(ApplyId("apply-1"),
                                                   ApplyGeneration(1U))
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
      fixture.fence_coordinator, fixture.journal, fixture.publication,
      *fixture.package.artifact_source, fixture.runtime_ports,
      fixture.runtime_ports, "linux", "foreign.shell", [] { return 1'000U; });

  EXPECT_THROW(static_cast<void>(incompatible.ConvergeOrRecover(
                   fixture.apply_intent, *fixture.package.bundle,
                   *fixture.package.bundle,
                   ManagerApplyFenceExpectationDTO{
                       0U,
                       ConfigurationFenceGeneration(0U),
                       {},
                   },
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
  EXPECT_FALSE(fixture.journal
                   .ReadAffectedScope(ApplyId("apply-1"), ApplyGeneration(1U))
                   .has_value());
  EXPECT_FALSE(fixture.journal
                   .ReadFenceAttempt(ApplyId("apply-1"), ApplyGeneration(1U))
                   .has_value());
  EXPECT_FALSE(fixture.journal
                   .ReadFenceAcceptance(ApplyId("apply-1"),
                                         ApplyGeneration(1U))
                   .has_value());
  EXPECT_FALSE(fixture.journal
                   .ReadRuntimeConvergenceIntent(ApplyId("apply-1"),
                                                 ApplyGeneration(1U))
                   .has_value());
  EXPECT_FALSE(fixture.journal
                   .ReadLatestRuntimeStagedReady(ApplyId("apply-1"),
                                                  ApplyGeneration(1U))
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
      fence_coordinator, journal, publication, *package.artifact_source,
      runtime_adapter, runtime_adapter, "linux", "posix.1-2017",
      [&now_ns] { return now_ns++; });
  const auto run = [&] {
    return coordinator.ConvergeOrRecover(apply_intent, *package.bundle,
                                         *package.bundle,
                                         ManagerApplyFenceExpectationDTO{
                                             0U,
                                             ConfigurationFenceGeneration(0U),
                                             {},
                                         },
                                         ActiveBundlePublicationGeneration(3U));
  };

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
  EXPECT_EQ(second_evidence.source_report_sequence, 2U);
  EXPECT_EQ(
      second.durable_staged_ready->record.staged_ready.staged_ready_generation,
      2U);
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

TEST(ManagerSceneStageCoordinator,
     PersistsBeforeStageAndReplaysWithoutAnotherOwnerCall) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ASSERT_EQ(fixture.Run().state,
            ManagerRuntimeConvergenceResultState::kStagedReady);
  ScriptedSceneOwner scene_owner(fixture.independent_reader);
  ManagerSceneStageCoordinator coordinator(fixture.journal,
                                           *fixture.package.bundle_source,
                                           fixture.publication, scene_owner);

  const auto staged =
      coordinator.StageOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(staged.path, ManagerSceneStagePath::kStagedFromNewCall);
  EXPECT_TRUE(staged.durable_scene_stage_intent.inserted);
  EXPECT_TRUE(staged.durable_scene_staged.inserted);
  EXPECT_TRUE(scene_owner.saw_durable_intent_before_stage);
  EXPECT_EQ(scene_owner.status_calls, 1);
  EXPECT_EQ(scene_owner.stage_calls, 1);
  EXPECT_TRUE(staged.durable_scene_stage_intent.record.scene_stage_intent
                  .native_transition_rows.empty());

  const auto replay =
      coordinator.StageOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(replay.path, ManagerSceneStagePath::kDurableStagedReplay);
  EXPECT_FALSE(replay.durable_scene_stage_intent.inserted);
  EXPECT_FALSE(replay.durable_scene_staged.inserted);
  EXPECT_EQ(replay.durable_scene_staged.record.scene_staged_digest,
            staged.durable_scene_staged.record.scene_staged_digest);
  EXPECT_EQ(scene_owner.status_calls, 1);
  EXPECT_EQ(scene_owner.stage_calls, 1);
  const auto report = fixture.journal.VerifyIntegrity();
  EXPECT_EQ(report.scene_stage_intent_count, 1);
  EXPECT_EQ(report.scene_staged_count, 1);
}

TEST(ManagerSceneStageCoordinator,
     LostStageResponseRecoversFromOwnerStatusWithoutDuplicateStage) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ASSERT_EQ(fixture.Run().state,
            ManagerRuntimeConvergenceResultState::kStagedReady);
  ScriptedSceneOwner scene_owner(fixture.independent_reader);
  scene_owner.throw_after_stage_effect = true;
  ManagerSceneStageCoordinator coordinator(fixture.journal,
                                           *fixture.package.bundle_source,
                                           fixture.publication, scene_owner);

  try {
    static_cast<void>(
        coordinator.StageOrRecover(ApplyId("apply-1"), ApplyGeneration(1U)));
    FAIL() << "expected a lost Stage response";
  } catch (const ManagerSceneStageEffectUncertainError &error) {
    EXPECT_TRUE(error.recovery_query().valid());
    EXPECT_EQ(error.recovery_query().apply_id, ApplyId("apply-1"));
  }
  EXPECT_TRUE(fixture.journal
                  .ReadSceneStageIntent(ApplyId("apply-1"), ApplyGeneration(1U))
                  .has_value());
  EXPECT_FALSE(
      fixture.journal.ReadSceneStaged(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
  EXPECT_EQ(scene_owner.stage_calls, 1);

  scene_owner.throw_after_stage_effect = false;
  const auto recovered =
      coordinator.StageOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(recovered.path, ManagerSceneStagePath::kRecoveredOwnerStatus);
  EXPECT_TRUE(recovered.durable_scene_staged.inserted);
  EXPECT_EQ(scene_owner.stage_calls, 1);
  EXPECT_EQ(scene_owner.status_calls, 2);
}

TEST(ManagerSceneStageCoordinator,
     QuiescingAndMalformedStatusRemainExplicitlyRecoverable) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ASSERT_EQ(fixture.Run().state,
            ManagerRuntimeConvergenceResultState::kStagedReady);
  ScriptedSceneOwner scene_owner(fixture.independent_reader);
  scene_owner.quiescing = true;
  ManagerSceneStageCoordinator coordinator(fixture.journal,
                                           *fixture.package.bundle_source,
                                           fixture.publication, scene_owner);

  try {
    static_cast<void>(
        coordinator.StageOrRecover(ApplyId("apply-1"), ApplyGeneration(1U)));
    FAIL() << "expected quiescing recovery context";
  } catch (const ManagerSceneStageEffectUncertainError &error) {
    ASSERT_TRUE(error.observed_status().has_value());
    EXPECT_EQ(error.observed_status()->state,
              ProjectionApplyStatusState::kQuiescing);
  }
  EXPECT_EQ(scene_owner.stage_calls, 0);

  scene_owner.quiescing = false;
  scene_owner.foreign_status = true;
  EXPECT_THROW(static_cast<void>(coordinator.StageOrRecover(
                   ApplyId("apply-1"), ApplyGeneration(1U))),
               ManagerSceneStageEffectUncertainError);
  EXPECT_EQ(scene_owner.stage_calls, 0);

  scene_owner.foreign_status = false;
  const auto recovered =
      coordinator.StageOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(recovered.path, ManagerSceneStagePath::kStagedFromNewCall);
  EXPECT_EQ(scene_owner.stage_calls, 1);
}

TEST(ManagerSceneStageCoordinator,
     PublicationDriftStopsBeforeOwnerObservationOrStage) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ASSERT_EQ(fixture.Run().state,
            ManagerRuntimeConvergenceResultState::kStagedReady);
  ScriptedSceneOwner scene_owner(fixture.independent_reader);
  ManagerSceneStageCoordinator coordinator(fixture.journal,
                                           *fixture.package.bundle_source,
                                           fixture.publication, scene_owner);
  fixture.publication.fail_on_call = fixture.publication.read_calls + 1;

  EXPECT_THROW(static_cast<void>(coordinator.StageOrRecover(
                   ApplyId("apply-1"), ApplyGeneration(1U))),
               ManagerSceneStageCoordinatorError);
  EXPECT_EQ(scene_owner.status_calls, 0);
  EXPECT_EQ(scene_owner.stage_calls, 0);
  EXPECT_TRUE(fixture.journal
                  .ReadSceneStageIntent(ApplyId("apply-1"), ApplyGeneration(1U))
                  .has_value());
  EXPECT_FALSE(
      fixture.journal.ReadSceneStaged(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
}

TEST(ManagerSceneActivationCoordinator,
     PersistsSelectionBeforeActivationAndReplaysWithoutAnotherOwnerCall) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ASSERT_EQ(fixture.Run().state,
            ManagerRuntimeConvergenceResultState::kStagedReady);
  ScriptedSceneOwner scene_owner(fixture.independent_reader);
  ManagerSceneStageCoordinator stage_coordinator(
      fixture.journal, *fixture.package.bundle_source, fixture.publication,
      scene_owner);
  ASSERT_EQ(
      stage_coordinator.StageOrRecover(ApplyId("apply-1"), ApplyGeneration(1U))
          .path,
      ManagerSceneStagePath::kStagedFromNewCall);
  ManagerSceneActivationCoordinator activation_coordinator(fixture.journal,
                                                           scene_owner);

  const auto activated = activation_coordinator.ActivateOrRecover(
      ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(activated.path, ManagerSceneActivationPath::kActivatedFromNewCall);
  EXPECT_TRUE(activated.durable_selection.inserted);
  EXPECT_TRUE(activated.durable_scene_activated.inserted);
  EXPECT_TRUE(scene_owner.saw_durable_selection_before_activation);
  EXPECT_EQ(scene_owner.activation_calls, 1);

  const auto replay = activation_coordinator.ActivateOrRecover(
      ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(replay.path, ManagerSceneActivationPath::kDurableActivatedReplay);
  EXPECT_FALSE(replay.durable_selection.inserted);
  EXPECT_FALSE(replay.durable_scene_activated.inserted);
  EXPECT_EQ(replay.durable_scene_activated.record.scene_activated_digest,
            activated.durable_scene_activated.record.scene_activated_digest);
  EXPECT_EQ(scene_owner.activation_calls, 1);
  const auto report = fixture.journal.VerifyIntegrity();
  EXPECT_EQ(report.activation_selection_count, 1);
  EXPECT_EQ(report.scene_activated_count, 1);
}

TEST(ManagerSceneActivationCoordinator,
     LostActivationResponseRecoversFromOwnerStatusWithoutDuplicateEffect) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ASSERT_EQ(fixture.Run().state,
            ManagerRuntimeConvergenceResultState::kStagedReady);
  ScriptedSceneOwner scene_owner(fixture.independent_reader);
  ManagerSceneStageCoordinator stage_coordinator(
      fixture.journal, *fixture.package.bundle_source, fixture.publication,
      scene_owner);
  static_cast<void>(stage_coordinator.StageOrRecover(ApplyId("apply-1"),
                                                     ApplyGeneration(1U)));
  scene_owner.throw_after_activation_effect = true;
  ManagerSceneActivationCoordinator activation_coordinator(fixture.journal,
                                                           scene_owner);

  try {
    static_cast<void>(activation_coordinator.ActivateOrRecover(
        ApplyId("apply-1"), ApplyGeneration(1U)));
    FAIL() << "expected a lost activation response";
  } catch (const ManagerSceneActivationEffectUncertainError &error) {
    EXPECT_TRUE(error.recovery_query().valid());
    EXPECT_EQ(error.recovery_query().apply_id, ApplyId("apply-1"));
  }
  EXPECT_TRUE(
      fixture.journal
          .ReadActivationSelection(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
  EXPECT_FALSE(fixture.journal
                   .ReadSceneActivated(ApplyId("apply-1"), ApplyGeneration(1U))
                   .has_value());
  EXPECT_EQ(scene_owner.activation_calls, 1);

  scene_owner.throw_after_activation_effect = false;
  const auto recovered = activation_coordinator.ActivateOrRecover(
      ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(recovered.path, ManagerSceneActivationPath::kRecoveredOwnerStatus);
  EXPECT_TRUE(recovered.durable_scene_activated.inserted);
  EXPECT_EQ(scene_owner.activation_calls, 1);
}

TEST(ManagerSelectedRuntimeCompositionCoordinator,
     PersistsOnlyAfterInstallAndReobservesTheCurrentChildOnReplay) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ScriptedSceneOwner scene_owner(fixture.independent_reader);
  ReachDurableSceneActivation(fixture, scene_owner);
  ScriptedRuntimeCompositionPort runtime_composition;
  runtime_composition.prepare_once = true;
  ManagerSelectedRuntimeCompositionCoordinator coordinator(
      fixture.journal, *fixture.package.bundle_source, runtime_composition,
      RuntimeEpoch("manager-runtime-1"));

  const auto preparing =
      coordinator.InstallOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(preparing.state,
            ManagerSelectedRuntimeCompositionResultState::kPending);
  EXPECT_FALSE(preparing.durable_materialization.has_value());
  EXPECT_FALSE(
      fixture.journal
          .ReadPrivateMaterialization(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());

  const auto installed =
      coordinator.InstallOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(installed.state,
            ManagerSelectedRuntimeCompositionResultState::kInstalled);
  ASSERT_TRUE(installed.durable_materialization.has_value());
  EXPECT_TRUE(installed.durable_materialization->inserted);
  ASSERT_TRUE(runtime_composition.last_request.has_value());
  EXPECT_EQ(runtime_composition.last_request->manager_runtime_epoch,
            RuntimeEpoch("manager-runtime-1"));
  EXPECT_EQ(runtime_composition.calls, 2);

  const auto replay =
      coordinator.InstallOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(replay.state,
            ManagerSelectedRuntimeCompositionResultState::kInstalled);
  ASSERT_TRUE(replay.durable_materialization.has_value());
  EXPECT_FALSE(replay.durable_materialization->inserted);
  EXPECT_EQ(replay.durable_materialization->record,
            installed.durable_materialization->record);
  EXPECT_EQ(runtime_composition.calls, 3);
}

TEST(ManagerSelectedRuntimeCompositionCoordinator,
     RejectsForeignOwnerEvidenceBeforeDurableMaterialization) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ScriptedSceneOwner scene_owner(fixture.independent_reader);
  ReachDurableSceneActivation(fixture, scene_owner);
  ScriptedRuntimeCompositionPort runtime_composition;
  runtime_composition.foreign_response = true;
  ManagerSelectedRuntimeCompositionCoordinator coordinator(
      fixture.journal, *fixture.package.bundle_source, runtime_composition,
      RuntimeEpoch("manager-runtime-1"));

  EXPECT_THROW(static_cast<void>(coordinator.InstallOrRecover(
                   ApplyId("apply-1"), ApplyGeneration(1U))),
               ManagerSelectedRuntimeCompositionCoordinatorError);
  EXPECT_FALSE(
      fixture.journal
          .ReadPrivateMaterialization(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
}

TEST(ManagerSelectedBundleTransitionCoordinator,
     PublishesThenFinalizesWithOneExternalEffectPerAdvance) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ScriptedSceneOwner scene_owner(fixture.independent_reader);
  ReachDurableSceneActivation(fixture, scene_owner);
  ScriptedRuntimeCompositionPort runtime_composition;
  ReachDurableRuntimeComposition(fixture, runtime_composition);
  ScriptedPublicationStore publication_store(
      PredecessorPublication(fixture.apply_intent),
      &fixture.independent_reader);
  ManagerSelectedBundleTransitionCoordinator coordinator(
      fixture.journal, *fixture.package.bundle_source, publication_store,
      scene_owner);

  const auto published =
      coordinator.AdvanceOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(published.path,
            ManagerSelectedBundleTransitionPath::kPublishedFromNewCall);
  EXPECT_FALSE(published.durable_materialization.inserted);
  EXPECT_TRUE(published.durable_retired_command_surfaces.inserted);
  EXPECT_TRUE(published.durable_publication.inserted);
  EXPECT_FALSE(published.durable_finalized.has_value());
  EXPECT_EQ(publication_store.publish_calls, 1);
  EXPECT_TRUE(publication_store.saw_prerequisites_before_publish);
  EXPECT_EQ(scene_owner.finalize_calls, 0);

  const auto finalized =
      coordinator.AdvanceOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(finalized.path,
            ManagerSelectedBundleTransitionPath::kFinalizedFromNewCall);
  ASSERT_TRUE(finalized.durable_finalized.has_value());
  EXPECT_TRUE(finalized.durable_finalized->inserted);
  EXPECT_TRUE(scene_owner.saw_durable_publication_before_finalize);
  EXPECT_EQ(publication_store.publish_calls, 1);
  EXPECT_EQ(scene_owner.finalize_calls, 1);

  const auto replay =
      coordinator.AdvanceOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(replay.path,
            ManagerSelectedBundleTransitionPath::kDurableFinalizedReplay);
  ASSERT_TRUE(replay.durable_finalized.has_value());
  EXPECT_FALSE(replay.durable_finalized->inserted);
  EXPECT_EQ(publication_store.publish_calls, 1);
  EXPECT_EQ(scene_owner.finalize_calls, 1);
  const auto report = fixture.journal.VerifyIntegrity();
  EXPECT_EQ(report.private_materialization_count, 1);
  EXPECT_EQ(report.retired_command_surfaces_count, 1);
  EXPECT_EQ(report.publication_receipt_count, 1);
  EXPECT_EQ(report.scene_finalized_count, 1);
}

TEST(ManagerSelectedBundleTransitionCoordinator,
     LostPublicationResponseRecoversCurrentHeadWithoutDuplicatePublish) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ScriptedSceneOwner scene_owner(fixture.independent_reader);
  ReachDurableSceneActivation(fixture, scene_owner);
  ScriptedRuntimeCompositionPort runtime_composition;
  ReachDurableRuntimeComposition(fixture, runtime_composition);
  ScriptedPublicationStore publication_store(
      PredecessorPublication(fixture.apply_intent),
      &fixture.independent_reader);
  publication_store.throw_after_publish_effect = true;
  ManagerSelectedBundleTransitionCoordinator coordinator(
      fixture.journal, *fixture.package.bundle_source, publication_store,
      scene_owner);

  try {
    static_cast<void>(
        coordinator.AdvanceOrRecover(ApplyId("apply-1"), ApplyGeneration(1U)));
    FAIL() << "expected uncertain lost publication response";
  } catch (const ManagerSelectedBundleTransitionEffectUncertainError &error) {
    EXPECT_TRUE(error.publication_request().valid());
    EXPECT_EQ(error.scene_status_query().apply_id, ApplyId("apply-1"));
  }
  EXPECT_EQ(publication_store.publish_calls, 1);
  EXPECT_FALSE(
      fixture.journal
          .ReadPublicationReceipt(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());

  const auto recovered =
      coordinator.AdvanceOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(recovered.path,
            ManagerSelectedBundleTransitionPath::kRecoveredPublicationHead);
  EXPECT_TRUE(recovered.durable_publication.inserted);
  EXPECT_EQ(publication_store.publish_calls, 1);
  EXPECT_EQ(scene_owner.finalize_calls, 0);
}

TEST(ManagerSelectedBundleTransitionCoordinator,
     LostFinalizeResponseRecoversOwnerStatusWithoutDuplicateFinalize) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ScriptedSceneOwner scene_owner(fixture.independent_reader);
  ReachDurableSceneActivation(fixture, scene_owner);
  ScriptedRuntimeCompositionPort runtime_composition;
  ReachDurableRuntimeComposition(fixture, runtime_composition);
  ScriptedPublicationStore publication_store(
      PredecessorPublication(fixture.apply_intent),
      &fixture.independent_reader);
  ManagerSelectedBundleTransitionCoordinator coordinator(
      fixture.journal, *fixture.package.bundle_source, publication_store,
      scene_owner);
  ASSERT_EQ(
      coordinator.AdvanceOrRecover(ApplyId("apply-1"), ApplyGeneration(1U))
          .path,
      ManagerSelectedBundleTransitionPath::kPublishedFromNewCall);

  scene_owner.throw_after_finalize_effect = true;
  EXPECT_THROW(static_cast<void>(coordinator.AdvanceOrRecover(
                   ApplyId("apply-1"), ApplyGeneration(1U))),
               ManagerSelectedBundleTransitionEffectUncertainError);
  EXPECT_EQ(scene_owner.finalize_calls, 1);
  EXPECT_FALSE(fixture.journal
                   .ReadSceneFinalized(ApplyId("apply-1"), ApplyGeneration(1U))
                   .has_value());

  const auto recovered =
      coordinator.AdvanceOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(
      recovered.path,
      ManagerSelectedBundleTransitionPath::kRecoveredFinalizedOwnerStatus);
  ASSERT_TRUE(recovered.durable_finalized.has_value());
  EXPECT_TRUE(recovered.durable_finalized->inserted);
  EXPECT_EQ(scene_owner.finalize_calls, 1);
  EXPECT_EQ(publication_store.publish_calls, 1);
}

TEST(ManagerApplyFenceReleaseCoordinator,
     PersistsIntentBeforeReleaseAndTerminalReplaysWithoutKernelCalls) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ScriptedSceneOwner scene_owner(fixture.independent_reader);
  ScriptedPublicationStore publication_store(
      PredecessorPublication(fixture.apply_intent),
      &fixture.independent_reader);
  ReachDurableSceneFinalize(fixture, scene_owner, publication_store);
  ScriptedFenceReleasePort release_port(fixture.independent_reader);
  ManagerApplyFenceReleaseCoordinator coordinator(
      fixture.journal, release_port, RuntimeEpoch("manager-runtime-1"));

  const auto released =
      coordinator.ReleaseOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(released.path, ManagerApplyFenceReleasePath::kReleasedFromNewCall);
  EXPECT_TRUE(released.durable_release_intent.inserted);
  EXPECT_TRUE(released.durable_release.inserted);
  EXPECT_TRUE(released.durable_terminal_outcome.inserted);
  EXPECT_TRUE(release_port.saw_durable_intent_before_release);
  EXPECT_EQ(release_port.status_calls, 1);
  EXPECT_EQ(release_port.release_calls, 1);

  const auto replay =
      coordinator.ReleaseOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(replay.path, ManagerApplyFenceReleasePath::kDurableTerminalReplay);
  EXPECT_FALSE(replay.durable_release_intent.inserted);
  EXPECT_FALSE(replay.durable_release.inserted);
  EXPECT_FALSE(replay.durable_terminal_outcome.inserted);
  EXPECT_EQ(release_port.status_calls, 1);
  EXPECT_EQ(release_port.release_calls, 1);
  const auto report = fixture.journal.VerifyIntegrity();
  EXPECT_EQ(report.fence_release_intent_count, 1);
  EXPECT_EQ(report.fence_released_count, 1);
  EXPECT_EQ(report.terminal_outcome_count, 1);
}

TEST(ManagerApplyFenceReleaseCoordinator,
     LostReleaseResponseRecoversExactStatusWithoutDuplicateRelease) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ScriptedSceneOwner scene_owner(fixture.independent_reader);
  ScriptedPublicationStore publication_store(
      PredecessorPublication(fixture.apply_intent),
      &fixture.independent_reader);
  ReachDurableSceneFinalize(fixture, scene_owner, publication_store);
  ScriptedFenceReleasePort release_port(fixture.independent_reader);
  release_port.throw_after_release_effect = true;
  ManagerApplyFenceReleaseCoordinator coordinator(
      fixture.journal, release_port, RuntimeEpoch("manager-runtime-1"));

  try {
    static_cast<void>(
        coordinator.ReleaseOrRecover(ApplyId("apply-1"), ApplyGeneration(1U)));
    FAIL() << "expected a lost Kernel Release response";
  } catch (const ManagerApplyFenceReleaseEffectUncertainError &error) {
    EXPECT_TRUE(error.release_invoked_in_this_call());
    EXPECT_TRUE(error.status_query().valid());
    EXPECT_TRUE(error.durable_release_request().has_value());
  }
  EXPECT_TRUE(
      fixture.journal
          .ReadFenceReleaseIntent(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
  EXPECT_FALSE(
      fixture.journal.ReadFenceReleased(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
  EXPECT_FALSE(fixture.journal
                   .ReadTerminalOutcome(ApplyId("apply-1"), ApplyGeneration(1U))
                   .has_value());
  EXPECT_EQ(release_port.release_calls, 1);

  release_port.throw_after_release_effect = false;
  const auto recovered =
      coordinator.ReleaseOrRecover(ApplyId("apply-1"), ApplyGeneration(1U));
  EXPECT_EQ(recovered.path,
            ManagerApplyFenceReleasePath::kRecoveredReleasedStatus);
  EXPECT_TRUE(recovered.durable_release.inserted);
  EXPECT_TRUE(recovered.durable_terminal_outcome.inserted);
  EXPECT_EQ(release_port.status_calls, 2);
  EXPECT_EQ(release_port.release_calls, 1);
}

TEST(ManagerApplyFenceReleaseCoordinator,
     PendingOrForeignStatusNeverPersistsIntentOrInvokesRelease) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ScriptedSceneOwner scene_owner(fixture.independent_reader);
  ScriptedPublicationStore publication_store(
      PredecessorPublication(fixture.apply_intent),
      &fixture.independent_reader);
  ReachDurableSceneFinalize(fixture, scene_owner, publication_store);
  ScriptedFenceReleasePort release_port(fixture.independent_reader);
  release_port.state = ConfigurationFenceState::kReleasePending;
  ManagerApplyFenceReleaseCoordinator coordinator(
      fixture.journal, release_port, RuntimeEpoch("manager-runtime-1"));

  EXPECT_THROW(static_cast<void>(coordinator.ReleaseOrRecover(
                   ApplyId("apply-1"), ApplyGeneration(1U))),
               ManagerApplyFenceReleaseEffectUncertainError);
  EXPECT_EQ(release_port.release_calls, 0);
  EXPECT_FALSE(
      fixture.journal
          .ReadFenceReleaseIntent(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());

  release_port.state = ConfigurationFenceState::kHeld;
  release_port.foreign_public_pointer = true;
  EXPECT_THROW(static_cast<void>(coordinator.ReleaseOrRecover(
                   ApplyId("apply-1"), ApplyGeneration(1U))),
               ManagerApplyFenceReleaseEffectUncertainError);
  EXPECT_EQ(release_port.release_calls, 0);
  EXPECT_FALSE(
      fixture.journal
          .ReadFenceReleaseIntent(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
}

TEST(ManagerNamedApplyCoordinator,
     ComposesTheDurableI4PhasesAndReplaysWithoutDuplicateEffects) {
  ScopedPackageRoot root;
  auto package = MaterializeRuntimePackage(root.path());
  const auto intent_template = ApplyIntent(*package.bundle);
  const auto predecessor = PredecessorPublication(intent_template);
  SqliteManagerApplyJournal journal(root.path() / "aggregate-apply.sqlite3",
                                    package.bundle->value().cell_graph.cell_id);
  SqliteManagerApplyJournal independent_reader(
      root.path() / "aggregate-apply.sqlite3",
      package.bundle->value().cell_graph.cell_id);
  ScriptedPublicationStore publication_store(predecessor, &independent_reader);
  FixedActiveRuntimePlanSource active_plan_source(
      predecessor, BuildVerifiedRuntimeConvergencePlan(
                       *package.bundle, *package.artifact_source, "linux",
                       {"posix.1-2017"}));
  ScriptedRuntimePorts runtime_ports(independent_reader);
  ScriptedSceneOwner scene_owner(independent_reader);
  AggregateFencePort kernel_fence(independent_reader, predecessor);
  ScriptedRuntimeCompositionPort runtime_composition;
  ScriptedCellAdmissionPort cell_admission;
  ManagerNamedApplyCoordinator coordinator(
      journal, active_plan_source, *package.bundle_source,
      *package.artifact_source, publication_store, scene_owner, nullptr,
      kernel_fence, cell_admission, runtime_composition, runtime_ports,
      runtime_ports,
      RuntimeEpoch("manager-runtime.aggregate.1"), "linux", {"posix.1-2017"},
      [] { return 1'000U; }, [] { return ApplyId("apply.aggregate.1"); });
  const ManagerNamedApplyRequestDTO request{
      intent_template.request_context,
      package.bundle->value().deployment_artifact_id,
      package.bundle->value().deployment_revision,
      predecessor.request.candidate_retained_bundle_id,
      predecessor.request.candidate_graph_revision,
      package.bundle->retained_bundle_id(),
      core::AdmissionGeneration(7U),
      std::nullopt,
  };

  const auto applied = coordinator.ApplyOrRecover(request);
  EXPECT_EQ(applied.state, ManagerNamedApplyResultState::kSucceeded);
  EXPECT_FALSE(applied.request_replayed);
  ASSERT_TRUE(applied.terminal_outcome.has_value());
  EXPECT_EQ(applied.apply_id, ApplyId("apply.aggregate.1"));
  EXPECT_EQ(kernel_fence.observe_admission_calls, 1);
  EXPECT_EQ(kernel_fence.acquire_calls, 1);
  EXPECT_EQ(kernel_fence.release_calls, 1);
  ASSERT_TRUE(cell_admission.last_release.has_value());
  EXPECT_EQ(cell_admission.last_release->settlement.settlement_kind,
            core::CellAdmissionSettlementKindDTO::kTerminalEffectsSettled);
  const auto durable_terminal = journal.ReadTerminalOutcome(
      ApplyId("apply.aggregate.1"), ApplyGeneration(1U));
  ASSERT_TRUE(durable_terminal.has_value());
  EXPECT_EQ(cell_admission.last_release->settlement.evidence_digest,
            durable_terminal->terminal_outcome_digest);
  EXPECT_EQ(runtime_ports.ensure_calls, 1);
  EXPECT_EQ(scene_owner.stage_calls, 1);
  EXPECT_EQ(scene_owner.activation_calls, 1);
  EXPECT_EQ(runtime_composition.calls, 1);
  EXPECT_EQ(publication_store.publish_calls, 1);
  EXPECT_EQ(scene_owner.finalize_calls, 1);

  const auto replayed = coordinator.ApplyOrRecover(request);
  EXPECT_EQ(replayed.state, ManagerNamedApplyResultState::kSucceeded);
  EXPECT_TRUE(replayed.request_replayed);
  EXPECT_EQ(replayed.terminal_outcome, applied.terminal_outcome);
  EXPECT_EQ(kernel_fence.observe_admission_calls, 1);
  EXPECT_EQ(kernel_fence.acquire_calls, 1);
  EXPECT_EQ(kernel_fence.release_calls, 1);
  EXPECT_EQ(runtime_ports.ensure_calls, 1);
  EXPECT_EQ(scene_owner.stage_calls, 1);
  EXPECT_EQ(scene_owner.activation_calls, 1);
  EXPECT_EQ(runtime_composition.calls, 1);
  EXPECT_EQ(publication_store.publish_calls, 1);
  EXPECT_EQ(scene_owner.finalize_calls, 1);
  EXPECT_FALSE(coordinator.RecoverIncompleteApply().has_value());

  const auto report = journal.VerifyIntegrity();
  EXPECT_EQ(report.apply_intent_count, 1);
  EXPECT_EQ(report.fence_acceptance_count, 1);
  EXPECT_EQ(report.runtime_staged_ready_count, 1);
  EXPECT_EQ(report.scene_staged_count, 1);
  EXPECT_EQ(report.scene_activated_count, 1);
  EXPECT_EQ(report.publication_receipt_count, 1);
  EXPECT_EQ(report.scene_finalized_count, 1);
  EXPECT_EQ(report.fence_released_count, 1);
  EXPECT_EQ(report.terminal_outcome_count, 1);
}

} // namespace
} // namespace manyforge::component_manager
