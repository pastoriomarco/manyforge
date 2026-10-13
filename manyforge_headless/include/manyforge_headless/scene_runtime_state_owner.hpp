#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_content_identity/canonical_value.hpp>

namespace manyforge::headless {

inline constexpr std::string_view kSceneRuntimeStateSnapshotApiVersion =
    "manyforge.scene_runtime_state.v1";
inline constexpr std::size_t kDefaultMaximumPoseObservationKeys = 4096U;

class SceneRuntimeStateError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class SceneRuntimeStateConflictError final : public SceneRuntimeStateError {
public:
  using SceneRuntimeStateError::SceneRuntimeStateError;
};

class SceneRuntimeStateCapacityError final : public SceneRuntimeStateError {
public:
  using SceneRuntimeStateError::SceneRuntimeStateError;
};

enum class SceneInitializationState {
  kOpen,
  kClosed,
  kResetting,
  kSeeding,
  kReplaying,
  kFailed,
  kRetired,
};

[[nodiscard]] std::string_view
ToString(SceneInitializationState state) noexcept;

enum class PoseSourceQuality {
  kGood,
  kUnknown,
};

[[nodiscard]] std::string_view ToString(PoseSourceQuality quality) noexcept;

enum class PoseObservationDisposition {
  kApplyNow,
  kBuffered,
  kReplacedBuffered,
  kObservedOnly,
  kDuplicate,
};

[[nodiscard]] std::string_view
ToString(PoseObservationDisposition disposition) noexcept;

struct SceneRuntimeIdentity final {
  std::string retained_bundle_id;
  std::string graph_revision;
  std::uint64_t deployment_generation{};
  std::uint64_t program_generation{};
  std::uint64_t runtime_generation{};

  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const SceneRuntimeIdentity &,
                         const SceneRuntimeIdentity &) = default;
};

struct PoseObservationKey final {
  std::string source_id;
  std::string object_id;
  std::string policy_id;

  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const PoseObservationKey &,
                         const PoseObservationKey &) = default;
  friend bool operator<(const PoseObservationKey &left,
                        const PoseObservationKey &right) noexcept;
};

struct PoseSourceObservation final {
  PoseObservationKey key;
  std::string source_epoch;
  std::uint64_t source_epoch_order{};
  std::uint64_t source_sequence{};
  std::int64_t observed_at_ns{};
  std::int64_t expires_at_ns{};
  PoseSourceQuality quality{PoseSourceQuality::kUnknown};
  bool eligible{};
  manyforge::content_identity::CanonicalValue payload;

  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const PoseSourceObservation &,
                         const PoseSourceObservation &) = default;
};

struct PoseObservationAcceptance final {
  PoseObservationDisposition disposition;
  std::uint64_t initialization_generation;
  bool fresh;
};

struct PoseSourceLifetimeSnapshot final {
  PoseSourceObservation observation;
  bool fresh;
  bool buffered;
};

struct SceneInitializationCounters final {
  std::uint64_t closed{};
  std::uint64_t buffered{};
  std::uint64_t buffer_replaced{};
  std::uint64_t replayed{};
  std::uint64_t opened{};
  std::uint64_t failed{};
  std::uint64_t retired{};
  std::uint64_t stale_completion_rejected{};
};

/// One Manager-configured cell source; loss does not discard historical poses.
struct ConfiguredPoseSourceAvailability final {
  std::string source_id;
  std::string source_epoch_base;
  bool available{true};
  friend bool operator==(const ConfiguredPoseSourceAvailability &,
                         const ConfiguredPoseSourceAvailability &) = default;
};

struct SceneRuntimeStateSnapshot final {
  std::uint64_t initialization_generation{};
  std::optional<std::string> initialization_token;
  SceneInitializationState state{SceneInitializationState::kOpen};
  bool deferred{};
  std::optional<SceneRuntimeIdentity> runtime_identity;
  std::string reason;
  std::optional<std::string> error;
  std::int64_t started_at_ns{};
  std::int64_t updated_at_ns{};
  std::optional<std::uint64_t> active_replay_id;
  SceneInitializationCounters counters;
  std::vector<PoseSourceLifetimeSnapshot> pose_sources;
  std::optional<ConfiguredPoseSourceAvailability> configured_pose_source{};
};

struct SceneReplayBatch final {
  std::uint64_t initialization_generation;
  std::string initialization_token;
  std::uint64_t replay_id;
  std::vector<PoseSourceObservation> observations;
};

