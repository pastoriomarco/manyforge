#pragma once

#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/runtime_policy_codec.hpp>
#include <optional>
#include <stdexcept>

namespace manyforge::deployment {

class RetainedRuntimePolicyError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Re-read and independently qualify the sole retained v1 runtime policy.
///
/// Absence is observable and leaves motion unqualified. More than one policy
/// is ambiguous because v1 has no runtime policy selector.
[[nodiscard]] std::optional<QualifiedRuntimePolicy> resolve_retained_runtime_policy(
    const RetainedBundle& bundle, const ICatalogArtifactSource& artifact_store);

}  // namespace manyforge::deployment
