#include <algorithm>
#include <iterator>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/stop_scope_resolver.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

constexpr std::string_view kRequestedSelectorIdentityDomain = "manyforge.stop.requested_selector";
constexpr std::string_view kRequestedSelectorApiVersion = "manyforge.stop.requested_selector.v1";
constexpr std::string_view kNormalizedIntentIdentityDomain = "manyforge.stop.normalized_intent";
constexpr std::string_view kNormalizedIntentApiVersion = "manyforge.stop.normalized_intent.v1";
constexpr std::string_view kCapturedScopeIdentityDomain = "manyforge.stop.captured_scope";
constexpr std::string_view kCapturedScopeApiVersion = "manyforge.stop.captured_scope.v1";
constexpr std::uint64_t kMaximumIJsonInteger = 9'007'199'254'740'991ULL;

template <typename Counter>
[[nodiscard]] bool safe_counter(const Counter& value) noexcept {
  return value.value() <= kMaximumIJsonInteger;
}

[[nodiscard]] CanonicalValue canonical_unsigned(const std::uint64_t value) {
  return CanonicalValue(static_cast<double>(value));
}

template <typename StrongId>
[[nodiscard]] CanonicalValue canonical_identity(const StrongId& value) {
  return CanonicalValue(value.value());
}

template <typename StrongId>
[[nodiscard]] CanonicalValue canonical_identity_array(const std::vector<StrongId>& values) {
  CanonicalValue::Array rows;
  rows.reserve(values.size());
  for (const auto& value : values) {
    rows.push_back(canonical_identity(value));
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] ErrorDTO digest_input_error(std::string reason, std::string field) {
  return ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                  "Stop identity input is not a valid canonical value",
                  {{"reason", std::move(reason)}, {"field", std::move(field)}}};
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

template <typename StrongId>
[[nodiscard]] bool canonical_id_set(const std::vector<StrongId>& values) {
  return values.size() <= kMaximumStopSelectorTargets &&
         std::all_of(values.begin(), values.end(),
                     [](const auto& value) { return bounded_identity(value); }) &&
         std::is_sorted(values.begin(), values.end()) &&
         std::adjacent_find(values.begin(), values.end()) == values.end();
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
                   return bounded_identity(target.operation_id) &&
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
        } else if constexpr (std::is_same_v<Targets, std::vector<StopResourceTargetDTO>>) {
          return canonical_rows(targets, [](const auto& target) { return target.resource_id; }) &&
                 std::all_of(targets.begin(), targets.end(), [](const auto& target) {
                   return bounded_identity(target.resource_id) &&
                          bounded_identity(target.expected_graph_revision);
                 });
        } else {
          return canonical_rows(targets,
                                [](const auto& target) { return target.program_run_id; }) &&
                 std::all_of(targets.begin(), targets.end(), [](const auto& target) {
                   return bounded_identity(target.program_run_id);
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
            return safe_counter(target.expected_operation_revision) &&
                   safe_counter(target.expected_stop_generation);
          } else if constexpr (std::is_same_v<Targets, std::vector<StopComponentTargetDTO>>) {
            return safe_counter(target.expected_component_lifecycle_generation);
          } else if constexpr (std::is_same_v<Targets, std::vector<StopResourceTargetDTO>>) {
            return target.expected_operation_resource_generation <= kMaximumIJsonInteger &&
                   safe_counter(target.expected_stop_generation);
          } else {
            return true;
          }
        });
      },
      selector);
}

[[nodiscard]] CanonicalValue requested_selector_value(const StopSelectorDTO& selector) {
  return std::visit(
      [](const auto& targets) -> CanonicalValue {
        using Targets = std::decay_t<decltype(targets)>;
        CanonicalValue::Array rows;
        rows.reserve(targets.size());
        for (const auto& target : targets) {
          if constexpr (std::is_same_v<Targets, std::vector<StopOperationTargetDTO>>) {
            rows.emplace_back(CanonicalValue::Object{
                {"expected_operation_revision",
                 canonical_unsigned(target.expected_operation_revision.value())},
                {"expected_stop_generation",
                 canonical_unsigned(target.expected_stop_generation.value())},
                {"operation_id", canonical_identity(target.operation_id)}});
          } else if constexpr (std::is_same_v<Targets, std::vector<StopComponentTargetDTO>>) {
            rows.emplace_back(CanonicalValue::Object{
                {"controllable_component_id", canonical_identity(target.controllable_component_id)},
                {"expected_component_lifecycle_generation",
                 canonical_unsigned(target.expected_component_lifecycle_generation.value())},
                {"expected_graph_revision", canonical_identity(target.expected_graph_revision)}});
          } else if constexpr (std::is_same_v<Targets, std::vector<StopResourceTargetDTO>>) {
            rows.emplace_back(CanonicalValue::Object{
                {"expected_graph_revision", canonical_identity(target.expected_graph_revision)},
                {"expected_operation_resource_generation",
                 canonical_unsigned(target.expected_operation_resource_generation)},
                {"expected_stop_generation",
                 canonical_unsigned(target.expected_stop_generation.value())},
                {"resource_id", canonical_identity(target.resource_id)}});
          } else {
            rows.emplace_back(CanonicalValue::Object{
                {"program_run_id", canonical_identity(target.program_run_id)}});
          }
        }
        if constexpr (std::is_same_v<Targets, std::vector<StopOperationTargetDTO>>) {
          return CanonicalValue(
              CanonicalValue::Object{{"selector_kind", CanonicalValue("operations")},
                                     {"targets", CanonicalValue(std::move(rows))}});
        } else if constexpr (std::is_same_v<Targets, std::vector<StopComponentTargetDTO>>) {
          return CanonicalValue(
              CanonicalValue::Object{{"selector_kind", CanonicalValue("components")},
                                     {"targets", CanonicalValue(std::move(rows))}});
        } else if constexpr (std::is_same_v<Targets, std::vector<StopResourceTargetDTO>>) {
          return CanonicalValue(
              CanonicalValue::Object{{"selector_kind", CanonicalValue("resources")},
                                     {"targets", CanonicalValue(std::move(rows))}});
        } else {
          return CanonicalValue(
              CanonicalValue::Object{{"selector_kind", CanonicalValue("program_runs")},
                                     {"targets", CanonicalValue(std::move(rows))}});
        }
      },
      selector);
}

[[nodiscard]] CanonicalValue request_context_value(const PublicRequestContextDTO& context) {
  CanonicalValue::Object value{
      {"request_id", canonical_identity(context.request_id)},
      {"request_owner_epoch", canonical_identity(context.request_owner_epoch)},
      {"source_label", CanonicalValue(context.source_label)}};
  if (context.correlation_id.has_value()) {
    value.emplace_back("correlation_id", CanonicalValue(*context.correlation_id));
  }
  std::sort(value.begin(), value.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });
  return CanonicalValue(std::move(value));
}

[[nodiscard]] bool known_operation_state(OperationStateKind state) noexcept {
  switch (state) {
    case OperationStateKind::kQueued:
    case OperationStateKind::kResolving:
    case OperationStateKind::kPlanning:
    case OperationStateKind::kValidating:
    case OperationStateKind::kPrepared:
    case OperationStateKind::kDispatching:
    case OperationStateKind::kExecuting:
    case OperationStateKind::kPaused:
    case OperationStateKind::kStopping:
    case OperationStateKind::kCompleted:
    case OperationStateKind::kStopped:
    case OperationStateKind::kFailed:
    case OperationStateKind::kInterrupted:
    case OperationStateKind::kInterventionRequired:
    case OperationStateKind::kCancelledPreDispatch:
      return true;
  }
  return false;
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
    case OperationStateKind::kPaused:
    case OperationStateKind::kStopping:
      return false;
  }
  return false;
}

[[nodiscard]] bool requires_containment(const StopScopeOperationSnapshotDTO& operation) noexcept {
  return !terminal_state(operation.state) || operation.effect_uncertain;
}

template <typename Rows, typename Identity, typename Key>
[[nodiscard]] const typename Rows::value_type* find_row(const Rows& rows, const Identity& identity,
                                                        Key&& key) {
  const auto found =
      std::lower_bound(rows.begin(), rows.end(), identity,
                       [&](const auto& row, const auto& value) { return key(row) < value; });
  if (found == rows.end() || key(*found) != identity) {
    return nullptr;
  }
  return &*found;
}

