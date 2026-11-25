#pragma once

// The Manager's `scene_changes` status section (OD-400): the last scene
// commit outcomes its command lane decided outside a Program node attempt,
// in memory only, for History. Headless records each record once from the
// status stream, whoever made the change.

#include "manyforge_headless/source_contract.hpp"

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/scene_transaction.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace manyforge::headless::detail {

/// The bounds of one record's texts, in bytes of their JSON encoding (so a
/// text full of escaped characters is cut earlier): History's bounds of an
/// error code, an error message and a caller or object text. An error's
/// details are kept only when their canonical encoding fits their bound, as
/// History keeps them.
inline constexpr std::size_t kSceneChangeCodeBytes = 256U;
inline constexpr std::size_t kSceneChangeTextBytes = 2048U;
inline constexpr std::size_t kSceneChangeDetailsBytes = 2048U;
inline constexpr std::size_t kSceneChangeNameBytes = 256U;
inline constexpr std::size_t kSceneChangeRequestIdBytes = 64U;

/// The largest encoded record: the bounded texts with their quotes, the
/// longest operation kind, outcome and transport, three numbers of at most
/// 24 bytes each, and 300 bytes for the member names and punctuation.
inline constexpr std::size_t kSceneChangeRecordMaximumBytes =
    (kSceneChangeCodeBytes + 2U) + (kSceneChangeTextBytes + 2U) +
    kSceneChangeDetailsBytes + 3U * (kSceneChangeNameBytes + 2U) +
    (kSceneChangeRequestIdBytes + 2U) +
    (std::string_view("detach_carried_object_at_current_pose").size() + 2U) +
    (std::string_view("succeeded").size() + 2U) + (std::string_view("http").size() + 2U) +
    3U * 24U + 300U;
/// The largest section payload: `{"records":[` and `]}`, the records and
/// their commas. It always fits one status stream frame (OD-400).
inline constexpr std::size_t kSceneChangesPayloadMaximumBytes =
    14U + kSceneChangeRingCapacity * (kSceneChangeRecordMaximumBytes + 1U);

/// The JSON-encoded length of one UTF-8 sequence starting with `byte`
/// (RFC 8785: two-character escapes, `\u00XX` for other control bytes).
[[nodiscard]] constexpr std::size_t SceneChangeEncodedBytes(const unsigned char byte) {
  switch (byte) {
  case 0x08: case 0x09: case 0x0a: case 0x0c: case 0x0d: case '"': case '\\':
    return 2U;
  default:
    return byte <= 0x1fU ? 6U : 1U;
  }
}

/// `text` cut at a code point boundary so that its JSON encoding (without
/// quotes) is at most `limit` bytes; a text without escaped characters is
/// cut at `limit` bytes, as History cuts it.
[[nodiscard]] inline std::string SceneChangeText(const std::string_view text,
                                                 const std::size_t limit) {
  std::size_t encoded = 0U;
  std::size_t end = 0U;
  while (end < text.size()) {
    const auto lead = static_cast<unsigned char>(text[end]);
    std::size_t length = 1U;
    while (end + length < text.size() &&
           (static_cast<unsigned char>(text[end + length]) & 0xC0U) == 0x80U)
      ++length;
    const auto cost = lead < 0x80U ? SceneChangeEncodedBytes(lead) : length;
    if (encoded + cost > limit)
      break;
    encoded += cost;
    end += length;
  }
  return std::string(text.substr(0U, end));
}

/// The operation kind of one scene request and the object it names.
[[nodiscard]] inline std::pair<std::string_view, std::optional<std::string>>
SceneChangeTarget(const core::SceneOperationRequestDTO &operation) {
  return std::visit(
      [](const auto &value)
          -> std::pair<std::string_view, std::optional<std::string>> {
        using Operation = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Operation, core::AddObjectRequestDTO>)
          return {"add_object", value.scene_resource_id.value()};
        else if constexpr (std::is_same_v<Operation, core::UpsertObjectRequestDTO>)
          return {"upsert_object", value.scene_resource_id.value()};
        else if constexpr (std::is_same_v<Operation, core::RemoveObjectRequestDTO>)
          return {"remove_object", value.scene_resource_id.value()};
        else if constexpr (std::is_same_v<Operation,
                                          core::UpdateObjectPoseRequestDTO>)
          return {"update_object_pose", value.scene_resource_id.value()};
        else if constexpr (std::is_same_v<Operation,
                                          core::AttachCarriedObjectRequestDTO>)
          return {"attach_carried_object", value.scene_resource_id.value()};
        else if constexpr (std::is_same_v<Operation,
                                          core::DetachCarriedObjectRequestDTO>)
          return {"detach_carried_object", std::nullopt};
        else if constexpr (std::is_same_v<
                               Operation,
                               core::DetachCarriedObjectAtCurrentPoseRequestDTO>)
          return {"detach_carried_object_at_current_pose",
                  value.expected_scene_resource_id.value()};
        else {
          static_assert(
              std::is_same_v<Operation, core::SetCollisionPolicyRequestDTO>);
          return {"set_collision_policy", std::nullopt};
        }
      },
      operation);
}

