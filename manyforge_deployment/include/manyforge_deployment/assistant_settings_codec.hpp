#pragma once

#include <manyforge_core/types/assistant.hpp>
#include <manyforge_core/types/deployment_applicability.hpp>

#include <memory>
#include <span>
#include <stdexcept>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {

inline constexpr std::string_view kAssistantInstrumentRegistryApiVersion =
    "manyforge.assistant_instrument_registry.v2";
inline constexpr std::string_view kRetainedAssistantProjectionApiVersion =
    "manyforge.retained_assistant.v3";

class AssistantSettingsCodecError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Strictly decoded source assistant block before external-resource and
/// instrument-catalog qualification. Absent and exact-empty source objects
/// both have disabled semantic meaning while source presence remains available
/// to the outer DeploymentRevision producer.
class TypedAssistantSettingsCandidate final {
 public:
  TypedAssistantSettingsCandidate(
      const TypedAssistantSettingsCandidate&) noexcept = default;
  TypedAssistantSettingsCandidate& operator=(
      const TypedAssistantSettingsCandidate&) noexcept = default;

  [[nodiscard]] bool source_was_present() const noexcept;
  [[nodiscard]] bool enabled() const noexcept;
  [[nodiscard]] const core::AssistantSettingsDTO& value() const noexcept;
  [[nodiscard]] const CanonicalValue& normalized_source() const noexcept;

 private:
  struct Data;
  explicit TypedAssistantSettingsCandidate(std::shared_ptr<const Data> data)
      : data_(std::move(data)) {}

  std::shared_ptr<const Data> data_;

  friend TypedAssistantSettingsCandidate decode_assistant_settings_candidate(
      const ParsedArtifactCandidate&);
  friend class QualifiedAssistantSettings;
};

/// Immutable build-owned assistant instrument registry. Callers cannot add
/// descriptors or substitute an identity.
class BuildQualifiedAssistantInstrumentRegistry final {
 public:
  [[nodiscard]] const core::AssistantInstrumentRegistryDTO& value()
      const noexcept;
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept;

 private:
  struct Data;
  explicit BuildQualifiedAssistantInstrumentRegistry(
      std::shared_ptr<const Data> data)
      : data_(std::move(data)) {}

  std::shared_ptr<const Data> data_;

  friend const BuildQualifiedAssistantInstrumentRegistry&
  build_qualified_assistant_instrument_registry();
};

[[nodiscard]] const BuildQualifiedAssistantInstrumentRegistry&
build_qualified_assistant_instrument_registry();

/// Pure compiler result after exact external-resource, registry, reserved-mode,
/// grant, catalog, skill-tool, and effect validation.
class QualifiedAssistantSettings final {
 public:
  QualifiedAssistantSettings(const QualifiedAssistantSettings&) noexcept =
      default;
  QualifiedAssistantSettings& operator=(
      const QualifiedAssistantSettings&) noexcept = default;

  [[nodiscard]] bool enabled() const noexcept;
  [[nodiscard]] const core::AssistantSettingsDTO& value() const noexcept;
  [[nodiscard]] const core::AssistantInstrumentRegistryDTO& instruments()
      const noexcept;
  [[nodiscard]] const CanonicalValue& normalized_projection() const noexcept;
  [[nodiscard]] const core::ContentDigest& projection_digest() const noexcept;
  [[nodiscard]] core::RetainedAssistantProjectionDTO retained_projection()
      const;

 private:
  struct Data;
  explicit QualifiedAssistantSettings(std::shared_ptr<const Data> data)
      : data_(std::move(data)) {}

  std::shared_ptr<const Data> data_;

  friend QualifiedAssistantSettings qualify_assistant_settings(
      const TypedAssistantSettingsCandidate&);
  friend QualifiedAssistantSettings verify_retained_assistant_projection(
      const CanonicalValue&);
};

[[nodiscard]] TypedAssistantSettingsCandidate
decode_assistant_settings_candidate(
    const ParsedArtifactCandidate& parsed_deployment);

[[nodiscard]] QualifiedAssistantSettings qualify_assistant_settings(
    const TypedAssistantSettingsCandidate& candidate);

/// Independently reconstructs one retained non-empty projection. A disabled
/// deployment has no retained assistant object and therefore never calls this
/// function.
[[nodiscard]] QualifiedAssistantSettings
verify_retained_assistant_projection(const CanonicalValue& projection);

}  // namespace manyforge::deployment
