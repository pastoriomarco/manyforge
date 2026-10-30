#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_headless/assistant_mode_projection.hpp>
#include <manyforge_headless/event_historian.hpp>
#include <manyforge_headless/node_environment_projection.hpp>
#include <manyforge_headless/operator_alarm_owner.hpp>
#include <manyforge_headless/runtime_snapshot.hpp>
#include <manyforge_headless/source_model_projection.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::headless {

inline constexpr std::string_view kHeadlessApiMutationResponseSchemaVersion =
    "manyforge.headless_api_mutation_response.v1";
inline constexpr std::string_view kViewerModelAssetSchemaVersion =
    "manyforge.viewer_model_asset.v1";
inline constexpr std::string_view kAppliedProgramSelectionSchemaVersion =
    "manyforge.applied_program_selection.v3";
inline constexpr std::string_view kAppliedProgramStatusSchemaVersion =
    "manyforge.applied_program_status.v2";
inline constexpr std::string_view kCommittedScenarioActivationSchemaVersion =
    "manyforge.committed_scenario_activation.v3";
inline constexpr std::string_view kSourceModelContextSchemaVersion =
    "manyforge.source_model_context.v1";
inline constexpr std::string_view kAssistantModeContextSchemaVersion =
    "manyforge.assistant_mode_context.v1";
inline constexpr std::size_t kMaximumViewerModelResourceUriBytes = 4096U;
/// The existing execution response envelope bound also covers its current
/// runtime-fault snapshot. Per-field owner contracts remain independently checked.
inline constexpr std::size_t kMaximumRuntimeFaultSnapshotBytes =
    manyforge::core::kMaximumCellAdmissionJsonResponseBytes;
inline constexpr std::string_view kAlarmSourceRefreshSchemaVersion =
    "manyforge.alarm_source_refresh.v1";
