#include <algorithm>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/operator_alarm_attention.hpp>
#include <set>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

[[nodiscard]] Result<void> invalid(std::string reason) {
  return Result<void>::Err(ErrorDTO{"invalid_request",
                                    "operator-alarm attention snapshot is invalid",
                                    {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool bounded_text(const std::string& value) noexcept {
  return !value.empty() && value.size() <= kMaximumOperatorAlarmAttentionTextBytes;
}

/// The canonical JSON membership, bounded by whole entries in condition-id
/// order; `truncated` is set when an entry was omitted.
[[nodiscard]] std::string membership_json(
    const std::vector<const OperatorAlarmAttentionMemberDTO*>& members, bool& truncated) {
  std::string json = "[]";
  std::size_t index = 0U;
  for (const auto* member : members) {
    auto row = content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
        {"activation_id", CanonicalValue(member->activation_id)},
        {"condition_id", CanonicalValue(member->condition_id)},
        {"record_revision", CanonicalValue(static_cast<double>(member->record_revision))},
    }));
    // `[` + rows joined by `,` + `]`.
    if (json.size() + row.size() + (index == 0U ? 0U : 1U) >
        kMaximumOperatorAlarmRefusalMembershipBytes) {
      truncated = true;
      break;
    }
    json.insert(json.size() - 1U, (index == 0U ? "" : ",") + row);
    ++index;
  }
  return json;
}

[[nodiscard]] ErrorDTO unavailable(std::string availability, std::string message,
                                   const OperatorAlarmAttentionSnapshotDTO* snapshot) {
  ErrorDTO refusal{std::string(kOperatorAlarmStateUnavailableReason),
                   std::move(message),
                   {{"reason", std::string(kOperatorAlarmStateUnavailableReason)},
                    {"availability", std::move(availability)},
                    {"interlock_alarms", "[]"}}};
  if (snapshot != nullptr) {
    refusal.details.emplace("owner_epoch", snapshot->owner_epoch);
    refusal.details.emplace("generation", std::to_string(snapshot->generation));
  }
  return refusal;
}

}  // namespace

Result<void> validate_operator_alarm_attention_snapshot(
    const OperatorAlarmAttentionSnapshotDTO& snapshot) {
  if (!bounded_text(snapshot.cell_id.value()) || !bounded_text(snapshot.owner_epoch) ||
      !bounded_text(snapshot.owner_host_boot_id) || snapshot.owner_incarnation_us == 0U ||
      (snapshot.active_configuration.has_value() &&
       (!bounded_text(snapshot.active_configuration->retained_bundle_id.value()) ||
        !bounded_text(snapshot.active_configuration->graph_revision.value())))) {
    return invalid("operator_alarm_attention_identity_invalid");
  }
  if (snapshot.members.size() > kMaximumOperatorAlarmAttentionMembers) {
    return invalid("operator_alarm_attention_membership_oversized");
  }
  const OperatorAlarmAttentionMemberDTO* previous = nullptr;
  for (const auto& member : snapshot.members) {
    if (!bounded_text(member.condition_id) || !bounded_text(member.activation_id) ||
        member.record_revision == 0U || !bounded_text(member.subject_kind) ||
        !bounded_text(member.subject_id)) {
      return invalid("operator_alarm_attention_member_invalid");
    }
    if (previous != nullptr && !(previous->condition_id < member.condition_id)) {
      return invalid("operator_alarm_attention_membership_unsorted");
    }
    previous = &member;
    if (member.resource_ids.size() > kMaximumOperatorAlarmSubjectResources) {
      return invalid("operator_alarm_attention_subject_oversized");
    }
    for (std::size_t index = 0U; index < member.resource_ids.size(); ++index) {
      if (!bounded_text(member.resource_ids[index].value()) ||
          (index > 0U && !(member.resource_ids[index - 1U] < member.resource_ids[index]))) {
        return invalid("operator_alarm_attention_subject_invalid");
      }
    }
    // spec 530 §5: the cell subject is the only resource-free subject.
    const bool cell_subject = member.subject_kind == "cell";
    if (cell_subject != member.resource_ids.empty() ||
        (cell_subject && member.subject_id != snapshot.cell_id.value())) {
      return invalid("operator_alarm_attention_subject_invalid");
    }
  }
  return Result<void>::Ok();
}

std::optional<ErrorDTO> evaluate_operator_alarm_motion_attention(
    const std::optional<RetainedOperatorAlarmAttentionDTO>& retained,
    const OperatorAlarmAttentionPolicyDTO& policy, const CellId& cell_id,
    const std::optional<OperatorAlarmAttentionConfigurationDTO>& active_configuration,
    const TimePoint now_ns, const std::vector<ResourceId>& reserved_resources) {
  if (!retained.has_value()) {
    return unavailable("missing", "no operator-alarm attention projection has been received",
                       nullptr);
  }
  const auto& snapshot = retained->snapshot;
  if (snapshot.cell_id != cell_id) {
    return unavailable("cell_mismatch",
                       "the operator-alarm attention projection names another cell", &snapshot);
  }
  if (!snapshot.reconciled) {
    return unavailable("unreconciled",
                       "the Alarm Lifecycle Owner is unreconciled or unhealthy", &snapshot);
  }
  // Subjects were judged against the owner's resource registry. Under any
  // other configuration (an Apply the owner has not observed yet, e.g. one
  // that renamed resources) they say nothing trustworthy about this Kernel's
  // resources, so attention is unavailable until the owner catches up.
  if (!active_configuration.has_value() ||
      snapshot.active_configuration != active_configuration) {
    return unavailable("configuration_mismatch",
                       "the Alarm Lifecycle Owner has not observed the Kernel's active "
                       "configuration",
                       &snapshot);
  }
  if (now_ns < retained->received_at_ns ||
      now_ns - retained->received_at_ns > policy.maximum_snapshot_age_ns) {
    return unavailable("stale", "the operator-alarm attention projection is stale", &snapshot);
  }
  const std::set<ResourceId> reserved(reserved_resources.begin(), reserved_resources.end());
  std::vector<const OperatorAlarmAttentionMemberDTO*> interlocks;
  for (const auto& member : snapshot.members) {
    const bool cell_global = member.subject_kind == "cell" &&
                             member.subject_id == cell_id.value() && member.resource_ids.empty();
    const bool intersects =
        std::any_of(member.resource_ids.begin(), member.resource_ids.end(),
                    [&](const ResourceId& resource) { return reserved.contains(resource); });
    if (cell_global || intersects) interlocks.push_back(&member);
  }
  if (interlocks.empty()) return std::nullopt;
  const std::string reason(kOperatorAlarmInterlockActiveReason);
  bool truncated = false;
  auto interlock_alarms = membership_json(interlocks, truncated);
  ErrorDTO refusal{
      reason,
      "an active operator-alarm condition interlock gates this motion; resolve its cause",
      {{"reason", reason},
       {"owner_epoch", snapshot.owner_epoch},
       {"generation", std::to_string(snapshot.generation)},
       {"interlock_alarms", std::move(interlock_alarms)}}};
  if (truncated) refusal.details.emplace("alarm_membership_truncated", "true");
  return refusal;
}

}  // namespace manyforge::core
