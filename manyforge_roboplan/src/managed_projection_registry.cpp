#include "managed_projection_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#if defined(MANYFORGE_ENABLE_NATIVE_PROJECTION_CONSTRUCTION_TEST_BOUNDARY)
#include <fstream>
#endif
#include <future>
#include <iostream>
#include <limits>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <manyforge_roboplan/retained_planning_model_stage.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

namespace component_manager = manyforge::component_manager;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

[[noreturn]] void Fail(std::string detail) {
  throw std::runtime_error("Native execution host: " + std::move(detail));
}

void Require(bool condition, std::string detail) {
  if (!condition) {
    Fail(std::move(detail));
  }
}

using ManagedProjectionKey = std::pair<core::CapabilityProviderBindingId,
                                       core::CapabilityProviderBindingRevision>;

#if defined(MANYFORGE_ENABLE_NATIVE_PROJECTION_CONSTRUCTION_TEST_BOUNDARY)
void AppendNativeProjectionTestTrace(const std::string &line) {
  const char *const root =
      std::getenv("MANYFORGE_NATIVE_PROJECTION_TEST_CONTROL_DIR");
  if (root == nullptr || *root == '\0') {
    return;
  }
  static std::mutex trace_mutex;
  std::lock_guard lock(trace_mutex);
  std::ofstream stream(std::filesystem::path(root) / "trace.log",
                       std::ios::binary | std::ios::app);
  Require(static_cast<bool>(stream),
          "native projection test trace could not be opened");
  stream << line << '\n';
  Require(static_cast<bool>(stream),
          "native projection test trace could not be written");
}
#endif

class FilesystemManagedProjectionAssetSource final
    : public IManagedProjectionAssetSource {
public:
  explicit FilesystemManagedProjectionAssetSource(
      ManagedProjectionRegistryConfig config)
      : config_(std::move(config)) {}

  [[nodiscard]] ManagedProjectionCandidateInputs
  VerifyCandidate(const ManagedProjectionCandidate &candidate) const override {
    auto selected_root = config_.package_root;
    deployment::FilesystemVerifiedRetainedBundleSource selected_source(
        selected_root);
    auto bundle = selected_source.get_verified(candidate.retained_bundle_id);
    if (bundle == nullptr && !config_.program_artifact_root.empty() &&
        config_.program_artifact_root != selected_root) {
      selected_root = config_.program_artifact_root;
      deployment::FilesystemVerifiedRetainedBundleSource fallback_source(
          selected_root);
      bundle = fallback_source.get_verified(candidate.retained_bundle_id);
    }
    Require(bundle != nullptr &&
                bundle->value().cell_graph.graph_revision ==
                    candidate.graph_revision &&
                bundle->value().compiled_products.planning_collision_model ==
                    candidate.planning_collision_model,
            "prepared native projection differs from bundles in the two "
            "verified retained roots");
    const auto &products = bundle->value().compiled_products;
    Require(products.symbol_role_maps.size() == 1U,
            "native projection requires one exact retained model map");
    const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
        selected_root);
    auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
        bundle->retained_bundle_id(),
        products.symbol_role_maps.front().product_id});
    Require(model_map.has_value(),
            "native projection retained model map is absent or invalid");
    const deployment::FilesystemVerifiedRetainedPlanningModelSource
        planning_source(selected_root);
    auto planning_model =
        planning_source.get_verified(deployment::RetainedPlanningModelQuery{
            bundle->retained_bundle_id(),
            candidate.planning_collision_model.product_id});
    Require(planning_model.has_value(),
            "native projection retained planning model is absent or invalid");
    return ManagedProjectionCandidateInputs{std::move(bundle),
                                            std::move(*model_map),
                                            std::move(*planning_model)};
  }

  [[nodiscard]] RetainedCollisionWorldProjector
  ConstructGroupProjector(const ManagedProjectionCandidateInputs &inputs,
                          const std::string &group) const override {
    auto stage = RetainedPlanningModelStage::create_verified(
        inputs.planning_model, config_.execution_run_root);
    // Scene meshes resolve only below the approved scene-asset root; the
    // retained package root is never a scene-mesh search path.
    auto shared_model = collision::ManyForgeCollisionEngine::load_shared_model(
        "manyforge_managed_native_projection", stage.loader_urdf_path(),
        stage.srdf_path(), stage.package_paths(), group,
        config_.scene_asset_root_fd);
    return RetainedCollisionWorldProjector::create_verified(
        std::move(shared_model), *inputs.bundle, inputs.model_map,
        inputs.planning_model);
  }

private:
  ManagedProjectionRegistryConfig config_;
};

} // namespace

std::unique_ptr<const IManagedProjectionAssetSource>
MakeFilesystemManagedProjectionAssetSource(
    const ManagedProjectionRegistryConfig &config) {
  return std::make_unique<FilesystemManagedProjectionAssetSource>(config);
}

/// Registry of the exact native projections owned by this runtime child.
///
/// Lock order (AP-R1): the registry mutex_ is never held across a projection
/// call, a future wait, or retained-artifact/model construction. A Rebuild
/// worker owns the projection's own mutex for the whole collision-world
/// construction, so every request path resolves the exact projection handle and
/// the pending-job identity under mutex_, releases it, and only then calls into
/// the projection. Worker threads acquire no registry lock at all, so joining a
/// settled worker under mutex_ cannot deadlock, and one provider's pending job
/// can never delay another provider's submission or an ordinary health/status
/// read. Drain() is the single documented exception: it runs only after request
/// ingress has closed, when no worker can still be waiting on the strand.
class ManagedProjectionRegistry::Impl final {
public:
  Impl(ManagedProjectionRegistryConfig config,
       std::unique_ptr<const IManagedProjectionAssetSource> assets,
       RetainedPlannerSceneProjection::Clock monotonic_now_ns)
      : config_(std::move(config)), assets_(std::move(assets)),
        clock_(std::move(monotonic_now_ns)) {
    Require(assets_ != nullptr && clock_ != nullptr,
            "native projection registry requires assets and a clock");
  }