/// One record: `{duration_ms, error, operation_kind, outcome, principal_id,
/// request_id, scene_resource_id, scene_revision_id, sequence, source_label,
/// transport}` as canonical JSON. `outcome` is succeeded, refused or unknown;
/// `error` the refusal's typed error (null on success).
[[nodiscard]] inline std::string
SceneChangeRecord(const std::uint64_t sequence,
                  const core::SceneTransactionRequestDTO &request,
                  const std::string_view outcome, const core::ErrorDTO *error,
                  const std::optional<std::uint64_t> scene_revision_id,
                  const double duration_ms) {
  using content_identity::CanonicalValue;
  const auto optional_text = [](const std::optional<std::string> &value,
                                  const std::size_t limit) {
    return value.has_value() ? CanonicalValue(SceneChangeText(*value, limit))
                             : CanonicalValue(nullptr);
  };
  CanonicalValue encoded_error(nullptr);
  if (error != nullptr) {
    CanonicalValue::Object members{
        {"code", CanonicalValue(SceneChangeText(error->code, kSceneChangeCodeBytes))},
        {"message",
         CanonicalValue(SceneChangeText(error->message, kSceneChangeTextBytes))},
    };
    if (!error->details.empty()) {
      CanonicalValue::Object details;
      details.reserve(error->details.size());
      for (const auto &[name, value] : error->details)
        details.emplace_back(name, CanonicalValue(value));
      CanonicalValue value(std::move(details));
      if (content_identity::canonical_json(value).size() <=
          kSceneChangeDetailsBytes)
        members.emplace_back("details", std::move(value));
    }
    encoded_error = CanonicalValue(std::move(members));
  }
  const auto [operation_kind, scene_resource_id] =
      SceneChangeTarget(request.operation);
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"duration_ms", CanonicalValue(duration_ms)},
      {"error", std::move(encoded_error)},
      {"operation_kind", CanonicalValue(std::string(operation_kind))},
      {"outcome", CanonicalValue(std::string(outcome))},
      {"principal_id",
       optional_text(request.attribution.principal_id, kSceneChangeNameBytes)},
      {"request_id", optional_text(request.request_id, kSceneChangeRequestIdBytes)},
      {"scene_resource_id", optional_text(scene_resource_id, kSceneChangeNameBytes)},
      {"scene_revision_id",
       scene_revision_id.has_value()
           ? CanonicalValue(static_cast<double>(*scene_revision_id))
           : CanonicalValue(nullptr)},
      {"sequence", CanonicalValue(static_cast<double>(sequence))},
      {"source_label", CanonicalValue(SceneChangeText(
                           request.request_context.source_label,
                           kSceneChangeNameBytes))},
      {"transport", optional_text(request.request_transport, 4U)},
  }));
}

/// The last `kSceneChangeRingCapacity` records. One producer (the command
/// lane): no lock, a fixed array, memory only.
class SceneChangeRing final {
public:
  /// The sequence the next record carries.
  [[nodiscard]] std::uint64_t next_sequence() const noexcept {
    return added_ + 1U;
  }

  /// Keeps `record` (sequence `next_sequence()`) in place of the oldest and
  /// returns the section payload `{"records":[...]}`, oldest first: one
  /// string built from the held records, never re-encoded.
  [[nodiscard]] std::shared_ptr<const std::string> Add(std::string record) {
    records_[added_ % kSceneChangeRingCapacity] = std::move(record);
    ++added_;
    const auto first =
        added_ > kSceneChangeRingCapacity ? added_ - kSceneChangeRingCapacity : 0U;
    std::size_t bytes = 14U;
    for (auto index = first; index < added_; ++index)
      bytes += records_[index % kSceneChangeRingCapacity].size() + 1U;
    std::string payload;
    payload.reserve(bytes);
    payload += "{\"records\":[";
    for (auto index = first; index < added_; ++index) {
      if (index != first)
        payload += ',';
      payload += records_[index % kSceneChangeRingCapacity];
    }
    payload += "]}";
    return std::make_shared<const std::string>(std::move(payload));
  }

private:
  std::array<std::string, kSceneChangeRingCapacity> records_{};
  std::uint64_t added_{};
};

} // namespace manyforge::headless::detail
