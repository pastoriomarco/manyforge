#pragma once

#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_core/ports/imanager_scene_projection.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <mutex>
#include <stdexcept>

namespace manyforge::component_manager {

class ManagerSceneInitializationError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class ManagerSceneInitializationPending final
    : public ManagerSceneInitializationError {
public:
  using ManagerSceneInitializationError::ManagerSceneInitializationError;
};

/// Thin Manager participant for the Scene-owned Begin/Read boundary. It does
/// not persist projection progress, replay an effect after owner loss, or
/// derive Scene content. Exact runtime epochs come from the already durable
/// staged-ready record and Scene captures the authoritative snapshot.
class ManagerSceneInitializationCoordinator final {
public:
  ManagerSceneInitializationCoordinator(
      SqliteManagerApplyJournal &apply_journal,
      manyforge::core::IManagerSceneProjectionMaintenancePort &scene_owner);

  [[nodiscard]]
  manyforge::core::ProjectionMaintenanceInitializationStatusDTO
  Initialize(const manyforge::core::ApplyId &apply_id,
             manyforge::core::ApplyGeneration apply_generation,
             const manyforge::deployment::RetainedBundle &candidate);

private:
  SqliteManagerApplyJournal &apply_journal_;
  manyforge::core::IManagerSceneProjectionMaintenancePort &scene_owner_;
  std::mutex operation_mutex_;
};

} // namespace manyforge::component_manager
