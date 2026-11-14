#include "managed_kernel_apply_fence_owner.hpp"

#include "kernel_apply_fence_rpc_internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace manyforge::roboplan_backend {
namespace {

namespace component_manager = manyforge::component_manager;
namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;

// First field of the ledger digest.
constexpr std::string_view kLedgerDigestDomain =
    "manyforge.internal.kernel-configuration-fence-ledger.v8";
constexpr std::size_t kMaximumFenceRecords = 256U;

[[noreturn]] void Fail(std::string detail) {
  throw std::runtime_error("Managed Kernel Apply-fence owner: " +
                           std::move(detail));
}

void Require(const bool condition, std::string detail) {
  if (!condition) {
    Fail(std::move(detail));
  }
}

void AppendField(std::string &bytes, const std::string_view value) {
  bytes.append(std::to_string(value.size()));
  bytes.push_back(':');
  bytes.append(value);
}

template <typename... Values>
[[nodiscard]] core::ContentDigest Digest(const Values &...values) {
  std::string bytes;
  (AppendField(bytes, std::string_view(values)), ...);
  return core::ContentDigest(content_identity::sha256_digest(bytes));
}

template <typename Value>
[[nodiscard]] std::string EncodeRequest(const Value &value) {
  return component_manager::detail::EncodeKernelApplyFenceRpcRequest(
      component_manager::detail::KernelApplyFenceRpcRequest(value));
}

template <typename Value>
[[nodiscard]] std::string EncodeResponse(const Value &value) {
  return component_manager::detail::EncodeKernelApplyFenceRpcResponse(
      component_manager::detail::KernelApplyFenceRpcResponse(value));
}

template <typename Value>
[[nodiscard]] Value DecodeRequest(const std::string_view bytes) {
  auto decoded =
      component_manager::detail::DecodeKernelApplyFenceRpcRequest(bytes);
  const auto *value = std::get_if<Value>(&decoded);
  Require(value != nullptr, "durable request blob has the wrong kind");
  return *value;
}

template <typename Value>
[[nodiscard]] Value DecodeResponse(const std::string_view bytes) {
  auto decoded =
      component_manager::detail::DecodeKernelApplyFenceRpcResponse(bytes);
  const auto *value = std::get_if<Value>(&decoded);
  Require(value != nullptr, "durable response blob has the wrong kind");
  return *value;
}

[[nodiscard]] bool ConfigurationLess(
    const core::DomainActiveControlConfigurationDTO &left,
    const core::DomainActiveControlConfigurationDTO &right) noexcept {
  return left.control_domain_id < right.control_domain_id;
}

void ValidateConfigurations(
    const std::vector<core::DomainActiveControlConfigurationDTO>
        &configurations) {
  Require(std::all_of(configurations.begin(), configurations.end(),
                      [](const auto &row) { return row.valid(); }) &&
              std::is_sorted(configurations.begin(), configurations.end(),
                             ConfigurationLess) &&
              std::adjacent_find(configurations.begin(), configurations.end(),
                                 [](const auto &left, const auto &right) {
                                   return left.control_domain_id ==
                                          right.control_domain_id;
                                 }) == configurations.end(),
          "active-control-configuration rows are invalid or noncanonical");
}

// The predecessor-side rows an Apply fence pins at acquisition: the
// reconciled configuration of every predecessor controller domain that has
// one. A domain with neither a reconciled configuration nor a route
// publication was never Enabled (or was Disabled): nothing is installed for
// it to pin, so it yields no row, the rule ObservedConfigurations applies for
// releases. A domain that still has a route publication is commandable or
// externally owned (e.g. an unreconciled external route); pinning nothing for
// it would hide live control, so it still refuses.
[[nodiscard]] std::vector<core::DomainActiveControlConfigurationDTO>
SelectConfigurations(
    const std::vector<core::DomainActiveControlConfigurationDTO> &available,
    const std::vector<core::DomainActiveControlConfigurationDTO>
        &route_published,
    const core::ConfigurationFenceScopeDTO &scope) {
  const auto find = [](const auto &rows, const core::ControlDomainId &id) {
    const auto found = std::lower_bound(
        rows.begin(), rows.end(), id,
        [](const auto &configuration, const auto &domain_id) {
          return configuration.control_domain_id < domain_id;
        });
    return found != rows.end() && found->control_domain_id == id ? &*found
                                                                 : nullptr;
  };
  std::vector<core::DomainActiveControlConfigurationDTO> result;
  for (const auto &domain : scope.domains) {
    if (!domain.has_controller_predecessor()) {
      continue;
    }
    if (const auto *found = find(available, domain.control_domain_id)) {
      result.push_back(*found);
      continue;
    }
    Require(find(route_published, domain.control_domain_id) == nullptr,
            "the execution owner has no reconciled active configuration for "
            "an affected domain that still has a route publication");
  }
  return result;
}

// The rows a held fence's status reports: the configurations actually active
// now for the scope's controller domains. A domain with none (new to the
// cell, or not installed yet) has nothing to report or restore, so neither a
// before-effect release nor a post-publication release requires one.
[[nodiscard]] std::vector<core::DomainActiveControlConfigurationDTO>
ObservedConfigurations(
    const std::vector<core::DomainActiveControlConfigurationDTO> &available,
    const core::ConfigurationFenceScopeDTO &scope) {
  std::vector<core::DomainActiveControlConfigurationDTO> result;
  for (const auto &domain : scope.domains) {
    if (!domain.has_controller_side()) {
      continue;
    }
    const auto found = std::lower_bound(
        available.begin(), available.end(), domain.control_domain_id,
        [](const auto &configuration, const auto &domain_id) {
          return configuration.control_domain_id < domain_id;
        });
    if (found != available.end() &&
        found->control_domain_id == domain.control_domain_id) {
      result.push_back(*found);
    }
  }
  return result;
}

[[nodiscard]] std::string
FenceIdFor(const core::ApplyConfigurationFenceIntentDTO &intent,
           const std::uint64_t generation) {
  const auto digest = content_identity::sha256_digest(EncodeRequest(intent));
  return "configuration-fence-" + std::to_string(generation) + "-" +
         digest.substr(digest.size() - 16U);
}

// One fence of the in-memory ledger. State 0 is held, 1 is released; a
// released fence carries its release request and receipt.
struct FenceRow final {
  core::ApplyFenceOwnerDTO owner;
  core::RequestKey request_key;
  std::string intent_bytes;
  std::string accepted_bytes;
  core::ConfigurationFenceId fence_id;
  std::uint64_t fence_generation;
  int state;
  std::optional<std::string> release_request_bytes;
  std::optional<std::string> release_receipt_bytes;
};

[[nodiscard]] bool
ScopesOverlap(const core::ConfigurationFenceScopeDTO &left,
              const core::ConfigurationFenceScopeDTO &right) noexcept {
  return std::any_of(
      left.resources.begin(), left.resources.end(), [&](const auto &resource) {
        return std::binary_search(right.resources.begin(),
                                  right.resources.end(), resource);
      });
}

} // namespace

