#pragma once

#include <cstddef>
#include <cstdint>

#include "manyforge_deployment/artifact_codec.hpp"

namespace manyforge::deployment::detail {

/// Shared compiler-owned limits that are not already part of a public typed
/// contract. The compiler-contract manifest projects these exact constants.
inline constexpr std::size_t kMaximumPortableIdentityBytes = 128U;
inline constexpr std::uint64_t
    kQuaternionNormSquaredTolerancePartsPerTrillion = 1000000ULL;
inline constexpr double kQuaternionNormSquaredTolerance =
    static_cast<double>(
        kQuaternionNormSquaredTolerancePartsPerTrillion) /
    1000000000000.0;

inline constexpr std::size_t kNodesPerCatalogRecord = 15U;
inline constexpr std::size_t kCatalogContainerNodes = 3U;
inline constexpr std::size_t kMaximumCatalogRecords =
    (kMaxArtifactNodeCount - kCatalogContainerNodes) /
    kNodesPerCatalogRecord;

inline constexpr std::size_t kMaximumManifestTextCodePoints = 1024U;
inline constexpr std::size_t kMaximumExecutableRegistryRows = 4096U;
inline constexpr std::size_t kMaximumExecutablePackageRows = 256U;
inline constexpr std::size_t kMaximumSourceCoverageRows = 65536U;
inline constexpr std::size_t kMaximumCompilerContractRows = 4096U;
inline constexpr std::size_t kMaximumCompilerBuildRows = 65536U;
inline constexpr std::uint64_t kMaximumIJsonInteger =
    9007199254740991ULL;

}  // namespace manyforge::deployment::detail
