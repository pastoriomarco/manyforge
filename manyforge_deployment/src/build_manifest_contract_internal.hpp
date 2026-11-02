#pragma once

#include <string_view>

namespace manyforge::deployment::detail {

// Read-neutral manifest contract constants shared by the compiler-side
// authority and the admission-only verifier. This header deliberately has no
// repository, materializer, or compiler declarations.
inline constexpr std::string_view kNodeEnvironmentManifestApiVersion =
    "manyforge.node_environment_manifest.v1";
inline constexpr std::string_view kCompilerContractManifestApiVersion =
    "manyforge.compiler_contract_manifest.v1";
inline constexpr std::string_view kCompilerBuildManifestApiVersion =
    "manyforge.compiler_build_manifest.v1";
inline constexpr std::string_view kNodeEnvironmentIdentityDomain =
    "node-environment-digest";
inline constexpr std::string_view kCompilerContractIdentityDomain =
    "compiler-contract-digest";
inline constexpr std::string_view kCompilerBuildIdentityDomain =
    "compiler-build-digest";

}  // namespace manyforge::deployment::detail