ManagedKernelAdmissionGate::ManagedKernelAdmissionGate()
    : admission_mutex_(std::make_shared<std::recursive_mutex>()) {}

std::shared_ptr<std::recursive_mutex>
ManagedKernelAdmissionGate::shared_admission_mutex() const {
  return admission_mutex_;
}

void ManagedKernelAdmissionGate::BindResourceTable(
    std::shared_ptr<const core::ResourceSlotTable> table) {
  Require(table != nullptr && table->shares_admission_mutex(admission_mutex_),
          "bound Kernel resource table does not share the admission mutex");
  std::lock_guard lock(*admission_mutex_);
  const auto current = resource_table_.lock();
  if (current != nullptr && current != table) {
    Require(!held_fences_.empty(),
            "live Kernel resource-table replacement is not fenced");
    for (const auto &held : held_fences_) {
      const auto observation =
          current->admission_snapshot(held.scope.resources);
      Require(observation.affected_queued_operation_count == 0U &&
                  observation.affected_active_operation_count == 0U,
              "live Kernel resource-table replacement still has affected work");
    }
  } else if (current == nullptr && resource_table_was_bound_) {
    Require(!held_fences_.empty(),
            "expired Kernel resource table cannot be replaced without a "
            "held fence");
  }
  resource_table_ = std::move(table);
  resource_table_was_bound_ = true;
}

core::Result<void> ManagedKernelAdmissionGate::reserve(
    core::ResourceSlotTable &table, const core::ResourceSlotOwnerDTO &owner,
    const std::vector<core::ResourceId> &resources) {
  std::lock_guard lock(*admission_mutex_);
  const auto bound = resource_table_.lock();
  if (bound == nullptr || bound.get() != &table ||
      !table.shares_admission_mutex(admission_mutex_)) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
        "Kernel operation admission has no exact bound resource table",
        {{"reason", "resource_admission_table_unbound"}}});
  }
  const auto blocked = std::any_of(
      held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
        return std::any_of(
            resources.begin(), resources.end(), [&](const auto &resource) {
              return std::binary_search(held.scope.resources.begin(),
                                        held.scope.resources.end(), resource);
            });
      });
  if (blocked) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
        "Kernel operation admission is closed by a configuration fence",
        {{"reason", "configuration_fence_held"}}});
  }
  return table.reserve(owner, resources);
}

