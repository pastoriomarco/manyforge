#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::core {

enum class ObservedRobotStateSourceKind {
  kActionFeedback,
  kControllerState,
  kJointState,
  kCommandedState,
  kPredictedState,
};

enum class ObservedRobotStateAuthorityProfile {
  kMeasured,
  kCommandedSimulation,
  kPredictedNative,
};

const char* observed_robot_state_source_kind_name(
    ObservedRobotStateSourceKind kind) noexcept;
const char* observed_robot_state_authority_profile_name(
    ObservedRobotStateAuthorityProfile profile) noexcept;

struct ObservedRobotStateSourceConfig {
  ObservedRobotStateSourceConfig() = default;
  ObservedRobotStateSourceConfig(
      ObservedRobotStateSourceKind source_kind,
      std::string id,
      ObservedRobotStateAuthorityProfile profile,
      std::int64_t budget_ns,
      std::string profile_id = {})
      : kind(source_kind),
        source_id(std::move(id)),
        authority_profile(profile),
        freshness_budget_ns(budget_ns),
        authority_profile_id(std::move(profile_id)) {}

  ObservedRobotStateSourceKind kind =
      ObservedRobotStateSourceKind::kJointState;
  std::string source_id;
  ObservedRobotStateAuthorityProfile authority_profile =
      ObservedRobotStateAuthorityProfile::kMeasured;
  /// Local monotonic freshness budget. Zero means non-expiring and is valid
  /// only for explicitly qualified commanded/predicted authorities.
  std::int64_t freshness_budget_ns = 0;
  std::string authority_profile_id;
};

struct ObservedRobotStateSnapshotDTO {
  std::uint64_t sequence = 0;
  std::vector<std::string> joint_names;
  std::vector<double> positions_rad;
  std::optional<std::vector<double>> velocities_rad_s;
  std::string source_id;
  ObservedRobotStateSourceKind source_kind =
      ObservedRobotStateSourceKind::kJointState;
  std::int64_t source_timestamp_ns = 0;
  std::int64_t received_timestamp_ns = 0;
  std::string authority_profile;
  bool complete = false;
};

struct ObservedRobotStatePublishResult {
  bool published = false;
  bool complete = false;
  bool velocities_complete = false;
  std::uint64_t sequence = 0;
  const char* reason = "";
};

struct ObservedRobotStatePublicationMetrics {
  std::uint64_t published_total = 0;
  std::uint64_t rejected_total = 0;
  std::uint64_t writer_busy_total = 0;
  std::uint64_t slot_exhausted_total = 0;
  std::uint64_t reader_retry_exhausted_total = 0;
};

struct ObservedRobotStateSourceMetrics {
  ObservedRobotStateSourceKind source_kind =
      ObservedRobotStateSourceKind::kJointState;
  std::string source_id;
  std::uint64_t published_total = 0;
  std::uint64_t incomplete_total = 0;
  std::uint64_t writer_busy_total = 0;
  std::int64_t latest_received_timestamp_ns = 0;
  std::uint64_t max_interarrival_ns = 0;
};

/// Fixed-capacity, allocation-free-after-construction publication channel for
/// observed robot state (spec/210 section 3.13, RT-8C).
///
/// Each configured source owns a three-slot single-writer lane. Readers pin a
/// published slot with a bounded atomic reference count; writers only reuse an
/// unpinned, non-current slot. This avoids seqlock reads over vectors, mutable
/// shared buffers, heap allocation in ROS callbacks, and lifecycle/scene locks.
class ObservedRobotStatePublication final {
 private:
  static constexpr std::size_t kSlotsPerSource = 3;

  struct Slot {
    // -1 = writer owns the slot, >= 0 = reader count.
    mutable std::atomic<std::int32_t> users{0};
    std::vector<double> positions_rad;
    std::vector<double> velocities_rad_s;
    std::uint64_t sequence = 0;
    std::int64_t source_timestamp_ns = 0;
    std::int64_t received_timestamp_ns = 0;
    bool complete = false;
    bool velocities_complete = false;
  };

  struct SourceLane {
    explicit SourceLane(ObservedRobotStateSourceConfig source,
                        std::size_t joint_count);

    ObservedRobotStateSourceConfig source;
    std::array<Slot, kSlotsPerSource> slots;
    std::atomic<int> published_slot{-1};
    std::atomic_flag writer = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> published_total{0};
    std::atomic<std::uint64_t> incomplete_total{0};
    std::atomic<std::uint64_t> writer_busy_total{0};
    std::atomic<std::int64_t> latest_received_timestamp_ns{-1};
    std::atomic<std::uint64_t> max_interarrival_ns{0};
  };

