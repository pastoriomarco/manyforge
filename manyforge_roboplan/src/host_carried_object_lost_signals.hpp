#pragma once

#include <manyforge_core/execution_event_change_signal.hpp>
#include <manyforge_core/types/io_signal.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend::host_detail {

/// Process-wide level of the compiled `carried_object_lost.<target>` system
/// signals of this execution host. The Manager publishes
/// each level through the control socket; the level outlives a composition, so
/// a re-Apply keeps it. One row per lost attachment (a gripper can carry
/// several, each with its own `set_key`): a row exists from the loss
/// until the gripper's level is published `false` (next attach, Program
/// detach, cell Reset), which removes all of that gripper's rows. Any row of a
/// gripper means its level is `true`. No cap and no eviction; the bound:
/// every attach by a gripper clears that gripper's rows (the Manager
/// publishes `false`), and a committed composition replacement drops the rows
/// of gripper targets the new composition does not hold (`InstallSink`, called
/// only at the terminal publication proof; an Apply candidate only `Seed`s, so
/// a refused candidate changes nothing).
///
/// Ordering: every store update and its sink delivery, and a sink's
/// installation with its initial levels, run under the one store mutex, so
/// the composed source sees the levels in the order they were recorded. The
/// sink is non-blocking (it enqueues to the host ingress) and never re-enters
/// the store.
class HostCarriedObjectLostSignals final {
public:
  struct Loss final {
    core::ControllableComponentId controllable_component_id;
    core::CommandTargetId command_target_id;
    std::string carried_object_attachment_id;
    std::string scene_resource_id;
    /// The attachment's stored `on_grasp_lost.set_key`, and
    /// the Manager's loss `event_id`, kept for the whole continuing loss.
    std::optional<std::string> set_key{};
    std::string loss_event_id{};
  };
  /// Coalescing: the last loss of a gripper per
  /// `set_key`. Set by each new loss naming that key, replaced only by the
  /// next one of that gripper with the same key; a `false` (Program detach,
  /// re-attach, Reset) never clears it; a committed composition that no longer
  /// holds the gripper drops it.
  struct LastLoss final {
    core::ControllableComponentId controllable_component_id;
    core::CommandTargetId command_target_id;
    std::string set_key;
    std::string loss_event_id;
  };
  /// Delivers one level to the current composition's in-process source.
  using Sink = std::function<std::optional<core::ErrorDTO>(
      const core::IoSignalId &, bool)>;
  /// Applies the initial `true` levels of a newly installed sink.
  using SeedLevels = std::function<void(const std::vector<core::IoSignalId> &)>;

  [[nodiscard]] static core::IoSignalId
  SignalId(const core::ControllableComponentId &component,
           const core::CommandTargetId &target) {
    return {component, deployment::CarriedObjectLostSignalName(target.value())};
  }

  /// Records the gripper's level (`loss` present = `true`: adds that
  /// attachment's row once; a repeated detection of the same continuing loss
  /// keeps the row and its `loss_event_id`; `false`: removes all of the
  /// gripper's rows) and
  /// delivers it to the current sink; `false` is always delivered, also for a
  /// gripper with no row. The recorded level stays when no sink is installed
  /// or it refuses; the refusal is returned.
  std::optional<core::ErrorDTO>
  Publish(const core::ControllableComponentId &component,
          const core::CommandTargetId &target, std::optional<Loss> loss) {
    const bool level = loss.has_value();
    std::optional<core::ErrorDTO> refused;
    {
      std::lock_guard lock(mutex_);
      const auto same_gripper = [&](const Loss &row) {
        return row.controllable_component_id == component &&
               row.command_target_id == target;
      };
      if (!loss) {
        std::erase_if(losses_, same_gripper);
      } else if (std::none_of(losses_.begin(), losses_.end(), [&](const Loss &row) {
                   return same_gripper(row) && row.carried_object_attachment_id ==
                                                   loss->carried_object_attachment_id;
                 })) {
        // A new loss (a repeated detection of a continuing one keeps both
        // the row and the last loss).
        if (loss->set_key) {
          std::erase_if(last_losses_, [&](const LastLoss &last) {
            return last.controllable_component_id == component &&
                   last.command_target_id == target &&
                   last.set_key == *loss->set_key;
          });
          last_losses_.push_back(
              {component, target, *loss->set_key, loss->loss_event_id});
        }
        losses_.push_back(std::move(*loss));
      }
      refused = sink_ ? sink_(SignalId(component, target), level)
                      : std::optional<core::ErrorDTO>(core::ErrorDTO{
                            "io_signal_service_unavailable",
                            "No composed I/O service holds the system signal",
                            {}});
    }
    core::execution_event_change_signal().Notify();  // Rows for event streams.
    return refused;
  }

