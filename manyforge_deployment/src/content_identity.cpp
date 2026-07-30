#include "manyforge_deployment/content_identity.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>

#include "content_identity_internal.hpp"

namespace manyforge::deployment {
namespace {

using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

void require_ascii_token(std::string_view value, std::string_view name) {
  if (value.empty()) {
    throw CanonicalValueError(std::string(name) + " must not be empty");
  }
  if (!std::all_of(value.begin(), value.end(),
                   [](unsigned char byte) { return byte >= 0x21U && byte <= 0x7eU; })) {
    throw CanonicalValueError(std::string(name) + " must contain printable ASCII only");
  }
}

[[nodiscard]] const std::string& require_string_member(const CanonicalValue& root,
                                                       std::string_view name) {
  const CanonicalValue* member = root.find_object_member(name);
  if (member == nullptr) {
    throw CanonicalValueError("missing required root member: " + std::string(name));
  }
  const auto* string = std::get_if<std::string>(&member->storage());
  if (string == nullptr) {
    throw CanonicalValueError("root member must be a string: " + std::string(name));
  }
  return *string;
}

[[nodiscard]] CanonicalValue omit_own_field(const CanonicalValue& normalized_root,
                                            std::string_view own_field) {
  const auto& object = normalized_root.as_object();
  CanonicalValue::Object payload;
  payload.reserve(object.empty() ? 0U : object.size() - 1U);
  bool omitted = false;
  for (const auto& member : object) {
    if (member.first == own_field) {
      omitted = true;
      continue;
    }
    payload.push_back(member);
  }
  if (!omitted) {
    throw CanonicalValueError("missing content-identity own field: " + std::string(own_field));
  }
  return CanonicalValue(std::move(payload));
}

void validate_root_shape(const CanonicalValue& normalized_root,
                         const detail::ContentIdentitySpec& spec) {
  switch (spec.root_shape) {
    case detail::ContentIdentityRootShape::kDocument: {
      const std::string& declared_api_version =
          require_string_member(normalized_root, "api_version");
      if (declared_api_version != spec.api_version) {
        throw CanonicalValueError(
            "root api_version does not match content-identity schema tag");
      }
      return;
    }
    case detail::ContentIdentityRootShape::kNestedRecord:
      if (normalized_root.find_object_member("api_version") != nullptr) {
        throw CanonicalValueError(
            "nested content-identity root must not contain api_version");
      }
      return;
  }
  throw CanonicalValueError("unsupported content-identity root shape");
}

[[nodiscard]] std::string content_identity_preimage(const CanonicalValue& normalized_root,
                                                    const detail::ContentIdentitySpec& spec) {
  require_ascii_token(spec.identity_domain, "identity domain");
  require_ascii_token(spec.api_version, "api_version");
  require_ascii_token(spec.own_field, "own field");

  validate_root_shape(normalized_root, spec);
  const std::string& declared_identity = require_string_member(normalized_root, spec.own_field);
  if (!is_sha256_digest(declared_identity)) {
    throw CanonicalValueError("content-identity own field must be an exact lowercase SHA-256");
  }

  const std::string payload = canonical_json(omit_own_field(normalized_root, spec.own_field));
  std::string preimage;
  preimage.reserve(detail::kContentIdentityPrefix.size() + 1U + spec.identity_domain.size() + 1U +
                   spec.api_version.size() + 1U + payload.size());
  preimage.append(detail::kContentIdentityPrefix);
  preimage.push_back('\0');
  preimage.append(spec.identity_domain);
  preimage.push_back('\0');
  preimage.append(spec.api_version);
  preimage.push_back('\0');
  preimage.append(payload);
  return preimage;
}

}  // namespace

std::string sha256_digest(std::string_view bytes) {
  DigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!context) {
    throw std::runtime_error("OpenSSL failed to allocate SHA-256 context");
  }
  if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
    throw std::runtime_error("OpenSSL failed to initialize SHA-256");
  }
  if (!bytes.empty() && EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1) {
    throw std::runtime_error("OpenSSL failed while hashing SHA-256 input");
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1 || digest_size != 32U) {
    throw std::runtime_error("OpenSSL failed to finalize SHA-256 digest");
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string result("sha256:");
  result.reserve(7U + (digest_size * 2U));
  for (unsigned int index = 0; index < digest_size; ++index) {
    const unsigned char byte = digest[index];
    result.push_back(kHex[(byte >> 4U) & 0x0fU]);
    result.push_back(kHex[byte & 0x0fU]);
  }
  return result;
}

bool is_sha256_digest(std::string_view value) noexcept {
  if (value.size() != 71U || value.substr(0U, 7U) != "sha256:") {
    return false;
  }
  return std::all_of(value.begin() + 7, value.end(), [](unsigned char byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
  });
}

namespace detail {

std::string compute_content_identity(const CanonicalValue& normalized_root,
                                     const ContentIdentitySpec& spec) {
  return sha256_digest(content_identity_preimage(normalized_root, spec));
}

bool verify_content_identity(const CanonicalValue& normalized_root,
                             const ContentIdentitySpec& spec) {
  const std::string& declared = require_string_member(normalized_root, spec.own_field);
  const std::string computed = compute_content_identity(normalized_root, spec);
  return declared.size() == computed.size() &&
         CRYPTO_memcmp(declared.data(), computed.data(), declared.size()) == 0;
}

std::string catalog_record_identity_domain(std::string_view record_kind) {
  require_ascii_token(record_kind, "record_kind");
  if (!std::all_of(record_kind.begin(), record_kind.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_';
      })) {
    throw CanonicalValueError("record_kind must use lowercase snake-case token spelling");
  }
  return "catalog-record-revision/" + std::string(record_kind);
}

}  // namespace detail
}  // namespace manyforge::deployment