[[nodiscard]] const StopScopeOperationSnapshotDTO* find_operation(
    const StopScopeResolverSnapshotDTO& snapshot, const OperationId& operation_id) {
  return find_row(snapshot.operations, operation_id,
                  [](const auto& row) { return row.operation_id; });
}

[[nodiscard]] const StopScopeComponentSnapshotDTO* find_component(
    const StopScopeResolverSnapshotDTO& snapshot, const ControllableComponentId& component_id) {
  return find_row(snapshot.components, component_id,
                  [](const auto& row) { return row.controllable_component_id; });
}

[[nodiscard]] const StopScopeResourceSnapshotDTO* find_resource(
    const StopScopeResolverSnapshotDTO& snapshot, const ResourceId& resource_id) {
  return find_row(snapshot.resources, resource_id, [](const auto& row) { return row.resource_id; });
}

struct ResourceOwnerIndexRow final {
  const ResourceId* resource_id{};
  const StopScopeComponentSnapshotDTO* owner{};
};

using ResourceOwnerIndex = std::vector<ResourceOwnerIndexRow>;

[[nodiscard]] ResourceOwnerIndex make_resource_owner_index(
    const StopScopeResolverSnapshotDTO& snapshot) {
  ResourceOwnerIndex index;
  index.reserve(std::min(kMaximumStopScopeMembershipRows,
                         snapshot.resources.size() + snapshot.components.size()));
  for (const auto& component : snapshot.components) {
    for (const auto& resource_id : component.resources) {
      index.push_back(ResourceOwnerIndexRow{&resource_id, &component});
    }
  }
  std::sort(index.begin(), index.end(), [](const auto& left, const auto& right) {
    return *left.resource_id < *right.resource_id;
  });
  return index;
}

[[nodiscard]] const StopScopeComponentSnapshotDTO* resource_owner(const ResourceOwnerIndex& index,
                                                                  const ResourceId& resource_id) {
  const auto found =
      std::lower_bound(index.begin(), index.end(), resource_id,
                       [](const auto& row, const auto& value) { return *row.resource_id < value; });
  if (found == index.end() || *found->resource_id != resource_id) {
    return nullptr;
  }
  return found->owner;
}

template <typename Expected, typename Current>
void add_stale(std::vector<StopScopeStaleDiagnosticDTO>& diagnostics, StopScopeStaleFieldDTO field,
               StopScopeDiagnosticTargetDTO target, const Expected& expected,
               const Current& current) {
  if (expected == current) {
    return;
  }
  diagnostics.push_back(StopScopeStaleDiagnosticDTO{
      field, std::move(target), StopScopeStaleValueDTO(expected), StopScopeStaleValueDTO(current)});
}

[[nodiscard]] std::string diagnostic_target_key(const StopScopeDiagnosticTargetDTO& target) {
  return std::visit(
      [](const auto& value) -> std::string {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::monostate>) {
          return {};
        } else {
          return value.value();
        }
      },
      target);
}

void canonicalize_diagnostics(std::vector<StopScopeStaleDiagnosticDTO>& diagnostics) {
  std::sort(diagnostics.begin(), diagnostics.end(), [](const auto& left, const auto& right) {
    if (left.field != right.field) {
      return left.field < right.field;
    }
    if (left.target.index() != right.target.index()) {
      return left.target.index() < right.target.index();
    }
    return diagnostic_target_key(left.target) < diagnostic_target_key(right.target);
  });
}

[[nodiscard]] bool known_disposition(const StopResolutionDispositionDTO disposition) noexcept {
  switch (disposition) {
    case StopResolutionDispositionDTO::kExact:
    case StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved:
    case StopResolutionDispositionDTO::kAlreadyTerminalNoEffect:
    case StopResolutionDispositionDTO::kPromotedToFull:
      return true;
  }
  return false;
}

[[nodiscard]] bool known_reason(const StopScopeResolutionReasonDTO reason) noexcept {
  switch (reason) {
    case StopScopeResolutionReasonDTO::kExactCurrentScope:
    case StopScopeResolutionReasonDTO::kStaleMetadataCurrentScope:
    case StopScopeResolutionReasonDTO::kKnownTerminalOrIdleNoEffect:
    case StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent:
    case StopScopeResolutionReasonDTO::kUnknownOperation:
    case StopScopeResolutionReasonDTO::kUnknownProgramRun:
    case StopScopeResolutionReasonDTO::kUnknownComponent:
    case StopScopeResolutionReasonDTO::kUnknownResource:
    case StopScopeResolutionReasonDTO::kRequestIdentityConflict:
    case StopScopeResolutionReasonDTO::kOverlappingStopTransactions:
    case StopScopeResolutionReasonDTO::kEmergencyReserveCoalesced:
    case StopScopeResolutionReasonDTO::kExplicitFullStop:
      return true;
  }
  return false;
}

[[nodiscard]] bool disposition_reason_valid(const StopResolutionDispositionDTO disposition,
                                            const StopScopeResolutionReasonDTO reason) noexcept {
  switch (disposition) {
    case StopResolutionDispositionDTO::kExact:
      return reason == StopScopeResolutionReasonDTO::kExactCurrentScope ||
             reason == StopScopeResolutionReasonDTO::kExplicitFullStop;
    case StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved:
      return reason == StopScopeResolutionReasonDTO::kStaleMetadataCurrentScope ||
             reason == StopScopeResolutionReasonDTO::kExplicitFullStop;
    case StopResolutionDispositionDTO::kAlreadyTerminalNoEffect:
      return reason == StopScopeResolutionReasonDTO::kKnownTerminalOrIdleNoEffect;
    case StopResolutionDispositionDTO::kPromotedToFull:
      return reason == StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent ||
             reason == StopScopeResolutionReasonDTO::kUnknownOperation ||
             reason == StopScopeResolutionReasonDTO::kUnknownProgramRun ||
             reason == StopScopeResolutionReasonDTO::kUnknownComponent ||
             reason == StopScopeResolutionReasonDTO::kUnknownResource ||
             reason == StopScopeResolutionReasonDTO::kRequestIdentityConflict ||
             reason == StopScopeResolutionReasonDTO::kOverlappingStopTransactions ||
             reason == StopScopeResolutionReasonDTO::kEmergencyReserveCoalesced;
  }
  return false;
}

[[nodiscard]] bool full_stop_reason_valid(const bool full_stop,
                                          const StopScopeResolutionReasonDTO reason) noexcept {
  switch (reason) {
    case StopScopeResolutionReasonDTO::kExplicitFullStop:
    case StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent:
    case StopScopeResolutionReasonDTO::kUnknownOperation:
    case StopScopeResolutionReasonDTO::kUnknownProgramRun:
    case StopScopeResolutionReasonDTO::kUnknownComponent:
    case StopScopeResolutionReasonDTO::kUnknownResource:
    case StopScopeResolutionReasonDTO::kRequestIdentityConflict:
    case StopScopeResolutionReasonDTO::kOverlappingStopTransactions:
    case StopScopeResolutionReasonDTO::kEmergencyReserveCoalesced:
      return full_stop;
    case StopScopeResolutionReasonDTO::kExactCurrentScope:
    case StopScopeResolutionReasonDTO::kStaleMetadataCurrentScope:
    case StopScopeResolutionReasonDTO::kKnownTerminalOrIdleNoEffect:
      return !full_stop;
  }
  return false;
}

template <typename Expected, typename Target>
[[nodiscard]] bool diagnostic_has_types(const StopScopeStaleDiagnosticDTO& diagnostic) noexcept {
  return std::holds_alternative<Target>(diagnostic.target) &&
         std::holds_alternative<Expected>(diagnostic.expected) &&
         std::holds_alternative<Expected>(diagnostic.current);
}

