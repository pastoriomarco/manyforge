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

/// Exact recovery context after publication or scene Finalize may have
/// committed while their response, or Manager's subsequent journal write,
/// was lost.
class ManagerSelectedBundleTransitionEffectUncertainError final
    : public ManagerSelectedBundleTransitionCoordinatorError {
 public:
  ManagerSelectedBundleTransitionEffectUncertainError(
      std::string detail, manyforge::core::ActiveBundlePublicationRequestDTO publication_request,
      manyforge::core::ProjectionApplyStatusQueryDTO scene_status_query,
      std::optional<manyforge::core::ActiveBundlePublicationReceiptDTO> observed_publication =
          std::nullopt,
      std::optional<manyforge::core::ProjectionApplyStatusDTO> observed_scene_status = std::nullopt,
      std::optional<manyforge::core::ProjectionApplyFinalizeResultDTO> observed_finalize_result =
          std::nullopt);

  [[nodiscard]] const manyforge::core::ActiveBundlePublicationRequestDTO& publication_request()
      const noexcept;
  [[nodiscard]] const manyforge::core::ProjectionApplyStatusQueryDTO& scene_status_query()
      const noexcept;
  [[nodiscard]] const std::optional<manyforge::core::ActiveBundlePublicationReceiptDTO>&
  observed_publication() const noexcept;
  [[nodiscard]] const std::optional<manyforge::core::ProjectionApplyStatusDTO>&
  observed_scene_status() const noexcept;
  [[nodiscard]] const std::optional<manyforge::core::ProjectionApplyFinalizeResultDTO>&
  observed_finalize_result() const noexcept;

 private:
  manyforge::core::ActiveBundlePublicationRequestDTO publication_request_;
  manyforge::core::ProjectionApplyStatusQueryDTO scene_status_query_;
  std::optional<manyforge::core::ActiveBundlePublicationReceiptDTO> observed_publication_;
  std::optional<manyforge::core::ProjectionApplyStatusDTO> observed_scene_status_;
  std::optional<manyforge::core::ProjectionApplyFinalizeResultDTO> observed_finalize_result_;
};

enum class ManagerSelectedBundleTransitionPath {
  kPublishedFromNewCall,
  kRecoveredPublicationHead,
  kFinalizedFromNewCall,
  kRecoveredFinalizedOwnerStatus,
  kDurableFinalizedReplay,
};

struct ManagerSelectedBundleTransitionResult final {
  ManagerApplyPrivateMaterializationWriteResult durable_materialization;
  ManagerApplyObsoleteCommandSurfacesRetiredWriteResult durable_retired_command_surfaces;
  ManagerApplyPublicationReceiptWriteResult durable_publication;
  std::optional<ManagerApplySceneFinalizedWriteResult> durable_finalized;
  ManagerSelectedBundleTransitionPath path;
};

/// Completes the selected named-Apply transition from durable activation to
/// public selection and scene Finalize.
///
/// Private materialization and complete command-surface disposition are
/// journaled before publication. One invocation performs at most one external
/// mutating operation: Publish or Finalize. Every recovery path observes the
/// authoritative durable head/scene status before retrying that exact effect.
class ManagerSelectedBundleTransitionCoordinator final {
 public:
   ManagerSelectedBundleTransitionCoordinator(
       SqliteManagerApplyJournal &apply_journal,
       const manyforge::deployment::IVerifiedRetainedBundleSource
           &retained_bundle_source,
       manyforge::core::IActiveBundlePublicationStore &publication_store,
       manyforge::core::IManagerSceneProjectionHandoffPort &scene_owner,
       const manyforge::deployment::IVerifiedRetainedProductArtifactSource
           *artifact_source = nullptr);

   [[nodiscard]] ManagerSelectedBundleTransitionResult
   AdvanceOrRecover(const manyforge::core::ApplyId &apply_id,
                    manyforge::core::ApplyGeneration apply_generation);

   ManagerSelectedBundleTransitionCoordinator(
       const ManagerSelectedBundleTransitionCoordinator &) = delete;
   ManagerSelectedBundleTransitionCoordinator &
   operator=(const ManagerSelectedBundleTransitionCoordinator &) = delete;

 private:
  SqliteManagerApplyJournal& apply_journal_;
  const manyforge::deployment::IVerifiedRetainedBundleSource& retained_bundle_source_;
  manyforge::core::IActiveBundlePublicationStore& publication_store_;
  manyforge::core::IManagerSceneProjectionHandoffPort& scene_owner_;
  const manyforge::deployment::IVerifiedRetainedProductArtifactSource* artifact_source_;
  std::mutex operation_mutex_;
};

}  // namespace manyforge::component_manager
