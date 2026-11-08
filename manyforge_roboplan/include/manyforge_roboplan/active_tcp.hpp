#pragma once

#include <manyforge_core/types/program_binding.hpp>
#include <manyforge_core/types/tcp_entry.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {

/// One row of a robot's TCP list: the
/// entry, its compiled frame with the owner's contact-body roles, the
/// End-effector mark and the owner's default mark.
using RobotTcp = core::ProgramRobotTcpBindingDTO;
struct TcpFrameAssociation final {
  core::TcpEntryReferenceDTO reference;
  std::vector<core::CollisionBodyRoleId> contact_body_role_ids;
};

[[nodiscard]] inline core::TcpEntryReferenceDTO
TcpReference(const RobotTcp &row) {
  return row.entry;
}

/// Active TCP: the TCP a robot uses now, one of its TCP-list
/// rows. Unknown (runtime start) or reset: the End-effector row, or none
/// when the robot has no End-effector TCP. Not thread safe:
/// the robot's owner guards it with its own lock.
class ActiveTcp final {
 public:
  ActiveTcp() = default;
  ActiveTcp(std::vector<RobotTcp> rows, std::vector<TcpFrameAssociation> associations)
      : rows_(std::move(rows)), associations_(std::move(associations)) { reset(); }

  [[nodiscard]] const std::optional<RobotTcp>& value() const noexcept { return value_; }
  [[nodiscard]] const std::vector<RobotTcp>& rows() const noexcept { return rows_; }
  [[nodiscard]] const std::vector<TcpFrameAssociation>& associations() const noexcept { return associations_; }

  [[nodiscard]] const std::optional<core::TcpEntryReferenceDTO>& reference() const noexcept { return reference_; }

  void set_read_provenance(std::uint64_t registration, std::uint64_t sequence) noexcept {
    read_provenance_ = std::pair{registration, sequence};
  }
  [[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>> read_provenance() const noexcept {
    return read_provenance_;
  }

  // The owning admission path has already verified target/frame association.
  // Resolve only the physical frame here; ambiguous physical rows are refused.
  [[nodiscard]] const RobotTcp* find(const core::TcpEntryReferenceDTO& entry) const noexcept {
    if (std::count_if(associations_.begin(), associations_.end(),
                      [&](const auto &row) { return row.reference == entry; }) != 1)
      return nullptr;
    const auto exact = std::find_if(rows_.begin(), rows_.end(),
        [&](const RobotTcp &row) { return row.entry == entry; });
    if (exact != rows_.end()) return &*exact;
    const RobotTcp *found = nullptr;
    for (const auto &row : rows_) {
      if (row.frame.reference != entry.frame) continue;
      if (found != nullptr) return nullptr;
      found = &row;
    }
    return found;
  }

  bool reset() {
    const auto row = std::find_if(rows_.begin(), rows_.end(),
                                  [](const RobotTcp& candidate) { return candidate.end_effector; });
    return row == rows_.end() ? assign(nullptr, std::nullopt)
                              : assign(&*row, TcpReference(*row));
  }

  bool commit(const core::TcpEntryReferenceDTO& entry) {
    const auto* row = find(entry);
    return row != nullptr && assign(row, entry);
  }

 private:
  std::optional<std::pair<std::uint64_t, std::uint64_t>> read_provenance_{};
  bool assign(const RobotTcp* row, std::optional<core::TcpEntryReferenceDTO> reference) {
    const bool changed = reference_ != reference;
    if (row == nullptr) value_.reset();
    else value_ = *row;
    reference_ = std::move(reference);
    return changed;
  }

  std::vector<RobotTcp> rows_;
  std::vector<TcpFrameAssociation> associations_;
  std::optional<RobotTcp> value_;
  std::optional<core::TcpEntryReferenceDTO> reference_;
};

}  // namespace manyforge::roboplan_backend
