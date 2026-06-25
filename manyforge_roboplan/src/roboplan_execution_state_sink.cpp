#include <manyforge_roboplan/roboplan_execution_state_sink.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

using core::ExecutionStateSinkResult;
using core::ExecutionStateSinkStatus;
using core::ExecutionStateUpdateMode;

Eigen::VectorXd vector_from_std(const std::vector<double>& values) {
  Eigen::VectorXd q(static_cast<Eigen::Index>(values.size()));
  for (size_t i = 0; i < values.size(); ++i) {
    q[static_cast<Eigen::Index>(i)] = values[i];
  }
  return q;
}

bool all_finite(const std::vector<double>& values) {
  return std::all_of(values.begin(), values.end(),
                     [](double value) { return std::isfinite(value); });
}

bool try_lock_for_mode(std::unique_lock<std::mutex>& lock,
                       ExecutionStateUpdateMode mode) {
  if (mode == ExecutionStateUpdateMode::kBestEffort) {
    return lock.try_lock();
  }
  lock.lock();
  return true;
}

}  // namespace

RoboPlanExecutionStateSink::RoboPlanExecutionStateSink(
    std::shared_ptr<roboplan::Scene> scene,
    std::shared_ptr<std::mutex> scene_mutex)
    : scene_(std::move(scene)), scene_mutex_(std::move(scene_mutex)) {
  if (!scene_) {
    throw std::invalid_argument("scene must not be null");
  }
  if (!scene_mutex_) {
    scene_mutex_ = std::make_shared<std::mutex>();
  }
}

ExecutionStateSinkResult RoboPlanExecutionStateSink::apply_group_joint_positions(
    const std::string& group_id,
    const std::vector<double>& positions_rad,
    ExecutionStateUpdateMode mode) {
  if (positions_rad.empty()) {
    return {ExecutionStateSinkStatus::kApplied};
  }
  if (!all_finite(positions_rad)) {
    return {ExecutionStateSinkStatus::kInvalidPositions};
  }

  std::unique_lock<std::mutex> scene_lock(*scene_mutex_, std::defer_lock);
  if (!try_lock_for_mode(scene_lock, mode)) {
    return {ExecutionStateSinkStatus::kBusy};
  }

  try {
    const auto current = scene_->getCurrentJointPositions();
    Eigen::VectorXd q = vector_from_std(positions_rad);
    if (q.size() != current.size()) {
      if (group_id.empty()) {
        return {ExecutionStateSinkStatus::kInvalidGroup};
      }
      const auto maybe_group_info = scene_->getJointGroupInfo(group_id);
      if (!maybe_group_info) {
        return {ExecutionStateSinkStatus::kInvalidGroup};
      }
      if (positions_rad.size() != maybe_group_info->joint_names.size() ||
          maybe_group_info->joint_names.size() !=
              static_cast<size_t>(maybe_group_info->q_indices.size())) {
        return {ExecutionStateSinkStatus::kInvalidPositions};
      }
      q = scene_->toFullJointPositions(group_id, q);
    }
    scene_->setJointPositions(q);
    return {ExecutionStateSinkStatus::kApplied};
  } catch (...) {
    if (mode == ExecutionStateUpdateMode::kBestEffort) {
      return {ExecutionStateSinkStatus::kException};
    }
    throw;
  }
}

std::optional<double> RoboPlanExecutionStateSink::group_joint_position(
    const std::string& group_id,
    const std::string& joint_name) const {
  if (group_id.empty() || joint_name.empty()) {
    return std::nullopt;
  }

  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  const auto maybe_group_info = scene_->getJointGroupInfo(group_id);
  if (!maybe_group_info) {
    return std::nullopt;
  }
  const auto& group_info = *maybe_group_info;
  const auto joint_it =
      std::find(group_info.joint_names.begin(), group_info.joint_names.end(),
                joint_name);
  if (joint_it == group_info.joint_names.end()) {
    return std::nullopt;
  }
  const auto index =
      static_cast<size_t>(std::distance(group_info.joint_names.begin(), joint_it));
  if (index >= static_cast<size_t>(group_info.q_indices.size())) {
    return std::nullopt;
  }
  const auto q = scene_->getCurrentJointPositions();
  const auto q_index = group_info.q_indices[static_cast<Eigen::Index>(index)];
  if (q_index < 0 || q_index >= q.size()) {
    return std::nullopt;
  }
  return q[q_index];
}

ExecutionStateSinkResult RoboPlanExecutionStateSink::apply_group_joint_position(
    const std::string& group_id,
    const std::string& joint_name,
    double position_rad,
    ExecutionStateUpdateMode mode) {
  if (group_id.empty()) {
    return {ExecutionStateSinkStatus::kInvalidGroup};
  }
  if (joint_name.empty()) {
    return {ExecutionStateSinkStatus::kInvalidJoint};
  }
  if (!std::isfinite(position_rad)) {
    return {ExecutionStateSinkStatus::kInvalidPositions};
  }

  std::unique_lock<std::mutex> scene_lock(*scene_mutex_, std::defer_lock);
  if (!try_lock_for_mode(scene_lock, mode)) {
    return {ExecutionStateSinkStatus::kBusy};
  }

  try {
    const auto maybe_group_info = scene_->getJointGroupInfo(group_id);
    if (!maybe_group_info) {
      return {ExecutionStateSinkStatus::kInvalidGroup};
    }
    const auto& group_info = *maybe_group_info;
    const auto joint_it =
        std::find(group_info.joint_names.begin(), group_info.joint_names.end(),
                  joint_name);
    if (joint_it == group_info.joint_names.end()) {
      return {ExecutionStateSinkStatus::kInvalidJoint};
    }
    if (group_info.joint_names.size() !=
        static_cast<size_t>(group_info.q_indices.size())) {
      return {ExecutionStateSinkStatus::kInvalidPositions};
    }

    const auto q_current = scene_->getCurrentJointPositions();
    Eigen::VectorXd group_q(group_info.q_indices.size());
    for (Eigen::Index i = 0; i < group_info.q_indices.size(); ++i) {
      const auto q_index = group_info.q_indices[i];
      if (q_index < 0 || q_index >= q_current.size()) {
        return {ExecutionStateSinkStatus::kInvalidPositions};
      }
      group_q[i] = q_current[q_index];
    }

    const auto group_index = static_cast<Eigen::Index>(
        std::distance(group_info.joint_names.begin(), joint_it));
    group_q[group_index] = position_rad;
    auto q_full = group_q;
    if (q_full.size() != q_current.size()) {
      q_full = scene_->toFullJointPositions(group_id, group_q);
    }
    scene_->setJointPositions(q_full);
    return {ExecutionStateSinkStatus::kApplied};
  } catch (...) {
    if (mode == ExecutionStateUpdateMode::kBestEffort) {
      return {ExecutionStateSinkStatus::kException};
    }
    throw;
  }
}

}  // namespace manyforge::roboplan_backend
