#pragma once

#include <filesystem>

#include "manyforge_deployment/canonical_empty_admission.hpp"

namespace manyforge::deployment::detail {

/// Builds one package from the exact embedded deployment and then reloads it
/// through the production read-only admission source.
///
/// The output root is placement only; every semantic identity is build-owned.
[[nodiscard]] CanonicalEmptyRetainedBundleAdmission
generate_canonical_empty_package(
    const std::filesystem::path& output_root);

}  // namespace manyforge::deployment::detail
