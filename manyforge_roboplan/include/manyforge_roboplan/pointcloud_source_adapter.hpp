#pragma once

#include <cstdint>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/ports/icomponent_adapter_port.hpp>
#include <manyforge_core/types/component_state.hpp>
#include <manyforge_roboplan/component_process.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::roboplan_backend {

/// The point-cloud source component (adapter family `pointcloud`, CONTRACTS
/// §6): a vendor-neutral sensor whose own process (the point-cloud observer
/// bridge) forwards a PointCloud2 topic, in the cell frame, to the 3D view
/// while a viewer is open. Display only. ROS-free here; the ROS side is
/// RosPointCloudBridgeLink.

/// The component's one stream.
inline constexpr std::string_view kPointCloudStreamId = "pointcloud";

/// The effective settings: the model's one adapter row (a sensor has no lane)
/// with the Deployment's settings set over it.
struct PointCloudSourceSettings final {
  /// Absolute PointCloud2 topic.
  std::string topic;
  /// The frame the cloud is published in (the cell frame).
  std::string source_frame_id;
  std::int64_t max_points{};
  double minimum_period_s{};
  /// The `pointcloud` stream is fresh while a cloud arrived this recently.
  double stream_budget_s{};
};

/// Parses the merged adapter row; throws std::invalid_argument when a setting is
/// missing or out of range (the registry reports `component_adapter_invalid`).
[[nodiscard]] PointCloudSourceSettings
pointcloud_source_settings(const content_identity::CanonicalValue &row);

/// The bridge launch of these settings: `pointcloud_source.launch.py`.
[[nodiscard]] ComponentProcessCommand pointcloud_bridge_command(
    const PointCloudSourceSettings &settings, std::string_view ros_namespace,
    std::string_view cell_id);

/// What the bridge reports: whether it is subscribed to the cloud topic (it
/// is only while forwarding and a viewer is open) and when its last cloud
/// arrived (host clock).
struct PointCloudBridgeObservation final {
  bool watching{};
  std::optional<std::int64_t> last_received_ns;
};

/// The bridge's ROS interface, ROS-free.
class IPointCloudBridgeLink {
 public:
  virtual ~IPointCloudBridgeLink() = default;
  /// Allows (true) or stops (false) forwarding (latched for the bridge).
  virtual void SetForwarding(bool forwarding) = 0;
  [[nodiscard]] virtual PointCloudBridgeObservation Observed() const = 0;
};

/// The component's state: no process → `not_prepared`; an exited process →
/// `fault_process_exited` (Recover restarts it); else `enabled` while
/// forwarding, `prepared` otherwise. Activity is always idle (no requests).
/// The `pointcloud` stream is freshness only, never a fault (CONTRACTS §7):
/// `none` while the bridge isn't watching, `fresh` while the last cloud is
/// within the budget, `stale` otherwise.
[[nodiscard]] core::ComponentState PointCloudSourceState(
    bool process_started, const std::optional<std::string> &exit_reason, bool forwarding,
    const PointCloudBridgeObservation &observed, std::int64_t stream_budget_ns,
    std::int64_t now_ns);

/// The adapter: `prepare`/`recover` (re)start the bridge with forwarding off,
/// `enable`/`disable` switch forwarding. Nothing waits or retries.
class PointCloudSourceAdapter final : public core::IComponentAdapter {
 public:
  PointCloudSourceAdapter(PointCloudSourceSettings settings, std::string ros_namespace,
                          std::string cell_id, ComponentProcessLauncher start_process,
                          std::shared_ptr<IPointCloudBridgeLink> link);

  [[nodiscard]] core::ComponentState State(std::int64_t now_ns) const override;
  [[nodiscard]] core::ActionResult Run(core::ComponentAction action,
                                       std::stop_token stop) override;
  [[nodiscard]] std::vector<core::Capability> capabilities() const override;

 private:
  PointCloudSourceSettings settings_;
  std::string ros_namespace_;
  std::string cell_id_;
  ComponentProcessLauncher start_process_;
  std::shared_ptr<IPointCloudBridgeLink> link_;
  mutable std::mutex mutex_;
  std::unique_ptr<IComponentProcess> process_;
  bool forwarding_{};
};

}  // namespace manyforge::roboplan_backend