[[nodiscard]] bool diagnostic_shape_valid(const StopScopeStaleDiagnosticDTO& diagnostic) noexcept {
  bool types_valid = false;
  switch (diagnostic.field) {
    case StopScopeStaleFieldDTO::kRequestOwnerEpoch:
      types_valid = diagnostic_has_types<RequestOwnerEpoch, std::monostate>(diagnostic);
      break;
    case StopScopeStaleFieldDTO::kOperationRevision:
      types_valid = diagnostic_has_types<OperationRevision, OperationId>(diagnostic);
      break;
    case StopScopeStaleFieldDTO::kOperationStopGeneration:
      types_valid = diagnostic_has_types<StopGeneration, OperationId>(diagnostic);
      break;
    case StopScopeStaleFieldDTO::kGraphRevision:
      types_valid = diagnostic_has_types<GraphRevision, ControllableComponentId>(diagnostic) ||
                    diagnostic_has_types<GraphRevision, ResourceId>(diagnostic);
      break;
    case StopScopeStaleFieldDTO::kComponentLifecycleGeneration:
      types_valid =
          diagnostic_has_types<ComponentLifecycleGeneration, ControllableComponentId>(diagnostic);
      break;
    case StopScopeStaleFieldDTO::kOperationResourceGeneration:
      types_valid = diagnostic_has_types<std::uint64_t, ResourceId>(diagnostic);
      break;
    case StopScopeStaleFieldDTO::kResourceStopGeneration:
      types_valid = diagnostic_has_types<StopGeneration, ResourceId>(diagnostic);
      break;
    case StopScopeStaleFieldDTO::kCellStopGeneration:
      types_valid = diagnostic_has_types<StopGeneration, std::monostate>(diagnostic);
      break;
  }
  if (!types_valid || diagnostic.expected == diagnostic.current) {
    return false;
  }
  const bool target_valid = std::visit(
      [](const auto& target) {
        using Target = std::decay_t<decltype(target)>;
        if constexpr (std::is_same_v<Target, std::monostate>) {
          return true;
        } else {
          return bounded_identity(target);
        }
      },
      diagnostic.target);
  if (!target_valid) {
    return false;
  }
  return std::visit(
             [](const auto& expected) {
               using Value = std::decay_t<decltype(expected)>;
               if constexpr (std::is_same_v<Value, std::uint64_t>) {
                 return expected <= kMaximumIJsonInteger;
               } else if constexpr (std::is_same_v<Value, RequestOwnerEpoch> ||
                                    std::is_same_v<Value, GraphRevision>) {
                 return bounded_identity(expected);
               } else {
                 return safe_counter(expected);
               }
             },
             diagnostic.expected) &&
         std::visit(
             [](const auto& current) {
               using Value = std::decay_t<decltype(current)>;
               if constexpr (std::is_same_v<Value, std::uint64_t>) {
                 return current <= kMaximumIJsonInteger;
               } else if constexpr (std::is_same_v<Value, RequestOwnerEpoch> ||
                                    std::is_same_v<Value, GraphRevision>) {
                 return bounded_identity(current);
               } else {
                 return safe_counter(current);
               }
             },
             diagnostic.current);
}

[[nodiscard]] bool diagnostic_less(const StopScopeStaleDiagnosticDTO& left,
                                   const StopScopeStaleDiagnosticDTO& right) {
  if (left.field != right.field) {
    return left.field < right.field;
  }
  if (left.target.index() != right.target.index()) {
    return left.target.index() < right.target.index();
  }
  return diagnostic_target_key(left.target) < diagnostic_target_key(right.target);
}

struct Capture final {
  std::set<OperationId> operations;
  std::set<ControllableComponentId> components;
  std::set<ResourceId> resources;
};

void capture_operation(const StopScopeOperationSnapshotDTO& operation, Capture& capture) {
  if (!requires_containment(operation)) {
    return;
  }
  capture.operations.insert(operation.operation_id);
  capture.components.insert(operation.controllable_components.begin(),
                            operation.controllable_components.end());
  capture.resources.insert(operation.resources.begin(), operation.resources.end());
}

void capture_effect_uncertain_resource(const ResourceOwnerIndex& owner_index,
                                       const StopScopeResourceSnapshotDTO& resource,
                                       Capture& capture) {
  if (!resource.effect_uncertain) {
    return;
  }
  capture.resources.insert(resource.resource_id);
  if (const auto* owner = resource_owner(owner_index, resource.resource_id); owner != nullptr) {
    capture.components.insert(owner->controllable_component_id);
  }
}

[[nodiscard]] Capture capture_full(const StopScopeResolverSnapshotDTO& snapshot,
                                   const ResourceOwnerIndex& owner_index) {
  Capture capture;
  for (const auto& operation : snapshot.operations) {
    capture_operation(operation, capture);
  }
  for (const auto& resource : snapshot.resources) {
    capture_effect_uncertain_resource(owner_index, resource, capture);
  }
  return capture;
}

[[nodiscard]] StopScopeResolutionDTO make_resolution(
    StopResolutionDispositionDTO disposition, StopScopeResolutionReasonDTO reason, bool full_stop,
    Capture capture, std::vector<StopScopeStaleDiagnosticDTO> diagnostics = {}) {
  canonicalize_diagnostics(diagnostics);
  return StopScopeResolutionDTO{
      disposition,
      reason,
      full_stop,
      std::vector<OperationId>(capture.operations.begin(), capture.operations.end()),
      std::vector<ControllableComponentId>(capture.components.begin(), capture.components.end()),
      std::vector<ResourceId>(capture.resources.begin(), capture.resources.end()),
      std::move(diagnostics)};
}

[[nodiscard]] StopScopeResolutionDTO promote_to_full(
    const StopScopeResolverSnapshotDTO& snapshot, const ResourceOwnerIndex& owner_index,
    StopScopeResolutionReasonDTO reason,
    std::vector<StopScopeStaleDiagnosticDTO> diagnostics = {}) {
  return make_resolution(StopResolutionDispositionDTO::kPromotedToFull, reason, true,
                         capture_full(snapshot, owner_index), std::move(diagnostics));
}

[[nodiscard]] ErrorDTO invalid_snapshot_error(std::string_view field) {
  return ErrorDTO{std::string(to_string(ErrorCode::kInvariantViolation)),
                  "Stop scope snapshot is not a complete canonical owner capture",
                  {{"reason", "stop_scope_snapshot_invalid"}, {"field", std::string(field)}}};
}

void add_common_stale(const PublicRequestContextDTO& context,
                      const StopScopeResolverSnapshotDTO& snapshot,
                      std::vector<StopScopeStaleDiagnosticDTO>& diagnostics) {
  add_stale(diagnostics, StopScopeStaleFieldDTO::kRequestOwnerEpoch, std::monostate{},
            context.request_owner_epoch, snapshot.request_owner_epoch);
}

[[nodiscard]] bool capture_empty(const Capture& capture) noexcept {
  return capture.operations.empty() && capture.components.empty() && capture.resources.empty();
}

}  // namespace

