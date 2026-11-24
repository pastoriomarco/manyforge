#pragma once

// Private to the logical-scene owner sources; not installed.

#include "logical_scene_owner_internal.hpp"

#include <condition_variable>

namespace manyforge::scene {

class LogicalSceneOwner::Impl final {
public:
  Impl(const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader)
      : Impl(AuthorityFromAdmission(admission), publication_reader, nullptr,
             nullptr, nullptr) {}

  Impl(const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader,
       core::IKernelCellAdmissionPort &cell_admission)
      : Impl(AuthorityFromAdmission(admission), publication_reader, nullptr,
             nullptr, &cell_admission) {}

  Impl(const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource &retained_bundle_source)
      : Impl(AuthorityFromAdmission(admission), publication_reader,
             &retained_bundle_source, nullptr, nullptr) {}

  Impl(const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
       const core::IPlannerSceneProjectionResolver &projection_resolver)
      : Impl(AuthorityFromAdmission(admission), publication_reader,
             &retained_bundle_source, &projection_resolver, nullptr) {}

  Impl(const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
       const core::IPlannerSceneProjectionResolver &projection_resolver,
       core::IKernelCellAdmissionPort &cell_admission)
      : Impl(AuthorityFromAdmission(admission), publication_reader,
             &retained_bundle_source, &projection_resolver, &cell_admission) {}

  // Starts with no rows: the command-fenced empty state.
  Impl(AdmittedBootstrapAuthority authority,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource *retained_bundle_source,
       const core::IPlannerSceneProjectionResolver *projection_resolver,
       core::IKernelCellAdmissionPort *cell_admission)
      : fixed_query_(std::move(authority.query)),
        admitted_bundle_(std::move(authority.retained_bundle)),
        publication_reader_(publication_reader),
        retained_bundle_source_(retained_bundle_source),
        projection_resolver_(projection_resolver),
        cell_admission_(cell_admission) {
    ValidateFixedQuery(fixed_query_);
  }

  [[nodiscard]] DurableState ReadStateTransaction() const {
    return ReadCompleteDurableState(memory_);
  }

  // While a Commit is between its intent and its outcome, readers see the
  // settled predecessor (OD-376); writers read `memory_` and see the
  // unresolved intent.
  [[nodiscard]] const SceneCommitStateCache &ReadSceneCommitState() const {
    if (commit_in_flight_.has_value()) {
      return *commit_in_flight_;
    }
    if (scene_commit_state_cache_.has_value()) {
      return *scene_commit_state_cache_;
    }
    auto durable_state = ReadCompleteDurableState(memory_);
    std::optional<SceneTransactionStoreState> transaction_state;
    if (durable_state.accepted.has_value() &&
        durable_state.finalization_receipt.has_value()) {
      transaction_state =
          ReadSceneTransactionStoreState(memory_, durable_state);
    }
    scene_commit_state_cache_ = SceneCommitStateCache{
        std::move(durable_state), std::move(transaction_state)};
    return *scene_commit_state_cache_;
  }

  void StoreSceneCommitState(
      DurableState durable_state,
      std::optional<SceneTransactionStoreState> transaction_state) {
    scene_commit_state_cache_ = SceneCommitStateCache{
        std::move(durable_state), std::move(transaction_state)};
  }

  // Every change of `memory_` calls this first, so an unchanged generation
  // proves an unchanged store (Commit's compare-and-commit, OD-376).
  void InvalidateSceneCommitState() noexcept {
    scene_commit_state_cache_.reset();
    ++state_generation_;
  }

  // Under `mutex_`: ends the in-flight Commit and wakes the Apply-lane
  // writers waiting for it.
  void EndCommitInFlight() noexcept {
    commit_in_flight_.reset();
    commit_settled_.notify_all();
  }

  // Under `lock` on `mutex_`: an Apply-lane writer waits, with the lock
  // released, until no Commit is in flight, then re-reads the state (B1).
  void AwaitNoCommitInFlight(std::unique_lock<std::mutex> &lock) {
    commit_settled_.wait(lock, [this] { return !commit_in_flight_.has_value(); });
  }

  ProjectionBootstrapPrerequisiteQueryDTO fixed_query_;
  RetainedBundleDTO admitted_bundle_;
  const core::IActiveBundlePublicationReader &publication_reader_;
  const deployment::IVerifiedRetainedBundleSource *retained_bundle_source_;
  const core::IPlannerSceneProjectionResolver *projection_resolver_;
  core::IKernelCellAdmissionPort *cell_admission_;
  // Only the original live continuation can complete a pending private
  // adoption. Recorded rows are evidence, not a replacement for that request.
  // Historical ordinary maintenance also preserved Scene revisions, so the
  // revision shape cannot distinguish a private operation from those rows.
  std::optional<QualifiedSceneConfigurationAdoptionRequest> live_adoption_;
  mutable std::mutex maintenance_operation_mutex_;
  mutable std::mutex mutex_;
  SceneStoreMemory memory_;
  mutable std::optional<SceneCommitStateCache> scene_commit_state_cache_;
  std::uint64_t state_generation_{};
  // The settled state of a Commit in flight between its intent and its
  // outcome; set and cleared under `mutex_` by that Commit only.
  std::optional<SceneCommitStateCache> commit_in_flight_;
  std::condition_variable commit_settled_;
  // Numbers each ReadPublication under `mutex_` (OD-302).
  mutable std::uint64_t publication_read_sequence_{};
};

} // namespace manyforge::scene
