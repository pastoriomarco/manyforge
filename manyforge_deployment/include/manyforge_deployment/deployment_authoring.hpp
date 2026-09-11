#pragma once

#include <span>
#include <string>
#include <string_view>

namespace manyforge::deployment {

/// One normalized, identity-complete Deployment Draft.
///
/// This is the sole build-qualified authoring entrypoint for
/// `manyforge.deployment.v1`. It parses the strict JSON-model document,
/// resolves only the runtime profiles selected by that document, recomputes
/// every nested/outer identity, and independently verifies the result. It has
/// no repository, Manager, Apply, or runtime effects.
struct CompiledDeploymentDraft final {
  std::string canonical_json;
  std::string deployment_artifact_id;
  std::string deployment_revision;
  std::string graph_revision;
};

[[nodiscard]] CompiledDeploymentDraft
compile_deployment_draft(std::string_view source);

/// Explicit offline migration of omitted controlled-Stop jerk choices. Existing
/// choices and limits remain unchanged; normal compilation/verification still
/// applies. Does not write a file, change a runtime, or infer an operator
/// choice.
[[nodiscard]] CompiledDeploymentDraft
migrate_controlled_stop_jerk_choice(std::string_view source, bool enabled);

/// Offline import for the currently qualified angular motion profile. Joint
/// names are explicit source-model symbols (not compiled/prefixed guesses).
/// Preserves activation/overrides; imports enable metadata, never infers a
/// machine-required limit. Normal activation still validates the exact model.
[[nodiscard]] CompiledDeploymentDraft import_moveit_angular_jerk_limits(
    std::string_view source, std::string_view moveit_source,
    std::string_view source_id, std::string_view runtime_unit_id,
    std::string_view component_id, std::string_view target_id,
    std::span<const std::string> source_joint_names);

/// Independently verify an already identity-complete Deployment document.
[[nodiscard]] CompiledDeploymentDraft
verify_deployment_draft(std::string_view source);

} // namespace manyforge::deployment