  [[nodiscard]] component_manager::detail::NativeProjectionRpcResponse Dispatch(
      const component_manager::detail::NativeProjectionRpcRequest &request) {
    // No registry lock is taken here. Each branch below performs its own short
    // locked resolution and then runs the projection effect unlocked.
    return std::visit(
        [&](const auto &value)
            -> component_manager::detail::NativeProjectionRpcResponse {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, component_manager::detail::
                                              NativeProjectionPrepareRequest>) {
            Prepare(value);
            return component_manager::detail::NativeProjectionPrepareResult{
                value};
          } else if constexpr (std::is_same_v<
                                   T, component_manager::detail::
                                          NativeProjectionHealthQuery>) {
            return ProjectionHealth(value.native_runtime_binding);
          } else if constexpr (std::is_same_v<
                                   T, component_manager::detail::
                                          NativeProjectionTransactionRequest>) {
            return ApplyTransaction(value);
          } else if constexpr (std::is_same_v<
                                   T, core::NativeProjectionRebuildDTO>) {
            return BeginRebuild(value);
          } else if constexpr (std::is_same_v<
                                   T,
                                   core::NativeProjectionBootstrapRebuildDTO>) {
            return BootstrapRebuild(value);
          } else if constexpr (
              std::is_same_v<T,
                             core::NativeProjectionBootstrapStatusQueryDTO>) {
            return BootstrapStatus(value);
          } else if constexpr (std::is_same_v<
                                   T, component_manager::detail::
                                          NativeProjectionRetainRequest>) {
            auto owner = LeaseOwner(value.native_runtime_binding,
                                    value.projection_lease_id);
            if (owner == nullptr) {
              owner = RevisionHandle(value.native_runtime_binding,
                                     value.projection_revision);
            }
            auto retained =
                IdleHandle(value.native_runtime_binding, "retain", owner)
                    ->RetainProjectionBinding(value.projection_revision,
                                             value.projection_lease_id);
            if (!retained.ok()) {
              return component_manager::detail::NativeProjectionRpcFailure{
                  retained.error()};
            }
            return component_manager::detail::NativeProjectionRetainResult{
                std::move(retained.value())};
          } else if constexpr (std::is_same_v<
                                   T, component_manager::detail::
                                          NativeProjectionReleaseRequest>) {
            auto owner = LeaseOwner(value.native_runtime_binding,
                                    value.projection_lease_id);
            if (owner != nullptr) {
              IdleHandle(value.native_runtime_binding, "release", owner)
                  ->ReleaseProjection(value.projection_lease_id);
            } else {
              // Release is idempotent. An absent lease must not be redirected
              // to a progressing candidate, nor acknowledge a foreign binding.
              Require(Find(value.native_runtime_binding) != nullptr,
                      "native projection release names an unprepared binding");
            }
            owner.reset();
            ReclaimRetiredProjections();
            return component_manager::detail::NativeProjectionReleaseResult{
                value.projection_lease_id};
          } else if constexpr (std::is_same_v<
                                   T, component_manager::detail::
                                          NativeProjectionSceneFrameRequest>) {
            auto resolved =
                IdleHandle(value.native_runtime_binding, "frame_resolution",
                           LeaseOwner(value.native_runtime_binding,
                                      value.query.projection_binding
                                          .projection_lease_id))
                    ->ResolveSceneFrame(value.query);
            if (!resolved.ok()) {
              return component_manager::detail::NativeProjectionRpcFailure{
                  resolved.error()};
            }
            return std::move(resolved.value());
          } else if constexpr (
              std::is_same_v<T, component_manager::detail::
                                    NativeProjectionProviderFreshnessRequest>) {
            return ApplyProviderFreshness(value);
          } else {
            static_assert(
                std::is_same_v<T, component_manager::detail::
                                      NativeProjectionEffectivePolicyRequest>);
            auto projected =
                IdleHandle(value.native_runtime_binding, "policy_projection",
                           PolicyHandle(value.native_runtime_binding,
                                        value.logical_scene))
                    ->ProjectEffectiveCollisionPolicy(
                        value.logical_scene,
                        value.effective_policy_revision_id);
            if (!projected.ok()) {
              return component_manager::detail::NativeProjectionRpcFailure{
                  projected.error()};
            }
            return std::move(projected.value());
          }
        },
        request);
  }

  /// Called only after exact runtime installation receipts have been verified,
  /// including an empty installation. Preparing an added provider does not
  /// make it part of the predecessor's installed set, nor make it disposable.
  void ConfirmInstalledProjectionSet(const deployment::RetainedBundle &bundle) {
    std::set<ManagedProjectionKey> active_keys;
    for (const auto &provider : bundle.value().capability_provider_bindings) {
      if (core::RequiresNativeSceneProjection(
              provider.capability_id,
              provider.capability_definition_revision) &&
          std::any_of(provider.endpoint_bindings.begin(),
                      provider.endpoint_bindings.end(), [&](const auto &row) {
                        const auto *unit =
                            std::get_if<core::RuntimeUnitId>(&row.source_id);
                        return row.source_kind ==
                                   core::CapabilityEndpointSourceKind::kRuntimeUnit &&
                               unit != nullptr &&
                               unit->value() == config_.runtime_unit_id;
                      })) {
        active_keys.insert({provider.capability_provider_binding_id,
                            provider.capability_provider_binding_revision});
      }
    }
    const auto product = std::pair{bundle.retained_bundle_id(),
                                  bundle.value().cell_graph.graph_revision};
    // Installed projections own their models; preparation inputs end here.
    candidate_inputs_.Release();
    std::lock_guard lock(mutex_);
    installed_product_ = product;
    installed_provider_keys_ = std::move(active_keys);
    if (installed_provider_keys_.empty()) {
      // A no-projection installation has no Prepare requests to name it.
      prepared_product_ = product;
    }
  }

  /// Two slots per provider, not a historical model cache. A predecessor stays
  /// addressable until both its composition references and exact leases have
  /// gone. Projection reads and destruction never run under the registry lock.
  void ReclaimRetiredProjections() {
    std::vector<std::pair<ManagedProjectionKey, RegisteredProjection>> retired;
    std::vector<std::pair<ManagedProjectionKey, RegisteredProjection>> removed;
    {
      std::lock_guard lock(mutex_);
      for (const auto &entry : retired_projections_) {
        retired.push_back(entry);
      }
      if (installed_product_.has_value() &&
          prepared_product_ == installed_product_) {
        for (const auto &entry : projections_) {
          if (!installed_provider_keys_.contains(entry.first)) {
            removed.push_back(entry);
          }
        }
      }
    }
    const auto reclaim = [&](auto &slots, auto &snapshot) {
      for (auto &[key, entry] : snapshot) {
        if (WorkPendingFor(key, entry.projection) ||
            entry.projection->scene_snapshot_lease_retention_stats()
                    .retained_count != 0U) {
          continue;
        }
        std::optional<PendingRebuild> terminal;
        std::lock_guard lock(mutex_);
        const auto found = slots.find(key);
        const auto job = pending_rebuilds_.find(key);
        const bool job_owns_slot = job != pending_rebuilds_.end() &&
                                   job->second.projection == entry.projection;
        if (found != slots.end() &&
            found->second.projection == entry.projection &&
            entry.projection.use_count() == (job_owns_slot ? 3 : 2) &&
            !WorkPendingForLocked(key, entry.projection)) {
          if (job_owns_slot) {
            terminal = TakeSettledRebuildLocked(key);
          }
          // The local snapshot keeps destruction outside mutex_.
          slots.erase(found);
        }
      }
    };
    reclaim(retired_projections_, retired);
    // Provider removals/revisions are retired too; they must not accumulate
    // as an unlimited cache across repeated Deployment replacements.
    reclaim(projections_, removed);
  }

  void Drain() {
    // Request ingress is stopped before this is called. Keep the registry and
    // every active composition dependency alive until all accepted projection
    // work has reached a terminal result. Waiting here is allowed only because
    // ingress is closed: a projection worker acquires no registry lock and
    // never waits for the request strand, so neither the wait nor the join of
    // an already accepted worker can deadlock against work owned here. The
    // Rebuild workers never need registry state.
    std::lock_guard lock(mutex_);
    for (auto &[_, pending] : pending_rebuilds_) {
      pending.result.wait();
      pending.worker.join();
    }
  }

  [[nodiscard]] std::shared_ptr<RetainedPlannerSceneProjection>
  FindActiveProjection(
      const core::ProjectionRevisionDTO &expected,
      const core::ProjectionNativeRuntimeBindingDTO &binding) const {
    const auto projection = FindRevision(binding, expected);
    if (projection == nullptr || WorkPendingFor(Key(binding), projection)) {
      return nullptr;
    }
    const auto inventory = projection->current_scene_snapshot_lease_inventory();
    return inventory.has_value() &&
                   inventory->projection_binding.projection_revision == expected
               ? projection
               : nullptr;
  }

  [[nodiscard]] std::optional<core::ProjectionRevisionDTO>
  FindCurrentProjectionRevision(
      const core::ProjectionRevisionDTO &expected,
      const core::ProjectionNativeRuntimeBindingDTO &binding) const {
    const auto handle = FindRevision(binding, expected);
    if (handle == nullptr || WorkPendingFor(Key(binding), handle)) {
      return std::nullopt;
    }
    const auto inventory = handle->current_scene_snapshot_lease_inventory();
    if (!inventory.has_value()) {
      return std::nullopt;
    }
    const auto &projection = inventory->projection_binding.projection_revision;
    if (!projection.valid() ||
        projection.retained_bundle_id != expected.retained_bundle_id ||
        projection.graph_revision != expected.graph_revision ||
        projection.scene_revision.scene_id !=
            expected.scene_revision.scene_id ||
        !handle->ProjectionHealth().healthy_for_target(projection)) {
      return std::nullopt;
    }
    return projection;
  }

  [[nodiscard]] bool AllRequiredProjectionsHealthy(
      const deployment::RetainedBundle &bundle,
      const core::ProjectionRevisionDTO &expected) const {
    // Handles and pending-job identity are resolved with a short lock, and
    // every ProjectionHealth read happens after that lock is released. A
    // provider whose worker is constructing a candidate therefore cannot hold
    // up this readiness hint, nor any other provider's assessment.
    const auto registered = Registered();
    bool found = false;
    for (const auto &provider : bundle.value().capability_provider_bindings) {
      if (!core::RequiresNativeSceneProjection(
              provider.capability_id,
              provider.capability_definition_revision)) {
        continue;
      }
      std::set<core::RuntimeUnitId> runtime_sources;
      for (const auto &endpoint : provider.endpoint_bindings) {
        if (endpoint.source_kind !=
            core::CapabilityEndpointSourceKind::kRuntimeUnit) {
          continue;
        }
        const auto *runtime_unit =
            std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
        if (runtime_unit == nullptr || runtime_unit->empty()) {
          return false;
        }
        runtime_sources.insert(*runtime_unit);
      }
      if (runtime_sources.size() != 1U ||
          runtime_sources.begin()->value() != config_.runtime_unit_id) {
        continue;
      }
      const ManagedProjectionKey key{
          provider.capability_provider_binding_id,
          provider.capability_provider_binding_revision};
      if (WorkPending(key)) {
        ReportProjectionReadiness("waiting for " + key.first.value());
        return false;
      }
      const auto entry = std::find_if(
          registered.begin(), registered.end(), [&](const auto &candidate) {
            return candidate.first == key;
          });
      if (entry == registered.end() || entry->second.projection == nullptr) {
        ReportProjectionReadiness("missing " + key.first.value());
        return false;
      }
      const core::ProjectionNativeRuntimeBindingDTO binding{
          provider.capability_provider_binding_id,
          provider.capability_provider_binding_revision,
          core::RuntimeUnitId(config_.runtime_unit_id),
          core::RuntimeEpoch(config_.runtime_epoch)};
      const auto health = entry->second.projection->ProjectionHealth();
      if (health.native_runtime_binding != binding ||
          !health.healthy_for_target(expected)) {
        ReportProjectionReadiness("not healthy " + key.first.value());
        return false;
      }
      found = true;
    }
    // A component-local controller can have no planning/collision provider.
    // There is then no projection lease to wait for; Scene authority and
    // command admission are still verified at their own boundaries.
    ReportProjectionReadiness(found ? "ready" : "no required projection");
    return true;
  }

  void InstallActiveFrameResolverSet(
      const deployment::RetainedBundle &bundle,
      const core::ProjectionRevisionDTO &expected,
      std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver) {
    Require(expected.valid() && frame_resolver != nullptr &&
                frame_resolver->matches_structural_revision(
                    expected.retained_bundle_id, expected.graph_revision) &&
                bundle.retained_bundle_id() == expected.retained_bundle_id &&
                bundle.value().cell_graph.graph_revision ==
                    expected.graph_revision,
            "active frame-resolver set does not name one retained product");

    // Required handles are resolved under mutex_; the stale-row validation and
    // the resolver installation itself run afterwards, so a provider whose
    // worker holds the projection mutex cannot block this composition step.
    std::set<ManagedProjectionKey> required_keys;
    std::vector<std::pair<core::ProjectionNativeRuntimeBindingDTO,
                          std::shared_ptr<RetainedPlannerSceneProjection>>>
        required;
    {
      std::lock_guard lock(mutex_);
      for (const auto &provider :
           bundle.value().capability_provider_bindings) {
        if (!core::RequiresNativeSceneProjection(
                provider.capability_id,
                provider.capability_definition_revision)) {
          continue;
        }
        std::set<core::RuntimeUnitId> runtime_sources;
        for (const auto &endpoint : provider.endpoint_bindings) {
          if (endpoint.source_kind !=
              core::CapabilityEndpointSourceKind::kRuntimeUnit) {
            continue;
          }
          const auto *runtime_unit =
              std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
          Require(runtime_unit != nullptr && !runtime_unit->empty(),
                  "active native projection has a malformed runtime endpoint");
          runtime_sources.insert(*runtime_unit);
        }
        Require(runtime_sources.size() == 1U,
                "active native projection does not resolve one runtime unit");
        if (runtime_sources.begin()->value() != config_.runtime_unit_id) {
          continue;
        }

        const ManagedProjectionKey key{
            provider.capability_provider_binding_id,
            provider.capability_provider_binding_revision};
        Require(required_keys.insert(key).second,
                "active native projection binding is repeated");
        const auto found = projections_.find(key);
        Require(found != projections_.end() &&
                    found->second.projection != nullptr,
                "active frame-resolver set lacks a required projection");
        Require(!WorkPendingLocked(key),
                "active frame-resolver set names progressing projection work");
        const core::ProjectionNativeRuntimeBindingDTO expected_binding{
            provider.capability_provider_binding_id,
            provider.capability_provider_binding_revision,
            core::RuntimeUnitId(config_.runtime_unit_id),
            core::RuntimeEpoch(config_.runtime_epoch)};
        required.emplace_back(expected_binding, found->second.projection);
      }
    }
    Require(!required.empty(),
            "active retained product has no native projection set");

    for (const auto &[binding, projection] : required) {
      const auto health = projection->ProjectionHealth();
      Require(health.native_runtime_binding == binding &&
                  health.healthy_for_target(expected),
              "active frame-resolver set contains a stale projection");
    }
    for (const auto &[_, projection] : required) {
      auto installed =
          projection->InstallActiveFrameResolver(expected, frame_resolver);
      if (!installed.ok()) {
        Fail("direct native frame resolver set installation failed: " +
             installed.error().message);
      }
    }
  }

