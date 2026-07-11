#include <manyforge_core/observed_robot_state_provider.hpp>

#include <algorithm>
#include <cmath>

namespace manyforge::core {

Result<RobotStateDTO> PublishedObservedRobotStateProvider::current_state(
    std::int64_t now_ns) const {
  auto view = publication_->acquire(now_ns);
  if (!view) {
    return Result<RobotStateDTO>::Err(ErrorDTO{
        to_string(ErrorCode::kExecutionRejected),
        "Observed robot state is unavailable",
        {{"reason", "observed_state_unavailable"}}});
  }
  if (!view.complete() ||
      view.joint_names().size() != view.positions_rad().size() ||
      view.joint_names().empty()) {
    return Result<RobotStateDTO>::Err(ErrorDTO{
        to_string(ErrorCode::kExecutionRejected),
        "Observed robot state is incomplete",
        {{"reason", "observed_state_incomplete"},
         {"source_id", std::string(view.source_id())}}});
  }
  const auto budget = view.freshness_budget_ns();
  if (budget > 0 &&
      (view.received_timestamp_ns() < 0 || now_ns < view.received_timestamp_ns() ||
       now_ns - view.received_timestamp_ns() > budget)) {
    return Result<RobotStateDTO>::Err(ErrorDTO{
        to_string(ErrorCode::kExecutionRejected),
        "Observed robot state is stale",
        {{"reason", "stale_observed_robot_state"},
         {"source_id", std::string(view.source_id())}}});
  }
  if (std::any_of(view.positions_rad().begin(), view.positions_rad().end(),
                  [](double value) { return !std::isfinite(value); })) {
    return Result<RobotStateDTO>::Err(ErrorDTO{
        to_string(ErrorCode::kExecutionRejected),
        "Observed robot state contains a non-finite position",
        {{"reason", "observed_state_not_finite"},
         {"source_id", std::string(view.source_id())}}});
  }

  RobotStateDTO state;
  state.joint_names.assign(view.joint_names().begin(), view.joint_names().end());
  state.positions_rad.assign(view.positions_rad().begin(),
                             view.positions_rad().end());
  if (view.velocities_complete() &&
      view.velocities_rad_s().size() == view.positions_rad().size()) {
    state.velocities_rad_s = std::vector<double>(
        view.velocities_rad_s().begin(), view.velocities_rad_s().end());
  }
  return Result<RobotStateDTO>::Ok(std::move(state));
}

}  // namespace manyforge::core
