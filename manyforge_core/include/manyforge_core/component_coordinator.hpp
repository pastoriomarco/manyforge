#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <manyforge_core/ports/icomponent_adapter_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/component_state.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::core {

/// Mapping from the 8-state lifecycle: undeclared/declared are
/// `not_prepared` (`offline` when the process or device isn't reachable),
/// prepared and disabling/disabled are `prepared`, active is `enabled`,
/// degraded/faulted are `fault`. Progress is the coordinator's `action`.
[[nodiscard]] ComponentLifecycle MapComponentLifecycleState(ComponentLifecycleStateDTO state,
                                                            bool reachable) noexcept;

/// Receives every component's state, in cell order, whenever one changed.
using ComponentStatesSink = std::function<void(const std::vector<ComponentState>&)>;

struct CoordinatedComponent final {
  std::string component_id;
  std::shared_ptr<IComponentAdapter> adapter;
  /// The owner (machine) of a member and its role; absent for
  /// owners and owner-less components.
  std::optional<std::string> owner_id{};
  std::optional<std::string> role{};
  /// The component is a machine's owner, also while every
  /// member is deactivated: no `enable`/`disable`; its `power_off`/`reboot`
  /// disable its members first.
  bool owner{};
};

/// Unmasked observations of the addressed component and its optional owner.
/// This carries no admission authority; the caller must bind it to the exact
/// installed composition and revalidate it at each effect.
struct ComponentRecoveryPreflight final {
  ComponentState member;
  std::optional<ComponentState> owner;
};

struct ComponentRecoveryFact final {
  std::string component_id;
  std::optional<std::string> cause_code;
  std::optional<std::string> vendor_code;
  std::string eligibility;
  std::optional<std::string> stop_code{};
  std::optional<std::string> stop_vendor_code{};
  friend bool operator==(const ComponentRecoveryFact&, const ComponentRecoveryFact&) = default;
};

[[nodiscard]] ComponentRecoveryFact RecoveryFact(const ComponentState& state);

/// The core component coordinator. Per component it runs
/// one action at a time on the component's adapter, holds the component's
/// `ComponentState` with its `action` and `last_action`, and publishes the
/// states on change. Actions are always available, during a cell latch and
/// during a run; nothing retries or resends an action.
class ComponentCoordinator final {
 public:
  ComponentCoordinator(IClock& clock, ComponentStatesSink publish);
  /// Interrupts every action in progress and waits for its adapter to return.
  ~ComponentCoordinator();

  ComponentCoordinator(const ComponentCoordinator&) = delete;
  ComponentCoordinator& operator=(const ComponentCoordinator&) = delete;

  /// The components in the cell, in cell order (Apply). A retained component
  /// keeps its action and `last_action`; a removed component's action in
  /// progress is interrupted and its result dropped. A member names its owner;
  /// the published state carries `owner_id` and `role`.
  /// Returns the removed components' action threads, interrupted: the caller
  /// destroys them (joins) after releasing its own locks, since an action may
  /// need them (e.g. a route opening inside `enable`).
  [[nodiscard]] std::vector<std::jthread> SetComponents(
      std::vector<CoordinatedComponent> components);

  /// Starts `action` on the component and returns the accepted action; the
  /// result arrives in `last_action`. Refused, before anything is dispatched,
  /// for a component that isn't in the cell (`component_not_found`), while
  /// another action of the component runs (`component_action_in_progress`),
  /// for an action that isn't one of the four lifecycle actions or a declared
  /// capability, or `enable`/`disable` on an owner (`component_action_unsupported`),
  /// and for a member's action while its owner isn't `prepared`
  /// (`owner_not_prepared`).
  ///
  /// An owner's `power_off`/`reboot` runs as a group:
  /// each member's `disable` first, in cell order, as that member's own
  /// action (its `action` and `last_action`), then the owner's action. A
  /// member's `disable` that doesn't succeed ends the group: the owner's
  /// result is that failure, the remaining members are left as they are.
  /// Refused (`component_action_in_progress`) while a member's action runs.
  [[nodiscard]] Result<ComponentActionInProgress> Submit(const std::string& component_id,
                                                         ComponentAction action);

