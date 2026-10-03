#pragma once

#include <string>
#include <string_view>

#include <manyforge_content_identity/canonical_value.hpp>

namespace manyforge::content_identity {

inline constexpr std::string_view kContentIdentityPrefix =
    "manyforge.content-identity.v1";

/// Returns `sha256:` followed by the lowercase SHA-256 of the exact bytes.
[[nodiscard]] std::string sha256_digest(std::string_view bytes);

/// Checks the exact lowercase `sha256:<64 hex>` wire spelling.
[[nodiscard]] bool is_sha256_digest(std::string_view value) noexcept;

/// Mechanical spec-210 framing over an already normalized canonical value.
///
/// This function does not close a schema, classify arrays, omit an identity
/// field, or establish semantic ownership. Only a schema/DTO owner that has
/// already proved those preconditions may treat the result as a trusted
/// identity.
[[nodiscard]] std::string framed_sha256(
    std::string_view identity_domain, std::string_view api_version,
    const CanonicalValue& normalized_payload);

/// The same framing over bytes the caller already holds as the exact RFC 8785
/// serialization of a normalized value, e.g. a member span of input that
/// `parse_canonical_json` accepted. Passing any other bytes yields an identity
/// no conforming producer can match.
[[nodiscard]] std::string framed_sha256_of_canonical_json(
    std::string_view identity_domain, std::string_view api_version,
    std::string_view canonical_payload);

/// Constant-time equality for two exact digest spellings.
[[nodiscard]] bool digest_equal(std::string_view left,
                                std::string_view right) noexcept;

}  // namespace manyforge::content_identity