core::Result<void> ManagedKernelAdmissionGate::reserve_batch(
    core::ResourceSlotTable &table,
    const std::vector<core::ResourceSlotReservationOwnerDTO> &owners) {
  std::lock_guard lock(*admission_mutex_);
  const auto bound = resource_table_.lock();
  if (bound == nullptr || bound.get() != &table ||
      !table.shares_admission_mutex(admission_mutex_)) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
        "Kernel operation admission has no exact bound resource table",
        {{"reason", "resource_admission_table_unbound"}}});
  }
  const auto blocked = std::any_of(
      held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
        return std::any_of(owners.begin(), owners.end(), [&](const auto &row) {
          return std::any_of(row.resources.begin(), row.resources.end(),
                             [&](const auto &resource) {
                               return std::binary_search(
                                   held.scope.resources.begin(),
                                   held.scope.resources.end(), resource);
                             });
        });
      });
  if (blocked) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
        "Kernel operation admission is closed by a configuration fence",
        {{"reason", "configuration_fence_held"}}});
  }
  return table.reserve_batch(owners);
}

std::vector<core::ResourceId>
ManagedKernelAdmissionGate::current_fenced_resources(
    const std::vector<core::ResourceId> &resources) const {
  std::lock_guard lock(*admission_mutex_);
  std::vector<core::ResourceId> result;
  result.reserve(resources.size());
  for (const auto &resource : resources) {
    if (std::any_of(
            held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
              return std::binary_search(held.scope.resources.begin(),
                                        held.scope.resources.end(), resource);
            })) {
      result.push_back(resource);
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

bool ManagedKernelAdmissionGate::exact_fence_covers(
    const core::ConfigurationFenceId &fence_id,
    const std::vector<core::ResourceId> &resources) const {
  std::lock_guard lock(*admission_mutex_);
  const auto held = std::find_if(
      held_fences_.begin(), held_fences_.end(),
      [&](const auto &candidate) { return candidate.fence_id == fence_id; });
  return !fence_id.empty() && !resources.empty() &&
         held != held_fences_.end() &&
         std::all_of(
             resources.begin(), resources.end(), [&](const auto &resource) {
               return std::binary_search(held->scope.resources.begin(),
                                         held->scope.resources.end(), resource);
             });
}

bool ManagedKernelAdmissionGate::has_held_configuration_fences() const {
  std::lock_guard lock(*admission_mutex_);
  return !held_fences_.empty();
}

bool ManagedKernelAdmissionGate::exact_fence_is_held(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope) const {
  std::lock_guard lock(*admission_mutex_);
  return !fence_id.empty() &&
         std::any_of(held_fences_.begin(), held_fences_.end(),
                     [&](const auto &held) {
                       return held.fence_id == fence_id && held.scope == scope;
                     });
}

ManagedKernelAdmissionObservation ManagedKernelAdmissionGate::Observe(
    const core::ConfigurationFenceScopeDTO &scope) const {
  Require(scope.valid(), "admission observation scope is invalid");
  std::lock_guard lock(*admission_mutex_);
  const auto table = resource_table_.lock();
  Require(table != nullptr || !resource_table_was_bound_,
          "the bound Kernel resource table is no longer available");
  if (table == nullptr) {
    return {0U, {}, 0U, 0U, {}};
  }
  const auto snapshot = table->admission_snapshot(scope.resources);
  return {snapshot.operation_resource_generation,
          snapshot.active_control_configurations,
          snapshot.affected_queued_operation_count,
          snapshot.affected_active_operation_count,
          snapshot.route_published_active_control_configurations};
}

void ManagedKernelAdmissionGate::AcquireFenceDurably(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope,
    const std::uint64_t expected_operation_resource_generation,
    const std::vector<core::DomainActiveControlConfigurationDTO>
        &expected_active_control_configurations,
    const std::function<void()> &commit_durable_fence,
    const std::function<void(const core::ConfigurationFenceId &,
                             const core::ConfigurationFenceScopeDTO &)>
        &validate_terminal_overlap) {
  Require(!fence_id.empty() && scope.valid(),
          "fence acquisition identity or scope is invalid");
  ValidateConfigurations(expected_active_control_configurations);
  Require(static_cast<bool>(commit_durable_fence),
          "fence acquisition durable commit is absent");
  std::lock_guard lock(*admission_mutex_);
  const auto table = resource_table_.lock();
  Require(table != nullptr || !resource_table_was_bound_,
          "the bound Kernel resource table is no longer available");
  const auto observation = table != nullptr
                               ? table->admission_snapshot(scope.resources)
                               : core::ResourceSlotAdmissionSnapshotDTO{};
  Require(observation.operation_resource_generation ==
                  expected_operation_resource_generation &&
              observation.affected_queued_operation_count == 0U &&
              observation.affected_active_operation_count == 0U,
          "affected operation admission is busy or changed before fence "
          "acquisition");
  Require(SelectConfigurations(
              observation.active_control_configurations,
              observation.route_published_active_control_configurations,
              scope) == expected_active_control_configurations,
          "active control configuration changed before fence acquisition");
  for (const auto &held : held_fences_) {
    if (!ScopesOverlap(held.scope, scope))
      continue;
    Require(held.active_effect_count == 0U &&
                static_cast<bool>(validate_terminal_overlap),
            "an overlapping configuration fence already closes operation "
            "admission");
    validate_terminal_overlap(held.fence_id, held.scope);
  }
  held_fences_.push_back({fence_id, scope});
  // The ledger row is recorded under the same admission mutex, so an
  // operation admission sees either neither or both.
  commit_durable_fence();
}

void ManagedKernelAdmissionGate::ReleaseFence(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope) {
  Require(!fence_id.empty() && scope.valid(),
          "released fence identity or scope is invalid");
  std::lock_guard lock(*admission_mutex_);
  const auto found = std::find_if(
      held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
        return held.fence_id == fence_id && held.scope == scope;
      });
  Require(found != held_fences_.end(),
          "released fence is not held by the admission gate");
  Require(found->active_effect_count == 0U,
          "released fence still guards a runtime-composition effect");
  held_fences_.erase(found);
}

void ManagedKernelAdmissionGate::ReleaseFenceDurably(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope,
    const std::function<void()> &commit_durable_release) {
  Require(!fence_id.empty() && scope.valid() &&
              static_cast<bool>(commit_durable_release),
          "durable fence release identity, scope or commit is invalid");
  std::lock_guard lock(*admission_mutex_);
  const auto found = std::find_if(
      held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
        return held.fence_id == fence_id && held.scope == scope;
      });
  Require(
      found != held_fences_.end() && found->active_effect_count == 0U,
      "durable release names an absent fence or an active composition effect");
  commit_durable_release();
  held_fences_.erase(found);
}

