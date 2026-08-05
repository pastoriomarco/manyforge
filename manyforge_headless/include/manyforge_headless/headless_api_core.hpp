#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <manyforge_headless/event_historian.hpp>
#include <manyforge_headless/node_environment_projection.hpp>
#include <manyforge_headless/operator_alarm_owner.hpp>
#include <manyforge_headless/runtime_snapshot.hpp>

namespace manyforge::headless {

inline constexpr std::string_view kHeadlessApiMutationResponseSchemaVersion =
    "manyforge.headless_api_mutation_response.v1";

class HeadlessApiOwnerLock;

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
  [[nodiscard]] manyforge::content_identity::CanonicalValue
  NodeEnvironmentProjectionLocked(const HeadlessRuntimeSnapshot &runtime);
  void
  RefreshRuntimeHealthAdapterLocked(const HeadlessRuntimeSnapshot &runtime);
  void RefreshSceneAdapterLocked(const HeadlessRuntimeSnapshot &runtime);
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
  ActiveNodeEnvironmentProjection node_environment_projection_;
  std::shared_ptr<SqliteEventHistoryStore> history_store_;
  AsyncEventHistorian historian_;
  RuntimeSnapshotAggregator runtime_aggregator_;
  AlarmLifecycleOwner alarm_owner_;
  AdapterCursor runtime_health_adapter_;
  AdapterCursor scene_adapter_;
  mutable std::mutex mutex_;
  std::map<std::string, ReplayEntry> replay_;
  std::uint64_t node_environment_generation_{};
  std::string previous_node_environment_digest_;
  bool shutdown_{};
};

[[nodiscard]] HeadlessApiCoreClocks ProductionHeadlessApiClocks();
[[nodiscard]] std::string MintHeadlessOwnerEpoch(std::string_view prefix);

} // namespace manyforge::headless