/// Alarm-source currency (spec 530 §11). Source health, the scene adapter and
/// the active resource registry that decide motion-attention reconciliation
/// are re-evaluated by `RefreshAlarmSources` at most once per period R; the
/// execution event pump calls it on every pump iteration.
inline constexpr std::int64_t kAlarmSourceRefreshPeriodNs = 250'000'000;
/// Bound C on the age of the last completed alarm-source refresh. A read of
/// the motion-attention projection that finds the refresh older than C names
/// owner health `operator_alarm_source_refresh_overdue`, so the projection
/// reports not-reconciled (a new generation) instead of silently old facts.
inline constexpr std::int64_t kAlarmSourceCurrencyBoundNs = 2'000'000'000;
inline constexpr std::string_view kAlarmSourceRefreshOverdueHealthCode =
    "operator_alarm_source_refresh_overdue";

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
  std::filesystem::path projection_root;
  std::filesystem::path install_root;
  std::filesystem::path artifact_root;
  std::filesystem::path node_catalog_path;
  std::filesystem::path node_environment_manifest_path;
  std::filesystem::path alarm_catalog_path;
  std::filesystem::path historian_database_path;
  std::string cell_id;
  std::string owner_id;
  std::string owner_epoch;
  RuntimeSnapshotFreshnessPolicy freshness_policy;
  std::size_t historian_queue_capacity{kDefaultHistorianQueueCapacity};
  std::size_t historian_record_retention{kDefaultHistorianRecordRetention};
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
  SourceModelContext();
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  AssistantModeContext(std::string_view mode_id);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  AppliedProgramSelection();
  /// Derived executable/non-executable status of the active Applied Program.
  /// For a non-executable Program it carries the preserved authored Program
  /// and the named unresolved references; it never carries a binding.
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  AppliedProgramStatus();
  /// The committed Scenario activation Manager currently publishes: its
  /// publication generation and Apply (identity and record digest), the
  /// authored Program (null when
  /// the Scenario has none), and every committed Apply of this publication
  /// history (identity, record digest and Scenario), all read from one
  /// Manager source snapshot.
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  CommittedScenarioActivation();
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
  /// The generation-fenced motion-attention projection the native Kernel
  /// evaluates at admission (`manyforge.operator_alarm_motion_attention.v3`).
  /// Reads the alarm owner's published projection without the core
  /// coordination lock and without refreshing sources; its currency is
  /// bounded by `kAlarmSourceCurrencyBoundNs` instead.
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  OperatorAlarmMotionAttention();
  /// Re-evaluate the alarm sources (runtime health, scene, active resource
  /// registry) when the last completed refresh is at least
  /// `kAlarmSourceRefreshPeriodNs` old; otherwise return without the core
  /// lock. Reports whether it refreshed, the current refresh time, and
  /// `component_lifecycle_observation_pending`: true from the moment the
  /// lifecycle source's quality became unknown because the active
  /// configuration changed or CCLC was unreachable, until a complete
  /// observation of the active configuration is accepted. The event pump
  /// then repeats the lifecycle read at its short retry floor instead of
  /// waiting its ordinary period (spec 530 §7.1, respected without polling).
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  RefreshAlarmSources();
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
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  HistoryQueryPage(const HistorianQuery &query, bool full_records = false);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  HistoryDiscovery(const std::string &kind, const std::string &after,
                   std::size_t limit);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  HistoryCapturePage(std::optional<std::uint64_t> before_capture_id,
                     std::size_t limit);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  HistoryDetail(std::uint64_t sequence);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  RecordClientIncident(std::string incident_id, std::string incident_kind,
                       std::string phase, std::string control_session_id,
                       std::string client_incarnation, std::string source_label,
                       std::string reason_code, std::uint64_t occurrence_count,
                       std::optional<std::uint64_t> maximum_delay_ns,
                       std::uint64_t first_observed_ns,
                       std::uint64_t last_observed_ns,
                       std::map<std::string, std::string> causal_context = {});
  [[nodiscard]] manyforge::content_identity::CanonicalValue HistorianHealth();
  /// Startup-only launcher evidence, committed before a pending file is finalized.
  /// Never opens or imports an archived owner journal.
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  AppendWorkbenchPreviousSessionReport(manyforge::content_identity::CanonicalValue report);
  /// Register only the current predecessor with the existing alarm owner.
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  ObserveWorkbenchPreviousSessionReport(manyforge::content_identity::CanonicalValue report);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  ReconcileHistorianHealth(std::uint64_t expected_health_generation,
                           AlarmCallerAttribution caller);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  CommitProgramModelAttempt(manyforge::content_identity::CanonicalValue record);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  RecordProgramDiagnostic(manyforge::content_identity::CanonicalValue record);
  /// Item 37: one History row per operator Force close, naming what the
  /// Kernel step left in an unknown state.
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  RecordScenarioForceClose(manyforge::content_identity::CanonicalValue record);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  IngestOperationEvent(
      std::string source_owner_epoch, std::uint64_t source_sequence,
      std::string record_kind, std::optional<std::string> request_id,
      std::optional<std::string> operation_id,
      std::optional<std::uint64_t> operation_revision,
      std::int64_t occurred_at_ns, std::string occurred_at_wall,
      bool effect_pending,
      manyforge::content_identity::CanonicalValue journal_payload,
      std::string payload_digest);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  CompleteOperationEventSnapshot(
      std::string source_owner_epoch, std::uint64_t through_source_sequence,
      std::optional<manyforge::content_identity::CanonicalValue>
          current_stop_fault = std::nullopt);
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  MarkOperationEventSourceUnknown(std::string source_owner_epoch);
  /// Observe two bounded current native-owner fault facts. First occurrences
  /// remain active for this Workbench lifetime, including across reconnects.
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  ObserveRuntimeFaultConditions(
      std::string source_owner_epoch,
      manyforge::content_identity::CanonicalValue current_runtime_faults);
  /// Internal accepted Program source; absence/owner replacement never clears an occurrence.
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  ObserveProgramFailureCondition(manyforge::content_identity::CanonicalValue condition);
  /// Controllable components of the active configuration whose CCLC
  /// snapshots the component-lifecycle alarm conditions are derived from,
  /// that configuration's retained bundle and graph revision (null before
  /// the first publication), and whether the lifecycle source is
  /// registered. A registered source with an empty scope still needs an
  /// observation (`ObserveEmptyComponentLifecycleConfiguration`), which
  /// returns the conditions of the removed components and reconciles its
  /// quality.
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  ComponentLifecycleConditionScope();
  /// Derive the safety-stop alarm condition (spec 530 §3.3, 2026-10-08)
  /// from CCLC component snapshots: an open component incident or an
  /// intervention-requiring blocking cause holds `component_lifecycle_fault`
  /// (incident id in the details) until the acknowledged incident is gone
  /// from a fresh snapshot. `snapshots`
  /// and `unavailable_component_ids` together must name exactly the current
  /// scope, and every snapshot must name the active retained bundle and
  /// graph revision; otherwise nothing is submitted (`scope_changed`). The
  /// active configuration is the one Manager publishes now, reread under the
  /// owner lock; a stale, unavailable or unverifiable publication submits
  /// nothing (`configuration_unavailable`). An unavailable component keeps
  /// its conditions unchanged and leaves the source quality as it was. Only
  /// a changed condition is submitted.
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  ObserveComponentLifecycleConditions(
      std::string cclc_owner_epoch,
      manyforge::content_identity::CanonicalValue snapshots,
      std::vector<std::string> unavailable_component_ids);
  /// The active configuration (`retained_bundle_id`, `graph_revision`) has
  /// no controlled components, so it has no CCLC owner and no lifecycle
  /// conditions: the conditions of the components that left the scope
  /// return to normal and the lifecycle source's quality becomes good
  /// (`observed`; `empty` when the source was never registered). Judged
  /// against the configuration Manager publishes now, like
  /// `ObserveComponentLifecycleConditions`: a different or non-empty
  /// configuration is `scope_changed`, a stale, unavailable or unverifiable
  /// publication `configuration_unavailable`; neither submits anything.
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  ObserveEmptyComponentLifecycleConfiguration(std::string retained_bundle_id,
                                              std::string graph_revision);
  /// CCLC is unreachable: its conditions keep their state at unknown quality.
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  MarkComponentLifecycleSourceUnknown();
  /// Record one CCLC incident history event (addendum 2 contract item 5,
  /// `component_incident` / `component_incident_acknowledged`) as historian
  /// component `manyforge.cclc`, event type = event kind, with the CCLC
  /// fields as details. Idempotent per (incident id, event kind): an event
  /// this process already enqueued or the durable history already holds is
  /// `duplicate`, so a lost reader cursor never records it twice. A full
  /// historian queue answers `rejected` (nothing recorded; retry later).
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  RecordCCLCIncidentEvent(std::string cclc_owner_epoch,
                          manyforge::content_identity::CanonicalValue event);
  void Shutdown();