void ManagedKernelAdmissionGate::RunWithExactHeldFence(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope,
    const std::function<void()> &effect) {
  Require(!fence_id.empty() && scope.valid() && static_cast<bool>(effect),
          "fenced runtime-composition effect is invalid");
  {
    std::lock_guard lock(*admission_mutex_);
    const auto found = std::find_if(
        held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
          return held.fence_id == fence_id && held.scope == scope;
        });
    Require(found != held_fences_.end(),
            "runtime composition does not name the exact held fence");
    Require(found->active_effect_count <
                std::numeric_limits<std::size_t>::max(),
            "runtime-composition fence guard count overflowed");
    ++found->active_effect_count;
  }
  const auto release_guard = [&] {
    std::lock_guard lock(*admission_mutex_);
    const auto found = std::find_if(
        held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
          return held.fence_id == fence_id && held.scope == scope;
        });
    Require(found != held_fences_.end() && found->active_effect_count != 0U,
            "runtime-composition fence guard was lost");
    --found->active_effect_count;
  };
  try {
    effect();
  } catch (...) {
    release_guard();
    throw;
  }
  release_guard();
}

class ManagedKernelApplyFenceOwner::Impl final {
public:
  Impl(ManagedKernelApplyFenceOwnerConfig config,
       ManagedKernelAdmissionGate &admission_gate)
      : config_(ValidateConfig(std::move(config))),
        admission_gate_(admission_gate) {
    // The bound is allocated once; acquire, eviction and release never grow it.
    fences_.reserve(kMaximumFenceRecords);
  }

  [[nodiscard]] core::ConfigurationFenceAdmissionSnapshotDTO ObserveAdmission(
      const core::ConfigurationFenceAdmissionQueryDTO &query) const {
    std::lock_guard lock(mutex_);
    Require(query.valid(), "admission observation query is invalid");
    const auto active = ReadActivePublication();
    const auto observation = admission_gate_.Observe(query.affected_scope);
    const auto configurations = SelectConfigurations(
        observation.active_control_configurations,
        observation.route_published_active_control_configurations,
        query.affected_scope);
    const auto epoch = ExecutionHostEpoch(active);
    core::ConfigurationFenceAdmissionSnapshotDTO snapshot{
        query,
        epoch,
        active.request.candidate_projection_revision,
        active.durable_public_pointer_content_digest,
        observation.operation_resource_generation,
        core::ConfigurationFenceGeneration(fence_generation_),
        configurations,
        observation.affected_queued_operation_count,
        observation.affected_active_operation_count,
        Digest("manyforge.kernel-fence-admission-observation.seed.v1")};
    snapshot.result_digest = core::ContentDigest(
        content_identity::sha256_digest(EncodeResponse(snapshot)));
    Require(snapshot.presence_valid(),
            "constructed admission observation is invalid");
    return snapshot;
  }

  [[nodiscard]] core::ConfigurationFenceAcceptedDTO
  Acquire(const core::ApplyConfigurationFenceIntentDTO &intent,
          const ManagedTerminalApplyFenceValidator &validate_terminal_overlap =
              {}) {
    std::lock_guard lock(mutex_);
    Require(intent.valid(), "Apply-fence intent is invalid");
    auto intent_bytes = EncodeRequest(intent);
    if (const auto *existing =
            FindByRequest(intent.owner_request_context.request_key)) {
      Require(existing->intent_bytes == intent_bytes && existing->state == 0,
              "request ID is bound to another or already released fence "
              "outcome");
      auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
          existing->accepted_bytes);
      accepted.replay_status = core::RequestReplayStatusDTO::kReplayedExact;
      Require(accepted.matches(intent),
              "durable exact-replay acceptance contradicts its intent");
      return accepted;
    }
    // The ledger is bounded by eviction, never refusal: a full ledger drops
    // its oldest released fence when this one is inserted (see below). Only
    // a ledger full of held fences refuses.
    const bool ledger_full = fences_.size() >= kMaximumFenceRecords;
    Require(!ledger_full ||
                std::any_of(fences_.begin(), fences_.end(),
                            [](const auto &row) { return row.state == 1; }),
            "durable fence ledger is full of held fences");