private:
  struct JobProgress final {
    JobProgress(std::string operation_value,
                core::ProjectionNativeRuntimeBindingDTO binding_value,
                std::string initial_milestone,
                RetainedPlannerSceneProjection::Clock clock_value)
        : operation(std::move(operation_value)),
          binding(std::move(binding_value)), clock(std::move(clock_value)),
          started_at_ns(clock()), last_progress_at_ns(started_at_ns) {
      Advance(std::move(initial_milestone));
    }

    void Advance(std::string next_milestone) {
      std::lock_guard lock(mutex);
      const auto now = clock();
      const auto since_start = (now - started_at_ns) / 1'000'000;
      const auto since_last = (now - last_progress_at_ns) / 1'000'000;
      milestone = std::move(next_milestone);
      ++sequence;
      last_progress_at_ns = now;
      std::cerr << "native projection job progress: operation=" << operation
                << " provider=" << binding.provider_binding_id.value()
                << " milestone=" << milestone << " sequence=" << sequence
                << " elapsed_ms=" << since_start
                << " since_last_progress_ms=" << since_last << '\n';
#if defined(MANYFORGE_ENABLE_NATIVE_PROJECTION_CONSTRUCTION_TEST_BOUNDARY)
      AppendNativeProjectionTestTrace(
          "progress " + operation + " " +
          binding.provider_binding_id.value() + " " +
          std::to_string(sequence) + " " + milestone);
#endif
    }

    std::string operation;
    core::ProjectionNativeRuntimeBindingDTO binding;
    RetainedPlannerSceneProjection::Clock clock;
    std::int64_t started_at_ns;
    std::int64_t last_progress_at_ns;
    std::uint64_t sequence{};
    std::string milestone;
    std::mutex mutex;
  };

  struct JoinedWorker final {
    std::thread thread;

    JoinedWorker() = default;
    explicit JoinedWorker(std::thread value) : thread(std::move(value)) {}
    JoinedWorker(JoinedWorker &&) noexcept = default;
    JoinedWorker &operator=(JoinedWorker &&) noexcept = default;
    JoinedWorker(const JoinedWorker &) = delete;
    JoinedWorker &operator=(const JoinedWorker &) = delete;
    ~JoinedWorker() { join(); }

    void join() {
      if (thread.joinable()) {
        thread.join();
      }
    }
  };

  struct PendingRebuild final {
    core::NativeProjectionRebuildDTO request;
    core::ProjectionHealthDTO accepted_health;
    std::shared_future<core::ProjectionCommitResultDTO> result;
    std::shared_ptr<JobProgress> progress;
    std::shared_ptr<RetainedPlannerSceneProjection> projection;
    std::vector<core::ProjectionLeaseId> retained_lease_ids;
    JoinedWorker worker;
  };

  struct PreparedIdentity final {
    core::RetainedBundleId retained_bundle_id;
    core::GraphRevision graph_revision;
    core::RetainedProductDTO planning_collision_model;
  };

  struct RegisteredProjection final {
    /// One immutable retained model under its exact provider/runtime binding.
    /// Candidate staging may keep its predecessor under the same binding;
    /// revision and lease selectors, never the provider key alone, choose the
    /// owner for revision-bearing requests.
    core::ProjectionNativeRuntimeBindingDTO binding;
    std::shared_ptr<RetainedPlannerSceneProjection> projection;
    PreparedIdentity identity;
  };

  template <typename Value>
  [[nodiscard]] static bool Settled(
      const std::shared_future<Value> &future) {
    // A zero-timeout poll is a state read, not a wait: it never parks the
    // request strand on projection work.
    return future.valid() && future.wait_for(std::chrono::seconds::zero()) ==
                                 std::future_status::ready;
  }

  template <typename Value>
  [[nodiscard]] static std::optional<Value> ReadSettled(
      const std::shared_future<Value> &future) {
    try {
      return future.get();
    } catch (...) {
      // A worker that terminated abnormally still has exactly one outcome to
      // report; the caller turns the absence into the typed uncertain result.
      return std::nullopt;
    }
  }

  [[nodiscard]] bool WorkPendingLocked(const ManagedProjectionKey &key) const {
    const auto rebuild = pending_rebuilds_.find(key);
    return rebuild != pending_rebuilds_.end() &&
           !Settled(rebuild->second.result);
  }

  [[nodiscard]] bool WorkPending(const ManagedProjectionKey &key) const {
    std::lock_guard lock(mutex_);
    return WorkPendingLocked(key);
  }

  [[nodiscard]] bool WorkPendingForLocked(
      const ManagedProjectionKey &key,
      const std::shared_ptr<RetainedPlannerSceneProjection> &projection) const {
    const auto found = pending_rebuilds_.find(key);
    return found != pending_rebuilds_.end() &&
           found->second.projection == projection &&
           !Settled(found->second.result);
  }

  [[nodiscard]] bool WorkPendingFor(
      const ManagedProjectionKey &key,
      const std::shared_ptr<RetainedPlannerSceneProjection> &projection) const {
    std::lock_guard lock(mutex_);
    return WorkPendingForLocked(key, projection);
  }

  /// Health captured for the job currently owned by this provider, if any.
  /// Pending work must be observed as pending predecessor health rather than
  /// by acquiring the mutex of the projection its worker is holding.
  [[nodiscard]] std::optional<core::ProjectionHealthDTO>
  CapturedJobHealthLocked(
      const ManagedProjectionKey &key,
      const std::shared_ptr<RetainedPlannerSceneProjection> &projection =
          nullptr) const {
    const auto rebuild = pending_rebuilds_.find(key);
    if (rebuild != pending_rebuilds_.end() &&
        !Settled(rebuild->second.result) &&
        (projection == nullptr || rebuild->second.projection == projection)) {
      return rebuild->second.accepted_health;
    }
    return std::nullopt;
  }

  /// One stable terminal result per retained job. An abnormal worker
  /// termination becomes the existing typed uncertain contract, carrying the
  /// operation and provider identity needed to reconcile a lost response.
  [[nodiscard]] static core::ProjectionCommitResultDTO RebuildOutcome(
      const PendingRebuild &job) {
    if (const auto settled = ReadSettled(job.result); settled.has_value()) {
      return *settled;
    }
    return UncertainRebuild(
        job.request, job.accepted_health,
        WorkerFailure("rebuild", job.request.expected_native_runtime_binding,
                      "native_projection_rebuild_aborted",
                      "the Rebuild worker did not settle"));
  }

  [[nodiscard]] static core::ErrorDTO WorkerFailure(
      const std::string_view operation,
      const core::ProjectionNativeRuntimeBindingDTO &binding,
      const std::string_view reason, const std::string_view detail) {
    return core::ErrorDTO{
        "projection_state_uncertain",
        std::string(operation) + " for native projection provider " +
            binding.provider_binding_id.value() +
            " ended without a terminal result: " + std::string(detail),
        {{"reason", std::string(reason)},
         {"operation", std::string(operation)},
         {"provider_binding_id", binding.provider_binding_id.value()}}};
  }

  /// Forces only the state needed to say "candidate readiness is not proven".
  /// Observed revisions are left untouched: an exception alone does not prove
  /// that an effect rolled back.
  [[nodiscard]] static core::ProjectionHealthDTO UncertainHealth(
      core::ProjectionHealthDTO health, const core::ErrorDTO &failure) {
    if (health.state != core::ProjectionHealthState::kDesynchronized) {
      health.state = core::ProjectionHealthState::kUnavailable;
    }
    health.error = failure;
    return health;
  }

  [[nodiscard]] static core::ProjectionCommitResultDTO UncertainRebuild(
      const core::NativeProjectionRebuildDTO &request,
      core::ProjectionHealthDTO health, const core::ErrorDTO &failure) {
    auto uncertain = UncertainHealth(std::move(health), failure);
    return core::ProjectionCommitResultDTO{
        std::nullopt,
        request.target_projection_revision,
        core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain,
        std::nullopt,
        uncertain.native_projection_generation,
        false,
        std::move(uncertain),
        failure};
  }

  /// Takes one settled Rebuild worker result without waiting.
  [[nodiscard]] std::optional<PendingRebuild> TakeSettledRebuildLocked(
      const ManagedProjectionKey &key) {
    const auto rebuild = pending_rebuilds_.find(key);
    if (rebuild == pending_rebuilds_.end() ||
        !Settled(rebuild->second.result)) {
      return std::nullopt;
    }
    auto taken = std::make_optional(std::move(rebuild->second));
    pending_rebuilds_.erase(rebuild);
    return taken;
  }

  [[nodiscard]] std::vector<
      std::pair<ManagedProjectionKey, RegisteredProjection>>
  Registered() const {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<ManagedProjectionKey, RegisteredProjection>>
        registered;
    registered.reserve(projections_.size());
    for (const auto &[key, entry] : projections_) {
      registered.emplace_back(key, entry);
    }
    return registered;
  }

  void ReportProjectionReadiness(std::string detail) const {
    // Readiness lines are emitted from the request strand and the composition
    // builders, so this deduplication has its own lock instead of borrowing the
    // registry mutex that callers deliberately release early.
    std::lock_guard lock(readiness_report_mutex_);
    if (detail == last_projection_readiness_detail_) {
      return;
    }
    last_projection_readiness_detail_ = std::move(detail);
    std::cerr << "native projection readiness: "
              << last_projection_readiness_detail_ << '\n';
  }

  [[nodiscard]] static ManagedProjectionKey
  Key(const core::ProjectionNativeRuntimeBindingDTO &binding) {
    return {binding.provider_binding_id, binding.provider_binding_revision};
  }

  void RequireLocalBinding(
      const core::ProjectionNativeRuntimeBindingDTO &binding) const {
    Require(binding.valid() &&
                binding.runtime_unit_id.value() == config_.runtime_unit_id &&
                binding.runtime_epoch.value() == config_.runtime_epoch,
            "native projection request names another runtime child");
  }

  /// Resolves the exact projection for one complete local binding. The whole
  /// binding, runtime unit and epoch included, is validated here, before any
  /// caller reaches a pending-map fast path, and the projection's own mutex is
  /// never acquired while the registry mutex is held.
  [[nodiscard]] std::shared_ptr<RetainedPlannerSceneProjection>
  Find(const core::ProjectionNativeRuntimeBindingDTO &binding) const {
    RequireLocalBinding(binding);
    std::lock_guard lock(mutex_);
    const auto found = projections_.find(Key(binding));
    if (found == projections_.end() || found->second.projection == nullptr ||
        found->second.binding != binding) {
      return nullptr;
    }
    return found->second.projection;
  }

  [[nodiscard]] std::vector<RegisteredProjection> OwnedProjections(
      const core::ProjectionNativeRuntimeBindingDTO &binding) const {
    RequireLocalBinding(binding);
    std::lock_guard lock(mutex_);
    std::vector<RegisteredProjection> result;
    for (const auto *slots : {&projections_, &retired_projections_}) {
      const auto found = slots->find(Key(binding));
      if (found != slots->end() && found->second.binding == binding) {
        result.push_back(found->second);
      }
    }
    return result;
  }

  [[nodiscard]] std::shared_ptr<RetainedPlannerSceneProjection> FindRevision(
      const core::ProjectionNativeRuntimeBindingDTO &binding,
      const core::ProjectionRevisionDTO &revision) const {
    for (const auto &entry : OwnedProjections(binding)) {
      if (entry.identity.retained_bundle_id == revision.retained_bundle_id &&
          entry.identity.graph_revision == revision.graph_revision) {
        return entry.projection;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::shared_ptr<RetainedPlannerSceneProjection> RevisionHandle(
      const core::ProjectionNativeRuntimeBindingDTO &binding,
      const core::ProjectionRevisionDTO &revision) const {
    auto projection = FindRevision(binding, revision);
    Require(projection != nullptr,
            "native projection request names an unprepared retained model");
    return projection;
  }

  /// Release has no revision selector. Resolve its actual retained lease owner,
  /// without a second lease database or interpreting an ID as the latest model.
  [[nodiscard]] std::shared_ptr<RetainedPlannerSceneProjection> LeaseOwner(
      const core::ProjectionNativeRuntimeBindingDTO &binding,
      const core::ProjectionLeaseId &lease_id) const {
    std::shared_ptr<RetainedPlannerSceneProjection> owner;
    for (const auto &entry : OwnedProjections(binding)) {
      std::optional<std::vector<core::ProjectionLeaseId>> pending_leases;
      {
        std::lock_guard lock(mutex_);
        const auto job = pending_rebuilds_.find(Key(binding));
        if (job != pending_rebuilds_.end() &&
            job->second.projection == entry.projection &&
            !Settled(job->second.result)) {
          pending_leases = job->second.retained_lease_ids;
        }
      }
      const auto ids = pending_leases.has_value()
                           ? std::move(*pending_leases)
                           : entry.projection->scene_snapshot_lease_retention_stats()
                                 .retained_ids_oldest_first;
      if (std::find(ids.begin(), ids.end(), lease_id) != ids.end()) {
        Require(owner == nullptr,
                "native projection lease has ambiguous model ownership");
        owner = entry.projection;
      }
    }
    return owner;
  }

  [[nodiscard]] std::shared_ptr<RetainedPlannerSceneProjection> PolicyHandle(
      const core::ProjectionNativeRuntimeBindingDTO &binding,
      const core::LogicalSceneSnapshotDTO &scene) const {
    std::shared_ptr<RetainedPlannerSceneProjection> owner;
    for (const auto &entry : OwnedProjections(binding)) {
      if (WorkPendingFor(Key(binding), entry.projection)) {
        continue;
      }
      const auto snapshot = entry.projection->logical_snapshot();
      if (snapshot != nullptr && *snapshot == scene) {
        Require(owner == nullptr,
                "native collision policy has ambiguous Scene ownership");
        owner = entry.projection;
      }
    }
    // Preserve the projection's existing typed refusal for an unknown Scene.
    return owner != nullptr ? owner : Handle(binding);
  }

  [[nodiscard]] std::shared_ptr<RetainedPlannerSceneProjection>
  Handle(const core::ProjectionNativeRuntimeBindingDTO &binding) const {
    auto projection = Find(binding);
    Require(projection != nullptr,
            "native projection request names an unprepared binding");
    return projection;
  }

  [[nodiscard]] std::shared_ptr<RetainedPlannerSceneProjection> IdleHandle(
      const core::ProjectionNativeRuntimeBindingDTO &binding,
      const std::string_view operation,
      std::shared_ptr<RetainedPlannerSceneProjection> projection = nullptr) const {
    // Resolve the complete binding first. Only then may a pending job reject a
    // direct operation; a foreign epoch must never learn or steer local work.
    if (projection == nullptr) {
      projection = Handle(binding);
    }
    std::lock_guard lock(mutex_);
    Require(!WorkPendingForLocked(Key(binding), projection),
            "native projection " + std::string(operation) +
                " conflicts with progressing projection work");
    return projection;
  }

  [[nodiscard]] static core::ErrorDTO PendingOperationError(
      const std::string_view operation,
      const core::ProjectionNativeRuntimeBindingDTO &binding) {
    return core::ErrorDTO{
        "projection_state_uncertain",
        "native projection " + std::string(operation) +
            " cannot settle while projection construction is progressing",
        {{"reason", "native_projection_" + std::string(operation) +
                        "_progressing"},
         {"operation", std::string(operation)},
         {"provider_binding_id", binding.provider_binding_id.value()}}};
  }

  [[nodiscard]] core::ProjectionCommitResultDTO ApplyTransaction(
      const component_manager::detail::NativeProjectionTransactionRequest
          &request) {
    Require(request.valid(), "native projection transaction is invalid");
    auto projection = RevisionHandle(
        request.native_runtime_binding,
        request.transaction.prior_projection_revision);
    std::optional<core::ProjectionHealthDTO> pending;
    {
      std::lock_guard lock(mutex_);
      pending = CapturedJobHealthLocked(Key(request.native_runtime_binding),
                                       projection);
    }
    if (!pending.has_value()) {
      return projection->ApplyTransaction(request.transaction);
    }
    const auto failure =
        PendingOperationError("transaction", request.native_runtime_binding);
    auto uncertain = UncertainHealth(*pending, failure);
    core::ProjectionCommitResultDTO result{
        request.transaction.transaction_sequence,
        request.transaction.proposed_projection_revision,
        core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain,
        std::nullopt,
        uncertain.native_projection_generation,
        false,
        std::move(uncertain),
        failure,
    };
    Require(result.presence_valid(),
            "progressing native projection transaction result is invalid");
    return result;
  }

  /// Freshness-only publication on the exact projection revision. It never
  /// waits for or overtakes progressing projection work: a pending rebuild or
  /// bootstrap on that projection refuses the confirmation, which the owner
  /// then does not record.
  [[nodiscard]] component_manager::detail::NativeProjectionRpcResponse
  ApplyProviderFreshness(
      const component_manager::detail::NativeProjectionProviderFreshnessRequest
          &request) {
    Require(request.valid(),
            "native projection provider freshness request is invalid");
    const auto &confirmation = request.confirmation;
    auto projection =
        RevisionHandle(request.native_runtime_binding,
                       confirmation.expected_projection_revision);
    if (WorkPendingFor(Key(request.native_runtime_binding), projection)) {
      return component_manager::detail::NativeProjectionRpcFailure{
          PendingOperationError("provider_freshness",
                                request.native_runtime_binding)};
    }
    auto applied = projection->ApplyProviderFreshness(confirmation);
    if (!applied.ok()) {
      return component_manager::detail::NativeProjectionRpcFailure{
          applied.error()};
    }
    return component_manager::detail::NativeProjectionProviderFreshnessResult{
        confirmation.expected_projection_revision,
        confirmation.scene_resource_id,
        confirmation.evidence.provider_source_sequence};
  }

  [[nodiscard]] core::NativeProjectionBootstrapRebuildResultDTO
  BootstrapRebuild(const core::NativeProjectionBootstrapRebuildDTO &request) {
    Require(request.valid(), "native projection bootstrap rebuild is invalid");
    auto projection = RevisionHandle(request.native_runtime_binding,
                                     request.canonical_empty_target);
    std::optional<core::ProjectionHealthDTO> pending;
    {
      std::lock_guard lock(mutex_);
      pending = CapturedJobHealthLocked(Key(request.native_runtime_binding),
                                       projection);
    }
    if (!pending.has_value()) {
      return projection->RebuildBootstrapProjection(request);
    }
    const auto failure = PendingOperationError(
        "bootstrap_rebuild", request.native_runtime_binding);
    core::NativeProjectionBootstrapRebuildResultDTO result{
        request.bootstrap_id,
        request.bootstrap_generation,
        request.native_runtime_binding,
        request.canonical_empty_target,
        request.planning_collision_model,
        request.initialized_logical_scene,
        core::NativeProjectionBootstrapRebuildState::
            kFailedProjectionUncertain,
        std::nullopt,
        pending->native_projection_generation,
        pending->projection_content_digest,
        *pending,
        failure,
    };
    Require(result.presence_valid(),
            "progressing native bootstrap rebuild result is invalid");
    return result;
  }

  [[nodiscard]] core::NativeProjectionBootstrapStatusDTO BootstrapStatus(
      const core::NativeProjectionBootstrapStatusQueryDTO &query) {
    Require(query.valid(), "native projection bootstrap status is invalid");
    auto projection = RevisionHandle(query.native_runtime_binding,
                                     query.canonical_empty_target);
    std::optional<core::ProjectionHealthDTO> pending;
    {
      std::lock_guard lock(mutex_);
      pending = CapturedJobHealthLocked(Key(query.native_runtime_binding),
                                       projection);
    }
    if (!pending.has_value()) {
      return projection->BootstrapProjectionStatus(query);
    }
    const auto failure =
        PendingOperationError("bootstrap_status", query.native_runtime_binding);
    core::NativeProjectionBootstrapStatusDTO result{
        query.bootstrap_id,
        query.bootstrap_generation,
        query.native_runtime_binding,
        query.canonical_empty_target,
        core::NativeProjectionBootstrapStatusState::
            kFailedProjectionUncertain,
        std::nullopt,
        pending->native_projection_generation,
        pending->projection_content_digest,
        *pending,
        failure,
    };
    Require(result.presence_valid(),
            "progressing native bootstrap status result is invalid");
    return result;
  }

  /// Accepts one exact recovery Rebuild for one provider.
  [[nodiscard]] core::ProjectionCommitResultDTO
  BeginRebuild(const core::NativeProjectionRebuildDTO &request) {
    Require(request.valid(), "native projection rebuild request is invalid");
    const auto &binding = request.expected_native_runtime_binding;
    auto projection = RevisionHandle(binding, request.target_projection_revision);
    const auto key = Key(binding);
    std::optional<PendingRebuild> superseded_rebuild;
    {
      std::lock_guard lock(mutex_);
      // Exact replay observes its already-retained Rebuild.
      if (const auto pending = pending_rebuilds_.find(key);
          pending != pending_rebuilds_.end() &&
          pending->second.request == request) {
        if (!Settled(pending->second.result)) {
          return AcceptedRebuild(request, pending->second.accepted_health);
        }
        return RebuildOutcome(pending->second);
      }
      if (const auto pending = pending_rebuilds_.find(key);
          pending != pending_rebuilds_.end()) {
        if (!Settled(pending->second.result)) {
          Fail("another native projection rebuild is already progressing");
        }
        // A recovery for a different target supersedes the retained record so
        // the retry can run; the projection still refuses an unnecessary or
        // mismatched rebuild with its own typed result.
        superseded_rebuild = TakeSettledRebuildLocked(key);
      }
    }

    auto accepted_health = projection->ProjectionHealth();
    if (accepted_health.state == core::ProjectionHealthState::kHealthy) {
      // An exact healthy target is an idempotent terminal replay; a different
      // healthy target is a proven recovery conflict. Neither needs a worker.
      return projection->RebuildProjection(request);
    }
    accepted_health.state = core::ProjectionHealthState::kRebuilding;
    accepted_health.active_projection_revision = std::nullopt;
    accepted_health.error = std::nullopt;
    if (accepted_health.source_sequence !=
        std::numeric_limits<std::uint64_t>::max()) {
      ++accepted_health.source_sequence;
    }
    const auto accepted = AcceptedRebuild(request, accepted_health);
    auto retained_lease_ids = projection->scene_snapshot_lease_retention_stats()
                                  .retained_ids_oldest_first;
    {
      std::lock_guard lock(mutex_);
      StartRebuildLocked(key, request, accepted.resulting_health, projection,
                         std::move(retained_lease_ids));
    }
    return accepted;
  }

  /// Registers one Rebuild job and starts its single worker. Callers hold
  /// mutex_.
  void StartRebuildLocked(
      const ManagedProjectionKey &key,
      const core::NativeProjectionRebuildDTO &request,
      const core::ProjectionHealthDTO &accepted_health,
      const std::shared_ptr<RetainedPlannerSceneProjection> &projection,
      std::vector<core::ProjectionLeaseId> retained_lease_ids) {
    auto job = std::make_shared<
        std::promise<core::ProjectionCommitResultDTO>>();
    auto result = job->get_future().share();
    const auto binding = request.expected_native_runtime_binding;
    auto progress =
        std::make_shared<JobProgress>("rebuild", binding, "input_ready",
                                      clock_);
    const auto [inserted, unique] = pending_rebuilds_.emplace(
        key, PendingRebuild{request, accepted_health, result, progress, projection,
                            std::move(retained_lease_ids),
                            JoinedWorker{}});
    Require(unique, "native projection rebuild registration conflicted");
    try {
      inserted->second.worker = JoinedWorker{std::thread(
          [job, projection, request, binding,
           progress = std::move(progress)]() {
            try {
              progress->Advance("collision_world_construction");
              auto outcome = projection->RebuildProjection(request);
              progress->Advance(
                  outcome.result ==
                          core::ProjectionCommitResultStateDTO::kCommitted
                      ? "prepared_publication"
                      : "terminal_result");
              job->set_value(std::move(outcome));
            } catch (const std::exception &error) {
              progress->Advance("worker_failed");
              job->set_value(UncertainRebuild(
                  request, ObservedHealth(projection, binding),
                  WorkerFailure("rebuild", binding,
                                "native_projection_rebuild_worker_failed",
                                error.what())));
            } catch (...) {
              progress->Advance("worker_failed");
              job->set_value(UncertainRebuild(
                  request, ObservedHealth(projection, binding),
                  WorkerFailure("rebuild", binding,
                                "native_projection_rebuild_worker_failed",
                                "unknown Rebuild worker failure")));
            }
          })};
    } catch (...) {
      pending_rebuilds_.erase(key);
      throw;
    }
  }

  [[nodiscard]] static core::ProjectionHealthDTO ObservedHealth(
      const std::shared_ptr<RetainedPlannerSceneProjection> &projection,
      const core::ProjectionNativeRuntimeBindingDTO &binding) {
    try {
      return projection->ProjectionHealth();
    } catch (...) {
      return core::ProjectionHealthDTO{binding, 1U,
                                       core::ProjectionHealthState::
                                           kUnavailable,
                                       std::nullopt, std::nullopt, std::nullopt,
                                       std::nullopt};
    }
  }

  [[nodiscard]] static core::ProjectionCommitResultDTO AcceptedRebuild(
      const core::NativeProjectionRebuildDTO &request,
      const core::ProjectionHealthDTO &health) {
    core::ProjectionCommitResultDTO accepted{
        std::nullopt,
        request.target_projection_revision,
        core::ProjectionCommitResultStateDTO::kAccepted,
        std::nullopt,
        health.native_projection_generation,
        false,
        health,
        std::nullopt,
    };
    Require(accepted.presence_valid(),
            "accepted native projection rebuild result is invalid");
    return accepted;
  }

  /// Observes current projection health.
  ///
  /// While a job runs, the answer is the predecessor health captured at
  /// acceptance plus the truthful statement that the job is unsettled; the
  /// captured row never proves candidate readiness. A retained job whose
  /// terminal result reported an uncertain effect keeps reporting that result
  /// instead of reverting to the predecessor row after one read.
  [[nodiscard]] core::ProjectionHealthDTO ProjectionHealth(
      const core::ProjectionNativeRuntimeBindingDTO &binding) {
    auto projection = Find(binding);
    const auto key = Key(binding);
    std::optional<core::ProjectionHealthDTO> captured;
    std::optional<core::ProjectionHealthDTO> terminal;
    {
      std::lock_guard lock(mutex_);
      Require(projection != nullptr,
              "native projection request names an unprepared binding");
      if (const auto pending = CapturedJobHealthLocked(key, projection);
          pending.has_value()) {
        captured = pending;
      }
      if (!captured.has_value() && !terminal.has_value()) {
        if (const auto rebuild = pending_rebuilds_.find(key);
            rebuild != pending_rebuilds_.end() &&
            rebuild->second.projection == projection) {
          const auto outcome = RebuildOutcome(rebuild->second);
          if (outcome.result == core::ProjectionCommitResultStateDTO::
                                    kFailedProjectionUncertain) {
            terminal = outcome.resulting_health;
          }
        }
      }
    }
    if (captured.has_value()) {
      return *captured;
    }
    if (terminal.has_value()) {
      return *terminal;
    }
    return projection->ProjectionHealth();
  }

  [[nodiscard]] const core::CapabilityProviderBindingDTO &
  RequireProviderBinding(
      const deployment::RetainedBundle &bundle,
      const core::ProjectionNativeRuntimeBindingDTO &runtime_binding) const {
    const auto &bindings = bundle.value().capability_provider_bindings;
    const auto found = std::find_if(
        bindings.begin(), bindings.end(), [&](const auto &candidate) {
          return candidate.capability_provider_binding_id ==
                     runtime_binding.provider_binding_id &&
                 candidate.capability_provider_binding_revision ==
                     runtime_binding.provider_binding_revision;
        });
    Require(
        found != bindings.end() &&
            core::RequiresNativeSceneProjection(
                found->capability_id, found->capability_definition_revision),
        "prepared provider is absent or not a native projection "
        "participant");
    std::set<core::RuntimeUnitId> runtime_sources;
    for (const auto &endpoint : found->endpoint_bindings) {
      if (endpoint.source_kind !=
          core::CapabilityEndpointSourceKind::kRuntimeUnit) {
        continue;
      }
      const auto *runtime_unit =
          std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
      Require(runtime_unit != nullptr && !runtime_unit->empty(),
              "prepared provider has a mismatched runtime endpoint");
      runtime_sources.insert(*runtime_unit);
    }
    Require(runtime_sources.size() == 1U &&
                runtime_sources.begin()->value() == config_.runtime_unit_id,
            "prepared provider is assigned to another runtime unit");
    return *found;
  }

  [[nodiscard]] static std::string
  SelectProjectionGroup(const core::CapabilityProviderBindingDTO &provider,
                        const deployment::VerifiedRetainedModelMap &model_map) {
    std::vector<std::string> groups;
    if (const auto *target =
            std::get_if<core::TargetBindingCapabilityProviderTargetDTO>(
                &provider.target)) {
      const auto *resolved = model_map.find_target(
          target->controllable_component_id, target->command_target_id);
      Require(resolved != nullptr,
              "native provider target is absent from its retained model map");
      for (const auto &group : resolved->model_groups) {
        groups.push_back(group.compiled_group_name);
      }
    } else {
      for (const auto &target : model_map.targets()) {
        for (const auto &group : target.model_groups) {
          groups.push_back(group.compiled_group_name);
        }
      }
    }
    groups.erase(std::remove(groups.begin(), groups.end(), std::string{}),
                 groups.end());
    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
    Require(!groups.empty(),
            "retained native projection has no compiled planning group");
    return groups.front();
  }

  [[nodiscard]] RetainedCollisionWorldProjector BuildProjectionAssets(
      const component_manager::detail::NativeProjectionPrepareRequest
          &request) {
    // Shared preparation runs once per candidate and group; the provider's own
    // binding checks run on every request, cache hit or not.
    return candidate_inputs_.Acquire(
        ManagedProjectionCandidate{request.candidate_retained_bundle_id,
                                   request.candidate_graph_revision,
                                   request.planning_collision_model},
        [&] {
          return assets_->VerifyCandidate(ManagedProjectionCandidate{
              request.candidate_retained_bundle_id,
              request.candidate_graph_revision,
              request.planning_collision_model});
        },
        [&](const ManagedProjectionCandidateInputs &inputs) {
          return SelectProjectionGroup(
              RequireProviderBinding(*inputs.bundle,
                                     request.native_runtime_binding),
              inputs.model_map);
        },
        [&](const ManagedProjectionCandidateInputs &inputs,
            const std::string &group) {
          return assets_->ConstructGroupProjector(inputs, group);
        });
  }

  /// Prepares one exact candidate model for a provider.
  ///
  /// Artifact verification and shared-model construction are the expensive
  /// part of preparation and deliberately run without the registry mutex.
  /// The predecessor keeps its immutable model and leases; replacing a model
  /// is candidate staging, not recovery of an already healthy projection.
  void Prepare(const component_manager::detail::NativeProjectionPrepareRequest
                   &request) {
    Require(request.valid(), "native projection preparation is invalid");
    RequireLocalBinding(request.native_runtime_binding);
    const auto key = Key(request.native_runtime_binding);
    ReclaimRetiredProjections();
    std::shared_ptr<RetainedPlannerSceneProjection> predecessor;
    std::uint64_t incarnation{};
    {
      std::lock_guard lock(mutex_);
      if (IsPreparedLocked(key, request)) {
        // Exact model preparation replay is not a new projection effect and
        // does not acknowledge a progressing rebuild as ready.
        prepared_product_ = std::pair{request.candidate_retained_bundle_id,
                                     request.candidate_graph_revision};
        return;
      }
      Require(!WorkPendingLocked(key),
              "native projection preparation conflicts with progressing "
              "projection work");
      Require(!retired_projections_.contains(key),
              "native projection predecessor is still retained; model "
              "preparation requires its leases and composition to settle");
      if (const auto prior = projections_.find(key); prior != projections_.end()) {
        predecessor = prior->second.projection;
      }
      Require(projection_incarnation_ !=
                  std::numeric_limits<std::uint64_t>::max(),
              "native projection construction incarnation is exhausted");
      incarnation = ++projection_incarnation_;
    }
    auto created = RetainedPlannerSceneProjection::CreateUninitialized(
        BuildProjectionAssets(request), request.planning_collision_model,
        request.native_runtime_binding,
        RetainedPlannerSceneProjectionConfig{
            "managed-native-projection-" +
                request.native_runtime_binding.provider_binding_id.value() +
                ".epoch." + config_.runtime_epoch + ".model." +
                std::to_string(incarnation),
            256U, RetainedCollisionWorldPublisher::kDefaultLedgerCapacity},
        clock_);
    if (!created.ok()) {
      Fail("native projection construction failed: " +
           created.error().message);
    }
    // Construction failure leaves the predecessor untouched. Recheck the
    // slot before installing the private candidate; a stale preparer cannot
    // overwrite a newer preparation or an accepted rebuild.
    std::optional<PendingRebuild> superseded;
    {
      std::lock_guard lock(mutex_);
      if (IsPreparedLocked(key, request)) {
        prepared_product_ = std::pair{request.candidate_retained_bundle_id,
                                     request.candidate_graph_revision};
        return;
      }
      const auto prior = projections_.find(key);
      Require(!WorkPendingLocked(key) &&
                  !retired_projections_.contains(key) &&
                  ((prior == projections_.end() && predecessor == nullptr) ||
                   (prior != projections_.end() &&
                    prior->second.projection == predecessor)),
              "native projection candidate preparation was superseded");
      // Stable terminal results belong to their exact projection incarnation.
      // Do not let a later model inherit a prior rebuild's replay record.
      superseded = TakeSettledRebuildLocked(key);
      if (prior != projections_.end()) {
        retired_projections_.emplace(key, prior->second);
      }
      projections_.insert_or_assign(
          key, RegisteredProjection{
                   request.native_runtime_binding, std::move(created.value()),
                   PreparedIdentity{request.candidate_retained_bundle_id,
                                    request.candidate_graph_revision,
                                    request.planning_collision_model}});
      prepared_product_ = std::pair{request.candidate_retained_bundle_id,
                                   request.candidate_graph_revision};
    }
  }

  [[nodiscard]] bool IsPreparedLocked(
      const ManagedProjectionKey &key,
      const component_manager::detail::NativeProjectionPrepareRequest
          &request) const {
    const auto prior = projections_.find(key);
    return prior != projections_.end() &&
           prior->second.identity.retained_bundle_id ==
               request.candidate_retained_bundle_id &&
           prior->second.identity.graph_revision ==
               request.candidate_graph_revision &&
           prior->second.identity.planning_collision_model ==
               request.planning_collision_model;
  }

  const ManagedProjectionRegistryConfig config_;
  const std::unique_ptr<const IManagedProjectionAssetSource> assets_;
  const RetainedPlannerSceneProjection::Clock clock_;
  std::map<ManagedProjectionKey, RegisteredProjection> projections_;
  std::map<ManagedProjectionKey, RegisteredProjection> retired_projections_;
  std::uint64_t projection_incarnation_{};
  std::optional<std::pair<core::RetainedBundleId, core::GraphRevision>>
      prepared_product_;
  std::optional<std::pair<core::RetainedBundleId, core::GraphRevision>>
      installed_product_;
  std::set<ManagedProjectionKey> installed_provider_keys_;
  // Retained one exact job per provider. A settled record is kept only as the
  // single stable terminal observation for that operation and is released when
  // a later submission or a confirmed discard/retirement supersedes it; it is
  // never a second history database.
  std::map<ManagedProjectionKey, PendingRebuild> pending_rebuilds_;
  mutable std::string last_projection_readiness_detail_;
  mutable std::mutex readiness_report_mutex_;
  mutable std::mutex mutex_;
  // Independent of `mutex_`: candidate verification and model construction
  // never block registry reads. Never held together with `mutex_`.
  ManagedCandidateProjectionCache<ManagedProjectionCandidateInputs,
                                  RetainedCollisionWorldProjector>
      candidate_inputs_;
};


ManagedProjectionRegistry::ManagedProjectionRegistry(
    ManagedProjectionRegistryConfig config,
    std::unique_ptr<const IManagedProjectionAssetSource> assets,
    RetainedPlannerSceneProjection::Clock monotonic_now_ns)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(assets),
                                   std::move(monotonic_now_ns))) {}

