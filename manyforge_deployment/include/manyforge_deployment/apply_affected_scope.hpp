#pragma once

#include <stdexcept>

#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::deployment {

class ApplyAffectedScopeError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// Purely derives the one atomic Kernel-admission scope for a named Apply.
///
/// Both inputs are sealed, independently byte-verified retained bundles. The
/// result is the canonical binding/resource-overlap closure defined by specs
/// 210, 230, and 430. The function performs no I/O and accepts no caller-owned
/// scope or busy disposition.
[[nodiscard]] core::ConfigurationFenceScopeDTO
derive_apply_configuration_fence_scope(const RetainedBundle &predecessor,
                                       const RetainedBundle &candidate);

} // namespace manyforge::deployment
