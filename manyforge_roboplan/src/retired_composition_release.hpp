#pragma once

#include "ros_host_composition.hpp"
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace manyforge::roboplan_backend::host_detail {

struct RetainedComponentSnapshot final {
  std::shared_ptr<RetainedExecutionHost> runtime;
  std::shared_ptr<RetainedGripperRuntime> gripper_runtime;
  std::shared_ptr<const RetainedPlanningModelStage> stage;
  core::RetainedProductDTO planning_model_product;
  std::optional<core::JointLimitsDTO> joint_limits;
  CanonicalValue motion_descriptor;
  CanonicalValue gripper_descriptor;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> ros2_control_runtime_handle;
  std::shared_ptr<ControllerLaunchProcess> controller_process;
  std::shared_ptr<const IoProviderDriverGroup> io_provider_drivers{};
  std::shared_ptr<const ComponentRuntimePins> component_runtime_pins{};
  std::shared_ptr<core::IComponentAdapter> component_adapter{};
  std::optional<core::CoordinatedComponent> owner_component{};
  std::shared_ptr<void> owner_handle{};
  const ComponentAdapterFamily *owner_family{};
  std::optional<std::string> owner_id{};
  std::optional<std::string> member_role{};
  std::vector<std::pair<std::string, std::shared_ptr<AdapterTransportConsumer>>>
      adapter_transport_consumers{};
};

struct PendingDiscard final {
  std::unique_ptr<Composition> root;
  std::vector<RetainedComponentSnapshot> snapshots;
  std::string component_id;
  bool logged{false};
};

// Owns the copied snapshots through all exits from an Apply.
struct ApplyPendingSnapshotCleanup final {
  std::vector<PendingDiscard> snapshots;
  const std::function<void(std::vector<PendingDiscard> &)> &settle;
  ~ApplyPendingSnapshotCleanup() { settle(snapshots); }
};

struct ProtectedCompositionClosure final {
  std::set<core::IResolvedParticipantExecutionEngine *> engines;
  // Sharing a backend alone does not keep a different runtime's engine alive.
  std::set<core::IResolvedParticipantExecutionEngine *> exact_engines;
  std::set<RetainedRos2ControlRuntimeHandle *> handles;
  std::set<core::ExecutionKernel *> motion_engines;
};

struct PendingSettlementCache final {
  std::map<RetainedRos2ControlRuntimeHandle *, std::pair<std::string, std::string>> handles;
  std::map<core::ExecutionKernel *, std::string> engines;
  std::map<core::IResolvedParticipantExecutionEngine *, std::string> subscriptions;
  std::map<RetainedRos2ControlRuntimeHandle *, std::size_t> handle_groups;
  std::map<core::IResolvedParticipantExecutionEngine *, std::size_t> engine_groups;
  std::vector<std::pair<std::string, std::string>> group_failures;
};

using PendingHandleSettle = std::function<RetainedRos2ControlRuntimeHandle::HostQuiesceResult(
    const std::shared_ptr<RetainedRos2ControlRuntimeHandle> &,
    std::chrono::steady_clock::time_point)>;

struct ProtectedCompositionPins final {
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
  std::shared_ptr<RetainedExecutionHost> runtime;
  std::shared_ptr<RetainedGripperRuntime> gripper;
  std::shared_ptr<ControllerLaunchProcess> process{};
};

// Both retirement and pending-discard cleanup use the same exact identities.
// Snapshot-specific ownership remains in the caller's typed snapshot.
template <class Pending, class SnapshotRegistrations, class SnapshotPins, class SnapshotEngines>
void CollectPendingCompositionProtection(
    const Pending &pending, std::vector<ProtectedCompositionPins> &pins,
    std::set<core::IResolvedParticipantExecutionEngine *> &engines,
    SnapshotRegistrations &&registrations, SnapshotPins &&snapshot_pins,
    SnapshotEngines &&snapshot_engines) {
  std::function<void(const Composition *)> collect = [&](const Composition *view) {
    if (!view) return;
    pins.push_back({view->ros2_control_runtime_handle, view->runtime,
                    view->gripper_runtime, view->controller_process});
    for (const auto &child : view->participant_compositions) collect(child.get());
  };
  collect(pending.root.get());
  if (pending.root) {
    const auto exact = registrations(*pending.root);
    engines.insert(exact.begin(), exact.end());
  }
  for (const auto &snapshot : pending.snapshots) {
    pins.push_back(snapshot_pins(snapshot));
    snapshot_engines(snapshot, engines);
  }
}

template <class SnapshotRegistrations>
void CollectRetainedPendingProtection(
    const PendingDiscard &pending, std::vector<ProtectedCompositionPins> &pins,
    std::set<core::IResolvedParticipantExecutionEngine *> &engines,
    SnapshotRegistrations &&registrations) {
  CollectPendingCompositionProtection(
      pending, pins, engines, registrations,
      [](const RetainedComponentSnapshot &snapshot) {
        return ProtectedCompositionPins{snapshot.ros2_control_runtime_handle, snapshot.runtime,
                                        snapshot.gripper_runtime, snapshot.controller_process};
      },
      [](const RetainedComponentSnapshot &snapshot, auto &exact) {
        if (snapshot.runtime) {
          for (const auto &row : RetainedComponentOwner(snapshot.runtime).registrations())
            exact.insert(row.engine);
        } else if (snapshot.gripper_runtime) {
          for (const auto &row : RetainedComponentOwner(snapshot.gripper_runtime).registrations())
            exact.insert(row.engine);
        }
      });
}

template <class Snapshot>
ProtectedCompositionClosure BuildProtectedCompositionClosure(
    const std::array<const Composition *, 4> &roots, const Composition *candidate,
    Snapshot &&snapshot_registration_engines,
    const std::vector<ProtectedCompositionPins> &extra = {},
    const std::vector<ProtectedCompositionPins> &candidate_extra = {},
    const std::set<core::IResolvedParticipantExecutionEngine *> &extra_exact = {}) {
    ProtectedCompositionClosure result;
    auto &active_engines = result.engines;
    auto &active_handles = result.handles;
    auto &active_motion_engines = result.motion_engines;
    std::set<core::ExecutionPortParticipant *> active_participants;
    std::set<RetainedGripperRuntime *> active_gripper_runtimes;
    std::set<const void *> active_backends;
    std::vector<std::shared_ptr<const void>> active_backend_owners;
    const auto same_owner = [](const auto &left, const auto &right) {
      return !left.owner_before(right) && !right.owner_before(left);
    };
    const auto add_owner = [&](const std::shared_ptr<const void> &candidate) {
      if (!candidate) return false;
      for (const auto &current : active_backend_owners)
        if (same_owner(candidate, current)) return false;
      active_backend_owners.push_back(candidate);
      return true;
    };
    const auto protect_pins = [&](const ProtectedCompositionPins &pins) {
      bool changed = false;
      if (pins.handle) {
        const auto handle = pins.handle;
        changed |= active_handles.insert(handle.get()).second;
        if (const auto participant = handle->participant())
          changed |= active_participants.insert(participant.get()).second;
        changed |= add_owner(handle);
      }
      if (pins.runtime) {
        if (pins.runtime->participant_engine) {
          changed |= active_motion_engines.insert(pins.runtime->participant_engine.get()).second;
          changed |= active_engines.insert(pins.runtime->participant_engine.get()).second;
        }
        if (pins.runtime->execution)
          changed |= active_backends.insert(pins.runtime->execution.get()).second;
        changed |= add_owner(pins.runtime->execution);
      }
      if (pins.gripper) {
        changed |= active_gripper_runtimes.insert(pins.gripper.get()).second;
        if (auto *engine = pins.gripper->participant_engine())
          changed |= active_engines.insert(engine).second;
        const auto backend = pins.gripper->backend_owner();
        if (backend) changed |= active_backends.insert(backend.get()).second;
        changed |= add_owner(backend);
      }
      return changed;
    };
    const auto protect_view = [&](const Composition *view) {
      return protect_pins({view->ros2_control_runtime_handle, view->runtime, view->gripper_runtime});
    };
    std::function<void(const Composition *)> collect_active = [&](const Composition *view) {
      if (!view) return;
      static_cast<void>(protect_view(view));
      for (const auto &child : view->participant_compositions) collect_active(child.get());
    };
    for (const auto *root : roots) {
      collect_active(root);
      if (!root) continue;
      const auto engines = snapshot_registration_engines(*root);
      active_engines.insert(engines.begin(), engines.end());
    }
    for (const auto &pins : extra) static_cast<void>(protect_pins(pins));
    active_engines.insert(extra_exact.begin(), extra_exact.end());
    result.exact_engines = active_engines;
    // Apply construction and retirement use the same Manager lane, so no
    // separate Apply-local candidate is resident while this collector runs.
    std::vector<const Composition *> retired_views;
    std::function<void(const Composition *)> collect_views = [&](const Composition *view) {
      if (!view) return;
      retired_views.push_back(view);
      for (const auto &child : view->participant_compositions) collect_views(child.get());
    };
    collect_views(candidate);
    const auto is_linked = [&](const ProtectedCompositionPins &pins) {
        const auto handle = pins.handle;
        const auto participant = handle ? handle->participant() : nullptr;
        const auto gripper = pins.gripper;
        const std::shared_ptr<const void> motion_backend =
            pins.runtime ? pins.runtime->execution : nullptr;
        const std::shared_ptr<const void> gripper_backend =
            gripper ? gripper->backend_owner() : nullptr;
        const auto owned_by_active = [&](const auto &candidate) {
          if (!candidate) return false;
          return std::any_of(active_backend_owners.begin(), active_backend_owners.end(),
                             [&](const auto &current) { return same_owner(candidate, current); });
        };
        return (handle && (active_handles.contains(handle.get()) || owned_by_active(handle))) ||
            (participant && active_participants.contains(participant.get())) ||
            (pins.runtime && pins.runtime->participant_engine &&
             (active_motion_engines.contains(pins.runtime->participant_engine.get()) ||
              active_engines.contains(pins.runtime->participant_engine.get()))) ||
            (gripper && (active_gripper_runtimes.contains(gripper.get()) ||
                         active_engines.contains(gripper->participant_engine()))) ||
            (motion_backend && active_backends.contains(motion_backend.get())) ||
            (gripper_backend && active_backends.contains(gripper_backend.get())) ||
            owned_by_active(motion_backend) || owned_by_active(gripper_backend);
    };
    bool expanded = false;
    do {
      expanded = false;
      for (const auto *view : retired_views) {
        const ProtectedCompositionPins pins{view->ros2_control_runtime_handle, view->runtime, view->gripper_runtime};
        if (is_linked(pins)) expanded |= protect_pins(pins);
      }
      for (const auto &pins : candidate_extra)
        if (is_linked(pins)) expanded |= protect_pins(pins);
    } while (expanded);
    return result;
}

// The caller runs this on the Manager lane with all roots and the Kernel pinned.
template <class KernelGetter, class Snapshot>
bool RetryPendingCompositionDiscards(
    std::vector<PendingDiscard> &pending_discards,
    KernelGetter &&stable_addressed_kernel, Snapshot &&snapshot_registration_engines,
    std::chrono::steady_clock::time_point host_deadline,
    const ProtectedCompositionClosure &fixed_protection,
    PendingSettlementCache *cache,
    bool handles_only = false, bool effects_only = false,
    const PendingHandleSettle *settle_handle = nullptr) noexcept {
    for (auto it = pending_discards.begin(); it != pending_discards.end();) {
        auto &pending = *it;
        const char *stage = "join";
        std::optional<std::size_t> current_group;
        try {
        auto *const owner = stable_addressed_kernel();
        if (!owner) throw std::runtime_error("addressed kernel unavailable");
        std::vector<ProtectedCompositionPins> candidates;
        for (const auto &snapshot : pending.snapshots)
          candidates.push_back({snapshot.ros2_control_runtime_handle, snapshot.runtime, snapshot.gripper_runtime});
        const auto &protected_owners = fixed_protection;
        std::vector<ProtectedCompositionPins> pins = candidates;
        std::function<void(const Composition *)> collect = [&](const Composition *view) {
          if (!view) return;
          pins.push_back({view->ros2_control_runtime_handle, view->runtime, view->gripper_runtime});
          for (const auto &child : view->participant_compositions) collect(child.get());
        };
        collect(pending.root.get());
        std::set<RetainedRos2ControlRuntimeHandle *> closed;
        std::set<core::ExecutionKernel *> detached;
        std::set<core::IResolvedParticipantExecutionEngine *> engines;
        if (pending.root) engines = snapshot_registration_engines(*pending.root);
        for (const auto &snapshot : pending.snapshots) {
          if (snapshot.runtime) {
            for (const auto &row : RetainedComponentOwner(snapshot.runtime).registrations())
              engines.insert(row.engine);
          } else if (snapshot.gripper_runtime) {
            for (const auto &row : RetainedComponentOwner(snapshot.gripper_runtime).registrations())
              engines.insert(row.engine);
          }
        }
        std::string code;
        for (const auto &pin : pins) {
          const auto handle = pin.handle;
          const bool unique_handle = handle && !protected_owners.handles.contains(handle.get());
          const bool unique_engine = pin.runtime && pin.runtime->participant_engine &&
              !protected_owners.exact_engines.contains(pin.runtime->participant_engine.get());
          std::optional<std::size_t> group;
          if (cache && handle) {
            const auto found = cache->handle_groups.find(handle.get());
            if (found != cache->handle_groups.end()) group = found->second;
          }
          if (cache && !group && pin.runtime && pin.runtime->participant_engine) {
            const auto found = cache->engine_groups.find(pin.runtime->participant_engine.get());
            if (found != cache->engine_groups.end()) group = found->second;
          }
          current_group = group;
          if (group && !cache->group_failures[*group].second.empty()) {
            stage = cache->group_failures[*group].first.c_str();
            code = cache->group_failures[*group].second;
            break;
          }
          if (!unique_handle && !unique_engine) continue;
          if (!handle) {
            code = "internal_error";
            if (group) cache->group_failures[*group] = {stage, code};
            if (cache && unique_engine) {
              cache->engines.emplace(pin.runtime->participant_engine.get(), code);
              cache->subscriptions.emplace(pin.runtime->participant_engine.get(), code);
            }
            break;
          }
          const auto authored_deadline = handle->OwnerShutdownDeadline();
          if (!authored_deadline) {
            code = "internal_error";
            if (group) cache->group_failures[*group] = {stage, code};
            if (cache && unique_engine) {
              cache->engines.emplace(pin.runtime->participant_engine.get(), code);
              cache->subscriptions.emplace(pin.runtime->participant_engine.get(), code);
            }
            break;
          }
          const auto deadline = std::min(*authored_deadline, host_deadline);
          if (unique_handle && closed.insert(handle.get()).second) {
            auto cached = cache ? cache->handles.find(handle.get()) : decltype(cache->handles.find(handle.get())){};
            if (cache && cached != cache->handles.end()) {
              stage = cached->second.first.c_str(); code = cached->second.second;
            } else {
              if (cache) cache->handles.emplace(handle.get(), std::make_pair("join", "internal_error"));
              const auto result = settle_handle ? (*settle_handle)(handle, deadline)
                                                : handle->QuiesceForHostShutdown(deadline);
              if (result.outcome != RetainedRos2ControlRuntimeHandle::HostQuiesceOutcome::kSettled) {
                stage = result.stage; code = result.code;
              }
              if (cache) cache->handles.at(handle.get()) = {stage, code};
            }
            if (!code.empty()) {
              if (group) cache->group_failures[*group] = {stage, code};
              // The same engine may be reachable through another handle;
              // do not detach it after this owner failed to quiesce.
              if (cache && unique_engine) {
                cache->engines.emplace(pin.runtime->participant_engine.get(), code);
                cache->subscriptions.emplace(pin.runtime->participant_engine.get(), code);
              }
              break;
            }
          }
          if (!handles_only && unique_engine && detached.insert(pin.runtime->participant_engine.get()).second) {
            auto *engine = pin.runtime->participant_engine.get();
            auto cached = cache ? cache->engines.find(engine) : decltype(cache->engines.find(engine)){};
            if (cache && cached != cache->engines.end()) code = cached->second;
            else {
              if (cache) cache->engines.emplace(engine, "internal_error");
              const auto result = engine->detach(deadline);
              if (!result.ok()) code = result.error().code;
              if (cache) cache->engines.at(engine) = code;
            }
            if (!code.empty()) {
              if (group) cache->group_failures[*group] = {stage, code};
              if (cache) cache->subscriptions.emplace(engine, code);
              break;
            }
          }
        }
        if (handles_only) { ++it; continue; }
        if (code.empty()) {
          stage = "subscription_release";
          for (auto *engine : engines) {
            if (!engine || protected_owners.exact_engines.contains(engine)) continue;
            if (cache) {
              const auto group = cache->engine_groups.find(engine);
              if (group != cache->engine_groups.end()) current_group = group->second;
              if (group != cache->engine_groups.end() &&
                  !cache->group_failures[group->second].second.empty()) {
                stage = cache->group_failures[group->second].first.c_str();
                code = cache->group_failures[group->second].second;
                break;
              }
            }
            auto cached = cache ? cache->subscriptions.find(engine) : decltype(cache->subscriptions.find(engine)){};
            if (cache && cached != cache->subscriptions.end()) code = cached->second;
            else {
              if (cache) cache->subscriptions.emplace(engine, "internal_error");
              const auto result = owner->release_retired_engine(engine);
              if (!result.ok()) {
                const auto reason = result.error().details.find("reason");
                if (result.error().code != "missing_reference" || reason == result.error().details.end() ||
                    reason->second != "retired_participant_engine_subscription_absent")
                  code = result.error().code;
              }
              if (cache) cache->subscriptions.at(engine) = code;
            }
            if (!code.empty()) {
              if (cache) {
                const auto group = cache->engine_groups.find(engine);
                if (group != cache->engine_groups.end())
                  cache->group_failures[group->second] = {stage, code};
              }
              break;
            }
          }
        }
        if (effects_only) { ++it; continue; }
        if (code.empty()) { it = pending_discards.erase(it); continue; }
        if (!pending.logged) {
          const auto component = content_identity::canonical_json(CanonicalValue(
              pending.component_id.size() <= 256U ? pending.component_id : "<too_long>"));
          const auto reason = content_identity::canonical_json(CanonicalValue(
              code.size() <= 128U ? code : "internal_error"));
          std::fprintf(stderr, "{\"code\":%s,\"component\":%s,\"stage\":\"%s\"}\n",
                       reason.c_str(), component.c_str(), stage);
          pending.logged = true;
        }
        ++it;
        } catch (...) {
          if (cache && current_group)
            cache->group_failures[*current_group] = {stage, "internal_error"};
          if (!pending.logged) {
            try {
              const auto component = content_identity::canonical_json(CanonicalValue(
                  pending.component_id.size() <= 256U ? pending.component_id : "<too_long>"));
              std::fprintf(stderr, "{\"code\":\"internal_error\",\"component\":%s,\"stage\":\"%s\"}\n",
                           component.c_str(), stage);
            } catch (...) {
              std::fputs("{\"code\":\"internal_error\",\"component\":\"<unavailable>\",\"stage\":\"exception\"}\n", stderr);
            }
            pending.logged = true;
          }
          ++it;
        }
    }
    return pending_discards.empty();
}

template <class KernelGetter, class Snapshot, class Collect>
void SettleApplyPendingSnapshots(
    std::vector<PendingDiscard> &snapshots, std::vector<PendingDiscard> &pending,
    const std::array<const Composition *, 4> &roots, KernelGetter &&kernel,
    Snapshot &&registrations, Collect &&collect,
    const PendingHandleSettle *settle_handle = nullptr,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
  if (snapshots.empty()) return;
  try {
  std::vector<ProtectedCompositionPins> protected_pins;
  std::set<core::IResolvedParticipantExecutionEngine *> protected_engines;
  for (const auto &entry : pending)
    collect(entry, protected_pins, protected_engines);
  std::vector<ProtectedCompositionPins> candidates;
  std::set<core::IResolvedParticipantExecutionEngine *> candidate_engines;
  for (const auto &entry : snapshots)
    collect(entry, candidates, candidate_engines);
  const auto protection = BuildProtectedCompositionClosure(
      roots, nullptr, registrations, protected_pins, candidates, protected_engines);
  PendingSettlementCache cache;
  std::vector<std::size_t> parent(candidates.size());
  for (std::size_t i = 0; i < parent.size(); ++i) parent[i] = i;
  const auto root_of = [&](std::size_t i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  std::map<std::shared_ptr<const void>, std::size_t,
           std::owner_less<std::shared_ptr<const void>>> owners;
  std::map<core::IResolvedParticipantExecutionEngine *, std::size_t> engines;
  const auto link_owner = [&](const std::shared_ptr<const void> &owner, std::size_t i) {
    if (!owner) return;
    const auto [it, inserted] = owners.emplace(owner, i);
    if (!inserted) parent[root_of(i)] = root_of(it->second);
  };
  const auto link_engine = [&](core::IResolvedParticipantExecutionEngine *engine, std::size_t i) {
    if (!engine) return;
    const auto [it, inserted] = engines.emplace(engine, i);
    if (!inserted) parent[root_of(i)] = root_of(it->second);
  };
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const auto &pin = candidates[i];
    link_owner(pin.handle, i);
    if (pin.runtime) {
      link_owner(pin.runtime->execution, i);
      link_engine(pin.runtime->participant_engine.get(), i);
      for (const auto &row : RetainedComponentOwner(pin.runtime).registrations())
        link_engine(row.engine, i);
    }
    if (pin.gripper) {
      link_owner(pin.gripper->backend_owner(), i);
      link_engine(pin.gripper->participant_engine(), i);
      for (const auto &row : RetainedComponentOwner(pin.gripper).registrations())
        link_engine(row.engine, i);
    }
  }
  cache.group_failures.resize(candidates.size());
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const auto &pin = candidates[i];
    const auto group = root_of(i);
    if (pin.handle) cache.handle_groups.emplace(pin.handle.get(), group);
    if (pin.runtime)
      for (const auto &row : RetainedComponentOwner(pin.runtime).registrations())
        cache.engine_groups.emplace(row.engine, group);
    if (pin.runtime && pin.runtime->participant_engine)
      cache.engine_groups.emplace(pin.runtime->participant_engine.get(), group);
    if (pin.gripper)
      for (const auto &row : RetainedComponentOwner(pin.gripper).registrations())
        cache.engine_groups.emplace(row.engine, group);
    if (pin.gripper && pin.gripper->participant_engine())
      cache.engine_groups.emplace(pin.gripper->participant_engine(), group);
  }
  static_cast<void>(RetryPendingCompositionDiscards(
      snapshots, kernel, registrations, deadline, protection, &cache, true, false, settle_handle));
  static_cast<void>(RetryPendingCompositionDiscards(
      snapshots, kernel, registrations, deadline, protection, &cache, false, true, settle_handle));
  static_cast<void>(RetryPendingCompositionDiscards(
      snapshots, kernel, registrations, deadline, protection, &cache, false, false, settle_handle));
  } catch (...) {
    // A protection failure cannot safely release any unprocessed snapshot.
    for (auto &entry : snapshots) {
      if (entry.logged) continue;
      try {
        const auto component = content_identity::canonical_json(CanonicalValue(
            entry.component_id.size() <= 256U ? entry.component_id : "<too_long>"));
        std::fprintf(stderr, "{\"code\":\"internal_error\",\"component\":%s,\"stage\":\"join\"}\n",
                     component.c_str());
      } catch (...) {
        std::fputs("{\"code\":\"internal_error\",\"component\":\"<unavailable>\",\"stage\":\"join\"}\n", stderr);
      }
      entry.logged = true;
    }
  }
  for (auto &entry : snapshots) pending.push_back(std::move(entry));
}

