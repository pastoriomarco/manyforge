#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <manyforge_roboplan/retained_externally_managed_lifecycle_ports.hpp>
#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>

namespace manyforge::roboplan_backend {

/// Weak, read-only product handle for the ROS adapter selected by one retained
/// runtime composition.
///
/// The backend factory binds this handle exactly once. Lifecycle/host NRT work
/// may then read immutable binding/transport snapshots and publish CCLC-owned
/// execution admission. The handle exposes no trajectory, gripper, command
/// gate, controller-manager, or simulator mutation surface.
class RetainedRos2ControlRuntimeHandle final
    : public std::enable_shared_from_this<RetainedRos2ControlRuntimeHandle> {
public:
  RetainedRos2ControlRuntimeHandle() = default;

  RetainedRos2ControlRuntimeHandle(const RetainedRos2ControlRuntimeHandle &) =
      delete;
  RetainedRos2ControlRuntimeHandle &
  operator=(const RetainedRos2ControlRuntimeHandle &) = delete;

  [[nodiscard]] core::Result<void> BindBackend(
      std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend>
          backend,
      const core::RuntimeParticipantBinding &motion_binding,
      const std::optional<core::RuntimeParticipantBinding> &gripper_binding);

  [[nodiscard]] RetainedExternallyManagedDomainSnapshotSource
  lifecycle_snapshot_source();

  [[nodiscard]] core::Result<RetainedExternallyManagedDomainSnapshot>
  ReadLifecycleSnapshot(const core::ControlDomainId &control_domain_id,
                        core::TimePoint now_ns);

  [[nodiscard]] bool publish_execution_admission_snapshot(
      ros2_control_execution::ExecutionAdmissionSnapshot snapshot);

  [[nodiscard]] std::optional<
      ros2_control_execution::ExecutionAdmissionSnapshot>
  execution_admission_snapshot() const;

  [[nodiscard]] std::optional<ros2_control_execution::ControllerBindingSnapshot>
  controller_binding_snapshot() const;

  /// Read-only transport diagnostics for the exactly bound gripper target.
  /// Absence means this handle has no live gripper binding/backend.
  [[nodiscard]] std::optional<
      ros2_control_execution::Ros2GripperTransportReadinessSnapshot>
  gripper_transport_readiness_snapshot() const;

private:
  struct DomainGenerationState final {
    std::uint64_t publication_generation{};
    std::string source_signature;
  };

  mutable std::mutex mutex_;
  std::weak_ptr<ros2_control_execution::Ros2ControlExecutionBackend> backend_;
  std::optional<core::ControlDomainId> motion_domain_id_;
  std::optional<core::ControlDomainId> gripper_domain_id_;
  std::optional<std::string> gripper_id_;
  std::map<core::ControlDomainId, DomainGenerationState> domain_generations_;
};

} // namespace manyforge::roboplan_backend
