#pragma once

#include <cstdint>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

/// A cell state an assistant mode may run in (each mode declares its own):
/// `no_program_active`, no Program run exists (also no paused or held one).
enum class AssistantModeRunState : std::uint8_t {
  kNoProgramActive = 0,
};

[[nodiscard]] constexpr std::string_view to_string(
    AssistantModeRunState value) noexcept {
  switch (value) {
    case AssistantModeRunState::kNoProgramActive:
      return "no_program_active";
  }
  return {};
}

enum class AssistantImageTransport : std::uint8_t {
  kHttpUrl = 0,
  kInlineDataUrl,
};

[[nodiscard]] constexpr std::string_view to_string(
    AssistantImageTransport value) noexcept {
  switch (value) {
    case AssistantImageTransport::kHttpUrl:
      return "http_url";
    case AssistantImageTransport::kInlineDataUrl:
      return "inline_data_url";
  }
  return {};
}

enum class AssistantToolCallFamily : std::uint8_t {
  kOpenAiChatTools = 0,
  kQwen3Coder,
  kQwen3Xml,
};

[[nodiscard]] constexpr std::string_view to_string(
    AssistantToolCallFamily value) noexcept {
  switch (value) {
    case AssistantToolCallFamily::kOpenAiChatTools:
      return "openai_chat_tools";
    case AssistantToolCallFamily::kQwen3Coder:
      return "qwen3_coder";
    case AssistantToolCallFamily::kQwen3Xml:
      return "qwen3_xml";
  }
  return {};
}

enum class AssistantPromptResponseMode : std::uint8_t {
  kJsonObject = 0,
  kToolCall,
};

[[nodiscard]] constexpr std::string_view to_string(
    AssistantPromptResponseMode value) noexcept {
  switch (value) {
    case AssistantPromptResponseMode::kJsonObject:
      return "json_object";
    case AssistantPromptResponseMode::kToolCall:
      return "tool_call";
  }
  return {};
}

enum class AssistantToolEffect : std::uint8_t {
  kReadOnly = 0,
  kProposal,
  kComposerDraftMutating,
  kRuntimeMutating,
};

[[nodiscard]] constexpr std::string_view to_string(
    AssistantToolEffect value) noexcept {
  switch (value) {
    case AssistantToolEffect::kReadOnly:
      return "read_only";
    case AssistantToolEffect::kProposal:
      return "proposal";
    case AssistantToolEffect::kComposerDraftMutating:
      return "composer_draft_mutating";
    case AssistantToolEffect::kRuntimeMutating:
      return "runtime_mutating";
  }
  return {};
}

enum class AssistantSessionKind : std::uint8_t {
  kManualIntervention = 0,
  kDelegatedRecovery,
};

[[nodiscard]] constexpr std::string_view to_string(
    AssistantSessionKind value) noexcept {
  switch (value) {
    case AssistantSessionKind::kManualIntervention:
      return "manual_intervention";
    case AssistantSessionKind::kDelegatedRecovery:
      return "delegated_recovery";
  }
  return {};
}

enum class AssistantApprovalMode : std::uint8_t {
  kHumanApproved = 0,
  kPolicyGranted,
};

[[nodiscard]] constexpr std::string_view to_string(
    AssistantApprovalMode value) noexcept {
  switch (value) {
    case AssistantApprovalMode::kHumanApproved:
      return "human_approved";
    case AssistantApprovalMode::kPolicyGranted:
      return "policy_granted";
  }
  return {};
}

enum class AssistantReviewPolicy : std::uint8_t {
  kRequired = 0,
  kBoundedByRecoveryProfile,
  kNotRequired,
};

[[nodiscard]] constexpr std::string_view to_string(
    AssistantReviewPolicy value) noexcept {
  switch (value) {
    case AssistantReviewPolicy::kRequired:
      return "required";
    case AssistantReviewPolicy::kBoundedByRecoveryProfile:
      return "bounded_by_recovery_profile";
    case AssistantReviewPolicy::kNotRequired:
      return "not_required";
  }
  return {};
}

// Optional sampling parameters for the model server. Only the fields that are
// set are sent; everything else keeps the model's or server's defaults.
struct AssistantModelSamplingDTO final {
  std::optional<double> temperature;
  std::optional<double> top_p;
  std::optional<std::uint32_t> top_k;

  friend bool operator==(const AssistantModelSamplingDTO&,
                         const AssistantModelSamplingDTO&) = default;
};

struct AssistantModelEndpointRequestContractDTO final {
  double default_timeout_s;
  std::uint32_t max_output_tokens;
  bool supports_response_tools{false};
  bool supports_json_object{false};
  bool supports_image_input{false};
  std::vector<AssistantImageTransport> supported_image_transports;
  AssistantToolCallFamily tool_call_family;
  // EXPERIMENTAL (pending maintainer decision): `reasoning: on|off` asks the
  // model server to enable or disable its reasoning (thinking) mode; absent
  // keeps the model's default.
  std::optional<bool> reasoning;
  // Absent sends no sampling parameters at all.
  std::optional<AssistantModelSamplingDTO> sampling;

  friend bool operator==(const AssistantModelEndpointRequestContractDTO&,
                         const AssistantModelEndpointRequestContractDTO&) =
      default;
};

struct AssistantModelEndpointAliasDTO final {
  std::string model_endpoint_alias;
  ExternalResourceAlias external_resource_alias;
  std::string model_id;
  AssistantModelEndpointRequestContractDTO request_contract;

