#include <manyforge_roboplan/pose_source_launch.hpp>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>

#include <fstream>
#include <sstream>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

using content_identity::CanonicalValue;

[[noreturn]] void fail(const std::string &message) {
  throw ControllerLaunchProcessError("pose source launch: " + message);
}

// The last bytes of `text` that fit one diagnostic detail value, starting on
// a UTF-8 boundary; empty when they are not valid NFC text.
std::string tail_detail(std::string_view text) {
  while (!text.empty() &&
         (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    text.remove_suffix(1U);
  if (text.size() > core::kMaxLifecycleErrorDetailValueBytes)
    text.remove_prefix(text.size() - core::kMaxLifecycleErrorDetailValueBytes);
  while (!text.empty() &&
         (static_cast<unsigned char>(text.front()) & 0xC0U) == 0x80U)
    text.remove_prefix(1U);
  return content_identity::is_utf8_nfc(text) ? std::string(text)
                                             : std::string();
}

} // namespace

std::string pose_source_observer_document(
    const std::string_view cell_id, const std::string_view component_id,
    const CanonicalValue &observer_configuration) {
  if (cell_id.empty() || component_id.empty() ||
      !observer_configuration.is_object())
    fail("the observer document needs a cell, a component and an object "
         "configuration");
  CanonicalValue::Object members = observer_configuration.as_object();
  const auto add_default = [&](const char *name, CanonicalValue value) {
    if (observer_configuration.find_object_member(name) == nullptr)
      members.emplace_back(name, std::move(value));
  };
  add_default("joint_state", CanonicalValue(nullptr));
  add_default("simulation_entities", CanonicalValue(CanonicalValue::Array{}));
  add_default("detection3d_sources", CanonicalValue(CanonicalValue::Array{}));
  members.emplace_back("api_version",
                       CanonicalValue(std::string(kPoseSourceSiteApiVersion)));
  members.emplace_back("cell_id", CanonicalValue(std::string(cell_id)));
  members.emplace_back("source_id", CanonicalValue(std::string(component_id)));
  return content_identity::canonical_json(CanonicalValue(std::move(members)));
}

PoseSourcePlan
plan_pose_source(const std::string_view cell_id, std::string component_id,
                 const CanonicalValue &observer_configuration,
                 const std::filesystem::path &execution_run_root,
                 std::filesystem::path manager_run_root) {
  if (execution_run_root.empty() || manager_run_root.empty())
    fail("a pose source needs the execution and Manager run roots");
  PoseSourcePlan plan;
  plan.document = pose_source_observer_document(cell_id, component_id,
                                                observer_configuration);
  plan.artifacts_root =
      execution_run_root / "controller-launch-artifacts" / component_id;
  plan.site_config_path =
      plan.artifacts_root / std::string(kPoseSourceSiteFileName);
  plan.component_id = std::move(component_id);
  plan.manager_run_root = std::move(manager_run_root);
  return plan;
}

void write_pose_source_document(const PoseSourcePlan &plan) {
  std::filesystem::create_directories(plan.artifacts_root);
  // Replace atomically: a starting observer never reads a partial file.
  const auto staged = plan.site_config_path.string() + ".tmp";
  {
    std::ofstream out(staged, std::ios::binary | std::ios::trunc);
    out << plan.document;
    out.close();
    if (!out)
      fail("cannot write " + staged);
  }
  std::filesystem::rename(staged, plan.site_config_path);
}

ControllerLaunchCommand
pose_source_launch_command(const PoseSourcePlan &plan,
                           const std::string &source_epoch_base) {
  if (plan.component_id.empty() || plan.document.empty() ||
      plan.site_config_path.empty() || plan.manager_run_root.empty() ||
      source_epoch_base.empty())
    fail("plan is incomplete");
  const auto site = plan.site_config_path.string();
  const auto manager = plan.manager_run_root.string();
  for (const auto *value : {&site, &manager, &source_epoch_base})
    if (value->find_first_of(" \t\n\r") != std::string::npos)
      fail("launch value contains whitespace: " + *value);
  ControllerLaunchCommand command;
  command.controllable_component_id = plan.component_id;
  command.lane = ControllerLaunchLane::kPoseSource;
  command.argv = {"ros2",
                  "launch",
                  "manyforge_ros_bringup",
                  "pose_source.launch.py",
                  "site_config_path:=" + site,
                  "manager_run_root:=" + manager,
                  "source_epoch_base:=" + source_epoch_base};
  std::ostringstream identity;
  identity << "pose_source\n"
           << plan.component_id << '\n'
           << content_identity::sha256_digest(plan.document) << '\n'
           << manager << '\n';
  command.identity = identity.str();
  command.keep_across_apply = true;
  return command;
}

bool pose_source_launch_survives(
    ControllerLaunchProcess *const process,
    const std::optional<PoseSourcePlan> &candidate) {
  return process != nullptr && candidate.has_value() &&
         process->command().lane == ControllerLaunchLane::kPoseSource &&
         owned_launch_live(process, false, false) &&
         process->command().identity ==
             pose_source_launch_command(*candidate, "survival").identity;
}

core::ErrorDTO pose_source_exit_fault(std::string reason,
                                      const std::optional<int> exit_status,
                                      const std::string_view output_tail) {
  // `ros2 launch` itself ends normally; it reports its child's status as
  // "process has died [pid N, exit code 78, ...]".
  const bool config_invalid =
      exit_status == kPoseSourceConfigInvalidExitStatus ||
      output_tail.find("exit code " +
                       std::to_string(kPoseSourceConfigInvalidExitStatus)) !=
          std::string_view::npos;
  if (!config_invalid)
    return {std::string(kPoseSourceExitedCode), std::move(reason), {}};
  core::ErrorDTO fault{std::string(kPoseSourceConfigInvalidCode),
                       std::move(reason),
                       {}};
  if (auto detail = tail_detail(output_tail); !detail.empty())
    fault.details.emplace("output_tail", std::move(detail));
  return fault;
}

std::function<std::optional<core::ErrorDTO>()>
pose_source_fault_source(std::shared_ptr<ControllerLaunchProcess> process) {
  auto source = ControllerLaunchSupervisor::fault_source(process);
  return [process = std::move(process), source = std::move(source)]()
             -> std::optional<core::ErrorDTO> {
    auto reason = source();
    if (!reason)
      return std::nullopt;
    const auto evidence = process->exit_evidence();
    return pose_source_exit_fault(
        std::move(*reason), evidence ? evidence->exit_status : std::nullopt,
        process->output_snapshot().tail);
  };
}

} // namespace manyforge::roboplan_backend
