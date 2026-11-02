#pragma once

#include <manyforge_component_manager/apply_completion.hpp>
#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_core/types/component_state.hpp>
#include <manyforge_core/types/typed_error.hpp>

#include <cstddef>
#include <exception>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::roboplan_backend::host_detail {

int RunExecutionHostMain(int argc, char **argv);

// The liveness lane owns `observed`. A drain first observed while the host is
// exiting does not turn its normal shutdown into a Manager-loss incident.
// A fault observed during service remains latched throughout teardown.
// A miss recorded just before shutdown but first observed afterward is treated
// as shutdown-only as well.
template <typename Renew, typename Refuse>
auto DispatchHeartbeatForDrain(const char *first_stuck, bool shutting_down,
                               const char *&observed, Renew &&renew,
                               Refuse &&refuse) {
  if (observed == nullptr && !shutting_down)
    observed = first_stuck;
  return observed != nullptr ? refuse(observed) : renew();
}

/// `text` cut to at most `bytes` bytes on a UTF-8 character boundary.
[[nodiscard]] inline std::string BoundedUtf8(const std::string_view text,
                                             const std::size_t bytes) {
  if (text.size() <= bytes)
    return std::string(text);
  auto end = bytes;
  while (end > 0U && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U)
    --end;
  return std::string(text.substr(0U, end));
}

/// CONTRACTS §11, §15 (N28): whether a replaced or removed component keeps
/// the host Apply from going ahead. An `enabled` one does, unless its latest
/// action is a failed Disable (the Apply's own, `apply_orchestration`), no
/// action runs and it isn't moving: that one is stopped like a dead one.
[[nodiscard]] inline bool
LeavingComponentRefusesApply(const manyforge::core::ComponentState &state) {
  using manyforge::core::ActionOutcome;
  using manyforge::core::ComponentAction;
  const bool disable_failed = state.last_action &&
                              state.last_action->action == ComponentAction::kDisable &&
                              state.last_action->outcome == ActionOutcome::kFailed;
  return state.lifecycle == manyforge::core::ComponentLifecycle::kEnabled &&
         !(disable_failed && !state.action &&
           state.activity != manyforge::core::ComponentActivity::kBusy);
}

/// A component's site address that can't be resolved (no site settings file,
/// unknown `device_address_ref`): the resolver's typed error, naming the
/// component in its message and `details.component_id`.
[[nodiscard]] inline manyforge::core::TypedError
SiteAddressRefusal(const manyforge::core::ErrorDTO &error,
                   const std::string &component_id) {
  auto details = error.details;
  details.insert_or_assign("component_id", component_id);
  return {error.code, "component " + component_id + ": " + error.message,
          std::move(details),
          std::string(manyforge::component_manager::kExecutionHostProducer)};
}

/// The typed refusal of a candidate whose composition found a data or setup
/// inconsistency. `details.cause` keeps the original message, `cause_code`
/// a typed cause's code, and `component_id` the component being composed
/// when known (else the typed cause's own); all fit the Manager's retained
/// failure bounds.
[[nodiscard]] inline manyforge::core::TypedError
ScenarioCompositionRefusal(const std::exception &error,
                           const std::string &component_id = {}) {
  namespace component_manager = manyforge::component_manager;
  const std::string cause = error.what();
  std::map<std::string, std::string> details{
      {"cause",
       BoundedUtf8(cause, component_manager::
                              kMaximumManagerApplyFailureCauseDetailValueBytes)}};
  const auto *typed = dynamic_cast<const manyforge::core::TypedError *>(&error);
  std::string component = component_id;
  if (component.empty() && typed != nullptr) {
    const auto named = typed->details().find("component_id");
    if (named != typed->details().end())
      component = named->second;
  }
  if (!component.empty())
    details.emplace("component_id",
                    BoundedUtf8(component,
                                component_manager::
                                    kMaximumManagerApplyFailureCauseDetailValueBytes));
  if (typed != nullptr)
    details.emplace("cause_code", typed->code());
  return {std::string(component_manager::kScenarioCompositionRefusedCode),
          BoundedUtf8("Scenario refused: " + cause,
                      component_manager::kMaximumManagerApplyFailureMessageBytes),
          std::move(details),
          std::string(component_manager::kExecutionHostProducer)};
}

/// Whether a stored composition failure is that refusal. It refuses only its
/// own Apply; the runtime process keeps running.
[[nodiscard]] inline bool
IsScenarioCompositionRefusal(const std::exception_ptr &failure) {
  if (failure == nullptr)
    return false;
  try {
    std::rethrow_exception(failure);
  } catch (const manyforge::core::TypedError &error) {
    return error.code() ==
           manyforge::component_manager::kScenarioCompositionRefusedCode;
  } catch (...) {
    return false;
  }
}

/// The selected-composition RPC error for one caught exception: the refusal
/// keeps its typed code and cause; anything else is the generic failure.
[[nodiscard]] inline manyforge::core::ErrorDTO
SelectedRuntimeCompositionRpcError(const std::exception &error) {
  if (const auto *typed = dynamic_cast<const manyforge::core::TypedError *>(&error);
      typed != nullptr &&
      typed->code() == manyforge::component_manager::kScenarioCompositionRefusedCode)
    return typed->error();
  return {"selected_runtime_composition_request_failed", error.what(), {}};
}

/// The failed host Apply result for a failure caught before the Apply
/// changed anything installed: the RPC error of `failure` with
/// `details.effect = none`, so the Manager releases the Apply's fence and
/// claim (the predecessor is still what is installed).
[[nodiscard]] inline manyforge::core::ErrorDTO
HostApplyFailureBeforeEffect(const std::exception_ptr &failure) {
  manyforge::core::ErrorDTO error;
  try {
    std::rethrow_exception(failure);
  } catch (const std::exception &caught) {
    error = SelectedRuntimeCompositionRpcError(caught);
  } catch (...) {
    error = {"selected_runtime_composition_request_failed",
             "selected runtime-composition request failed", {}};
  }
  error.message = BoundedUtf8(
      error.message, manyforge::component_manager::kMaximumManagerApplyFailureMessageBytes);
  error.details.insert_or_assign("effect", "none");
  return error;
}

} // namespace manyforge::roboplan_backend::host_detail
