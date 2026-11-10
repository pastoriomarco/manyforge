#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/ports/icomponent_adapter_port.hpp>
#include <manyforge_core/time/iclock.hpp>
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

/// The assistant component (adapter family `assistant`): the Composer AI chat
/// bridge, run by the component's own process (the assistant host), which
/// owns the backend (`local_process` first). Backend-neutral and ROS-free
/// here; the ROS side is RosAssistantHostLink.

/// The component's one stream: a `/health` answer from the bridge.
inline constexpr std::string_view kAssistantStreamId = "bridge";
/// The endpoint role the bridge serves (provider manifest
/// `manyforge.provider.assistant_bridge`).
inline constexpr std::string_view kAssistantRequestRole = "assistant_request";
/// How often Enable and Disable read the host's report.
inline constexpr std::chrono::milliseconds kAssistantPollPeriod{20};

/// The effective settings: the model's one adapter row (the assistant has no
/// lane) with the Deployment's settings set over it. The row's model keys
/// (`assistant_profile_id`, `model_*`) are there only so that changing them
/// replaces the component; the adapter doesn't read them.
struct AssistantSettings final {
  /// The host's backend (`local_process`).
  std::string backend;
  /// How often the host polls the bridge's `/health` while serving.
  double health_period_s{};
  /// The `bridge` stream is fresh while a `/health` answer arrived this recently.
  double stream_budget_s{};
};

/// Parses the merged adapter row; throws std::invalid_argument when a setting is
/// missing or out of range (the registry reports `component_adapter_invalid`).
[[nodiscard]] AssistantSettings assistant_settings(const content_identity::CanonicalValue &row);

/// The host launch of these settings: `assistant.launch.py`.
[[nodiscard]] ComponentProcessCommand assistant_host_command(const AssistantSettings &settings,
                                                             std::string_view ros_namespace);

/// What the host reports: the bridge's URL while it serves (empty
/// otherwise), whether a chat turn runs, and when its last `/health` answer
/// arrived (host clock).
struct AssistantHostObservation final {
  std::string endpoint;
  bool busy{};
  std::optional<std::int64_t> last_health_ns;
};

/// The host's ROS interface, ROS-free.
class IAssistantHostLink {
 public:
  virtual ~IAssistantHostLink() = default;
  /// Asks the host to serve (true) or stop serving (false) (latched for the host).
  virtual void SetServing(bool serving) = 0;
  [[nodiscard]] virtual AssistantHostObservation Observed() const = 0;
};

/// The component's state: no process → `not_prepared`; an exited process →
/// `fault_process_exited` (Recover restarts it); else `enabled` while the last
/// completed Enable stands and the host reports an endpoint, `prepared`
/// otherwise. Activity is `busy` while enabled and a turn runs, else idle.
/// The `bridge` stream is freshness only, never a fault: `none` unless
/// enabled, `fresh` while the last `/health` answer is within the budget,
/// `stale` otherwise. `endpoints` carries the bridge's URL only while enabled.
[[nodiscard]] core::ComponentState AssistantComponentState(
    bool process_started, const std::optional<std::string> &exit_reason, bool serving,
    const AssistantHostObservation &observed, std::int64_t stream_budget_ns,
    std::int64_t now_ns);

/// The adapter: `prepare`/`recover` (re)start the host with serving off;
/// `enable`/`disable` switch serving and wait, on the action's thread, until
/// the host reports the bridge serving or stopped, within the component's
/// lifecycle command time from the action's start.
class AssistantComponentAdapter final : public core::IComponentAdapter {
 public:
  /// `pause` runs between two reads of the host's report (production:
  /// sleeps kAssistantPollPeriod).
  AssistantComponentAdapter(AssistantSettings settings, std::string ros_namespace,
                            ComponentProcessLauncher start_process,
                            std::shared_ptr<IAssistantHostLink> link,
                            std::shared_ptr<core::IClock> clock,
                            std::chrono::nanoseconds lifecycle_command_timeout,
                            std::function<void()> pause = {});

  [[nodiscard]] core::ComponentState State(std::int64_t now_ns) const override;
  [[nodiscard]] core::ActionResult Run(core::ComponentAction action,
                                       std::stop_token stop) override;
  [[nodiscard]] std::vector<core::Capability> capabilities() const override;

 private:
  AssistantSettings settings_;
  std::string ros_namespace_;
  ComponentProcessLauncher start_process_;
  std::shared_ptr<IAssistantHostLink> link_;
  std::shared_ptr<core::IClock> clock_;
  std::chrono::nanoseconds lifecycle_command_timeout_;
  std::function<void()> pause_;
  mutable std::mutex mutex_;
  std::shared_ptr<IComponentProcess> process_;
  /// The last completed Enable (true) or Disable (false).
  bool serving_{};
};

}  // namespace manyforge::roboplan_backend
