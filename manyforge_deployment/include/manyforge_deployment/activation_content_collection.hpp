#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::deployment {

/// The configuration the cell runs now: the published bundle and the Program
/// and binding of the Apply that published it (both absent for a bootstrap
/// publication; the binding is absent for a non-executable Program), plus
/// the bundle that Apply replaced (the Scene runtime keeps reading it until
/// the Apply's Scene initialization completes). `scene_mesh_digests` names
/// the scene meshes the cell uses beyond those: those of the active Apply and
/// of the current logical Scene (a mesh captured when an API scene call added
/// its object is named by nothing else). A scene mesh references nothing, so
/// its bytes are never read for references.
struct ActiveActivationContent final {
  std::vector<core::RetainedBundleId> retained_bundle_ids;
  std::optional<core::ProgramRevision> program_revision;
  std::optional<core::ProgramBindingId> program_binding_id;
  std::vector<core::ContentDigest> scene_mesh_digests;
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
  /// Live documents whose references an earlier collection had read.
  std::size_t reused_documents{0U};
};

/// The digest tokens of the live documents named by their content digest
/// (store entries and prepared activations; immutable under their name), as
/// the last collection found them. The next collection reads only the live
/// documents not in here, and keeps exactly the documents it finds live.
/// Owned by the one caller that runs the collections; starts empty, so the
/// first collection reads every live document.
struct ActivationContentReferences final {
  std::unordered_map<std::string, std::vector<std::string>> tokens_by_path;
};

/// Delete the content-addressed entries of one activation root (retained
/// bundles, Programs, Program bindings, raw and catalog artifacts) that no
/// live configuration references. Live are: the active configuration, every
/// retained prepared activation and every canonical-empty admission at the
/// root, every raw artifact `active` names, and every raw or catalog artifact
/// those name (transitively). A name
/// counts as referenced when it occurs in a live document, so the rule keeps
/// more rather than less. Build manifests and prepared activations are
/// bounded elsewhere and never touched.
///
/// The Manager calls this after each successful Apply. It holds the root's
/// writer lock exclusively, so it never runs while a writer (a preparation,
/// or Headless storing and committing a runtime-captured scene mesh) holds
/// it; it then returns `collected == false` and the next Apply collects.
/// `active` is evaluated only once that exclusive lock is held, so content a
/// writer stored and made live before releasing its hold is in it.
/// `known` carries the references already read (see
/// `ActivationContentReferences`); it is left unchanged when deferred.
[[nodiscard]] ActivationContentCollection collect_unreferenced_activation_content(
    const std::filesystem::path &activation_root,
    const std::function<ActiveActivationContent()> &active,
    ActivationContentReferences &known);

/// Shared hold of the activation root's writer lock for one preparation: from
/// its first content write until its prepared activation is published (from
/// then on the prepared activation keeps that content live). Headless takes
/// the same lock file shared around storing a runtime-captured scene mesh and
/// committing it to the Scene (`ActivationPreparer.content_writer_hold`), so
/// the stored mesh is live, named by the Scene, before collection can run.
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
