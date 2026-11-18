#include <algorithm>
#include <manyforge_core/stop_scope_resolver.hpp>
#include <type_traits>
#include <variant>

namespace manyforge::core {
namespace {

constexpr std::uint64_t kMaximumIJsonInteger = 9'007'199'254'740'991ULL;

template <typename Counter>
[[nodiscard]] bool safe_counter(const Counter& value) noexcept {
  return value.value() <= kMaximumIJsonInteger;
}

template <typename StrongId>
[[nodiscard]] bool bounded_identity(const StrongId& identity) noexcept {
  return !identity.empty() && identity.value().size() <= kMaximumStopIdentityBytes &&
         identity.value().find('\0') == std::string::npos;
}

template <typename Rows, typename Key>
[[nodiscard]] bool canonical_rows(const Rows& rows, Key&& key) {
  return std::is_sorted(
             rows.begin(), rows.end(),
             [&](const auto& left, const auto& right) { return key(left) < key(right); }) &&
         std::adjacent_find(rows.begin(), rows.end(), [&](const auto& left, const auto& right) {
           return key(left) == key(right);
         }) == rows.end();
}

[[nodiscard]] bool selector_scope_shape_valid(const StopSelectorDTO& selector) {
  return std::visit(
      [](const auto& targets) {
        using Targets = std::decay_t<decltype(targets)>;
        if (targets.empty() || targets.size() > kMaximumStopSelectorTargets) {
          return false;
        }
        if constexpr (std::is_same_v<Targets, std::vector<StopOperationTargetDTO>>) {
          return canonical_rows(targets, [](const auto& target) { return target.operation_id; }) &&
                 std::all_of(targets.begin(), targets.end(), [](const auto& target) {
                   return target.operation_id.nonzero() &&
                          target.expected_operation_revision.nonzero();
                 });
        } else if constexpr (std::is_same_v<Targets, std::vector<StopComponentTargetDTO>>) {
          return canonical_rows(
                     targets,
                     [](const auto& target) { return target.controllable_component_id; }) &&
                 std::all_of(targets.begin(), targets.end(), [](const auto& target) {
                   return bounded_identity(target.controllable_component_id) &&
                          bounded_identity(target.expected_graph_revision) &&
                          target.expected_component_lifecycle_generation.nonzero();
                 });
        } else {
          return canonical_rows(targets, [](const auto& target) { return target.resource_id; }) &&
                 std::all_of(targets.begin(), targets.end(), [](const auto& target) {
                   return bounded_identity(target.resource_id) &&
                          bounded_identity(target.expected_graph_revision);
                 });
        }
      },
      selector);
}

[[nodiscard]] bool selector_expectations_safe(const StopSelectorDTO& selector) noexcept {
  return std::visit(
      [](const auto& targets) {
        using Targets = std::decay_t<decltype(targets)>;
        return std::all_of(targets.begin(), targets.end(), [](const auto& target) {
          if constexpr (std::is_same_v<Targets, std::vector<StopOperationTargetDTO>>) {
            return safe_counter(target.expected_operation_revision);
          } else if constexpr (std::is_same_v<Targets, std::vector<StopComponentTargetDTO>>) {
            return safe_counter(target.expected_component_lifecycle_generation);
          } else {
            return target.expected_operation_resource_generation <= kMaximumIJsonInteger;
          }
        });
      },
      selector);
}

[[nodiscard]] bool terminal_state(OperationStateKind state) noexcept {
  switch (state) {
    case OperationStateKind::kCompleted:
    case OperationStateKind::kStopped:
    case OperationStateKind::kFailed:
    case OperationStateKind::kInterrupted:
    case OperationStateKind::kInterventionRequired:
    case OperationStateKind::kCancelledPreDispatch:
      return true;
    case OperationStateKind::kQueued:
    case OperationStateKind::kResolving:
    case OperationStateKind::kPlanning:
    case OperationStateKind::kValidating:
    case OperationStateKind::kPrepared:
    case OperationStateKind::kDispatching:
    case OperationStateKind::kExecuting:
    case OperationStateKind::kStopping:
      return false;
  }
  return false;
}

}  // namespace

std::optional<std::string_view> NormalizedStopIntentFieldError(const StopIntentDTO& intent) {
  const auto field = stop_intent_field_error(intent);
  if (field.has_value() && *field != "stop_reason_invalid") {
    return field;
  }
  if (!selector_scope_shape_valid(intent.selector) ||
      !selector_expectations_safe(intent.selector)) {
    return "selector";
  }
  return std::nullopt;
}

bool stop_requires_containment(const OperationStateKind state, const bool effect_uncertain) noexcept {
  return !terminal_state(state) || effect_uncertain;
}

StopTargetResolution classify_stop_operation_target(
    const StopOperationTargetDTO& target, const std::optional<OperationRevision> current_revision,
    const bool requires_containment, const bool known_historical) noexcept {
  if (!current_revision.has_value()) {
    return known_historical ? StopTargetResolution::kNoEffect : StopTargetResolution::kUnknown;
  }
  if (!requires_containment) return StopTargetResolution::kNoEffect;
  return target.expected_operation_revision == *current_revision
             ? StopTargetResolution::kExact
             : StopTargetResolution::kStaleMetadata;
}

const StopScopeComponentSnapshotDTO* find_stop_component(
    const std::span<const StopScopeComponentSnapshotDTO> components,
    const ControllableComponentId& component) noexcept {
  const auto found = std::lower_bound(
      components.begin(), components.end(), component,
      [](const auto& row, const ControllableComponentId& id) {
        return row.controllable_component_id < id;
      });
  return found != components.end() && found->controllable_component_id == component ? &*found
                                                                                     : nullptr;
}

const StopScopeComponentSnapshotDTO* find_stop_resource_owner(
    const std::span<const StopScopeComponentSnapshotDTO> components,
    const ResourceId& resource) noexcept {
  for (const auto& component : components) {
    if (std::find(component.resources.begin(), component.resources.end(), resource) !=
        component.resources.end())
      return &component;
  }
  return nullptr;
}

}  // namespace manyforge::core
