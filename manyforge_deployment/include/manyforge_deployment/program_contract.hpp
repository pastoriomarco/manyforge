#pragma once

#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::deployment {

class ProgramContractError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

namespace detail {
class ProgramBindingContractConstructionAuthority;
}

/// Sealed binding-relevant projection of one canonical Program revision.
///
/// The complete canonical Program remains attached to the projection so a
/// caller cannot manufacture or omit a role requirement. Node-catalog
/// parameter/dataflow validation is the preceding program-loader phase; this
/// boundary independently closes the Program envelope, revision, roles, tree
/// structure, and exact retained capability revisions needed by binding.
class ProgramBindingContract final {
 public:
  ~ProgramBindingContract() = default;
  ProgramBindingContract(const ProgramBindingContract&) = delete;
  ProgramBindingContract& operator=(const ProgramBindingContract&) = delete;
  ProgramBindingContract(ProgramBindingContract&&) = delete;
  ProgramBindingContract& operator=(ProgramBindingContract&&) = delete;

  [[nodiscard]] const core::ProgramBindingContractDTO& value() const noexcept {
    return value_;
  }
  [[nodiscard]] const CanonicalValue& normalized_program_root() const noexcept {
    return normalized_program_root_;
  }
  [[nodiscard]] std::string_view canonical_program_bytes() const noexcept {
    return canonical_program_bytes_;
  }

 private:
  ProgramBindingContract(core::ProgramBindingContractDTO value,
                         CanonicalValue normalized_program_root,
                         std::string canonical_program_bytes)
      : value_(std::move(value)),
        normalized_program_root_(std::move(normalized_program_root)),
        canonical_program_bytes_(std::move(canonical_program_bytes)) {}

  core::ProgramBindingContractDTO value_;
  CanonicalValue normalized_program_root_;
  std::string canonical_program_bytes_;

  friend class detail::ProgramBindingContractConstructionAuthority;
};

/// Extract and verify the complete role contract from schema-normalized,
/// RFC-8785 canonical Program bytes against one exact retained catalog.
///
/// This is deliberately not a YAML authoring parser or a substitute for the
/// NodeCatalog validation phase. The Program loader supplies canonical bytes
/// only after schema/catalog/program validation; this function independently
/// proves the content identity and binding-relevant completeness.
[[nodiscard]] std::shared_ptr<const ProgramBindingContract>
verify_program_binding_contract(
    std::string_view canonical_program_bytes,
    const RetainedBundle& retained_bundle);

}  // namespace manyforge::deployment
