#pragma once

#include <cstdint>
#include <limits>
#include <optional>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

/// JSON forms of contract 1.5: a participant is `{operation_id, ordinal}`, a
/// controller goal `{operation_id, ordinal, generation}`, every member a JSON
/// integer within the Kernel's JSON integer bound and nonzero.
inline constexpr std::uint64_t kOperationMemberIdJsonIntegerLimit = 9'007'199'254'740'991ULL;

[[nodiscard]] inline content_identity::CanonicalValue OperationParticipantIdValue(
    const OperationParticipantId& id) {
  using content_identity::CanonicalValue;
  return CanonicalValue(CanonicalValue::Object{
      {"operation_id", CanonicalValue(static_cast<double>(id.operation_id.value()))},
      {"ordinal", CanonicalValue(static_cast<double>(id.ordinal))}});
}

[[nodiscard]] inline content_identity::CanonicalValue ControllerGoalIdValue(
    const ControllerGoalId& id) {
  using content_identity::CanonicalValue;
  return CanonicalValue(CanonicalValue::Object{
      {"generation", CanonicalValue(static_cast<double>(id.generation))},
      {"operation_id", CanonicalValue(static_cast<double>(id.operation_id.value()))},
      {"ordinal", CanonicalValue(static_cast<double>(id.ordinal))}});
}

namespace operation_member_id_json_detail {

[[nodiscard]] inline std::optional<std::uint64_t> PositiveInteger(
    const content_identity::CanonicalValue* value, std::uint64_t limit) {
  if (value == nullptr || !value->is_number()) return std::nullopt;
  const double number = value->as_number();
  if (!(number >= 1.0) || number > static_cast<double>(limit)) return std::nullopt;
  const auto integer = static_cast<std::uint64_t>(number);
  if (static_cast<double>(integer) != number) return std::nullopt;
  return integer;
}

}  // namespace operation_member_id_json_detail

/// Exact closed shape; nullopt for anything else.
[[nodiscard]] inline std::optional<OperationParticipantId> ParseOperationParticipantId(
    const content_identity::CanonicalValue& value) {
  using operation_member_id_json_detail::PositiveInteger;
  if (!value.is_object() || value.as_object().size() != 2U) return std::nullopt;
  const auto operation =
      PositiveInteger(value.find_object_member("operation_id"), kOperationMemberIdJsonIntegerLimit);
  const auto ordinal = PositiveInteger(value.find_object_member("ordinal"),
                                       std::numeric_limits<std::uint32_t>::max());
  if (!operation || !ordinal) return std::nullopt;
  return OperationParticipantId{OperationNumber(*operation), static_cast<std::uint32_t>(*ordinal)};
}

/// Exact closed shape; nullopt for anything else.
[[nodiscard]] inline std::optional<ControllerGoalId> ParseControllerGoalId(
    const content_identity::CanonicalValue& value) {
  using operation_member_id_json_detail::PositiveInteger;
  if (!value.is_object() || value.as_object().size() != 3U) return std::nullopt;
  const auto operation =
      PositiveInteger(value.find_object_member("operation_id"), kOperationMemberIdJsonIntegerLimit);
  const auto ordinal = PositiveInteger(value.find_object_member("ordinal"),
                                       std::numeric_limits<std::uint32_t>::max());
  const auto generation = PositiveInteger(value.find_object_member("generation"),
                                          std::numeric_limits<std::uint32_t>::max());
  if (!operation || !ordinal || !generation) return std::nullopt;
  return ControllerGoalId{OperationNumber(*operation), static_cast<std::uint32_t>(*ordinal),
                          static_cast<std::uint32_t>(*generation)};
}

}  // namespace manyforge::core
