#pragma once

#include <manyforge_core/types/scene_transaction.hpp>
#include <string>
#include <string_view>

namespace manyforge::scene {

/// Canonical value codecs shared by bounded owner-to-projection transports.
///
/// These functions preserve the scene owner's already-normalized DTOs. They
/// neither resolve authored frames nor mint scene, projection, or retained
/// identities.
[[nodiscard]] std::string EncodeLogicalSceneSnapshot(
    const manyforge::core::LogicalSceneSnapshotDTO &snapshot);
[[nodiscard]] manyforge::core::LogicalSceneSnapshotDTO
DecodeLogicalSceneSnapshot(std::string_view bytes);

[[nodiscard]] std::string EncodeResolvedSceneOperation(
    const manyforge::core::ResolvedSceneOperationDTO &operation);
[[nodiscard]] manyforge::core::ResolvedSceneOperationDTO
DecodeResolvedSceneOperation(std::string_view bytes);

[[nodiscard]] std::string
EncodeSceneTransaction(const manyforge::core::SceneTransactionDTO &transaction);
[[nodiscard]] manyforge::core::SceneTransactionDTO
DecodeSceneTransaction(std::string_view bytes);

[[nodiscard]] std::string EncodeResolvedSceneCandidatePreviewRequest(
    const manyforge::core::ResolvedSceneCandidatePreviewRequestDTO &request);
[[nodiscard]] manyforge::core::ResolvedSceneCandidatePreviewRequestDTO
DecodeResolvedSceneCandidatePreviewRequest(std::string_view bytes);

[[nodiscard]] std::string EncodeSceneCandidatePreview(
    const manyforge::core::SceneCandidatePreviewDTO &preview);
[[nodiscard]] manyforge::core::SceneCandidatePreviewDTO
DecodeSceneCandidatePreview(std::string_view bytes);

} // namespace manyforge::scene
