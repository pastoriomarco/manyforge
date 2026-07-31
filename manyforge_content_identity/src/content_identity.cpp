#include "manyforge_content_identity/content_identity.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>

namespace manyforge::content_identity {
namespace {

using DigestContext =
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

void require_ascii_token(std::string_view value, std::string_view name) {
  if (value.empty()) {
    throw CanonicalValueError(std::string(name) + " must not be empty");
  }
  if (!std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x21U && byte <= 0x7eU;
      })) {
    throw CanonicalValueError(std::string(name) +
                              " must contain printable ASCII only");
  }
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
  if (!bytes.empty() &&
      EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1) {
    throw std::runtime_error("OpenSSL failed while hashing SHA-256 input");
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0U;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1 ||
      digest_size != 32U) {
    throw std::runtime_error("OpenSSL failed to finalize SHA-256 digest");
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string result("sha256:");
  result.reserve(7U + (digest_size * 2U));
  for (unsigned int index = 0U; index < digest_size; ++index) {
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
  return std::all_of(value.begin() + 7, value.end(),
                     [](unsigned char byte) {
                       return (byte >= '0' && byte <= '9') ||
                              (byte >= 'a' && byte <= 'f');
                     });
}

std::string framed_sha256(std::string_view identity_domain,
                          std::string_view api_version,
                          const CanonicalValue& normalized_payload) {
  require_ascii_token(identity_domain, "identity domain");
  require_ascii_token(api_version, "api_version");

  const std::string payload = canonical_json(normalized_payload);
  std::string preimage;
  preimage.reserve(kContentIdentityPrefix.size() + 1U +
                   identity_domain.size() + 1U + api_version.size() + 1U +
                   payload.size());
  preimage.append(kContentIdentityPrefix);
  preimage.push_back('\0');
  preimage.append(identity_domain);
  preimage.push_back('\0');
  preimage.append(api_version);
  preimage.push_back('\0');
  preimage.append(payload);
  return sha256_digest(preimage);
}

bool digest_equal(std::string_view left, std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  return left.empty() ||
         CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

}  // namespace manyforge::content_identity