  /// Applies the current `true` levels to a starting composition (an Apply
  /// candidate too) in the serialized step; the sink and rows are unchanged.
  void Seed(const SeedLevels &seed) const {
    std::lock_guard lock(mutex_);
    SeedLocked(seed);
  }

  /// Installs the sink of the committed composition holding the system
  /// signals `held` (an empty sink: none): drops the rows of every other
  /// gripper, then applies, through `seed`, the levels that are `true`, in the
  /// same serialized step: a later Publish reaches this sink only after it.
  void InstallSink(Sink sink, const std::vector<core::IoSignalId> &held,
                   const SeedLevels &seed) {
    {
      std::lock_guard lock(mutex_);
      sink_ = std::move(sink);
      std::erase_if(losses_, [&](const Loss &row) {
        return std::find(held.begin(), held.end(),
                         SignalId(row.controllable_component_id,
                                  row.command_target_id)) == held.end();
      });
      std::erase_if(last_losses_, [&](const LastLoss &last) {
        return std::find(held.begin(), held.end(),
                         SignalId(last.controllable_component_id,
                                  last.command_target_id)) == held.end();
      });
      SeedLocked(seed);
    }
    core::execution_event_change_signal().Notify();  // Rows may be dropped.
  }

private:
  void SeedLocked(const SeedLevels &seed) const {
    std::vector<core::IoSignalId> lost;
    lost.reserve(losses_.size());
    for (const auto &row : losses_) {
      auto id = SignalId(row.controllable_component_id, row.command_target_id);
      if (std::find(lost.begin(), lost.end(), id) == lost.end())
        lost.push_back(std::move(id));
    }
    seed(lost);
  }

public:
  [[nodiscard]] bool Level(const core::IoSignalId &signal) const {
    std::lock_guard lock(mutex_);
    return std::any_of(losses_.begin(), losses_.end(), [&](const Loss &row) {
      return SignalId(row.controllable_component_id, row.command_target_id) ==
             signal;
    });
  }

  /// Sorted by robot, target, attachment.
  [[nodiscard]] std::vector<Loss> Losses() const {
    std::vector<Loss> result;
    {
      std::lock_guard lock(mutex_);
      result = losses_;
    }
    std::sort(result.begin(), result.end(), [](const Loss &a, const Loss &b) {
      return std::tie(a.controllable_component_id, a.command_target_id,
                      a.carried_object_attachment_id) <
             std::tie(b.controllable_component_id, b.command_target_id,
                      b.carried_object_attachment_id);
    });
    return result;
  }

  /// Sorted by robot, target, key, loss event.
  [[nodiscard]] std::vector<LastLoss> LastLosses() const {
    std::vector<LastLoss> result;
    {
      std::lock_guard lock(mutex_);
      result = last_losses_;
    }
    std::sort(result.begin(), result.end(), [](const LastLoss &a, const LastLoss &b) {
      return std::tie(a.controllable_component_id, a.command_target_id, a.set_key,
                      a.loss_event_id) <
             std::tie(b.controllable_component_id, b.command_target_id, b.set_key,
                      b.loss_event_id);
    });
    return result;
  }

private:
  mutable std::mutex mutex_;
  std::vector<Loss> losses_;
  std::vector<LastLoss> last_losses_;
  Sink sink_;
};

/// The one store of this execution host process.
inline HostCarriedObjectLostSignals &host_carried_object_lost_signals() {
  static HostCarriedObjectLostSignals signals;
  return signals;
}

} // namespace manyforge::roboplan_backend::host_detail
