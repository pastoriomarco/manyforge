#pragma once

#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <mutex>
#include <stdexcept>

namespace manyforge::component_manager {

class ManagerSelectedRuntimeCompositionCoordinatorError final
    : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Sends the Apply's one host call (CONTRACTS §15) to the execution host that
/// holds the Apply's configuration fence, then records the private
/// materialization the publication binds to. A host refusal is thrown as a
/// typed error with the host's code; a lost or failed exchange is thrown as
/// is (its effects are unknown).
class ManagerSelectedRuntimeCompositionCoordinator final {
public:
  explicit ManagerSelectedRuntimeCompositionCoordinator(
      SqliteManagerApplyJournal &apply_journal,
      IManagerSelectedRuntimeCompositionPort &runtime_composition);

  [[nodiscard]] HostApplyResult
  ApplyOnHost(const manyforge::core::ApplyId &apply_id,
              manyforge::core::ApplyGeneration apply_generation,
              const manyforge::deployment::RetainedBundle &predecessor,
              const manyforge::deployment::RetainedBundle &candidate);

private:
  SqliteManagerApplyJournal &apply_journal_;
  IManagerSelectedRuntimeCompositionPort &runtime_composition_;
  std::mutex operation_mutex_;
};

} // namespace manyforge::component_manager