  friend bool operator==(const AssistantModelEndpointAliasDTO&,
                         const AssistantModelEndpointAliasDTO&) = default;
};

struct AssistantModelAuditPolicyDTO final {
  bool store_full_prompts{false};
  bool store_full_responses{false};
  bool redact_argument_values{true};

  friend bool operator==(const AssistantModelAuditPolicyDTO&,
                         const AssistantModelAuditPolicyDTO&) = default;
};

struct AssistantPromptResponseContractDTO final {
  AssistantPromptResponseMode response_mode;
  std::optional<std::string> tool_name;
  std::optional<std::string> tool_description;

  friend bool operator==(const AssistantPromptResponseContractDTO&,
                         const AssistantPromptResponseContractDTO&) = default;
};

/// One prompt-template variable and the Program value type it holds: bool,
/// float64, int64, string or string_list.
struct AssistantPromptTemplateVariableDTO final {
  std::string variable_id;
  std::string value_type;

  friend bool operator==(const AssistantPromptTemplateVariableDTO&,
                         const AssistantPromptTemplateVariableDTO&) = default;
};

struct AssistantPromptTemplateDTO final {
  std::string prompt_template_id;
  std::string version;
  std::string template_text;
  std::vector<AssistantPromptTemplateVariableDTO> variables;  // sorted by ID
  AssistantPromptResponseContractDTO response_contract;
  ContentDigest content_digest;

  friend bool operator==(const AssistantPromptTemplateDTO&,
                         const AssistantPromptTemplateDTO&) = default;
};

struct AssistantGrantPolicyDTO final {
  std::optional<AssistantSessionKind> session_kind;
  std::optional<AssistantApprovalMode> approval_mode;
  AssistantReviewPolicy review_policy;

  friend bool operator==(const AssistantGrantPolicyDTO&,
                         const AssistantGrantPolicyDTO&) = default;
};

// The authorable node set is not listed per mode: it is derived from the
// node catalog and the active Deployment's capabilities where the mode
// projection is built.
struct AssistantModeCatalogDTO final {
  std::vector<std::string> skill_ids;
  std::vector<std::string> tool_ids;

  friend bool operator==(const AssistantModeCatalogDTO&,
                         const AssistantModeCatalogDTO&) = default;
};

struct AssistantModeDTO final {
  std::string mode_id;
  bool enabled{false};
  std::string description;
  /// The Deployment's assistant component, whose bridge serves the mode.
  std::string assistant_component_id;
  std::string model_endpoint_alias;
  /// The cell states the mode may run in (sorted, non-empty).
  std::vector<AssistantModeRunState> runs_in;
  AssistantGrantPolicyDTO grant_policy;
  AssistantModeCatalogDTO catalog;
  // Repeated same-class tool failures get a recovery hint, then end the
  // request with a clarification (OD-76).
  bool stuck_loop_recovery{true};
  // EXPERIMENTAL: the bridge keeps completed requests in the conversation
  // history as request, answer and edit summary only.
  bool compact_history{false};
  // EXPERIMENTAL (E1): an argument-validation failure gets one extra model
  // call constrained to the full JSON Schema of that exact call.
  bool constrained_regeneration{false};

  friend bool operator==(const AssistantModeDTO&,
                         const AssistantModeDTO&) = default;
};

struct AssistantSettingsDTO final {
  AssistantModelAuditPolicyDTO audit_policy;
  std::vector<AssistantPromptTemplateDTO> prompt_templates;
  std::vector<AssistantModelEndpointAliasDTO> model_endpoint_aliases;
  std::vector<AssistantModeDTO> modes;

  [[nodiscard]] bool enabled() const noexcept {
    return !prompt_templates.empty() || !model_endpoint_aliases.empty() ||
           !modes.empty();
  }

  friend bool operator==(const AssistantSettingsDTO&,
                         const AssistantSettingsDTO&) = default;
};

struct AssistantSkillDescriptorDTO final {
  std::string skill_id;
  std::vector<std::string> requires_tools;

  friend bool operator==(const AssistantSkillDescriptorDTO&,
                         const AssistantSkillDescriptorDTO&) = default;
};

// Tool identity and effect only: descriptions and input schemas are built
// from the loaded node catalog where the mode projection is built.
struct AssistantToolDescriptorDTO final {
  std::string tool_id;
  AssistantToolEffect effect;

  friend bool operator==(const AssistantToolDescriptorDTO&,
                         const AssistantToolDescriptorDTO&) = default;
};

struct AssistantInstrumentRegistryDTO final {
  std::string api_version;
  ContentDigest registry_digest;
  std::vector<AssistantSkillDescriptorDTO> skills;
  std::vector<AssistantToolDescriptorDTO> tools;

  friend bool operator==(const AssistantInstrumentRegistryDTO&,
                         const AssistantInstrumentRegistryDTO&) = default;
};

/// Exact canonical non-empty assistant projection retained with a bundle.
///
/// The deployment package verifies the closed object represented by
/// `canonical_json` and its domain-separated digest before constructing a
/// RetainedBundle. Core consumers cannot treat arbitrary JSON as qualified
/// assistant settings merely by filling this transport value.
struct RetainedAssistantProjectionDTO final {
  ContentDigest projection_digest;
  std::string canonical_json;

  friend bool operator==(const RetainedAssistantProjectionDTO&,
                         const RetainedAssistantProjectionDTO&) = default;
};

}  // namespace manyforge::core
