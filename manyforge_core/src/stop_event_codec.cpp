#include <manyforge_core/stop_event_codec.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/error_catalog.generated.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;

void Require(const bool condition, const char* message) {
  if (!condition) throw std::invalid_argument(message);
}

[[nodiscard]] CanonicalValue Integer(const std::uint64_t value) {
  Require(value <= kMaximumSafeInteger, "Stop event integer exceeds the lossless I-JSON range");
  return CanonicalValue(static_cast<double>(value));
}

/// Kernel monotonic time, as operation rows write it: a number up to
/// 2^53 - 1 ns (about 104 days of uptime), canonical decimal text above.
[[nodiscard]] CanonicalValue Time(const std::int64_t value) {
  Require(value >= 0, "Stop event time is negative");
  return content_identity::nonnegative_int64_value(value);
}

[[nodiscard]] CanonicalValue Token(const std::string_view token) {
  Require(!token.empty(), "Stop event enum value has no wire token");
  return CanonicalValue(std::string(token));
}

[[nodiscard]] CanonicalValue Text(const std::string_view text) {
  return CanonicalValue(std::string(text));
}

[[nodiscard]] std::string_view TransportToken(const RequestTransportDTO value) noexcept {
  switch (value) {
    case RequestTransportDTO::kRos:
      return "ros";
    case RequestTransportDTO::kHttp:
      return "http";
    case RequestTransportDTO::kInternal:
      return "internal";
  }
  return {};
}

[[nodiscard]] std::string_view AssuranceToken(const RequestAssuranceDTO value) noexcept {
  switch (value) {
    case RequestAssuranceDTO::kUnverified:
      return "unverified";
    case RequestAssuranceDTO::kExternallyAuthenticated:
      return "externally_authenticated";
  }
  return {};
}

[[nodiscard]] CanonicalValue ErrorCodeValue(const std::uint16_t code) {
  const auto text = StopEventErrorCodeText(code);
  return text ? Text(*text) : CanonicalValue(nullptr);
}

/// Group names: the record keeps indexes into the capture's named
/// selections; the built-in `all` selector is not stored there (it is the
/// complete node list), so the one index past the named selections names it.
[[nodiscard]] CanonicalValue GroupNames(const StopEventRecord& record) {
  CanonicalValue::Array names;
  const auto stored = std::min<std::size_t>(record.group_count, kStopEventGroupSlots);
  Require(stored == 0U || (record.capture != nullptr && record.capture->table != nullptr),
          "Stop event names groups without the capture that names them");
  names.reserve(stored);
  for (std::size_t index = 0U; index < stored; ++index) {
    const auto& selections = record.capture->table->named_selections;
    const auto group = record.groups[index];
    Require(group <= selections.size(), "Stop event group index is outside its capture");
    names.emplace_back(group == selections.size() ? std::string("all")
                                                  : selections[group].group_id);
  }
  return CanonicalValue(std::move(names));
}

[[nodiscard]] CanonicalValue HoldChange(const std::uint8_t bits) {
  CanonicalValue::Array tokens;
  for (const auto& [bit, token] :
       {std::pair{StopHoldChange::kNodes, "nodes"}, std::pair{StopHoldChange::kAllGroups, "all_groups"},
        std::pair{StopHoldChange::kFrozen, "frozen"}, std::pair{StopHoldChange::kRule, "rule"}}) {
    if ((bits & static_cast<std::uint8_t>(bit)) != 0U) tokens.emplace_back(token);
  }
  return CanonicalValue(std::move(tokens));
}

[[nodiscard]] std::span<const StopCoveredEntry> Covered(const StopEventRecord& record) {
  Require(record.covered_count <= record.covered.size(),
          "Stop event covered count exceeds its entries");
  return {record.covered.data(), record.covered_count};
}

[[nodiscard]] CanonicalValue::Object Common(const StopEventRecord& record, const char* row) {
  Require(record.event_id.nonzero(), "Stop event record is empty");
  return CanonicalValue::Object{
      {"schema_version", Text(kStopEventPayloadSchema)},
      {"row", CanonicalValue(row)},
      {"event_id", Integer(record.event_id.value())},
      {"applied", Token(to_string(record.applied))},
      {"accepted_at_ns", Time(record.accepted_at_ns)},
  };
}

}  // namespace

std::optional<std::string_view> StopEventErrorCodeText(const std::uint16_t code) noexcept {
  if (code == 0U) return std::nullopt;
  if (code == kStopErrorCodeUncatalogued) return std::string_view("uncatalogued");
  return error_codes::kSortedCodes[code - 1U];
}

