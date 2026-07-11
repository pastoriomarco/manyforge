#pragma once

#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/types/execution_status.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {

struct CollisionPipelineResources {
  ICollisionWorldSnapshotPublisher* snapshot_publisher = nullptr;
  std::shared_ptr<ICollisionQueryEngine> runtime_monitor_engine;
  std::vector<std::shared_ptr<ICollisionQueryEngine>> validation_engines;
};

enum class CollisionValidationAdmissionStatus {
  kAdmitted,
  kMissingSnapshotPublisher,
  kMissingRuntimeResource,
  kMissingValidationResource,
  kRuntimeQueryCapabilityUnsupported,
  kRuntimeMonitorLeaseActiveSharedResource,
};

struct CollisionValidationAdmission {
  CollisionValidationAdmissionStatus status =
      CollisionValidationAdmissionStatus::kAdmitted;
  std::string resource_conflict_reason;

  [[nodiscard]] bool admitted() const {
    return status == CollisionValidationAdmissionStatus::kAdmitted;
  }
};

/// Role-aware collision resource registry. Runtime monitoring and validation
/// are separate roles even when a legacy/test caller wires the same engine into
/// both; validation admission can then fail closed instead of borrowing the
/// runtime monitor's active resource.
class CollisionResourceManager {
 public:
  void configure(ICollisionWorldSnapshotPublisher& publisher,
                 std::shared_ptr<ICollisionQueryEngine> runtime_monitor_engine,
                 std::vector<std::shared_ptr<ICollisionQueryEngine>>
                     validation_engines) {
    CollisionPipelineResources next;
    next.snapshot_publisher = &publisher;
    next.runtime_monitor_engine = std::move(runtime_monitor_engine);
    next.validation_engines.reserve(validation_engines.size());
    for (auto& engine : validation_engines) {
      if (engine) {
        next.validation_engines.push_back(std::move(engine));
      }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    resources_ = std::move(next);
  }

  [[nodiscard]] CollisionPipelineResources snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return resources_;
  }

  [[nodiscard]] static bool runtime_monitor_may_own_resource(
      ExecutionState state) {
    return state == ExecutionState::kExecuting ||
           state == ExecutionState::kPaused ||
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
