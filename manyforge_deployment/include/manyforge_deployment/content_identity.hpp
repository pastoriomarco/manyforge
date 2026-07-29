#pragma once

#include <string>
#include <string_view>

namespace manyforge::deployment {

/// Returns `sha256:` followed by the lowercase SHA-256 of the exact bytes.
[[nodiscard]] std::string sha256_digest(std::string_view bytes);

/// Checks the exact lowercase `sha256:<64 hex>` wire spelling.
[[nodiscard]] bool is_sha256_digest(std::string_view value) noexcept;

}  // namespace manyforge::deployment
