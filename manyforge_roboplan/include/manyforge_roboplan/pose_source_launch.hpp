#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_roboplan/controller_launch_process.hpp>

namespace manyforge::roboplan_backend {

/// CP9: the supervised observer process of one `pose_source` Deployment
/// component (sim-PLC pattern). ManyForge writes the observer's site document
/// from the Deployment row and launches `pose_source.launch.py`; it never
/// waits for the Manager or any endpoint (the Manager may be waiting on this
/// host). ROS-free.

inline constexpr std::string_view kPoseSourceSiteApiVersion =
    "manyforge.isaac_pose_observer_site.v1";
inline constexpr std::string_view kPoseSourceSiteFileName = "pose_source.json";
/// The observer's exit status on a configuration error.
inline constexpr int kPoseSourceConfigInvalidExitStatus = 78;
inline constexpr std::string_view kPoseSourceExitedCode = "pose_source_exited";
inline constexpr std::string_view kPoseSourceConfigInvalidCode =
    "pose_source_config_invalid";

/// The same observer pattern for one `ai_model` component: its probe
/// (`ai_model.launch.py`) checks that the external endpoint lists the model
/// and feeds the component's one `endpoint` stream.
inline constexpr std::string_view kAiModelSiteApiVersion =
    "manyforge.ai_model_probe_site.v1";
inline constexpr std::string_view kAiModelSiteFileName = "ai_model.json";
inline constexpr std::string_view kAiModelExitedCode = "ai_model_exited";
inline constexpr std::string_view kAiModelConfigInvalidCode =
    "ai_model_config_invalid";
/// Default budget of the `endpoint` stream. The probe runs every
/// default budget/5 (2 s), so a Deployment budget override never changes the
/// probe's launch identity; overrides below the period read as stale.
inline constexpr std::int64_t kAiModelStreamBudgetNs = 10'000'000'000LL;
inline constexpr std::int64_t kAiModelProbePeriodMs =
    kAiModelStreamBudgetNs / 5 / 1'000'000;

/// The plan of one observer process: `lane` kPoseSource or kAiModel.
struct PoseSourcePlan final {
  ControllerLaunchLane lane{ControllerLaunchLane::kPoseSource};
  std::string component_id;
  /// Exact canonical JSON bytes of the observer site document.
  std::string document;
  /// `<execution_run_root>/controller-launch-artifacts/<component>`.
  std::filesystem::path artifacts_root;
  /// `<artifacts_root>/pose_source.json` (`ai_model.json`).
  std::filesystem::path site_config_path;
  /// Pose source only (empty for an AI model probe).
  std::filesystem::path manager_run_root;

  friend bool operator==(const PoseSourcePlan &,
                         const PoseSourcePlan &) = default;
};

/// The observer document: `api_version`, `cell_id`, `source_id` = the
/// component id, `joint_state` (null when absent) and every other member of
/// the Deployment row's `observer_configuration`.
[[nodiscard]] std::string pose_source_observer_document(
    std::string_view cell_id, std::string_view component_id,
    const content_identity::CanonicalValue &observer_configuration);

[[nodiscard]] PoseSourcePlan
plan_pose_source(std::string_view cell_id, std::string component_id,
                 const content_identity::CanonicalValue &observer_configuration,
                 const std::filesystem::path &execution_run_root,
                 std::filesystem::path manager_run_root);

/// The probe document: `api_version`, `cell_id`, `component_id`,
/// `stream_id`, `base_url`, `model`, `api_key_env` (null when absent) and
/// `probe_period_ms`.
[[nodiscard]] std::string ai_model_probe_document(
    std::string_view cell_id, std::string_view component_id,
    std::string_view stream_id, std::string_view base_url,
    std::string_view model, const std::optional<std::string> &api_key_env,
    std::int64_t probe_period_ms);

/// The probe plan (`probe_period_ms` = kAiModelProbePeriodMs).
[[nodiscard]] PoseSourcePlan
plan_ai_model(std::string_view cell_id, std::string component_id,
              std::string_view stream_id, std::string_view base_url,
              std::string_view model,
              const std::optional<std::string> &api_key_env,
              const std::filesystem::path &execution_run_root);

/// Writes `plan.document` to `plan.site_config_path` (directories created).
void write_pose_source_document(const PoseSourcePlan &plan);

/// `ros2 launch manyforge_ros_bringup pose_source.launch.py
/// site_config_path:=… manager_run_root:=… source_epoch_base:=…`, kept across
/// Apply on equal identity (the document digest and the Manager run root; the
/// per-spawn epoch base is not part of it). An AI model plan launches
/// `ai_model.launch.py site_config_path:=…` (identity: the document digest;
/// `source_epoch_base` is unused).
[[nodiscard]] ControllerLaunchCommand
pose_source_launch_command(const PoseSourcePlan &plan,
                           const std::string &source_epoch_base);

/// A running observer outlives a rebuild of its component exactly when the
/// candidate plans the same lane and launch identity (as the simulated PLC).
[[nodiscard]] bool
pose_source_launch_survives(ControllerLaunchProcess *process,
                            const std::optional<PoseSourcePlan> &candidate);

/// The typed fault of an exited observer: `pose_source_config_invalid` with
/// the output tail (detail `output_tail`) for exit status 78 (directly, or as
/// `ros2 launch` reports its child), otherwise `pose_source_exited`; for
/// kAiModel `ai_model_config_invalid` / `ai_model_exited`.
[[nodiscard]] core::ErrorDTO
pose_source_exit_fault(std::string reason, std::optional<int> exit_status,
                       std::string_view output_tail,
                       ControllerLaunchLane lane = ControllerLaunchLane::kPoseSource);

/// The driver's supervised-process fault source for `process` (its lane
/// picks the codes).
[[nodiscard]] std::function<std::optional<core::ErrorDTO>()>
pose_source_fault_source(std::shared_ptr<ControllerLaunchProcess> process);

} // namespace manyforge::roboplan_backend
