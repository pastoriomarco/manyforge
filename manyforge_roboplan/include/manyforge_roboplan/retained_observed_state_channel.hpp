#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <semaphore>
#include <shared_mutex>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <manyforge_core/addressed_observed_state_publication.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_roboplan/retained_kinematic_state_projector.hpp>

namespace manyforge::roboplan_backend {

/// Shared exact native realization check. It does not replace current route
/// reconciliation, qualified-capability selection or observation freshness.
[[nodiscard]] bool retained_bindings_share_backend_owner(
    const core::RuntimeParticipantBinding& primary,
    const core::RuntimeParticipantBinding& representative) noexcept;

class RetainedObservedStateChannelTestAccess;

/// One coalesced notification for the composition's single native viewer
/// reader. There is no sample queue and publishers never wait for that reader.
/// Browser fanout remains outside the execution process.
class RetainedObservationWake final {
public:
  RetainedObservationWake();
  [[nodiscard]] std::uint64_t incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] std::uint64_t generation() const noexcept {
    return generation_.load(std::memory_order_acquire);
  }
  void notify() noexcept;
  [[nodiscard]] bool wait_after(std::uint64_t generation,
                                std::chrono::nanoseconds timeout) const;

private:
  const std::uint64_t incarnation_;
  std::atomic<std::uint64_t> generation_{};
  mutable std::atomic<bool> pending_{};
  mutable std::binary_semaphore wake_{0};
};

struct RetainedObservedStateChannelConfig final {
  RetainedObservedStateChannelConfig() = default;

  RetainedObservedStateChannelConfig(
      core::ObservedStateSourceKind selected_source_kind,
      std::string selected_source_id, std::int64_t selected_freshness_budget_ns,
      std::uint64_t selected_authority_generation,
      std::vector<core::ObservedRobotStateSourceConfig>
          selected_source_precedence = {})
      : source_kind(selected_source_kind),
        source_id(std::move(selected_source_id)),
        freshness_budget_ns(selected_freshness_budget_ns),
        authority_generation(selected_authority_generation),
        source_precedence(std::move(selected_source_precedence)) {}

  core::ObservedStateSourceKind source_kind{
      core::ObservedStateSourceKind::kPredictedState};
  std::string source_id;
  std::int64_t freshness_budget_ns{};
  std::uint64_t authority_generation{1U};
  /// Optional exact authority-compatible source precedence. Empty preserves
  /// the native single-source default. Measured diagnostic sources belonging
  /// to a commanded adapter are intentionally not valid here.
  std::vector<core::ObservedRobotStateSourceConfig> source_precedence;
};

/// One route-consistent observation: the channel's current binding and the
/// state sample selected through that same binding, taken under one route
/// fence. A route rebind can never interleave between the two.
struct RetainedBoundObservedState final {
  core::RuntimeParticipantBinding binding;
  core::ObservedStateSnapshotDTO state;
};

/// One exact native state-authority channel for a sealed runtime participant.
///
/// Publication is committed before best-effort scene projection, so execution
/// and safety readers retain the latest owner-issued state even when the
/// mutable RoboPlan visualization/planning scene is temporarily contended.
/// A pure component-local participant has no projector: its addressed state is
/// still owner-issued, but it never fabricates a URDF coordinate or mutates the
/// retained kinematic scene.
/// Linear coordinates remain metres and angular coordinates remain radians at
/// this boundary; only an explicitly constructed all-angular compatibility
/// adapter may expose the historical RobotStateDTO view.
class IRetainedObservedStateChannel {
public:
  virtual ~IRetainedObservedStateChannel() = default;

  /// A component owner was retired under the cell publication fence. Wake
  /// readers so they reselect the current product instead of waiting on an
  /// observation source that can no longer publish.
  virtual void notify_composition_replaced() noexcept {}

