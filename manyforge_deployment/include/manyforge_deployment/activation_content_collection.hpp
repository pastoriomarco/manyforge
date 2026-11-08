#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::deployment {

/// The configuration the cell runs now: the published bundle and the Program
/// and binding of the Apply that published it (both absent for a bootstrap
/// publication; the binding is absent for a non-executable Program), plus
/// the bundle that Apply replaced (the Scene runtime keeps reading it until
/// the Apply's Scene initialization completes).
struct ActiveActivationContent final {
  std::vector<core::RetainedBundleId> retained_bundle_ids;
  std::optional<core::ProgramRevision> program_revision;
  std::optional<core::ProgramBindingId> program_binding_id;
};

struct ActivationContentCollection final {
  /// False when a preparation was writing; nothing was removed then.
  bool collected{false};
  /// Committed entries of the five stores looked at for removal.
  std::size_t examined_entries{0U};
  std::size_t removed_entries{0U};
  std::uint64_t removed_bytes{0U};
  /// Bytes of the live documents read to find the referenced entries.
  std::uint64_t read_bytes{0U};
};

/// Delete the content-addressed entries of one activation root (retained
/// bundles, Programs, Program bindings, raw and catalog artifacts) that no
/// live configuration references. Live are: the active configuration, every
/// retained prepared activation and every canonical-empty admission at the
/// root, and every raw or catalog artifact those name (transitively). A name
/// counts as referenced when it occurs in a live document, so the rule keeps
/// more rather than less. Build manifests and prepared activations are
/// bounded elsewhere and never touched.
///
/// The Manager calls this after each successful Apply. It holds the root's
/// writer lock exclusively, so it never runs while a preparation is writing
/// (it then returns `collected == false` and the next Apply collects).
[[nodiscard]] ActivationContentCollection collect_unreferenced_activation_content(
    const std::filesystem::path &activation_root,
    const ActiveActivationContent &active);

/// Shared hold of the activation root's writer lock for one preparation: from
/// its first content write until its prepared activation is published (from
/// then on the prepared activation keeps that content live).
class ActivationContentWriterLock final {
public:
  explicit ActivationContentWriterLock(
      const std::filesystem::path &activation_root);
  ~ActivationContentWriterLock();

  ActivationContentWriterLock(const ActivationContentWriterLock &) = delete;
  ActivationContentWriterLock &
  operator=(const ActivationContentWriterLock &) = delete;

private:
  int descriptor_;
};

} // namespace manyforge::deployment
