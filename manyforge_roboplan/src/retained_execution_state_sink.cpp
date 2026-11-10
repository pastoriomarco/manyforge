#include <manyforge_roboplan/retained_execution_state_sink.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <variant>

namespace manyforge::roboplan_backend {
namespace {

[[nodiscard]] RetainedStateProjectionMode projection_mode(
    const core::ExecutionStateUpdateMode mode) noexcept {
  return mode == core::ExecutionStateUpdateMode::kBestEffort
             ? RetainedStateProjectionMode::kBestEffort
             : RetainedStateProjectionMode::kBlocking;
}

}  // namespace

RetainedExecutionStateSink::RetainedExecutionStateSink(
    std::shared_ptr<core::IClock> clock,
    std::vector<RetainedExecutionStateSinkBinding> bindings)
    : clock_(std::move(clock)), bindings_(std::move(bindings)) {
  std::set<std::string> group_names;
  if (!clock_ || bindings_.empty()) {
    throw std::invalid_argument(
        "retained execution-state sink requires a clock and at least one binding");
  }
  for (const auto& binding : bindings_) {
    if (binding.compiled_group_name.empty() || !binding.state_channel ||
        !group_names.insert(binding.compiled_group_name).second ||
        binding.state_channel->query_context().resources.empty()) {
      throw std::invalid_argument(
          "retained execution-state sink binding is incomplete or duplicated");
    }
  }
}

const RetainedExecutionStateSinkBinding*
RetainedExecutionStateSink::find_binding(
    const std::string& group_id) const noexcept {
  const auto found = std::find_if(
      bindings_.begin(), bindings_.end(), [&](const auto& binding) {
        return binding.compiled_group_name == group_id;
      });
  return found == bindings_.end() ? nullptr : &*found;
}

core::ExecutionStateSinkResult RetainedExecutionStateSink::map_result(
    const RetainedStateProjectionResult& result) noexcept {
  switch (result.status) {
    case RetainedStateProjectionStatus::kApplied:
      return {core::ExecutionStateSinkStatus::kApplied};
    case RetainedStateProjectionStatus::kBusy:
      return {core::ExecutionStateSinkStatus::kBusy};
    case RetainedStateProjectionStatus::kRejected:
      return {core::ExecutionStateSinkStatus::kInvalidPositions};
    case RetainedStateProjectionStatus::kFailed:
      return {core::ExecutionStateSinkStatus::kException};
  }
  return {core::ExecutionStateSinkStatus::kException};
}

double RetainedExecutionStateSink::position_value(
    const core::CoordinatePositionDTO& position) noexcept {
  if (const auto* angular =
          std::get_if<core::AngularPositionDTO>(&position)) {
    return angular->value_rad;
  }
  return std::get<core::LinearPositionDTO>(position).value_m;
}

core::ExecutionStateSinkResult
RetainedExecutionStateSink::apply_group_joint_positions(
    const std::string& group_id, const std::vector<double>& positions_si,
    const core::ExecutionStateUpdateMode mode) {
  const auto* binding = find_binding(group_id);
  if (binding == nullptr) {
    return {core::ExecutionStateSinkStatus::kInvalidGroup};
  }
  const auto& context = binding->state_channel->query_context();
  if (positions_si.size() != context.resources.size() ||
      std::any_of(positions_si.begin(), positions_si.end(),
                  [](const double value) { return !std::isfinite(value); })) {
    return {core::ExecutionStateSinkStatus::kInvalidPositions};
  }
  if (binding->publish_on_apply) {
    std::lock_guard lock(publish_on_apply_mutex_);
    const auto now_ns = clock_->now();
    return map_result(binding->state_channel->publish_and_project(
        positions_si, std::vector<double>(positions_si.size(), 0.0), now_ns,
        now_ns, projection_mode(mode)));
  }
  const auto now_ns = clock_->now();
  return map_result(
      binding->state_channel->project_latest(now_ns, projection_mode(mode)));
}

std::optional<double> RetainedExecutionStateSink::group_joint_position(
    const std::string& group_id, const std::string& joint_name) const {
  const auto* binding = find_binding(group_id);
  if (binding == nullptr || joint_name.empty()) {
    return std::nullopt;
  }
  const auto& context = binding->state_channel->query_context();
  const auto found =
      std::find(context.joint_names.begin(), context.joint_names.end(), joint_name);
  if (found == context.joint_names.end()) {
    return std::nullopt;
  }
  const auto state = binding->state_channel->current_controlled_state();
  if (!state.ok() || state.value().positions.size() != context.joint_names.size()) {
    return std::nullopt;
  }
  const auto index = static_cast<std::size_t>(
      std::distance(context.joint_names.begin(), found));
  return position_value(state.value().positions[index]);
}

core::ExecutionStateSinkResult
RetainedExecutionStateSink::apply_group_joint_position(
    const std::string& group_id, const std::string& joint_name,
    const double position_si, const core::ExecutionStateUpdateMode mode) {
  const auto* binding = find_binding(group_id);
  if (binding == nullptr) {
    return {core::ExecutionStateSinkStatus::kInvalidGroup};
  }
  if (!binding->publish_on_apply || joint_name.empty()) {
    return {core::ExecutionStateSinkStatus::kInvalidJoint};
  }
  const auto& context = binding->state_channel->query_context();
  const auto found =
      std::find(context.joint_names.begin(), context.joint_names.end(), joint_name);
  if (found == context.joint_names.end()) {
    return {core::ExecutionStateSinkStatus::kInvalidJoint};
  }
  if (!std::isfinite(position_si)) {
    return {core::ExecutionStateSinkStatus::kInvalidPositions};
  }
  std::lock_guard lock(publish_on_apply_mutex_);
  const auto current = binding->state_channel->current_controlled_state();
  if (!current.ok() ||
      current.value().positions.size() != context.joint_names.size()) {
    return {core::ExecutionStateSinkStatus::kStateUnavailable};
  }
  std::vector<double> positions;
  positions.reserve(current.value().positions.size());
  for (const auto& position : current.value().positions) {
    positions.push_back(position_value(position));
  }
  positions[static_cast<std::size_t>(
      std::distance(context.joint_names.begin(), found))] = position_si;
  const auto now_ns = clock_->now();
  return map_result(binding->state_channel->publish_and_project(
      positions, std::vector<double>(positions.size(), 0.0), now_ns, now_ns,
      projection_mode(mode)));
}

}  // namespace manyforge::roboplan_backend