std::optional<std::string_view> stop_scope_snapshot_field_error(
    const StopScopeResolverSnapshotDTO& snapshot) {
  if (!bounded_identity(snapshot.request_owner_epoch)) {
    return "stop_scope_request_owner_epoch_invalid";
  }
  if (!bounded_identity(snapshot.graph_revision)) {
    return "stop_scope_graph_revision_invalid";
  }
  if (snapshot.operation_resource_generation > kMaximumIJsonInteger ||
      !safe_counter(snapshot.cell_stop_generation)) {
    return "stop_scope_generation_not_i_json_safe";
  }
  if (snapshot.operations.size() > kMaximumStopScopeSnapshotOperations ||
      !canonical_rows(snapshot.operations, [](const auto& row) { return row.operation_id; })) {
    return "stop_scope_operations_not_canonical";
  }
  if (snapshot.known_program_runs.size() > kMaximumStopScopeSnapshotProgramRuns ||
      !canonical_id_set(snapshot.known_program_runs)) {
    return "stop_scope_program_runs_not_canonical";
  }
  if (snapshot.components.size() > kMaximumStopScopeSnapshotComponents ||
      !canonical_rows(snapshot.components,
                      [](const auto& row) { return row.controllable_component_id; })) {
    return "stop_scope_components_not_canonical";
  }
  if (snapshot.resources.size() > kMaximumStopScopeSnapshotResources ||
      !canonical_rows(snapshot.resources, [](const auto& row) { return row.resource_id; })) {
    return "stop_scope_resources_not_canonical";
  }

  std::size_t membership_rows = 0U;
  for (const auto& resource : snapshot.resources) {
    if (!bounded_identity(resource.resource_id) || !safe_counter(resource.stop_generation)) {
      return "stop_scope_resource_invalid";
    }
  }
  for (const auto& component : snapshot.components) {
    if (!bounded_identity(component.controllable_component_id) ||
        !component.component_lifecycle_generation.nonzero() ||
        !safe_counter(component.component_lifecycle_generation)) {
      return "stop_scope_component_invalid";
    }
    if (!canonical_id_set(component.resources)) {
      return "stop_scope_component_resources_not_canonical";
    }
    if (component.resources.size() > kMaximumStopScopeMembershipRows - membership_rows) {
      return "stop_scope_membership_limit_exceeded";
    }
    membership_rows += component.resources.size();
    for (const auto& resource_id : component.resources) {
      if (find_resource(snapshot, resource_id) == nullptr) {
        return "stop_scope_component_resource_unknown";
      }
    }
  }

  const auto owner_index = make_resource_owner_index(snapshot);
  for (auto current = owner_index.begin(); current != owner_index.end();) {
    const auto next = std::upper_bound(
        current, owner_index.end(), *current->resource_id,
        [](const auto& value, const auto& row) { return value < *row.resource_id; });
    if (static_cast<std::size_t>(std::distance(current, next)) > 1U) {
      return "stop_scope_resource_owner_ambiguous";
    }
    current = next;
  }

  for (const auto& operation : snapshot.operations) {
    if (!bounded_identity(operation.operation_id) || !operation.operation_revision.nonzero() ||
        !bounded_identity(operation.program_run_id) ||
        (operation.parent_program_run_id && !bounded_identity(*operation.parent_program_run_id)) ||
        !safe_counter(operation.operation_revision) || !safe_counter(operation.stop_generation) ||
        !known_operation_state(operation.state)) {
      return "stop_scope_operation_invalid";
    }
    if (!canonical_id_set(operation.controllable_components) ||
        !canonical_id_set(operation.resources)) {
      return "stop_scope_operation_membership_not_canonical";
    }
    if (operation.controllable_components.size() >
            kMaximumStopScopeMembershipRows - membership_rows ||
        operation.resources.size() > kMaximumStopScopeMembershipRows - membership_rows -
                                         operation.controllable_components.size()) {
      return "stop_scope_membership_limit_exceeded";
    }
    membership_rows += operation.controllable_components.size() + operation.resources.size();

    if (requires_containment(operation) && !operation.scope_complete) {
      return "stop_scope_effectful_operation_scope_incomplete";
    }
    if (!operation.scope_complete &&
        (!operation.controllable_components.empty() || !operation.resources.empty())) {
      return "stop_scope_partial_operation_scope_forbidden";
    }
    if (operation.scope_complete &&
        (operation.controllable_components.empty() || operation.resources.empty())) {
      return "stop_scope_complete_operation_scope_empty";
    }
    for (const auto& component_id : operation.controllable_components) {
      if (find_component(snapshot, component_id) == nullptr) {
        return "stop_scope_operation_component_unknown";
      }
    }
    for (const auto& resource_id : operation.resources) {
      if (find_resource(snapshot, resource_id) == nullptr) {
        return "stop_scope_operation_resource_unknown";
      }
      const auto* owner = resource_owner(owner_index, resource_id);
      if (owner == nullptr || !std::binary_search(operation.controllable_components.begin(),
                                                  operation.controllable_components.end(),
                                                  owner->controllable_component_id)) {
        return "stop_scope_operation_resource_owner_outside_scope";
      }
    }
  }
  return std::nullopt;
}

Result<StopScopeResolutionDTO> resolve_stop_scope(const StopIntentDTO& intent,
                                                  const StopScopeResolverSnapshotDTO& snapshot) {
  if (const auto field = stop_scope_snapshot_field_error(snapshot); field.has_value()) {
    return Result<StopScopeResolutionDTO>::Err(invalid_snapshot_error(*field));
  }
  const auto owner_index = make_resource_owner_index(snapshot);
  // Optional diagnostic text is not scope authority. A conforming adapter
  // omits/truncates it before this boundary; preserve that rule if a local
  // caller nevertheless supplies it. Other malformed fields still promote.
  if (const auto field = stop_intent_field_error(intent);
      field.has_value() &&
      (*field != "stop_reason_invalid" || !selector_scope_shape_valid(intent.selector))) {
    return Result<StopScopeResolutionDTO>::Ok(promote_to_full(
        snapshot, owner_index, StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent));
  }

  Capture capture;
  std::vector<StopScopeStaleDiagnosticDTO> diagnostics;
  add_common_stale(intent.request_context, snapshot, diagnostics);
  std::optional<StopScopeResolutionReasonDTO> unresolved_reason;

  std::visit(
      [&](const auto& targets) {
        using Targets = std::decay_t<decltype(targets)>;
        if constexpr (std::is_same_v<Targets, std::vector<StopOperationTargetDTO>>) {
          for (const auto& target : targets) {
            const auto* operation = find_operation(snapshot, target.operation_id);
            if (operation == nullptr) {
              unresolved_reason = StopScopeResolutionReasonDTO::kUnknownOperation;
              continue;
            }
            add_stale(diagnostics, StopScopeStaleFieldDTO::kOperationRevision, target.operation_id,
                      target.expected_operation_revision, operation->operation_revision);
            add_stale(diagnostics, StopScopeStaleFieldDTO::kOperationStopGeneration,
                      target.operation_id, target.expected_stop_generation,
                      operation->stop_generation);
            capture_operation(*operation, capture);
          }
        } else if constexpr (std::is_same_v<Targets, std::vector<StopComponentTargetDTO>>) {
          for (const auto& target : targets) {
            const auto* component = find_component(snapshot, target.controllable_component_id);
            if (component == nullptr) {
              unresolved_reason = StopScopeResolutionReasonDTO::kUnknownComponent;
              continue;
            }
            add_stale(diagnostics, StopScopeStaleFieldDTO::kGraphRevision,
                      target.controllable_component_id, target.expected_graph_revision,
                      snapshot.graph_revision);
            add_stale(diagnostics, StopScopeStaleFieldDTO::kComponentLifecycleGeneration,
                      target.controllable_component_id,
                      target.expected_component_lifecycle_generation,
                      component->component_lifecycle_generation);
            for (const auto& operation : snapshot.operations) {
              if (requires_containment(operation) &&
                  std::binary_search(operation.controllable_components.begin(),
                                     operation.controllable_components.end(),
                                     target.controllable_component_id)) {
                capture_operation(operation, capture);
              }
            }
            for (const auto& resource_id : component->resources) {
              capture_effect_uncertain_resource(owner_index, *find_resource(snapshot, resource_id),
                                                capture);
            }
          }
        } else if constexpr (std::is_same_v<Targets, std::vector<StopResourceTargetDTO>>) {
          for (const auto& target : targets) {
            const auto* resource = find_resource(snapshot, target.resource_id);
            if (resource == nullptr) {
              unresolved_reason = StopScopeResolutionReasonDTO::kUnknownResource;
              continue;
            }
            add_stale(diagnostics, StopScopeStaleFieldDTO::kGraphRevision, target.resource_id,
                      target.expected_graph_revision, snapshot.graph_revision);
            add_stale(diagnostics, StopScopeStaleFieldDTO::kOperationResourceGeneration,
                      target.resource_id, target.expected_operation_resource_generation,
                      snapshot.operation_resource_generation);
            add_stale(diagnostics, StopScopeStaleFieldDTO::kResourceStopGeneration,
                      target.resource_id, target.expected_stop_generation,
                      resource->stop_generation);
            for (const auto& operation : snapshot.operations) {
              if (requires_containment(operation) &&
                  std::binary_search(operation.resources.begin(), operation.resources.end(),
                                     target.resource_id)) {
                capture_operation(operation, capture);
              }
            }
            capture_effect_uncertain_resource(owner_index, *resource, capture);
          }
        } else {
          for (const auto& target : targets) {
            bool known =
                std::binary_search(snapshot.known_program_runs.begin(),
                                   snapshot.known_program_runs.end(), target.program_run_id);
            for (const auto& operation : snapshot.operations) {
              if (operation.program_run_id != target.program_run_id &&
                  operation.parent_program_run_id != target.program_run_id) {
                continue;
              }
              known = true;
              if (requires_containment(operation)) {
                capture_operation(operation, capture);
              }
            }
            if (!known) {
              unresolved_reason = StopScopeResolutionReasonDTO::kUnknownProgramRun;
            }
          }
        }
      },
      intent.selector);

  if (unresolved_reason.has_value()) {
    return Result<StopScopeResolutionDTO>::Ok(
        promote_to_full(snapshot, owner_index, *unresolved_reason, std::move(diagnostics)));
  }
  if (capture_empty(capture)) {
    return Result<StopScopeResolutionDTO>::Ok(
        make_resolution(StopResolutionDispositionDTO::kAlreadyTerminalNoEffect,
                        StopScopeResolutionReasonDTO::kKnownTerminalOrIdleNoEffect, false,
                        std::move(capture), std::move(diagnostics)));
  }
  if (!diagnostics.empty()) {
    return Result<StopScopeResolutionDTO>::Ok(
        make_resolution(StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved,
                        StopScopeResolutionReasonDTO::kStaleMetadataCurrentScope, false,
                        std::move(capture), std::move(diagnostics)));
  }
  return Result<StopScopeResolutionDTO>::Ok(
      make_resolution(StopResolutionDispositionDTO::kExact,
                      StopScopeResolutionReasonDTO::kExactCurrentScope, false, std::move(capture)));
}

