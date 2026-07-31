#pragma once

#include <string>
#include <string_view>

#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment::detail {

/// How the schema version participating in an identity is represented.
///
/// Standalone documents carry their schema tag in an `api_version` member.
/// Nested records inherit the document-family tag from their closed parent
/// schema and must not repeat that member.
enum class ContentIdentityRootShape {
  kDocument,
  kNestedRecord,
};

/// Schema-owned parameters for one canonical content identity.
///
/// This is deliberately internal: only an exact schema validator may invoke
/// the identity primitive after it has closed, validated, normalized, and
/// default-materialized the complete value. Merely parsed or structurally
/// typed candidate data must not gain a trusted identity operation.
struct ContentIdentitySpec final {
  std::string identity_domain;
  std::string api_version;
  std::string own_field;
  ContentIdentityRootShape root_shape{ContentIdentityRootShape::kDocument};

  friend bool operator==(const ContentIdentitySpec&, const ContentIdentitySpec&) = default;
};

/// Computes a spec-210 content identity from an exact schema-qualified root.
///
/// The caller is responsible for proving the complete schema precondition.
/// A document root must contain an `api_version` string matching the spec. A
/// nested-record root must omit `api_version`; its parent document-family tag
/// is supplied by the spec and still participates in the preimage. Both
/// shapes must contain `own_field`. Exactly `own_field` is omitted from the
/// JCS payload.
[[nodiscard]] std::string compute_content_identity(const CanonicalValue& normalized_root,
                                                   const ContentIdentitySpec& spec);

/// Verifies the declared own field after the same exact-schema precondition.
///
/// Structural/precondition violations throw CanonicalValueError. A
/// well-formed but non-matching declared digest returns false.
[[nodiscard]] bool verify_content_identity(const CanonicalValue& normalized_root,
                                           const ContentIdentitySpec& spec);

/// Constructs the immutable-catalog domain required by specs 210/430.
///
/// The typed catalog schema layer remains responsible for checking that
/// `record_kind` is one of the closed registered kinds.
[[nodiscard]] std::string catalog_record_identity_domain(std::string_view record_kind);

}  // namespace manyforge::deployment::detail
