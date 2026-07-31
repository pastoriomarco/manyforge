#include "manyforge_deployment/content_identity.hpp"

#include <algorithm>

#include "content_identity_internal.hpp"

namespace manyforge::deployment {
namespace {

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

[[nodiscard]] CanonicalValue content_identity_payload(
    const CanonicalValue& normalized_root,
    const detail::ContentIdentitySpec& spec) {
  require_ascii_token(spec.identity_domain, "identity domain");
  require_ascii_token(spec.api_version, "api_version");
  require_ascii_token(spec.own_field, "own field");

  validate_root_shape(normalized_root, spec);
  const std::string& declared_identity = require_string_member(normalized_root, spec.own_field);
  if (!is_sha256_digest(declared_identity)) {
    throw CanonicalValueError("content-identity own field must be an exact lowercase SHA-256");
  }
  return omit_own_field(normalized_root, spec.own_field);
}

}  // namespace

namespace detail {

std::string compute_content_identity(const CanonicalValue& normalized_root,
                                     const ContentIdentitySpec& spec) {
  return content_identity::framed_sha256(
      spec.identity_domain, spec.api_version,
      content_identity_payload(normalized_root, spec));
}

bool verify_content_identity(const CanonicalValue& normalized_root,
                             const ContentIdentitySpec& spec) {
  const std::string& declared = require_string_member(normalized_root, spec.own_field);
  const std::string computed = compute_content_identity(normalized_root, spec);
  return content_identity::digest_equal(declared, computed);
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
