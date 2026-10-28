#include <manyforge_core/observed_robot_state_provider.hpp>
#include <manyforge_core/time/received_sample_age.hpp>

#include <algorithm>
#include <cmath>

namespace manyforge::core {

Result<RobotStateDTO> PublishedObservedRobotStateProvider::current_state(
    std::int64_t now_ns) const {
  const std::uint64_t authority_epoch_before =
      authority_epoch_ == nullptr
          ? 0
          : authority_epoch_->load(std::memory_order_acquire);
  if (authority_epoch_ != nullptr && (authority_epoch_before & 1u) != 0u) {
    return Result<RobotStateDTO>::Err(ErrorDTO{
        to_string(ErrorCode::kExecutionRejected),
        "Observed robot state authority is invalidated",
        {{"reason", "observed_state_authority_invalidated"}}});
  }
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
  if (required_authority_profile_.has_value() &&
      view.authority_profile() != *required_authority_profile_) {
    return Result<RobotStateDTO>::Err(ErrorDTO{
        to_string(ErrorCode::kExecutionRejected),
        "Observed robot state authority does not match the deployment profile",
        {{"reason", "observed_state_authority_mismatch"},
         {"source_id", std::string(view.source_id())},
         {"expected_authority_profile",
          observed_robot_state_authority_profile_name(
              *required_authority_profile_)},
         {"actual_authority_profile",
          observed_robot_state_authority_profile_name(
              view.authority_profile())}}});
  }
  const auto budget = view.freshness_budget_ns();
  if (budget > 0 &&
      (now_ns < 0 || view.received_timestamp_ns() < 0 ||
       !received_sample_current(view.received_timestamp_ns(), now_ns, budget))) {
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
  // Close the read-copy race with an owner invalidation that occurs while the
  // publication slot is pinned. The copied DTO is not released as admissible
  // evidence if its authority generation was invalidated concurrently.
  const std::uint64_t authority_epoch_after =
      authority_epoch_ == nullptr
          ? authority_epoch_before
          : authority_epoch_->load(std::memory_order_acquire);
  if (authority_epoch_ != nullptr &&
      (authority_epoch_after != authority_epoch_before ||
       (authority_epoch_after & 1u) != 0u)) {
    return Result<RobotStateDTO>::Err(ErrorDTO{
        to_string(ErrorCode::kExecutionRejected),
        "Observed robot state authority changed while it was read",
        {{"reason", "observed_state_authority_invalidated"}}});
  }
  return Result<RobotStateDTO>::Ok(std::move(state));
}

}  // namespace manyforge::core
