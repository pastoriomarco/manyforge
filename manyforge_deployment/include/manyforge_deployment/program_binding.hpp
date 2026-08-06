#pragma once

#include <manyforge_core/types/program_binding.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::deployment {

inline constexpr std::size_t kMaxDurableProgramBindingCount = 4096U;
inline constexpr std::uint64_t kMaxDurableProgramBindingBytes =
    512ULL * 1024ULL * 1024ULL;

namespace detail {
class ProgramBindingConstructionAuthority;
}

class ProgramBindingError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Immutable, identity-verified resolution of one Program revision against
/// one exact retained bundle.
///
/// Construction is sealed. A read consumer receives this value only after
/// the complete closed envelope, content identity, selected bundle, retained
/// graph, node environment, and generated role-map coordinates agree.
class ProgramBinding final {
 public:
  ~ProgramBinding() = default;
  ProgramBinding(const ProgramBinding&) = delete;
  ProgramBinding& operator=(const ProgramBinding&) = delete;
  ProgramBinding(ProgramBinding&&) = delete;
  ProgramBinding& operator=(ProgramBinding&&) = delete;

  [[nodiscard]] const core::ProgramBindingId& program_binding_id()
      const noexcept {
    return program_binding_id_;
  }
  [[nodiscard]] const core::ProgramBindingDTO& value() const noexcept {
    return value_;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return normalized_root_;
  }
  [[nodiscard]] std::string_view canonical_bytes() const noexcept {
    return canonical_bytes_;
  }

 private:
  ProgramBinding(core::ProgramBindingId program_binding_id,
                 core::ProgramBindingDTO value,
                 CanonicalValue normalized_root, std::string canonical_bytes)
      : program_binding_id_(std::move(program_binding_id)),
        value_(std::move(value)),
        normalized_root_(std::move(normalized_root)),
        canonical_bytes_(std::move(canonical_bytes)) {}

  core::ProgramBindingId program_binding_id_;
  core::ProgramBindingDTO value_;
  CanonicalValue normalized_root_;
  std::string canonical_bytes_;

  friend class detail::ProgramBindingConstructionAuthority;
};

/// Reconstruct one binding only in the context that gives its coordinates
/// meaning. Canonical bytes that are self-consistent but select another
/// bundle, graph, node environment, or generated model map fail closed.
[[nodiscard]] std::shared_ptr<const ProgramBinding>
reload_verified_program_binding(
    std::string_view canonical_bytes, const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map);

/// Re-run the complete intrinsic identity proof over an already sealed
/// binding. This does not replace contextual bundle/model-map verification.
[[nodiscard]] bool verify_program_binding_identity(
    const ProgramBinding& binding) noexcept;

/// Immutable Program-Binding repository port.
///
/// Publication and lookup are both contextual: a self-consistent envelope is
/// not accepted unless its exact retained bundle and compiler-owned model map
/// still give every role coordinate the same meaning. The repository owns no
/// active/current alias and performs no Apply or runtime selection.
class IProgramBindingRepository {
 public:
  virtual ~IProgramBindingRepository() = default;

  [[nodiscard]] virtual bool put_if_absent(
      std::shared_ptr<const ProgramBinding> binding,
      const RetainedBundle& retained_bundle,
      const VerifiedRetainedModelMap& retained_model_map) = 0;

  [[nodiscard]] virtual std::shared_ptr<const ProgramBinding> get_verified(
      const core::ProgramBindingId& program_binding_id,
      const RetainedBundle& retained_bundle,
      const VerifiedRetainedModelMap& retained_model_map) const = 0;
};

/// Durable content-addressed Program-Binding repository.
///
/// Entries are stored by ProgramBindingId below one pre-existing trusted
/// package/state root using the same no-replace, fsync, symlink-rejecting
/// durable collection as retained bundles. Reopen verifies every intrinsic
/// identity; point reads additionally require the caller-supplied exact bundle
/// and model-map context.
class FilesystemProgramBindingRepository final
    : public IProgramBindingRepository {
 public:
  explicit FilesystemProgramBindingRepository(
      std::filesystem::path trusted_root,
      std::size_t max_binding_count = kMaxDurableProgramBindingCount,
      std::uint64_t max_total_bytes = kMaxDurableProgramBindingBytes,
      std::uint64_t max_single_binding_bytes = kMaxArtifactEncodedBytes);
  ~FilesystemProgramBindingRepository() override;

  FilesystemProgramBindingRepository(
      const FilesystemProgramBindingRepository&) = delete;
  FilesystemProgramBindingRepository& operator=(
      const FilesystemProgramBindingRepository&) = delete;
  FilesystemProgramBindingRepository(
      FilesystemProgramBindingRepository&&) = delete;
  FilesystemProgramBindingRepository& operator=(
      FilesystemProgramBindingRepository&&) = delete;

  [[nodiscard]] bool put_if_absent(
      std::shared_ptr<const ProgramBinding> binding,
      const RetainedBundle& retained_bundle,
      const VerifiedRetainedModelMap& retained_model_map) override;

  [[nodiscard]] std::shared_ptr<const ProgramBinding> get_verified(
      const core::ProgramBindingId& program_binding_id,
      const RetainedBundle& retained_bundle,
      const VerifiedRetainedModelMap& retained_model_map) const override;

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::uint64_t retained_bytes() const;

 private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
};

}  // namespace manyforge::deployment
