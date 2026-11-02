#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {

struct CollisionPipelineResources {
  ICollisionWorldSnapshotPublisher* snapshot_publisher = nullptr;
  /// Present for retained resource-set replacement. Initial bootstrap callers
  /// may keep publisher ownership in their composition root.
  std::shared_ptr<ICollisionWorldSnapshotPublisher> snapshot_publisher_owner;
  std::shared_ptr<ICollisionQueryEngine> runtime_monitor_engine;
  std::vector<std::shared_ptr<ICollisionQueryEngine>> validation_engines;
  std::vector<std::shared_ptr<ICollisionQueryEngine>> runtime_lookahead_engines;
};

/// Owned candidate resources for a retained Kernel replacement. Every
/// resource is shared so the Kernel and its readback keep the exact installed
/// objects alive.
struct CollisionPipelineResourceOwners final {
  std::shared_ptr<ICollisionWorldSnapshotPublisher> snapshot_publisher;
  std::shared_ptr<ICollisionQueryEngine> runtime_monitor_engine;
  std::vector<std::shared_ptr<ICollisionQueryEngine>> validation_engines;
  std::vector<std::shared_ptr<ICollisionQueryEngine>> runtime_lookahead_engines;

  friend bool operator==(const CollisionPipelineResourceOwners&,
                         const CollisionPipelineResourceOwners&) = default;
};

struct CollisionPipelineResourceReadbackDTO final {
  std::uint64_t generation{};
  CollisionPipelineResourceOwners resources;
  /// Exact immutable fallback policy installed with the resource set. It is
  /// captured from the same published runtime bundle as its world and index.
  std::shared_ptr<const CollisionPolicySnapshot> coordinator_policy_snapshot;
  std::shared_ptr<const CollisionPolicyIndex> coordinator_policy_index;

  friend bool operator==(const CollisionPipelineResourceReadbackDTO&,
                         const CollisionPipelineResourceReadbackDTO&) = default;
};

enum class CollisionValidationAdmissionStatus {
  kAdmitted,
  kMissingSnapshotPublisher,
  kMissingRuntimeResource,
  kMissingValidationResource,
  kRuntimeQueryCapabilityUnsupported,
  kRuntimeMonitorLeaseActiveSharedResource,
  kRuntimeLookaheadResourceConflict,
};

struct CollisionValidationAdmission {
  CollisionValidationAdmissionStatus status =
      CollisionValidationAdmissionStatus::kAdmitted;
  std::string resource_conflict_reason;

  [[nodiscard]] bool admitted() const {
    return status == CollisionValidationAdmissionStatus::kAdmitted;
  }
};

struct CollisionValidationWorkerCapacity {
  std::size_t effective_contexts = 0;
  std::optional<std::string> mismatch_reason;
};

/// Role-aware collision resource registry. Runtime monitoring and validation
/// are separate roles even when a legacy/test caller wires the same engine into
/// both; validation admission can then fail closed instead of borrowing the
/// runtime monitor's active resource.
class CollisionResourceManager {
 public:
  /// Inspect how many validation contexts can be proven independent. A
  /// single-context engine without an identity can still serve the serial
  /// fallback, but it cannot substantiate a declared parallel capacity.
  [[nodiscard]] static CollisionValidationWorkerCapacity inspect_validation_worker_capacity(
      const std::vector<std::shared_ptr<ICollisionQueryEngine>>& engines) {
    CollisionValidationWorkerCapacity result;
    std::vector<const ICollisionQueryEngine*> admitted_instances;
    std::vector<std::string> admitted_single_context_ids;
    bool has_usable_engine = false;

    for (const auto& engine : engines) {
      if (!engine) {
        continue;
      }
      has_usable_engine = true;
      if (std::find(admitted_instances.begin(), admitted_instances.end(), engine.get()) !=
          admitted_instances.end()) {
        if (!result.mismatch_reason) {
          result.mismatch_reason = "duplicate_engine_instance";
        }
        continue;
      }
      admitted_instances.push_back(engine.get());

      const auto& capability = engine->capability();
      if (capability.thread_safety == CollisionEngineThreadSafetyClass::kSingleContextExclusive) {
        if (capability.mutable_context_id.empty()) {
          if (!result.mismatch_reason) {
            result.mismatch_reason = "missing_mutable_context_id";
          }
          continue;
        }
        if (std::find(admitted_single_context_ids.begin(), admitted_single_context_ids.end(),
                      capability.mutable_context_id) != admitted_single_context_ids.end()) {
          if (!result.mismatch_reason) {
            result.mismatch_reason = "duplicate_mutable_context_id";
          }
          continue;
        }
        admitted_single_context_ids.push_back(capability.mutable_context_id);
      }
      ++result.effective_contexts;
    }

    if (result.mismatch_reason && has_usable_engine) {
      // This is the capacity the pool can safely use after its named serial
      // fallback. It must not be reported as the requested parallel capacity.
      result.effective_contexts = 1;
    }
    return result;
  }