Result<StopScopeResolutionDTO> resolve_full_stop_scope(
    const FullStopIntentDTO& intent, const StopScopeResolverSnapshotDTO& snapshot) {
  if (const auto field = stop_scope_snapshot_field_error(snapshot); field.has_value()) {
    return Result<StopScopeResolutionDTO>::Err(invalid_snapshot_error(*field));
  }
  const auto owner_index = make_resource_owner_index(snapshot);
  if (const auto field = full_stop_intent_field_error(intent); field.has_value()) {
    return Result<StopScopeResolutionDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                 "Full Stop intent is not a valid normalized internal value",
                 {{"reason", "full_stop_intent_invalid"}, {"field", std::string(*field)}}});
  }

  std::vector<StopScopeStaleDiagnosticDTO> diagnostics;
  add_common_stale(intent.request_context, snapshot, diagnostics);
  add_stale(diagnostics, StopScopeStaleFieldDTO::kCellStopGeneration, std::monostate{},
            intent.expected_stop_generation, snapshot.cell_stop_generation);
  const auto disposition = diagnostics.empty()
                               ? StopResolutionDispositionDTO::kExact
                               : StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved;
  return Result<StopScopeResolutionDTO>::Ok(
      make_resolution(disposition, StopScopeResolutionReasonDTO::kExplicitFullStop, true,
                      capture_full(snapshot, owner_index), std::move(diagnostics)));
}

Result<ContentDigest> ComputeRequestedStopSelectorDigest(const StopIntentDTO& intent) {
  if (!selector_scope_shape_valid(intent.selector) ||
      !selector_expectations_safe(intent.selector)) {
    return Result<ContentDigest>::Err(
        digest_input_error("requested_stop_selector_invalid", "selector"));
  }
  try {
    return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
        kRequestedSelectorIdentityDomain, kRequestedSelectorApiVersion,
        requested_selector_value(intent.selector))));
  } catch (const std::exception&) {
    return Result<ContentDigest>::Err(
        digest_input_error("requested_stop_selector_not_canonical", "selector"));
  }
}

Result<ContentDigest> ComputeRequestedStopSelectorDigest(const FullStopIntentDTO& intent) {
  if (!safe_counter(intent.expected_stop_generation)) {
    return Result<ContentDigest>::Err(
        digest_input_error("requested_full_stop_selector_invalid", "expected_stop_generation"));
  }
  try {
    const CanonicalValue payload(CanonicalValue::Object{
        {"expected_stop_generation", canonical_unsigned(intent.expected_stop_generation.value())},
        {"selector_kind", CanonicalValue("full_stop")}});
    return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
        kRequestedSelectorIdentityDomain, kRequestedSelectorApiVersion, payload)));
  } catch (const std::exception&) {
    return Result<ContentDigest>::Err(
        digest_input_error("requested_full_stop_selector_not_canonical", "selector"));
  }
}

Result<ContentDigest> ComputeRequestedStopSelectorDigest(const MalformedStopIngressDTO& ingress) {
  if (malformed_stop_ingress_field_error(ingress).has_value()) {
    return Result<ContentDigest>::Err(
        digest_input_error("requested_malformed_stop_selector_invalid", "ingress"));
  }
  try {
    const CanonicalValue payload(
        CanonicalValue::Object{{"recognized_route", CanonicalValue(ingress.recognized_route)},
                               {"selector_kind", CanonicalValue("malformed_ingress")}});
    return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
        kRequestedSelectorIdentityDomain, kRequestedSelectorApiVersion, payload)));
  } catch (const std::exception&) {
    return Result<ContentDigest>::Err(
        digest_input_error("requested_malformed_stop_selector_not_canonical", "ingress"));
  }
}

Result<ContentDigest> ComputeNormalizedStopIntentDigest(const StopIntentDTO& intent) {
  const auto field = stop_intent_field_error(intent);
  if ((field.has_value() && *field != "stop_reason_invalid") ||
      !selector_scope_shape_valid(intent.selector) ||
      !selector_expectations_safe(intent.selector)) {
    return Result<ContentDigest>::Err(digest_input_error(
        "normalized_stop_intent_invalid", field.has_value() ? std::string(*field) : "selector"));
  }
  try {
    CanonicalValue::Object payload{
        {"request_context", request_context_value(intent.request_context)},
        {"selector", requested_selector_value(intent.selector)}};
    if (intent.reason.has_value() && !field.has_value()) {
      payload.emplace_back("reason", CanonicalValue(*intent.reason));
    }
    std::sort(payload.begin(), payload.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
        kNormalizedIntentIdentityDomain, kNormalizedIntentApiVersion,
        CanonicalValue(std::move(payload)))));
  } catch (const std::exception&) {
    return Result<ContentDigest>::Err(
        digest_input_error("normalized_stop_intent_not_canonical", "intent"));
  }
}

Result<ContentDigest> ComputeNormalizedStopIntentDigest(const FullStopIntentDTO& intent) {
  if (full_stop_intent_field_error(intent).has_value() ||
      !safe_counter(intent.expected_stop_generation)) {
    return Result<ContentDigest>::Err(
        digest_input_error("normalized_full_stop_intent_invalid", "intent"));
  }
  try {
    const CanonicalValue payload(CanonicalValue::Object{
        {"expected_stop_generation", canonical_unsigned(intent.expected_stop_generation.value())},
        {"request_context", request_context_value(intent.request_context)}});
    return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
        kNormalizedIntentIdentityDomain, kNormalizedIntentApiVersion, payload)));
  } catch (const std::exception&) {
    return Result<ContentDigest>::Err(
        digest_input_error("normalized_full_stop_intent_not_canonical", "intent"));
  }
}

Result<ContentDigest> ComputeCapturedStopScopeDigest(const StopScopeResolutionDTO& resolution) {
  if (!known_disposition(resolution.disposition) || !known_reason(resolution.reason) ||
      !disposition_reason_valid(resolution.disposition, resolution.reason) ||
      !full_stop_reason_valid(resolution.full_stop, resolution.reason) ||
      !canonical_id_set(resolution.operations) ||
      !canonical_id_set(resolution.controllable_components) ||
      !canonical_id_set(resolution.resources)) {
    return Result<ContentDigest>::Err(
        digest_input_error("captured_stop_scope_invalid", "resolution"));
  }
  try {
    const CanonicalValue payload(CanonicalValue::Object{
        {"controllable_components", canonical_identity_array(resolution.controllable_components)},
        {"full_stop", CanonicalValue(resolution.full_stop)},
        {"operations", canonical_identity_array(resolution.operations)},
        {"resources", canonical_identity_array(resolution.resources)}});
    return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
        kCapturedScopeIdentityDomain, kCapturedScopeApiVersion, payload)));
  } catch (const std::exception&) {
    return Result<ContentDigest>::Err(
        digest_input_error("captured_stop_scope_not_canonical", "resolution"));
  }
}

