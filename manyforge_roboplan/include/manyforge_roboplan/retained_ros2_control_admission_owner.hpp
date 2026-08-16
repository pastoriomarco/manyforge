#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>

namespace manyforge::roboplan_backend {

struct RetainedRos2ControlAdmissionOwnerConfig final {
  core::ControllableComponentId controllable_component_id;
  core::RuntimeEpoch runtime_epoch;
  std::string binding_contract_generation_id;
  std::string provider_component_epoch;
  std::string cell_supervisor_component_epoch;
  core::Duration publication_period_ns{50'000'000};
  core::Duration freshness_ns{250'000'000};

  [[nodiscard]] bool locally_valid() const noexcept;
};

using RetainedCurrentComponentSnapshotSource = std::function<
    core::Result<core::ComponentLifecycleCurrentSnapshotDTO>()>;

struct RetainedRos2ControlAdmissionOwnerStatus final {
  bool running{};
  bool healthy{};
  std::uint64_t publication_generation{};
  std::uint64_t lifecycle_generation{};
  std::uint64_t binding_lease_expirations_deferred_total{};
  core::TimePoint last_publication_ns{};
  std::string reason_code;
};

/// NRT lifecycle/controller join that publishes immutable execution admission
/// to the generic ROS backend. It owns no controller-manager, command-gate,
/// trajectory, gripper, simulator, or Kernel mutation surface.
class RetainedRos2ControlAdmissionOwner final {
public:
  [[nodiscard]] static core::Result<
      std::unique_ptr<RetainedRos2ControlAdmissionOwner>>
  Create(RetainedRos2ControlAdmissionOwnerConfig config,
         std::shared_ptr<RetainedRos2ControlRuntimeHandle> runtime_handle,
         core::IClock &clock,
         RetainedCurrentComponentSnapshotSource component_snapshot_source);

  ~RetainedRos2ControlAdmissionOwner();

  RetainedRos2ControlAdmissionOwner(
      const RetainedRos2ControlAdmissionOwner &) = delete;
  RetainedRos2ControlAdmissionOwner &
  operator=(const RetainedRos2ControlAdmissionOwner &) = delete;

  [[nodiscard]] RetainedRos2ControlAdmissionOwnerStatus status() const;

private:
  RetainedRos2ControlAdmissionOwner(
      RetainedRos2ControlAdmissionOwnerConfig config,
      std::shared_ptr<RetainedRos2ControlRuntimeHandle> runtime_handle,
      core::IClock &clock,
      RetainedCurrentComponentSnapshotSource component_snapshot_source);

  void Run(std::stop_token stop_token) noexcept;

  RetainedRos2ControlAdmissionOwnerConfig config_;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> runtime_handle_;
  core::IClock &clock_;
  RetainedCurrentComponentSnapshotSource component_snapshot_source_;
  mutable std::mutex status_mutex_;
  RetainedRos2ControlAdmissionOwnerStatus status_;
  std::condition_variable_any wakeup_;
  std::mutex wakeup_mutex_;
  std::jthread worker_;
};

} // namespace manyforge::roboplan_backend
