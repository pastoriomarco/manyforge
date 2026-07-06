#include <manyforge_roboplan/roboplan_execution_state_sink.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <optional>
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

void RoboPlanExecutionStateSink::set_robot_model_authority_metadata(
    std::map<std::string, std::string> canonical_to_view_joint_names,
    std::map<std::string, std::string> view_to_canonical_joint_names,
    std::map<std::string, std::vector<std::string>>
        group_commandable_joint_names) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  canonical_to_view_joint_names_ = std::move(canonical_to_view_joint_names);
  view_to_canonical_joint_names_ = std::move(view_to_canonical_joint_names);
  group_commandable_joint_names_ = std::move(group_commandable_joint_names);
  commandable_group_cache_.clear();
}

std::string RoboPlanExecutionStateSink::view_joint_name_locked(
    const std::string& joint_name) const {
  const auto it = canonical_to_view_joint_names_.find(joint_name);
  return it == canonical_to_view_joint_names_.end() ? joint_name : it->second;
}

std::string RoboPlanExecutionStateSink::commandable_group_cache_key_locked(
    const std::string& group_id,
    const std::vector<std::string>& group_joint_names) const {
  std::string key = group_id;
  key.push_back('\0');
  for (const auto& joint_name : group_joint_names) {
    key.append(joint_name);
    key.push_back('\0');
  }
  return key;
}

std::optional<RoboPlanExecutionStateSink::CommandableGroupMapping>
RoboPlanExecutionStateSink::commandable_group_mapping_locked(
    const std::string& group_id,
    const std::vector<std::string>& group_joint_names) const {
  const auto cache_key =
      commandable_group_cache_key_locked(group_id, group_joint_names);
  const auto cache_it = commandable_group_cache_.find(cache_key);
  if (cache_it != commandable_group_cache_.end()) {
    return cache_it->second;
  }

  CommandableGroupMapping mapping;
  const auto metadata_it = group_commandable_joint_names_.find(group_id);
  if (metadata_it != group_commandable_joint_names_.end() &&
      !metadata_it->second.empty()) {
    mapping.commandable_view_joint_names.reserve(metadata_it->second.size());
    mapping.expanded_group_indices.reserve(metadata_it->second.size());
    for (const auto& canonical_joint_name : metadata_it->second) {
      const auto view_joint_name = view_joint_name_locked(canonical_joint_name);
      const auto group_it =
          std::find(group_joint_names.begin(), group_joint_names.end(),
                    view_joint_name);
      if (group_it == group_joint_names.end()) {
        return std::nullopt;
      }
      const auto maybe_joint_info = scene_->getJointInfo(view_joint_name);
      if (!maybe_joint_info || maybe_joint_info->mimic_info.has_value() ||
          maybe_joint_info->num_position_dofs != 1) {
        return std::nullopt;
      }
      mapping.commandable_view_joint_names.push_back(view_joint_name);
      mapping.expanded_group_indices.push_back(static_cast<size_t>(
          std::distance(group_joint_names.begin(), group_it)));
    }
    commandable_group_cache_[cache_key] = mapping;
    return mapping;
  }

  for (size_t i = 0; i < group_joint_names.size(); ++i) {
    const auto& view_joint_name = group_joint_names[i];
    const auto maybe_joint_info = scene_->getJointInfo(view_joint_name);
    if (!maybe_joint_info) {
      return std::nullopt;
    }
    if (maybe_joint_info->mimic_info.has_value() ||
        maybe_joint_info->num_position_dofs == 0) {
      continue;
    }
    if (maybe_joint_info->num_position_dofs != 1) {
      return std::nullopt;
    }
    mapping.commandable_view_joint_names.push_back(view_joint_name);
    mapping.expanded_group_indices.push_back(i);
  }
  commandable_group_cache_[cache_key] = mapping;
  return mapping;
}

std::vector<std::string>
RoboPlanExecutionStateSink::commandable_view_joint_names_locked(
    const std::string& group_id,
    const std::vector<std::string>& group_joint_names) const {
  const auto mapping =
      commandable_group_mapping_locked(group_id, group_joint_names);
  if (!mapping.has_value()) {
    return {};
  }
  return mapping->commandable_view_joint_names;
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
      const auto& group_info = *maybe_group_info;
      const auto commandable_mapping =
          commandable_group_mapping_locked(group_id, group_info.joint_names);
      if (!commandable_mapping.has_value() ||
          commandable_mapping->commandable_view_joint_names.size() !=
          static_cast<size_t>(group_info.q_indices.size())) {
        return {ExecutionStateSinkStatus::kInvalidPositions};
      }
      if (positions_rad.size() ==
          static_cast<size_t>(group_info.q_indices.size())) {
        q = vector_from_std(positions_rad);
      } else if (positions_rad.size() == group_info.joint_names.size()) {
        if (commandable_mapping->expanded_group_indices.size() !=
            commandable_mapping->commandable_view_joint_names.size()) {
          return {ExecutionStateSinkStatus::kInvalidPositions};
        }
        q = Eigen::VectorXd(static_cast<Eigen::Index>(
            commandable_mapping->expanded_group_indices.size()));
        for (size_t i = 0; i < commandable_mapping->expanded_group_indices.size();
             ++i) {
          const auto expanded_index =
              commandable_mapping->expanded_group_indices[i];
          if (expanded_index >= positions_rad.size()) {
            return {ExecutionStateSinkStatus::kInvalidPositions};
          }
          q[static_cast<Eigen::Index>(i)] = positions_rad[expanded_index];
        }
      } else {
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
  const auto commandable_view_joint_names =
      commandable_view_joint_names_locked(group_id, group_info.joint_names);
  if (commandable_view_joint_names.size() !=
      static_cast<size_t>(group_info.q_indices.size())) {
    return std::nullopt;
  }
  const auto requested_view_joint_name = view_joint_name_locked(joint_name);
  const auto joint_it =
      std::find(commandable_view_joint_names.begin(),
                commandable_view_joint_names.end(), requested_view_joint_name);
  if (joint_it == commandable_view_joint_names.end()) {
    return std::nullopt;
  }
  const auto index = static_cast<size_t>(
      std::distance(commandable_view_joint_names.begin(), joint_it));
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
    const auto commandable_view_joint_names =
        commandable_view_joint_names_locked(group_id, group_info.joint_names);
    if (commandable_view_joint_names.size() !=
        static_cast<size_t>(group_info.q_indices.size())) {
      return {ExecutionStateSinkStatus::kInvalidPositions};
    }
    const auto requested_view_joint_name = view_joint_name_locked(joint_name);
    const auto joint_it =
        std::find(commandable_view_joint_names.begin(),
                  commandable_view_joint_names.end(), requested_view_joint_name);
    if (joint_it == commandable_view_joint_names.end()) {
      return {ExecutionStateSinkStatus::kInvalidJoint};
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
        std::distance(commandable_view_joint_names.begin(), joint_it));
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
