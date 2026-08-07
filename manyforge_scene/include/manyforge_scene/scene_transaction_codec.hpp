#pragma once

#include <string_view>

#include <manyforge_core/types/scene_transaction.hpp>

namespace manyforge::scene {

/// Decode the exact canonical internal scene-transaction packet carried over
/// the same-user Manager command socket.
///
/// The public HTTP body does not contain RequestAttributionDTO. The receiving
/// headless adapter stamps that value before encoding this internal packet;
/// the logical-scene owner still validates and durably digests the complete
/// normalized request.
[[nodiscard]] manyforge::core::SceneTransactionRequestDTO
DecodeCanonicalSceneTransactionRequest(std::string_view bytes);

} // namespace manyforge::scene
