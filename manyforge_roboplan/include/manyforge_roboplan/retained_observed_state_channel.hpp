#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include <manyforge_core/addressed_observed_state_publication.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_roboplan/retained_kinematic_state_projector.hpp>

namespace manyforge::roboplan_backend {

struct RetainedObservedStateChannelConfig final {
  core::ObservedStateSourceKind source_kind{
      core::ObservedStateSourceKind::kPredictedState};
  std::string source_id;
  std::int64_t freshness_budget_ns{};
  std::uint64_t authority_generation{1U};
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

  [[nodiscard]] virtual RetainedStateProjectionResult publish_state(
      std::span<const double> positions_si,
      std::span<const double> velocities_si,
      std::int64_t source_timestamp_ns,
      std::int64_t received_timestamp_ns) = 0;
  [[nodiscard]] virtual RetainedStateProjectionResult project_latest(
      std::int64_t now_ns, RetainedStateProjectionMode mode) = 0;
  [[nodiscard]] virtual RetainedStateProjectionResult publish_and_project(
      std::span<const double> positions_si,
      std::span<const double> velocities_si,
      std::int64_t source_timestamp_ns,
      std::int64_t received_timestamp_ns,
      RetainedStateProjectionMode mode) = 0;
  [[nodiscard]] virtual core::Result<core::ObservedStateSnapshotDTO>
  current_state(std::int64_t now_ns) const = 0;
  [[nodiscard]] virtual core::Result<core::ControlledStateDTO>
  current_controlled_state() const = 0;
  [[nodiscard]] virtual const core::ObservedStateQueryContextDTO&
  query_context() const noexcept = 0;
  [[nodiscard]] virtual const core::AddressedObservedStatePublication*
  addressed_publication() const noexcept = 0;
};

class RetainedObservedStateChannel final
    : public IRetainedObservedStateChannel {
 public:
  RetainedObservedStateChannel(
      core::RuntimeParticipantBinding binding,
      std::shared_ptr<RetainedKinematicStateProjector> projector,
      RetainedObservedStateChannelConfig config);

  RetainedObservedStateChannel(const RetainedObservedStateChannel&) = delete;
  RetainedObservedStateChannel& operator=(
      const RetainedObservedStateChannel&) = delete;

  /// Seed this channel from the exact retained scene state. This is the only
  /// supported native bootstrap path; callers cannot supply unnamed vectors.
  [[nodiscard]] RetainedStateProjectionResult initialize_from_projection(
      std::int64_t timestamp_ns);

  /// Publish exact target-order SI values and project them into the shared
  /// retained model. The source kind and target namespace are construction-time
  /// facts and cannot be changed per call.
  [[nodiscard]] RetainedStateProjectionResult publish_and_project(
      std::span<const double> positions_si,
      std::span<const double> velocities_si,
      std::int64_t source_timestamp_ns,
      std::int64_t received_timestamp_ns,
      RetainedStateProjectionMode mode =
          RetainedStateProjectionMode::kBlocking) override;

  [[nodiscard]] RetainedStateProjectionResult publish_state(
      std::span<const double> positions_si,
      std::span<const double> velocities_si,
      std::int64_t source_timestamp_ns,
      std::int64_t received_timestamp_ns) override;
  [[nodiscard]] RetainedStateProjectionResult project_latest(
      std::int64_t now_ns, RetainedStateProjectionMode mode) override;

  [[nodiscard]] core::Result<core::ObservedStateSnapshotDTO> current_state(
      std::int64_t now_ns) const override;
  [[nodiscard]] core::Result<core::ControlledStateDTO>
  current_controlled_state() const override;

  [[nodiscard]] const core::RuntimeParticipantBinding& binding()
      const noexcept {
    return binding_;
  }
  [[nodiscard]] const core::ObservedStateQueryContextDTO& query_context()
      const noexcept override {
    return publication_.query_context();
  }
  [[nodiscard]] const core::AddressedObservedStatePublication& publication()
      const noexcept {
    return publication_;
  }
  [[nodiscard]] const core::AddressedObservedStatePublication*
  addressed_publication() const noexcept override {
    return &publication_;
  }

 private:
  core::RuntimeParticipantBinding binding_;
  std::shared_ptr<RetainedKinematicStateProjector> projector_;
  RetainedObservedStateChannelConfig config_;
  core::AddressedObservedStatePublication publication_;
};

}  // namespace manyforge::roboplan_backend