namespace {

[[nodiscard]] ErrorDTO stop_acceptance_input_error(std::string reason) {
  return ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                  "Stop acceptance input is not one exact prepared transaction",
                  {{"reason", std::move(reason)}}};
}

template <typename Intent>
[[nodiscard]] Result<StopAcceptedDTO> make_stop_accepted(
    const Intent& intent, StopId stop_id, StopGeneration stop_generation,
    StopScopeResolutionDTO resolution,
    std::vector<StopAffectedOperationRevisionDTO> affected_operation_revisions,
    const StopDeadlineBudgetDTO& fixed_budget, const std::int64_t remaining_outer_duration_ns,
    const std::int64_t accepted_at_ns,
    std::span<const PreparedParticipantStopPlanDTO> participant_plans) {
  const auto selector_digest = ComputeRequestedStopSelectorDigest(intent);
  if (!selector_digest.ok()) {
    return Result<StopAcceptedDTO>::Err(selector_digest.error());
  }
  const auto scope_digest = ComputeCapturedStopScopeDigest(resolution);
  if (!scope_digest.ok()) {
    return Result<StopAcceptedDTO>::Err(scope_digest.error());
  }
  std::sort(
      affected_operation_revisions.begin(), affected_operation_revisions.end(),
      [](const auto& left, const auto& right) { return left.operation_id < right.operation_id; });
  if (!canonical_rows(affected_operation_revisions,
                      [](const auto& row) { return row.operation_id; })) {
    return Result<StopAcceptedDTO>::Err(
        stop_acceptance_input_error("stop_acceptance_operation_revision_duplicate"));
  }
  if (resolution.operations.size() != affected_operation_revisions.size() ||
      !std::equal(resolution.operations.begin(), resolution.operations.end(),
                  affected_operation_revisions.begin(),
                  [](const auto& operation_id, const auto& affected) {
                    return operation_id == affected.operation_id;
                  })) {
    return Result<StopAcceptedDTO>::Err(
        stop_acceptance_input_error("stop_acceptance_operation_scope_mismatch"));
  }

  if (fixed_budget.outer_duration_ns <= 0 ||
      fixed_budget.outer_duration_ns > kMaximumStopDurationNs ||
      fixed_budget.kernel_completion_reserve_ns <= 0 ||
      fixed_budget.kernel_completion_reserve_ns >= fixed_budget.outer_duration_ns ||
      remaining_outer_duration_ns < 0 ||
      remaining_outer_duration_ns > fixed_budget.outer_duration_ns || accepted_at_ns < 0 ||
      participant_plans.size() > kMaximumStopAcceptedParticipants) {
    return Result<StopAcceptedDTO>::Err(
        stop_acceptance_input_error("stop_acceptance_budget_invalid"));
  }

  std::vector<StopAcceptedParticipantBudgetDTO> participant_budgets;
  participant_budgets.reserve(participant_plans.size());
  for (const auto& plan : participant_plans) {
    const auto affected =
        std::lower_bound(affected_operation_revisions.begin(), affected_operation_revisions.end(),
                         plan.operation_id, [](const auto& row, const OperationId& operation_id) {
                           return row.operation_id < operation_id;
                         });
    if (!plan.operation_revision.nonzero() || !safe_counter(plan.operation_revision) ||
        plan.operation_revision.value() == kMaximumIJsonInteger ||
        plan.deadline_budget.outer_duration_ns != fixed_budget.outer_duration_ns ||
        plan.deadline_budget.kernel_completion_reserve_ns !=
            fixed_budget.kernel_completion_reserve_ns ||
        plan.deadline_budget.phases.empty() ||
        plan.deadline_budget.phases.size() > kMaximumStopAcceptedPhases ||
        affected == affected_operation_revisions.end() ||
        affected->operation_id != plan.operation_id ||
        affected->operation_revision_after_acceptance <= plan.operation_revision) {
      return Result<StopAcceptedDTO>::Err(
          stop_acceptance_input_error("stop_acceptance_participant_plan_mismatch"));
    }
    std::int64_t prior_phase_offset = 0;
    std::set<StopBudgetPhaseId> phase_ids;
    for (const auto& phase : plan.deadline_budget.phases) {
      if (!bounded_identity(phase.phase_id) || !phase_ids.insert(phase.phase_id).second ||
          phase.deadline_offset_ns <= prior_phase_offset ||
          phase.deadline_offset_ns >
              fixed_budget.outer_duration_ns - fixed_budget.kernel_completion_reserve_ns) {
        return Result<StopAcceptedDTO>::Err(
            stop_acceptance_input_error("stop_acceptance_participant_plan_mismatch"));
      }
      prior_phase_offset = phase.deadline_offset_ns;
    }
    StopAcceptedParticipantBudgetDTO participant{stop_id,
                                                 stop_generation,
                                                 plan.operation_id,
                                                 plan.participant_id,
                                                 plan.participant_stop_invocation_plan_digest,
                                                 {}};
    participant.phases.reserve(plan.deadline_budget.phases.size());
    const auto elapsed = fixed_budget.outer_duration_ns - remaining_outer_duration_ns;
    for (const auto& phase : plan.deadline_budget.phases) {
      const auto remaining =
          remaining_outer_duration_ns <= fixed_budget.kernel_completion_reserve_ns
              ? 0
              : std::max<std::int64_t>(0, std::min(phase.deadline_offset_ns - elapsed,
                                                   remaining_outer_duration_ns -
                                                       fixed_budget.kernel_completion_reserve_ns));
      participant.phases.push_back({phase.phase_id, phase.deadline_offset_ns, remaining});
    }
    participant_budgets.push_back(std::move(participant));
  }
  std::sort(participant_budgets.begin(), participant_budgets.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.operation_id, left.participant_id) <
                     std::tie(right.operation_id, right.participant_id);
            });

  StopAcceptedDTO accepted{intent.request_context.request_id,
                           RequestReplayStatusDTO::kAcceptedNew,
                           std::move(stop_id),
                           stop_generation,
                           std::move(selector_digest.value()),
                           resolution.disposition,
                           resolution.reason,
                           std::move(resolution.stale_diagnostics),
                           std::move(scope_digest.value()),
                           {},
                           std::move(affected_operation_revisions),
                           fixed_budget.outer_duration_ns,
                           fixed_budget.kernel_completion_reserve_ns,
                           remaining_outer_duration_ns,
                           accepted_at_ns,
                           std::move(participant_budgets)};
  if (const auto field = stop_accepted_field_error(accepted); field.has_value()) {
    return Result<StopAcceptedDTO>::Err(stop_acceptance_input_error(std::string(*field)));
  }
  return Result<StopAcceptedDTO>::Ok(std::move(accepted));
}

}  // namespace

Result<StopAcceptedDTO> MakeStopAccepted(
    const StopIntentDTO& intent, StopId stop_id, StopGeneration stop_generation,
    StopScopeResolutionDTO resolution,
    std::vector<StopAffectedOperationRevisionDTO> affected_operation_revisions,
    const StopDeadlineBudgetDTO& fixed_budget, const std::int64_t remaining_outer_duration_ns,
    const std::int64_t accepted_at_ns,
    std::span<const PreparedParticipantStopPlanDTO> participant_plans) {
  return make_stop_accepted(intent, std::move(stop_id), stop_generation, std::move(resolution),
                            std::move(affected_operation_revisions), fixed_budget,
                            remaining_outer_duration_ns, accepted_at_ns, participant_plans);
}

Result<StopAcceptedDTO> MakeStopAccepted(
    const FullStopIntentDTO& intent, StopId stop_id, StopGeneration stop_generation,
    StopScopeResolutionDTO resolution,
    std::vector<StopAffectedOperationRevisionDTO> affected_operation_revisions,
    const StopDeadlineBudgetDTO& fixed_budget, const std::int64_t remaining_outer_duration_ns,
    const std::int64_t accepted_at_ns,
    std::span<const PreparedParticipantStopPlanDTO> participant_plans) {
  return make_stop_accepted(intent, std::move(stop_id), stop_generation, std::move(resolution),
                            std::move(affected_operation_revisions), fixed_budget,
                            remaining_outer_duration_ns, accepted_at_ns, participant_plans);
}

