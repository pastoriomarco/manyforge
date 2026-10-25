#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/apply_affected_scope.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <memory>
#include <set>
#include <manyforge_scene/logical_scene_owner.hpp>
#include <manyforge_scene/logical_scene_value_codec.hpp>
#include <manyforge_core/ports/iplanner_scene_projection.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
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
#include "canonical_empty_admission_internal.hpp"

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
MaterializeRosRuntimePackage(const std::filesystem::path &root,
                             bool empty = false,
                             std::string_view motion_limit_target =
                                 deployment::test::kLite6ArmTargetId,
                             std::string_view lifecycle_digest = {},
                             std::string_view authored_name = {}) {
  auto fixture = deployment::test::make_lite6_controllable_retention_fixture(
      false, false, 1U, false, true, "runtime_only_cell", false, false,
      "resource", false, lifecycle_digest, {}, true, false, false, std::nullopt, false,
      motion_limit_target);
  const deployment::test::Lite6ControllableCompilerInputArtifactSource
      input_source(fixture);
  auto qualified = fixture.qualified;
  if (empty) {
    const std::vector<deployment::QualifiedRuntimeProfile> profiles{
        *fixture.ros2_control_execution_runtime_profile};
    auto derived =
        deployment::detail::derive_cell_scoped_canonical_empty_deployment(
            fixture.qualified.deployment(), profiles);
    if (!authored_name.empty()) {
      // Produce a distinct authoring revision through the actual compiler;
      // keep the host graph/profile/infrastructure and zero component closure.
      auto authored = derived.normalized_authored_root().as_object();
      const auto name = std::find_if(authored.begin(), authored.end(),
          [](const auto &field) { return field.first == "name"; });
      if (name == authored.end()) {
        throw std::runtime_error("ROS host-only fixture has no authored name");
      }
      name->second = deployment::CanonicalValue(std::string(authored_name));
      derived = deployment::compute_deployment_artifact_revision(
          deployment::parse_strict_json_model_yaml(deployment::canonical_json(
              deployment::CanonicalValue(std::move(authored)))), profiles);
    }
    const auto composition = deployment::compose_cell_occurrences(
        derived.graph(), {}, {});
    const auto overlay = deployment::select_site_overlay_for_deployment(
        derived.applicability_inputs().value().site_overlay_ref,
        derived.applicability_inputs().value().external_resources, nullptr);
    const auto applicability = deployment::qualify_cell_applicability(
        composition, derived.applicability_inputs(), profiles, {}, {}, {},
        {}, {}, {}, {}, {}, overlay);
    qualified = deployment::qualify_deployment_for_retention(
        derived, deployment::compile_cell_ir(applicability));
  }
  deployment::FilesystemBuildManifestRepository manifests(root);
  deployment::FilesystemCatalogArtifactStore catalogs(root);
  deployment::FilesystemRawArtifactStore artifacts(root);
  deployment::FilesystemRetainedBundleRepository bundles(root);
  auto bundle = deployment::detail::RetainedBundleMaterializer::
      materialize_store_and_reload_production(
          qualified, manifests, input_source,
          empty ? std::span<const deployment::BuildQualifiedStructuralModelPreparation>{}
                : fixture.structural_preparations(),
          catalogs, artifacts, bundles);
  if (bundle == nullptr) {
    throw std::runtime_error("ROS continuity fixture produced no retained bundle");
  }
  if (empty) {
    const auto configuration =
        deployment::ros2_control_execution_runtime_contract::
            require_configuration(*bundle, bundle->value().cell_graph.runtime_units.front().runtime_unit_id);
    if (!configuration.controller_profiles.empty() ||
        !configuration.initial_joint_positions_rad.empty() ||
        !configuration.motion_target_limits.empty() ||
        !configuration.default_fault_stop_profiles.empty()) {
      throw std::runtime_error(
          "derived canonical-empty ROS host retained component configuration");
    }
  }
  return {
      std::move(bundle),
      std::make_unique<deployment::FilesystemVerifiedRetainedBundleSource>(root),
      std::make_unique<deployment::FilesystemVerifiedRetainedProductArtifactSource>(root),
  };
}

[[nodiscard]] RuntimePackage MaterializeNoneInventoryPackage(
    const std::filesystem::path &root, std::string_view output_mode = "none",
    std::string_view actuation_kind = "pneumatic", bool empty = false, bool include_arm = false,
    bool feedback_alias = false) {
  auto fixture = (include_arm || feedback_alias)
      ? deployment::test::make_lite6_controllable_retention_fixture(
          false, false, 1U, true, true, {}, false, true, "resource", true,
          {}, {}, true, feedback_alias, !include_arm, std::nullopt, false,
          deployment::test::kLite6ArmTargetId, std::nullopt, std::nullopt, {}, true, {},
          "manyforge.provider.ros2_control", true, true, nullptr, nullptr,
          false, false, actuation_kind, 0.2, output_mode)
      : deployment::test::make_lite6_ros2_control_gripper_only_cclc_product_retention_fixture(
          {}, actuation_kind, 0.2, nullptr, output_mode);
  const deployment::test::Lite6ControllableCompilerInputArtifactSource inputs(fixture);
  auto qualified = fixture.qualified;
  if (empty) {
    const std::vector<deployment::QualifiedRuntimeProfile> profiles{*fixture.ros2_control_execution_runtime_profile};
    auto authored = deployment::detail::derive_cell_scoped_canonical_empty_deployment(
        fixture.qualified.deployment(), profiles);
    const auto composition = deployment::compose_cell_occurrences(authored.graph(), {}, {});
    const auto overlay = deployment::select_site_overlay_for_deployment(
        authored.applicability_inputs().value().site_overlay_ref,
        authored.applicability_inputs().value().external_resources, nullptr);
    const auto applicable = deployment::qualify_cell_applicability(
        composition, authored.applicability_inputs(), profiles, {}, {}, {}, {}, {}, {}, {}, {}, overlay);
    qualified = deployment::qualify_deployment_for_retention(authored, deployment::compile_cell_ir(applicable));
  }
  deployment::FilesystemBuildManifestRepository manifests(root);
  deployment::FilesystemCatalogArtifactStore catalogs(root);
  deployment::FilesystemRawArtifactStore artifacts(root);
  deployment::FilesystemRetainedBundleRepository bundles(root);
  auto bundle = deployment::detail::RetainedBundleMaterializer::materialize_store_and_reload_production(
      qualified, manifests, inputs,
      empty ? std::span<const deployment::BuildQualifiedStructuralModelPreparation>{}
            : fixture.structural_preparations(), catalogs, artifacts, bundles);
  if (!bundle) throw std::runtime_error("None inventory fixture produced no verified retained bundle");
  return {std::move(bundle),
          std::make_unique<deployment::FilesystemVerifiedRetainedBundleSource>(root),
          std::make_unique<deployment::FilesystemVerifiedRetainedProductArtifactSource>(root)};
}

