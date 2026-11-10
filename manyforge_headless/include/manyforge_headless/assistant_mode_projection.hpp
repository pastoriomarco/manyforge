#pragma once

#include <cstdint>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::headless {

inline constexpr std::string_view kAssistantModeProjectionApiVersion =
    "manyforge.assistant_mode_projection.v2";
inline constexpr std::string_view kAssistantModeIndexApiVersion =
    "manyforge.assistant_mode_index.v2";
inline constexpr std::uint64_t kMaximumAssistantModeProjectionBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumAssistantModeIndexBytes = 8ULL * 1024ULL * 1024ULL;

class AssistantModeProjectionError : public manyforge::core::TypedError {
 public:
  AssistantModeProjectionError(std::string code, std::string message)
      : manyforge::core::TypedError(std::move(code), message) {}
};

/// Build the native part of one mode projection from an independently
/// verified bundle: tool identities and effects, skills, grant policy, model
/// endpoint, `stuck_loop_recovery`, `compact_history` and
/// `constrained_regeneration`. The Headless Python service joins the
/// catalog part (tool descriptions and input schemas, the derived node set,
/// `instrument_registry_digest`, `catalog_hash`, `source_content_digest`).
///
/// This publishes descriptors and their retained identities. It grants no
/// callback or runtime availability by itself. `model_endpoint` carries the
/// AI model component serving the mode's alias (the row named by the alias's
/// `external_resource_alias`: `ai_model_component_id`, `base_url`,
/// `api_key_env` name or null); without it the projection fails
/// `assistant_ai_model_absent`. `assistant_component_id` names the assistant
/// component serving the mode and `runs_in` the cell states it may run in;
/// without an assistant component (no `assistant.bridge.request` provider)
/// it fails `assistant_component_absent`.
[[nodiscard]] manyforge::content_identity::CanonicalValue ProjectAssistantMode(
    const manyforge::deployment::RetainedBundle& active_bundle, std::string_view mode_id);

/// Build the bounded index of enabled modes from the same independently
/// verified assistant projection used by ProjectAssistantMode.
///
/// The index exists for discovery only. `ai_model_component_id` is the AI
/// model component of the chat alias (`assistant_model`), or null. A caller must still fetch the exact
/// selected mode projection before offering or invoking any instrument.
[[nodiscard]] manyforge::content_identity::CanonicalValue ProjectAssistantModeIndex(
    const manyforge::deployment::RetainedBundle& active_bundle);

}  // namespace manyforge::headless