  [[nodiscard]] virtual RetainedStateProjectionResult
  publish_state(std::span<const double> positions_si,
                std::span<const double> velocities_si,
                std::int64_t source_timestamp_ns,
                std::int64_t received_timestamp_ns) = 0;
  /// Stop-stage callers use this bounded adapter seam. Implementations must
  /// not wait past deadline_ns or rely on worker abandonment.
  [[nodiscard]] virtual RetainedStateProjectionResult publish_state_bounded(
      std::span<const double> positions_si,
      std::span<const double> velocities_si, std::int64_t source_timestamp_ns,
      std::int64_t received_timestamp_ns, std::int64_t deadline_ns,
      std::stop_token cancellation) {
    if (cancellation.stop_requested() ||
        (deadline_ns >= 0 && received_timestamp_ns >= deadline_ns)) {
      return {RetainedStateProjectionStatus::kFailed,
              core::ErrorDTO{
                  "invalid_request",
                  cancellation.stop_requested()
                      ? "retained state publication was cancelled"
                      : "retained state publication missed its deadline",
                  {{"reason",
                    cancellation.stop_requested()
                        ? "retained_state_publication_cancelled"
                        : "retained_state_publication_deadline_expired"}}}};
    }
    static_cast<void>(positions_si);
    static_cast<void>(velocities_si);
    static_cast<void>(source_timestamp_ns);
    static_cast<void>(received_timestamp_ns);
    return {
        RetainedStateProjectionStatus::kFailed,
        core::ErrorDTO{
            "invalid_request",
            "bounded retained state publication is "
            "not available",
            {{"reason", "retained_state_bounded_publication_unavailable"}}}};
  }
  [[nodiscard]] virtual RetainedStateProjectionResult
  project_latest(std::int64_t now_ns, RetainedStateProjectionMode mode) = 0;
  [[nodiscard]] virtual RetainedStateProjectionResult publish_and_project(
      std::span<const double> positions_si,
      std::span<const double> velocities_si, std::int64_t source_timestamp_ns,
      std::int64_t received_timestamp_ns, RetainedStateProjectionMode mode) = 0;
  [[nodiscard]] virtual core::Result<core::ObservedStateSnapshotDTO>
  current_state(std::int64_t now_ns) const = 0;
  [[nodiscard]] virtual core::Result<core::ObservedStateSnapshotDTO>
  current_state_bounded(std::int64_t now_ns, std::int64_t deadline_ns,
                        std::stop_token cancellation) const {
    if (cancellation.stop_requested() ||
        (deadline_ns >= 0 && now_ns >= deadline_ns)) {
      return core::Result<core::ObservedStateSnapshotDTO>::Err(core::ErrorDTO{
          "invalid_request",
          cancellation.stop_requested()
              ? "retained state read was cancelled"
              : "retained state read missed its deadline",
          {{"reason", cancellation.stop_requested()
                          ? "retained_state_read_cancelled"
                          : "retained_state_read_deadline_expired"}}});
    }
    static_cast<void>(now_ns);
    return core::Result<core::ObservedStateSnapshotDTO>::Err(core::ErrorDTO{
        "invalid_request",
        "bounded retained state read is not available",
        {{"reason", "retained_state_bounded_read_unavailable"}}});
  }
  [[nodiscard]] virtual core::Result<core::ControlledStateDTO>
  current_controlled_state() const = 0;
  [[nodiscard]] virtual core::ObservedStateQueryContextDTO
  query_context() const = 0;
  [[nodiscard]] virtual const core::AddressedObservedStatePublication *
  addressed_publication() const noexcept = 0;
  /// Wait for a source-owned sample after a route rebind. The default keeps
  /// older focused sources fail-closed rather than fabricating freshness.
  [[nodiscard]] virtual bool
  wait_for_fresh_state(std::chrono::nanoseconds timeout) const {
    static_cast<void>(timeout);
    return false;
  }
  /// Monotonic, process-local wake identity for render-only latest-state
  /// consumers. Repeated freshness samples at identical coordinates do not
  /// advance it. It is not a robot-state sequence or a durable cursor.
  [[nodiscard]] virtual std::uint64_t
  render_change_generation() const noexcept {
    return 0U;
  }
  /// Wait until visible coordinates or their route advance beyond
  /// ``after_generation``.
  /// Implementations perform no state read, projection, encoding, or fanout
  /// while waiting.  The default keeps focused fake channels non-blocking.
  [[nodiscard]] virtual bool
  wait_for_render_change(std::uint64_t after_generation,
                         std::chrono::nanoseconds timeout) const {
    static_cast<void>(after_generation);
    static_cast<void>(timeout);
    return false;
  }
};

