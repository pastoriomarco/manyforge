#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/verified_retained_bundle.hpp"

namespace manyforge::deployment::detail {

/// The catalog contract deliberately treats store keys as opaque bounded
/// strings.  This codec is the only place where an opaque key is mapped to a
/// filesystem-safe name.  The original key remains inside the immutable
/// envelope and is reverified on every reopen/read, so a filename-hash
/// collision cannot silently substitute one key for another.
inline constexpr std::size_t kMaximumCatalogArtifactKeyBytes =
    kMaxArtifactScalarBytes;
inline constexpr std::string_view kCatalogArtifactNamespaceDirectory =
    "catalog-artifacts";
inline constexpr std::string_view kCatalogArtifactDataDirectory =
    "by-key-sha256";
inline constexpr std::string_view kCatalogArtifactFileSuffix = ".record";
inline constexpr std::string_view kCatalogArtifactEnvelopeMagic{
    "manyforge.catalog-artifact-envelope.v1\0",
    sizeof("manyforge.catalog-artifact-envelope.v1\0") - 1U};
inline constexpr std::string_view kCatalogArtifactFilenameDomain{
    "manyforge.catalog-artifact-filename.v1\0",
    sizeof("manyforge.catalog-artifact-filename.v1\0") - 1U};
inline constexpr std::size_t kSha256DigestWireBytes = 71U;
inline constexpr std::uint64_t kCatalogArtifactEnvelopeFixedBytes =
    static_cast<std::uint64_t>(kCatalogArtifactEnvelopeMagic.size()) +
    sizeof(std::uint64_t) + kSha256DigestWireBytes +
    sizeof(std::uint64_t);
inline constexpr std::uint64_t kMaximumCatalogArtifactEnvelopeOverhead =
    kCatalogArtifactEnvelopeFixedBytes +
    static_cast<std::uint64_t>(kMaximumCatalogArtifactKeyBytes);

struct DecodedCatalogArtifactEnvelope final {
  std::string_view content_addressed_store_key;
  std::string_view content_digest;
  std::string_view payload;
};

[[nodiscard]] inline bool is_bounded_catalog_artifact_key(
    std::string_view key) noexcept {
  return !key.empty() && key.size() <= kMaximumCatalogArtifactKeyBytes;
}

inline void append_u64_be(std::string& output, std::uint64_t value) {
  for (std::size_t shift = 8U; shift > 0U; --shift) {
    output.push_back(static_cast<char>(
        (value >> ((shift - 1U) * 8U)) & 0xffU));
  }
}

[[nodiscard]] inline std::optional<std::uint64_t> read_u64_be(
    std::string_view bytes, std::size_t& offset) noexcept {
  if (offset > bytes.size() ||
      bytes.size() - offset < sizeof(std::uint64_t)) {
    return std::nullopt;
  }
  std::uint64_t value = 0U;
  for (std::size_t index = 0U; index < sizeof(std::uint64_t); ++index) {
    value = (value << 8U) |
            static_cast<unsigned char>(bytes[offset + index]);
  }
  offset += sizeof(std::uint64_t);
  return value;
}

/// Returns exactly 64 lowercase hexadecimal characters suitable for a single
/// filename component, or null when `key` violates the existing catalog-key
/// contract. The length frame makes the domain separation unambiguous.
[[nodiscard]] inline std::optional<std::string>
catalog_artifact_storage_hex(std::string_view key) {
  if (!is_bounded_catalog_artifact_key(key)) {
    return std::nullopt;
  }
  std::string preimage;
  if (key.size() >
      std::numeric_limits<std::size_t>::max() -
          kCatalogArtifactFilenameDomain.size() -
          sizeof(std::uint64_t)) {
    return std::nullopt;
  }
  preimage.reserve(kCatalogArtifactFilenameDomain.size() +
                   sizeof(std::uint64_t) + key.size());
  preimage.append(kCatalogArtifactFilenameDomain);
  append_u64_be(preimage, static_cast<std::uint64_t>(key.size()));
  preimage.append(key);
  const std::string digest = sha256_digest(preimage);
  if (!is_sha256_digest(digest)) {
    throw std::logic_error(
        "SHA-256 helper returned an invalid catalog filename digest");
  }
  return digest.substr(std::string_view("sha256:").size());
}

[[nodiscard]] inline std::string encode_catalog_artifact_envelope(
    std::string_view key, std::string_view expected_digest,
    std::string_view payload) {
  if (!is_bounded_catalog_artifact_key(key)) {
    throw RetainedBundleError(
        "catalog artifact key must be non-empty and within its fixed bound");
  }
  if (!is_sha256_digest(expected_digest) ||
      sha256_digest(payload) != expected_digest) {
    throw RetainedBundleError(
        "catalog artifact bytes do not match their expected SHA-256 digest");
  }
  const std::uint64_t overhead =
      kCatalogArtifactEnvelopeFixedBytes +
      static_cast<std::uint64_t>(key.size());
  if (payload.size() >
      std::numeric_limits<std::size_t>::max() -
          static_cast<std::size_t>(overhead)) {
    throw RetainedBundleError("catalog artifact envelope size overflows");
  }

  std::string envelope;
  envelope.reserve(static_cast<std::size_t>(overhead) + payload.size());
  envelope.append(kCatalogArtifactEnvelopeMagic);
  append_u64_be(envelope, static_cast<std::uint64_t>(key.size()));
  envelope.append(key);
  envelope.append(expected_digest);
  append_u64_be(envelope, static_cast<std::uint64_t>(payload.size()));
  envelope.append(payload);
  return envelope;
}

[[nodiscard]] inline DecodedCatalogArtifactEnvelope
decode_catalog_artifact_envelope(std::string_view envelope) {
  if (!envelope.starts_with(kCatalogArtifactEnvelopeMagic)) {
    throw RetainedBundleError(
        "durable catalog artifact has an invalid envelope header");
  }
  std::size_t offset = kCatalogArtifactEnvelopeMagic.size();
  const auto key_size = read_u64_be(envelope, offset);
  if (!key_size.has_value() ||
      *key_size > kMaximumCatalogArtifactKeyBytes ||
      *key_size > static_cast<std::uint64_t>(envelope.size() - offset)) {
    throw RetainedBundleError(
        "durable catalog artifact has an invalid bounded key");
  }
  const std::string_view key =
      envelope.substr(offset, static_cast<std::size_t>(*key_size));
  offset += static_cast<std::size_t>(*key_size);
  if (!is_bounded_catalog_artifact_key(key) ||
      envelope.size() - offset <
          kSha256DigestWireBytes + sizeof(std::uint64_t)) {
    throw RetainedBundleError(
        "durable catalog artifact has invalid envelope metadata");
  }

  const std::string_view digest =
      envelope.substr(offset, kSha256DigestWireBytes);
  offset += kSha256DigestWireBytes;
  if (!is_sha256_digest(digest)) {
    throw RetainedBundleError(
        "durable catalog artifact has an invalid content digest");
  }

  const auto payload_size = read_u64_be(envelope, offset);
  if (!payload_size.has_value() ||
      *payload_size !=
          static_cast<std::uint64_t>(envelope.size() - offset)) {
    throw RetainedBundleError(
        "durable catalog artifact has an invalid payload size");
  }
  const std::string_view payload = envelope.substr(offset);
  if (sha256_digest(payload) != digest) {
    throw RetainedBundleError(
        "durable catalog artifact payload does not match its digest");
  }
  return {key, digest, payload};
}

}  // namespace manyforge::deployment::detail