  void configure(
      ICollisionWorldSnapshotPublisher& publisher,
      std::shared_ptr<ICollisionQueryEngine> runtime_monitor_engine,
      std::vector<std::shared_ptr<ICollisionQueryEngine>> validation_engines,
      std::vector<std::shared_ptr<ICollisionQueryEngine>> runtime_lookahead_engines = {}) {
    CollisionPipelineResources next;
    next.snapshot_publisher = &publisher;
    next.runtime_monitor_engine = std::move(runtime_monitor_engine);
    next.validation_engines.reserve(validation_engines.size());
    for (auto& engine : validation_engines) {
      if (engine) {
        next.validation_engines.push_back(std::move(engine));
      }
    }
    next.runtime_lookahead_engines.reserve(runtime_lookahead_engines.size());
    for (auto& engine : runtime_lookahead_engines) {
      if (engine) {
        next.runtime_lookahead_engines.push_back(std::move(engine));
      }
    }
    configure(std::move(next));
  }

  void configure(CollisionPipelineResourceOwners owned_resources) {
    CollisionPipelineResources next;
    next.snapshot_publisher = owned_resources.snapshot_publisher.get();
    next.snapshot_publisher_owner =
        std::move(owned_resources.snapshot_publisher);
    next.runtime_monitor_engine = std::move(owned_resources.runtime_monitor_engine);
    next.validation_engines = std::move(owned_resources.validation_engines);
    next.validation_engines.erase(
        std::remove(next.validation_engines.begin(), next.validation_engines.end(), nullptr),
        next.validation_engines.end());
    next.runtime_lookahead_engines =
        std::move(owned_resources.runtime_lookahead_engines);
    next.runtime_lookahead_engines.erase(
        std::remove(next.runtime_lookahead_engines.begin(),
                    next.runtime_lookahead_engines.end(), nullptr),
        next.runtime_lookahead_engines.end());
    configure(std::move(next));
  }

  void configure(CollisionPipelineResources resources) {
    resources.validation_engines.erase(
        std::remove(resources.validation_engines.begin(),
                    resources.validation_engines.end(), nullptr),
        resources.validation_engines.end());
    resources.runtime_lookahead_engines.erase(
        std::remove(resources.runtime_lookahead_engines.begin(),
                    resources.runtime_lookahead_engines.end(), nullptr),
        resources.runtime_lookahead_engines.end());
    std::lock_guard<std::mutex> lock(mutex_);
    resources_ = std::move(resources);
  }