    const auto observed = admission_gate_.Observe(intent.scope);
    Require(observed.operation_resource_generation ==
                    intent.expected_operation_resource_generation &&
                observed.affected_queued_operation_count == 0U &&
                observed.affected_active_operation_count == 0U,
            "affected operation admission is busy or changed");
    const auto active_configurations = SelectConfigurations(
        observed.active_control_configurations,
        observed.route_published_active_control_configurations, intent.scope);
    Require(active_configurations ==
                intent.expected_active_control_configurations,
            "active control configuration changed before fence acquisition");

    const auto predecessor_generation = fence_generation_;
    Require(
        predecessor_generation == intent.expected_fence_generation.value() &&
            predecessor_generation < std::numeric_limits<std::uint64_t>::max(),
        "configuration-fence generation changed before acquisition");
    const auto next_generation = predecessor_generation + 1U;
    core::ConfigurationFenceAcceptedDTO accepted{
        core::ConfigurationFenceId(FenceIdFor(intent, next_generation)),
        core::ConfigurationFenceGeneration(next_generation), intent.scope,
        config_.now_ns(), core::RequestReplayStatusDTO::kAcceptedNew};
    Require(accepted.matches(intent),
            "constructed Apply-fence acceptance is invalid");
    // Built before the admission mutex is taken; the commit below only moves.
    FenceRow row{intent.owner_transaction,
                 intent.owner_request_context.request_key,
                 std::move(intent_bytes),
                 EncodeResponse(accepted),
                 accepted.configuration_fence_id,
                 next_generation,
                 0,
                 std::nullopt,
                 std::nullopt};

