#pragma once

#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::deployment {

class ICatalogArtifactSource;

inline constexpr std::size_t kMaxDurableProgramCount = 4096U;
inline constexpr std::uint64_t kMaxDurableProgramBytes =
    512ULL * 1024ULL * 1024ULL;

class ProgramContractError final : public std::invalid_argument {
public:
  struct UnsupportedFormat final {
    std::string actual_api_version;
    std::string supported_api_version;
  };
  using std::invalid_argument::invalid_argument;
  ProgramContractError(const std::string &message, std::string code)
      : std::invalid_argument(message), code_(std::move(code)) {}
  ProgramContractError(const std::string &message, UnsupportedFormat format)
      : std::invalid_argument(message), code_("program_format_unsupported"),
        unsupported_format_(std::move(format)) {}
  [[nodiscard]] const std::string &code() const noexcept { return code_; }
  [[nodiscard]] const std::optional<UnsupportedFormat> &unsupported_format() const noexcept {
    return unsupported_format_;
  }

private:
  std::string code_;
  std::optional<UnsupportedFormat> unsupported_format_;
};

/// Reject unsupported versions before normalization or identity checks.
/// Missing and non-string API versions remain structural errors.
void require_supported_program_format(const CanonicalValue &source_root);

/// Pure bounded authoring normalization shared by revision tools and the
/// verifier. This is not a Program/catalog/binding proof or a migration.
[[nodiscard]] CanonicalValue
normalize_program_authoring(const CanonicalValue &source_root);

/// Read-only view of the build-owned node catalog, qualified against the
/// candidate's exact retained catalog identity. The referenced data has
/// process lifetime. This does not validate an authored Program.
struct QualifiedProgramCatalogView final {
  const CanonicalValue &schema_types;
  const std::map<std::string, CanonicalValue, std::less<>> &entries;
};

[[nodiscard]] QualifiedProgramCatalogView qualified_program_catalog(
    const core::RetainedNodeEnvironmentDTO &environment);

namespace detail {
class ProgramBindingContractConstructionAuthority;
}

/// Sealed binding-relevant projection of one canonical Program revision.
///
/// The complete canonical Program remains attached to the projection.
/// The existing Program loader validates node-catalog parameters and dataflow
/// on the exact captured Program before native preparation derives effects.
/// Durable reload rechecks the admitted Program's shape and identity, and
/// this boundary validates references against the exact retained candidate
/// before deriving and sealing its binding-relevant projection.
class ProgramBindingContract final {
public:
  ~ProgramBindingContract() = default;
  ProgramBindingContract(const ProgramBindingContract &) = delete;
  ProgramBindingContract &operator=(const ProgramBindingContract &) = delete;
  ProgramBindingContract(ProgramBindingContract &&) = delete;
  ProgramBindingContract &operator=(ProgramBindingContract &&) = delete;

  [[nodiscard]] const core::ProgramBindingContractDTO &value() const noexcept {
    return value_;
  }
  [[nodiscard]] const CanonicalValue &normalized_program_root() const noexcept {
    return normalized_program_root_;
  }
  [[nodiscard]] std::string_view canonical_program_bytes() const noexcept {
    return canonical_program_bytes_;
  }
  [[nodiscard]] const core::ProgramExecutionTableDTO &
  execution_table() const noexcept {
    return execution_table_;
  }
  [[nodiscard]] const std::vector<core::ProgramBlackboardKeyDeclarationDTO> &
  blackboard_key_declarations() const noexcept {
    return blackboard_key_declarations_;
  }

private:
  ProgramBindingContract(core::ProgramBindingContractDTO value,
                         CanonicalValue normalized_program_root,
                         std::string canonical_program_bytes,
                         core::ProgramExecutionTableDTO execution_table,
                         std::vector<core::ProgramBlackboardKeyDeclarationDTO>
                             blackboard_key_declarations)
      : value_(std::move(value)),
        normalized_program_root_(std::move(normalized_program_root)),
        canonical_program_bytes_(std::move(canonical_program_bytes)),
        execution_table_(std::move(execution_table)),
        blackboard_key_declarations_(std::move(blackboard_key_declarations)) {}