  [[nodiscard]] CollisionPipelineResources snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return resources_;
  }

  [[nodiscard]] static bool runtime_monitor_may_own_resource(
      ExecutionState state) {
    return state == ExecutionState::kExecuting ||
           state == ExecutionState::kStopping;
  }

  [[nodiscard]] static bool validation_pool_shares_runtime_resource(
      const CollisionPipelineResources& resources,
      std::string* reason = nullptr) {
    if (!resources.runtime_monitor_engine) {
      return false;
    }
    const auto* runtime = resources.runtime_monitor_engine.get();
    const auto& runtime_cap = runtime->capability();
    for (const auto& engine : resources.validation_engines) {
      if (!engine) {
        continue;
      }
      if (engine.get() == runtime) {
        if (reason) {
          *reason = "same_engine_instance";
        }
        return true;
      }
      const auto& validation_cap = engine->capability();
      if (runtime_cap.thread_safety ==
              CollisionEngineThreadSafetyClass::kSingleContextExclusive &&
          validation_cap.thread_safety ==
              CollisionEngineThreadSafetyClass::kSingleContextExclusive) {
        if (runtime_cap.mutable_context_id.empty() ||
            validation_cap.mutable_context_id.empty()) {
          if (reason) {
            *reason = "missing_mutable_context_id";
          }
          return true;
        }
        if (runtime_cap.mutable_context_id ==
            validation_cap.mutable_context_id) {
          if (reason) {
            *reason = "shared_mutable_context_id";
          }
          return true;
        }
      }
    }
    return false;
  }

  /// Return a structured reason when lookahead aliases either the reserved
  /// immediate monitor, authoritative validation, or another lookahead
  /// context. Distinct wrapper objects are not sufficient: mutable context
  /// identity is the resource authority for single-context engines.
  [[nodiscard]] static std::optional<std::string> runtime_lookahead_resource_conflict(
      const CollisionPipelineResources& resources) {
    const auto conflict =
        [](const std::shared_ptr<ICollisionQueryEngine>& lhs,
           const std::shared_ptr<ICollisionQueryEngine>& rhs) -> std::optional<std::string> {
      if (!lhs || !rhs) {
        return std::nullopt;
      }
      if (lhs.get() == rhs.get()) {
        return "same_engine_instance";
      }
      const auto& lhs_cap = lhs->capability();
      const auto& rhs_cap = rhs->capability();
      if (lhs_cap.thread_safety != CollisionEngineThreadSafetyClass::kSingleContextExclusive ||
          rhs_cap.thread_safety != CollisionEngineThreadSafetyClass::kSingleContextExclusive) {
        return std::nullopt;
      }
      if (lhs_cap.mutable_context_id.empty() || rhs_cap.mutable_context_id.empty()) {
        return "missing_mutable_context_id";
      }
      if (lhs_cap.mutable_context_id == rhs_cap.mutable_context_id) {
        return "shared_mutable_context_id";
      }
      return std::nullopt;
    };

    for (std::size_t index = 0; index < resources.runtime_lookahead_engines.size(); ++index) {
      const auto& lookahead = resources.runtime_lookahead_engines[index];
      if (const auto reason = conflict(lookahead, resources.runtime_monitor_engine)) {
        return "lookahead_runtime_" + *reason;
      }
      for (const auto& validation : resources.validation_engines) {
        if (const auto reason = conflict(lookahead, validation)) {
          return "lookahead_validation_" + *reason;
        }
      }
      for (std::size_t prior = 0; prior < index; ++prior) {
        if (const auto reason = conflict(lookahead, resources.runtime_lookahead_engines[prior])) {
          return "lookahead_internal_" + *reason;
        }
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] static CollisionValidationAdmission validate_admission(
      const CollisionPipelineResources& resources,
      bool runtime_protection_active) {
    if (resources.snapshot_publisher == nullptr) {
      return {CollisionValidationAdmissionStatus::kMissingSnapshotPublisher,
              "missing_collision_world_snapshot_publisher"};
    }
    if (resources.validation_engines.empty()) {
      return {CollisionValidationAdmissionStatus::kMissingValidationResource,
              "missing_collision_query_engines"};
    }
    if (resources.runtime_monitor_engine) {
      const auto& runtime_cap = resources.runtime_monitor_engine->capability();
      const auto capability_status =
          runtime_collision_query_capability_status(runtime_cap);
      if (capability_status !=
          RuntimeCollisionQueryCapabilityStatus::kAdmitted) {
        return {CollisionValidationAdmissionStatus::
                    kRuntimeQueryCapabilityUnsupported,
                runtime_collision_query_capability_status_name(
                    capability_status)};
      }
    }
    if (runtime_protection_active) {
      if (!resources.runtime_monitor_engine) {
        return {CollisionValidationAdmissionStatus::kMissingRuntimeResource,
                "missing_runtime_collision_query_engine"};
      }
      std::string conflict_reason;
      if (validation_pool_shares_runtime_resource(resources,
                                                  &conflict_reason)) {
        return {
            CollisionValidationAdmissionStatus::
                kRuntimeMonitorLeaseActiveSharedResource,
            conflict_reason};
      }
    }
    if (const auto conflict = runtime_lookahead_resource_conflict(resources)) {
      return {CollisionValidationAdmissionStatus::kRuntimeLookaheadResourceConflict, *conflict};
    }
    return {};
  }

  [[nodiscard]] static bool validation_conflicts_with_active_runtime_monitor(
      const CollisionPipelineResources& resources,
      ExecutionState state) {
    return runtime_monitor_may_own_resource(state) &&
           validation_pool_shares_runtime_resource(resources);
  }

 private:
  mutable std::mutex mutex_;
  CollisionPipelineResources resources_;
};

}  // namespace manyforge::core
