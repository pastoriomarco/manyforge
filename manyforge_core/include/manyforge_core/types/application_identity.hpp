#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace manyforge::core {

/// The application-start identity `A` (OD-122 (3), (14); contract 1.1).
/// Minted once by the Manager when the application starts; every
/// application process receives it at launch and keeps it for life. 0 is
/// never minted and is refused by the parser.
struct ApplicationStartId final {
  std::uint64_t value{};
  friend bool operator==(const ApplicationStartId &,
                         const ApplicationStartId &) = default;
};

/// The only wire and file form: exactly 16 lowercase hex characters, most
/// significant nibble first, not all zeros. No allocation.
[[nodiscard]] constexpr std::optional<ApplicationStartId>
ParseApplicationStartId(const std::string_view text) noexcept {
  if (text.size() != 16U) return std::nullopt;
  std::uint64_t value = 0U;
  for (const char character : text) {
    std::uint64_t nibble = 0U;
    if (character >= '0' && character <= '9') {
      nibble = static_cast<std::uint64_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      nibble = static_cast<std::uint64_t>(character - 'a') + 10U;
    } else {
      return std::nullopt;
    }
    value = (value << 4U) | nibble;
  }
  if (value == 0U) return std::nullopt;
  return ApplicationStartId{value};
}

[[nodiscard]] constexpr std::array<char, 16U>
FormatApplicationStartId(const ApplicationStartId id) noexcept {
  constexpr char kHex[] = "0123456789abcdef";
  std::array<char, 16U> text{};
  for (std::size_t index = 0U; index < text.size(); ++index) {
    const auto shift = static_cast<unsigned>((15U - index) * 4U);
    text[index] = kHex[(id.value >> shift) & 0x0fU];
  }
  return text;
}

/// One enumeration of process and owner roles (Q13, build spec 12;
/// contract 1.2). An identity is (role, A); nothing is composed. Values are
/// never reused; 0 is invalid.
enum class ApplicationRole : std::uint8_t {
  kManager = 1,
  kLogicalScene = 2,
  kSceneRuntime = 3,
  kAuthoredSceneOwner = 4,
  kExecutionHost = 5,
  kKernel = 6,
  kProgramRunner = 7,
  kHeadless = 8,
  kHeadlessProjection = 9,
  kHeadlessAlarm = 10,
  kComposer = 11,
  kAuthoringOwner = 12,
  kComposerSnapshot = 13,
  kManualOperationOwner = 14,
  kInterventionOrRecoveryOwner = 15,
  kPoseSource = 16,
};

inline constexpr std::array<std::pair<ApplicationRole, std::string_view>, 16U>
    kApplicationRoleTexts{{
        {ApplicationRole::kManager, "manager"},
        {ApplicationRole::kLogicalScene, "logical_scene"},
        {ApplicationRole::kSceneRuntime, "scene_runtime"},
        {ApplicationRole::kAuthoredSceneOwner, "authored_scene_owner"},
        {ApplicationRole::kExecutionHost, "execution_host"},
        {ApplicationRole::kKernel, "kernel"},
        {ApplicationRole::kProgramRunner, "program_runner"},
        {ApplicationRole::kHeadless, "headless"},
        {ApplicationRole::kHeadlessProjection, "headless_projection"},
        {ApplicationRole::kHeadlessAlarm, "headless_alarm"},
        {ApplicationRole::kComposer, "composer"},
        {ApplicationRole::kAuthoringOwner, "authoring_owner"},
        {ApplicationRole::kComposerSnapshot, "composer_snapshot"},
        {ApplicationRole::kManualOperationOwner, "manual_operation_owner"},
        {ApplicationRole::kInterventionOrRecoveryOwner,
         "intervention_or_recovery_owner"},
        {ApplicationRole::kPoseSource, "pose_source"},
    }};

/// Text form, at most 32 bytes. Every enumerator has a text.
[[nodiscard]] constexpr std::string_view
ApplicationRoleText(const ApplicationRole role) noexcept {
  return kApplicationRoleTexts[static_cast<std::size_t>(role) - 1U].second;
}

[[nodiscard]] constexpr std::optional<ApplicationRole>
ParseApplicationRole(const std::string_view text) noexcept {
  for (const auto &[role, spelling] : kApplicationRoleTexts) {
    if (spelling == text) return role;
  }
  return std::nullopt;
}

}  // namespace manyforge::core
