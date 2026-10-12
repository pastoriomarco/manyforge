#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <builtin_interfaces/msg/time.hpp>
#include <diagnostic_msgs/msg/diagnostic_status.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/joint_state.hpp>

#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_roboplan/isaac_arm_guard.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>

namespace manyforge::roboplan_backend {

/// The host side the Isaac guard binding acts on (the ROS runtime handle in
/// production).
class IRetainedIsaacArmHost {
public:
  virtual ~IRetainedIsaacArmHost() = default;
  /// The current binding epoch's readiness and latest sample, and whether a
  /// goal of the arm is in flight.
  [[nodiscard]] virtual IsaacArmGuardFacts ReadFacts() = 0;
  /// The arm controller's joint order (the guard reports its own).
  [[nodiscard]] virtual std::vector<std::string> ControllerJointNames() = 0;
  /// Item 44: the trip ends motion in flight InterventionRequired.
  virtual void ReportFault(core::ErrorDTO error) = 0;
  /// Q41: the controller is switched off through the CCLC switch path.
  virtual void DeactivateForFault(std::stop_token cancellation) = 0;
};

/// The handle as `IRetainedIsaacArmHost`.
[[nodiscard]] std::shared_ptr<IRetainedIsaacArmHost>
make_retained_isaac_arm_host(std::weak_ptr<RetainedRos2ControlRuntimeHandle> handle);

struct RetainedIsaacArmGuardBindingConfig final {
  /// The arm's joint command topic: one lane-profile value, also the launch
  /// argument. Authorization and status topics are its suffixes.
  std::string joint_commands_topic;
  /// Isaac's own joint states (receipt age only, §4.3).
  std::string joint_states_topic;
  /// Lane value (`main`'s `joint_state_max_age_s`).
  core::Duration joint_state_max_age_ns{0};
  /// Host step period over the arm's facts (the controller state period).
  core::Duration step_period_ns{0};
};

/// Diagnostic view of the binding.
struct RetainedIsaacArmGuardBindingStatus final {
  bool motion_admitted{false};
  bool tripped{false};
  bool activation_allowed{false};
  std::optional<core::TimePoint> pending_authorization_ns{};
  std::optional<core::TimePoint> last_authorization_sent_ns{};
  bool joint_names_mismatch{false};
  bool feedback_stale{false};
  std::uint64_t statuses_received{0U};
  std::uint64_t statuses_rejected{0U};
  std::uint64_t guard_reconnects{0U};
  std::optional<std::string> last_diagnostic{};
};

/// ROS binding of the ROS-free `IsaacArmGuard` (design §4.3, Q13, Q41, Q43,
/// Q46): receipt age of Isaac's joint states, the guard's status
/// (`<command_topic>/guard_status`, reliable, transient-local; decisions on
/// `values` only) and the timestamped authorization
/// (`<command_topic>/authorization`, reliable, volatile).
///
/// One worker processes every observation in arrival order (status, joint
/// states, the periodic step over the arm's facts) and performs the effects in
/// the owner's order: authorization publication; on a trip the fault report,
/// then the controller switch-off. A status from a guard publisher other than
/// the one seen before is preceded by the reconnect handling, so a restarted
/// or rematched guard is never trusted on its first status. A status whose `joint_names`
/// is missing, unparsable or differs from the controller's joint order is not
/// used and blocks motion until a matching status arrives.
class RetainedIsaacArmGuardBinding final
    : public std::enable_shared_from_this<RetainedIsaacArmGuardBinding> {
public:
  [[nodiscard]] static std::shared_ptr<RetainedIsaacArmGuardBinding>
  Create(RetainedIsaacArmGuardBindingConfig config,
         rclcpp::Node::SharedPtr node,
         std::shared_ptr<IRetainedIsaacArmHost> host, core::IClock &clock);

  ~RetainedIsaacArmGuardBinding();
  RetainedIsaacArmGuardBinding(const RetainedIsaacArmGuardBinding &) = delete;
  RetainedIsaacArmGuardBinding &
  operator=(const RetainedIsaacArmGuardBinding &) = delete;

  /// Hooks for the runtime handle (motion gate, activation gate, Recover,
  /// fault cause). They keep this binding alive.
  [[nodiscard]] RetainedRos2ControlArmGuardHooks hooks();

  [[nodiscard]] RetainedIsaacArmGuardBindingStatus status() const;

  /// Parses one guard status into the guard's own terms. nullopt when a
  /// decision value is missing or malformed; `joint_names` separately.
  struct ParsedStatus final {
    std::optional<IsaacGuardStatus> status;
    std::optional<std::vector<std::string>> joint_names;
  };
  [[nodiscard]] static ParsedStatus
  ParseStatus(const diagnostic_msgs::msg::DiagnosticStatus &message);

private:
  struct Event;

  RetainedIsaacArmGuardBinding(RetainedIsaacArmGuardBindingConfig config,
                               std::shared_ptr<IRetainedIsaacArmHost> host,
                               core::IClock &clock);
  void Enqueue(Event event);
  void Run(std::stop_token stop);
  void Apply(const IsaacArmGuardActions &actions, std::stop_token stop);
  void PublishDerivedLocked();

  RetainedIsaacArmGuardBindingConfig config_;
  std::shared_ptr<IRetainedIsaacArmHost> host_;
  core::IClock &clock_;

  mutable std::mutex mutex_;
  std::condition_variable_any wake_;
  std::deque<Event> events_;
  IsaacArmGuard guard_;
  RetainedIsaacArmGuardBindingStatus status_;
  std::optional<std::string> trip_reason_;
  std::optional<std::array<std::uint8_t, RMW_GID_STORAGE_SIZE>> guard_publisher_;

  rclcpp::Publisher<builtin_interfaces::msg::Time>::SharedPtr authorization_;
  rclcpp::Subscription<diagnostic_msgs::msg::DiagnosticStatus>::SharedPtr
      guard_status_;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr joint_states_;
  std::jthread worker_;
};

} // namespace manyforge::roboplan_backend