/// One non-real-time logical-scene/runtime owner state machine.
///
/// This owner only commits bounded state and returns immutable work batches.
/// Callers perform backend reset, authored-scene seed, and pose application
/// after its lock is released, then report the exact generation/token/batch
/// completion. A stale completion cannot open a newer runtime generation.
class SceneRuntimeStateOwner final {
public:
  using Clock = std::function<std::int64_t()>;
  using TokenFactory = std::function<std::string()>;

  SceneRuntimeStateOwner(Clock clock, TokenFactory token_factory,
                         std::size_t maximum_pose_observation_keys =
                             kDefaultMaximumPoseObservationKeys);

  /// Establishes the already-durable active runtime after external bootstrap.
  /// It is allowed exactly once before any candidate initialization starts.
  void EstablishOpenRuntime(SceneRuntimeIdentity identity,
                            std::uint64_t initialization_generation,
                            std::string reason);

  /// Idempotently advances the open projection to an externally finalized
  /// Apply result. The logical-scene owner has already completed its durable
  /// transition, so this performs no reset, seed, replay, or robot effect.
  /// A stale or conflicting generation is rejected.
  [[nodiscard]] bool
  AdoptFinalizedOpenRuntime(SceneRuntimeIdentity identity,
                            std::uint64_t initialization_generation,
                            std::string reason);

  /// Internal Manager transition after exact observer retirement, before any
  /// replacement Open. Clears only that source's retained poses in the exact
  /// already-open runtime; never resets Scene or an initialization/replay.
  void RetirePoseSource(const SceneRuntimeIdentity &expected_runtime_identity,
                        std::string_view source_id);

  /// Pins the current checked source. Exact re-publication never clears loss.
  void ConfigurePoseSourceAvailability(
      const SceneRuntimeIdentity &expected_runtime_identity,
      std::string source_id, std::string source_epoch_base);
  /// Exact current-source loss closes freshness independently of incident IO.
  void MarkPoseSourceUnavailable(
      const SceneRuntimeIdentity &expected_runtime_identity,
      std::string_view source_id, std::string_view source_epoch_base);

  [[nodiscard]] std::string BeginInitialization(SceneRuntimeIdentity candidate,
                                                bool deferred,
                                                std::string reason);
  void PrepareReset(std::string_view token, std::string reason);
  void MarkSeeded(std::string_view token);
  [[nodiscard]] SceneReplayBatch BeginReplay(std::string_view token);
  void CompleteReplay(const SceneReplayBatch &batch, std::size_t applied_count);
  void FailReplay(const SceneReplayBatch &batch, std::string error);
  void FailInitialization(std::string_view token, std::string error);
  void Retire(std::string_view token, std::string reason);

  [[nodiscard]] PoseObservationAcceptance
  ObservePose(PoseSourceObservation observation);

  [[nodiscard]] SceneRuntimeStateSnapshot Snapshot() const;

private:
  struct PoseRecord final {
    PoseSourceObservation observation;
    bool buffered{};
  };

  [[nodiscard]] std::int64_t Now() const;
  void RequireCurrentTokenLocked(std::string_view token);
  void RestoreReplayLocked(const SceneReplayBatch &batch);

  Clock clock_;
  TokenFactory token_factory_;
  std::size_t maximum_pose_observation_keys_;
  mutable std::mutex mutex_;
  std::uint64_t initialization_generation_{};
  std::optional<std::string> initialization_token_;
  SceneInitializationState state_{SceneInitializationState::kOpen};
  bool deferred_{};
  std::optional<SceneRuntimeIdentity> runtime_identity_;
  std::string reason_{"startup"};
  std::optional<std::string> error_;
  std::int64_t started_at_ns_{};
  std::int64_t updated_at_ns_{};
  std::uint64_t next_replay_id_{};
  std::optional<std::uint64_t> active_replay_id_;
  std::optional<std::string> active_replay_digest_;
  SceneInitializationCounters counters_;
  std::map<PoseObservationKey, PoseRecord> pose_records_;
  std::optional<ConfiguredPoseSourceAvailability> configured_pose_source_;
};

[[nodiscard]] manyforge::content_identity::CanonicalValue
SceneRuntimeStateValue(const SceneRuntimeStateSnapshot &snapshot);

} // namespace manyforge::headless
