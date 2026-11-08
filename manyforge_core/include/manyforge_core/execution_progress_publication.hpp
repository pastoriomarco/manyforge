#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace manyforge::core {

enum class ExecutionProgressSourceKind {
  kBackendCommanded,
  kMeasuredProjection,
};

enum class ExecutionProgressAuthorityProfile {
  kCommandedSimulation,
  kPredictedNative,
  kMeasuredPhysical,
};

const char* execution_progress_source_kind_name(ExecutionProgressSourceKind kind) noexcept;
const char* execution_progress_authority_profile_name(
    ExecutionProgressAuthorityProfile profile) noexcept;

struct ExecutionProgressCursorSnapshotDTO {
  std::uint64_t sequence = 0;
  std::uint64_t execution_generation = 0;
  std::int64_t trajectory_time_ns = 0;
  ExecutionProgressSourceKind source_kind = ExecutionProgressSourceKind::kBackendCommanded;
  std::string_view authority_profile;
  std::int64_t source_timestamp_ns = 0;
  std::int64_t received_timestamp_ns = 0;
  std::int64_t freshness_budget_ns = 0;
  bool valid = false;
  // A terminal marker is an explicit backend-to-kernel handoff, not an
  // invalid cursor. It prevents queued lookahead work from treating normal
  // completion as progress-authority loss until the kernel disarms and
  // retires the generation.
  bool terminal = false;
};

struct ExecutionProgressPublishResult {
  bool published = false;
  std::uint64_t sequence = 0;
  const char* reason = "";
};

enum class ExecutionProgressHazardClaimResult {
  kClaimed,
  kTerminal,
  kGenerationMismatch,
};

/// Scalar, latest-only execution-progress publication.
/// Writers serialize on one leaf mutex; readers perform bounded atomic
/// snapshots and never take that mutex. The publication owns no trajectory and
/// cannot infer authority: the backend constructor fixes source/profile for its
/// lifetime.
class ExecutionProgressPublication final {
 public:
  ExecutionProgressPublication(ExecutionProgressSourceKind source_kind,
                               ExecutionProgressAuthorityProfile profile,
                               std::string authority_profile_id, std::int64_t freshness_budget_ns);

  ExecutionProgressPublication(const ExecutionProgressPublication&) = delete;
  ExecutionProgressPublication& operator=(const ExecutionProgressPublication&) = delete;

  ExecutionProgressPublishResult publish(std::uint64_t execution_generation,
                                         std::int64_t trajectory_time_ns,
                                         std::int64_t source_timestamp_ns,
                                         std::int64_t received_timestamp_ns) noexcept;

  /// Publish the definitive natural-completion boundary for one generation.
  /// The marker remains valid until kernel terminal handoff retires it. Once
  /// terminal, late same-generation progress callbacks cannot reopen motion.
  ExecutionProgressPublishResult publish_terminal(std::uint64_t execution_generation,
                                                  std::int64_t trajectory_time_ns,
                                                  std::int64_t source_timestamp_ns,
                                                  std::int64_t received_timestamp_ns) noexcept;

  /// Atomically arbitrate a completed lookahead hazard against natural
  /// completion for one generation. The first authority boundary wins:
  /// terminal prevents a hazard claim, while a hazard claim prevents a late
  /// terminal callback from erasing the stop decision. An invalid active
  /// cursor remains claimable so authority loss still fails closed.
  ExecutionProgressHazardClaimResult claim_hazard_authority(
      std::uint64_t execution_generation) noexcept;

  /// Read existing hazard authority for one exact generation. Nullopt means
  /// another/no generation; this never claims or clears authority. Handover
  /// callers serialize this read through publication with the same Kernel
  /// protection lock used by claim_hazard_authority.
  [[nodiscard]] std::optional<bool> hazard_authority_claimed(
      std::uint64_t execution_generation) const noexcept;

  /// Lifecycle-bound invalidation is guaranteed, not best effort, and retires
  /// the current generation. Readers see either the preceding complete even
  /// epoch or the resulting invalid epoch; only a newer generation can restore
  /// validity.
  void invalidate() noexcept;
  bool invalidate_generation(std::uint64_t expected_generation) noexcept;

  [[nodiscard]] ExecutionProgressCursorSnapshotDTO acquire() const noexcept;

  [[nodiscard]] ExecutionProgressSourceKind source_kind() const noexcept { return source_kind_; }
  [[nodiscard]] ExecutionProgressAuthorityProfile authority_profile() const noexcept {
    return authority_profile_;
  }
  [[nodiscard]] const std::string& authority_profile_id() const noexcept {
    return authority_profile_id_;
  }
  [[nodiscard]] std::int64_t freshness_budget_ns() const noexcept { return freshness_budget_ns_; }

 private:
  ExecutionProgressPublishResult publish_impl(std::uint64_t execution_generation,
                                              std::int64_t trajectory_time_ns,
                                              std::int64_t source_timestamp_ns,
                                              std::int64_t received_timestamp_ns,
                                              bool terminal) noexcept;
  struct Cursor {
    std::uint64_t sequence = 0;
    std::uint64_t execution_generation = 0;
    std::int64_t trajectory_time_ns = 0;
    std::int64_t source_timestamp_ns = 0;
    std::int64_t received_timestamp_ns = 0;
    bool valid = false;
    bool terminal = false;
  };
  // One seqlock slot. The writer fills the slot readers are not directed to,
  // then flips latest_slot_, so a reader overlapping one write still reads the
  // previous complete cursor instead of failing closed.
  struct Slot {
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint64_t> execution_generation{0};
    std::atomic<std::int64_t> trajectory_time_ns{0};
    std::atomic<std::int64_t> source_timestamp_ns{0};
    std::atomic<std::int64_t> received_timestamp_ns{0};
    std::atomic<bool> valid{false};
    std::atomic<bool> terminal{false};
  };
  // Writer-only; requires writer_mutex_.
  void store_locked() noexcept;

  const ExecutionProgressSourceKind source_kind_;
  const ExecutionProgressAuthorityProfile authority_profile_;
  const std::string authority_profile_id_;
  const std::int64_t freshness_budget_ns_;

  mutable std::mutex writer_mutex_;
  bool hazard_authority_claimed_ = false;
  std::uint64_t next_sequence_ = 1;
  Cursor current_;
  Slot slots_[2];
  std::atomic<unsigned> latest_slot_{0};
};

}  // namespace manyforge::core
