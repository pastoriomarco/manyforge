#include "io_blackboard_snapshot.hpp"

#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string_view>

namespace manyforge::core::detail {
namespace {
using Snapshot = IoProgramBlackboardSnapshotDTO;
using Reply = Result<std::shared_ptr<const Snapshot>>;
using Descriptors = std::map<std::string_view, IoConditionValueKind>;
using Values = std::map<std::string_view, const IoBlackboardValueDTO*>;

Reply invalid(const char* reason) {
  return Reply::Err(
      {"invalid_request", "Program blackboard snapshot is invalid", {{"reason", reason}}});
}
Reply stale() {
  return Reply::Err({"stale_generation",
                     "Program blackboard scope is not current",
                     {{"reason", "program_bb_scope_stale"}}});
}

bool text_valid(const std::string& text) {
  return !text.empty() && text.find('\0') == std::string::npos;
}
bool kind_valid(IoConditionValueKind kind) {
  switch (kind) {
    case IoConditionValueKind::kBoolean:
    case IoConditionValueKind::kInt64:
    case IoConditionValueKind::kFloat64:
    case IoConditionValueKind::kString:
      return true;
  }
  return false;
}
bool scope_valid(const IoProgramBlackboardScopeDTO& scope) {
  return text_valid(scope.request_owner_epoch.value()) &&
         text_valid(scope.program_runner_owner_epoch.value()) &&
         text_valid(scope.program_binding_id.value()) && scope.cycle_generation != 0U &&
         scope.active_publication_generation.nonzero() &&
         scope.claim_generation.has_value() == scope.program_run_id.has_value() &&
         (!scope.claim_generation || scope.claim_generation->nonzero()) &&
         (!scope.program_run_id || text_valid(scope.program_run_id->value()));
}

bool consume(std::size_t amount, std::size_t maximum, std::size_t& used) {
  if (amount > maximum - used) return false;
  used += amount;
  return true;
}

const char* validate_value(const IoBlackboardValueDTO& row, const Descriptors& descriptors) {
  const auto key = descriptors.find(row.name);
  if (key == descriptors.end()) return "program_bb_key_union_mismatch";
  if (row.value_kind != key->second) return "program_bb_value_kind_mismatch";
  if (!row.value) return nullptr;
  const bool exact = [&] {
    switch (row.value_kind) {
      case IoConditionValueKind::kBoolean:
        return std::holds_alternative<bool>(*row.value);
      case IoConditionValueKind::kInt64:
        return std::holds_alternative<std::int64_t>(*row.value);
      case IoConditionValueKind::kFloat64:
        return std::holds_alternative<double>(*row.value);
      case IoConditionValueKind::kString:
        return std::holds_alternative<std::string>(*row.value);
    }
    return false;
  }();
  if (!exact) return "program_bb_value_type_mismatch";
  if (const auto* number = std::get_if<double>(&*row.value); number && !std::isfinite(*number))
    return "program_bb_value_nonfinite";
  return nullptr;
}

std::size_t payload_bytes(const IoBlackboardValueDTO& row) {
  if (!row.value) return 0U;
  switch (row.value_kind) {
    case IoConditionValueKind::kBoolean:
      return 1U;
    case IoConditionValueKind::kInt64:
    case IoConditionValueKind::kFloat64:
      return 8U;
    case IoConditionValueKind::kString:
      return std::get<std::string>(*row.value).size();
  }
  return 0U;  // reached only after closed-kind validation
}
}  // namespace

Reply ValidateAndBuildProgramBlackboardSnapshot(
    const IoProgramBlackboardSourceBindingDTO& binding,
    const std::shared_ptr<const Snapshot>& previous,
    const IoProgramBlackboardPublicationDTO& publication) {
  if (!scope_valid(binding.scope) || !text_valid(binding.source_id.value()) ||
      !text_valid(binding.incarnation.value()) || binding.maximum_value_bytes == 0U)
    return invalid("program_bb_binding_invalid");
  if (publication.scope != binding.scope) return stale();
  if (publication.trusted_received_at_ns < 0) return invalid("program_bb_receipt_invalid");

  // Bound key names before allocating the lookup. Nonempty names also bound
  // the number of map entries under the trusted aggregate capacity.
  std::size_t name_bytes = 0U;
  for (const auto& key : binding.keys) {
    if (!text_valid(key.name) || !kind_valid(key.value_kind))
      return invalid("program_bb_key_descriptor_invalid");
    if (!consume(key.name.size(), binding.maximum_value_bytes, name_bytes))
      return invalid("program_bb_value_bound_exceeded");
  }
  Descriptors descriptors;
  for (const auto& key : binding.keys)
    if (!descriptors.emplace(key.name, key.value_kind).second)
      return invalid("program_bb_duplicate_key");

  Values candidate;
  if (!previous) {
    if (publication.kind != IoBlackboardPublicationKind::kBaseline ||
        publication.message_sequence != 1U || publication.previous_sequence != 0U)
      return invalid("program_bb_baseline_required");
    if (publication.values.size() != binding.keys.size())
      return invalid("program_bb_key_union_mismatch");
  } else {
    if (previous->scope != binding.scope || previous->source_id != binding.source_id ||
        previous->incarnation != binding.incarnation)
      return stale();
    if (previous->message_sequence == 0U || previous->trusted_received_at_ns < 0)
      return invalid("program_bb_previous_snapshot_invalid");
    if (publication.kind != IoBlackboardPublicationKind::kUpdate)
      return invalid("program_bb_update_required");
    if (previous->message_sequence == std::numeric_limits<std::uint64_t>::max())
      return invalid("program_bb_sequence_exhausted");
    if (publication.previous_sequence != previous->message_sequence ||
        publication.message_sequence != previous->message_sequence + 1U)
      return invalid("program_bb_sequence_mismatch");
    if (publication.values.empty() || publication.values.size() > binding.keys.size() ||
        previous->values.size() != binding.keys.size())
      return invalid("program_bb_key_union_mismatch");
    std::size_t previous_bytes = name_bytes;
    for (const auto& row : previous->values) {
      if (const auto* reason = validate_value(row, descriptors)) return invalid(reason);
      if (!consume(payload_bytes(row), binding.maximum_value_bytes, previous_bytes))
        return invalid("program_bb_value_bound_exceeded");
      if (!candidate.emplace(row.name, &row).second) return invalid("program_bb_duplicate_key");
    }
  }

  std::set<std::string_view> changed;
  for (const auto& row : publication.values) {
    if (const auto* reason = validate_value(row, descriptors)) return invalid(reason);
    if (!changed.insert(row.name).second) return invalid("program_bb_duplicate_key");
    candidate.insert_or_assign(row.name, &row);
  }
  if (candidate.size() != descriptors.size()) return invalid("program_bb_key_union_mismatch");

  // Preflight the proposed WHOLE retained state, not just this frame. Earlier
  // successful updates cannot accumulate arbitrarily large sibling strings.
  std::size_t bytes = name_bytes;
  for (const auto& [name, row] : candidate) {
    (void)name;
    if (!consume(payload_bytes(*row), binding.maximum_value_bytes, bytes))
      return invalid("program_bb_value_bound_exceeded");
  }
  std::vector<IoBlackboardValueDTO> values;
  values.reserve(candidate.size());
  for (const auto& [name, row] : candidate) {
    (void)name;
    values.push_back(*row);
  }
  return Reply::Ok(std::make_shared<const Snapshot>(
      Snapshot{binding.scope, binding.source_id, binding.incarnation, publication.message_sequence,
               publication.trusted_received_at_ns, std::move(values)}));
}

}  // namespace manyforge::core::detail
