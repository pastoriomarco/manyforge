#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_deployment/scenario_apply_candidate_source.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_headless/assistant_mode_projection.hpp>
#include <manyforge_headless/event_historian.hpp>
#include <manyforge_headless/node_environment_projection.hpp>
#include <manyforge_headless/operator_alarm_owner.hpp>
#include <manyforge_headless/program_apply_owner.hpp>
#include <manyforge_headless/runtime_snapshot.hpp>
#include <manyforge_headless/source_model_projection.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::headless {

inline constexpr std::string_view kHeadlessApiMutationResponseSchemaVersion =
    "manyforge.headless_api_mutation_response.v1";
inline constexpr std::string_view kViewerModelAssetSchemaVersion =
    "manyforge.viewer_model_asset.v1";
inline constexpr std::string_view kScenarioProgramSelectionSchemaVersion =
    "manyforge.scenario_program_selection.v2";
inline constexpr std::size_t kMaximumViewerModelResourceUriBytes = 4096U;

class HeadlessApiOwnerLock;

/// Immutable byte view into one exact active retained viewer product.
///
/// The shared model keeps the selected immutable closure alive after the core
/// mutex is released and even if a later request observes a new Apply. The
/// view carries its original bundle/product identities, so a response can
/// never relabel old bytes as the successor selection.
class ViewerModelAsset final {
public:
  ViewerModelAsset(const ViewerModelAsset &) = default;
  ViewerModelAsset &operator=(const ViewerModelAsset &) = default;
  ViewerModelAsset(ViewerModelAsset &&) noexcept = default;
  ViewerModelAsset &operator=(ViewerModelAsset &&) noexcept = default;
  ~ViewerModelAsset() = default;

  [[nodiscard]] std::string_view asset_kind() const noexcept {
    return resource_index_.has_value() ? "resource" : "urdf";
  }
  [[nodiscard]] std::string_view cell_id() const noexcept { return cell_id_; }
  [[nodiscard]] std::string_view graph_revision() const noexcept {
    return graph_revision_;
  }
  [[nodiscard]] const manyforge::core::RetainedBundleId &
  retained_bundle_id() const noexcept {
    return model_->query().retained_bundle_id;
  }
  [[nodiscard]] const manyforge::core::ProductId &product_id() const noexcept {
    return model_->query().product_id;
  }
  [[nodiscard]] const manyforge::core::ProductContractId &
  product_contract_id() const noexcept {
    return model_->product_contract_id();
  }
  [[nodiscard]] const manyforge::core::ProductContractRevision &
  product_contract_revision() const noexcept {
    return model_->product_contract_revision();
  }
  [[nodiscard]] const manyforge::core::ContentDigest &
  viewer_model_revision() const noexcept {
    return viewer_model_revision_;
  }
  [[nodiscard]] std::optional<std::string_view> logical_uri() const noexcept {
    if (!resource_index_.has_value()) {
      return std::nullopt;
    }
    return model_->resources()[*resource_index_].logical_uri();
  }
  [[nodiscard]] std::string_view media_type() const noexcept {
    if (!resource_index_.has_value()) {
      return model_->urdf_artifact_ref().media_type;
    }
    return model_->resources()[*resource_index_].media_type();
  }
  [[nodiscard]] std::uint64_t byte_size() const noexcept {
    if (!resource_index_.has_value()) {
      return model_->urdf_artifact_ref().byte_size;
    }
    return model_->resources()[*resource_index_].byte_size();
  }
  [[nodiscard]] const manyforge::core::ContentDigest &
  content_digest() const noexcept {
    if (!resource_index_.has_value()) {
      return model_->urdf_artifact_ref().content_digest;
    }
    return model_->resources()[*resource_index_].content_digest();
  }
  [[nodiscard]] std::string_view bytes() const noexcept {
    if (!resource_index_.has_value()) {
      return model_->urdf_bytes();
    }
    return model_->resources()[*resource_index_].bytes();
  }

private:
  ViewerModelAsset(
      std::shared_ptr<const manyforge::deployment::VerifiedRetainedViewerModel>
          model,
      std::string cell_id, std::string graph_revision,
      manyforge::core::ContentDigest viewer_model_revision,
      std::optional<std::size_t> resource_index)
      : model_(std::move(model)), cell_id_(std::move(cell_id)),
        graph_revision_(std::move(graph_revision)),
        viewer_model_revision_(std::move(viewer_model_revision)),
        resource_index_(resource_index) {}

  std::shared_ptr<const manyforge::deployment::VerifiedRetainedViewerModel>
      model_;
  std::string cell_id_;
  std::string graph_revision_;
  manyforge::core::ContentDigest viewer_model_revision_;
  std::optional<std::size_t> resource_index_;

  friend class HeadlessApiCore;
};