    admission_gate_.AcquireFenceDurably(
        accepted.configuration_fence_id, accepted.captured_scope,
        intent.expected_operation_resource_generation,
        intent.expected_active_control_configurations,
        [&] {
          if (ledger_full) {
            // The oldest released fence goes; held fences are never evicted.
            fences_.erase(std::find_if(
                fences_.begin(), fences_.end(),
                [](const auto &candidate) { return candidate.state == 1; }));
          }
          fences_.push_back(std::move(row));
          fence_generation_ = next_generation;
        },
        validate_terminal_overlap
            ? std::function<void(const core::ConfigurationFenceId &,
                                 const core::ConfigurationFenceScopeDTO
                                     &)>{[&](const auto &id,
                                             const auto &scope) {
                const auto old = std::find_if(
                    fences_.begin(), fences_.end(),
                    [&](const auto &candidate) {
                      return candidate.fence_id == id;
                    });
                Require(
                    old != fences_.end() && old->state == 0,
                    "overlapping barrier is not a held Apply fence");
                const auto old_intent =
                    DecodeRequest<core::ApplyConfigurationFenceIntentDTO>(
                        old->intent_bytes);
                const auto old_accepted =
                    DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
                        old->accepted_bytes);
                Require(old_accepted.matches(old_intent) &&
                            old_accepted.configuration_fence_id == id &&
                            old_accepted.captured_scope == scope,
                        "overlapping Apply fence differs from its durable "
                        "exact scope");
                const auto validated =
                    validate_terminal_overlap(old_intent, old_accepted);
                if (!validated.ok())
                  Fail(validated.error().code + ": " +
                       validated.error().message);
              }}
            : std::function<void(const core::ConfigurationFenceId &,
                                 const core::ConfigurationFenceScopeDTO &)>{});
    return accepted;
  }

  [[nodiscard]] core::ApplyFenceAcquireAttemptReconcileResultDTO
  ReconcileAcquireAttempt(
      const core::ApplyFenceAcquireAttemptQueryDTO &query) const {
    std::lock_guard lock(mutex_);
    Require(query.valid(), "fence-attempt reconciliation query is invalid");
    const auto active = ReadActivePublication();
    const auto epoch = ExecutionHostEpoch(active);
    const auto intent_bytes = EncodeRequest(query.persisted_intent);
    const auto *existing =
        FindByRequest(query.persisted_intent.owner_request_context.request_key);
    core::ApplyFenceAcquireAttemptReconcileResultDTO result{
        epoch,
        query,
        core::ApplyFenceAcquireAttemptReconcileState::kNotSeen,
        std::nullopt,
        false,
        {},
        Digest("manyforge.kernel-fence-attempt-reconciliation.seed.v1"),
        std::nullopt};
    if (existing != nullptr) {
      if (existing->intent_bytes == intent_bytes && existing->state == 0) {
        result.state =
            core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact;
        result.accepted_fence =
            DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
                existing->accepted_bytes);
        result.admission_fenced = true;
      } else {
        result.state =
            core::ApplyFenceAcquireAttemptReconcileState::kConflicting;
        result.admission_fenced = true;
        result.required_compensation_or_operator_direction =
            "keep affected admission closed and reconcile the exact durable "
            "Kernel request ledger";
        result.error = core::ErrorDTO{
            "configuration_fence_request_conflict",
            "the durable request identity names another or already released "
            "fence outcome",
            {}};
      }
    }
    result.result_digest = core::ContentDigest(
        content_identity::sha256_digest(EncodeResponse(result)));
    Require(result.presence_valid(),
            "constructed attempt reconciliation is invalid");
    return result;
  }

  [[nodiscard]] core::ConfigurationFenceStatusDTO
  QueryStatus(const core::ConfigurationFenceStatusQueryDTO &query) const {
    std::lock_guard lock(mutex_);
    return QueryStatusLocked(query);
  }

  [[nodiscard]] std::optional<core::ConfigurationFenceStatusQueryDTO>
  HeldApplyFence(const core::NamedApplyPublicationTransactionDTO &apply) const {
    std::lock_guard lock(mutex_);
    Require(apply.valid(), "held Apply-fence lookup is invalid");
    const auto row =
        std::find_if(fences_.begin(), fences_.end(), [&](const auto &candidate) {
          return candidate.owner.apply_id == apply.apply_id &&
                 candidate.owner.apply_generation == apply.apply_generation &&
                 candidate.state == 0;
        });
    if (row == fences_.end())
      return std::nullopt;
    const auto accepted =
        DecodeResponse<core::ConfigurationFenceAcceptedDTO>(row->accepted_bytes);
    return core::ConfigurationFenceStatusQueryDTO{
        {apply.apply_id, apply.apply_generation},
        accepted.configuration_fence_id,
        accepted.configuration_fence_generation,
        accepted.captured_scope};
  }

  [[nodiscard]] std::vector<core::ConfigurationFenceStatusQueryDTO>
  HeldApplyFences() const {
    std::lock_guard lock(mutex_);
    std::vector<core::ConfigurationFenceStatusQueryDTO> held;
    for (const auto &row : fences_) {
      if (row.state != 0)
        continue;
      const auto accepted =
          DecodeResponse<core::ConfigurationFenceAcceptedDTO>(row.accepted_bytes);
      held.push_back({row.owner, accepted.configuration_fence_id,
                      accepted.configuration_fence_generation,
                      accepted.captured_scope});
    }
    return held;
  }

  void RunWithExactHeldPredecessor(
      const core::ConfigurationFenceStatusQueryDTO &query,
      const std::function<void()> &effect) const {
    std::lock_guard lock(mutex_);
    Require(query.valid() && static_cast<bool>(effect),
            "fenced runtime-composition installation request is invalid");
    const auto *row = FindByFence(query.configuration_fence_id,
                                  query.configuration_fence_generation);
    Require(row != nullptr && row->state == 0,
            "runtime composition does not name a durable held fence");
    const auto intent = DecodeRequest<core::ApplyConfigurationFenceIntentDTO>(
        row->intent_bytes);
    const auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
        row->accepted_bytes);
    Require(intent.owner_transaction == query.owner_transaction &&
                accepted.configuration_fence_id ==
                    query.configuration_fence_id &&
                accepted.configuration_fence_generation ==
                    query.configuration_fence_generation &&
                accepted.captured_scope == query.expected_scope,
            "runtime composition conflicts with the durable held fence");
    admission_gate_.RunWithExactHeldFence(query.configuration_fence_id,
                                          query.expected_scope, effect);
  }

  [[nodiscard]] core::ConfigurationFenceReleasedDTO
  Release(const core::ConfigurationFenceReleaseDTO &request) {
    std::lock_guard lock(mutex_);
    Require(request.valid(), "fence release request is invalid");
    const auto &evidence = request.reconciliation_evidence;
    auto *row = FindByFence(evidence.configuration_fence_id,
                            evidence.configuration_fence_generation);
    Require(row != nullptr,
            "released fence is absent from the durable ledger");
    if (row->state == 1) {
      Require(*row->release_request_bytes == EncodeRequest(request),
              "exact release replay conflicts with the durable receipt");
      return DecodeResponse<core::ConfigurationFenceReleasedDTO>(
          *row->release_receipt_bytes);
    }
    const core::ConfigurationFenceStatusQueryDTO query{
        evidence.owner_transaction, evidence.configuration_fence_id,
        evidence.configuration_fence_generation, evidence.reconciled_scope};
    const auto status = QueryStatusLocked(query);
    Require(
        status.state == core::ConfigurationFenceState::kHeld &&
            status.reconciliation_evidence == evidence &&
            status.reconciliation_evidence.affected_queued_operation_count ==
                0U &&
            status.reconciliation_evidence.affected_active_operation_count ==
                0U,
        "fence release evidence is stale or affected work remains");
    core::ConfigurationFenceReleasedDTO released{
        request, evidence.execution_host_epoch, config_.now_ns(),
        core::RequestReplayStatusDTO::kAcceptedNew,
        Digest("manyforge.kernel-fence-release-receipt.v1",
               EncodeRequest(request),
               evidence.execution_host_epoch.durable_stop_fence_ledger_digest
                   .value())};
    Require(released.valid(), "constructed fence release receipt is invalid");
    auto request_bytes = EncodeRequest(request);
    auto receipt_bytes = EncodeResponse(released);
    row->state = 1;
    row->release_request_bytes = std::move(request_bytes);
    row->release_receipt_bytes = std::move(receipt_bytes);
    admission_gate_.ReleaseFence(evidence.configuration_fence_id,
                                 evidence.reconciled_scope);
    return released;
  }

  [[nodiscard]] static ManagedKernelApplyFenceOwnerConfig
  ValidateConfig(ManagedKernelApplyFenceOwnerConfig config) {
    Require(!config.cell_id.empty() && !config.runtime_unit_id.empty() &&
                !config.runtime_epoch.empty() &&
                static_cast<bool>(config.now_ns) &&
                static_cast<bool>(config.active_publication),
            "owner construction identities or clock are invalid");
    return config;
  }

  [[nodiscard]] const FenceRow *
  FindByRequest(const core::RequestKey &request_key) const {
    const auto found =
        std::find_if(fences_.begin(), fences_.end(), [&](const auto &row) {
          return row.request_key == request_key;
        });
    return found == fences_.end() ? nullptr : &*found;
  }

  [[nodiscard]] const FenceRow *
  FindByFence(const core::ConfigurationFenceId &fence_id,
              const core::ConfigurationFenceGeneration generation) const {
    const auto found =
        std::find_if(fences_.begin(), fences_.end(), [&](const auto &row) {
          return row.fence_id == fence_id &&
                 row.fence_generation == generation.value();
        });
    return found == fences_.end() ? nullptr : &*found;
  }

  [[nodiscard]] FenceRow *
  FindByFence(const core::ConfigurationFenceId &fence_id,
              const core::ConfigurationFenceGeneration generation) {
    return const_cast<FenceRow *>(
        std::as_const(*this).FindByFence(fence_id, generation));
  }

  // The rows in fence-generation order, the order they are appended in.
  [[nodiscard]] core::ContentDigest LedgerDigest() const {
    std::string bytes;
    AppendField(bytes, kLedgerDigestDomain);
    AppendField(bytes, config_.cell_id.value());
    AppendField(bytes, std::to_string(fence_generation_));
    for (const auto &row : fences_) {
      AppendField(bytes, row.intent_bytes);
      AppendField(bytes, row.accepted_bytes);
      AppendField(bytes, std::to_string(row.state));
      AppendField(bytes, row.release_request_bytes.value_or(""));
      AppendField(bytes, row.release_receipt_bytes.value_or(""));
    }
    return core::ContentDigest(content_identity::sha256_digest(bytes));
  }

  // Memory only: the host's slot, safe under mutex_.
  [[nodiscard]] core::ActiveBundlePublicationReceiptDTO ReadActivePublication() const {
    return config_.active_publication();
  }

  [[nodiscard]] core::ExecutionHostEpochDTO ExecutionHostEpoch(
      const core::ActiveBundlePublicationReceiptDTO &publication) const {
    core::ExecutionHostEpochDTO epoch{
        config_.runtime_unit_id, config_.runtime_epoch, LedgerDigest(),
        publication.publication_generation};
    Require(epoch.valid(), "constructed execution-host epoch is invalid");
    return epoch;
  }

  [[nodiscard]] core::ConfigurationFenceStatusDTO
  QueryStatusLocked(const core::ConfigurationFenceStatusQueryDTO &query) const {
    Require(query.valid(), "fence status query is invalid");
    const auto *row = FindByFence(query.configuration_fence_id,
                                  query.configuration_fence_generation);
    Require(row != nullptr, "fence status identity is unknown");
    const auto intent = DecodeRequest<core::ApplyConfigurationFenceIntentDTO>(
        row->intent_bytes);
    const auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
        row->accepted_bytes);
    Require(intent.owner_transaction == query.owner_transaction &&
                accepted.configuration_fence_id ==
                    query.configuration_fence_id &&
                accepted.configuration_fence_generation ==
                    query.configuration_fence_generation &&
                accepted.captured_scope == query.expected_scope,
            "fence status query conflicts with the durable fence");
    if (row->state == 1) {
      const auto released = DecodeResponse<core::ConfigurationFenceReleasedDTO>(
          *row->release_receipt_bytes);
      core::ConfigurationFenceStatusDTO status{
          query,
          released.request.reconciliation_evidence,
          core::ConfigurationFenceState::kReleased,
          false,
          released,
          {},
          Digest("manyforge.kernel-fence-status.v1", EncodeRequest(query),
                 released.release_receipt_digest.value()),
          std::nullopt};
      Require(status.presence_valid(),
              "constructed released fence status is invalid");
      return status;
    }

    const auto active = ReadActivePublication();
    const auto observation = admission_gate_.Observe(query.expected_scope);
    const auto configurations = ObservedConfigurations(
        observation.active_control_configurations, query.expected_scope);
    const auto epoch = ExecutionHostEpoch(active);
    core::ConfigurationFenceReconciliationEvidenceDTO evidence{
        epoch,
        query.owner_transaction,
        query.configuration_fence_id,
        query.configuration_fence_generation,
        query.expected_scope,
        observation.operation_resource_generation,
        active.request.candidate_graph_revision,
        active.request.candidate_projection_revision,
        active.publication_generation,
        active.durable_public_pointer_content_digest,
        configurations,
        observation.affected_queued_operation_count,
        observation.affected_active_operation_count,
        Digest("manyforge.kernel-fence-reconciliation-evidence.seed.v1")};
    Require(evidence.valid(),
            "constructed fence reconciliation evidence is invalid");
    core::ConfigurationFenceStatusDTO status{
        query,
        evidence,
        core::ConfigurationFenceState::kHeld,
        true,
        std::nullopt,
        {},
        Digest("manyforge.kernel-fence-status.seed.v1"),
        std::nullopt};
    evidence.evidence_digest = core::ContentDigest(
        content_identity::sha256_digest(EncodeResponse(status)));
    status.reconciliation_evidence = evidence;
    status.result_digest = core::ContentDigest(
        content_identity::sha256_digest(EncodeResponse(status)));
    Require(status.presence_valid(),
            "constructed held fence status is invalid");
    return status;
  }

  ManagedKernelApplyFenceOwnerConfig config_;
  ManagedKernelAdmissionGate &admission_gate_;
  mutable std::mutex mutex_;
  // The ledger, guarded by mutex_: at most kMaximumFenceRecords rows in
  // fence-generation order, and the last generation handed out.
  std::vector<FenceRow> fences_;
  std::uint64_t fence_generation_{0U};
};