  /// Reads owner and member observations without applying owner presentation.
  [[nodiscard]] Result<ComponentRecoveryPreflight> RecoveryPreflight(
      const std::string& component_id) const;

  /// Accept one confirmed owner/member recovery and enable transaction.
  /// `current` revalidates the host's exact captured composition without waiting.
  [[nodiscard]] Result<ComponentActionInProgress> SubmitRecoveryEnable(
      const std::string& component_id, std::vector<ComponentRecoveryFact> confirmed,
      std::function<bool()> current, std::function<bool()> owner_recovered = {});

  /// A new Stop: every action in progress is interrupted. Its result is
  /// `failed` with `component_action_interrupted_by_stop` unless the adapter
  /// reports it `succeeded` or `unknown`.
  void InterruptActions();

  /// Close action admission and wait for every accepted action to finish.
  [[nodiscard]] bool CloseAndDrainActions(std::chrono::steady_clock::time_point deadline);

  /// Reads every adapter's state and publishes when any component changed. A
  /// member whose owner isn't usable is published `offline` through it
  /// (`presented_member_state`).
  void Refresh();

  /// The latest state of every component, in cell order.
  [[nodiscard]] std::vector<ComponentState> States() const;
  /// Atomically reserve existing affected components for Apply, or name the
  /// first one whose existing action/reservation prevents it.
  [[nodiscard]] std::optional<std::string> ReserveForApply(
      const std::vector<std::string>& component_ids, std::vector<std::string>& reserved);
  void ReleaseApplyReservation(const std::vector<std::string>& component_ids) noexcept;

 private:
  struct Entry final {
    std::string component_id;
    std::shared_ptr<IComponentAdapter> adapter;
    std::optional<std::string> owner_id;
    std::optional<std::string> role;
    bool owner{};
    std::optional<ComponentActionInProgress> action;
    std::optional<ActionResult> last_action;
    bool interrupted{};
    std::jthread worker;
    bool composite_reserved{};
  };

  /// A member disabled before its owner's power action.
  struct GroupMember final {
    std::string component_id;
    std::shared_ptr<IComponentAdapter> adapter;
  };

  void RunAction(const std::string& component_id, const std::shared_ptr<IComponentAdapter>& adapter,
                 ComponentAction action, std::int64_t started_at_ns,
                 const std::vector<GroupMember>& members, const std::stop_token& stop,
                 const IComponentAdapter::PreparedRecovery& prepared_recovery);
  void RunRecoveryEnable(const std::string& component_id,
                         const std::shared_ptr<IComponentAdapter>& adapter,
                         const std::optional<std::string>& owner_id,
                         const std::shared_ptr<IComponentAdapter>& owner,
                         std::vector<ComponentRecoveryFact> confirmed,
                         std::function<bool()> current, std::function<bool()> owner_recovered,
                         std::int64_t started_at_ns,
                         std::stop_token stop);
  /// Ends the component's action with `result` (none: ended without running).
  void Finish(const std::string& component_id, const std::shared_ptr<IComponentAdapter>& adapter,
              std::optional<ActionResult> result);

  struct ActionCompletion final {
    ComponentCoordinator* coordinator;
    ~ActionCompletion();
  };

  IClock& clock_;
  ComponentStatesSink publish_;
  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
  std::vector<ComponentState> published_;
  bool closing_{};
  bool action_admission_closed_{};
  std::size_t actions_in_flight_{};
  std::condition_variable actions_drained_;
  /// Orders Refresh calls so publications never go back in time. The sink
  /// runs under it and must not call Submit or Refresh.
  std::mutex refresh_mutex_;
};

}  // namespace manyforge::core