Result<StopAcceptedDTO> MakeAggregatedStopAccepted(
    const FullStopIntentDTO& intent, StopId stop_id, StopGeneration stop_generation,
    StopScopeResolutionDTO resolution,
    std::vector<StopAffectedOperationRevisionDTO> newly_affected_operation_revisions,
    const StopDeadlineBudgetDTO& fixed_budget,
    const std::int64_t proposed_remaining_outer_duration_ns, const std::int64_t accepted_at_ns,
    std::span<const PreparedParticipantStopPlanDTO> new_participant_plans,
    std::span<const StopAcceptedPredecessorInput> predecessors) {
  const bool aggregate_resolution =
      resolution.disposition == StopResolutionDispositionDTO::kPromotedToFull ||
      ((resolution.disposition == StopResolutionDispositionDTO::kExact ||
        resolution.disposition == StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved) &&
       resolution.reason == StopScopeResolutionReasonDTO::kExplicitFullStop);
  if (!aggregate_resolution || !resolution.full_stop || predecessors.empty() ||
      predecessors.size() > kMaximumStopPredecessors || proposed_remaining_outer_duration_ns <= 0) {
    return Result<StopAcceptedDTO>::Err(
        stop_acceptance_input_error("stop_aggregate_shape_invalid"));
  }

  auto newly_affected_resolution = resolution;
  newly_affected_resolution.operations.clear();
  newly_affected_resolution.operations.reserve(newly_affected_operation_revisions.size());
  for (const auto& affected : newly_affected_operation_revisions) {
    newly_affected_resolution.operations.push_back(affected.operation_id);
  }
  std::sort(newly_affected_resolution.operations.begin(),
            newly_affected_resolution.operations.end());

  auto made = make_stop_accepted(
      intent, stop_id, stop_generation, std::move(newly_affected_resolution),
      newly_affected_operation_revisions, fixed_budget, proposed_remaining_outer_duration_ns,
      accepted_at_ns, new_participant_plans);
  if (!made.ok()) {
    return made;
  }
  auto accepted = std::move(made.value());
  const auto aggregate_scope_digest = ComputeCapturedStopScopeDigest(resolution);
  if (!aggregate_scope_digest.ok()) {
    return Result<StopAcceptedDTO>::Err(aggregate_scope_digest.error());
  }
  accepted.resolution_disposition = resolution.disposition;
  accepted.resolution_reason = resolution.reason;
  accepted.stale_diagnostics = std::move(resolution.stale_diagnostics);
  accepted.captured_scope_digest = aggregate_scope_digest.value();

  std::int64_t aggregate_remaining = proposed_remaining_outer_duration_ns;
  accepted.joined_predecessor_stops.reserve(predecessors.size());
  for (const auto& predecessor : predecessors) {
    const auto& prior = predecessor.acceptance;
    if (const auto field = stop_accepted_field_error(prior); field.has_value()) {
      return Result<StopAcceptedDTO>::Err(
          stop_acceptance_input_error("stop_aggregate_predecessor_invalid"));
    }
    if (!prior.joined_predecessor_stops.empty() || prior.stop_id == stop_id ||
        prior.fixed_outer_duration_ns != fixed_budget.outer_duration_ns ||
        prior.fixed_kernel_completion_reserve_ns != fixed_budget.kernel_completion_reserve_ns ||
        predecessor.remaining_outer_duration_ns_at_join <= 0 ||
        predecessor.remaining_outer_duration_ns_at_join > prior.remaining_outer_duration_ns) {
      return Result<StopAcceptedDTO>::Err(
          stop_acceptance_input_error("stop_aggregate_predecessor_mismatch"));
    }
    accepted.joined_predecessor_stops.push_back({prior.stop_id, prior.stop_generation,
                                                 prior.captured_scope_digest,
                                                 predecessor.remaining_outer_duration_ns_at_join});
    aggregate_remaining =
        std::min(aggregate_remaining, predecessor.remaining_outer_duration_ns_at_join);
    accepted.affected_operation_revisions.insert(accepted.affected_operation_revisions.end(),
                                                 prior.affected_operation_revisions.begin(),
                                                 prior.affected_operation_revisions.end());
    accepted.participant_budget_allocations.insert(accepted.participant_budget_allocations.end(),
                                                   prior.participant_budget_allocations.begin(),
                                                   prior.participant_budget_allocations.end());
  }
  std::sort(accepted.joined_predecessor_stops.begin(), accepted.joined_predecessor_stops.end(),
            [](const auto& left, const auto& right) {
              return left.predecessor_stop_id < right.predecessor_stop_id;
            });
  std::sort(
      accepted.affected_operation_revisions.begin(), accepted.affected_operation_revisions.end(),
      [](const auto& left, const auto& right) { return left.operation_id < right.operation_id; });
  std::sort(accepted.participant_budget_allocations.begin(),
            accepted.participant_budget_allocations.end(), [](const auto& left, const auto& right) {
              return std::tie(left.operation_id, left.participant_id) <
                     std::tie(right.operation_id, right.participant_id);
            });
  accepted.remaining_outer_duration_ns = aggregate_remaining;
  const auto elapsed = accepted.fixed_outer_duration_ns - aggregate_remaining;
  for (auto& participant : accepted.participant_budget_allocations) {
    for (auto& phase : participant.phases) {
      phase.remaining_deadline_offset_ns =
          aggregate_remaining <= accepted.fixed_kernel_completion_reserve_ns
              ? 0
              : std::max<std::int64_t>(
                    0, std::min(phase.fixed_deadline_offset_ns - elapsed,
                                aggregate_remaining - accepted.fixed_kernel_completion_reserve_ns));
    }
  }
  if (!std::equal(resolution.operations.begin(), resolution.operations.end(),
                  accepted.affected_operation_revisions.begin(),
                  accepted.affected_operation_revisions.end(),
                  [](const auto& operation_id, const auto& affected) {
                    return operation_id == affected.operation_id;
                  }) ||
      resolution.operations.size() != accepted.affected_operation_revisions.size()) {
    return Result<StopAcceptedDTO>::Err(
        stop_acceptance_input_error("stop_aggregate_operation_scope_mismatch"));
  }
  if (const auto field = stop_accepted_field_error(accepted); field.has_value()) {
    return Result<StopAcceptedDTO>::Err(stop_acceptance_input_error(std::string(*field)));
  }
  return Result<StopAcceptedDTO>::Ok(std::move(accepted));
}

Result<StopAcceptedDTO> MakeAggregatedStopAccepted(
    const StopIntentDTO& intent, StopId stop_id, StopGeneration stop_generation,
    StopScopeResolutionDTO resolution,
    std::vector<StopAffectedOperationRevisionDTO> newly_affected_operation_revisions,
    const StopDeadlineBudgetDTO& fixed_budget,
    const std::int64_t proposed_remaining_outer_duration_ns, const std::int64_t accepted_at_ns,
    std::span<const PreparedParticipantStopPlanDTO> new_participant_plans,
    std::span<const StopAcceptedPredecessorInput> predecessors) {
  auto accepted = MakeAggregatedStopAccepted(
      FullStopIntentDTO{intent.request_context, stop_generation, intent.attribution},
      std::move(stop_id), stop_generation, std::move(resolution),
      std::move(newly_affected_operation_revisions), fixed_budget,
      proposed_remaining_outer_duration_ns, accepted_at_ns, new_participant_plans, predecessors);
  if (!accepted.ok()) {
    return accepted;
  }
  const auto selector_digest = ComputeRequestedStopSelectorDigest(intent);
  if (!selector_digest.ok()) {
    return Result<StopAcceptedDTO>::Err(selector_digest.error());
  }
  accepted.value().requested_selector_digest = selector_digest.value();
  return accepted;
}