std::string EncodeStopEventAccepted(const StopEventRecord& record) {
  auto payload = Common(record, "accepted");

  CanonicalValue caused_by(nullptr);
  if (record.requested == StopKind::kStopFailedHalt) {
    caused_by = CanonicalValue(CanonicalValue::Object{
        {"event_id", Integer(record.caused_by.event_id.value())},
        {"operation_id", Integer(record.caused_by.operation_id.value())},
        {"participant_ordinal", Integer(record.caused_by.participant_ordinal)},
    });
  }

  CanonicalValue::Array covered;
  for (const auto& entry : Covered(record)) {
    covered.emplace_back(CanonicalValue::Object{
        {"operation_id", Integer(entry.operation_id.value())},
        {"participant_ordinal", Integer(entry.participant_ordinal)},
        {"started", CanonicalValue(entry.started)},
    });
  }

  Require(record.stale_count <= record.stale.size() && record.stale_count <= record.stale_total,
          "Stop event stale count exceeds its entries");
  CanonicalValue::Array stale;
  stale.reserve(record.stale_count);
  for (std::size_t index = 0U; index < record.stale_count; ++index) {
    const auto& entry = record.stale[index];
    stale.emplace_back(CanonicalValue::Object{
        {"operation_id", Integer(entry.operation_id.value())},
        {"expected_operation_revision", Integer(entry.expected.value())},
        {"current_operation_revision", Integer(entry.actual.value())},
    });
  }

  payload.emplace_back("request_key", Text(RequestKeyText(record.request_key)));
  payload.emplace_back("requested", Token(to_string(record.requested)));
  payload.emplace_back("widen_reason", Token(to_string(record.widen_reason)));
  payload.emplace_back("caused_by", std::move(caused_by));
  payload.emplace_back(
      "attribution",
      CanonicalValue(CanonicalValue::Object{
          {"transport", Token(TransportToken(record.transport))},
          {"assurance", Token(AssuranceToken(record.assurance))},
          {"principal_id", record.principal.length == 0U ? CanonicalValue(nullptr)
                                                         : Text(record.principal.view())},
      }));
  payload.emplace_back("source_label", Text(record.source_label.view()));
  payload.emplace_back("reason", Text(record.reason.view()));
  payload.emplace_back("program_run_id", record.run_number.nonzero()
                                             ? Integer(record.run_number.value())
                                             : CanonicalValue(nullptr));
  payload.emplace_back("groups", GroupNames(record));
  payload.emplace_back("group_count", Integer(record.group_count));
  payload.emplace_back("hold_change", HoldChange(record.hold_change));
  payload.emplace_back("hold_revision_after", Integer(record.hold_revision_after));
  payload.emplace_back("covered", CanonicalValue(std::move(covered)));
  payload.emplace_back("stale_diagnostics", CanonicalValue(std::move(stale)));
  payload.emplace_back("stale_total", Integer(record.stale_total));
  payload.emplace_back("fence_degraded",
                       record.fence_degraded
                           ? CanonicalValue(CanonicalValue::Object{
                                 {"reason", ErrorCodeValue(record.fence_degraded_reason)}})
                           : CanonicalValue(nullptr));
  return content_identity::canonical_json(CanonicalValue(std::move(payload)));
}

std::string EncodeStopEventCompleted(const StopEventRecord& record) {
  Require(record.result != StopEventResult::kPending &&
              record.completed_at_ns >= record.accepted_at_ns,
          "Stop event record is not complete");
  auto payload = Common(record, "completed");
  CanonicalValue::Array covered;
  for (const auto& entry : Covered(record)) {
    const bool contained = entry.containing_stage != kNoStopStage;
    covered.emplace_back(CanonicalValue::Object{
        {"operation_id", Integer(entry.operation_id.value())},
        {"participant_ordinal", Integer(entry.participant_ordinal)},
        {"started", CanonicalValue(entry.started)},
        {"phase", Token(to_string(entry.phase))},
        {"containing_stage_index",
         contained ? Integer(entry.containing_stage) : CanonicalValue(nullptr)},
        {"containing_stage_kind",
         contained ? Token(to_string(entry.containing_stage_kind)) : CanonicalValue(nullptr)},
        {"escalated", CanonicalValue(entry.escalated)},
        {"stop_path_collision", CanonicalValue(entry.stop_path_collision)},
        {"error_code", ErrorCodeValue(entry.error_code)},
    });
  }
  payload.emplace_back("result", Token(to_string(record.result)));
  payload.emplace_back("completed_at_ns", Time(record.completed_at_ns));
  payload.emplace_back("covered", CanonicalValue(std::move(covered)));
  return content_identity::canonical_json(CanonicalValue(std::move(payload)));
}

}  // namespace manyforge::core