struct HeadlessApiCoreConfig final {
  std::filesystem::path publication_root;
  std::filesystem::path package_root;
  std::filesystem::path node_catalog_path;
  std::filesystem::path alarm_catalog_path;
  std::filesystem::path historian_database_path;
  std::string cell_id;
  std::set<std::string> resource_ids;
  std::string owner_id;
  std::string owner_epoch;
  RuntimeSnapshotFreshnessPolicy freshness_policy;
  std::size_t historian_queue_capacity{kDefaultHistorianQueueCapacity};
  std::size_t historian_record_retention{kDefaultHistorianRecordRetention};
  std::size_t mutation_replay_capacity{4096U};
};

struct HeadlessApiCoreClocks final {
  std::function<std::int64_t()> wall_time_ns;
  std::function<std::int64_t()> monotonic_time_ns;
  std::function<std::string()> wall_time_text;
};

/// C++ semantic core hosted by the independently restartable FastAPI process.
/// HTTP and CLI layers consume canonical values and cannot instantiate an
/// alternate alarm reducer or historian.
class HeadlessApiCore final {
public:
  explicit HeadlessApiCore(
      HeadlessApiCoreConfig config,
      HeadlessApiCoreClocks clocks = HeadlessApiCoreClocks{});
  ~HeadlessApiCore();

  HeadlessApiCore(const HeadlessApiCore &) = delete;
  HeadlessApiCore &operator=(const HeadlessApiCore &) = delete;

  [[nodiscard]] manyforge::content_identity::CanonicalValue RuntimeSnapshot();
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  NodeEnvironmentProjection();
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  SourceModelProjection();
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  ScenarioCatalogProjection();
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  ScenarioProgramSelection(std::string_view scenario_id);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  PrepareProgramApply(std::string_view scenario_id,
                      std::string_view expected_scenario_catalog_revision,
                      std::string_view canonical_program_bytes);
  [[nodiscard]] manyforge::content_identity::CanonicalValue CommitProgramApply(
      std::string_view canonical_preparation_bytes,
      const std::optional<std::string> &expected_applied_record_digest);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  AppliedProgramSelection();
  [[nodiscard]] ViewerModelAsset ViewerModelUrdf();
  [[nodiscard]] ViewerModelAsset
  ViewerModelResource(std::string_view expected_retained_bundle_id,
                      std::string_view logical_uri);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  AssistantModeIndex();
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  AssistantModeProjection(std::string_view mode_id);
  [[nodiscard]] manyforge::content_identity::CanonicalValue AlarmSnapshot(
      std::optional<std::size_t> limit = 32U,
      const std::set<AlarmLifecycleState> &states = {},
      AlarmSnapshotOrdering ordering = AlarmSnapshotOrdering::kDefault);
  [[nodiscard]] manyforge::content_identity::CanonicalValue AlarmMetrics();
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  AcknowledgeAlarm(std::string condition_id, std::string activation_id,
                   std::uint64_t expected_record_revision,
                   AlarmCallerAttribution caller,
                   std::optional<std::string> reason, bool confirmed);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  AcknowledgeAllAlarms(std::vector<AlarmAcknowledgementMember> members,
                       std::uint64_t expected_generation,
                       AlarmCallerAttribution caller,
                       std::optional<std::string> reason, bool confirmed);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  HistoryPage(const HistorianQuery &query);
  [[nodiscard]] manyforge::content_identity::CanonicalValue HistorianHealth();
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  ReconcileHistorianHealth(std::uint64_t expected_health_generation,
                           AlarmCallerAttribution caller);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  IngestOperationEvent(
      std::string source_owner_epoch, std::uint64_t source_sequence,
      std::string record_kind, std::optional<std::string> request_id,
      std::string operation_id, std::uint64_t operation_revision,
      std::int64_t occurred_at_ns, std::string occurred_at_wall,
      bool effect_pending,
      manyforge::content_identity::CanonicalValue journal_payload,
      std::string payload_digest);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  CompleteOperationEventSnapshot(std::string source_owner_epoch,
                                 std::uint64_t through_source_sequence);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  MarkOperationEventSourceUnknown(std::string source_owner_epoch);
  void Shutdown();

private:
  struct AdapterCursor final {
    std::string source_id;
    std::string domain;
    std::string adapter_id;
    std::string source_epoch;
    std::uint64_t source_epoch_order{};
    std::uint64_t source_sequence{};
    std::string last_condition_fingerprint;
    std::optional<AlarmConditionUpdate> last_update;
  };

  struct ReplayEntry final {
    std::string request_digest;
    manyforge::content_identity::CanonicalValue response;
  };

