#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <manyforge_content_identity/canonical_value.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::headless {

inline constexpr std::string_view kHeadlessRuntimeSnapshotSchemaVersion =
    "manyforge.headless_runtime_snapshot.v3";
inline constexpr std::string_view kHeadlessRuntimeSnapshotIdentityDomain =
    "manyforge-headless-runtime-snapshot";

class RuntimeSnapshotError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// A status stream frame that ends the subscription: an `error` frame (its
/// own code) or an invalid frame (`manager_status_stream_invalid`). The
/// reader reconnects.
class ManagerStatusStreamError final : public std::runtime_error {
public:
  ManagerStatusStreamError(std::string code, const std::string &message)
      : std::runtime_error(message), code_(std::move(code)) {}
  [[nodiscard]] const std::string &code() const noexcept { return code_; }

private:
  std::string code_;
};

struct RuntimeSnapshotFreshnessPolicy final {
  /// The Manager is not answering when no frame showing a new tick of its
  /// main loop arrived for longer than this (or its connection ended).
  std::int64_t manager_silence_ns;
};

struct HeadlessRuntimeSnapshot final {
  std::uint64_t projection_generation;
  std::int64_t projected_at_ns;
  std::string semantic_content_digest;
  manyforge::content_identity::CanonicalValue root;
};

/// The Headless API's one mirror of the Manager's status stream (OD-216):
/// the newest value of each section and whether the Manager answers. Only
/// the stream's follower thread applies frames; every reader takes one
/// atomic snapshot of the view, with no lock.
class ManagerStatusMirror final {
public:
  using Clock = std::function<std::int64_t()>;

  /// Section indexes, in the stream's order.
  static constexpr std::size_t kManager = 0U;
  static constexpr std::size_t kLogicalScene = 1U;
  static constexpr std::size_t kSceneRuntime = 2U;
  static constexpr std::size_t kViewerPoseSessions = 3U;
  static constexpr std::size_t kSectionCount = 4U;

  enum class Connection { kNeverConnected, kConnected, kDisconnected };
  enum class Problem { kNone, kInvalid, kOversized };

  /// One valid section value as received.
  struct Value final {
    std::uint64_t generation{};
    std::int64_t observed_at_ns{};
    manyforge::content_identity::CanonicalValue payload;
    /// The digest today's `source_content_digest` carries.
    std::string semantic_content_digest;
    /// The digest of what the control state shows of this value: without
    /// pose samples, the scene runtime's update time and the Manager's
    /// observation counters (manager, logical scene, scene runtime only).
    std::string control_digest;
    /// Canonical payload JSON, kept for the viewer's two sections only.
    std::string payload_json;
  };
  struct Section final {
    /// The last valid value, kept across a problem or a lost connection.
    std::shared_ptr<const Value> last_valid;
    /// A value or a problem arrived on the current connection.
    bool current{};
    Problem problem{Problem::kNone};
  };
  struct View final {
    Connection connection{Connection::kNeverConnected};
    std::uint64_t tick{};
    /// Monotonic receive time of the last frame that advanced the tick.
    std::int64_t last_tick_advance_ns{};
    std::array<Section, kSectionCount> sections{};
  };
  struct ApplyResult final {
    /// What the control state shows changed: a new subscription, or the
    /// meaning (control digest), problem or currency of the manager,
    /// logical-scene or scene-runtime section.
    bool control_changed{};
    /// A new subscription, or the scene-runtime or pose-session section
    /// changed (the viewer pose stream's sources).
    bool viewer_changed{};
    /// The frame showed a new tick of the Manager's main loop.
    bool tick_advanced{};
  };

  ManagerStatusMirror(std::string cell_id, std::int64_t manager_silence_ns,
                      Clock monotonic_clock_ns);

  ManagerStatusMirror(const ManagerStatusMirror &) = delete;
  ManagerStatusMirror &operator=(const ManagerStatusMirror &) = delete;

  /// Applies one received frame (follower thread only). Throws
  /// `ManagerStatusStreamError` for an `error` frame or an invalid frame.
  ApplyResult Apply(std::string_view frame);
  /// The subscription ended (follower thread only): the Manager is not
  /// answering at once. Returns true when it was connected.
  bool MarkDisconnected();

  [[nodiscard]] std::shared_ptr<const View> Load() const noexcept {
    return view_.load();
  }
  [[nodiscard]] std::int64_t Now() const { return monotonic_clock_ns_(); }
  /// Connected and a new tick arrived within the silence bound.
  [[nodiscard]] bool Answering(const View &view, std::int64_t now_ns) const noexcept;

  /// The viewer's scene-runtime payload and pose sessions (canonical JSON),
  /// while the Manager answers and both are current and valid.
  [[nodiscard]] std::optional<std::pair<std::string, std::string>>
  ViewerPoseSources() const;

private:
  std::string cell_id_;
  std::int64_t manager_silence_ns_;
  Clock monotonic_clock_ns_;
  std::atomic<std::shared_ptr<const View>> view_;
};

/// Read-only projection over the Manager's three status sections (manager,
/// logical scene, scene runtime) as mirrored from its status stream. It
/// retains a stale section's last value without relabelling it as current
/// truth, and never invokes either source owner.
class RuntimeSnapshotAggregator final {
public:
  using Clock = std::function<std::int64_t()>;

  RuntimeSnapshotAggregator(const ManagerStatusMirror &mirror,
                            std::string projection_owner_id,
                            std::string application_start_id,
                            Clock projection_clock_ns);

  [[nodiscard]] HeadlessRuntimeSnapshot Evaluate();

private:
  const ManagerStatusMirror &mirror_;
  std::string projection_owner_id_;
  std::string application_start_id_;
  Clock projection_clock_ns_;
  std::mutex mutex_;
  std::uint64_t projection_generation_{};
  std::string previous_semantic_digest_;
};

} // namespace manyforge::headless
