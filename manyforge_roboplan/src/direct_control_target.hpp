#pragma once

#include <manyforge_core/types/program_binding.hpp>

#include <span>
#include <string_view>

namespace manyforge::roboplan_backend::host_detail {

enum class DirectControlTargetStatus { kFound, kMissing, kAmbiguous, kCapabilityMissing };
struct DirectControlTargetSelection {
  DirectControlTargetStatus status;
  const core::ProgramTargetBindingDTO *target;
};

inline DirectControlTargetSelection SelectDirectControlTarget(
    std::span<const core::ProgramTargetBindingDTO> targets,
    const core::ProgramTargetReferenceDTO &expected,
    std::string_view capability_id) {
  const core::ProgramTargetBindingDTO *selected = nullptr;
  for (const auto &target : targets) {
    if (target.target != expected) continue;
    if (selected) return {DirectControlTargetStatus::kAmbiguous, nullptr};
    selected = &target;
  }
  if (!selected) return {DirectControlTargetStatus::kMissing, nullptr};
  for (const auto &capability : selected->capability_bindings)
    if (capability.capability_id.value() == capability_id)
      return {DirectControlTargetStatus::kFound, selected};
  return {DirectControlTargetStatus::kCapabilityMissing, nullptr};
}

} // namespace manyforge::roboplan_backend::host_detail
