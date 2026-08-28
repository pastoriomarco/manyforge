#pragma once

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

[[nodiscard]] CompiledDeploymentDraft compile_deployment_draft(std::string_view source);

/// Independently verify an already identity-complete Deployment document.
[[nodiscard]] CompiledDeploymentDraft verify_deployment_draft(std::string_view source);

}  // namespace manyforge::deployment