ManagedProjectionRegistry::~ManagedProjectionRegistry() = default;

component_manager::detail::NativeProjectionRpcResponse
ManagedProjectionRegistry::Dispatch(
    const component_manager::detail::NativeProjectionRpcRequest &request) {
  return impl_->Dispatch(request);
}

void ManagedProjectionRegistry::ConfirmInstalledProjectionSet(
    const deployment::RetainedBundle &bundle) {
  impl_->ConfirmInstalledProjectionSet(bundle);
}

void ManagedProjectionRegistry::ReclaimRetiredProjections() {
  impl_->ReclaimRetiredProjections();
}

void ManagedProjectionRegistry::Drain() { impl_->Drain(); }

std::shared_ptr<RetainedPlannerSceneProjection>
ManagedProjectionRegistry::FindActiveProjection(
    const core::ProjectionRevisionDTO &expected,
    const core::ProjectionNativeRuntimeBindingDTO &binding) const {
  return impl_->FindActiveProjection(expected, binding);
}

std::optional<core::ProjectionRevisionDTO>
ManagedProjectionRegistry::FindCurrentProjectionRevision(
    const core::ProjectionRevisionDTO &expected,
    const core::ProjectionNativeRuntimeBindingDTO &binding) const {
  return impl_->FindCurrentProjectionRevision(expected, binding);
}

bool ManagedProjectionRegistry::AllRequiredProjectionsHealthy(
    const deployment::RetainedBundle &bundle,
    const core::ProjectionRevisionDTO &expected) const {
  return impl_->AllRequiredProjectionsHealthy(bundle, expected);
}

void ManagedProjectionRegistry::InstallActiveFrameResolverSet(
    const deployment::RetainedBundle &bundle,
    const core::ProjectionRevisionDTO &expected,
    std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver) {
  impl_->InstallActiveFrameResolverSet(bundle, expected,
                                       std::move(frame_resolver));
}

} // namespace manyforge::roboplan_backend
