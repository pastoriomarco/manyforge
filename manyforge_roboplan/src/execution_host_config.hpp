#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_component_manager/site_settings.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/application_identity.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>

namespace manyforge::roboplan_backend::host_detail {

namespace component_manager = manyforge::component_manager;
namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

using content_identity::CanonicalValue;

inline constexpr std::string_view kCommandApiVersion =
    "manyforge.execution_command.v4";
inline constexpr std::string_view kResponseApiVersion =
    "manyforge.execution_command_response.v6";
inline constexpr std::string_view kSocketFileName = "execution-command.sock";
inline constexpr std::string_view kControlSocketFileName = "execution-control.sock";
inline constexpr std::string_view kObservationSocketFileName =
    "execution-observation.sock";
/// Component actions, the component model and operator-alarm attention.
inline constexpr std::string_view kComponentSocketFileName =
    "execution-component.sock";
/// Event subscription stream: one subscription per connection.
inline constexpr std::string_view kEventStreamSocketFileName = "execution-events.sock";
inline constexpr std::string_view kManagerSocketFileName = "manager-command.sock";
inline constexpr std::string_view kManagerCommandApiVersion =
    "manyforge.manager_command.v2";
inline constexpr std::string_view kManagerResponseApiVersion =
    "manyforge.manager_command_response.v3";
inline constexpr std::string_view kLockFileName = ".execution-host.lock";
inline constexpr std::size_t kMaximumPacketBytes = 1024U * 1024U;
inline constexpr std::size_t kMaximumResponseBytes =
    core::kMaximumCellAdmissionJsonResponseBytes;
inline constexpr std::size_t kMaximumManagerResponseBytes = 512U * 1024U;
inline constexpr std::size_t kMaximumIdentityBytes = 2048U;
inline constexpr std::size_t kMaximumReasonBytes = 4096U;
inline constexpr std::size_t kMaximumEventPage = 16U;
inline constexpr double kMaximumSafeInteger = 9'007'199'254'740'991.0;
inline constexpr std::size_t kRandomTokenBytes = 16U;
inline constexpr int kManagedPackageRootFd = 6;
inline constexpr int kManagedProgramArtifactRootFd = 8;
// Optional Manager-opened scene-asset root; never a retained package root.
inline constexpr int kManagedSceneAssetRootFd = 9;
inline constexpr int kSocketBacklog = 16;
// The host tick: subordinate reconciliation and the component-state refresh.
inline constexpr int kReconcilePeriodMilliseconds = 5;
// Room in a connected client's receive queue for its one reply datagram; it
// runs out only for a client that is gone or never reads. A request itself
// is waited for without a time limit.
inline constexpr int kResponseSendDeadlineMilliseconds = 100;
// Requests the ordinary socket handed to one deferred lane and that are not
// yet answered: the one executing plus those queued behind it. Upstream
// admits one preview per cell target; this host-side bound keeps a
// misbehaving client from accumulating sockets behind long work.
inline constexpr std::size_t kMaximumPendingDeferredCommands = 8U;
// Limits connecting to the Manager's command socket only (as the Headless
// API does, OD-298 (a)); a connected scene request waits for the Manager's
// answer, ended by the reply, the Manager closing the connection or the
// host's own drain (the request's stop token).
inline constexpr int kManagerConnectDeadlineMilliseconds = 5000;
inline constexpr std::size_t kPlanningSceneSynchronizationAttempts = 32U;
inline constexpr auto kPlanningSceneSynchronizationRetryDelay =
    std::chrono::milliseconds(10);

[[nodiscard]] inline constexpr std::string_view SelectedExecutionRuntimeRole() {
  return deployment::ros2_control_execution_runtime_contract::kRuntimeRoleId;
}

struct ManagedRuntimeChildConfig final {
  int manager_channel_fd{-1};
  int manager_liveness_channel_fd{-1};
  int package_root_fd{-1};
  int program_artifact_root_fd{-1};
  /// Approved scene-asset root for relative scene meshes; absent when the
  /// Manager has none configured (relative scene meshes then fail closed).
  std::optional<int> scene_asset_root_fd;
  /// Generic machine site facts (`manyforge.site.v2`); absent when the
  /// Manager has no site settings file configured.
  std::optional<component_manager::SiteSettings> site_settings;
  std::filesystem::path package_root;
  std::filesystem::path program_artifact_root;
  std::filesystem::path manager_run_root;
  std::filesystem::path execution_state_root;
  std::filesystem::path execution_run_root;
  std::filesystem::path candidate_worker;
  std::string cell_id;
  core::RetainedBundleId retained_bundle_id{""};
  core::GraphRevision graph_revision{""};
  std::string runtime_unit_id;
  /// The application-start identity `A`, from `--application-start-id`
  /// (contract 1.1).
  core::ApplicationStartId application_start_id;
  std::string product_identity;
  std::string configuration_identity;
};


} // namespace manyforge::roboplan_backend::host_detail