  core::ProgramBindingContractDTO value_;
  CanonicalValue normalized_program_root_;
  std::string canonical_program_bytes_;
  core::ProgramExecutionTableDTO execution_table_;
  std::vector<core::ProgramBlackboardKeyDeclarationDTO>
      blackboard_key_declarations_;

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
verify_program_binding_contract(std::string_view canonical_program_bytes,
                                const RetainedBundle &retained_bundle);

/// Verify against the exact immutable model map and catalog before deriving effects.
[[nodiscard]] std::shared_ptr<const ProgramBindingContract>
verify_program_binding_contract(std::string_view canonical_program_bytes,
                                const RetainedBundle &retained_bundle,
                                const std::optional<VerifiedRetainedModelMap> &model_map,
                                const ICatalogArtifactSource &catalog_source);

/// Immutable content-addressed Program read port.
///
/// ProgramRevision addresses the complete canonical Program. Lookup remains
/// contextual: the exact retained bundle must still validate every catalog
/// and capability reference before a Program is returned.
class IProgramSource {
public:
  virtual ~IProgramSource() = default;

  [[nodiscard]] virtual std::shared_ptr<const ProgramBindingContract>
  get_verified(const core::ProgramId &program_id,
               const core::ProgramRevision &program_revision,
               const RetainedBundle &retained_bundle) const = 0;
};

/// Immutable content-addressed Program publication and read port.
class IProgramRepository : public IProgramSource {
public:
  ~IProgramRepository() override = default;

  [[nodiscard]] virtual bool
  put_if_absent(std::shared_ptr<const ProgramBindingContract> program,
                const RetainedBundle &retained_bundle) = 0;
};

/// Get-only Program source below one immutable package root.
class FilesystemVerifiedProgramSource final : public IProgramSource {
public:
  explicit FilesystemVerifiedProgramSource(
      std::filesystem::path trusted_package_root,
      std::uint64_t maximum_single_program_bytes = kMaxArtifactEncodedBytes);

  [[nodiscard]] std::shared_ptr<const ProgramBindingContract>
  get_verified(const core::ProgramId &program_id,
               const core::ProgramRevision &program_revision,
               const RetainedBundle &retained_bundle) const override;

private:
  std::filesystem::path trusted_package_root_;
  std::uint64_t maximum_single_program_bytes_;
};

/// Durable Program repository below one pre-existing trusted package root.
///
/// Entries are stored by ProgramRevision with no current/latest alias. Reopen
/// proves canonical content identity; each point lookup repeats the complete
/// retained-bundle contextual verification before returning the sealed value.
class FilesystemProgramRepository final : public IProgramRepository {
public:
  explicit FilesystemProgramRepository(
      std::filesystem::path trusted_root,
      std::size_t max_program_count = kMaxDurableProgramCount,
      std::uint64_t max_total_bytes = kMaxDurableProgramBytes,
      std::uint64_t max_single_program_bytes = kMaxArtifactEncodedBytes);
  ~FilesystemProgramRepository() override;

  FilesystemProgramRepository(const FilesystemProgramRepository &) = delete;
  FilesystemProgramRepository &
  operator=(const FilesystemProgramRepository &) = delete;
  FilesystemProgramRepository(FilesystemProgramRepository &&) = delete;
  FilesystemProgramRepository &
  operator=(FilesystemProgramRepository &&) = delete;

  [[nodiscard]] bool
  put_if_absent(std::shared_ptr<const ProgramBindingContract> program,
                const RetainedBundle &retained_bundle) override;

  [[nodiscard]] std::shared_ptr<const ProgramBindingContract>
  get_verified(const core::ProgramId &program_id,
               const core::ProgramRevision &program_revision,
               const RetainedBundle &retained_bundle) const override;

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::uint64_t retained_bytes() const;

private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
};

} // namespace manyforge::deployment