std::optional<std::string_view> stop_accepted_field_error(
    const StopAcceptedDTO& accepted) noexcept {
  if (!bounded_identity(accepted.request_id) ||
      !IsAcceptedRequestReplayStatus(accepted.replay_status) ||
      !bounded_identity(accepted.stop_id) || !accepted.stop_generation.nonzero() ||
      !safe_counter(accepted.stop_generation)) {
    return "stop_accepted_identity_or_generation_invalid";
  }
  if (!content_identity::is_sha256_digest(accepted.requested_selector_digest.value()) ||
      !content_identity::is_sha256_digest(accepted.captured_scope_digest.value())) {
    return "stop_accepted_digest_invalid";
  }
  if (!known_disposition(accepted.resolution_disposition) ||
      !known_reason(accepted.resolution_reason) ||
      !disposition_reason_valid(accepted.resolution_disposition, accepted.resolution_reason)) {
    return "stop_accepted_resolution_invalid";
  }
  if (accepted.stale_diagnostics.size() > kMaximumStopStaleDiagnostics ||
      !std::all_of(accepted.stale_diagnostics.begin(), accepted.stale_diagnostics.end(),
                   diagnostic_shape_valid) ||
      !std::is_sorted(accepted.stale_diagnostics.begin(), accepted.stale_diagnostics.end(),
                      diagnostic_less) ||
      std::adjacent_find(accepted.stale_diagnostics.begin(), accepted.stale_diagnostics.end(),
                         [](const auto& left, const auto& right) {
                           return !diagnostic_less(left, right) && !diagnostic_less(right, left);
                         }) != accepted.stale_diagnostics.end()) {
    return "stop_accepted_stale_diagnostics_invalid";
  }
  if ((accepted.resolution_disposition == StopResolutionDispositionDTO::kExact &&
       !accepted.stale_diagnostics.empty()) ||
      (accepted.resolution_disposition ==
           StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved &&
       accepted.stale_diagnostics.empty())) {
    return "stop_accepted_stale_diagnostics_presence_invalid";
  }
  if (accepted.affected_operation_revisions.size() > kMaximumStopSelectorTargets ||
      !canonical_rows(accepted.affected_operation_revisions,
                      [](const auto& row) { return row.operation_id; }) ||
      !std::all_of(accepted.affected_operation_revisions.begin(),
                   accepted.affected_operation_revisions.end(), [](const auto& row) {
                     return bounded_identity(row.operation_id) &&
                            row.operation_revision_after_acceptance.nonzero() &&
                            safe_counter(row.operation_revision_after_acceptance);
                   })) {
    return "stop_accepted_operation_revisions_invalid";
  }
  if (accepted.joined_predecessor_stops.size() > kMaximumStopPredecessors ||
      !canonical_rows(accepted.joined_predecessor_stops,
                      [](const auto& row) { return row.predecessor_stop_id; }) ||
      !std::all_of(accepted.joined_predecessor_stops.begin(),
                   accepted.joined_predecessor_stops.end(), [&](const auto& row) {
                     return bounded_identity(row.predecessor_stop_id) &&
                            row.predecessor_stop_id != accepted.stop_id &&
                            row.predecessor_stop_generation.nonzero() &&
                            safe_counter(row.predecessor_stop_generation) &&
                            content_identity::is_sha256_digest(row.captured_scope_digest.value()) &&
                            row.remaining_outer_duration_ns_at_join > 0 &&
                            row.remaining_outer_duration_ns_at_join <=
                                accepted.fixed_outer_duration_ns;
                   })) {
    return "stop_accepted_predecessors_invalid";
  }
  if (!accepted.joined_predecessor_stops.empty() &&
      accepted.resolution_disposition != StopResolutionDispositionDTO::kPromotedToFull &&
      !((accepted.resolution_disposition == StopResolutionDispositionDTO::kExact ||
         accepted.resolution_disposition ==
             StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved) &&
        accepted.resolution_reason == StopScopeResolutionReasonDTO::kExplicitFullStop)) {
    return "stop_accepted_predecessor_resolution_invalid";
  }
  if (accepted.resolution_disposition == StopResolutionDispositionDTO::kAlreadyTerminalNoEffect &&
      !accepted.affected_operation_revisions.empty()) {
    return "stop_accepted_no_effect_operation_revision_present";
  }
  if (accepted.fixed_outer_duration_ns <= 0 ||
      accepted.fixed_outer_duration_ns > kMaximumStopDurationNs ||
      accepted.fixed_kernel_completion_reserve_ns <= 0 ||
      accepted.fixed_kernel_completion_reserve_ns >= accepted.fixed_outer_duration_ns ||
      accepted.remaining_outer_duration_ns < 0 ||
      accepted.remaining_outer_duration_ns > accepted.fixed_outer_duration_ns ||
      accepted.accepted_at_ns < 0) {
    return "stop_accepted_outer_budget_invalid";
  }
  if (accepted.participant_budget_allocations.size() > kMaximumStopAcceptedParticipants ||
      !canonical_rows(accepted.participant_budget_allocations, [](const auto& row) {
        return std::tie(row.operation_id, row.participant_id);
      })) {
    return "stop_accepted_participant_budget_invalid";
  }
  std::set<OperationId> budgeted_operations;
  const auto elapsed_ns = accepted.fixed_outer_duration_ns - accepted.remaining_outer_duration_ns;
  for (const auto& participant : accepted.participant_budget_allocations) {
    const auto affected = std::lower_bound(
        accepted.affected_operation_revisions.begin(), accepted.affected_operation_revisions.end(),
        participant.operation_id, [](const auto& row, const OperationId& operation_id) {
          return row.operation_id < operation_id;
        });
    const auto predecessor = std::lower_bound(
        accepted.joined_predecessor_stops.begin(), accepted.joined_predecessor_stops.end(),
        participant.origin_stop_id,
        [](const auto& row, const StopId& stop_id) { return row.predecessor_stop_id < stop_id; });
    const bool current_origin = participant.origin_stop_id == accepted.stop_id &&
                                participant.origin_stop_generation == accepted.stop_generation;
    const bool predecessor_origin =
        predecessor != accepted.joined_predecessor_stops.end() &&
        predecessor->predecessor_stop_id == participant.origin_stop_id &&
        predecessor->predecessor_stop_generation == participant.origin_stop_generation;
    if ((!current_origin && !predecessor_origin) || !bounded_identity(participant.origin_stop_id) ||
        !participant.origin_stop_generation.nonzero() ||
        !safe_counter(participant.origin_stop_generation) ||
        !bounded_identity(participant.operation_id) ||
        !bounded_identity(participant.participant_id) ||
        !content_identity::is_sha256_digest(
            participant.participant_stop_invocation_plan_digest.value()) ||
        affected == accepted.affected_operation_revisions.end() ||
        affected->operation_id != participant.operation_id || participant.phases.empty() ||
        participant.phases.size() > kMaximumStopAcceptedPhases) {
      return "stop_accepted_participant_budget_invalid";
    }
    budgeted_operations.insert(participant.operation_id);
    std::set<StopBudgetPhaseId> phase_ids;
    std::int64_t prior_fixed = 0;
    for (const auto& phase : participant.phases) {
      const auto remaining_phase_ns =
          accepted.remaining_outer_duration_ns <= accepted.fixed_kernel_completion_reserve_ns
              ? 0
              : std::max<std::int64_t>(0,
                                       std::min(phase.fixed_deadline_offset_ns - elapsed_ns,
                                                accepted.remaining_outer_duration_ns -
                                                    accepted.fixed_kernel_completion_reserve_ns));
      if (!bounded_identity(phase.phase_id) || !phase_ids.insert(phase.phase_id).second ||
          phase.fixed_deadline_offset_ns <= prior_fixed ||
          phase.fixed_deadline_offset_ns >
              accepted.fixed_outer_duration_ns - accepted.fixed_kernel_completion_reserve_ns ||
          phase.remaining_deadline_offset_ns != remaining_phase_ns) {
        return "stop_accepted_phase_budget_invalid";
      }
      prior_fixed = phase.fixed_deadline_offset_ns;
    }
  }
  if (std::any_of(accepted.joined_predecessor_stops.begin(),
                  accepted.joined_predecessor_stops.end(), [&](const auto& predecessor) {
                    return accepted.remaining_outer_duration_ns >
                           predecessor.remaining_outer_duration_ns_at_join;
                  })) {
    return "stop_accepted_predecessor_deadline_widened";
  }
  if (budgeted_operations.size() != accepted.affected_operation_revisions.size() ||
      !std::equal(budgeted_operations.begin(), budgeted_operations.end(),
                  accepted.affected_operation_revisions.begin(),
                  [](const OperationId& operation_id, const auto& affected) {
                    return operation_id == affected.operation_id;
                  })) {
    return "stop_accepted_participant_budget_coverage_invalid";
  }
  return std::nullopt;
}

}  // namespace manyforge::core
