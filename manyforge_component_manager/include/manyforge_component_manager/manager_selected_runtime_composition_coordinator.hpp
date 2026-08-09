#pragma once

#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace manyforge::component_manager {

class ManagerSelectedRuntimeCompositionCoordinatorError final
    : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

enum class ManagerSelectedRuntimeCompositionResultState {
  kPending,
  kInstalled,
};

struct ManagerSelectedRuntimeCompositionCoordinatorResult final {
  ManagerSelectedRuntimeCompositionResultState state;
  ManagerSelectedRuntimeCompositionResultDTO owner_result;
  std::optional<ManagerApplyPrivateMaterializationWriteResult>
      durable_materialization;
};

/// Installs the selected product inside the already-running fence-owning host
/// before Manager records private materialization or publishes the candidate.
/// Exact replay always re-observes the current child, so Manager/child
/// replacement cannot reuse a journal row as live-runtime evidence.
class ManagerSelectedRuntimeCompositionCoordinator final {
public:
  ManagerSelectedRuntimeCompositionCoordinator(
      SqliteManagerApplyJournal &apply_journal,
      const manyforge::deployment::IVerifiedRetainedBundleSource
          &retained_bundle_source,
      IManagerSelectedRuntimeCompositionPort &runtime_composition,
      manyforge::core::RuntimeEpoch manager_runtime_epoch);

  [[nodiscard]] ManagerSelectedRuntimeCompositionCoordinatorResult
  InstallOrRecover(const manyforge::core::ApplyId &apply_id,
                   manyforge::core::ApplyGeneration apply_generation);

private:
  SqliteManagerApplyJournal &apply_journal_;
  const manyforge::deployment::IVerifiedRetainedBundleSource
      &retained_bundle_source_;
  IManagerSelectedRuntimeCompositionPort &runtime_composition_;
  manyforge::core::RuntimeEpoch manager_runtime_epoch_;
  std::mutex operation_mutex_;
};

} // namespace manyforge::component_manager
