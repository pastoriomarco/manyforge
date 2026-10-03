#pragma once

#include <filesystem>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/retained_collision_world_projector.hpp>
#include <manyforge_roboplan/retained_planner_scene_projection.hpp>
#include <manyforge_roboplan/retained_scene_frame_resolver.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "native_projection_rpc_internal.hpp"

namespace manyforge::roboplan_backend {

/// The runtime-child facts the projection registry needs. Every path names a
/// directory capability already validated by the managed child.
struct ManagedProjectionRegistryConfig final {
  std::filesystem::path package_root;
  /// Empty when the Program artifact root is the package root itself.
  std::filesystem::path program_artifact_root;
  std::filesystem::path execution_run_root;
  std::string runtime_unit_id;
  std::string runtime_epoch;
};

/// The exact candidate every provider Prepare names. Providers prepared for
/// one candidate share its retained bundle, model map and planning model.
struct ManagedProjectionCandidate final {
  core::RetainedBundleId retained_bundle_id;
  core::GraphRevision graph_revision;
  core::RetainedProductDTO planning_collision_model;

  friend bool operator==(const ManagedProjectionCandidate &,
                         const ManagedProjectionCandidate &) = default;
};

/// One candidate's verified inputs and its immutable per-group models.
///
/// Verification and model construction run once per candidate and compiled
/// group, not once per provider. The per-request group selection (which also
/// performs that provider's binding checks) runs on every Acquire. Exactly
/// one candidate is held: a different candidate replaces it and Release()
/// drops it.
///
/// Lock discipline: the cache owns one mutex, held while its callbacks run.
/// Callers must hold no other lock when calling Acquire or Release, and the
/// callbacks must not call back into their owner's locked state.
template <typename Inputs, typename Model>
class ManagedCandidateProjectionCache final {
public:
  template <typename Verify, typename SelectGroup, typename Construct>
  [[nodiscard]] Model Acquire(const ManagedProjectionCandidate &candidate,
                              Verify &&verify, SelectGroup &&select_group,
                              Construct &&construct) {
    std::lock_guard lock(mutex_);
    if (!entry_.has_value() || entry_->candidate != candidate) {
      entry_.reset();
      entry_.emplace(Entry{candidate, verify(), {}});
    }
    const std::string group = select_group(std::as_const(entry_->inputs));
    auto model = entry_->models.find(group);
    if (model == entry_->models.end()) {
      model = entry_->models
                  .emplace(group,
                           construct(std::as_const(entry_->inputs), group))
                  .first;
    }
    return model->second;
  }

  void Release() {
    std::lock_guard lock(mutex_);
    entry_.reset();
  }

private:
  struct Entry final {
    ManagedProjectionCandidate candidate;
    Inputs inputs;
    std::map<std::string, Model> models;
  };

  std::mutex mutex_;
  std::optional<Entry> entry_;
};

/// Verified retained inputs shared by every provider of one candidate.
struct ManagedProjectionCandidateInputs final {
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  deployment::VerifiedRetainedModelMap model_map;
  deployment::VerifiedRetainedPlanningModel planning_model;
};

/// Retained-input verification and shared collision-model construction.
class IManagedProjectionAssetSource {
public:
  virtual ~IManagedProjectionAssetSource() = default;

  /// Verifies the candidate's retained bundle, model map and planning model.
  [[nodiscard]] virtual ManagedProjectionCandidateInputs
  VerifyCandidate(const ManagedProjectionCandidate &candidate) const = 0;

  /// Builds the immutable collision projector for one compiled group.
  [[nodiscard]] virtual RetainedCollisionWorldProjector
  ConstructGroupProjector(const ManagedProjectionCandidateInputs &inputs,
                          const std::string &group) const = 0;
};

/// Production source: reads the pinned package root first and the Program
/// artifact root only as the explicit fallback.
[[nodiscard]] std::unique_ptr<const IManagedProjectionAssetSource>
MakeFilesystemManagedProjectionAssetSource(
    const ManagedProjectionRegistryConfig &config);

/// Registry of the exact native projections owned by one runtime child.
///
/// Lock order: the registry mutex is never held across a projection call, a
/// future wait, retained-artifact verification or model construction, and is
/// never held together with the candidate cache's mutex.
class ManagedProjectionRegistry final {
public:
  ManagedProjectionRegistry(
      ManagedProjectionRegistryConfig config,
      std::unique_ptr<const IManagedProjectionAssetSource> assets,
      RetainedPlannerSceneProjection::Clock monotonic_now_ns);
  ~ManagedProjectionRegistry();
  ManagedProjectionRegistry(const ManagedProjectionRegistry &) = delete;
  ManagedProjectionRegistry &
  operator=(const ManagedProjectionRegistry &) = delete;

  [[nodiscard]] component_manager::detail::NativeProjectionRpcResponse
  Dispatch(const component_manager::detail::NativeProjectionRpcRequest &request);

  /// Called only after exact runtime installation receipts have been verified,
  /// including an empty installation. Ends candidate preparation inputs.
  void ConfirmInstalledProjectionSet(const deployment::RetainedBundle &bundle);

  void ReclaimRetiredProjections();

  /// Waits for accepted projection work; only after request ingress closed.
  void Drain();

  [[nodiscard]] std::shared_ptr<RetainedPlannerSceneProjection>
  FindActiveProjection(
      const core::ProjectionRevisionDTO &expected,
      const core::ProjectionNativeRuntimeBindingDTO &binding) const;

  [[nodiscard]] std::optional<core::ProjectionRevisionDTO>
  FindCurrentProjectionRevision(
      const core::ProjectionRevisionDTO &expected,
      const core::ProjectionNativeRuntimeBindingDTO &binding) const;

  [[nodiscard]] bool
  AllRequiredProjectionsHealthy(const deployment::RetainedBundle &bundle,
                                const core::ProjectionRevisionDTO &expected) const;

  void InstallActiveFrameResolverSet(
      const deployment::RetainedBundle &bundle,
      const core::ProjectionRevisionDTO &expected,
      std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace manyforge::roboplan_backend