 public:
  class ReadView final {
   public:
    ReadView() = default;
    ReadView(const ReadView&) = delete;
    ReadView& operator=(const ReadView&) = delete;
    ReadView(ReadView&& other) noexcept;
    ReadView& operator=(ReadView&& other) noexcept;
    ~ReadView();

    explicit operator bool() const noexcept { return slot_ != nullptr; }
    std::span<const std::string> joint_names() const noexcept;
    std::span<const double> positions_rad() const noexcept;
    std::span<const double> velocities_rad_s() const noexcept;
    std::uint64_t sequence() const noexcept;
    std::int64_t source_timestamp_ns() const noexcept;
    std::int64_t received_timestamp_ns() const noexcept;
    std::int64_t freshness_budget_ns() const noexcept;
    bool complete() const noexcept;
    bool velocities_complete() const noexcept;
    std::string_view joint_order_token() const noexcept;
    std::string_view source_id() const noexcept;
    ObservedRobotStateSourceKind source_kind() const noexcept;
    ObservedRobotStateAuthorityProfile authority_profile() const noexcept;
    std::string_view authority_profile_id() const noexcept;

   private:
    friend class ObservedRobotStatePublication;
    ReadView(const ObservedRobotStatePublication* owner,
             const SourceLane* lane,
             const Slot* slot) noexcept;
    void release() noexcept;

    const ObservedRobotStatePublication* owner_ = nullptr;
    const SourceLane* lane_ = nullptr;
    const Slot* slot_ = nullptr;
  };

  ObservedRobotStatePublication(
      std::vector<std::string> canonical_joint_names,
      std::vector<ObservedRobotStateSourceConfig> precedence,
      std::size_t max_source_joint_count = 256);
  ObservedRobotStatePublication(const ObservedRobotStatePublication&) = delete;
  ObservedRobotStatePublication& operator=(
      const ObservedRobotStatePublication&) = delete;

  ObservedRobotStatePublishResult publish(
      ObservedRobotStateSourceKind source,
      std::span<const std::string> source_joint_names,
      std::span<const double> positions_rad,
      std::span<const double> velocities_rad_s,
      std::int64_t source_timestamp_ns,
      std::int64_t received_timestamp_ns) noexcept;

  /// Select the first fresh, complete source in configured precedence order.
  /// If no such source exists, returns the highest-precedence published sample
  /// so callers can report the exact incomplete/stale reason fail-closed.
  [[nodiscard]] ReadView acquire(std::int64_t now_ns) const noexcept;
  /// Select the highest-precedence published source without evaluating
  /// freshness. This is the immutable latest-publication surface; admission
  /// callers must use acquire(now_ns) instead.
  [[nodiscard]] ReadView acquire_latest() const noexcept;
  /// NRT value-copy surface for bindings/diagnostics. RT consumers use
  /// acquire() so no allocation or mutable storage crosses the hot path.
  [[nodiscard]] std::optional<ObservedRobotStateSnapshotDTO> snapshot(
      std::int64_t now_ns) const;
  [[nodiscard]] std::optional<ObservedRobotStateSnapshotDTO>
  snapshot_latest() const;

  [[nodiscard]] std::span<const std::string> canonical_joint_names() const {
    return canonical_joint_names_;
  }
  [[nodiscard]] const std::string& joint_order_token() const noexcept {
    return joint_order_token_;
  }
  [[nodiscard]] ObservedRobotStatePublicationMetrics metrics() const noexcept;
  [[nodiscard]] std::vector<ObservedRobotStateSourceMetrics> source_metrics()
      const;

 private:
  ReadView acquire_lane(const SourceLane& lane) const noexcept;
  static std::string make_joint_order_token(
      std::span<const std::string> joint_names);

  std::vector<std::string> canonical_joint_names_;
  std::string joint_order_token_;
  std::vector<std::unique_ptr<SourceLane>> lanes_;
  std::size_t max_source_joint_count_ = 0;
  std::atomic<std::uint64_t> next_sequence_{1};
  std::atomic<std::uint64_t> published_total_{0};
  std::atomic<std::uint64_t> rejected_total_{0};
  std::atomic<std::uint64_t> writer_busy_total_{0};
  std::atomic<std::uint64_t> slot_exhausted_total_{0};
  mutable std::atomic<std::uint64_t> reader_retry_exhausted_total_{0};
};

}  // namespace manyforge::core
