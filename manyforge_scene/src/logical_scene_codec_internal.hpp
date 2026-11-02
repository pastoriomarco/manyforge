#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <manyforge_core/types/scene_transaction.hpp>

namespace manyforge::scene::logical_scene_codec_internal {

[[nodiscard]] std::string
EncodeRequest(const manyforge::core::SceneTransactionRequestDTO &request);
/// Canonical public payload/precondition projection used for request replay.
/// Transport-stamped attribution is deliberately absent.
[[nodiscard]] std::string EncodeRequestDigestInput(
    const manyforge::core::SceneTransactionRequestDTO &request);
[[nodiscard]] manyforge::core::SceneTransactionRequestDTO
DecodeRequest(std::string_view bytes);
[[nodiscard]] std::string EncodeResolvedOperation(
    const manyforge::core::ResolvedSceneOperationDTO &operation);
[[nodiscard]] std::string
EncodeSnapshot(const manyforge::core::LogicalSceneSnapshotDTO &snapshot);
/// `verify_content_hash` is false only while converting a v9 store, whose
/// stored hashes still cover the retired provider observation fields.
[[nodiscard]] manyforge::core::LogicalSceneSnapshotDTO
DecodeSnapshot(std::string_view bytes, bool verify_content_hash = true);
/// One-time v9 -> v10 store conversion: removes the retired observation
/// fields (sequence, receive time, source timestamp, freshness budget) from
/// every `provider_evidence` object of one stored canonical JSON document.
/// Returns nullopt when the document carries none.
[[nodiscard]] std::optional<std::string>
StripRetiredProviderEvidenceFields(std::string_view bytes);
[[nodiscard]] std::string
EncodeErrorDetails(const std::map<std::string, std::string> &details);
[[nodiscard]] std::map<std::string, std::string>
DecodeErrorDetails(std::string_view bytes);

} // namespace manyforge::scene::logical_scene_codec_internal