  [[nodiscard]] HeadlessRuntimeSnapshot RefreshLocked();
  [[nodiscard]] std::shared_ptr<const manyforge::deployment::RetainedBundle>
  VerifiedActiveBundleLocked(std::string_view retained_bundle_id,
                             std::string_view graph_revision,
                             std::string_view invalid_code,
                             std::string_view unavailable_code);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  NodeEnvironmentProjectionLocked(const HeadlessRuntimeSnapshot &runtime);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  SourceModelProjectionLocked(const HeadlessRuntimeSnapshot &runtime);
  [[nodiscard]] ViewerModelAsset ViewerModelAssetLocked(
      const HeadlessRuntimeSnapshot &runtime,
      std::optional<std::string_view> expected_retained_bundle_id,
      std::optional<std::string_view> logical_uri);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  AssistantModeIndexLocked(const HeadlessRuntimeSnapshot &runtime);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  AssistantModeProjectionLocked(const HeadlessRuntimeSnapshot &runtime,
                                std::string_view mode_id);
  void
  RefreshRuntimeHealthAdapterLocked(const HeadlessRuntimeSnapshot &runtime);
  void RefreshSceneAdapterLocked(const HeadlessRuntimeSnapshot &runtime);
  void RegisterOperationEpochLocked(std::string source_owner_epoch);
  void ReconcileOperationFaultLocked(
      const manyforge::content_identity::CanonicalValue &journal_payload,
      std::string_view record_kind, std::string_view request_id,
      std::uint64_t native_sequence, std::string_view payload_digest,
      std::int64_t occurred_at_ns, std::string_view occurred_at_wall,
      std::string_view raw_event_id);
  void RegisterAdapterEpochLocked(AdapterCursor &cursor,
                                  std::string source_epoch);
  [[nodiscard]] AlarmMutation SubmitAdapterUpdateLocked(
      AdapterCursor &cursor, std::string definition_id, AlarmSubject subject,
      bool active, manyforge::content_identity::CanonicalValue details,
      manyforge::content_identity::CanonicalValue correlations);
  [[nodiscard]] std::vector<std::string>
  DeliverFactsLocked(const std::vector<AlarmLifecycleFact> &facts);
  void SynchronizeHistorianHealthLocked();
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  MutationResponse(const AlarmMutation &mutation,
                   const std::vector<std::string> &audit_event_ids);
  [[nodiscard]] std::optional<manyforge::content_identity::CanonicalValue>
  LookupReplayLocked(std::string_view request_key,
                     std::string_view request_digest) const;
  void RequireReplayCapacityLocked() const;
  void StoreReplayLocked(std::string request_key, std::string request_digest,
                         manyforge::content_identity::CanonicalValue response);

  HeadlessApiCoreConfig config_;
  HeadlessApiCoreClocks clocks_;
  std::unique_ptr<HeadlessApiOwnerLock> owner_lock_;
  manyforge::deployment::FilesystemVerifiedRetainedBundleSource
      retained_bundle_source_;
  manyforge::deployment::FilesystemScenarioApplyCandidateSource
      scenario_catalog_source_;
  ProgramApplyOwner program_apply_owner_;
  ActiveNodeEnvironmentProjection node_environment_projection_;
  ActiveSourceModelProjection source_model_projection_;
  manyforge::deployment::FilesystemVerifiedRetainedViewerModelSource
      viewer_model_source_;
  std::shared_ptr<const manyforge::deployment::RetainedBundle>
      verified_active_bundle_;
  std::shared_ptr<const manyforge::deployment::VerifiedRetainedViewerModel>
      verified_viewer_model_;
  std::optional<manyforge::content_identity::CanonicalValue>
      source_model_projection_cache_;
  std::optional<manyforge::content_identity::CanonicalValue>
      assistant_mode_index_cache_;
  std::map<std::string, manyforge::content_identity::CanonicalValue>
      assistant_mode_projection_cache_;
  std::shared_ptr<SqliteEventHistoryStore> history_store_;
  AsyncEventHistorian historian_;
  RuntimeSnapshotAggregator runtime_aggregator_;
  AlarmLifecycleOwner alarm_owner_;
  AdapterCursor runtime_health_adapter_;
  AdapterCursor scene_adapter_;
  AdapterCursor operation_adapter_;
  std::uint64_t operation_native_sequence_{};
  std::string operation_native_record_digest_;
  std::map<std::string, AlarmSubject> active_operation_faults_;
  std::set<std::string> observed_operation_faults_;
  mutable std::mutex mutex_;
  std::map<std::string, ReplayEntry> replay_;
  std::uint64_t node_environment_generation_{};
  std::string previous_node_environment_digest_;
  std::uint64_t source_model_generation_{};
  std::string previous_source_model_digest_;
  bool shutdown_{};
};

[[nodiscard]] HeadlessApiCoreClocks ProductionHeadlessApiClocks();
[[nodiscard]] std::string MintHeadlessOwnerEpoch(std::string_view prefix);

} // namespace manyforge::headless
