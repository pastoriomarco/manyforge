#pragma once

#include <manyforge_content_identity/content_identity.hpp>

namespace manyforge::deployment {

// Existing deployment callers share the exact lower implementation while the
// schema-owned compute/verify operations remain private to deployment.
using content_identity::is_sha256_digest;
using content_identity::sha256_digest;

}  // namespace manyforge::deployment