private:
  class PackageRootDescriptor;

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
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  ActiveProgramSelectionLocked(const HeadlessRuntimeSnapshot &runtime);
  [[nodiscard]] const manyforge::content_identity::CanonicalValue &
  ActiveAppliedRecordLocked(const HeadlessRuntimeSnapshot &runtime);
  void
  RefreshRuntimeHealthAdapterLocked(const HeadlessRuntimeSnapshot &runtime);
  void RefreshSceneAdapterLocked(const HeadlessRuntimeSnapshot &runtime);
  void RegisterOperationEpochLocked(std::string source_owner_epoch);
  /// Call-local facts parsed once from one immutable committed capture.
  struct ParsedOperationFacts final {
    bool captured_origin{};
    bool terminal_facts_present{};
    bool preemption_present{};
    bool captured_failure_effects_known{};
  };
  [[nodiscard]] static ParsedOperationFacts
  ParseOperationFacts(const manyforge::content_identity::CanonicalValue &state,
                      const manyforge::content_identity::CanonicalValue *intent,
                      bool captured_origin_allowed, bool terminal_facts_allowed,
                      bool preemption_allowed);
  void ReconcileRuntimeCollisionStopLocked(
      const manyforge::content_identity::CanonicalValue &journal_payload,
      std::uint64_t native_sequence,
      std::string_view payload_digest, std::int64_t occurred_at_ns,
      std::string_view occurred_at_wall, std::string_view raw_event_id);
  void ReconcileOperationFaultLocked(
      const manyforge::content_identity::CanonicalValue &journal_payload,
      const ParsedOperationFacts &facts, std::string_view record_kind,
      std::string_view request_id, std::uint64_t native_sequence,
      std::string_view payload_digest, std::int64_t occurred_at_ns,
      std::string_view occurred_at_wall, std::string_view raw_event_id);
  void ReconcileStopFaultLocked(
      const manyforge::content_identity::CanonicalValue &journal_payload,
      std::string_view record_kind, std::uint64_t native_sequence,
      std::string_view payload_digest, std::int64_t occurred_at_ns,
      std::string_view occurred_at_wall, std::string_view raw_event_id);
  void ObserveCurrentStopFaultLocked(
      const manyforge::content_identity::CanonicalValue &fault_latch,
      std::uint64_t native_sequence);
  /// Which adapter-local record an admitted operation condition updates.
  enum class OperationConditionKind {
    kExecutionFault,
    kRuntimeCollision,
    kStopFault,
    kProgramFailure,
  };
  /// A still-current activation the alarm owner refused for its scope. It is
  /// reconsidered once whenever the active configuration changes.
  struct RefusedOperationCondition final {
    AlarmConditionUpdate update;
    OperationConditionKind kind;
    std::string tracking_id;
  };

  void SynchronizeAlarmResourceRegistryLocked(
      const HeadlessRuntimeSnapshot &runtime);
  /// Reread the current Manager publication and synchronize the alarm
  /// resource registry from it. True only when that publication is fresh and
  /// the registry now holds the exact configuration it names, so an
  /// observation can be judged against the active configuration rather than
  /// one a reader last happened to refresh.
  [[nodiscard]] bool RefreshCurrentAlarmRegistryLocked();
  /// Return the lifecycle conditions of components outside `in_scope` to
  /// normal and forget them; `correlations` names the observation for one
  /// removed component.
  void ReturnRemovedComponentConditionsLocked(
      const std::set<std::string> &in_scope,
      const std::function<manyforge::content_identity::CanonicalValue(
          const std::string &component_id)> &correlations);
  /// Submit one operation-derived condition. A scope the alarm owner refuses
  /// (stale or invalid resources) is recorded as a named, historian-visible
  /// rejection and returns no mutation; it never fails the operation feed.
  /// A refused activation is retained for reconsideration after the next
  /// active-configuration change; any later update of it supersedes that.
  [[nodiscard]] std::optional<AlarmMutation>
  SubmitOperationConditionLocked(AlarmConditionUpdate update,
                                 OperationConditionKind kind,
                                 std::string tracking_id);
  void TrackAdmittedOperationConditionLocked(OperationConditionKind kind,
                                             const std::string &tracking_id,
                                             const AlarmConditionUpdate &update);
  void ForgetRefusedOperationConditionLocked(std::string_view definition_id,
                                             std::string_view tracking_id);
  void ReconsiderRefusedOperationConditionsLocked();
  /// Hold the cell-scoped alarm-scope condition active exactly while at least
  /// one refused operation condition is retained, naming each of them. Called
  /// after every change of the retained refusals; unchanged state is not
  /// resubmitted.
  void SynchronizeAlarmScopeConditionLocked();
  /// Last submitted state of one component-lifecycle condition point.
  struct ComponentConditionState final {
    AlarmSubject subject;
    bool active{};
    std::string fingerprint;
  };
  /// Submit `active` for one (definition, component) point when it differs
  /// from the last submission. A never-raised point is not cleared, and a
  /// point whose subject changed while active is first cleared under its
  /// old subject.
  void SynchronizeComponentConditionLocked(
      AdapterCursor &cursor, const std::string &definition_id,
      const std::string &component_id, const AlarmSubject &subject,
      bool active, const manyforge::content_identity::CanonicalValue &details,
      const manyforge::content_identity::CanonicalValue &correlations);
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

  HeadlessApiCoreConfig config_;
  HeadlessApiCoreClocks clocks_;
  std::unique_ptr<HeadlessApiOwnerLock> owner_lock_;
  std::unique_ptr<PackageRootDescriptor> package_root_descriptor_;
  manyforge::deployment::FilesystemVerifiedRetainedBundleSource
      retained_bundle_source_;
  manyforge::deployment::FilesystemProgramRepository program_repository_;
  manyforge::deployment::FilesystemProgramBindingRepository binding_repository_;
  manyforge::deployment::FilesystemVerifiedRetainedModelMapSource
      retained_model_map_source_;
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
  /// (retained bundle, graph revision) whose resources the alarm owner
  /// currently admits; empty until Manager publishes an active configuration.
  std::optional<std::pair<std::string, std::string>> alarm_registry_identity_;
  std::map<std::string, RefusedOperationCondition> refused_operation_conditions_;
  AdapterCursor runtime_health_adapter_;
  AdapterCursor runtime_fault_adapter_;
  std::set<std::string> active_runtime_fault_kinds_;
  bool runtime_health_baseline_observed_{};
  AdapterCursor scene_adapter_;
  /// Scene-domain alarm source: `scene.carried_object_lost` events.
  AdapterCursor scene_pose_adapter_;
  /// Event ids of the published losses already alarmed.
  std::set<std::string> raised_carried_object_losses_;
  AdapterCursor operation_adapter_;
  AdapterCursor program_failure_adapter_;
  std::uint64_t program_failure_control_revision_{};
  std::uint64_t program_failure_run_revision_{};
  std::string program_failure_run_id_;
  AdapterCursor alarm_scope_adapter_;
  /// CCLC lifecycle condition (`component_lifecycle_fault`). It follows the
  /// CCLC owner epoch and is registered only after the first complete
  /// observation.
  AdapterCursor component_lifecycle_adapter_;
  AdapterCursor workbench_session_adapter_;
  std::optional<std::string> workbench_previous_session_id_;
  /// Controllable components of the active configuration, sorted.
  std::vector<std::string> alarm_component_ids_;
  /// (definition id, component id) -> last submitted state.
  std::map<std::pair<std::string, std::string>, ComponentConditionState>
      component_conditions_;
  /// (incident id, event kind) of CCLC incident events enqueued by this
  /// process; bounded, the durable history answers beyond it.
  std::set<std::pair<std::string, std::string>> recorded_cclc_incident_events_;
  std::uint64_t operation_native_sequence_{};
  std::string operation_native_record_digest_;
  std::map<std::string, AlarmSubject> active_operation_faults_;
  std::set<std::string> observed_operation_faults_;
  /// A runtime-monitor Stop alarm: a collision, or lost measured robot state.
  struct RuntimeMonitorStopCondition final {
    std::string definition_id;
    AlarmSubject subject;
  };
  std::map<std::string, RuntimeMonitorStopCondition> active_runtime_collisions_;
  std::set<std::string> observed_runtime_collisions_;
  std::map<std::string, AlarmSubject> active_stop_faults_;
  std::string current_stop_fault_fingerprint_;
  mutable std::mutex mutex_;
  std::uint64_t node_environment_generation_{};
  std::string previous_node_environment_digest_;
  std::uint64_t source_model_generation_{};
  std::string previous_source_model_digest_;
  /// Monotonic observation time of the last completed `RefreshLocked`;
  /// negative before the first. Read without `mutex_` by the attention read.
  std::atomic<std::int64_t> alarm_sources_refreshed_at_ns_{-1};
  /// Set (under `mutex_`) when the component-lifecycle source needs an
  /// observation of the active configuration: a registry replacement or an
  /// unreachable CCLC made its quality unknown. Cleared (under `mutex_`) when
  /// a complete observation of the active configuration completes the
  /// source snapshot with good quality, or when nothing is in scope. Read
  /// without `mutex_` by `RefreshAlarmSources`.
  std::atomic<bool> component_lifecycle_observation_pending_{};
  std::atomic<bool> shutdown_{};
};

[[nodiscard]] HeadlessApiCoreClocks ProductionHeadlessApiClocks();
[[nodiscard]] std::string MintHeadlessOwnerEpoch(std::string_view prefix);
/// The Applied Program selection's `assistant_projection.ai_models`: every AI
/// model component row of `graph` (`controllable_component_id`, `base_url`,
/// `model`, `api_key_env` or null) in retained (id) order; empty when none.
[[nodiscard]] manyforge::content_identity::CanonicalValue
AssistantAiModels(const manyforge::core::CellGraphDTO &graph);

} // namespace manyforge::headless