ManagedKernelApplyFenceOwner::ManagedKernelApplyFenceOwner(
    ManagedKernelApplyFenceOwnerConfig config,
    ManagedKernelAdmissionGate &admission_gate)
    : impl_(std::make_unique<Impl>(std::move(config), admission_gate)) {}

ManagedKernelApplyFenceOwner::~ManagedKernelApplyFenceOwner() = default;

core::ConfigurationFenceAdmissionSnapshotDTO
ManagedKernelApplyFenceOwner::ObserveAdmission(
    const core::ConfigurationFenceAdmissionQueryDTO &query) const {
  return impl_->ObserveAdmission(query);
}

core::ConfigurationFenceAcceptedDTO ManagedKernelApplyFenceOwner::Acquire(
    const core::ApplyConfigurationFenceIntentDTO &intent) {
  return impl_->Acquire(intent);
}

core::ConfigurationFenceAcceptedDTO ManagedKernelApplyFenceOwner::Acquire(
    const core::ApplyConfigurationFenceIntentDTO &intent,
    const ManagedTerminalApplyFenceValidator &validate_terminal_overlap) {
  return impl_->Acquire(intent, validate_terminal_overlap);
}

core::ApplyFenceAcquireAttemptReconcileResultDTO
ManagedKernelApplyFenceOwner::ReconcileAcquireAttempt(
    const core::ApplyFenceAcquireAttemptQueryDTO &query) const {
  return impl_->ReconcileAcquireAttempt(query);
}

core::ConfigurationFenceStatusDTO ManagedKernelApplyFenceOwner::QueryStatus(
    const core::ConfigurationFenceStatusQueryDTO &query) const {
  return impl_->QueryStatus(query);
}

std::optional<core::ConfigurationFenceStatusQueryDTO>
ManagedKernelApplyFenceOwner::HeldApplyFence(
    const core::NamedApplyPublicationTransactionDTO &apply) const {
  return impl_->HeldApplyFence(apply);
}

std::vector<core::ConfigurationFenceStatusQueryDTO>
ManagedKernelApplyFenceOwner::HeldApplyFences() const {
  return impl_->HeldApplyFences();
}

void ManagedKernelApplyFenceOwner::RunWithExactHeldPredecessor(
    const core::ConfigurationFenceStatusQueryDTO &query,
    const std::function<void()> &effect) const {
  impl_->RunWithExactHeldPredecessor(query, effect);
}

core::ConfigurationFenceReleasedDTO ManagedKernelApplyFenceOwner::Release(
    const core::ConfigurationFenceReleaseDTO &request) {
  return impl_->Release(request);
}

} // namespace manyforge::roboplan_backend
