#pragma once

#include <map>
#include <string>
#include <string_view>

#include <manyforge_core/types/managed_runtime_withdrawal.hpp>
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
[[nodiscard]] manyforge::core::LogicalSceneSnapshotDTO
DecodeSnapshot(std::string_view bytes);
[[nodiscard]] std::string
EncodeErrorDetails(const std::map<std::string, std::string> &details);
[[nodiscard]] std::map<std::string, std::string>
DecodeErrorDetails(std::string_view bytes);

[[nodiscard]] std::string EncodeManagedWithdrawal(
    const manyforge::core::ManagedRuntimeWithdrawalDTO &receipt);
[[nodiscard]] manyforge::core::ManagedRuntimeWithdrawalDTO
DecodeManagedWithdrawal(std::string_view bytes);

} // namespace manyforge::scene::logical_scene_codec_internal
