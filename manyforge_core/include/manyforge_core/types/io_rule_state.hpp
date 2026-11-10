#pragma once

#include <cstdint>
#include <manyforge_core/types/io_rule.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

/// State key `interrupt_state.<rule_id>`: `inactive | clear |
/// tripped`. `kUnknown` is an active rule that can't watch its signals: the
/// key is withdrawn (absent) and the rule gates its scope.
enum class IoGlobalRuleState { kInactive, kClear, kTripped, kUnknown };

/// Exact installed activation, independent of a Run or operation token. These
/// are existing owner/catalog/publication identities, never a new cursor.
struct IoRuleStateContextDTO final {
  RequestOwnerEpoch owner_epoch;
  std::uint64_t catalog_generation;
  ActiveBundlePublicationGeneration active_publication_generation{0U};
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  friend bool operator==(const IoRuleStateContextDTO&, const IoRuleStateContextDTO&) = default;
};

struct IoRuleStateDTO final {
  InterruptPolicyId rule_id;
  IoGlobalRuleState state;
  /// Actual retained causal acquisition only. Baseline matches and clear
  /// transitions do not manufacture a source event.
  std::optional<IoRuleAcquisitionKeyDTO> triggering_acquisition{};
  /// Typed cause while the rule can't watch (`kUnknown`); absent otherwise.
  std::optional<ErrorDTO> unavailable_reason{};
  friend bool operator==(const IoRuleStateDTO&, const IoRuleStateDTO&) = default;
};

struct IoRuleStateChangeDTO final {
  IoRuleStateContextDTO context;
  IoRuleStateDTO rule;
  TimePoint occurred_at_ns;
  friend bool operator==(const IoRuleStateChangeDTO&, const IoRuleStateChangeDTO&) = default;
};

/// Captured atomically by the existing Kernel activation owner. Mirror this
/// baseline before same-context journal events after through_sequence; neither
/// silence nor an absent catalog can be interpreted as fabricated clear.
struct IoRuleStateBaselineDTO final {
  IoRuleStateContextDTO context;
  std::uint64_t through_sequence;
  std::vector<IoRuleStateDTO> states;
  friend bool operator==(const IoRuleStateBaselineDTO&, const IoRuleStateBaselineDTO&) = default;
};

inline std::string_view io_global_rule_state_token(IoGlobalRuleState state) noexcept {
  switch (state) {
    case IoGlobalRuleState::kInactive:
      return "inactive";
    case IoGlobalRuleState::kClear:
      return "clear";
    case IoGlobalRuleState::kTripped:
      return "tripped";
    case IoGlobalRuleState::kUnknown:
      return "unknown";
  }
  return {};
}

inline std::optional<std::string_view> io_rule_state_context_field_error(
    const IoRuleStateContextDTO& context) noexcept {
  const auto identity = [](const auto& id) {
    return !id.empty() && id.value().find('\0') == std::string::npos;
  };
  if (!identity(context.owner_epoch) || !identity(context.retained_bundle_id) ||
      !identity(context.graph_revision) || context.catalog_generation == 0U ||
      !context.active_publication_generation.nonzero() ||
      context.active_publication_generation.value() > 9'007'199'254'740'991ULL)
    return "io_rule_state_context_invalid";
  return std::nullopt;
}

inline std::optional<std::string_view> io_rule_state_field_error(
    const IoRuleStateContextDTO& context, const IoRuleStateDTO& row) noexcept {
  if (const auto error = io_rule_state_context_field_error(context)) return error;
  if (row.rule_id.empty() || row.rule_id.value().find('\0') != std::string::npos ||
      io_global_rule_state_token(row.state).empty() ||
      (row.state != IoGlobalRuleState::kTripped && row.triggering_acquisition) ||
      (row.state == IoGlobalRuleState::kUnknown) != row.unavailable_reason.has_value() ||
      (row.unavailable_reason && row.unavailable_reason->code.empty()))
    return "io_rule_state_row_invalid";
  if (row.triggering_acquisition) {
    const auto& acquired = *row.triggering_acquisition;
    if (acquired.source_id.empty() || acquired.incarnation.empty() ||
        acquired.source_id.value().find('\0') != std::string::npos ||
        acquired.incarnation.value().find('\0') != std::string::npos ||
        acquired.source_sequence == 0U || acquired.source_cursor.record_sequence == 0U ||
        acquired.source_cursor.source_id != acquired.source_id ||
        acquired.source_cursor.owner_epoch != context.owner_epoch ||
        acquired.source_cursor.catalog_generation != context.catalog_generation)
      return "io_rule_state_acquisition_invalid";
  }
  return std::nullopt;
}

inline std::optional<std::string_view> io_rule_state_baseline_field_error(
    const IoRuleStateBaselineDTO& baseline) noexcept {
  if (const auto error = io_rule_state_context_field_error(baseline.context)) return error;
  if (baseline.through_sequence > 9'007'199'254'740'991ULL) return "io_rule_state_sequence_invalid";
  const IoRuleStateDTO* previous = nullptr;
  for (const auto& row : baseline.states) {
    if (const auto error = io_rule_state_field_error(baseline.context, row)) return error;
    if (previous && previous->rule_id >= row.rule_id) return "io_rule_state_rows_not_canonical";
    previous = &row;
  }
  return std::nullopt;
}

}  // namespace manyforge::core