// The caller keeps discarded and all remaining roots pinned for this call.
// Only the Kernel's exact typed absent-subscription result is a no-op.
template <class Snapshot>
core::Result<void> ReleaseDiscardedCompositionSubscriptions(
    core::ResourceAddressedExecutionKernel &owner, const Composition *discarded,
    const std::array<const Composition *, 4> &remaining_roots,
    Snapshot &&snapshot_registration_engines,
    const std::vector<ProtectedCompositionPins> &extra = {}) {
  if (!discarded) return core::Result<void>::Ok();
  const auto protected_owners = BuildProtectedCompositionClosure(
      remaining_roots, discarded, snapshot_registration_engines, extra);
  const auto discarded_engines = snapshot_registration_engines(*discarded);
  for (auto *engine : discarded_engines) {
    if (protected_owners.exact_engines.contains(engine)) continue;
    const auto released = owner.release_retired_engine(engine);
    if (released.ok()) continue;
    const auto &error = released.error();
    const auto reason = error.details.find("reason");
    if (error.code == "missing_reference" && reason != error.details.end() &&
        reason->second == "retired_participant_engine_subscription_absent")
      continue;
    return core::Result<void>::Err(error);
  }
  return core::Result<void>::Ok();
}

template <class KernelGetter, class Snapshot, class Index, class Retire, class Reclaim, class RequireFn>
void TryReleaseRetiredComposition(
    std::mutex &managed_composition_mutex,
    std::unique_ptr<Composition> &active_managed_composition,
    std::unique_ptr<Composition> &prepared_managed_composition,
    std::unique_ptr<Composition> &failed_prepared_composition,
    std::unique_ptr<Composition> &retired_managed_composition,
    std::set<core::IResolvedParticipantExecutionEngine *> &retired_registration_engines,
    const Composition *&retired_failure_logged_for,
    KernelGetter &&stable_addressed_kernel, Snapshot &&snapshot_registration_engines,
    Index &&IndexNativeComponentCompositions, Retire &&retire_process,
    Reclaim &&reclaim, RequireFn &&require,
    const std::vector<ProtectedCompositionPins> &extra = {},
    const std::set<core::IResolvedParticipantExecutionEngine *> &extra_exact = {}) {
    std::unique_lock lock(managed_composition_mutex);
    auto *const owner = stable_addressed_kernel();
    if (owner == nullptr) {
      return;
    }
    if (retired_managed_composition == nullptr) {
      retired_failure_logged_for = nullptr;
      require(retired_registration_engines.empty(),
              "retired engine snapshot has no retired composition");
      reclaim();
      return;
    }
    const auto *const retiring = retired_managed_composition.get();
    const auto protected_owners = BuildProtectedCompositionClosure(
        {active_managed_composition.get(), prepared_managed_composition.get(),
         failed_prepared_composition.get()}, retiring, snapshot_registration_engines, extra, {}, extra_exact);
    const auto &active_handles = protected_owners.handles;
    for (auto *engine : retired_registration_engines) {
      if (!protected_owners.exact_engines.contains(engine) && owner->participant_engine_in_use(engine))
        return;
    }
    struct RetiredOwner final {
      std::string component;
      std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
      std::shared_ptr<core::ExecutionKernel> engine;
      bool close_handle;
    };
    std::vector<RetiredOwner> unique_owners;
    std::set<RetainedRos2ControlRuntimeHandle *> seen_handles;
    std::set<core::ExecutionKernel *> seen_motion_engines;
    std::function<bool(const Composition *)> collect_retired = [&](const Composition *view) {
      if (!view) return true;
      const auto handle = view->ros2_control_runtime_handle;
      const auto engine = view->runtime ? view->runtime->participant_engine : nullptr;
      const bool unique_handle = handle && !active_handles.contains(handle.get()) &&
                                 seen_handles.insert(handle.get()).second;
      const bool unique_engine = engine && !protected_owners.exact_engines.contains(engine.get()) &&
                                 seen_motion_engines.insert(engine.get()).second;
      if (unique_handle || unique_engine)
        unique_owners.push_back({view->controllable_component_id.value(),
                                 handle,
                                 unique_engine ? engine : nullptr, unique_handle});
      for (const auto &child : view->participant_compositions)
        if (!collect_retired(child.get())) return false;
      return true;
    };
    if (!collect_retired(retiring)) return;
    lock.unlock();
    bool owner_failed = false;
    const auto log_retired_failure = [&](const RetiredOwner &item,
                                         const char *stage,
                                         const std::string &failure_code) {
      if (retired_failure_logged_for == retiring) return;
      const auto component = content_identity::canonical_json(CanonicalValue(
          item.component.size() <= 256U ? item.component : "<too_long>"));
      const auto code = content_identity::canonical_json(CanonicalValue(
          failure_code.size() <= 128U ? failure_code : "internal_error"));
      std::fprintf(stderr, "{\"code\":%s,\"component\":%s,\"generation\":%llu,\"stage\":\"%s\"}\n",
                   code.c_str(), component.c_str(),
                   static_cast<unsigned long long>(retiring->publication_generation.value()), stage);
      retired_failure_logged_for = retiring;
    };
    for (const auto &item : unique_owners) {
      if (!item.handle) { log_retired_failure(item, "join", "internal_error"); owner_failed = true; break; }
      const auto deadline = item.handle->OwnerShutdownDeadline().value();
      if (item.close_handle) {
        const auto result = item.handle->QuiesceForHostShutdown(deadline);
        if (result.outcome != RetainedRos2ControlRuntimeHandle::HostQuiesceOutcome::kSettled) {
          log_retired_failure(item, result.stage, result.code);
          owner_failed = true;
          break;
        }
      }
      if (item.engine) {
        const auto detached = item.engine->detach(deadline);
        if (!detached.ok()) {
          log_retired_failure(item, "join", detached.error().code);
          owner_failed = true;
          break;
        }
      }
    }
    lock.lock();
    if (retired_managed_composition.get() != retiring || stable_addressed_kernel() != owner) return;
    if (owner_failed) return;
    std::vector<std::shared_ptr<ControllerLaunchProcess>> removed_processes;
    std::set<const ControllerLaunchProcess *> active_processes;
    for (const auto &pins : extra)
      if (pins.process) active_processes.insert(pins.process.get());
    for (const auto *root : {active_managed_composition.get(), prepared_managed_composition.get(),
                             failed_prepared_composition.get()}) {
      if (!root) continue;
      for (const auto &[id, view] : IndexNativeComponentCompositions(*root)) {
        static_cast<void>(id);
        if (view->controller_process)
          active_processes.insert(view->controller_process.get());
      }
    }
    for (const auto &[id, view] :
         IndexNativeComponentCompositions(*retired_managed_composition)) {
      static_cast<void>(id);
      if (view->controller_process &&
          !active_processes.contains(view->controller_process.get()))
        removed_processes.push_back(view->controller_process);
    }
    // The same Manager channel owns composition replacement. Keep its
    // retired Composition, model and engine pins until exact group cleanup
    // completes; operational Stop does not wait on this mutex.
    lock.unlock();
    for (const auto &process : removed_processes)
      // A stale slot means the supervisor already ran or is running its stop protocol.
      static_cast<void>(
          retire_process(process));
    lock.lock();
    // Only Manager dispatch mutates the retired owner/fence; the cell owner is stable.
    require(retired_managed_composition.get() == retiring &&
                stable_addressed_kernel() == owner,
            "retired composition changed during exact process cleanup");
    for (auto engine = retired_registration_engines.begin();
         engine != retired_registration_engines.end();) {
      if (protected_owners.exact_engines.contains(*engine)) {
        ++engine;
        continue;
      }
      if (!owner->release_retired_engine(*engine).ok()) {
        return;
      }
      engine = retired_registration_engines.erase(engine);
    }
    retired_registration_engines.clear();
    retired_managed_composition.reset();
    retired_failure_logged_for = nullptr;
    reclaim();
}

} // namespace manyforge::roboplan_backend::host_detail