TEST(ManagerRuntimeConvergenceFixture,
     PopulatedDeploymentDerivesADecodableCanonicalEmptyRosHost) {
  ScopedPackageRoot package_root;
  const auto package = MaterializeRosRuntimePackage(package_root.path(), true);
  ASSERT_NE(package.bundle, nullptr);
  const auto &graph = package.bundle->value().cell_graph;
  EXPECT_TRUE(graph.controllable_components.empty());
  EXPECT_TRUE(graph.control_bindings.empty());
  ASSERT_EQ(graph.runtime_units.size(), 1U);
  const auto configuration =
      deployment::ros2_control_execution_runtime_contract::
          require_configuration(*package.bundle,
                                graph.runtime_units.front().runtime_unit_id);
  EXPECT_TRUE(configuration.controller_profiles.empty());
  EXPECT_TRUE(configuration.initial_joint_positions_rad.empty());
  EXPECT_TRUE(configuration.motion_target_limits.empty());
  EXPECT_TRUE(configuration.default_fault_stop_profiles.empty());
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

  void SetCurrent(ActiveBundlePublicationReceiptDTO receipt) { current_ = std::move(receipt); }

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
    auto epoch = original_execution_host.value_or(ExecutionHostEpochDTO{
        RuntimeUnitId("runtime.kernel"),
        RuntimeEpoch("kernel-runtime-aggregate-1"),
        RequestOwnerEpoch("kernel-owner-aggregate-1"),
        ContentDigest(std::string(kDigestOne)),
        predecessor_.publication_generation});
    epoch.publication_generation = predecessor_.publication_generation;
    return {
        query,
        std::move(epoch),
        predecessor_.request.candidate_projection_revision,
        predecessor_.durable_public_pointer_content_digest,
        0U,
        ConfigurationFenceGeneration(0U),
        predecessor_configurations,
        0U,
        0U,
        ContentDigest(std::string(kDigestTwo)),
    };
  }

  [[nodiscard]] ConfigurationFenceAcceptedDTO
  Acquire(const ApplyConfigurationFenceIntentDTO &intent) override {
    ++acquire_calls;
    captured_intent_ = intent;
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
    if (captured_intent_ && query.persisted_intent != *captured_intent_) {
      throw std::runtime_error("acquire-attempt query differs from the actual acquired intent");
    }
    auto epoch = original_execution_host.value_or(HostEpoch());
    epoch.publication_generation = predecessor_.publication_generation;
    if (stale_attempt_host) epoch.runtime_epoch = RuntimeEpoch("replaced-host");
    auto echoed = query;
    if (foreign_attempt_digest) echoed.persisted_attempt_digest = ContentDigest(std::string(kDigestTwo));
    auto acceptance = accepted_;
    if (foreign_attempt_acceptance && acceptance) {
      acceptance->configuration_fence_generation = ConfigurationFenceGeneration(
          acceptance->configuration_fence_generation.value() + 1U);
    }
    if (released_attempt || released_) {
      return {epoch, echoed, ApplyFenceAcquireAttemptReconcileState::kUncertain,
              std::nullopt, true, "fence was already released",
              ContentDigest(std::string(kDigestOne)),
              ErrorDTO{"apply_fence_already_released", "the original held fence is unavailable", {}}};
    }
    return {
        epoch, echoed,
        acceptance ? ApplyFenceAcquireAttemptReconcileState::kAcceptedExact
                   : ApplyFenceAcquireAttemptReconcileState::kNotSeen,
        acceptance, acceptance.has_value(), "",
        ContentDigest(std::string(kDigestOne)), std::nullopt,
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
    if (std::exchange(throw_next_status, false))
      throw std::runtime_error("actual parent fence lookup unavailable");
    const auto publication = independent_reader_.ReadPublicationReceipt(
        query.owner_transaction.apply_id,
        query.owner_transaction.apply_generation);
    if (!publication.has_value()) {
      if (!allow_prepublication_status || !accepted_ ||
          query.configuration_fence_id != accepted_->configuration_fence_id ||
          query.configuration_fence_generation != accepted_->configuration_fence_generation ||
          query.expected_scope != accepted_->captured_scope) {
        throw std::runtime_error(
            "aggregate fence status lacks completed publication");
      }
    }
    const auto &receipt = publication ? publication->publication.publication_receipt : predecessor_;
    const auto &candidate = receipt.request.candidate_projection_revision;
    auto epoch = original_execution_host.value_or(ExecutionHostEpochDTO{
        RuntimeUnitId("runtime.kernel"), RuntimeEpoch("kernel-runtime-aggregate-1"),
        RequestOwnerEpoch("kernel-owner-aggregate-1"), ContentDigest(std::string(kDigestOne)),
        receipt.publication_generation});
    epoch.publication_generation = receipt.publication_generation;
    ConfigurationFenceReconciliationEvidenceDTO evidence{
        epoch,
        query.owner_transaction,
        query.configuration_fence_id,
        query.configuration_fence_generation,
        query.expected_scope,
        1U,
        candidate.graph_revision,
        candidate,
        receipt.publication_generation,
        receipt.durable_public_pointer_content_digest,
        installed_configurations,
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

  // These rows are produced by the fake Host's actual Prepare installation,
  // never by a desired configuration or pre-install held observation.
  std::vector<DomainActiveControlConfigurationDTO> predecessor_configurations{};
  std::vector<DomainActiveControlConfigurationDTO> installed_configurations{};
  std::optional<ExecutionHostEpochDTO> original_execution_host{};
  bool foreign_attempt_digest{}, stale_attempt_host{}, released_attempt{}, foreign_attempt_acceptance{};
  mutable bool throw_next_status{false};
  bool allow_prepublication_status{false};
  mutable int observe_admission_calls{0};
  int acquire_calls{0};
  mutable int reconcile_calls{0};
  int rehydrate_calls{0};
  mutable int status_calls{0};
  int release_calls{0};

private:
  SqliteManagerApplyJournal &independent_reader_;
  ActiveBundlePublicationReceiptDTO predecessor_;
  std::optional<ApplyConfigurationFenceIntentDTO> captured_intent_{};
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
  [[nodiscard]] Result<ManagerComponentProcessInventoryResultDTO>
  InspectInstalledComponentProcesses(
      const ManagerComponentProcessInventoryRequestDTO &request) override {
    if (installed_processes) return installed_processes(request);
    return IManagerSelectedRuntimeCompositionPort::
        InspectInstalledComponentProcesses(request);
  }

  [[nodiscard]] Result<ManagerComponentProcessStopResultDTO>
  StopComponentProcess(const ManagerComponentProcessStopRequestDTO &) override {
    ++component_stop_calls;
    return Result<ManagerComponentProcessStopResultDTO>::Err(
        {"fixture_unexpected_process_stop", "no child process Stop was expected", {}});
  }
  [[nodiscard]] Result<ManagerComponentProcessStopResultDTO>
  ReadComponentProcessStop(const ManagerComponentProcessStopReadRequestDTO &) override {
    ++component_stop_read_calls;
    return Result<ManagerComponentProcessStopResultDTO>::Err(
        {"fixture_unexpected_process_stop", "no child drain receipt was expected", {}});
  }
  int component_stop_calls{};
  int component_stop_read_calls{};

  std::function<Result<ManagerComponentProcessInventoryResultDTO>(
      const ManagerComponentProcessInventoryRequestDTO &)> installed_processes;

  [[nodiscard]] ManagerSelectedRuntimeCompositionResultDTO InstallOrObserve(
      const ManagerSelectedRuntimeCompositionRequestDTO &request) override {
    ++calls;
    phases.push_back(request.phase);
    last_request = request;
    if (calls == fail_on_call) {
      if (before_failure) before_failure(request);
      throw std::runtime_error("scripted exact composition failure");
    }
    auto response_request = request;
    if (foreign_response) {
      response_request.manager_runtime_epoch = RuntimeEpoch("foreign-manager");
    }
    const auto preparing = (prepare_once && calls == 1) ||
        (pending && pending(request));
    if (preparing) {
      return {std::move(response_request),
              ManagerSelectedRuntimeCompositionState::kPreparing,
              {}};
    }
    if (installed) installed(request);
    return {std::move(response_request),
            ManagerSelectedRuntimeCompositionState::kInstalled,
            omit_receipts ? std::vector<ManagerRuntimeComponentReceiptDTO>{}
                          : Receipts(request)};
  }

  [[nodiscard]] Result<ManagerApplyComponentBringUpResultDTO>
  SubmitApplyComponentBringUp(const ManagerApplyComponentBringUpRequestDTO &request) override {
    ++child_submit_calls;
    if (child_submit) return child_submit(request);
    return IManagerSelectedRuntimeCompositionPort::SubmitApplyComponentBringUp(request);
  }

  [[nodiscard]] Result<ManagerApplyComponentBringUpResultDTO>
  ReadApplyComponentBringUp(const ManagerApplyComponentBringUpReadRequestDTO &request) override {
    ++child_read_calls;
    if (child_read) return child_read(request.request);
    return IManagerSelectedRuntimeCompositionPort::ReadApplyComponentBringUp(request);
  }

  int child_submit_calls{0};
  int child_read_calls{0};
  std::function<Result<ManagerApplyComponentBringUpResultDTO>(
      const ManagerApplyComponentBringUpRequestDTO &)> child_submit;
  std::function<Result<ManagerApplyComponentBringUpResultDTO>(
      const ManagerApplyComponentBringUpRequestDTO &)> child_read;

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

  int fail_on_call{0};
  std::function<void(const ManagerSelectedRuntimeCompositionRequestDTO &)> installed;
  std::function<void(const ManagerSelectedRuntimeCompositionRequestDTO &)> before_failure;
  std::function<bool(const ManagerSelectedRuntimeCompositionRequestDTO &)> pending;
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
      const core::CellAdmissionReadinessSnapshotDTO &,
      std::shared_ptr<core::IIoSignalService>* actual_retired_service = nullptr) override {
    if (actual_retired_service) actual_retired_service->reset();
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        GetAuthoringAdmission());
  }

  [[nodiscard]] core::Result<core::CellAdmissionClaimDTO>
  AcquireCellAdmissionClaim(
      const core::CellAdmissionClaimIntentDTO &intent) override {
    ++acquire_calls;
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
    ++release_calls;
    if (refuse_release) {
      return core::Result<core::AuthoringAdmissionDTO>::Err(
          {"fixture_release_rejected", "scripted admission release refused", {}});
    }
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
    lookup_intents.push_back(query.intent);
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
    if (return_invalid_held_generation) {
      receipt.status = core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact;
      receipt.matched_claim = core::CellAdmissionClaimDTO{
          query.intent, core::CellAdmissionClaimGeneration(1U),
          core::AdmissionGeneration(query.intent.expected_admission_generation.value() + 1U), 1};
      receipt.held_claims.push_back(*receipt.matched_claim);
    } else if (held_claim_.has_value() && held_claim_->intent == query.intent) {
      receipt.status = core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact;
      receipt.matched_claim = held_claim_;
      receipt.held_claims.push_back(*held_claim_);
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
  int acquire_calls{0};
  int release_calls{0};
  bool refuse_release{false};
  bool return_invalid_held_generation{false};
  mutable std::vector<core::CellAdmissionClaimIntentDTO> lookup_intents;

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
      MaterializeRosRuntimePackage(native_root.path());
  auto candidate = MaterializeRuntimePackage(candidate_root.path());
  ASSERT_EQ(predecessor.bundle->value().cell_graph.cell_id,
            candidate.bundle->value().cell_graph.cell_id);
  ASSERT_EQ(predecessor.bundle->value().cell_graph.runtime_units.size(), 1U);
  ASSERT_EQ(predecessor.bundle->value().cell_graph.runtime_units.front()
                .runtime_profile_id.value(),
            deployment::ros2_control_execution_runtime_contract::
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
      {std::string(deployment::ros2_control_execution_runtime_contract::
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
     RosExecutionHostDerivedEmptyRetainsExactCandidateProduct) {
  ScopedPackageRoot root;
  const auto predecessor = MaterializeRosRuntimePackage(root.path(), true);
  const auto candidate = MaterializeRosRuntimePackage(root.path());
  EXPECT_TRUE(predecessor.bundle->value().cell_graph.controllable_components.empty());
  EXPECT_FALSE(candidate.bundle->value().cell_graph.controllable_components.empty());
  EXPECT_EQ(predecessor.bundle->value().cell_graph.runtime_units,
            candidate.bundle->value().cell_graph.runtime_units);
  EXPECT_EQ(predecessor.bundle->value().compiled_products.runtime_unit_products,
            candidate.bundle->value().compiled_products.runtime_unit_products);
  for (const auto *package : {&predecessor, &candidate}) {
    const auto product = deployment::verify_runtime_unit_product(
        *package->bundle,
        package->bundle->value().cell_graph.runtime_units.front().runtime_unit_id,
        *package->artifact_source);
    ASSERT_TRUE(product.has_value());
    ASSERT_TRUE(product->manager_liveness_policy().has_value());
    EXPECT_EQ(*product->manager_liveness_policy(),
              deployment::execution_runtime_contract::manager_liveness_policy());
  }
  EXPECT_TRUE(VerifyNativeExecutionHostContinuity(
      *predecessor.bundle, *candidate.bundle, *predecessor.artifact_source).ok());
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     RosExecutionHostRemovalOrReplacementRejectsBeforeRuntimeEffect) {
  ScopedPackageRoot root;
  ScopedPackageRoot state_root;
  const auto predecessor = MaterializeRosRuntimePackage(root.path(), true);
  const auto removed = MaterializeRuntimePackage(root.path(), true);
  const auto replaced = MaterializeRuntimePackage(root.path());
  const auto drifted = MaterializeRosRuntimePackage(root.path(), false, "unhosted_arm");
  // Native-to-ROS positive-family setup retires with the native executor.
  // The surviving ROS-to-POSIX replacement preserves before-effect refusal.
  EXPECT_TRUE(removed.bundle->value().cell_graph.runtime_units.empty());
  ASSERT_EQ(replaced.bundle->value().cell_graph.runtime_units.size(), 1U);
  EXPECT_EQ(replaced.bundle->value().cell_graph.runtime_units.front()
                .runtime_profile_id.value(),
            deployment::detail::kReferenceRuntimeProfileId);
  for (const auto *candidate : {&removed, &replaced}) {
    auto intent = ApplyIntent(*predecessor.bundle);
    intent.candidate_retained_bundle_id = candidate->bundle->retained_bundle_id();
    intent.candidate_graph_revision = candidate->bundle->value().cell_graph.graph_revision;
    const auto continuity = VerifyNativeExecutionHostContinuity(
        *predecessor.bundle, *candidate->bundle, *predecessor.artifact_source);
    ASSERT_FALSE(continuity.ok());
    EXPECT_EQ(continuity.error().code, "native_execution_host_incompatible");
    EXPECT_EQ(continuity.error().details.at("reason"),
              "candidate_native_host_missing");
    const auto database = state_root.path() /
        (candidate->bundle->retained_bundle_id().value().substr(7U) + ".sqlite3");
    SqliteManagerApplyJournal journal(database, intent.cell_id);
    SqliteManagerApplyJournal reader(database, intent.cell_id);
    CurrentPublicationReader publication(PredecessorPublication(intent));
    ScriptedRuntimePorts ports(reader);
    ManagerApplyRuntimeConvergenceCoordinator coordinator(
        journal, publication, *predecessor.artifact_source, ports, ports,
        "linux", std::string(deployment::ros2_control_execution_runtime_contract::
                                 kLaunchEnvironmentContract),
        [] { return 1'000U; });
    EXPECT_THROW(static_cast<void>(coordinator.ConvergeOrRecover(
        intent, *predecessor.bundle, *candidate->bundle,
        ActiveBundlePublicationGeneration(3U))),
        ManagerRuntimeConvergenceCoordinatorError);
    EXPECT_EQ(publication.read_calls, 0);
    EXPECT_EQ(ports.observe_calls, 0);
    EXPECT_EQ(ports.ensure_calls, 0);
    EXPECT_FALSE(journal.ReadIntent(intent.apply_id, intent.apply_generation).has_value());
    EXPECT_FALSE(journal.ReadFenceAttempt(intent.apply_id, intent.apply_generation).has_value());
    EXPECT_FALSE(journal.ReadFenceAcceptance(intent.apply_id, intent.apply_generation).has_value());
    EXPECT_FALSE(journal.ReadRuntimeConvergenceIntent(intent.apply_id, intent.apply_generation).has_value());
  }
  // The ROS runtime manifest is infrastructure-only, like native's (DX-228):
  // a controller-configuration change is not a host-product change, so host
  // continuity holds. The drifted candidate's motion-limit table names a
  // target its component does not host; that configuration must still be
  // refused before any fence or runtime effect.
  {
    auto intent = ApplyIntent(*predecessor.bundle);
    intent.candidate_retained_bundle_id = drifted.bundle->retained_bundle_id();
    intent.candidate_graph_revision = drifted.bundle->value().cell_graph.graph_revision;
    EXPECT_TRUE(VerifyNativeExecutionHostContinuity(
        *predecessor.bundle, *drifted.bundle, *predecessor.artifact_source).ok());
    const auto database = state_root.path() /
        (drifted.bundle->retained_bundle_id().value().substr(7U) + ".sqlite3");
    SqliteManagerApplyJournal journal(database, intent.cell_id);
    ExactFencePort kernel;
    ManagerApplyFenceCoordinator fence_coordinator(
        journal, kernel, RuntimeEpoch("manager-runtime-1"));
    try {
      static_cast<void>(fence_coordinator.AcquireOrRecoverFence(
          intent, *predecessor.bundle, *drifted.bundle,
          ManagerApplyFenceExpectationDTO{
              0U, ConfigurationFenceGeneration(0U), {}}));
      ADD_FAILURE() << "unhosted motion-limit target reached the Apply fence";
    } catch (const std::exception &error) {
      EXPECT_NE(std::string(error.what()).find("TCP-speed maximum"),
                std::string::npos)
          << error.what();
    }
    EXPECT_EQ(kernel.acquire_calls, 0);
    EXPECT_FALSE(journal.ReadIntent(intent.apply_id, intent.apply_generation).has_value());
    EXPECT_FALSE(journal.ReadFenceAttempt(intent.apply_id, intent.apply_generation).has_value());
    EXPECT_FALSE(journal.ReadFenceAcceptance(intent.apply_id, intent.apply_generation).has_value());
    EXPECT_FALSE(journal.ReadRuntimeConvergenceIntent(intent.apply_id, intent.apply_generation).has_value());
  }
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
     LinkedBringUpParentReplayAndRecoveryOnlyObserveRetainedFacts) {
  ScopedPackageRoot package_root;
  ScopedPackageRoot state_root;
  auto package = MaterializeRosRuntimePackage(package_root.path());
  RefusingBundleReloadSource bundle_source(*package.bundle_source);
  bundle_source.refuse_reads = true;
  const auto intent = ApplyIntent(*package.bundle);
  const auto predecessor = PredecessorPublication(intent);
  const auto &graph = package.bundle->value().cell_graph;
  ASSERT_FALSE(graph.controllable_components.empty());
  ASSERT_FALSE(graph.control_domains.empty());
  ASSERT_FALSE(graph.control_bindings.empty());
  const auto &component = graph.controllable_components.front();
  const auto &domain = graph.control_domains.front();
  const auto &binding = graph.control_bindings.front();
  ASSERT_EQ(binding.controllable_component_id,
            component.controllable_component_id);
  ASSERT_EQ(binding.control_domain_id, domain.control_domain_id);
  ASSERT_FALSE(binding.legal_control_route_ids.empty());
  const core::ExpectedControlDomainRevisionDTO expected_domain{
      domain.control_domain_id,
      domain.control_domain_revision,
      domain.controller().controller_topology_id,
      domain.controller().topology_revision,
      {core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt},
      {{binding.control_binding_id, binding.control_binding_revision,
        binding.legal_control_route_ids.front()}}};
  const core::ComponentLifecycleIntentDTO child_intent{
      {RequestId("child.original.1"), RequestOwnerEpoch("cclc.original.1"),
       "linked-parent-test", std::nullopt},
      component.controllable_component_id,
      core::ComponentLifecycleIntentKindDTO::kPrepare,
      graph.graph_revision,
      core::ComponentLifecycleGeneration(1U),
      {expected_domain},
      std::nullopt,
      {},
      *intent.expected_admission_generation,
      std::nullopt};
  const ManagerApplyBringUpChildRequestDTO child{
      child_intent,
      intent.expected_active_projection_revision,
      ContentDigest(std::string(kDigestTwo)),
      512U,
      binding.provider_runtime_unit_id,
      RuntimeEpoch("host.original.1"),
      RuntimeEpoch("manager.original.1")};
  const ManagerApplyPrivateMaterializationDTO materialization{
      child.candidate_projection_revision.scene_revision.content_hash,
      intent.apply_id,
      intent.apply_generation,
      child.candidate_projection_revision,
      child.candidate_retained_bundle_digest,
      child.candidate_retained_bundle_bytes};
  const ManagerApplyBringUpPlanDTO plan{
      intent.apply_id,
      intent.apply_generation,
      ComputeManagerApplyIntentDigest(intent),
      {component.controllable_component_id},
      {child},
      {core::NamedApplyPublicationTransactionDTO{intent.apply_id,
                                                 intent.apply_generation},
       child.candidate_projection_revision, intent.candidate_retained_bundle_id,
       intent.candidate_graph_revision, predecessor.publication_generation,
       ComputeManagerApplyPrivateMaterializationDigest(materialization)},
      predecessor.durable_public_pointer_content_digest};
  ASSERT_TRUE(plan.valid());
  const auto database = state_root.path() / "linked-parent.sqlite3";
  SqliteManagerApplyJournal journal(database, intent.cell_id);
  SqliteManagerApplyJournal independent_reader(database, intent.cell_id);
  const auto parent = journal.EnsureBringUpIntent(intent, plan);
  const ManagerApplyBringUpChildProofDTO rejected_child{
      intent.apply_id,
      intent.apply_generation,
      parent.plan.plan_digest,
      child_intent.controllable_component_id,
      ManagerApplyBringUpChildPhase::kAccepted,
      {core::RequestReplayStatusDTO::kAcceptedNew, std::nullopt, std::nullopt,
       core::ErrorDTO{
           "not_qualified", "original child refused without admission", {{"effect", "not_started"}}},
       std::nullopt}};
  const auto proof = journal.EnsureBringUpChildProof(rejected_child);
  ScriptedPublicationStore publication_store(predecessor, &independent_reader);
  FixedActiveRuntimePlanSource active_plan_source(
      predecessor, BuildVerifiedRuntimeConvergencePlan(
                       *package.bundle, *package.artifact_source, "linux",
                       {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)}));
  ScriptedRuntimePorts runtime_ports(independent_reader);
  MaintenanceSceneOwner scene_owner(
      predecessor.request.candidate_projection_revision);
  AggregateFencePort kernel_fence(independent_reader, predecessor);
  ScriptedRuntimeCompositionPort runtime_composition;
  ScriptedCellAdmissionPort cell_admission;
  const core::CellAdmissionClaimIntentDTO claim_intent{
      core::CellAdmissionClaimKindDTO::kManagerApply,
      core::CellAdmissionOwnerRoleDTO::kManager,
      intent.request_context.request_owner_epoch,
      intent.apply_id.value(),
      intent.request_context.request_id,
      parent.plan.plan_digest,
      *intent.expected_admission_generation,
      {},
      std::nullopt,
      std::nullopt};
  ASSERT_TRUE(cell_admission.AcquireCellAdmissionClaim(claim_intent).ok());
  const auto held_admission = cell_admission.GetAuthoringAdmission();
  ASSERT_FALSE(held_admission.authoring_allowed);
  AlwaysIdleMaintenanceGate maintenance_gate;
  ManagerPlannedMaintenanceCoordinator planned_maintenance(active_plan_source,
                                                           maintenance_gate);
  int progress_callbacks = 0;
  int capture_calls = 0;
  int mint_calls = 0;
  ManagerNamedApplyCoordinator reopened(
      journal, active_plan_source, bundle_source, *package.artifact_source,
      publication_store, scene_owner, nullptr, kernel_fence, cell_admission,
      runtime_composition, runtime_ports, runtime_ports, planned_maintenance,
      RuntimeEpoch("manager.replaced.1"), "linux", {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)},
      [] { return 2'000U; },
      [&] {
        ++mint_calls;
        return ApplyId("must-not-be-minted");
      },
      [&](const auto &, const auto &) {
        ++capture_calls;
        return std::vector<core::AddObjectRequestDTO>{};
      },
      {}, [&](const auto &) { ++progress_callbacks; });
  const ManagerNamedApplyRequestDTO request{
      intent.request_context,
      package.bundle->value().deployment_artifact_id,
      package.bundle->value().deployment_revision,
      intent.expected_active_projection_revision.retained_bundle_id,
      intent.expected_active_projection_revision.graph_revision,
      intent.candidate_retained_bundle_id,
      intent.prepared_activation,
      intent.expected_admission_generation,
      std::nullopt};
  for (int observation = 0; observation < 2; ++observation) {
    const auto pending = reopened.RecoverIncompleteApply();
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->state, ManagerNamedApplyResultState::kPending);
    EXPECT_TRUE(pending->request_replayed);
    EXPECT_FALSE(pending->terminal_outcome);
    const auto replay = reopened.AcceptOrReplay(request);
    EXPECT_EQ(replay.state, ManagerNamedApplyResultState::kPending);
    EXPECT_TRUE(replay.request_replayed);
    EXPECT_FALSE(replay.terminal_outcome);
  }
  EXPECT_EQ(journal.VerifyIntegrity().terminal_outcome_count, 0);
  EXPECT_EQ(journal.ReadBringUpPlan(intent.apply_id, intent.apply_generation),
            parent.plan);
  const auto retained_proof =
      journal.ReadBringUpChildProof(intent.apply_id, intent.apply_generation,
                                    child_intent.controllable_component_id,
                                    ManagerApplyBringUpChildPhase::kAccepted);
  ASSERT_TRUE(retained_proof.has_value());
  EXPECT_EQ(retained_proof->proof_digest, proof.record.proof_digest);
  const ManagerApplyTerminalOutcomeDTO terminal{
      intent.apply_id,
      intent.apply_generation,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ManagerApplyTerminalOutcomeState::kFailed,
      ManagerApplyFailureEvidenceDTO{
          parent.intent.intent_digest, ManagerApplyPhase::kAccepted,
          "not_qualified", "actual original parent refusal",
          ManagerApplyEffectDisposition::kKnown}};
  static_cast<void>(journal.EnsureTerminalOutcome(terminal));
  const auto terminal_replay = reopened.AcceptOrReplay(request);
  EXPECT_EQ(terminal_replay.state, ManagerNamedApplyResultState::kFailed);
  EXPECT_EQ(terminal_replay.terminal_outcome, terminal);
  EXPECT_FALSE(reopened.RecoverIncompleteApply());
  EXPECT_EQ(cell_admission.GetAuthoringAdmission(), held_admission);
  EXPECT_FALSE(cell_admission.last_release);
  EXPECT_EQ(bundle_source.read_calls, 0);
  EXPECT_EQ(active_plan_source.read_calls, 0);
  EXPECT_EQ(runtime_ports.observe_calls, 0);
  EXPECT_EQ(runtime_ports.ensure_calls, 0);
  EXPECT_EQ(runtime_ports.stop_calls, 0);
  EXPECT_EQ(scene_owner.begin_calls, 0);
  EXPECT_EQ(scene_owner.install_scene_calls, 0);
  EXPECT_EQ(runtime_composition.calls, 0);
  EXPECT_EQ(publication_store.publish_calls, 0);
  EXPECT_EQ(kernel_fence.observe_admission_calls, 0);
  EXPECT_EQ(kernel_fence.acquire_calls, 0);
  EXPECT_EQ(kernel_fence.release_calls, 0);
  EXPECT_EQ(progress_callbacks, 0);
  EXPECT_EQ(capture_calls, 0);
  EXPECT_EQ(mint_calls, 0);
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

// The fake original CCLC reports its own retained values; Manager must record
// these values, never manufacture acceptance or Holding from the plan.
// Real Scene owner protocol with native participant test clients. Production
// host registration is separately qualified; no Scene row/status is faked.
class AdoptionProjection final : public IPlannerSceneProjection {
public:
  explicit AdoptionProjection(ProjectionNativeRuntimeBindingDTO binding)
      : health_{std::move(binding), 1U, ProjectionHealthState::kUninitialized,
                std::nullopt, 1U, ContentDigest(std::string(kDigestOne)), std::nullopt} {}
  void Qualify(const ProjectionRevisionDTO &target) {
    health_.state = ProjectionHealthState::kHealthy;
    health_.active_projection_revision = target;
    ++health_.source_sequence;
  }
  ProjectionHealthDTO ProjectionHealth() const override { return health_; }
  ProjectionCommitResultDTO ApplyTransaction(const SceneTransactionDTO &request) override {
    ++effect_calls;
    Qualify(request.proposed_projection_revision);
    ++*health_.native_projection_generation;
    return {request.transaction_sequence, request.proposed_projection_revision,
            ProjectionCommitResultStateDTO::kCommitted, request.proposed_projection_revision,
            health_.native_projection_generation, false, health_, std::nullopt};
  }
  ProjectionCommitResultDTO RebuildProjection(const NativeProjectionRebuildDTO &request) override {
    if (!request.valid() || request.expected_native_runtime_binding != health_.native_runtime_binding)
      throw std::runtime_error("foreign test native rebuild");
    ++effect_calls;
    if (std::exchange(accept_next_rebuild, false)) {
      pending_target = request.target_projection_revision;
      health_.state = ProjectionHealthState::kRebuilding;
      health_.active_projection_revision = std::nullopt;
      ++health_.source_sequence;
      return {std::nullopt, request.target_projection_revision, ProjectionCommitResultStateDTO::kAccepted,
              std::nullopt, health_.native_projection_generation, false, health_, std::nullopt};
    }
    Qualify(request.target_projection_revision);
    ++*health_.native_projection_generation;
    return {std::nullopt, request.target_projection_revision, ProjectionCommitResultStateDTO::kCommitted,
            request.target_projection_revision, health_.native_projection_generation, false,
            health_, std::nullopt};
  }
  NativeProjectionBootstrapRebuildResultDTO RebuildBootstrapProjection(
      const NativeProjectionBootstrapRebuildDTO &) override { throw std::logic_error("native bootstrap unused"); }
  NativeProjectionBootstrapStatusDTO BootstrapProjectionStatus(
      const NativeProjectionBootstrapStatusQueryDTO &) const override { throw std::logic_error("native bootstrap unused"); }
  Result<CollisionWorldLeaseInventory> RetainProjection(const ProjectionRevisionDTO &, ProjectionLeaseId) override {
    return Result<CollisionWorldLeaseInventory>::Err({"unsupported_capability", "inventory unused", {}});
  }
  Result<ProjectionLeaseBindingDTO> RetainProjectionBinding(
      const ProjectionRevisionDTO &revision, ProjectionLeaseId lease) override {
    if (!health_.healthy_for_target(revision) || lease.empty() || !leases_.insert(lease).second)
      return Result<ProjectionLeaseBindingDTO>::Err({"live_scene_snapshot_unavailable", "foreign lease", {}});
    return Result<ProjectionLeaseBindingDTO>::Ok({revision, std::move(lease)});
  }
  void ReleaseProjection(const ProjectionLeaseId &lease) override { leases_.erase(lease); }
  Result<SceneFrameResolutionReceiptDTO> ResolveSceneFrame(const SceneFrameResolutionQueryDTO &query) const override {
    if (!query.valid() || !leases_.contains(query.projection_binding.projection_lease_id))
      return Result<SceneFrameResolutionReceiptDTO>::Err({"live_scene_snapshot_unavailable", "missing lease", {}});
    const auto *carrier = std::get_if<TargetCarrierSceneFrameRefDTO>(&query.selector);
    if (!carrier) return Result<SceneFrameResolutionReceiptDTO>::Err({"missing_reference", "carrier required", {}});
    const SceneFrameResolutionReceiptDTO receipt{
        ResolvedComponentModelFrameDTO{carrier->controllable_component_id, ModelInstanceId("model.1"),
                                      FrameRoleId("asset_tool_frame"), "compiled_asset_tool_frame",
                                      ModelRevision("model-revision-1")},
        Pose3DTO{}, {ComponentAttachmentRevision("component-attachment-revision-1")},
        query.projection_binding, std::nullopt, ContentDigest(std::string(kDigestTwo))};
    return Result<SceneFrameResolutionReceiptDTO>::Ok(receipt);
  }
  Result<CollisionPolicySnapshot> ProjectEffectiveCollisionPolicy(
      const LogicalSceneSnapshotDTO &snapshot, std::uint64_t revision) const override {
    auto policy = snapshot.configured_collision_policy;
    policy.revision.revision_id = revision;
    policy.revision.content_hash = logical_scene_identity::ComputeCollisionPolicyContentDigest(policy).value();
    return Result<CollisionPolicySnapshot>::Ok(std::move(policy));
  }
  void CompletePendingRebuild() {
    if (!pending_target) throw std::logic_error("no native job to complete");
    Qualify(*pending_target);
    pending_target.reset();
  }
  bool accept_next_rebuild{};
  std::optional<ProjectionRevisionDTO> pending_target{};
  std::size_t effect_calls{};
private:
  ProjectionHealthDTO health_;
  std::set<ProjectionLeaseId> leases_;
};

class AdoptionProjectionResolver final : public IPlannerSceneProjectionResolver {
public:
  Result<std::shared_ptr<IPlannerSceneProjection>> ResolveProjection(
      const ProjectionNativeRuntimeBindingDTO &binding) const override {
    for (const auto &client : clients) {
      if (client->ProjectionHealth().native_runtime_binding == binding)
        return Result<std::shared_ptr<IPlannerSceneProjection>>::Ok(client);
    }
    return Result<std::shared_ptr<IPlannerSceneProjection>>::Err({"missing_reference", "foreign projection", {}});
  }
  std::vector<std::shared_ptr<AdoptionProjection>> clients;
};

std::vector<ProjectionNativeRuntimeBindingDTO> AdoptionBindings(
    const deployment::RetainedBundle &bundle, const RuntimeEpoch &epoch) {
  std::vector<ProjectionNativeRuntimeBindingDTO> result;
  for (const auto &provider : bundle.value().capability_provider_bindings) {
    if (!core::RequiresNativeSceneProjection(provider.capability_id, provider.capability_definition_revision)) continue;
    std::set<RuntimeUnitId> units;
    for (const auto &endpoint : provider.endpoint_bindings) {
      if (endpoint.source_kind == CapabilityEndpointSourceKind::kRuntimeUnit) {
        const auto *unit = std::get_if<RuntimeUnitId>(&endpoint.source_id);
        if (!unit) throw std::runtime_error("malformed native fixture endpoint");
        units.insert(*unit);
      }
    }
    if (units.size() != 1U) throw std::runtime_error("fixture native provider lacks exact unit");
    result.push_back({provider.capability_provider_binding_id, provider.capability_provider_binding_revision,
                      *units.begin(), epoch});
  }
  std::sort(result.begin(), result.end(), CanonicalProjectionNativeBindingLess);
  return result;
}

class OriginalSceneAdapter final : public IManagerSceneProjectionMaintenancePort {
public:
  OriginalSceneAdapter(scene::LogicalSceneOwner &owner, AggregateFencePort &fence)
      : owner_(owner), fence_(fence) {}
  ProjectionRevisionDTO CurrentProjectionRevision() const override { return owner_.CurrentProjectionRevision(); }
  ProjectionAggregateHealthDTO ActiveProjectionHealth() const override { return owner_.ActiveProjectionHealth(); }
  ProjectionMaintenanceInitializationStatusDTO BeginMaintenanceInitialization(
      const ProjectionMaintenanceInitializationRequestDTO &) override {
    ++ordinary_begin_calls; throw std::logic_error("linked path called ordinary Scene Begin");
  }
  ProjectionMaintenanceInitializationStatusDTO InstallInitialScene(
      const ProjectionMaintenanceInitializationRequestDTO &) override {
    ++ordinary_install_calls; throw std::logic_error("linked path called ordinary Scene Install");
  }
  ProjectionMaintenanceInitializationStatusDTO CompleteMaintenanceInitialization(
      const ProjectionMaintenanceCompletionDTO &) override {
    ++ordinary_complete_calls; throw std::logic_error("linked path called ordinary Scene Complete");
  }
  std::optional<ProjectionMaintenanceInitializationStatusDTO> ReadMaintenanceInitialization(
      const ProjectionMaintenanceInitializationQueryDTO &query) const override {
    return owner_.ReadMaintenanceInitialization(query);
  }
  std::vector<ProjectionHealthDTO> StageOriginalManagerProductAdoption(
      const QualifiedSceneProductAdoptionRequestDTO &request,
      const ApplyFenceAcquireAttemptQueryDTO &attempt_query) override {
    ++stage_calls;
    if (before_stage) before_stage(request);
    return owner_.StageOriginalManagerProductAdoption(request, fence_, attempt_query);
  }
  std::vector<ProjectionHealthDTO> ReadOriginalManagerProductAdoptionProjectionHealth(
      const QualifiedSceneProductAdoptionRequestDTO &request,
      const ApplyFenceAcquireAttemptQueryDTO &attempt_query) override {
    ++stage_read_calls;
    return owner_.ReadOriginalManagerProductAdoptionProjectionHealth(request, fence_, attempt_query);
  }
  ProjectionMaintenanceInitializationStatusDTO BeginOriginalManagerProductAdoption(
      const QualifiedSceneProductAdoptionRequestDTO &request) override {
    ++begin_calls;
    if (before_begin) before_begin(request);
    auto input = request;
    if (alter_begin) alter_begin(input);
    const auto result = owner_.BeginOriginalManagerProductAdoption(input, fence_);
    if (after_begin) after_begin();
    return result;
  }
  ProjectionMaintenanceInitializationStatusDTO CompleteOriginalManagerProductAdoption(
      const QualifiedSceneProductAdoptionRequestDTO &request,
      const ActiveBundlePublicationReceiptDTO &receipt) override {
    ++complete_calls;
    const auto result = owner_.CompleteOriginalManagerProductAdoption(request, receipt, fence_);
    if (after_complete) after_complete();
    return result;
  }
  std::vector<ProjectionHealthDTO> StageOriginalManagerConfigurationAdoption(
      const core::QualifiedSceneConfigurationAdoptionRequestDTO &request,
      const core::ApplyFenceAcquireAttemptQueryDTO &attempt) override {
    ++stage_calls;
    if (before_configuration_stage) before_configuration_stage(request);
    auto result = owner_.StageOriginalManagerConfigurationAdoption(request, fence_, attempt);
    if (after_configuration_stage) after_configuration_stage();
    return result;
  }
  std::vector<ProjectionHealthDTO> ReadOriginalManagerConfigurationAdoptionProjectionHealth(
      const core::QualifiedSceneConfigurationAdoptionRequestDTO &request,
      const core::ApplyFenceAcquireAttemptQueryDTO &attempt) override {
    ++stage_read_calls;
    if (configuration_event) configuration_event("Read");
    return owner_.ReadOriginalManagerConfigurationAdoptionProjectionHealth(request, fence_, attempt);
  }
  ProjectionMaintenanceInitializationStatusDTO BeginOriginalManagerConfigurationAdoption(
      const core::QualifiedSceneConfigurationAdoptionRequestDTO &request) override {
    ++begin_calls;
    if (configuration_event) configuration_event("Begin");
    return owner_.BeginOriginalManagerConfigurationAdoption(request, fence_);
  }
  ProjectionMaintenanceInitializationStatusDTO CompleteOriginalManagerConfigurationAdoption(
      const core::QualifiedSceneConfigurationAdoptionRequestDTO &request,
      const core::ActiveBundlePublicationReceiptDTO &receipt) override {
    ++complete_calls;
    if (configuration_event) configuration_event("Complete");
    return owner_.CompleteOriginalManagerConfigurationAdoption(request, receipt, fence_);
  }
  std::function<void(const core::QualifiedSceneConfigurationAdoptionRequestDTO &)> before_configuration_stage;
  std::function<void()> after_configuration_stage;
  std::function<void(const std::string &)> configuration_event;
  int ordinary_begin_calls{}, ordinary_install_calls{}, ordinary_complete_calls{},
      stage_calls{}, stage_read_calls{}, begin_calls{}, complete_calls{};
  std::function<void(const QualifiedSceneProductAdoptionRequestDTO &)> before_stage, before_begin;
  std::function<void(QualifiedSceneProductAdoptionRequestDTO &)> alter_begin;
  std::function<void()> after_begin, after_complete;
private:
  scene::LogicalSceneOwner &owner_;
  AggregateFencePort &fence_;
};

ActiveBundlePublicationReceiptDTO EstablishActualScenePredecessor(
    scene::LogicalSceneOwner &owner, ScriptedPublicationStore &publication,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const deployment::RetainedBundle &predecessor,
    const std::vector<ProjectionNativeRuntimeBindingDTO> &bindings) {
  const auto accepted = owner.BeginProjectionBootstrap(
      {BootstrapId("actual-scene-bootstrap"), BootstrapGeneration(1U), admission.cell_id(),
       admission.retained_bundle().value(), {}});
  static_cast<void>(owner.RebuildProjectionBootstrap(
      {accepted.bootstrap_id, accepted.bootstrap_generation, accepted.canonical_empty_target}));
  const ActiveBundlePublicationReceiptDTO bootstrap{
      {BootstrapPublicationTransactionDTO{accepted.bootstrap_id, accepted.bootstrap_generation},
       accepted.canonical_empty_target, accepted.canonical_empty_target.retained_bundle_id,
       accepted.canonical_empty_target.graph_revision, std::nullopt, ContentDigest(std::string(kDigestOne))},
      ActiveBundlePublicationGeneration(1U), ContentDigest(std::string(kDigestTwo))};
  publication.SetCurrent(bootstrap);
  const auto finalized = owner.FinalizeProjectionBootstrap(
      {accepted.bootstrap_id, accepted.bootstrap_generation, accepted.canonical_empty_target, bootstrap});
  if (finalized.state != ProjectionBootstrapFinalizeResultState::kFinalized)
    throw std::runtime_error("actual Scene bootstrap did not finalize");
  if (predecessor.retained_bundle_id() == admission.retained_bundle().retained_bundle_id()) return bootstrap;
  const ProjectionMaintenanceInitializationRequestDTO request{
      ApplyId("establish-actual-predecessor"), ApplyGeneration(1U), accepted.canonical_empty_target,
      predecessor.value(), bindings};
  static_cast<void>(owner.BeginMaintenanceInitialization(request));
  const auto installed = owner.InstallInitialScene(request);
  const ActiveBundlePublicationReceiptDTO receipt{
      {NamedApplyPublicationTransactionDTO{request.apply_id, request.apply_generation},
       installed.target_projection_revision, predecessor.retained_bundle_id(),
       predecessor.value().cell_graph.graph_revision, bootstrap.publication_generation,
       ContentDigest(std::string(kDigestOne))},
      ActiveBundlePublicationGeneration(2U), ContentDigest(std::string(kDigestTwo))};
  publication.SetCurrent(receipt);
  const auto completed = owner.CompleteMaintenanceInitialization(
      {request.apply_id, request.apply_generation, installed.target_projection_revision,
       installed.authoritative_snapshot_identity, receipt});
  if (completed.state != ProjectionMaintenanceInitializationState::kCompleted)
    throw std::runtime_error("actual Scene predecessor did not complete");
  return receipt;
}

// Runtime-only regression fixture: establish a real durable Scene over the
// admitted canonical-empty predecessor, then preserve it through M Apply.
struct RuntimeConfigurationSceneFixture final {
  RuntimeConfigurationSceneFixture(const std::filesystem::path &package_root,
      const std::filesystem::path &state_root, const RuntimePackage &package,
      ScriptedPublicationStore &publication, IKernelCellAdmissionPort &cell_admission)
      : empty(MaterializeRosRuntimePackage(package_root, true)),
        admission(deployment::detail::publish_cell_scoped_canonical_empty_admission(
            package_root,
            deployment::cell_scoped_canonical_empty_admission_filename(
                package.bundle->value().cell_graph.cell_id), *empty.bundle)),
        owner(state_root / "actual-configuration-scene.sqlite3", admission,
              publication, *package.bundle_source, resolver, cell_admission),
        predecessor(EstablishActualScenePredecessor(
            owner, publication, admission, *empty.bundle, {})),
        snapshot_bytes(scene::EncodeLogicalSceneSnapshot(*owner.LogicalSnapshot())) {}

  RuntimePackage empty;
  deployment::CanonicalEmptyRetainedBundleAdmission admission;
  AdoptionProjectionResolver resolver;
  scene::LogicalSceneOwner owner;
  ActiveBundlePublicationReceiptDTO predecessor;
  std::string snapshot_bytes;
};

TEST(ManagerNamedApplyCoordinator,
     CapturesOnceProgressesWithoutBundleReloadAndReplaysWithoutEffects) {
  for (const int settlement_scenario : {0, 1, 2, 3}) {
    SCOPED_TRACE(settlement_scenario);
    const bool defer_renewal = settlement_scenario != 0;
    const bool fail_release = settlement_scenario == 2;
    const bool fail_journal_read = settlement_scenario == 3;
    ScopedPackageRoot package_root;
    ScopedPackageRoot state_root;
    auto package = MaterializeRosRuntimePackage(
        package_root.path(), true, deployment::test::kLite6ArmTargetId, {},
        "runtime-only-configuration-candidate");
    RefusingBundleReloadSource bundle_source(*package.bundle_source);
    const auto intent_template = ApplyIntent(*package.bundle);
    const auto initial_predecessor = PredecessorPublication(intent_template);
    SqliteManagerApplyJournal journal(state_root.path() / "named-apply.sqlite3",
                                      package.bundle->value().cell_graph.cell_id);
    SqliteManagerApplyJournal independent_reader(
        state_root.path() / "named-apply.sqlite3",
        package.bundle->value().cell_graph.cell_id);
    ScriptedPublicationStore publication_store(initial_predecessor, &independent_reader);
    ScriptedCellAdmissionPort cell_admission;
    RuntimeConfigurationSceneFixture actual_scene(
        package_root.path(), state_root.path(), package, publication_store, cell_admission);
    const auto &predecessor = actual_scene.predecessor;
    ASSERT_NE(predecessor.request.candidate_retained_bundle_id, package.bundle->retained_bundle_id());
    ASSERT_EQ(actual_scene.empty.bundle->value().cell_graph.cell_id, package.bundle->value().cell_graph.cell_id);
    ASSERT_TRUE(package.bundle->value().cell_graph.controllable_components.empty());
    ASSERT_TRUE(package.bundle->value().cell_graph.control_domains.empty());
    const auto continuity = VerifyNativeExecutionHostContinuity(
        *actual_scene.empty.bundle, *package.bundle, *package.artifact_source);
    ASSERT_TRUE(continuity.ok()) << continuity.error().message;
    FixedActiveRuntimePlanSource active_plan_source(
        predecessor, BuildVerifiedRuntimeConvergencePlan(
            *actual_scene.empty.bundle, *actual_scene.empty.artifact_source, "linux",
            {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)}));
    ScriptedRuntimePorts runtime_ports(independent_reader);
    AggregateFencePort kernel_fence(independent_reader, predecessor);
    OriginalSceneAdapter scene_owner(actual_scene.owner, kernel_fence);
    const auto actual_runtime_plan = BuildVerifiedRuntimeConvergencePlan(
        *package.bundle, *package.artifact_source, "linux",
        {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)});
    ASSERT_EQ(actual_runtime_plan.size(), 1U);
    kernel_fence.original_execution_host = ExecutionHostEpochDTO{
        actual_runtime_plan.front().runtime_unit_id,
        RuntimeEpoch("process-epoch-1"), RequestOwnerEpoch("original-host-owner"),
        ContentDigest(std::string(kDigestOne)), predecessor.publication_generation};
    kernel_fence.allow_prepublication_status = true;
    ScriptedRuntimeCompositionPort runtime_composition;
    ASSERT_TRUE(deployment::verify_retained_bundle_identity(*actual_scene.empty.bundle));
    const auto &installed_graph = actual_scene.empty.bundle->value().cell_graph;
    ASSERT_TRUE(installed_graph.controllable_components.empty());
    ASSERT_TRUE(installed_graph.control_domains.empty());
    ASSERT_TRUE(installed_graph.control_bindings.empty());
    // This fresh scripted owner has launched no component processes. Its exact
    // verified predecessor and host/Manager epochs qualify that known inventory;
    // an uncomposed inspection on any other fixture still refuses.
    const ManagerComponentProcessInventoryRequestDTO installed_scope{
        predecessor.request.candidate_projection_revision,
        predecessor.publication_generation,
        predecessor.durable_public_pointer_content_digest,
        kernel_fence.original_execution_host->runtime_unit_id,
        kernel_fence.original_execution_host->runtime_epoch,
        RuntimeEpoch("manager-runtime.aggregate.1"), true};
    ASSERT_TRUE(installed_scope.valid());
    ASSERT_EQ(installed_scope.predecessor_projection_revision.retained_bundle_id,
              actual_scene.empty.bundle->retained_bundle_id());
    ASSERT_EQ(installed_scope.predecessor_projection_revision.graph_revision,
              installed_graph.graph_revision);
    const std::vector<ManagerComponentProcessOccurrenceDTO> installed_processes{};
    runtime_composition.installed_processes =
        [installed_scope, installed_processes](
            const ManagerComponentProcessInventoryRequestDTO &request) {
          if (!request.valid() || request != installed_scope) {
            return Result<ManagerComponentProcessInventoryResultDTO>::Err(
                {"fixture_installed_inventory_foreign",
                 "installed process inspection does not name the fixture owner", {}});
          }
          return Result<ManagerComponentProcessInventoryResultDTO>::Ok(
              {request, installed_processes, std::nullopt});
        };
    ManagerActiveRuntimeCoordinator active_runtime(
        active_plan_source, runtime_ports, runtime_ports, [] { return 1'000U; });
    AlwaysIdleMaintenanceGate maintenance_gate;
    ManagerPlannedMaintenanceCoordinator planned_maintenance(
        active_plan_source, maintenance_gate);
    int authored_scene_reads = 0;
    bool successor_renewal_available = !defer_renewal;
    std::vector<ManagerApplySettlementAttempt> settlement_attempts;
    std::optional<ManagerApplyIntentDTO> settlement_successor;
    ManagerNamedApplyCoordinator coordinator(
        journal, active_plan_source, bundle_source,
        *package.artifact_source, publication_store, scene_owner, nullptr,
        kernel_fence, cell_admission, runtime_composition, runtime_ports,
        runtime_ports, planned_maintenance,
        RuntimeEpoch("manager-runtime.aggregate.1"), "linux",
        {"posix.1-2017", std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)},
        [] { return 1'000U; },
        [] { return ApplyId("apply.aggregate.1"); },
        [&](const auto &, const auto &) {
          ++authored_scene_reads;
          return std::vector<core::AddObjectRequestDTO>{
              {core::SceneResourceId("invalid"), core::SphereShapeDTO{-0.1},
               core::Pose3DTO{}, core::CellSceneFrameRefDTO{
                   package.bundle->value().cell_graph.cell_id}, {}}};
        }, {}, {},
        [&](const ManagerApplyIntentDTO &intent,
            const ManagerApplySettlementAttempt attempt) {
          settlement_attempts.push_back(attempt);
          if (settlement_successor) {
            EXPECT_EQ(intent, *settlement_successor);
          } else {
            settlement_successor = intent;
          }
          EXPECT_EQ(intent.candidate_retained_bundle_id,
                    package.bundle->retained_bundle_id());
          EXPECT_EQ(intent.candidate_graph_revision,
                    package.bundle->value().cell_graph.graph_revision);
          const auto terminal = independent_reader.ReadTerminalOutcome(
              intent.apply_id, intent.apply_generation);
          EXPECT_TRUE(terminal.has_value());
          if (!terminal) return false;
          EXPECT_TRUE(terminal->terminal_outcome.succeeded());
          EXPECT_TRUE(independent_reader.ReadPublicationReceipt(
              intent.apply_id, intent.apply_generation).has_value());
          EXPECT_EQ(publication_store.publish_calls, 1);
          EXPECT_EQ(cell_admission.release_calls, 0);
          EXPECT_FALSE(cell_admission.GetAuthoringAdmission().authoring_allowed);
          return successor_renewal_available;
        });
    const ManagerNamedApplyRequestDTO request{
        intent_template.request_context,
        package.bundle->value().deployment_artifact_id,
        package.bundle->value().deployment_revision,
        predecessor.request.candidate_retained_bundle_id,
        predecessor.request.candidate_graph_revision,
        package.bundle->retained_bundle_id(),
        intent_template.prepared_activation,
        cell_admission.GetAuthoringAdmission().admission_generation,
        std::nullopt,
    };

    int prepare_acknowledgements = 0;
    const auto accepted = coordinator.AcceptOrReplay(request, {},
        [&](const auto &original, const auto &candidate, const auto &acknowledgement) {
          ++prepare_acknowledgements;
          EXPECT_EQ(original.request_context, request.request_context);
          EXPECT_EQ(candidate.canonical_bytes(), package.bundle->canonical_bytes());
          EXPECT_TRUE(acknowledgement.valid());
          EXPECT_EQ(acknowledgement.request.apply_id, original.apply_id);
          EXPECT_EQ(acknowledgement.request.apply_generation, original.apply_generation);
          EXPECT_EQ(acknowledgement.request.phase, ManagerSelectedRuntimeCompositionPhase::kPrepare);
          EXPECT_EQ(acknowledgement.state, ManagerSelectedRuntimeCompositionState::kInstalled);
          EXPECT_EQ(scene_owner.ordinary_install_calls, 0);
          EXPECT_EQ(publication_store.publish_calls, 0);
          EXPECT_EQ(runtime_composition.calls, 1);
        });
    ASSERT_EQ(accepted.state, ManagerNamedApplyResultState::kPending);
    EXPECT_FALSE(accepted.request_replayed);
    ASSERT_TRUE(journal.ReadRequest(request.request_context.request_owner_epoch,
                                   request.request_context.request_id));
    const auto pending_replay = coordinator.AcceptOrReplay(request, {},
        [](const auto &, const auto &, const auto &) {
          ADD_FAILURE() << "pending replay replaced the original capture";
        });
    EXPECT_EQ(pending_replay.state, ManagerNamedApplyResultState::kPending);
    EXPECT_TRUE(pending_replay.request_replayed);
    EXPECT_EQ(authored_scene_reads, 0);
    EXPECT_EQ(prepare_acknowledgements, 0);
    EXPECT_EQ(runtime_ports.ensure_calls, 0);
    EXPECT_EQ(scene_owner.begin_calls, 0);

    // Continue must use the accepted bundles, not look them up again. Retained
    // executable products remain available for their separate verification and
    // process initialization.
    const auto accepted_bundle_reads = bundle_source.read_calls;
    bundle_source.refuse_reads = true;
    EXPECT_FALSE(coordinator.PendingSettlementForHeartbeat().has_value());
    auto completed = coordinator.RecoverIncompleteApply();
    ASSERT_TRUE(completed.has_value());
    const NamedApplyPublicationTransactionDTO completed_reference{
        completed->apply_id, completed->apply_generation};
    if (defer_renewal) {
      EXPECT_EQ(completed->state, ManagerNamedApplyResultState::kPending);
      EXPECT_TRUE(completed->settlement_pending);
      EXPECT_FALSE(completed->terminal_outcome.has_value());
      EXPECT_EQ(settlement_attempts,
                (std::vector<ManagerApplySettlementAttempt>{
                    ManagerApplySettlementAttempt::kInitial}));
      ASSERT_TRUE(settlement_successor.has_value());
      const auto heartbeat_reference = coordinator.PendingSettlementForHeartbeat();
      ASSERT_TRUE(heartbeat_reference.has_value());
      EXPECT_EQ(*heartbeat_reference, completed_reference);
      const auto durable_success = independent_reader.ReadTerminalOutcome(
          completed->apply_id, completed->apply_generation);
      ASSERT_TRUE(durable_success.has_value());
      ASSERT_TRUE(durable_success->terminal_outcome.succeeded());
      EXPECT_EQ(cell_admission.release_calls, 0);
      EXPECT_FALSE(cell_admission.GetAuthoringAdmission().authoring_allowed);

      // Apply progression and request replay do not invent another heartbeat.
      const auto progression = coordinator.RecoverIncompleteApply();
      ASSERT_TRUE(progression.has_value());
      EXPECT_TRUE(progression->settlement_pending);
      const auto waiting_replay = coordinator.AcceptOrReplay(request);
      EXPECT_TRUE(waiting_replay.settlement_pending);
      EXPECT_EQ(settlement_attempts.size(), 1U);
      EXPECT_EQ(runtime_ports.ensure_calls, 1);
      EXPECT_EQ(runtime_composition.calls, 3);
      EXPECT_EQ(publication_store.publish_calls, 1);
      auto another_request = request;
      another_request.request_context.request_id = RequestId("request.while-settlement-held");
      EXPECT_THROW(static_cast<void>(coordinator.AcceptOrReplay(another_request)),
                   ManagerNamedApplyBusyError);
      const NamedApplyPublicationTransactionDTO foreign_reference{
          ApplyId("apply.foreign-heartbeat"), completed->apply_generation};
      EXPECT_FALSE(coordinator.RetryPendingSettlementOnHeartbeat(
          foreign_reference).has_value());
      auto stale_generation = *heartbeat_reference;
      stale_generation.apply_generation = ApplyGeneration(
          stale_generation.apply_generation.value() + 1U);
      EXPECT_FALSE(coordinator.RetryPendingSettlementOnHeartbeat(
          stale_generation).has_value());
      EXPECT_EQ(settlement_attempts.size(), 1U);
      EXPECT_EQ(cell_admission.release_calls, 0);
      EXPECT_EQ(coordinator.PendingSettlementForHeartbeat(), heartbeat_reference);
      EXPECT_EQ(runtime_composition.calls, 3);
      EXPECT_EQ(publication_store.publish_calls, 1);

      // A failed ordinary-heartbeat proof keeps the exact successful operation
      // and claim pending; the next ordinary heartbeat can reuse current proof.
      const auto still_waiting = coordinator.RetryPendingSettlementOnHeartbeat(
          *heartbeat_reference);
      ASSERT_TRUE(still_waiting.has_value());
      EXPECT_TRUE(still_waiting->settlement_pending);
      EXPECT_EQ(cell_admission.release_calls, 0);
      EXPECT_FALSE(cell_admission.GetAuthoringAdmission().authoring_allowed);
      EXPECT_EQ(independent_reader.ReadTerminalOutcome(
                    completed->apply_id, completed->apply_generation),
                durable_success);
      EXPECT_EQ(coordinator.PendingSettlementForHeartbeat(), heartbeat_reference);
      successor_renewal_available = true;
      if (fail_journal_read) {
        const auto attempts_before_read_error = settlement_attempts;
        journal.Close();
        const auto metadata = coordinator.RecoverIncompleteApply();
        ASSERT_TRUE(metadata.has_value());
        EXPECT_EQ(metadata->state, ManagerNamedApplyResultState::kPending);
        EXPECT_EQ(metadata->apply_id, heartbeat_reference->apply_id);
        EXPECT_EQ(metadata->apply_generation, heartbeat_reference->apply_generation);
        EXPECT_TRUE(metadata->settlement_pending);
        EXPECT_FALSE(metadata->terminal_outcome.has_value());
        EXPECT_EQ(coordinator.PendingSettlementForHeartbeat(), heartbeat_reference);
        EXPECT_EQ(settlement_attempts, attempts_before_read_error);
        EXPECT_EQ(cell_admission.release_calls, 0);
        EXPECT_THROW(static_cast<void>(coordinator.RetryPendingSettlementOnHeartbeat(
                         *heartbeat_reference)), std::runtime_error);
        EXPECT_FALSE(coordinator.PendingSettlementForHeartbeat().has_value());
        EXPECT_EQ(settlement_attempts, attempts_before_read_error);
        EXPECT_EQ(cell_admission.release_calls, 0);
        EXPECT_FALSE(cell_admission.GetAuthoringAdmission().authoring_allowed);
        EXPECT_EQ(independent_reader.ReadTerminalOutcome(
                      completed->apply_id, completed->apply_generation),
                  durable_success);
        EXPECT_FALSE(coordinator.RetryPendingSettlementOnHeartbeat(
            *heartbeat_reference).has_value());
        EXPECT_EQ(settlement_attempts, attempts_before_read_error);
        EXPECT_EQ(publication_store.publish_calls, 1);
        EXPECT_EQ(runtime_composition.calls, 3);
        continue;
      }
      if (fail_release) {
        cell_admission.refuse_release = true;
        EXPECT_THROW(static_cast<void>(coordinator.RetryPendingSettlementOnHeartbeat(
                         *heartbeat_reference)), std::runtime_error);
        EXPECT_FALSE(coordinator.PendingSettlementForHeartbeat().has_value());
        EXPECT_EQ(cell_admission.release_calls, 1);
        EXPECT_FALSE(cell_admission.last_release.has_value());
        EXPECT_FALSE(cell_admission.GetAuthoringAdmission().authoring_allowed);
        EXPECT_EQ(independent_reader.ReadTerminalOutcome(
                      completed->apply_id, completed->apply_generation),
                  durable_success);
        const auto failed_attempts = settlement_attempts;
        cell_admission.refuse_release = false;
        EXPECT_FALSE(coordinator.RetryPendingSettlementOnHeartbeat(
            *heartbeat_reference).has_value());
        EXPECT_EQ(settlement_attempts, failed_attempts);
        EXPECT_EQ(cell_admission.release_calls, 1);
        EXPECT_EQ(runtime_composition.calls, 3);
        EXPECT_EQ(publication_store.publish_calls, 1);
        continue;
      }
      completed = coordinator.RetryPendingSettlementOnHeartbeat(*heartbeat_reference);
      ASSERT_TRUE(completed.has_value());
      EXPECT_EQ(settlement_attempts,
                (std::vector<ManagerApplySettlementAttempt>{
                    ManagerApplySettlementAttempt::kInitial,
                    ManagerApplySettlementAttempt::kOrdinaryHeartbeat,
                    ManagerApplySettlementAttempt::kOrdinaryHeartbeat}));
      EXPECT_EQ(completed->terminal_outcome,
                std::optional(durable_success->terminal_outcome));
    } else {
      EXPECT_EQ(settlement_attempts,
                (std::vector<ManagerApplySettlementAttempt>{
                    ManagerApplySettlementAttempt::kInitial}));
    }
    EXPECT_FALSE(completed->settlement_pending);
    EXPECT_EQ(cell_admission.release_calls, 1);
    EXPECT_TRUE(cell_admission.GetAuthoringAdmission().authoring_allowed);
    const auto settled_attempts = settlement_attempts;
    EXPECT_FALSE(coordinator.PendingSettlementForHeartbeat().has_value());
    EXPECT_FALSE(coordinator.RetryPendingSettlementOnHeartbeat(
        completed_reference).has_value());
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
    EXPECT_EQ(prepare_acknowledgements, 1);
    EXPECT_EQ(runtime_ports.ensure_calls, 1);
    EXPECT_EQ(scene_owner.begin_calls, 1);
    EXPECT_EQ(scene_owner.ordinary_install_calls, 0);
    EXPECT_EQ(scene_owner.complete_calls, 1);
    EXPECT_EQ(scene_owner.stage_calls, 1);
    EXPECT_EQ(scene_owner.ordinary_begin_calls, 0);
    EXPECT_EQ(authored_scene_reads, 0);
    EXPECT_EQ(scene::EncodeLogicalSceneSnapshot(*actual_scene.owner.LogicalSnapshot()),
              actual_scene.snapshot_bytes);
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

    const auto replayed = coordinator.AcceptOrReplay(request, {},
        [&](const auto &, const auto &, const auto &) {
          ADD_FAILURE() << "replay reconstructed the original Prepare continuation";
        });
    EXPECT_EQ(replayed.state, ManagerNamedApplyResultState::kSucceeded);
    EXPECT_TRUE(replayed.request_replayed);
    EXPECT_EQ(replayed.terminal_outcome, completed->terminal_outcome);
    EXPECT_EQ(runtime_ports.ensure_calls, 1);
    EXPECT_EQ(scene_owner.begin_calls, 1);
    EXPECT_EQ(scene_owner.ordinary_install_calls, 0);
    EXPECT_EQ(scene_owner.complete_calls, 1);
    EXPECT_EQ(scene_owner.stage_calls, 1);
    EXPECT_EQ(scene_owner.ordinary_begin_calls, 0);
    EXPECT_EQ(authored_scene_reads, 0);
    EXPECT_EQ(scene::EncodeLogicalSceneSnapshot(*actual_scene.owner.LogicalSnapshot()),
              actual_scene.snapshot_bytes);
    EXPECT_EQ(runtime_composition.calls, 3);  // Replay repeats no effect.
    EXPECT_EQ(publication_store.publish_calls, 1);
    EXPECT_EQ(kernel_fence.acquire_calls, 1);
    EXPECT_EQ(kernel_fence.release_calls, 1);
    EXPECT_FALSE(coordinator.RecoverIncompleteApply().has_value());
    EXPECT_EQ(prepare_acknowledgements, 1);
    EXPECT_EQ(settlement_attempts, settled_attempts);
    EXPECT_EQ(cell_admission.release_calls, 1);
    EXPECT_FALSE(coordinator.PendingSettlementForHeartbeat().has_value());
    EXPECT_FALSE(coordinator.RetryPendingSettlementOnHeartbeat(
        completed_reference).has_value());
  }
}

TEST(ManagerNamedApplyCoordinator,
     PreparationAndFinalizationFailuresRetainDistinctOriginalTerminalEvidence) {
  for (const int failing_call : {1, 2, 3}) {
    SCOPED_TRACE(failing_call);
    ScopedPackageRoot package_root;
    ScopedPackageRoot state_root;
    auto package = MaterializeRosRuntimePackage(
        package_root.path(), true, deployment::test::kLite6ArmTargetId, {},
        "runtime-only-configuration-candidate");
    const auto intent_template = ApplyIntent(*package.bundle);
    const auto initial_predecessor = PredecessorPublication(intent_template);
    const auto database = state_root.path() / "failed-composition.sqlite3";
    const auto cell_id = package.bundle->value().cell_graph.cell_id;
    SqliteManagerApplyJournal journal(database, cell_id);
    SqliteManagerApplyJournal independent_reader(database, cell_id);
    ScriptedPublicationStore publication_store(initial_predecessor, &independent_reader);
    core::FakeClock kernel_clock;
    kernel_clock.set(1'000U);
    core::ManualScheduler kernel_scheduler;
    core::SqliteOperationJournal operation_journal(
        state_root.path() / "kernel-operation.sqlite3", cell_id);
    const auto kernel_empty = MaterializeRosRuntimePackage(package_root.path(), true);
    core::ResourceAddressedExecutionKernelConfig kernel_config{
        core::RequestOwnerEpoch("kernel.failed-composition.1"), cell_id, 64U};
    kernel_config.empty_runtime_retained_bundle_id = kernel_empty.bundle->retained_bundle_id();
    kernel_config.empty_runtime_graph_revision = kernel_empty.bundle->value().cell_graph.graph_revision;
    core::ManagerLivenessRequiredSetDTO liveness{
        cell_id, kernel_empty.bundle->retained_bundle_id(),
        core::ComputeRequiredRuntimeSetDigest({}),
        core::RuntimeUnitId("manager.failed-composition"),
        core::RuntimeEpoch("manager-runtime.failed-composition.1"),
        intent_template.request_context.request_owner_epoch, {}};
    kernel_config.manager_liveness_required_set = liveness;
    int settlement_calls = 0;
    kernel_config.cell_admission_settlement_validator =
        [database, cell_id, &settlement_calls](const core::CellAdmissionClaimDTO &claim,
                                             const core::CellAdmissionClaimReleaseDTO &release) {
          ++settlement_calls;
          return ValidateManagerApplyCellAdmissionSettlementReadOnly(
              database, cell_id, claim, release);
        };
    core::ResourceAddressedExecutionKernel cell_admission(
        {}, kernel_clock, kernel_scheduler, operation_journal, std::move(kernel_config));
    const core::ProgramCycleStatusDTO idle{
        core::RequestOwnerEpoch("program.failed-composition"), 1U,
        core::ProgramCycleStateDTO::kIdleAfterReset,
        std::nullopt, std::nullopt, std::nullopt};
    const core::CellAdmissionReadinessSnapshotDTO ready{
        true, idle, true, 0U, 0U, true, true, 0U, 0U, 0U};
    ASSERT_TRUE(cell_admission.UpdateCellAdmissionReadiness(ready).ok());
    RuntimeConfigurationSceneFixture actual_scene(
        package_root.path(), state_root.path(), package, publication_store, cell_admission);
    const auto &predecessor = actual_scene.predecessor;
    ASSERT_NE(predecessor.request.candidate_retained_bundle_id, package.bundle->retained_bundle_id());
    ASSERT_EQ(actual_scene.empty.bundle->value().cell_graph.cell_id, package.bundle->value().cell_graph.cell_id);
    ASSERT_TRUE(package.bundle->value().cell_graph.controllable_components.empty());
    ASSERT_TRUE(package.bundle->value().cell_graph.control_domains.empty());
    const auto continuity = VerifyNativeExecutionHostContinuity(
        *actual_scene.empty.bundle, *package.bundle, *package.artifact_source);
    ASSERT_TRUE(continuity.ok()) << continuity.error().message;
    FixedActiveRuntimePlanSource active_plan_source(
        predecessor, BuildVerifiedRuntimeConvergencePlan(
          *actual_scene.empty.bundle, *actual_scene.empty.artifact_source, "linux",
          {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)}));
    ScriptedRuntimePorts runtime_ports(independent_reader);
    AggregateFencePort kernel_fence(independent_reader, predecessor);
    OriginalSceneAdapter scene_owner(actual_scene.owner, kernel_fence);
    const auto actual_runtime_plan = BuildVerifiedRuntimeConvergencePlan(
        *package.bundle, *package.artifact_source, "linux",
        {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)});
    ASSERT_EQ(actual_runtime_plan.size(), 1U);
    kernel_fence.original_execution_host = ExecutionHostEpochDTO{
        actual_runtime_plan.front().runtime_unit_id, RuntimeEpoch("process-epoch-1"),
        RequestOwnerEpoch("original-host-owner"), ContentDigest(std::string(kDigestOne)),
        predecessor.publication_generation};
    kernel_fence.allow_prepublication_status = true;
    ScriptedRuntimeCompositionPort runtime_composition;
    ASSERT_TRUE(deployment::verify_retained_bundle_identity(*actual_scene.empty.bundle));
    const auto &installed_graph = actual_scene.empty.bundle->value().cell_graph;
    ASSERT_TRUE(installed_graph.controllable_components.empty());
    ASSERT_TRUE(installed_graph.control_domains.empty());
    ASSERT_TRUE(installed_graph.control_bindings.empty());
    // This fresh scripted owner has launched no component processes. Its exact
    // verified predecessor and host/Manager epochs qualify that known inventory;
    // an uncomposed inspection on any other fixture still refuses.
    const ManagerComponentProcessInventoryRequestDTO installed_scope{
        predecessor.request.candidate_projection_revision,
        predecessor.publication_generation,
        predecessor.durable_public_pointer_content_digest,
        kernel_fence.original_execution_host->runtime_unit_id,
        kernel_fence.original_execution_host->runtime_epoch,
        RuntimeEpoch("manager-runtime.failed-composition.1"), true};
    ASSERT_TRUE(installed_scope.valid());
    ASSERT_EQ(installed_scope.predecessor_projection_revision.retained_bundle_id,
              actual_scene.empty.bundle->retained_bundle_id());
    ASSERT_EQ(installed_scope.predecessor_projection_revision.graph_revision,
              installed_graph.graph_revision);
    const std::vector<ManagerComponentProcessOccurrenceDTO> installed_processes{};
    runtime_composition.installed_processes =
        [installed_scope, installed_processes](
            const ManagerComponentProcessInventoryRequestDTO &request) {
          if (!request.valid() || request != installed_scope) {
            return Result<ManagerComponentProcessInventoryResultDTO>::Err(
                {"fixture_installed_inventory_foreign",
                 "installed process inspection does not name the fixture owner", {}});
          }
          return Result<ManagerComponentProcessInventoryResultDTO>::Ok(
              {request, installed_processes, std::nullopt});
        };
    runtime_composition.fail_on_call = failing_call == 3 ? 0 : failing_call;
    const auto original_admission = cell_admission.GetAuthoringAdmission();
    ASSERT_TRUE(original_admission.authoring_allowed);
    const auto native_database = state_root.path() / "native-plan.sqlite3";
    std::optional<core::NativeApplyPlanDTO> original_native_plan;
    std::optional<ManagerSelectedRuntimeCompositionRequestDTO> failed_request;
    std::optional<core::CellAdmissionClaimDTO> original_claim_before_failure;
    std::optional<core::ConfigurationFenceAcceptedDTO> original_fence_before_failure;
    runtime_composition.before_failure = [&](const auto &composition_request) {
      failed_request = composition_request;
      original_claim_before_failure = cell_admission.ReadHeldCellAdmissionClaim(
          core::CellAdmissionClaimKindDTO::kManagerApply);
      const auto parent = journal.ReadStatus(composition_request.apply_id,
                                            composition_request.apply_generation);
      if (!original_claim_before_failure || !parent || !parent->fence_acceptance)
        throw std::runtime_error("fixture lost the original claim or accepted fence before injection");
      original_fence_before_failure = parent->fence_acceptance->accepted_fence;
      core::SqliteCCLCJournal native_plan(native_database, cell_id);
      const core::NamedApplyPublicationTransactionDTO planned_apply{
          composition_request.apply_id, composition_request.apply_generation};
      const core::NativeApplyPlanRequestDTO planned{
          planned_apply,
          package.bundle->retained_bundle_id(),
          package.bundle->value().cell_graph.graph_revision,
          {}, {}, {}, {}, {}, {}};
      const auto recorded = native_plan.PlanNativeApply(planned);
      if (!recorded.ok()) throw std::runtime_error(recorded.error().message);
      original_native_plan = recorded.value();
      const auto persisted = native_plan.LoadNativeApplyPlan(planned_apply);
      if (!persisted.ok() || persisted.value() != original_native_plan)
        throw std::runtime_error("fixture native plan was not durably exact before injection");
    };
    AlwaysIdleMaintenanceGate maintenance_gate;
    ManagerPlannedMaintenanceCoordinator planned_maintenance(
        active_plan_source, maintenance_gate);
    ManagerNamedApplyCoordinator coordinator(
        journal, active_plan_source, *package.bundle_source,
        *package.artifact_source, publication_store, scene_owner, nullptr,
        kernel_fence, cell_admission, runtime_composition, runtime_ports,
        runtime_ports, planned_maintenance,
        RuntimeEpoch("manager-runtime.failed-composition.1"), "linux",
        {"posix.1-2017", std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)},
        [] { return 1'000U; },
        [] { return ApplyId("apply.failed-composition.1"); },
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
        original_admission.admission_generation,
        std::nullopt,
    };
    int prepare_acknowledgements = 0;
    ManagerNamedApplyCoordinator::OriginalPrepareAcknowledgement original_acknowledgement;
    if (failing_call == 3) {
      original_acknowledgement = [&](const auto &, const auto &, const auto &acknowledgement) {
        ++prepare_acknowledgements;
        EXPECT_EQ(acknowledgement.request.phase, ManagerSelectedRuntimeCompositionPhase::kPrepare);
        EXPECT_EQ(acknowledgement.state, ManagerSelectedRuntimeCompositionState::kInstalled);
        runtime_composition.before_failure(acknowledgement.request);
        throw std::runtime_error("original observer transition could not be qualified");
      };
    }
    ASSERT_EQ(coordinator.AcceptOrReplay(request, {}, original_acknowledgement).state,
              ManagerNamedApplyResultState::kPending);
    const auto failed = coordinator.RecoverIncompleteApply();
    ASSERT_TRUE(failed.has_value());
    ASSERT_EQ(failed->state, ManagerNamedApplyResultState::kUnknownEffect);
    ASSERT_TRUE(failed->terminal_outcome.has_value());
    ASSERT_TRUE(failed->terminal_outcome->failure.has_value());
    const auto &failure = *failed->terminal_outcome->failure;
    // Ordinary M preserves the single configuration-failure code. Exact
    // dispatch history and callback evidence identify the attempted stage.
    EXPECT_EQ(failure.failure_code, "configuration_preservation_failed");
    EXPECT_EQ(failure.failure_message,
              failing_call == 3 ? "original observer transition could not be qualified"
                                : "scripted exact composition failure");
    EXPECT_EQ(runtime_composition.calls, failing_call == 2 ? 2 : 1);
    EXPECT_EQ(runtime_composition.phases,
              failing_call == 2
                  ? (std::vector<ManagerSelectedRuntimeCompositionPhase>{
                        ManagerSelectedRuntimeCompositionPhase::kPrepare,
                        ManagerSelectedRuntimeCompositionPhase::kFinalize})
                  : (std::vector<ManagerSelectedRuntimeCompositionPhase>{
                        ManagerSelectedRuntimeCompositionPhase::kPrepare}));
    ASSERT_TRUE(failed_request);
    EXPECT_EQ(failed_request->apply_id, failed->apply_id);
    EXPECT_EQ(failed_request->apply_generation, failed->apply_generation);
    EXPECT_EQ(failed_request->phase, failing_call == 2
        ? ManagerSelectedRuntimeCompositionPhase::kFinalize
        : ManagerSelectedRuntimeCompositionPhase::kPrepare);
    ASSERT_TRUE(original_native_plan);
    EXPECT_EQ(original_native_plan->apply,
              (core::NamedApplyPublicationTransactionDTO{failed->apply_id, failed->apply_generation}));
    EXPECT_EQ(original_native_plan->candidate_retained_bundle_id, package.bundle->retained_bundle_id());
    EXPECT_EQ(original_native_plan->candidate_graph_revision, package.bundle->value().cell_graph.graph_revision);
    // Both failing stages precede durable materialization/publication.
    EXPECT_EQ(failure.phase, ManagerApplyPhase::kRuntimeConverged);
    EXPECT_EQ(failure.effect_disposition, ManagerApplyEffectDisposition::kUnknown);
    const auto status = journal.ReadStatus(failed->apply_id, failed->apply_generation);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(failure.intent_digest, status->intent.intent_digest);
    ASSERT_TRUE(status->fence_acceptance.has_value());
    EXPECT_EQ(status->fence_acceptance->accepted_fence, original_fence_before_failure);
    EXPECT_EQ(failed_request->configuration_fence_id,
              status->fence_acceptance->accepted_fence.configuration_fence_id);
    EXPECT_EQ(failed_request->configuration_fence_generation,
              status->fence_acceptance->accepted_fence.configuration_fence_generation);
    EXPECT_EQ(failed_request->configuration_fence_scope,
              status->fence_acceptance->accepted_fence.captured_scope);
    EXPECT_TRUE(status->latest_runtime_staged_ready.has_value());
    EXPECT_FALSE(status->private_materialization.has_value());
    EXPECT_FALSE(status->publication_receipt.has_value());
    EXPECT_FALSE(status->fence_released.has_value());
    EXPECT_EQ(scene_owner.ordinary_install_calls, 0);
    EXPECT_EQ(scene_owner.ordinary_begin_calls, 0);
    EXPECT_EQ(scene_owner.stage_calls, 1);
    EXPECT_EQ(scene_owner.begin_calls, failing_call == 2 ? 1 : 0);
    const auto preserved = actual_scene.owner.ReadActiveSnapshot();
    ASSERT_TRUE(preserved);
    EXPECT_EQ(scene::EncodeLogicalSceneSnapshot(*preserved->shared()), actual_scene.snapshot_bytes);
    EXPECT_EQ(prepare_acknowledgements, failing_call == 3 ? 1 : 0);
    EXPECT_EQ(kernel_fence.release_calls, 0);
    EXPECT_EQ(settlement_calls, 0);
    const auto original_claim = cell_admission.ReadHeldCellAdmissionClaim(
        core::CellAdmissionClaimKindDTO::kManagerApply);
    ASSERT_TRUE(original_claim.has_value());
    EXPECT_EQ(original_claim, original_claim_before_failure);
    ASSERT_TRUE(status->fence_attempt);
    const core::ApplyFenceAcquireAttemptQueryDTO actual_attempt{
        status->fence_attempt->fence_intent, status->fence_attempt->fence_attempt_digest,
        RuntimeEpoch("manager-runtime.failed-composition.1")};
    const auto actual_fence = kernel_fence.ReconcileAcquireAttempt(actual_attempt);
    ASSERT_TRUE(actual_fence.matches(actual_attempt));
    EXPECT_EQ(actual_fence.state, core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact);
    EXPECT_EQ(actual_fence.accepted_fence, original_fence_before_failure);
    EXPECT_TRUE(actual_fence.admission_fenced);
    const core::CellAdmissionRecoveryReceiptQueryDTO held_query{
        original_claim->intent, *original_claim, std::nullopt};
    const auto held = cell_admission.GetCellAdmissionRecoveryReceipt(held_query);
    ASSERT_TRUE(held.ok());
    EXPECT_EQ(held.value().status, core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
    const core::NativeApplyParentPreparationFailureSource source =
        [&](const core::NativeApplyPlanDTO &plan) -> core::Result<bool> {
          if (!original_native_plan || plan != *original_native_plan)
            return core::Result<bool>::Err({"fixture_plan_changed", "foreign native parent plan", {}});
          const auto receipt = cell_admission.GetCellAdmissionRecoveryReceipt(held_query);
          if (!receipt.ok()) return core::Result<bool>::Err(receipt.error());
          const auto observed = ReadNativeApplyPreparationFailureReadOnly(
              database, cell_id, plan, *package.bundle, *original_claim,
              held_query, receipt.value());
          if (!observed.ok()) return core::Result<bool>::Err(observed.error());
          return core::Result<bool>::Ok(observed.value().has_value());
        };
    {
      core::SqliteCCLCJournal native_plan(native_database, cell_id,
          core::kDefaultCCLCExactResultHistory, core::kDefaultCCLCTerminalResultBytes,
          {}, source);
      const auto pending = native_plan.ReadUnsettledNativeApplyPlan();
      ASSERT_TRUE(pending.ok());
      // The old exact preparation classifier does not classify M's generic
      // configuration failure. All unknown plans remain retained, never adopted.
      ASSERT_TRUE(pending.value().has_value());
      EXPECT_EQ(*pending.value(), original_native_plan->apply);
      const auto retained_plan = native_plan.LoadNativeApplyPlan(original_native_plan->apply);
      ASSERT_TRUE(retained_plan.ok());
      EXPECT_EQ(retained_plan.value(), original_native_plan);
      const auto still_held = cell_admission.GetCellAdmissionRecoveryReceipt(held_query);
      ASSERT_TRUE(still_held.ok());
      EXPECT_EQ(still_held.value().matched_claim, original_claim);
      EXPECT_EQ(still_held.value().status, core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
    }
    EXPECT_FALSE(cell_admission.GetAuthoringAdmission().authoring_allowed);
    const auto calls = runtime_composition.calls;
    const auto replay = coordinator.AcceptOrReplay(request, {}, original_acknowledgement);
    EXPECT_EQ(replay.terminal_outcome, failed->terminal_outcome);
    EXPECT_FALSE(coordinator.RecoverIncompleteApply().has_value());
    {
      core::SqliteCCLCJournal replayed_native(native_database, cell_id);
      const auto replayed_plan = replayed_native.LoadNativeApplyPlan(original_native_plan->apply);
      ASSERT_TRUE(replayed_plan.ok());
      EXPECT_EQ(replayed_plan.value(), original_native_plan);
    }
    const auto replayed_claim = cell_admission.GetCellAdmissionRecoveryReceipt(held_query);
    ASSERT_TRUE(replayed_claim.ok());
    EXPECT_EQ(replayed_claim.value().matched_claim, original_claim_before_failure);
    EXPECT_EQ(replayed_claim.value().status, core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
    EXPECT_EQ(settlement_calls, 0);
    EXPECT_EQ(runtime_composition.calls, calls);
    EXPECT_EQ(prepare_acknowledgements, failing_call == 3 ? 1 : 0);
    journal.Close();
    independent_reader.Close();
    SqliteManagerApplyJournal reopened(database, cell_id);
    const auto retained = reopened.ReadStatus(failed->apply_id, failed->apply_generation);
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->terminal_outcome, status->terminal_outcome);
    EXPECT_FALSE(retained->private_materialization.has_value());
    EXPECT_FALSE(retained->fence_released.has_value());
    core::SqliteCCLCJournal reopened_native(native_database, cell_id,
        core::kDefaultCCLCExactResultHistory, core::kDefaultCCLCTerminalResultBytes,
        {}, source);
    const auto reopened_pending = reopened_native.ReadUnsettledNativeApplyPlan();
    ASSERT_TRUE(reopened_pending.ok());
    ASSERT_TRUE(reopened_pending.value().has_value());
    EXPECT_EQ(*reopened_pending.value(), original_native_plan->apply);
    const auto reopened_plan = reopened_native.LoadNativeApplyPlan(original_native_plan->apply);
    ASSERT_TRUE(reopened_plan.ok());
    EXPECT_EQ(reopened_plan.value(), original_native_plan);
    const auto reopened_claim = cell_admission.GetCellAdmissionRecoveryReceipt(held_query);
    ASSERT_TRUE(reopened_claim.ok());
    EXPECT_EQ(reopened_claim.value().matched_claim, original_claim_before_failure);
    EXPECT_EQ(reopened_claim.value().status, core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
    EXPECT_EQ(settlement_calls, 0);
  }
}

TEST(ManagerNamedApplyCoordinator,
     EmptyDeltaInstallationDoesNotRequireComponentReceipts) {
  ScopedPackageRoot package_root;
  ScopedPackageRoot state_root;
  auto package = MaterializeRosRuntimePackage(
      package_root.path(), true, deployment::test::kLite6ArmTargetId, {},
      "runtime-only-configuration-candidate");
  const auto intent_template = ApplyIntent(*package.bundle);
  const auto initial_predecessor = PredecessorPublication(intent_template);
  const auto database = state_root.path() / "receipt-apply.sqlite3";
  SqliteManagerApplyJournal journal(database,
                                    package.bundle->value().cell_graph.cell_id);
  SqliteManagerApplyJournal independent_reader(
      database, package.bundle->value().cell_graph.cell_id);
  ScriptedPublicationStore publication_store(initial_predecessor, &independent_reader);
  ScriptedCellAdmissionPort cell_admission;
  RuntimeConfigurationSceneFixture actual_scene(
      package_root.path(), state_root.path(), package, publication_store, cell_admission);
  const auto &predecessor = actual_scene.predecessor;
  ASSERT_NE(predecessor.request.candidate_retained_bundle_id, package.bundle->retained_bundle_id());
  ASSERT_EQ(actual_scene.empty.bundle->value().cell_graph.cell_id, package.bundle->value().cell_graph.cell_id);
  ASSERT_TRUE(package.bundle->value().cell_graph.controllable_components.empty());
  ASSERT_TRUE(package.bundle->value().cell_graph.control_domains.empty());
  const auto continuity = VerifyNativeExecutionHostContinuity(
      *actual_scene.empty.bundle, *package.bundle, *package.artifact_source);
  ASSERT_TRUE(continuity.ok()) << continuity.error().message;
  FixedActiveRuntimePlanSource active_plan_source(
      predecessor, BuildVerifiedRuntimeConvergencePlan(
          *actual_scene.empty.bundle, *actual_scene.empty.artifact_source, "linux",
          {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)}));
  ScriptedRuntimePorts runtime_ports(independent_reader);
  AggregateFencePort kernel_fence(independent_reader, predecessor);
  OriginalSceneAdapter scene_owner(actual_scene.owner, kernel_fence);
  const auto actual_runtime_plan = BuildVerifiedRuntimeConvergencePlan(
      *package.bundle, *package.artifact_source, "linux",
      {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)});
  ASSERT_EQ(actual_runtime_plan.size(), 1U);
  kernel_fence.original_execution_host = ExecutionHostEpochDTO{
      actual_runtime_plan.front().runtime_unit_id,
      RuntimeEpoch("process-epoch-1"), RequestOwnerEpoch("original-host-owner"),
      ContentDigest(std::string(kDigestOne)), predecessor.publication_generation};
  kernel_fence.allow_prepublication_status = true;
  ScriptedRuntimeCompositionPort runtime_composition;
  ASSERT_TRUE(deployment::verify_retained_bundle_identity(*actual_scene.empty.bundle));
  const auto &installed_graph = actual_scene.empty.bundle->value().cell_graph;
  ASSERT_TRUE(installed_graph.controllable_components.empty());
  ASSERT_TRUE(installed_graph.control_domains.empty());
  ASSERT_TRUE(installed_graph.control_bindings.empty());
  // This fresh scripted owner has launched no component processes. Its exact
  // verified predecessor and host/Manager epochs qualify that known inventory;
  // an uncomposed inspection on any other fixture still refuses.
  const ManagerComponentProcessInventoryRequestDTO installed_scope{
      predecessor.request.candidate_projection_revision,
      predecessor.publication_generation,
      predecessor.durable_public_pointer_content_digest,
      kernel_fence.original_execution_host->runtime_unit_id,
      kernel_fence.original_execution_host->runtime_epoch,
      RuntimeEpoch("manager-runtime.receipts.1"), true};
  ASSERT_TRUE(installed_scope.valid());
  ASSERT_EQ(installed_scope.predecessor_projection_revision.retained_bundle_id,
            actual_scene.empty.bundle->retained_bundle_id());
  ASSERT_EQ(installed_scope.predecessor_projection_revision.graph_revision,
            installed_graph.graph_revision);
  const std::vector<ManagerComponentProcessOccurrenceDTO> installed_processes{};
  runtime_composition.installed_processes =
      [installed_scope, installed_processes](
          const ManagerComponentProcessInventoryRequestDTO &request) {
        if (!request.valid() || request != installed_scope) {
          return Result<ManagerComponentProcessInventoryResultDTO>::Err(
              {"fixture_installed_inventory_foreign",
               "installed process inspection does not name the fixture owner", {}});
        }
        return Result<ManagerComponentProcessInventoryResultDTO>::Ok(
            {request, installed_processes, std::nullopt});
      };
  runtime_composition.omit_receipts = true;
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
      RuntimeEpoch("manager-runtime.receipts.1"), "linux",
      {"posix.1-2017", std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)},
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
      cell_admission.GetAuthoringAdmission().admission_generation,
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
  EXPECT_EQ(scene_owner.stage_calls, 1);
  EXPECT_EQ(scene_owner.begin_calls, 1);
  EXPECT_EQ(scene_owner.complete_calls, 1);
  EXPECT_EQ(scene_owner.ordinary_begin_calls, 0);
  EXPECT_EQ(scene_owner.ordinary_install_calls, 0);
  EXPECT_EQ(scene::EncodeLogicalSceneSnapshot(*actual_scene.owner.LogicalSnapshot()),
            actual_scene.snapshot_bytes);
}

CCLCLifecycleRequestResultDTO InitialApplyChildResult(
    const ManagerApplyComponentBringUpRequestDTO &request,
    const deployment::RetainedBundle &candidate, const int stage,
    const deployment::RetainedBundle *observed_predecessor = nullptr) {
  const ComponentLifecycleAcceptedDTO accepted{
      request.intent.request_context.request_id, RequestReplayStatusDTO::kAcceptedNew,
      LifecycleTransactionId("apply-lifecycle.1"), LifecycleTransactionGeneration(1U),
      request.intent.apply_generation_selection->candidate_generation, 42LL, 1000LL};
  const ComponentLifecycleRequestedStateDTO requested{
      ComponentLifecycleRequestedStatePresenceDTO::kResolved,
      ComponentLifecycleStateDTO::kPrepared};
  auto scope = request.parent_authority.held_fence_query.expected_scope;
  scope.controllable_component_ids = {request.intent.controllable_component_id};
  std::vector<ComponentLifecycleDomainStatusDTO> domains;
  for (const auto &expected : request.intent.expected_control_domains) {
    std::optional<ResolvedLifecycleControlDomainScopeDTO> current_scope;
    std::optional<ResolvedLifecycleProviderOccurrenceDTO> provider;
    if (stage != 0 || observed_predecessor != nullptr) {
      std::vector<ResourceId> resources;
      const auto &observed_bundle = stage == 0 ? *observed_predecessor : candidate;
      const auto &graph = observed_bundle.value().cell_graph;
      const auto desired = std::find_if(observed_bundle.value().desired_control_configurations.begin(),
          observed_bundle.value().desired_control_configurations.end(), [&](const auto &row) {
            return row.control_domain_id == expected.control_domain_id;
          });
      if (desired == observed_bundle.value().desired_control_configurations.end())
        throw std::runtime_error("fixture lacks observed topology");
      std::vector<ExpectedControlBindingRevisionDTO> observed_bindings;
      for (const auto &binding : desired->binding_routes)
        observed_bindings.push_back({binding.control_binding_id, binding.control_binding_revision, binding.control_route_id});
      const auto first = std::find_if(graph.control_bindings.begin(), graph.control_bindings.end(),
          [&](const auto &binding) { return binding.control_domain_id == expected.control_domain_id; });
      if (first == graph.control_bindings.end()) throw std::runtime_error("fixture lacks provider");
      for (const auto &binding : graph.control_bindings) {
        if (binding.control_domain_id != expected.control_domain_id) continue;
        for (const auto &resource : binding.resource_bindings) resources.push_back(resource.resource_id);
      }
      std::sort(resources.begin(), resources.end());
      resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
      current_scope = ResolvedLifecycleControlDomainScopeDTO{
          expected.control_domain_id, desired->control_domain_revision,
          desired->controller_topology_id, desired->topology_revision, std::nullopt,
          observed_bindings, resources};
      provider = ResolvedLifecycleProviderOccurrenceDTO{
          expected.control_domain_id, desired->control_domain_revision,
          first->provider_instance_id, first->provider_manifest_id, first->provider_manifest_revision,
          first->provider_runtime_unit_id, request.runtime_epoch, ContentDigest(std::string(kDigestOne))};
    }
    domains.push_back({expected.control_domain_id, expected, current_scope, provider,
        stage == 0 && !observed_predecessor ? std::nullopt : std::optional(ComponentLifecycleStateDTO::kDeclared),
        stage == 0 && !observed_predecessor ? std::nullopt : std::optional(stage <= 1 ? ComponentLifecycleStateDTO::kDeclared
                                                                         : ComponentLifecycleStateDTO::kPrepared),
        requested, stage < 2 ? ComponentLifecycleDomainActionStateDTO::kReserved
                             : ComponentLifecycleDomainActionStateDTO::kSucceeded,
        stage == 0 ? ComponentLifecycleDomainEffectDTO::kNone
                   : ComponentLifecycleDomainEffectDTO::kKnownNoEffect,
        {}, std::nullopt, std::nullopt});
  }
  const ApplyComponentBringUpEvidenceDTO evidence{
      request.parent_authority, scope,
      ApplyDelegatedComponentLifecycleSemanticRequestDigest(request.intent,
          request.candidate_projection_revision, request.candidate_retained_bundle_digest,
          request.candidate_retained_bundle_bytes, request.runtime_unit_id, request.runtime_epoch,
          request.manager_runtime_epoch, request.parent_authority.parent),
      request.candidate_projection_revision, request.candidate_retained_bundle_digest,
      request.candidate_retained_bundle_bytes, {request.runtime_unit_id, request.runtime_epoch},
      std::nullopt, std::nullopt, std::nullopt};
  const ComponentLifecycleStatusDTO status{
      accepted, request.intent, static_cast<std::uint64_t>(stage + 1),
      request.intent.request_context.request_owner_epoch,
      observed_predecessor ? std::optional(ComponentLifecycleStateDTO::kDeclared) : std::nullopt,
      stage == 0 && !observed_predecessor ? std::nullopt : std::optional(stage <= 1 ? ComponentLifecycleStateDTO::kDeclared
                                                                       : ComponentLifecycleStateDTO::kPrepared),
      requested, stage == 0 ? ComponentLifecycleTransactionPhaseDTO::kAccepted
                           : (stage == 3 ? ComponentLifecycleTransactionPhaseDTO::kSucceeded
                                         : ComponentLifecycleTransactionPhaseDTO::kVerifying),
      domains, false, stage == 3 ? std::optional(ComponentLifecycleTerminalResultDTO::kSucceeded)
                                : std::nullopt,
      std::nullopt, 42LL, 42LL + stage, stage == 3 ? 0LL : 900LL,
      std::nullopt, evidence};
  if (!status.locally_valid()) throw std::runtime_error("invalid original CCLC fixture status");
  return {RequestReplayStatusDTO::kAcceptedNew, accepted, status, std::nullopt, std::nullopt};
}

TEST(ManagerNamedApplyCoordinator,
     OriginalBringUpAcceptanceSealsCaptureAndReplayNeverReentersAdmission) {
  for (int scenario = 0; scenario < 7; ++scenario) {
    SCOPED_TRACE(scenario);
    ScopedPackageRoot package_root;
    ScopedPackageRoot state_root;
    auto previous = MaterializeRosRuntimePackage(package_root.path(), true);
    auto package = MaterializeRosRuntimePackage(package_root.path());
    const auto prototype = ApplyIntent(*package.bundle);
    const auto predecessor = PredecessorPublication(ApplyIntent(*previous.bundle));
    ASSERT_TRUE(previous.bundle->value().cell_graph.controllable_components.empty());
    ASSERT_FALSE(package.bundle->value().cell_graph.controllable_components.empty());
    ASSERT_FALSE(package.bundle->value().cell_graph.control_bindings.empty());
    ASSERT_FALSE(package.bundle->value().desired_control_configurations.empty());
    ASSERT_EQ(package.bundle->value().cell_graph.control_bindings.front().controllable_component_id,
              package.bundle->value().cell_graph.controllable_components.front().controllable_component_id);
    const auto path = state_root.path() / "original-bring-up.sqlite3";
    SqliteManagerApplyJournal journal(path, package.bundle->value().cell_graph.cell_id);
    SqliteManagerApplyJournal independent_reader(path, journal.cell_id());
    ScriptedPublicationStore publication_store(predecessor, &independent_reader);
    FixedActiveRuntimePlanSource active_plan_source(
        predecessor, BuildVerifiedRuntimeConvergencePlan(
                         *previous.bundle, *previous.artifact_source, "linux",
                         {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)}));
    ScriptedRuntimePorts runtime_ports(independent_reader);
    MaintenanceSceneOwner scene_owner(predecessor.request.candidate_projection_revision);
    AggregateFencePort kernel_fence(independent_reader, predecessor);
    ScriptedRuntimeCompositionPort runtime_composition;
    ScriptedCellAdmissionPort cell_admission;
    cell_admission.return_invalid_held_generation = scenario == 6;
    AlwaysIdleMaintenanceGate maintenance_gate;
    ManagerPlannedMaintenanceCoordinator planned_maintenance(active_plan_source, maintenance_gate);
    int authored_scene_reads = 0;
    ManagerNamedApplyCoordinator coordinator(
        journal, active_plan_source, *package.bundle_source, *package.artifact_source,
        publication_store, scene_owner, nullptr, kernel_fence, cell_admission,
        runtime_composition, runtime_ports, runtime_ports, planned_maintenance,
        RuntimeEpoch("manager-runtime.original"), "linux",
        {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)},
        [] { return 1'000U; }, [] { return ApplyId("apply.original-bring-up"); },
        [&](const auto &, const auto &) {
          ++authored_scene_reads;
          return std::vector<core::AddObjectRequestDTO>{};
        });
    ManagerNamedApplyRequestDTO request{
        prototype.request_context, package.bundle->value().deployment_artifact_id,
        package.bundle->value().deployment_revision,
        predecessor.request.candidate_retained_bundle_id,
        predecessor.request.candidate_graph_revision, package.bundle->retained_bundle_id(),
        prototype.prepared_activation, core::AdmissionGeneration(7U), std::nullopt};
    if (scenario == 0) {
      request.isaac_pose_observer_launch_facts =
          ManagerIsaacPoseObserverLaunchFactsDTO{
              "/run/manyforge/checked-observer.yaml", "observer.actual",
              "manager.original.observer.1"};
      ASSERT_TRUE(request.valid());
    }
    int original_source_calls = 0;
    const ManagerNamedApplyCoordinator::OriginalBringUpPlanSource original =
        [&](const ManagerApplyIntentDTO &intent, const deployment::RetainedBundle &old,
            const deployment::RetainedBundle &candidate, const ManagerActiveRuntimePlanDTO &active) {
          ++original_source_calls;
          EXPECT_EQ(old.retained_bundle_id(), predecessor.request.candidate_retained_bundle_id);
          EXPECT_EQ(candidate.retained_bundle_id(), package.bundle->retained_bundle_id());
          EXPECT_EQ(active.active_publication, predecessor);
          const auto &graph = candidate.value().cell_graph;
          const auto &component = graph.controllable_components.front();
          const core::ProjectionRevisionDTO target{
              candidate.retained_bundle_id(), graph.graph_revision,
              intent.expected_active_projection_revision.scene_revision};
          core::ComponentLifecycleIntentDTO child_intent{
              {core::RequestId("child.original"), intent.request_context.request_owner_epoch,
               "Apply", std::nullopt},
              component.controllable_component_id, core::ComponentLifecycleIntentKindDTO::kEnable,
              intent.expected_active_projection_revision.graph_revision,
              core::ComponentLifecycleGeneration(1U), {}, std::nullopt, {},
              *intent.expected_admission_generation};
          for (const auto &desired : candidate.value().desired_control_configurations) {
            std::vector<core::ExpectedControlBindingRevisionDTO> bindings;
            for (const auto &route : desired.binding_routes)
              bindings.push_back({route.control_binding_id, route.control_binding_revision,
                                  route.control_route_id});
            const core::ExpectedActiveControlConfigurationDTO absent{
                core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
                std::nullopt, std::nullopt};
            const core::ExpectedControlDomainRevisionDTO domain{
                desired.control_domain_id, desired.control_domain_revision,
                desired.controller_topology_id, desired.topology_revision, absent, bindings};
            child_intent.expected_control_domains.push_back(domain);
          }
          const auto digest = core::ContentDigest(deployment::sha256_digest(candidate.canonical_bytes()));
          ManagerApplyBringUpChildRequestDTO child{
              child_intent, target, digest, candidate.canonical_bytes().size(),
              graph.control_bindings.front().provider_runtime_unit_id,
              RuntimeEpoch("host.original"), RuntimeEpoch("manager-runtime.original")};
          const ManagerApplyPrivateMaterializationDTO materialization{
              target.scene_revision.content_hash, intent.apply_id, intent.apply_generation,
              target, digest, candidate.canonical_bytes().size()};
          const core::ActiveBundlePublicationRequestDTO publication{
              core::NamedApplyPublicationTransactionDTO{intent.apply_id, intent.apply_generation},
              target, target.retained_bundle_id, target.graph_revision,
              predecessor.publication_generation,
              ComputeManagerApplyPrivateMaterializationDigest(materialization)};
          ManagerApplyBringUpPlanDTO plan{
              intent.apply_id, intent.apply_generation, ComputeManagerApplyIntentDigest(intent),
              {}, {child}, publication, predecessor.durable_public_pointer_content_digest};
          for (const auto &row : graph.controllable_components)
            plan.configured_components.push_back(row.controllable_component_id);
          std::sort(plan.configured_components.begin(), plan.configured_components.end());
          if (scenario == 1) plan.configured_components.push_back(core::ControllableComponentId("z.foreign"));
          if (scenario == 2) plan.children.front().manager_runtime_epoch = RuntimeEpoch("manager.foreign");
          if (scenario == 3) plan.predecessor_public_pointer_digest = ContentDigest("sha256:" + std::string(64U, 'c'));
          if (scenario == 4) plan.children.front().candidate_retained_bundle_digest = ContentDigest("sha256:" + std::string(64U, 'd'));
          if (scenario == 5) plan.children.front().intent.expected_admission_generation = core::AdmissionGeneration(8U);
          return plan;
        };
    if (scenario == 6) {
      const auto refused = coordinator.AcceptOrReplay(request, original);
      ASSERT_EQ(refused.state, ManagerNamedApplyResultState::kFailed);
      ASSERT_TRUE(refused.terminal_outcome);
      ASSERT_TRUE(refused.terminal_outcome->failure);
      EXPECT_EQ(refused.terminal_outcome->failure->failure_code, "apply_admission_failed");
      EXPECT_EQ(cell_admission.acquire_calls, 0);
      EXPECT_EQ(cell_admission.lookup_intents.size(), 1U);
    } else if (scenario != 0) {
      EXPECT_THROW(static_cast<void>(coordinator.AcceptOrReplay(request, original)),
                   ManagerNamedApplyCoordinatorError);
      EXPECT_FALSE(journal.ReadRequest(request.request_context.request_owner_epoch,
                                       request.request_context.request_id));
      EXPECT_EQ(cell_admission.acquire_calls, 0);
      EXPECT_TRUE(cell_admission.lookup_intents.empty());
    } else {
      const auto accepted = coordinator.AcceptOrReplay(request, original);
      ASSERT_EQ(accepted.state, ManagerNamedApplyResultState::kPending);
      const auto plan = journal.ReadBringUpPlan(accepted.apply_id, accepted.apply_generation);
      ASSERT_TRUE(plan);
      const auto stored = journal.ReadIntent(accepted.apply_id, accepted.apply_generation);
      ASSERT_TRUE(stored);
      EXPECT_EQ(stored->intent.isaac_pose_observer_launch_facts,
                request.isaac_pose_observer_launch_facts);
      ASSERT_EQ(cell_admission.lookup_intents.size(), 1U);
      EXPECT_EQ(cell_admission.lookup_intents.front().request_payload_digest, plan->plan_digest);
      EXPECT_EQ(cell_admission.acquire_calls, 1);
      const auto held = cell_admission.GetCellAdmissionRecoveryReceipt(
          {cell_admission.lookup_intents.front(), std::nullopt, std::nullopt});
      ASSERT_TRUE(held.ok());
      ASSERT_TRUE(held.value().matched_claim);
      EXPECT_EQ(held.value().matched_claim->intent.request_payload_digest, plan->plan_digest);
      const auto lookups = cell_admission.lookup_intents.size();
      const ManagerNamedApplyCoordinator::OriginalBringUpPlanSource never_reconstruct =
          [](const auto &, const auto &, const auto &, const auto &) -> ManagerApplyBringUpPlanDTO {
            throw std::runtime_error("replayed source must not run");
          };
      EXPECT_TRUE(coordinator.AcceptOrReplay(request, never_reconstruct).request_replayed);
      auto conflicting_purpose = request;
      conflicting_purpose.scene_initialization =
          ManagerSceneInitialization::kInitialize;
      EXPECT_THROW(static_cast<void>(coordinator.AcceptOrReplay(
                       conflicting_purpose, never_reconstruct)),
                   ManagerNamedApplyCoordinatorError);
      EXPECT_EQ(stored->intent.scene_initialization,
                ManagerSceneInitialization::kPreserve);
      auto conflicting_facts = request;
      conflicting_facts.isaac_pose_observer_launch_facts->source_epoch_base =
          "manager.original.observer.foreign";
      ASSERT_TRUE(conflicting_facts.valid());
      EXPECT_THROW(static_cast<void>(coordinator.AcceptOrReplay(
                       conflicting_facts, never_reconstruct)),
                   ManagerNamedApplyCoordinatorError);
      EXPECT_TRUE(coordinator.AcceptOrReplay(request, never_reconstruct).request_replayed);
      EXPECT_EQ(cell_admission.acquire_calls, 1);
      EXPECT_EQ(cell_admission.lookup_intents.size(), lookups);
    }
    EXPECT_EQ(original_source_calls, 1);
    EXPECT_EQ(authored_scene_reads, 0);
    EXPECT_EQ(runtime_ports.ensure_calls, 0);
    EXPECT_EQ(runtime_composition.calls, 0);
    EXPECT_EQ(scene_owner.begin_calls, 0);
    EXPECT_EQ(kernel_fence.acquire_calls, 0);
    EXPECT_EQ(publication_store.publish_calls, 0);
  }
}


TEST(ManagerNamedApplyCoordinator,
     OriginalLinkedApplyUsesActualScenePreservingObjectsAttachmentsAndStopsOnUncertainty) {
  for (int scenario = -1; scenario != 13; ++scenario) {
    SCOPED_TRACE(scenario);
    ScopedPackageRoot package_root;
    ScopedPackageRoot state_root;
    const bool initial = scenario == -1 || scenario >= 7;
    auto empty = MaterializeRosRuntimePackage(package_root.path(), true);
    auto previous = MaterializeRosRuntimePackage(package_root.path(), initial, deployment::test::kLite6ArmTargetId, kDigestOne);
    auto package = MaterializeRosRuntimePackage(package_root.path(), false, deployment::test::kLite6ArmTargetId, kDigestTwo);
    const auto prototype = ApplyIntent(*package.bundle);
    ASSERT_NE(previous.bundle->retained_bundle_id(), package.bundle->retained_bundle_id());
    ASSERT_FALSE(package.bundle->value().cell_graph.controllable_components.empty());
    ASSERT_FALSE(package.bundle->value().cell_graph.control_bindings.empty());
    ASSERT_FALSE(package.bundle->value().desired_control_configurations.empty());
    ASSERT_EQ(package.bundle->value().cell_graph.control_bindings.front().controllable_component_id,
              package.bundle->value().cell_graph.controllable_components.front().controllable_component_id);
    const auto path = state_root.path() / "original-bring-up.sqlite3";
    SqliteManagerApplyJournal journal(path, package.bundle->value().cell_graph.cell_id);
    SqliteManagerApplyJournal independent_reader(path, journal.cell_id());
    ScriptedPublicationStore publication_store(PredecessorPublication(ApplyIntent(*empty.bundle)), &independent_reader);
    ScriptedCellAdmissionPort cell_admission;
    const auto admission = deployment::detail::publish_cell_scoped_canonical_empty_admission(
        package_root.path(), deployment::cell_scoped_canonical_empty_admission_filename(journal.cell_id()), *empty.bundle);
    AdoptionProjectionResolver resolver;
    const auto old_bindings = AdoptionBindings(*previous.bundle, RuntimeEpoch("predecessor-projection-epoch-1"));
    EXPECT_EQ(old_bindings.empty(), initial);
    for (const auto &binding : old_bindings) resolver.clients.push_back(std::make_shared<AdoptionProjection>(binding));
    const auto scene_path = state_root.path() / "actual-logical-scene.sqlite3";
    scene::LogicalSceneOwner actual_scene(scene_path, admission, publication_store,
        *package.bundle_source, resolver, cell_admission);
    const auto predecessor = EstablishActualScenePredecessor(
        actual_scene, publication_store, admission, *previous.bundle, old_bindings);
    if (!initial) {
    auto current = actual_scene.CurrentProjectionRevision();
    SceneTransactionRequestDTO add{
        {RequestId("actual-add"), actual_scene.CurrentRequestOwnerEpoch(), "Manager regression", std::nullopt},
        {}, current, AddObjectRequestDTO{SceneResourceId("scene-box"), BoxShapeDTO{{0.1, 0.2, 0.3}},
                                        Pose3DTO{}, CellSceneFrameRefDTO{journal.cell_id()}, {}},
        cell_admission.GetAuthoringAdmission().admission_generation};
    const auto added = actual_scene.Commit(add);
    ASSERT_TRUE(added.success) << (added.error ? added.error->message : "missing error");
    ASSERT_TRUE(added.committed_projection_revision);
    current = *added.committed_projection_revision;
    SceneTransactionRequestDTO attach{
        {RequestId("actual-attach"), actual_scene.CurrentRequestOwnerEpoch(), "Manager regression", std::nullopt},
        {}, current,
        AttachCarriedObjectRequestDTO{CarriedObjectAttachmentId("attachment.1"), SceneResourceId("scene-box"),
            ControllableComponentId("component.1"), CommandTargetId("arm"), FrameRoleId("tool_frame"),
            {CollisionBodyRoleId("finger")}, {ComponentAttachmentRevision("component-attachment-revision-1")}},
        cell_admission.GetAuthoringAdmission().admission_generation};
    attach.program_node_evidence = ProgramNodeAdmissionEvidenceDTO{
        RequestOwnerEpoch("runner.2"), ProgramRunId("program.2"), CellAdmissionClaimGeneration(9U),
        "attach", "attach:attempt.1", "scene.carried_attachment"};
    const auto attached = actual_scene.Commit(attach);
    ASSERT_TRUE(attached.success) << (attached.error ? attached.error->message : "missing error");
    }
    cell_admission.last_release.reset();
    const auto preserved = actual_scene.LogicalSnapshot();
    ASSERT_EQ(preserved->objects.size(), initial ? 0U : 1U);
    ASSERT_EQ(preserved->carried_object_attachments.size(), initial ? 0U : 1U);
    const auto preserved_bytes = scene::EncodeLogicalSceneSnapshot(*preserved);
    const auto original_scene_owner = actual_scene.CurrentRequestOwnerEpoch();
    FixedActiveRuntimePlanSource active_plan_source(
        predecessor, BuildVerifiedRuntimeConvergencePlan(
                         *previous.bundle, *previous.artifact_source, "linux",
                         {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)}));
    ScriptedRuntimePorts runtime_ports(independent_reader);
    AggregateFencePort kernel_fence(independent_reader, predecessor);
    OriginalSceneAdapter scene_owner(actual_scene, kernel_fence);
    ScriptedRuntimeCompositionPort runtime_composition;
    kernel_fence.original_execution_host = ExecutionHostEpochDTO{
        package.bundle->value().cell_graph.control_bindings.front().provider_runtime_unit_id,
        RuntimeEpoch("process-epoch-1"), RequestOwnerEpoch("original-host-owner"),
        ContentDigest(std::string(kDigestOne)), predecessor.publication_generation};
    // The replacement's observed predecessor metadata exists before Apply.
    // Initial zero-component predecessors have no configuration rows.
    for (const auto &desired : previous.bundle->value().desired_control_configurations) {
      kernel_fence.predecessor_configurations.push_back({
          desired.control_domain_id,
          ActiveControlConfigurationId("predecessor." + desired.control_domain_id.value()),
          ActiveControlConfigurationGeneration(1U)});
    }
    kernel_fence.foreign_attempt_digest = scenario == 9;
    kernel_fence.stale_attempt_host = scenario == 10;
    kernel_fence.released_attempt = scenario == 11;
    kernel_fence.foreign_attempt_acceptance = scenario == 12;
    runtime_composition.installed = [&](const auto &input) {
      if (input.phase != ManagerSelectedRuntimeCompositionPhase::kPrepare) return;
      EXPECT_TRUE(kernel_fence.installed_configurations.empty());
      for (const auto &domain : input.configuration_fence_scope.domains) {
        if (domain.candidate_control_domain_revision) {
          kernel_fence.installed_configurations.push_back({
              domain.control_domain_id,
              ActiveControlConfigurationId("installed." + domain.control_domain_id.value()),
              ActiveControlConfigurationGeneration(1U)});
        }
      }
    };
    scene_owner.before_stage = [&](const auto &input) {
      EXPECT_TRUE(kernel_fence.installed_configurations.empty());
      EXPECT_EQ(kernel_fence.status_calls, 0);
      EXPECT_GT(kernel_fence.reconcile_calls, 0);
      for (const auto &binding : input.native_runtime_bindings) {
        const auto found = std::find_if(resolver.clients.begin(), resolver.clients.end(),
            [&](const auto &client) { return client->ProjectionHealth().native_runtime_binding == binding; });
        if (found != resolver.clients.end()) continue;
        auto projection = std::make_shared<AdoptionProjection>(binding);
        projection->accept_next_rebuild = scenario == 8;
        resolver.clients.push_back(std::move(projection));
      }
    };
    scene_owner.alter_begin = [&](auto &input) {
      if (scenario == 1) input.parent_authority.parent_claim.intent.owner_epoch = RequestOwnerEpoch("foreign-manager");
      if (scenario == 2) input.expected_predecessor_publication.durable_public_pointer_content_digest = ContentDigest(std::string(kDigestOne));
      if (scenario == 3) ++input.expected_snapshot_identity.revision.revision_id;
    };
    scene_owner.after_begin = [&] {
      if (scenario == 4) kernel_fence.throw_next_status = true;
      if (scenario == 5) publication_store.fail_next_read = true;
    };
    scene_owner.after_complete = [&] { if (scenario == 6) kernel_fence.throw_next_status = true; };
    kernel_fence.allow_prepublication_status = true;
    AlwaysIdleMaintenanceGate maintenance_gate;
    ManagerPlannedMaintenanceCoordinator planned_maintenance(active_plan_source, maintenance_gate);
    int authored_scene_reads = 0;
    ManagerNamedApplyCoordinator coordinator(
        journal, active_plan_source, *package.bundle_source, *package.artifact_source,
        publication_store, scene_owner, nullptr, kernel_fence, cell_admission,
        runtime_composition, runtime_ports, runtime_ports, planned_maintenance,
        RuntimeEpoch("manager-runtime.original"), "linux",
        {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)},
        [] { return 1'000U; }, [] { return ApplyId("apply.original-bring-up"); },
        [&](const auto &, const auto &) {
          ++authored_scene_reads;
          return std::vector<core::AddObjectRequestDTO>{};
        });
    ManagerNamedApplyRequestDTO request{
        prototype.request_context, package.bundle->value().deployment_artifact_id,
        package.bundle->value().deployment_revision,
        predecessor.request.candidate_retained_bundle_id,
        predecessor.request.candidate_graph_revision, package.bundle->retained_bundle_id(),
        prototype.prepared_activation, cell_admission.GetAuthoringAdmission().admission_generation, std::nullopt};
    int original_source_calls = 0;
    const ManagerNamedApplyCoordinator::OriginalBringUpPlanSource original =
        [&](const ManagerApplyIntentDTO &intent, const deployment::RetainedBundle &old,
            const deployment::RetainedBundle &candidate, const ManagerActiveRuntimePlanDTO &active) {
          ++original_source_calls;
          EXPECT_EQ(old.retained_bundle_id(), predecessor.request.candidate_retained_bundle_id);
          EXPECT_EQ(candidate.retained_bundle_id(), package.bundle->retained_bundle_id());
          EXPECT_EQ(active.active_publication, predecessor);
          const auto &graph = candidate.value().cell_graph;
          const auto &component = graph.controllable_components.front();
          const core::ProjectionRevisionDTO target{
              candidate.retained_bundle_id(), graph.graph_revision,
              intent.expected_active_projection_revision.scene_revision};
          core::ComponentLifecycleIntentDTO child_intent{
              {core::RequestId("child.original"), intent.request_context.request_owner_epoch,
               "Apply", std::nullopt},
              component.controllable_component_id, core::ComponentLifecycleIntentKindDTO::kPrepare,
              intent.expected_active_projection_revision.graph_revision,
              core::ComponentLifecycleGeneration(initial ? 0U : 1U), {}, std::nullopt, {},
              *intent.expected_admission_generation};
          child_intent.apply_generation_selection = ApplyComponentGenerationSelectionDTO{
              initial ? std::nullopt : std::optional(ComponentLifecycleGeneration(1U)),
              ComponentLifecycleGeneration(initial ? 1U : 2U), initial ? 0U : 1U, std::nullopt};
          for (const auto &desired : candidate.value().desired_control_configurations) {
            std::vector<core::ExpectedControlBindingRevisionDTO> bindings;
            for (const auto &route : desired.binding_routes)
              bindings.push_back({route.control_binding_id, route.control_binding_revision,
                                  route.control_route_id});
            const core::ExpectedActiveControlConfigurationDTO absent{
                core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
                std::nullopt, std::nullopt};
            const core::ExpectedControlDomainRevisionDTO domain{
                desired.control_domain_id, desired.control_domain_revision,
                desired.controller_topology_id, desired.topology_revision, absent, bindings};
            child_intent.expected_control_domains.push_back(domain);
          }
          const auto digest = core::ContentDigest(deployment::sha256_digest(candidate.canonical_bytes()));
          ManagerApplyBringUpChildRequestDTO child{
              child_intent, target, digest, candidate.canonical_bytes().size(),
              graph.control_bindings.front().provider_runtime_unit_id,
              RuntimeEpoch("process-epoch-1"), RuntimeEpoch("manager-runtime.original")};
          const ManagerApplyPrivateMaterializationDTO materialization{
              target.scene_revision.content_hash, intent.apply_id, intent.apply_generation,
              target, digest, candidate.canonical_bytes().size()};
          const core::ActiveBundlePublicationRequestDTO publication{
              core::NamedApplyPublicationTransactionDTO{intent.apply_id, intent.apply_generation},
              target, target.retained_bundle_id, target.graph_revision,
              predecessor.publication_generation,
              ComputeManagerApplyPrivateMaterializationDigest(materialization)};
          ManagerApplyBringUpPlanDTO plan{
              intent.apply_id, intent.apply_generation, ComputeManagerApplyIntentDigest(intent),
              {}, {child}, publication, predecessor.durable_public_pointer_content_digest};
          for (const auto &row : graph.controllable_components)
            plan.configured_components.push_back(row.controllable_component_id);
          std::sort(plan.configured_components.begin(), plan.configured_components.end());
          return plan;
        };

    ASSERT_EQ(package.bundle->value().cell_graph.controllable_components.size(), 1U);
    bool holding = false;
    bool terminal = false;
    runtime_composition.child_submit = [&](const auto &child) {
      EXPECT_TRUE(journal.ReadBringUpChildProof(child.parent_authority.parent.owner.apply_id,
          child.parent_authority.parent.owner.apply_generation, child.intent.controllable_component_id,
          ManagerApplyBringUpChildPhase::kAccepted));
      if (scenario == 7) return Result<ManagerApplyComponentBringUpResultDTO>::Err(
          {"uncertain_send", "reply lost after send", {}});
      return Result<ManagerApplyComponentBringUpResultDTO>::Ok(
          {child, InitialApplyChildResult(child, *package.bundle, 1, initial ? nullptr : previous.bundle.get())});
    };
    runtime_composition.child_read = [&](const auto &child) {
      const int stage = runtime_composition.child_submit_calls == 0 ? 0 : (terminal ? 3 : holding ? 2 : 1);
      return Result<ManagerApplyComponentBringUpResultDTO>::Ok(
          {child, InitialApplyChildResult(child, *package.bundle, stage, initial ? nullptr : previous.bundle.get())});
    };
    const auto accepted = coordinator.AcceptOrReplay(request, original);
    ASSERT_EQ(accepted.state, ManagerNamedApplyResultState::kPending);
    const auto plan = journal.ReadBringUpPlan(accepted.apply_id, accepted.apply_generation);
    ASSERT_TRUE(plan);
    const auto component = plan->plan.children.front().intent.controllable_component_id;
    auto first = coordinator.RecoverIncompleteApply();
    ASSERT_TRUE(first);
    if (scenario >= 9) {
      ASSERT_EQ(first->state, ManagerNamedApplyResultState::kUnknownEffect);
      ASSERT_TRUE(first->terminal_outcome && first->terminal_outcome->failure);
      EXPECT_EQ(first->terminal_outcome->failure->failure_code, "scene_product_projection_staging_failed");
      EXPECT_EQ(first->terminal_outcome->failure->phase, ManagerApplyPhase::kRuntimeConverged);
      EXPECT_EQ(first->terminal_outcome->failure->effect_disposition, ManagerApplyEffectDisposition::kUnknown);
      EXPECT_EQ(scene_owner.stage_calls, 0);
      EXPECT_EQ(scene_owner.stage_read_calls, 0);
      EXPECT_EQ(scene_owner.begin_calls, 0);
      EXPECT_EQ(runtime_composition.calls, 0);
      EXPECT_EQ(runtime_composition.child_submit_calls, 0);
      EXPECT_EQ(kernel_fence.status_calls, 0);
      EXPECT_EQ(kernel_fence.release_calls, 0);
      EXPECT_TRUE(kernel_fence.installed_configurations.empty());
      EXPECT_FALSE(cell_admission.last_release);
      EXPECT_FALSE(actual_scene.ReadMaintenanceInitialization({accepted.apply_id, accepted.apply_generation}));
      const auto before_replay = kernel_fence.reconcile_calls;
      const auto replay = coordinator.AcceptOrReplay(request, original);
      EXPECT_EQ(replay.state, ManagerNamedApplyResultState::kUnknownEffect);
      EXPECT_EQ(kernel_fence.reconcile_calls, before_replay);
      EXPECT_FALSE(coordinator.RecoverIncompleteApply());
      EXPECT_EQ(original_source_calls, 1);
      continue;
    }
    ::testing::Message first_progress;
    if (scene_owner.stage_calls != 1) {
      const auto observed = journal.ReadStatus(accepted.apply_id, accepted.apply_generation);
      first_progress << "first.state=" << static_cast<int>(first->state)
                     << ", replayed=" << first->request_replayed
                     << ", source_calls=" << original_source_calls
                     << ", fence_acquire=" << kernel_fence.acquire_calls
                     << ", fence_status=" << kernel_fence.status_calls
                     << ", runtime_observe=" << runtime_ports.observe_calls
                     << ", runtime_ensure=" << runtime_ports.ensure_calls
                     << ", runtime_progress=" << runtime_ports.progress_calls;
      if (first->terminal_outcome && first->terminal_outcome->failure) {
        const auto &failure = *first->terminal_outcome->failure;
        first_progress << ", returned_failure=" << failure.failure_code
                       << ": " << failure.failure_message
                       << ", returned_phase=" << ToString(failure.phase)
                       << ", returned_effect=" << ToString(failure.effect_disposition);
      }
      if (observed) {
        first_progress << ", retained_phase=" << ToString(DeriveManagerApplyProgress(*observed).phase)
                       << ", retained_fence=" << observed->fence_acceptance.has_value()
                       << ", retained_runtime_intent=" << observed->runtime_convergence_intent.has_value()
                       << ", retained_staged=" << observed->latest_runtime_staged_ready.has_value()
                       << ", retained_child_proofs=" << observed->bring_up_child_proofs.size()
                       << ", retained_publication=" << observed->publication_receipt.has_value();
        if (observed->terminal_outcome && observed->terminal_outcome->terminal_outcome.failure) {
          const auto &failure = *observed->terminal_outcome->terminal_outcome.failure;
          first_progress << ", retained_failure=" << failure.failure_code
                         << ": " << failure.failure_message
                         << ", retained_effect=" << ToString(failure.effect_disposition)
                         << ", retained_intent=" << failure.intent_digest.value();
        }
      } else {
        first_progress << ", retained_status=absent";
      }
      const auto head = publication_store.ReadActiveBundlePublication(predecessor.publication_generation);
      if (head.ok()) {
        first_progress << ", publication_generation=" << head.value().publication_generation.value()
                       << ", publication_bundle=" << head.value().request.candidate_retained_bundle_id.value()
                       << ", publication_pointer=" << head.value().durable_public_pointer_content_digest.value();
      } else {
        first_progress << ", publication_error=" << head.error().code
                       << ": " << head.error().message;
      }
      // Preserve the failed fixture's main databases and WAL files before its
      // scoped cleanup. The synchronous producer is idle at this observation.
      const auto evidence = state_root.path().parent_path() /
          (state_root.path().filename().string() + "-original-linked-stage-failure");
      std::error_code copy_error;
      std::filesystem::copy(state_root.path(), evidence,
                            std::filesystem::copy_options::recursive, copy_error);
      first_progress << ", fixture_evidence=" << evidence.string()
                     << ", fixture_copy_error=" << copy_error.message();
    }
    ASSERT_EQ(scene_owner.stage_calls, 1) << first_progress.GetString();
    if (scenario == 8) {
      ASSERT_EQ(first->state, ManagerNamedApplyResultState::kPending);
      EXPECT_EQ(runtime_composition.calls, 0);
      EXPECT_EQ(runtime_composition.child_submit_calls, 0);
      EXPECT_EQ(scene_owner.begin_calls, 0);
      EXPECT_FALSE(actual_scene.ReadMaintenanceInitialization({accepted.apply_id, accepted.apply_generation}));
      for (const auto &projection : resolver.clients) {
        if (projection->pending_target) projection->CompletePendingRebuild();
      }
      first = coordinator.RecoverIncompleteApply();
      ASSERT_TRUE(first);
      EXPECT_EQ(scene_owner.stage_calls, 1);
      EXPECT_EQ(scene_owner.stage_read_calls, 1);
    }
    ASSERT_EQ(runtime_composition.child_submit_calls, 1);
    EXPECT_TRUE(journal.ReadBringUpChildProof(accepted.apply_id, accepted.apply_generation, component,
                                            ManagerApplyBringUpChildPhase::kAccepted));
    EXPECT_FALSE(journal.ReadBringUpChildProof(accepted.apply_id, accepted.apply_generation, component,
                                             ManagerApplyBringUpChildPhase::kHolding));
    EXPECT_EQ(publication_store.publish_calls, 0);
    EXPECT_EQ(kernel_fence.release_calls, 0);
    EXPECT_FALSE(cell_admission.last_release && cell_admission.last_release->owner_role == CellAdmissionOwnerRoleDTO::kManager);
    ASSERT_EQ(runtime_composition.phases.size(), 1U);
    EXPECT_EQ(runtime_composition.phases.front(), ManagerSelectedRuntimeCompositionPhase::kPrepare);
    ASSERT_TRUE(runtime_composition.last_request);
    const auto &host_request = *runtime_composition.last_request;
    EXPECT_EQ(host_request.candidate_projection_revision, plan->plan.expected_publication.candidate_projection_revision);
    EXPECT_EQ(host_request.authoritative_scene_snapshot_digest,
              preserved->revision.content_hash);
    EXPECT_EQ(host_request.predecessor_projection_revision.scene_revision, preserved->revision);
    EXPECT_EQ(host_request.expected_predecessor_publication_generation,
              *plan->plan.expected_publication.expected_previous_publication_generation);
    EXPECT_EQ(host_request.expected_predecessor_public_pointer_digest, plan->plan.predecessor_public_pointer_digest);
    EXPECT_EQ(host_request.retained_bundle_canonical_content_digest,
              ContentDigest(deployment::sha256_digest(package.bundle->canonical_bytes())));
    EXPECT_EQ(host_request.retained_bundle_canonical_byte_size, package.bundle->canonical_bytes().size());
    const ManagerApplyPrivateMaterializationDTO expected_materialization{
        host_request.authoritative_scene_snapshot_digest, accepted.apply_id, accepted.apply_generation,
        host_request.candidate_projection_revision, host_request.retained_bundle_canonical_content_digest,
        host_request.retained_bundle_canonical_byte_size};
    EXPECT_EQ(ComputeManagerApplyPrivateMaterializationDigest(expected_materialization),
              plan->plan.expected_publication.private_materialization_content_digest);
    const auto retained = journal.ReadStatus(accepted.apply_id, accepted.apply_generation);
    ASSERT_TRUE(retained && retained->fence_acceptance && retained->latest_runtime_staged_ready);
    EXPECT_EQ(host_request.configuration_fence_id, retained->fence_acceptance->accepted_fence.configuration_fence_id);
    EXPECT_EQ(host_request.configuration_fence_generation,
              retained->fence_acceptance->accepted_fence.configuration_fence_generation);
    EXPECT_EQ(host_request.configuration_fence_scope, retained->fence_acceptance->accepted_fence.captured_scope);
    EXPECT_EQ(host_request.runtime_epoch, retained->latest_runtime_staged_ready->staged_ready.runtime_units.front().runtime_epoch);
    EXPECT_EQ(host_request.manager_runtime_epoch, RuntimeEpoch("manager-runtime.original"));
    EXPECT_EQ(scene_owner.ordinary_begin_calls, 0);
    EXPECT_EQ(scene_owner.ordinary_install_calls, 0);
    EXPECT_FALSE(scene_owner.ReadMaintenanceInitialization({accepted.apply_id, accepted.apply_generation}));
    EXPECT_FALSE(retained->private_materialization);
    EXPECT_FALSE(retained->publication_receipt);
    if (scenario == 7) {
      ASSERT_EQ(first->state, ManagerNamedApplyResultState::kUnknownEffect);
      ASSERT_TRUE(first->terminal_outcome && first->terminal_outcome->failure);
      EXPECT_EQ(first->terminal_outcome->failure->failure_code, "apply_component_bring_up_failed");
      EXPECT_EQ(runtime_composition.child_submit_calls, 1);
      EXPECT_EQ(scene_owner.begin_calls, 0);
      EXPECT_EQ(kernel_fence.release_calls, 0);
      const auto calls = runtime_composition.calls;
      EXPECT_FALSE(coordinator.RecoverIncompleteApply());
      EXPECT_TRUE(coordinator.AcceptOrReplay(request, original).request_replayed);
      EXPECT_EQ(runtime_composition.calls, calls);
      EXPECT_EQ(runtime_composition.child_submit_calls, 1);
      continue;
    }
    const auto actual_accepted_proof = journal.ReadBringUpChildProof(
        accepted.apply_id, accepted.apply_generation, component, ManagerApplyBringUpChildPhase::kAccepted);
    ASSERT_TRUE(actual_accepted_proof && actual_accepted_proof->proof.result.status);
    const auto &actual_parent_claim = actual_accepted_proof->proof.result.status->apply_bring_up->parent_authority.parent_claim;
    const auto held = cell_admission.GetCellAdmissionRecoveryReceipt(
        {actual_parent_claim.intent, actual_parent_claim, std::nullopt});
    ASSERT_TRUE(held.ok() && held.value().matched_claim);
    const LogicalSceneSnapshotIdentityDTO actual_snapshot{
        retained->intent.intent.expected_active_projection_revision.scene_revision, journal.cell_id()};
    ManagerSelectedRuntimeCompositionCoordinator original_preparer(
        journal, scene_owner, runtime_composition, RuntimeEpoch("manager-runtime.original"));
    const auto host_calls = runtime_composition.calls;
    auto foreign_snapshot = actual_snapshot;
    ++foreign_snapshot.revision.revision_id;
    EXPECT_THROW(static_cast<void>(original_preparer.PrepareOriginalApplyComponentBringUp(
                     *plan, foreign_snapshot, *held.value().matched_claim, *package.bundle)),
                 ManagerSelectedRuntimeCompositionCoordinatorError);
    auto foreign_plan = *plan;
    foreign_plan.plan_digest = ContentDigest(std::string(kDigestTwo));
    EXPECT_THROW(static_cast<void>(original_preparer.PrepareOriginalApplyComponentBringUp(
                     foreign_plan, actual_snapshot, *held.value().matched_claim, *package.bundle)),
                 ManagerSelectedRuntimeCompositionCoordinatorError);
    auto foreign_claim = *held.value().matched_claim;
    foreign_claim.acquired_admission_generation = AdmissionGeneration(
        foreign_claim.acquired_admission_generation.value() + 1U);
    EXPECT_THROW(static_cast<void>(original_preparer.PrepareOriginalApplyComponentBringUp(
                     *plan, actual_snapshot, foreign_claim, *package.bundle)),
                 ManagerSelectedRuntimeCompositionCoordinatorError);
    EXPECT_THROW(static_cast<void>(original_preparer.PrepareOriginalApplyComponentBringUp(
                     *plan, actual_snapshot, *held.value().matched_claim, *previous.bundle)),
                 ManagerSelectedRuntimeCompositionCoordinatorError);
    EXPECT_EQ(runtime_composition.calls, host_calls);
    EXPECT_EQ(scene_owner.ordinary_begin_calls, 0);
    EXPECT_EQ(scene_owner.ordinary_install_calls, 0);
    ASSERT_EQ(first->state, ManagerNamedApplyResultState::kPending);
    {
      ManagerNamedApplyCoordinator replacement_before_scene(
          journal, active_plan_source, *package.bundle_source, *package.artifact_source,
          publication_store, scene_owner, nullptr, kernel_fence, cell_admission,
          runtime_composition, runtime_ports, runtime_ports, planned_maintenance,
          RuntimeEpoch("manager-runtime.replaced"), "linux",
          {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)},
          [] { return 1'000U; }, [] { return ApplyId("must-not-be-minted"); },
          [&](const auto &, const auto &) { ++authored_scene_reads; return std::vector<AddObjectRequestDTO>{}; });
      const auto prior_calls = runtime_composition.calls;
      const auto observed_pending = replacement_before_scene.RecoverIncompleteApply();
      ASSERT_TRUE(observed_pending);
      EXPECT_EQ(observed_pending->state, ManagerNamedApplyResultState::kPending);
      EXPECT_EQ(runtime_composition.calls, prior_calls);
      EXPECT_EQ(runtime_composition.child_submit_calls, 1);
      EXPECT_EQ(scene_owner.begin_calls, 0);
      EXPECT_EQ(scene_owner.complete_calls, 0);
      EXPECT_EQ(kernel_fence.release_calls, 0);
    }
    holding = true;
    const auto progressed = coordinator.RecoverIncompleteApply();
    ASSERT_TRUE(progressed);
    EXPECT_TRUE(journal.ReadBringUpChildProof(accepted.apply_id, accepted.apply_generation, component,
                                            ManagerApplyBringUpChildPhase::kHolding));
    EXPECT_EQ(scene_owner.begin_calls, 1);
    EXPECT_EQ(scene_owner.ordinary_begin_calls, 0);
    EXPECT_EQ(scene_owner.ordinary_install_calls, 0);
    EXPECT_EQ(scene_owner.ordinary_complete_calls, 0);
    const auto observed_snapshot = actual_scene.ReadActiveSnapshot();
    ASSERT_TRUE(observed_snapshot);
    EXPECT_EQ(scene::EncodeLogicalSceneSnapshot(*observed_snapshot->shared()), preserved_bytes);
    EXPECT_EQ(actual_scene.CurrentRequestOwnerEpoch(), original_scene_owner);
    if (scenario >= 1 && scenario <= 6) {
      ASSERT_EQ(progressed->state, ManagerNamedApplyResultState::kUnknownEffect);
      ASSERT_TRUE(progressed->terminal_outcome && progressed->terminal_outcome->failure);
      EXPECT_EQ(progressed->terminal_outcome->failure->failure_code,
                scenario == 6 ? "apply_component_bring_up_settlement_failed" : "scene_product_adoption_failed");
      EXPECT_EQ(publication_store.publish_calls, scenario == 6 ? 1 : 0);
      EXPECT_EQ(scene_owner.complete_calls, scenario == 6 ? 1 : 0);
      EXPECT_EQ(kernel_fence.release_calls, 0);
      EXPECT_FALSE(cell_admission.last_release && cell_admission.last_release->owner_role == CellAdmissionOwnerRoleDTO::kManager);
      EXPECT_EQ(runtime_composition.phases.size(), scenario == 6 ? 2U : 1U);
      EXPECT_EQ(scene_owner.ReadMaintenanceInitialization({accepted.apply_id, accepted.apply_generation}).has_value(), scenario >= 4);
    } else {
      ASSERT_EQ(progressed->state, ManagerNamedApplyResultState::kPending);
      EXPECT_EQ(publication_store.publish_calls, 1);
      EXPECT_EQ(scene_owner.complete_calls, 1);
      ASSERT_TRUE(journal.ReadPublicationReceipt(accepted.apply_id, accepted.apply_generation));
      EXPECT_FALSE(journal.ReadBringUpChildProof(accepted.apply_id, accepted.apply_generation, component,
                                               ManagerApplyBringUpChildPhase::kTerminal));
      EXPECT_EQ(kernel_fence.release_calls, 0);
      EXPECT_EQ(runtime_composition.phases.size(), 2U);
      const auto before_terminal = actual_scene.VerifyIntegrity();
      terminal = true;
      const auto settled = coordinator.RecoverIncompleteApply();
      ASSERT_TRUE(settled);
      ASSERT_EQ(settled->state, ManagerNamedApplyResultState::kSucceeded);
      EXPECT_TRUE(journal.ReadBringUpChildProof(accepted.apply_id, accepted.apply_generation, component,
                                              ManagerApplyBringUpChildPhase::kTerminal));
      EXPECT_EQ(runtime_composition.phases.size(), 3U);
      EXPECT_EQ(runtime_composition.phases.back(), ManagerSelectedRuntimeCompositionPhase::kFinalize);
      EXPECT_EQ(scene_owner.complete_calls, 1);
      EXPECT_EQ(kernel_fence.release_calls, 1);
      ASSERT_TRUE(cell_admission.last_release);
      EXPECT_EQ(cell_admission.last_release->owner_role, CellAdmissionOwnerRoleDTO::kManager);
      EXPECT_EQ(scene::EncodeLogicalSceneSnapshot(*actual_scene.LogicalSnapshot()), preserved_bytes);
      const auto after_terminal = actual_scene.VerifyIntegrity();
      EXPECT_EQ(after_terminal.transaction_intent_count, before_terminal.transaction_intent_count);
      EXPECT_EQ(after_terminal.projection_result_count, before_terminal.projection_result_count);
      // Begin/Complete never rebuild: only the explicit Stage may do so.
      EXPECT_EQ(scene_owner.stage_calls, 1);
    }
    EXPECT_EQ(runtime_composition.child_submit_calls, 1);
    EXPECT_EQ(original_source_calls, 1);
    EXPECT_EQ(scene_owner.stage_calls, 1);
    const auto composition_calls = runtime_composition.calls;
    const auto scene_begin_calls = scene_owner.begin_calls;
    const auto scene_complete_calls = scene_owner.complete_calls;
    const auto releases = kernel_fence.release_calls;
    const auto publishes = publication_store.publish_calls;
    ManagerNamedApplyCoordinator replaced(
        journal, active_plan_source, *package.bundle_source, *package.artifact_source,
        publication_store, scene_owner, nullptr, kernel_fence, cell_admission,
        runtime_composition, runtime_ports, runtime_ports, planned_maintenance,
        RuntimeEpoch("manager-runtime.replaced"), "linux",
        {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)},
        [] { return 1'000U; }, [] { return ApplyId("must-not-be-minted"); },
        [&](const auto &, const auto &) { ++authored_scene_reads; return std::vector<AddObjectRequestDTO>{}; });
    const auto observed = replaced.AcceptOrReplay(request,
        [](const auto &, const auto &, const auto &, const auto &) -> ManagerApplyBringUpPlanDTO {
          throw std::logic_error("replacement reconstructed original plan callback");
        });
    EXPECT_TRUE(observed.request_replayed);
    EXPECT_EQ(observed.state, (scenario <= 0 || scenario == 8) ? ManagerNamedApplyResultState::kSucceeded
                                          : ManagerNamedApplyResultState::kUnknownEffect);
    static_cast<void>(replaced.RecoverIncompleteApply());
    EXPECT_EQ(runtime_composition.calls, composition_calls);
    EXPECT_EQ(runtime_composition.child_submit_calls, 1);
    EXPECT_EQ(scene_owner.begin_calls, scene_begin_calls);
    EXPECT_EQ(scene_owner.complete_calls, scene_complete_calls);
    EXPECT_EQ(kernel_fence.release_calls, releases);
    EXPECT_EQ(publication_store.publish_calls, publishes);
    EXPECT_EQ(authored_scene_reads, 0);
    scene::LogicalSceneOwner reopened_scene(scene_path, admission, publication_store,
        *package.bundle_source, resolver, cell_admission);
    const auto reopened_snapshot = reopened_scene.ReadActiveSnapshot();
    ASSERT_TRUE(reopened_snapshot);
    EXPECT_EQ(scene::EncodeLogicalSceneSnapshot(*reopened_snapshot->shared()), preserved_bytes);
    EXPECT_EQ(reopened_scene.CurrentRequestOwnerEpoch(), original_scene_owner);
  }
}


void ExerciseOriginalConfigurationScene(const bool lose_stage_response, const int eligibility = 0) {
    ScopedPackageRoot package_root;
    ScopedPackageRoot state_root;
    auto empty = MaterializeRosRuntimePackage(package_root.path(), true);
    auto previous = MaterializeRosRuntimePackage(package_root.path(), false, deployment::test::kLite6ArmTargetId, kDigestOne);
    auto package = MaterializeRosRuntimePackage(package_root.path(), false, deployment::test::kLite6ArmTargetId, kDigestTwo);
    const auto prototype = ApplyIntent(*package.bundle);
    ASSERT_NE(previous.bundle->retained_bundle_id(), package.bundle->retained_bundle_id());
    ASSERT_FALSE(package.bundle->value().cell_graph.controllable_components.empty());
    ASSERT_FALSE(package.bundle->value().cell_graph.control_bindings.empty());
    ASSERT_FALSE(package.bundle->value().desired_control_configurations.empty());
    ASSERT_EQ(package.bundle->value().cell_graph.control_bindings.front().controllable_component_id,
              package.bundle->value().cell_graph.controllable_components.front().controllable_component_id);
    const auto path = state_root.path() / "original-configuration.sqlite3";
    SqliteManagerApplyJournal journal(path, package.bundle->value().cell_graph.cell_id);
    SqliteManagerApplyJournal independent_reader(path, journal.cell_id());
    ScriptedPublicationStore publication_store(PredecessorPublication(ApplyIntent(*empty.bundle)), &independent_reader);
    ScriptedCellAdmissionPort cell_admission;
    const auto admission = deployment::detail::publish_cell_scoped_canonical_empty_admission(
        package_root.path(), deployment::cell_scoped_canonical_empty_admission_filename(journal.cell_id()), *empty.bundle);
    AdoptionProjectionResolver resolver;
    const auto old_bindings = AdoptionBindings(*previous.bundle, RuntimeEpoch("predecessor-projection-epoch-1"));
    ASSERT_FALSE(old_bindings.empty());
    for (const auto &binding : old_bindings) resolver.clients.push_back(std::make_shared<AdoptionProjection>(binding));
    const auto scene_path = state_root.path() / "actual-logical-scene.sqlite3";
    scene::LogicalSceneOwner actual_scene(scene_path, admission, publication_store,
        *package.bundle_source, resolver, cell_admission);
    const auto predecessor = EstablishActualScenePredecessor(
        actual_scene, publication_store, admission, *previous.bundle, old_bindings);
    auto current = actual_scene.CurrentProjectionRevision();
    SceneTransactionRequestDTO add{
        {RequestId("actual-add"), actual_scene.CurrentRequestOwnerEpoch(), "Manager regression", std::nullopt},
        {}, current, AddObjectRequestDTO{SceneResourceId("scene-box"), BoxShapeDTO{{0.1, 0.2, 0.3}},
                                        Pose3DTO{}, CellSceneFrameRefDTO{journal.cell_id()}, {}},
        cell_admission.GetAuthoringAdmission().admission_generation};
    const auto added = actual_scene.Commit(add);
    ASSERT_TRUE(added.success) << (added.error ? added.error->message : "missing error");
    ASSERT_TRUE(added.committed_projection_revision);
    current = *added.committed_projection_revision;
    SceneTransactionRequestDTO attach{
        {RequestId("actual-attach"), actual_scene.CurrentRequestOwnerEpoch(), "Manager regression", std::nullopt},
        {}, current,
        AttachCarriedObjectRequestDTO{CarriedObjectAttachmentId("attachment.1"), SceneResourceId("scene-box"),
            ControllableComponentId("component.1"), CommandTargetId("arm"), FrameRoleId("tool_frame"),
            {CollisionBodyRoleId("finger")}, {ComponentAttachmentRevision("component-attachment-revision-1")}},
        cell_admission.GetAuthoringAdmission().admission_generation};
    attach.program_node_evidence = ProgramNodeAdmissionEvidenceDTO{
        RequestOwnerEpoch("runner.2"), ProgramRunId("program.2"), CellAdmissionClaimGeneration(9U),
        "attach", "attach:attempt.1", "scene.carried_attachment"};
    const auto attached = actual_scene.Commit(attach);
    ASSERT_TRUE(attached.success) << (attached.error ? attached.error->message : "missing error");
    cell_admission.last_release.reset();
    const auto preserved = actual_scene.LogicalSnapshot();
    ASSERT_EQ(preserved->objects.size(), 1U);
    ASSERT_EQ(preserved->carried_object_attachments.size(), 1U);
    const auto preserved_bytes = scene::EncodeLogicalSceneSnapshot(*preserved);
    const auto original_scene_owner = actual_scene.CurrentRequestOwnerEpoch();
    FixedActiveRuntimePlanSource active_plan_source(
        predecessor, BuildVerifiedRuntimeConvergencePlan(
                         *previous.bundle, *previous.artifact_source, "linux",
                         {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)}));
    ScriptedRuntimePorts runtime_ports(independent_reader);
    AggregateFencePort kernel_fence(independent_reader, predecessor);
    OriginalSceneAdapter scene_owner(actual_scene, kernel_fence);
    ScriptedRuntimeCompositionPort runtime_composition;
    kernel_fence.original_execution_host = ExecutionHostEpochDTO{
        package.bundle->value().cell_graph.control_bindings.front().provider_runtime_unit_id,
        RuntimeEpoch("process-epoch-1"), RequestOwnerEpoch("original-host-owner"),
        ContentDigest(std::string(kDigestOne)), predecessor.publication_generation};
    kernel_fence.allow_prepublication_status = true;
    ASSERT_TRUE(deployment::verify_retained_bundle_identity(*previous.bundle));
    const auto &installed_graph = previous.bundle->value().cell_graph;
    ASSERT_EQ(installed_graph.controllable_components.size(), 1U);
    ASSERT_FALSE(installed_graph.control_bindings.empty());
    const auto component = installed_graph.controllable_components.front().controllable_component_id;
    ASSERT_EQ(installed_graph.control_bindings.front().controllable_component_id, component);
    ASSERT_EQ(installed_graph.control_bindings.front().provider_runtime_unit_id,
              kernel_fence.original_execution_host->runtime_unit_id);
    const auto installed_identity =
        deployment::derive_component_runtime_identity(*previous.bundle, component);
    ASSERT_TRUE(installed_identity.has_value());
    const ManagerComponentProcessInventoryRequestDTO installed_scope{
        predecessor.request.candidate_projection_revision,
        predecessor.publication_generation,
        predecessor.durable_public_pointer_content_digest,
        kernel_fence.original_execution_host->runtime_unit_id,
        kernel_fence.original_execution_host->runtime_epoch,
        RuntimeEpoch("manager-runtime.original"), true};
    ASSERT_TRUE(installed_scope.valid());
    ASSERT_EQ(installed_scope.predecessor_projection_revision.retained_bundle_id,
              previous.bundle->retained_bundle_id());
    ASSERT_EQ(installed_scope.predecessor_projection_revision.graph_revision,
              installed_graph.graph_revision);
    // Initial observations in this controlled fake owner, not POSIX evidence:
    // the original host and this component occurrence are already present.
    // Their installed identity comes from the previous sealed product.
    runtime_ports.ReplaceProcess("process-epoch-1");
    const std::vector<ManagerComponentProcessOccurrenceDTO> installed_processes{
        {component, "fixture-original-configuration-component", 42,
         ProcessPresenceState::kPresent, core::ControllerLaunchCleanupState::kNotRequested,
         std::nullopt, installed_identity}};
    ASSERT_TRUE((ManagerComponentProcessInventoryResultDTO{
        installed_scope, installed_processes, std::nullopt}).valid());
    runtime_composition.installed_processes =
        [installed_scope, installed_processes, component, eligibility](
            const ManagerComponentProcessInventoryRequestDTO &request) {
          if (!request.valid() || request != installed_scope) {
            return Result<ManagerComponentProcessInventoryResultDTO>::Err(
                {"fixture_installed_inventory_foreign",
                 "installed process inspection does not name the fixture owner", {}});
          }
          return Result<ManagerComponentProcessInventoryResultDTO>::Ok(
              {request, installed_processes, std::nullopt, eligibility == 3
                  ? std::vector<ManagerComponentLifecycleObservationDTO>{}
                  : std::vector<ManagerComponentLifecycleObservationDTO>{{component, ComponentLifecycleGeneration(3U),
                      eligibility == 0 ? ComponentLifecycleStateDTO::kDisabled : ComponentLifecycleStateDTO::kActive}}});
        };
    for (const auto &desired : previous.bundle->value().desired_control_configurations) {
      kernel_fence.predecessor_configurations.push_back({
          desired.control_domain_id,
          ActiveControlConfigurationId("predecessor." + desired.control_domain_id.value()),
          ActiveControlConfigurationGeneration(1U)});
    }
    std::vector<std::string> events;
    scene_owner.configuration_event = [&](const auto &event) { events.push_back(event); };
    scene_owner.before_configuration_stage = [&](const auto &input) {
      events.push_back("Stage");
      const auto stored = journal.ReadIntent(input.authority.owner.apply_id, input.authority.owner.apply_generation);
      ASSERT_TRUE(stored);
      EXPECT_EQ(input.authority.persisted_manager_intent_digest, stored->intent_digest);
      EXPECT_EQ(input.authority.held_cell_claim.intent.request_payload_digest, stored->intent_digest);
      EXPECT_EQ(input.authority.held_cell_claim.intent.claim_kind, CellAdmissionClaimKindDTO::kManagerApply);
      EXPECT_TRUE(input.authority.held_cell_claim.intent.scope.resources.empty());
      EXPECT_TRUE(input.authority.held_cell_claim.intent.scope.configurations.empty());
      EXPECT_EQ(input.expected_snapshot_identity.revision, preserved->revision);
      EXPECT_EQ(input.candidate_retained_bundle, package.bundle->value());
      EXPECT_TRUE(kernel_fence.installed_configurations.empty());
      EXPECT_EQ(kernel_fence.status_calls, 0);
      EXPECT_GT(kernel_fence.reconcile_calls, 0);
      for (const auto &binding : input.native_runtime_bindings) {
        auto projection = std::make_shared<AdoptionProjection>(binding);
        projection->accept_next_rebuild = true;
        resolver.clients.push_back(std::move(projection));
      }
    };
    scene_owner.after_configuration_stage = [&] {
      if (lose_stage_response) throw std::runtime_error("lost original configuration Stage response");
    };
    runtime_composition.installed = [&](const auto &input) {
      if (input.phase == ManagerSelectedRuntimeCompositionPhase::kPrepare) {
        events.push_back("Prepare");
        EXPECT_FALSE(actual_scene.ReadMaintenanceInitialization({input.apply_id, input.apply_generation}));
        for (const auto &domain : input.configuration_fence_scope.domains) {
          if (domain.candidate_control_domain_revision) {
            kernel_fence.installed_configurations.push_back({
                domain.control_domain_id,
                ActiveControlConfigurationId("installed." + domain.control_domain_id.value()),
                ActiveControlConfigurationGeneration(1U)});
          }
        }
      } else {
        events.push_back("Finalize");
        EXPECT_EQ(publication_store.publish_calls, 0);
      }
    };
    // This checkpoint deliberately stops before Q105 context exchange. The
    // test Host reports pending verification after actual Scene completion.
    runtime_composition.pending = [&](const auto &input) {
      const auto completed = actual_scene.ReadMaintenanceInitialization({input.apply_id, input.apply_generation});
      const bool pending = input.phase == ManagerSelectedRuntimeCompositionPhase::kFinalize && completed &&
          completed->state == ProjectionMaintenanceInitializationState::kCompleted;
      if (pending) events.push_back("postpublication-verification");
      return pending;
    };
    AlwaysIdleMaintenanceGate maintenance_gate;
    ManagerPlannedMaintenanceCoordinator planned_maintenance(active_plan_source, maintenance_gate);
    int authored_scene_reads = 0;
    ManagerNamedApplyCoordinator coordinator(
        journal, active_plan_source, *package.bundle_source, *package.artifact_source,
        publication_store, scene_owner, nullptr, kernel_fence, cell_admission,
        runtime_composition, runtime_ports, runtime_ports, planned_maintenance,
        RuntimeEpoch("manager-runtime.original"), "linux",
        {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)},
        [] { return 1'000U; }, [] { return ApplyId("apply.original-configuration"); },
        [&](const auto &, const auto &) { ++authored_scene_reads; return std::vector<AddObjectRequestDTO>{}; });
    std::optional<CellAdmissionClaimDTO> parent;
    if (eligibility == 2) {
      const CellAdmissionClaimIntentDTO intent{CellAdmissionClaimKindDTO::kAuthoringMutation,
          CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("composer-owner"),
          "composer-confirmed-apply", RequestId("composer-confirmed-apply"), ContentDigest(std::string(kDigestOne)),
          cell_admission.GetAuthoringAdmission().admission_generation, {}, std::nullopt, std::nullopt};
      ASSERT_TRUE(ValidateCellAdmissionClaimIntent(intent).ok());
      const auto acquired = cell_admission.AcquireCellAdmissionClaim(intent);
      ASSERT_TRUE(acquired.ok());
      parent = acquired.value();
    }
    const ManagerNamedApplyRequestDTO request{
        prototype.request_context, package.bundle->value().deployment_artifact_id,
        package.bundle->value().deployment_revision, predecessor.request.candidate_retained_bundle_id,
        predecessor.request.candidate_graph_revision, package.bundle->retained_bundle_id(),
        prototype.prepared_activation, parent ? std::nullopt : std::optional(cell_admission.GetAuthoringAdmission().admission_generation), parent};
    // The real Scene setup already settled its add/attach admission claims.
    const auto initial_release_calls = cell_admission.release_calls;
    const auto accepted = coordinator.AcceptOrReplay(request);
    ASSERT_EQ(accepted.state, ManagerNamedApplyResultState::kPending);
    const auto stored = journal.ReadIntent(accepted.apply_id, accepted.apply_generation);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->intent_digest, ComputeManagerApplyIntentDigest(stored->intent));
    EXPECT_FALSE(journal.ReadBringUpPlan(accepted.apply_id, accepted.apply_generation));
    const auto first = coordinator.RecoverIncompleteApply();
    ASSERT_TRUE(first);
    const auto failure_detail = first->terminal_outcome && first->terminal_outcome->failure
        ? first->terminal_outcome->failure->failure_code + ": " +
              first->terminal_outcome->failure->failure_message
        : std::string("no terminal failure");
    SCOPED_TRACE(failure_detail);
    if (eligibility != 0) {
      ASSERT_TRUE(first->terminal_outcome && first->terminal_outcome->failure);
      const bool expected_active = eligibility != 3;
      EXPECT_EQ(first->state, expected_active ? ManagerNamedApplyResultState::kFailed : ManagerNamedApplyResultState::kUnknownEffect);
      EXPECT_EQ(first->terminal_outcome->failure->failure_code,
          expected_active ? "apply_requires_inactive_component" : "apply_fence_evidence_failed");
      EXPECT_EQ(first->terminal_outcome->failure->effect_disposition,
          expected_active ? ManagerApplyEffectDisposition::kKnown : ManagerApplyEffectDisposition::kUnknown);
      EXPECT_EQ(kernel_fence.acquire_calls, 0);
      EXPECT_FALSE(journal.ReadFenceAttempt(accepted.apply_id, accepted.apply_generation));
      EXPECT_EQ(runtime_ports.ensure_calls, 0);
      EXPECT_EQ(runtime_composition.calls, 0);
      EXPECT_EQ(scene_owner.stage_calls, 0);
      EXPECT_EQ(scene_owner.begin_calls, 0);
      EXPECT_EQ(publication_store.publish_calls, 0);
      EXPECT_EQ(cell_admission.release_calls, initial_release_calls + (expected_active ? 1 : 0));
      EXPECT_EQ(scene::EncodeLogicalSceneSnapshot(*actual_scene.LogicalSnapshot()), preserved_bytes);
      if (parent) {
        EXPECT_EQ(stored->intent.parent_authoring_claim, parent);
      }
      EXPECT_TRUE(coordinator.AcceptOrReplay(request).request_replayed);
      EXPECT_EQ(kernel_fence.acquire_calls, 0);
      return;
    }
    EXPECT_EQ(scene_owner.stage_calls, 1);
    EXPECT_EQ(scene_owner.stage_read_calls, 0);
    EXPECT_EQ(runtime_composition.calls, 0);
    EXPECT_EQ(scene_owner.begin_calls, 0);
    EXPECT_FALSE(actual_scene.ReadMaintenanceInitialization({accepted.apply_id, accepted.apply_generation}));
    EXPECT_EQ(publication_store.publish_calls, 0);
    if (lose_stage_response) {
      ASSERT_EQ(first->state, ManagerNamedApplyResultState::kUnknownEffect);
      ASSERT_TRUE(first->terminal_outcome && first->terminal_outcome->failure);
      EXPECT_EQ(first->terminal_outcome->failure->failure_code, "configuration_preservation_failed");
      EXPECT_EQ(first->terminal_outcome->failure->effect_disposition, ManagerApplyEffectDisposition::kUnknown);
      EXPECT_TRUE(coordinator.AcceptOrReplay(request).request_replayed);
      EXPECT_FALSE(coordinator.RecoverIncompleteApply());
      EXPECT_EQ(events, std::vector<std::string>({"Stage"}));
    } else {
      ASSERT_EQ(first->state, ManagerNamedApplyResultState::kPending);
      // Complete the fake native producer's existing accepted job; READ itself
      // performs no rebuild and this is not native-worker qualification.
      std::size_t completed_jobs = 0;
      for (const auto &projection : resolver.clients) {
        if (projection->pending_target) {
          EXPECT_EQ(projection->effect_calls, 1U);
          projection->CompletePendingRebuild();
          ++completed_jobs;
        }
      }
      ASSERT_GT(completed_jobs, 0U);
      const auto progressed = coordinator.RecoverIncompleteApply();
      ASSERT_TRUE(progressed);
      ASSERT_EQ(progressed->state, ManagerNamedApplyResultState::kPending);
      EXPECT_FALSE(progressed->terminal_outcome);
      EXPECT_EQ(events, std::vector<std::string>({"Stage", "Read", "Prepare", "Begin", "Finalize",
                                                "Complete", "postpublication-verification"}));
      EXPECT_EQ(scene_owner.stage_calls, 1);
      EXPECT_EQ(scene_owner.stage_read_calls, 1);
      EXPECT_EQ(scene_owner.begin_calls, 1);
      EXPECT_EQ(scene_owner.complete_calls, 1);
      EXPECT_EQ(publication_store.publish_calls, 1);
      const auto completed = actual_scene.ReadMaintenanceInitialization({accepted.apply_id, accepted.apply_generation});
      ASSERT_TRUE(completed);
      ASSERT_EQ(completed->state, ProjectionMaintenanceInitializationState::kCompleted);
      const auto publication = journal.ReadPublicationReceipt(accepted.apply_id, accepted.apply_generation);
      ASSERT_TRUE(publication);
      EXPECT_EQ(completed->publication_receipt, publication->publication.publication_receipt);
      EXPECT_EQ(completed->authoritative_snapshot_identity.revision, preserved->revision);
      const auto head = publication_store.ReadActiveBundlePublication(publication->publication.publication_receipt.publication_generation);
      ASSERT_TRUE(head.ok());
      EXPECT_EQ(head.value(), publication->publication.publication_receipt);
      EXPECT_EQ(actual_scene.CurrentProjectionRevision(), publication->publication.publication_receipt.request.candidate_projection_revision);
      EXPECT_EQ(runtime_composition.phases, std::vector<ManagerSelectedRuntimeCompositionPhase>({
          ManagerSelectedRuntimeCompositionPhase::kPrepare, ManagerSelectedRuntimeCompositionPhase::kFinalize,
          ManagerSelectedRuntimeCompositionPhase::kFinalize}));
    }
    const auto status = journal.ReadStatus(accepted.apply_id, accepted.apply_generation);
    ASSERT_TRUE(status);
    EXPECT_FALSE(status->bring_up_plan);
    EXPECT_TRUE(status->bring_up_child_proofs.empty());
    EXPECT_EQ(runtime_composition.child_submit_calls, 0);
    EXPECT_EQ(runtime_composition.child_read_calls, 0);
    EXPECT_EQ(scene_owner.ordinary_begin_calls, 0);
    EXPECT_EQ(scene_owner.ordinary_install_calls, 0);
    EXPECT_EQ(scene_owner.ordinary_complete_calls, 0);
    EXPECT_EQ(kernel_fence.release_calls, 0);
    EXPECT_FALSE(cell_admission.last_release);
    EXPECT_EQ(authored_scene_reads, 0);
    const auto snapshot = actual_scene.ReadActiveSnapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(scene::EncodeLogicalSceneSnapshot(*snapshot->shared()), preserved_bytes);
    EXPECT_EQ(actual_scene.CurrentRequestOwnerEpoch(), original_scene_owner);
    const auto held_claim = cell_admission.GetCellAdmissionRecoveryReceipt({
        {CellAdmissionClaimKindDTO::kManagerApply, CellAdmissionOwnerRoleDTO::kManager,
         stored->intent.request_context.request_owner_epoch, stored->intent.apply_id.value(),
         stored->intent.request_context.request_id, stored->intent_digest,
         *stored->intent.expected_admission_generation, {}, std::nullopt, std::nullopt},
        std::nullopt, std::nullopt});
    ASSERT_TRUE(held_claim.ok());
    EXPECT_EQ(held_claim.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
    const auto prior_events = events;
    const auto prior_composition_calls = runtime_composition.calls;
    const auto prior_reconcile_calls = kernel_fence.reconcile_calls;
    const auto prior_lookup_calls = cell_admission.lookup_intents.size();
    ManagerNamedApplyCoordinator replacement(
        journal, active_plan_source, *package.bundle_source, *package.artifact_source,
        publication_store, scene_owner, nullptr, kernel_fence, cell_admission,
        runtime_composition, runtime_ports, runtime_ports, planned_maintenance,
        RuntimeEpoch("manager-runtime.replacement"), "linux",
        {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)},
        [] { return 1'000U; }, [] { return ApplyId("must-not-be-minted"); },
        [&](const auto &, const auto &) { ++authored_scene_reads; return std::vector<AddObjectRequestDTO>{}; });
    const auto replay = replacement.AcceptOrReplay(request);
    EXPECT_TRUE(replay.request_replayed);
    EXPECT_EQ(replay.state, lose_stage_response ? ManagerNamedApplyResultState::kUnknownEffect
                                             : ManagerNamedApplyResultState::kPending);
    const auto recovered = replacement.RecoverIncompleteApply();
    if (lose_stage_response) {
      EXPECT_FALSE(recovered);
    } else {
      ASSERT_TRUE(recovered);
      EXPECT_EQ(recovered->state, ManagerNamedApplyResultState::kUnknownEffect);
      ASSERT_TRUE(recovered->terminal_outcome && recovered->terminal_outcome->failure);
      EXPECT_EQ(recovered->terminal_outcome->failure->failure_code, "planned_maintenance_interrupted");
    }
    EXPECT_EQ(events, prior_events);
    EXPECT_EQ(runtime_composition.calls, prior_composition_calls);
    EXPECT_EQ(kernel_fence.reconcile_calls, prior_reconcile_calls);
    EXPECT_EQ(cell_admission.lookup_intents.size(), prior_lookup_calls);
    ASSERT_TRUE(held_claim.value().matched_claim);
    const auto still_held = cell_admission.GetCellAdmissionRecoveryReceipt({
        held_claim.value().matched_claim->intent, held_claim.value().matched_claim, std::nullopt});
    ASSERT_TRUE(still_held.ok());
    EXPECT_EQ(still_held.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
    EXPECT_EQ(still_held.value().matched_claim, held_claim.value().matched_claim);
    EXPECT_EQ(kernel_fence.release_calls, 0);
    EXPECT_FALSE(cell_admission.last_release);
}

TEST(ManagerNamedApplyCoordinator,
     OriginalConfigurationApplyPreservesActualSceneWithoutChildrenAndRetainsPendingExchange) {
  ExerciseOriginalConfigurationScene(false);
}

TEST(ManagerNamedApplyCoordinator,
     OriginalConfigurationStageUncertaintyKeepsParentHeldWithoutReplacementRecapture) {
  ExerciseOriginalConfigurationScene(true);
}

} // namespace

namespace {

[[nodiscard]] ManagerComponentProcessOccurrenceDTO NoneInventoryOccurrence(
    const deployment::RetainedBundle &bundle) {
  const auto &bindings = bundle.value().cell_graph.control_bindings;
  const auto gripper = std::find_if(bindings.begin(), bindings.end(),
      [](const auto &binding) { return binding.capability_id == CapabilityId("tool.gripper.command"); });
  const auto &binding = gripper == bindings.end() ? bindings.front() : *gripper;
  return {binding.controllable_component_id, {}, 0, ProcessPresenceState::kAbsent,
          ControllerLaunchCleanupState::kNotRequested, std::nullopt,
          deployment::derive_component_runtime_identity(bundle, binding.controllable_component_id),
          ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal,
          ManagerPneumaticNoneProcessOwnerDTO{binding.command_target_id, binding.control_binding_id,
                                             binding.control_binding_revision}};
}

[[nodiscard]] ManagerComponentProcessInventoryRequestDTO NoneInventoryRequest(
    const deployment::RetainedBundle &bundle) {
  const auto publication = PredecessorPublication(ApplyIntent(bundle));
  return {publication.request.candidate_projection_revision, publication.publication_generation,
          publication.durable_public_pointer_content_digest,
          bundle.value().cell_graph.control_bindings.front().provider_runtime_unit_id,
          RuntimeEpoch("none-host.original"), RuntimeEpoch("none-manager.original")};
}

} // namespace

TEST(ManagerNamedApplyCoordinator, NoneProcessOwnerRequiresExactImmutablePneumaticComponentClosure) {
  ScopedPackageRoot root;
  const auto none = MaterializeNoneInventoryPackage(root.path());
  const auto request = NoneInventoryRequest(*none.bundle);
  const auto exact = NoneInventoryOccurrence(*none.bundle);
  ASSERT_TRUE(request.valid());
  ASSERT_TRUE(exact.valid());
  ASSERT_TRUE(ValidateManagerPneumaticNoneProcessOwner(*none.bundle, request, exact).ok());
  EXPECT_TRUE(exact.launch_identity.empty());
  EXPECT_EQ(exact.leader_pid, 0);
  EXPECT_EQ(exact.process_presence, ProcessPresenceState::kAbsent);
  EXPECT_FALSE(exact.receipt);
  const auto reject = [&](auto row) {
    EXPECT_FALSE(ValidateManagerPneumaticNoneProcessOwner(*none.bundle, request, row).ok());
  };
  auto foreign = exact;
  foreign.installed_runtime_identity = ContentDigest(std::string(kDigestOne));
  ASSERT_TRUE(foreign.valid());
  reject(foreign);
  foreign = exact;
  foreign.pneumatic_none_owner->command_target_id = CommandTargetId("foreign-target");
  ASSERT_TRUE(foreign.valid());
  reject(foreign);
  foreign = exact;
  foreign.pneumatic_none_owner->control_binding_id = ControlBindingId("foreign-binding");
  ASSERT_TRUE(foreign.valid());
  reject(foreign);
  foreign = exact;
  foreign.pneumatic_none_owner->control_binding_revision = ControlBindingRevision("foreign-revision");
  ASSERT_TRUE(foreign.valid());
  reject(foreign);
  auto foreign_source = request;
  foreign_source.runtime_unit_id = RuntimeUnitId("foreign-runtime");
  ASSERT_TRUE(foreign_source.valid());
  EXPECT_FALSE(ValidateManagerPneumaticNoneProcessOwner(*none.bundle, foreign_source, exact).ok());
  const auto commanded = MaterializeNoneInventoryPackage(root.path(), "commanded");
  const auto position = MaterializeNoneInventoryPackage(root.path(), "commanded", "position_commanded");
  // Each negative first matches its own real sealed runtime identity and graph:
  // the strict output/actuation check, rather than a foreign digest, refuses it.
  for (const auto &package : {&commanded, &position}) {
    const auto physical_row = NoneInventoryOccurrence(*package->bundle);
    ASSERT_TRUE(physical_row.valid());
    EXPECT_FALSE(ValidateManagerPneumaticNoneProcessOwner(
        *package->bundle, NoneInventoryRequest(*package->bundle), physical_row).ok());
  }
  const auto arm_and_none = MaterializeNoneInventoryPackage(root.path(), "none", "pneumatic", false, true);
  const auto mixed_row = NoneInventoryOccurrence(*arm_and_none.bundle);
  ASSERT_TRUE(mixed_row.valid());
  const auto mixed_config = deployment::ros2_control_execution_runtime_contract::require_component_configuration(
      *arm_and_none.bundle, NoneInventoryRequest(*arm_and_none.bundle).runtime_unit_id,
      mixed_row.controllable_component_id);
  ASSERT_FALSE(mixed_config.motion_target_limits.empty());
  ASSERT_EQ(mixed_config.gripper_target_configurations.front().output_mode, GripperOutputMode::kNone);
  EXPECT_FALSE(ValidateManagerPneumaticNoneProcessOwner(
      *arm_and_none.bundle, NoneInventoryRequest(*arm_and_none.bundle), mixed_row).ok());
  const auto arm = MaterializeRosRuntimePackage(root.path());
  auto arm_row = NoneInventoryOccurrence(*arm.bundle);
  ASSERT_TRUE(arm_row.valid());
  EXPECT_FALSE(ValidateManagerPneumaticNoneProcessOwner(
      *arm.bundle, NoneInventoryRequest(*arm.bundle), arm_row).ok());
}

TEST(ManagerNamedApplyCoordinator, NoneFeedbackAliasRequiresItsExactVerifiedCommandOwner) {
  ScopedPackageRoot root;
  const auto package = MaterializeNoneInventoryPackage(root.path(), "none", "pneumatic", false, false, true);
  const auto row = NoneInventoryOccurrence(*package.bundle);
  const auto request = NoneInventoryRequest(*package.bundle);
  const auto &bindings = package.bundle->value().cell_graph.control_bindings;
  ASSERT_EQ(bindings.size(), 2U);
  const auto command = std::find_if(bindings.begin(), bindings.end(),
      [](const auto &entry) { return entry.capability_id == CapabilityId("tool.gripper.command"); });
  const auto feedback = std::find_if(bindings.begin(), bindings.end(),
      [](const auto &entry) { return entry.capability_id == CapabilityId("tool.gripper.feedback"); });
  ASSERT_NE(command, bindings.end());
  ASSERT_NE(feedback, bindings.end());
  EXPECT_NE(command->control_binding_id, feedback->control_binding_id);
  ASSERT_NE(command->control_binding_revision, feedback->control_binding_revision);
  EXPECT_EQ(command->command_target_id, feedback->command_target_id);
  EXPECT_EQ(command->resource_bindings, feedback->resource_bindings);
  EXPECT_EQ(command->provider_instance_id, feedback->provider_instance_id);
  EXPECT_EQ(command->provider_manifest_revision, feedback->provider_manifest_revision);
  EXPECT_EQ(command->provider_runtime_unit_id, feedback->provider_runtime_unit_id);
  ASSERT_TRUE(row.valid());
  const auto validated = ValidateManagerPneumaticNoneProcessOwner(*package.bundle, request, row);
  ASSERT_TRUE(validated.ok()) << validated.error().message;
  EXPECT_EQ(row.process_presence, ProcessPresenceState::kAbsent);
  EXPECT_EQ(row.leader_pid, 0);
  EXPECT_TRUE(row.launch_identity.empty());
  EXPECT_FALSE(row.receipt);
  // Even a real, independently qualified feedback binding cannot substitute
  // for the exact command-binding proof of the installed local owner.
  auto forged = row;
  forged.pneumatic_none_owner->control_binding_id = feedback->control_binding_id;
  forged.pneumatic_none_owner->control_binding_revision = feedback->control_binding_revision;
  ASSERT_TRUE(forged.valid());
  EXPECT_FALSE(ValidateManagerPneumaticNoneProcessOwner(*package.bundle, request, forged).ok());
  forged = row;
  forged.pneumatic_none_owner->control_binding_revision = feedback->control_binding_revision;
  ASSERT_TRUE(forged.valid());
  EXPECT_FALSE(ValidateManagerPneumaticNoneProcessOwner(*package.bundle, request, forged).ok());
  forged = row;
  forged.pneumatic_none_owner->command_target_id = CommandTargetId("foreign-progress-target");
  ASSERT_TRUE(forged.valid());
  EXPECT_FALSE(ValidateManagerPneumaticNoneProcessOwner(*package.bundle, request, forged).ok());
}

TEST(ManagerNamedApplyCoordinator, NoneInstalledOwnerRetainReplaceRemoveUsesExactFenceWithoutChildProcessEffects) {
  for (const int variant : {0, 1, 2, 3}) {
    SCOPED_TRACE(variant);
    ScopedPackageRoot root;
    ScopedPackageRoot state;
    const bool feedback_alias = variant == 3;
    auto previous = MaterializeNoneInventoryPackage(root.path(), "none", "pneumatic", false, false, feedback_alias);
    auto candidate = (variant == 0 || feedback_alias)
        ? MaterializeNoneInventoryPackage(root.path(), "none", "pneumatic", false, false, feedback_alias)
        : MaterializeNoneInventoryPackage(root.path(), variant == 1 ? "commanded" : "none", "pneumatic", variant == 2);
    const auto row = NoneInventoryOccurrence(*previous.bundle);
    const auto inventory = NoneInventoryRequest(*previous.bundle);
    ASSERT_TRUE(ValidateManagerPneumaticNoneProcessOwner(*previous.bundle, inventory, row).ok());
    const auto delta = deployment::derive_apply_component_delta(*previous.bundle, *candidate.bundle);
    ASSERT_EQ(delta.components.size(), 1U);
    EXPECT_EQ(delta.components.front().disposition, (variant == 0 || feedback_alias) ? deployment::ApplyComponentDisposition::kRetain
        : variant == 1 ? deployment::ApplyComponentDisposition::kReplace : deployment::ApplyComponentDisposition::kRemove);
    const auto continuity = VerifyNativeExecutionHostContinuity(
        *previous.bundle, *candidate.bundle, *candidate.artifact_source);
    ASSERT_TRUE(continuity.ok()) << continuity.error().message;
    const auto predecessor = PredecessorPublication(ApplyIntent(*previous.bundle));
    SqliteManagerApplyJournal journal(state.path() / "none.sqlite3", previous.bundle->value().cell_graph.cell_id);
    SqliteManagerApplyJournal reader(state.path() / "none.sqlite3", journal.cell_id());
    ScriptedPublicationStore publication(predecessor, &reader);
    FixedActiveRuntimePlanSource plan(predecessor, BuildVerifiedRuntimeConvergencePlan(
        *previous.bundle, *previous.artifact_source, "linux",
        {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)}));
    ScriptedRuntimePorts runtime(reader);
    runtime.ReplaceProcess(inventory.runtime_epoch.value()); // The existing host, not a gripper child.
    runtime.report_available = false; // Stop at actual host convergence, before Scene effects.
    AggregateFencePort fence(reader, predecessor);
    fence.original_execution_host = ExecutionHostEpochDTO{inventory.runtime_unit_id, inventory.runtime_epoch,
        RequestOwnerEpoch("none-host.owner"), ContentDigest(std::string(kDigestOne)), predecessor.publication_generation};
    const auto expected_fence_scope = deployment::derive_apply_configuration_fence_scope(
        *previous.bundle, *candidate.bundle);
    ASSERT_TRUE(expected_fence_scope.valid());
    for (const auto &domain : expected_fence_scope.domains) {
      if (!domain.predecessor_control_domain_revision) continue;
      const auto &desired = previous.bundle->value().desired_control_configurations;
      const auto configured = std::find_if(desired.begin(), desired.end(), [&](const auto &value) {
        return value.control_domain_id == domain.control_domain_id &&
            value.control_domain_revision == *domain.predecessor_control_domain_revision;
      });
      ASSERT_TRUE(configured != desired.end());
      // Existing scripted host observation grammar, qualified by this exact
      // predecessor's configured domain; no candidate/None-mode inference.
      fence.predecessor_configurations.push_back({
          configured->control_domain_id,
          ActiveControlConfigurationId("predecessor." + configured->control_domain_id.value()),
          ActiveControlConfigurationGeneration(1U)});
    }
    const ConfigurationFenceAdmissionQueryDTO admission_query{
        expected_fence_scope, inventory.manager_runtime_epoch};
    ASSERT_TRUE(admission_query.valid());
    ASSERT_TRUE(fence.ObserveAdmission(admission_query).matches(admission_query));
    MaintenanceSceneOwner scene_owner(predecessor.request.candidate_projection_revision);
    ScriptedRuntimeCompositionPort composition;
    composition.installed_processes = [inventory, row](const auto &request) {
      auto expected = inventory;
      expected.include_component_lifecycles = true;
      if (request != expected) return Result<ManagerComponentProcessInventoryResultDTO>::Err(
          {"fixture_foreign_inventory", "wrong original request", {}});
      return Result<ManagerComponentProcessInventoryResultDTO>::Ok({request, {row}, std::nullopt, {{row.controllable_component_id, ComponentLifecycleGeneration(1U), ComponentLifecycleStateDTO::kDisabled}}});
    };
    ScriptedCellAdmissionPort admission;
    AlwaysIdleMaintenanceGate gate;
    ManagerPlannedMaintenanceCoordinator maintenance(plan, gate);
    ManagerNamedApplyCoordinator coordinator(journal, plan, *candidate.bundle_source, *candidate.artifact_source,
        publication, scene_owner, nullptr, fence, admission, composition, runtime, runtime, maintenance,
        inventory.manager_runtime_epoch, "linux",
        {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)},
        [] { return 1'000U; }, [] { return ApplyId("apply.none-inventory"); },
        [](const auto &, const auto &) { return std::vector<AddObjectRequestDTO>{}; });
    auto prototype = ApplyIntent(*candidate.bundle);
    const ManagerNamedApplyRequestDTO request{prototype.request_context,
        candidate.bundle->value().deployment_artifact_id, candidate.bundle->value().deployment_revision,
        previous.bundle->retained_bundle_id(), previous.bundle->value().cell_graph.graph_revision,
        candidate.bundle->retained_bundle_id(), prototype.prepared_activation,
        admission.GetAuthoringAdmission().admission_generation, std::nullopt};
    const auto accepted = coordinator.AcceptOrReplay(request);
    ASSERT_EQ(accepted.state, ManagerNamedApplyResultState::kPending);
    const auto progress = coordinator.RecoverIncompleteApply();
    ASSERT_TRUE(progress);
    ASSERT_EQ(progress->state, ManagerNamedApplyResultState::kPending)
        << (progress->terminal_outcome && progress->terminal_outcome->failure
            ? progress->terminal_outcome->failure->failure_message : "no failure");
    const auto attempt = journal.ReadFenceAttempt(accepted.apply_id, accepted.apply_generation);
    ASSERT_TRUE(attempt);
    EXPECT_EQ(attempt->fence_intent.scope,
              deployment::derive_apply_configuration_fence_scope(*previous.bundle, *candidate.bundle));
    EXPECT_EQ(fence.acquire_calls, 1);
    EXPECT_EQ(runtime.start_effect_calls, 0);
    EXPECT_EQ(runtime.stop_calls, 0);
    EXPECT_EQ(composition.component_stop_calls, 0);
    EXPECT_EQ(composition.component_stop_read_calls, 0);
    EXPECT_EQ(composition.child_submit_calls, 0);
    EXPECT_EQ(composition.child_read_calls, 0);
    EXPECT_EQ(composition.calls, 0);
    EXPECT_EQ(scene_owner.begin_calls, 0);
    EXPECT_EQ(publication.publish_calls, 0);
    EXPECT_EQ(row.process_presence, ProcessPresenceState::kAbsent);
    EXPECT_FALSE(row.receipt);
    EXPECT_TRUE(coordinator.AcceptOrReplay(request).request_replayed);
    EXPECT_EQ(fence.acquire_calls, 1);
  }
}

TEST(ManagerNamedApplyCoordinator, NoneInventoryForeignEpochOrBindingStopsBeforeFenceAndEffects) {
  for (const int variant : {0, 1, 2, 3, 4}) {
    SCOPED_TRACE(variant);
    ScopedPackageRoot root;
    ScopedPackageRoot state;
    const auto package = MaterializeNoneInventoryPackage(root.path());
    const auto inventory = NoneInventoryRequest(*package.bundle);
    const auto exact = NoneInventoryOccurrence(*package.bundle);
    ASSERT_TRUE(ValidateManagerPneumaticNoneProcessOwner(*package.bundle, inventory, exact).ok());
    const auto predecessor = PredecessorPublication(ApplyIntent(*package.bundle));
    SqliteManagerApplyJournal journal(state.path() / "foreign-none.sqlite3", package.bundle->value().cell_graph.cell_id);
    SqliteManagerApplyJournal reader(state.path() / "foreign-none.sqlite3", journal.cell_id());
    ScriptedPublicationStore publication(predecessor, &reader);
    FixedActiveRuntimePlanSource plan(predecessor, BuildVerifiedRuntimeConvergencePlan(
        *package.bundle, *package.artifact_source, "linux",
        {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)}));
    ScriptedRuntimePorts runtime(reader);
    AggregateFencePort fence(reader, predecessor);
    fence.original_execution_host = ExecutionHostEpochDTO{inventory.runtime_unit_id, inventory.runtime_epoch,
        RequestOwnerEpoch("none-host.owner"), ContentDigest(std::string(kDigestOne)), predecessor.publication_generation};
    MaintenanceSceneOwner scene_owner(predecessor.request.candidate_projection_revision);
    ScriptedRuntimeCompositionPort composition;
    composition.installed_processes = [inventory, exact, variant](const auto &request) {
      auto response = inventory;
      response.include_component_lifecycles = request.include_component_lifecycles;
      auto row = exact;
      if (variant == 0) response.runtime_epoch = RuntimeEpoch("foreign-host");
      if (variant == 1) response.manager_runtime_epoch = RuntimeEpoch("foreign-manager");
      if (variant == 2) row.pneumatic_none_owner->control_binding_revision = ControlBindingRevision("foreign-binding");
      if (variant == 3) row.installed_runtime_identity = ContentDigest(std::string(kDigestOne));
      if (variant == 4) response.runtime_unit_id = RuntimeUnitId("foreign-source");
      auto expected = inventory;
      expected.include_component_lifecycles = true;
      EXPECT_EQ(request, expected);
      return Result<ManagerComponentProcessInventoryResultDTO>::Ok({response, {row}, std::nullopt});
    };
    ScriptedCellAdmissionPort admission;
    AlwaysIdleMaintenanceGate gate;
    ManagerPlannedMaintenanceCoordinator maintenance(plan, gate);
    ManagerNamedApplyCoordinator coordinator(journal, plan, *package.bundle_source, *package.artifact_source,
        publication, scene_owner, nullptr, fence, admission, composition, runtime, runtime, maintenance,
        inventory.manager_runtime_epoch, "linux",
        {std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)},
        [] { return 1'000U; }, [] { return ApplyId("apply.foreign-none"); },
        [](const auto &, const auto &) { return std::vector<AddObjectRequestDTO>{}; });
    const auto prototype = ApplyIntent(*package.bundle);
    const ManagerNamedApplyRequestDTO request{prototype.request_context,
        package.bundle->value().deployment_artifact_id, package.bundle->value().deployment_revision,
        package.bundle->retained_bundle_id(), package.bundle->value().cell_graph.graph_revision,
        package.bundle->retained_bundle_id(), prototype.prepared_activation,
        admission.GetAuthoringAdmission().admission_generation, std::nullopt};
    ASSERT_EQ(coordinator.AcceptOrReplay(request).state, ManagerNamedApplyResultState::kPending);
    const auto result = coordinator.RecoverIncompleteApply();
    ASSERT_TRUE(result && result->terminal_outcome && result->terminal_outcome->failure);
    EXPECT_EQ(result->state, ManagerNamedApplyResultState::kUnknownEffect);
    EXPECT_EQ(result->terminal_outcome->failure->failure_code, "apply_fence_evidence_failed");
    EXPECT_EQ(fence.acquire_calls, 0);
    EXPECT_EQ(runtime.ensure_calls, 0);
    EXPECT_EQ(composition.component_stop_calls, 0);
    EXPECT_EQ(composition.component_stop_read_calls, 0);
    EXPECT_EQ(composition.calls, 0);
    EXPECT_EQ(scene_owner.begin_calls, 0);
    EXPECT_EQ(publication.publish_calls, 0);
  }
}

TEST(ManagerNamedApplyCoordinator, ActiveChangedComponentRefusesBeforeFenceSceneOrRuntimeWithDirectOrAuthoringClaim) {
  ExerciseOriginalConfigurationScene(false, 1);
  ExerciseOriginalConfigurationScene(false, 2);
}

TEST(ManagerNamedApplyCoordinator, MissingChangedComponentLifecycleIsNotClaimedAsKnownActiveRefusal) {
  ExerciseOriginalConfigurationScene(false, 3);
}

} // namespace manyforge::component_manager
