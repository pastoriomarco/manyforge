#pragma once

#include <manyforge_core/types/cell_admission.hpp>

#include <cstdint>

namespace manyforge::headless {

struct ManagerCellAdmissionReadinessFacts final {
  bool program_cycle_status_current{};
  manyforge::core::ProgramCycleStatusDTO program_cycle_status;
  bool required_component_status_current{};
  std::uint32_t required_component_total{};
  std::uint32_t required_component_ready{};
  bool deployment_status_current{};
  bool deployment_converged{};
  std::uint32_t blocking_fault_count{};
  std::uint32_t unresolved_owner_count{};
  std::uint32_t unsettled_effect_count{};
};

[[nodiscard]] inline manyforge::core::CellAdmissionReadinessSnapshotDTO
ReduceManagerCellAdmissionReadiness(
    const ManagerCellAdmissionReadinessFacts& facts) {
  return {
      facts.program_cycle_status_current,
      facts.program_cycle_status,
      facts.required_component_status_current,
      facts.required_component_total,
      facts.required_component_ready,
      facts.deployment_status_current,
      facts.deployment_converged,
      facts.blocking_fault_count,
      facts.unresolved_owner_count,
      facts.unsettled_effect_count,
  };
}

} // namespace manyforge::headless