class RetainedObservedStateChannel final
    : public IRetainedObservedStateChannel {
public:
  RetainedObservedStateChannel(
      core::RuntimeParticipantBinding binding,
      std::shared_ptr<RetainedKinematicStateProjector> projector,
      RetainedObservedStateChannelConfig config, core::IClock *clock = nullptr,
      std::shared_ptr<RetainedObservationWake> render_wake = nullptr);

  RetainedObservedStateChannel(const RetainedObservedStateChannel &) = delete;
  RetainedObservedStateChannel &
  operator=(const RetainedObservedStateChannel &) = delete;

  /// Seed this channel from the exact retained scene state. This is the only
  /// supported native bootstrap path; callers cannot supply unnamed vectors.
  [[nodiscard]] RetainedStateProjectionResult
  initialize_from_projection(std::int64_t timestamp_ns);

  /// Rebind this stable physical source to one verified route/configuration
  /// identity. The caller must hold the Kernel route fence; a legitimate
  /// same-owner commanded/predicted rebind republishes the complete seed at
  /// the transition time before exposing the new binding.
  [[nodiscard]] core::Result<void>
  RebindRoute(core::RuntimeParticipantBinding binding,
              std::int64_t transition_time_ns);

  /// Retarget this stable physical source to the state projector of a
  /// candidate whole-cell model generation. The caller holds the Apply fence
  /// and has already rebound the route, so the candidate must register this
  /// channel's current publication context exactly once. In-flight
  /// projections finish on the predecessor first; the latest owner sample is
  /// then projected into the candidate scene and must apply.
  [[nodiscard]] core::Result<void> ReplaceKinematicProjector(
      std::shared_ptr<RetainedKinematicStateProjector> candidate,
      std::int64_t now_ns);

  /// The projector currently selected for model projection; empty for a
  /// component-local source.
  [[nodiscard]] std::shared_ptr<const RetainedKinematicStateProjector>
  kinematic_projector() const;

  /// Publish exact target-order SI values and project them into the shared
  /// retained model. The source kind and target namespace are construction-time
  /// facts and cannot be changed per call.
  [[nodiscard]] RetainedStateProjectionResult
  publish_and_project(std::span<const double> positions_si,
                      std::span<const double> velocities_si,
                      std::int64_t source_timestamp_ns,
                      std::int64_t received_timestamp_ns,
                      RetainedStateProjectionMode mode =
                          RetainedStateProjectionMode::kBlocking) override;

  [[nodiscard]] RetainedStateProjectionResult
  publish_state(std::span<const double> positions_si,
                std::span<const double> velocities_si,
                std::int64_t source_timestamp_ns,
                std::int64_t received_timestamp_ns) override;
  /// Adapter-owned publication facade. Unlike numeric_publication_writer(),
  /// this path also clears the channel's route-freshness gate and wakes
  /// bounded recovery readers.
  [[nodiscard]] core::ObservedRobotStatePublishResult
  publish_adapter_state(core::ObservedRobotStateSourceKind source,
                        std::span<const std::string> source_joint_names,
                        std::span<const double> positions_si,
                        std::span<const double> velocities_si,
                        std::int64_t source_timestamp_ns,
                        std::int64_t received_timestamp_ns);
  [[nodiscard]] RetainedStateProjectionResult publish_state_bounded(
      std::span<const double> positions_si,
      std::span<const double> velocities_si, std::int64_t source_timestamp_ns,
      std::int64_t received_timestamp_ns, std::int64_t deadline_ns,
      std::stop_token cancellation) override;
  [[nodiscard]] RetainedStateProjectionResult
  project_latest(std::int64_t now_ns,
                 RetainedStateProjectionMode mode) override;

  [[nodiscard]] core::Result<core::ObservedStateSnapshotDTO>
  current_state(std::int64_t now_ns) const override;
  /// Render/observation readers use this instead of pairing an owner binding
  /// read with a later current_state(): Apply may rebind the route between
  /// two separate reads.
  [[nodiscard]] core::Result<RetainedBoundObservedState>
  current_bound_state(std::int64_t now_ns) const;
  [[nodiscard]] core::Result<core::ObservedStateSnapshotDTO>
  current_state_bounded(std::int64_t now_ns, std::int64_t deadline_ns,
                        std::stop_token cancellation) const override;
  [[nodiscard]] core::Result<core::ControlledStateDTO>
  current_controlled_state() const override;
  [[nodiscard]] bool
  wait_for_fresh_state(std::chrono::nanoseconds timeout) const override;
  [[nodiscard]] std::uint64_t
  render_change_generation() const noexcept override {
    return render_wake_->generation();
  }
  // Called under the host's composition replacement fence. A waiting viewer
  // must recapture the new product (or its absence), even while we are idle.
  void notify_composition_replaced() noexcept override {
    render_wake_->notify();
  }
  [[nodiscard]] std::uint64_t render_incarnation() const noexcept {
    return render_wake_->incarnation();
  }
  [[nodiscard]] bool
  wait_for_render_change(std::uint64_t after_generation,
                         std::chrono::nanoseconds timeout) const override;

  [[nodiscard]] core::RuntimeParticipantBinding binding() const {
    std::lock_guard lock(route_rebind_mutex_);
    return current_binding_;
  }
  [[nodiscard]] core::ObservedStateSourceKind source_kind() const noexcept {
    return config_.source_kind;
  }
  [[nodiscard]] core::ObservedStateQueryContextDTO
  query_context() const override {
    return publication_.query_context();
  }
  [[nodiscard]] const core::AddressedObservedStatePublication &
  publication() const noexcept {
    return publication_;
  }
  [[nodiscard]] core::AddressedObservedStatePublication &
  addressed_publication_writer() noexcept {
    return publication_;
  }
  [[nodiscard]] core::ObservedRobotStatePublication &
  numeric_publication_writer() noexcept {
    return publication_.numeric_publication_writer();
  }
  [[nodiscard]] const core::AddressedObservedStatePublication *
  addressed_publication() const noexcept override {
    return &publication_;
  }

private:
  friend class RetainedObservedStateChannelTestAccess;

  // One selected projector and the exact context it registered for this
  // source. Replacement swaps both together under projection_mutex_.
  struct KinematicProjection final {
    std::shared_ptr<RetainedKinematicStateProjector> projector;
    core::ObservedStateQueryContextDTO registered_context;
  };

  core::RuntimeParticipantBinding current_binding_;
  mutable std::shared_mutex projection_mutex_;
  std::shared_ptr<const KinematicProjection> projection_;
  RetainedObservedStateChannelConfig config_;
  core::IClock *clock_{};
  core::AddressedObservedStatePublication publication_;
  core::ObservedStateQueryContextDTO projection_context_;
  mutable std::mutex route_rebind_mutex_;
  mutable std::mutex fresh_state_mutex_;
  mutable std::condition_variable fresh_state_cv_;
  std::atomic<bool> fresh_state_required_{};
  std::atomic<std::uint64_t> fresh_sample_generation_{};
  std::shared_ptr<RetainedObservationWake> render_wake_;
  std::vector<double> last_render_positions_;
  bool render_positions_valid_{};

  void record_render_positions(std::span<const std::string> source_joint_names,
                               std::span<const double> positions_si);
  void record_render_unavailable() noexcept;
  void notify_render_change() noexcept;
};

} // namespace manyforge::roboplan_backend
