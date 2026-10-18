#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/coordinate_state.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kCommandSchemaApiVersion = "manyforge.command_schema.v1";
inline constexpr std::string_view kIoSignalWriteCommandSchemaApiVersion =
    "manyforge.io_signal_write_command_schema.v1";
inline constexpr std::size_t kMaxCommandSchemaCompatibleCapabilities = 64U;
inline constexpr std::size_t kMaxCommandSchemaSelectors = 64U;
inline constexpr std::size_t kMaxCommandSchemaCallerOptions = 64U;
inline constexpr std::size_t kMaxCommandSchemaStopRequirements = 64U;

struct CommandSchemaCompatibleCapabilityDTO final {
  CapabilityId capability_id;
  CapabilityDefinitionRevision capability_definition_revision;

  friend bool operator==(const CommandSchemaCompatibleCapabilityDTO&,
                         const CommandSchemaCompatibleCapabilityDTO&) = default;
};

enum class CommandValueContractKind : std::uint8_t {
  kGripperPosition = 0,
  kIoSignalWrite = 1,
};

enum class CommandEffortLimitContract : std::uint8_t {
  kOptionalNonnegativeMatchingCoordinate = 0,
};

[[nodiscard]] constexpr std::string_view to_string(CommandValueContractKind value) noexcept {
  switch (value) {
    case CommandValueContractKind::kGripperPosition:
      return "gripper_position";
    case CommandValueContractKind::kIoSignalWrite:
      return "io_signal_write";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(CommandEffortLimitContract value) noexcept {
  switch (value) {
    case CommandEffortLimitContract::kOptionalNonnegativeMatchingCoordinate:
      return "optional_nonnegative_matching_coordinate";
  }
  return {};
}

struct GripperPositionCommandValueContractDTO final {
  std::vector<CoordinateKind> accepted_position_kinds;
  std::uint64_t minimum_resource_count{};
  std::uint64_t maximum_resource_count{};
  CommandEffortLimitContract max_effort_contract{
      CommandEffortLimitContract::kOptionalNonnegativeMatchingCoordinate};

  friend bool operator==(const GripperPositionCommandValueContractDTO&,
                         const GripperPositionCommandValueContractDTO&) = default;
};

struct IoSignalWriteCommandValueContractDTO final {
  friend bool operator==(IoSignalWriteCommandValueContractDTO,
                         IoSignalWriteCommandValueContractDTO) = default;
};

using CommandValueContractDTO =
    std::variant<GripperPositionCommandValueContractDTO, IoSignalWriteCommandValueContractDTO>;

enum class CommandCallerOptionValueKind : std::uint8_t {
  kNonnegativeDurationSeconds = 0,
};

[[nodiscard]] constexpr std::string_view to_string(CommandCallerOptionValueKind value) noexcept {
  switch (value) {
    case CommandCallerOptionValueKind::kNonnegativeDurationSeconds:
      return "nonnegative_duration_seconds";
  }
  return {};
}

struct CommandCallerOptionDTO final {
  CommandOptionId option_id;
  CommandCallerOptionValueKind value_kind;
  bool required{};

  friend bool operator==(const CommandCallerOptionDTO&, const CommandCallerOptionDTO&) = default;
};

enum class CommandCompletionRule : std::uint8_t {
  kGoalFencedTargetReached = 0,
};

enum class CommandConfirmationRule : std::uint8_t {
  kBindingFeedbackRequired = 0,
};

enum class CommandStopEvidenceRequirement : std::uint8_t {
  kCommandTerminal = 0,
};

[[nodiscard]] constexpr std::string_view to_string(CommandCompletionRule value) noexcept {
  switch (value) {
    case CommandCompletionRule::kGoalFencedTargetReached:
      return "goal_fenced_target_reached";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(CommandConfirmationRule value) noexcept {
  switch (value) {
    case CommandConfirmationRule::kBindingFeedbackRequired:
      return "binding_feedback_required";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(CommandStopEvidenceRequirement value) noexcept {
  switch (value) {
    case CommandStopEvidenceRequirement::kCommandTerminal:
      return "command_terminal";
  }
  return {};
}

/// Typed payload of the closed gripper or boolean signal-write schema branch.
struct CommandSchemaDTO final {
  CommandSchemaId command_schema_id;
  CommandSchemaRevision command_schema_revision;
  std::vector<CommandSchemaCompatibleCapabilityDTO> compatible_capabilities;
  CommandValueContractDTO command_value;
  std::vector<ResourceRoleId> resource_role_selectors;
  std::vector<ChannelRoleId> channel_role_selectors;
  std::vector<CommandCallerOptionDTO> caller_options;
  std::optional<CommandCompletionRule> completion_rule;
  std::optional<CommandConfirmationRule> confirmation_rule;
  std::vector<CommandStopEvidenceRequirement> stop_evidence_requirements;

  friend bool operator==(const CommandSchemaDTO&, const CommandSchemaDTO&) = default;
};

inline constexpr std::string_view kGripperPositionCommandSchemaId = "gripper.command_position";
inline constexpr std::string_view kGripperPositionCommandSchemaRevision =
    "sha256:4808a6bec7d020cb66df2082325e1838471a2604deaddfd1afe31dae427140e9";

/// Build-owned first I6 command family. Its semantic revision is pinned to
/// the exact normalized catalog record shipped by manyforge_deployment.
[[nodiscard]] inline CommandSchemaDTO builtin_gripper_position_command_schema() {
  const auto* capability = find_builtin_capability_definition("tool.gripper.command");
  if (capability == nullptr) {
    throw std::logic_error("build-owned gripper command capability is missing");
  }
  return CommandSchemaDTO{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision)),
      {CommandSchemaCompatibleCapabilityDTO{
          CapabilityId(std::string(capability->capability_id)),
          CapabilityDefinitionRevision(std::string(capability->capability_definition_revision))}},
      GripperPositionCommandValueContractDTO{
          {CoordinateKind::kAngular, CoordinateKind::kLinear},
          1U,
          4096U,
          CommandEffortLimitContract::kOptionalNonnegativeMatchingCoordinate},
      {},
      {},
      {CommandCallerOptionDTO{CommandOptionId("command_duration_s"),
                              CommandCallerOptionValueKind::kNonnegativeDurationSeconds, false}},
      CommandCompletionRule::kGoalFencedTargetReached,
      CommandConfirmationRule::kBindingFeedbackRequired,
      {CommandStopEvidenceRequirement::kCommandTerminal},
  };
}

inline constexpr std::string_view kIoSignalWriteCommandSchemaId = "io.signal_write_boolean";
inline constexpr std::string_view kIoSignalWriteCommandSchemaRevision =
    "sha256:2a0823f3255907443875c1428495eac7ab3945280477b95332f67fccd5ace9ea";

[[nodiscard]] inline CommandSchemaDTO builtin_io_signal_write_command_schema() {
  const auto* capability = find_builtin_capability_definition("io.gpio.set_output");
  if (capability == nullptr) {
    throw std::logic_error("build-owned output command capability is missing");
  }
  return {CommandSchemaId(std::string(kIoSignalWriteCommandSchemaId)),
          CommandSchemaRevision(std::string(kIoSignalWriteCommandSchemaRevision)),
          {{CapabilityId(std::string(capability->capability_id)),
            CapabilityDefinitionRevision(std::string(capability->capability_definition_revision))}},
          IoSignalWriteCommandValueContractDTO{},
          {},
          {},
          {},
          std::nullopt,
          std::nullopt,
          {}};
}

}  // namespace manyforge::core
