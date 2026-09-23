#pragma once

#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_core/ports/iactive_bundle_publication_store.hpp>
#include <manyforge_core/ports/imanager_scene_projection.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace manyforge::component_manager {

class ManagerSelectedBundleTransitionCoordinatorError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Exact observation context when publication or Scene completion cannot be
/// proven. The operation is terminally failed/unknown by its caller; this
/// object never authorizes automatic replay after owner loss.
class ManagerSelectedBundleTransitionEffectUncertainError final
    : public ManagerSelectedBundleTransitionCoordinatorError {
 public:
  ManagerSelectedBundleTransitionEffectUncertainError(
      std::string detail, manyforge::core::ActiveBundlePublicationRequestDTO publication_request,
      manyforge::core::ProjectionMaintenanceInitializationQueryDTO scene_status_query,
      std::optional<manyforge::core::ActiveBundlePublicationReceiptDTO> observed_publication =
          std::nullopt,
      std::optional<manyforge::core::ProjectionMaintenanceInitializationStatusDTO>
          observed_scene_status = std::nullopt);

  [[nodiscard]] const manyforge::core::ActiveBundlePublicationRequestDTO& publication_request()
      const noexcept;
  [[nodiscard]] const manyforge::core::ProjectionMaintenanceInitializationQueryDTO& scene_status_query()
      const noexcept;
  [[nodiscard]] const std::optional<manyforge::core::ActiveBundlePublicationReceiptDTO>&
  observed_publication() const noexcept;
  [[nodiscard]] const std::optional<manyforge::core::ProjectionMaintenanceInitializationStatusDTO>&
  observed_scene_status() const noexcept;

 private:
  manyforge::core::ActiveBundlePublicationRequestDTO publication_request_;
  manyforge::core::ProjectionMaintenanceInitializationQueryDTO scene_status_query_;
  std::optional<manyforge::core::ActiveBundlePublicationReceiptDTO> observed_publication_;
  std::optional<manyforge::core::ProjectionMaintenanceInitializationStatusDTO>
      observed_scene_status_;
};

enum class ManagerSelectedBundleTransitionPath {
  kPublishedFromNewCall,
  kRecoveredPublicationHead,
  kCompletedFromNewCall,
  kRecoveredCompletedOwnerStatus,
};

struct ManagerSelectedBundleTransitionResult final {
  ManagerApplyPrivateMaterializationWriteResult durable_materialization;
  ManagerApplyPublicationReceiptWriteResult durable_publication;
  bool scene_completed{};
  ManagerSelectedBundleTransitionPath path;
};

/// Completes the selected named-Apply transition from Scene-owned initialized
/// state to public selection and Scene-owned completion.
///
/// Private materialization is journaled before publication. One invocation performs at most one external
/// mutating operation: Publish or Complete. Every in-process continuation
/// observes the authoritative durable head and Scene status first.
class ManagerSelectedBundleTransitionCoordinator final {
 public:
   ManagerSelectedBundleTransitionCoordinator(
       SqliteManagerApplyJournal &apply_journal,
       manyforge::core::IActiveBundlePublicationStore &publication_store,
       manyforge::core::IManagerSceneProjectionMaintenancePort &scene_owner);

   [[nodiscard]] ManagerSelectedBundleTransitionResult
   AdvanceOrRecover(const manyforge::core::ApplyId &apply_id,
                    manyforge::core::ApplyGeneration apply_generation,
                    const manyforge::deployment::RetainedBundle &predecessor,
                    const manyforge::deployment::RetainedBundle &candidate);

   ManagerSelectedBundleTransitionCoordinator(
       const ManagerSelectedBundleTransitionCoordinator &) = delete;
   ManagerSelectedBundleTransitionCoordinator &
   operator=(const ManagerSelectedBundleTransitionCoordinator &) = delete;

 private:
  SqliteManagerApplyJournal& apply_journal_;
  manyforge::core::IActiveBundlePublicationStore& publication_store_;
  manyforge::core::IManagerSceneProjectionMaintenancePort& scene_owner_;
  std::mutex operation_mutex_;
};

}  // namespace manyforge::component_manager
