#pragma once

#include <manyforge_content_identity/canonical_value.hpp>

namespace manyforge::deployment {

// Deployment's public codec surface deliberately re-exports the one lower
// mechanical RFC 8785 value type. Deployment remains the schema-qualified
// semantic identity owner; this is one type/implementation, not a fallback.
using content_identity::CanonicalValue;
using content_identity::CanonicalValueError;
using content_identity::canonical_json;
using content_identity::is_utf8_nfc;
using content_identity::require_utf8_nfc;

}  // namespace manyforge::deployment
