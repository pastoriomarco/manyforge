#pragma once

#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_deployment/canonical_value.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::deployment {

class RevisionVerifiedDeploymentArtifact;

enum class ScenarioAssistantMode : std::uint8_t {
  kRequired = 0,
  kOptional,
  kForbidden,
};

struct ScenarioDeploymentCompatibilityDTO final {
  std::string lane;
  std::vector<std::string> provides;

  friend bool operator==(const ScenarioDeploymentCompatibilityDTO &,
                         const ScenarioDeploymentCompatibilityDTO &) = default;
};

[[nodiscard]] constexpr std::string_view
to_string(ScenarioAssistantMode mode) noexcept {
  switch (mode) {
  case ScenarioAssistantMode::kRequired:
    return "required";
  case ScenarioAssistantMode::kOptional:
    return "optional";
  case ScenarioAssistantMode::kForbidden:
    return "forbidden";
  }
  return {};
}

class ScenarioProgramAdmissionError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

[[nodiscard]] ScenarioDeploymentCompatibilityDTO
scenario_deployment_compatibility(
    const RevisionVerifiedDeploymentArtifact &deployment);

void verify_scenario_program_admission(
    const CanonicalValue &normalized_program_root,
    const core::RetainedBundleDTO &retained_bundle,
    const ScenarioDeploymentCompatibilityDTO &deployment_compatibility,
    ScenarioAssistantMode assistant_mode,
    const CanonicalValue &typed_parameter_overrides);

} // namespace manyforge::deployment
