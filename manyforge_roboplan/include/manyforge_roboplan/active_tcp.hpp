#pragma once

#include <manyforge_core/types/program_binding.hpp>
#include <manyforge_core/types/tcp_entry.hpp>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {

/// One row of a robot's TCP list (design 1.2; TCP-A1's binding row): the
/// entry, its compiled frame with the owner's contact-body roles, the
/// End-effector mark and the owner's default mark.
using RobotTcp = core::ProgramRobotTcpBindingDTO;

/// Active TCP (M7, design 2): the TCP a robot uses now, one of its TCP-list
/// rows. Unknown (runtime start) or reset: the End-effector row, or none
/// when the robot has no End-effector TCP (C-P2, B-run-1). Not thread safe:
/// the robot's owner guards it with its own lock.
class ActiveTcp final {
 public:
  ActiveTcp() = default;
  explicit ActiveTcp(std::vector<RobotTcp> rows) : rows_(std::move(rows)) { reset(); }

  [[nodiscard]] const std::optional<RobotTcp>& value() const noexcept { return value_; }
  [[nodiscard]] const std::vector<RobotTcp>& rows() const noexcept { return rows_; }

  [[nodiscard]] const RobotTcp* find(const core::TcpEntryDTO& entry) const noexcept {
    const auto row = std::find_if(rows_.begin(), rows_.end(),
                                  [&](const RobotTcp& candidate) { return candidate.entry == entry; });
    return row == rows_.end() ? nullptr : &*row;
  }

  /// The End-effector row or none. True if the value changed.
  bool reset() {
    const auto row = std::find_if(rows_.begin(), rows_.end(),
                                  [](const RobotTcp& candidate) { return candidate.end_effector; });
    return assign(row == rows_.end() ? nullptr : &*row);
  }

  /// An adapter-confirmed entry of the list. True if the value changed;
  /// an entry outside the list changes nothing.
  bool commit(const core::TcpEntryDTO& entry) {
    const auto* row = find(entry);
    return row != nullptr && assign(row);
  }

 private:
  bool assign(const RobotTcp* row) {
    const bool changed = row == nullptr ? value_.has_value()
                                        : !value_ || value_->entry != row->entry;
    if (row == nullptr) {
      value_.reset();
    } else {
      value_ = *row;
    }
    return changed;
  }

  std::vector<RobotTcp> rows_;
  std::optional<RobotTcp> value_;
};

}  // namespace manyforge::roboplan_backend
