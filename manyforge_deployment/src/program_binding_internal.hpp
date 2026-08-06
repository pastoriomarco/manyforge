#pragma once

#include <manyforge_deployment/program_binding.hpp>

#include <memory>
#include <string_view>

namespace manyforge::deployment::detail {

[[nodiscard]] CanonicalValue program_binding_projection(
    const core::ProgramBindingDTO& value);

/// Sole in-process producer used by the binding compiler after exact role
/// resolution. The supplied ID is ignored and recomputed from the complete
/// normalized value.
class ProgramBindingConstructionAuthority final {
 public:
  [[nodiscard]] static std::shared_ptr<const ProgramBinding> seal(
      core::ProgramBindingDTO value);

  [[nodiscard]] static std::shared_ptr<const ProgramBinding> reload_intrinsic(
      std::string_view canonical_bytes);
};

}  // namespace manyforge::deployment::detail
