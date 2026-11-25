#include <manyforge_deployment/scenario_program_admission.hpp>

#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_core/types/program_contract.hpp>

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>

namespace manyforge::deployment {
namespace {

[[noreturn]] void Fail(const std::string &message) {
  throw ScenarioProgramAdmissionError(message);
}

[[nodiscard]] const CanonicalValue::Object &Object(
    const CanonicalValue &value, std::string_view path) {
  const auto *object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr)
    Fail(std::string(path) + " must be a mapping");
  return *object;
}

[[nodiscard]] const CanonicalValue::Array &Array(
    const CanonicalValue &value, std::string_view path) {
  const auto *array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr)
    Fail(std::string(path) + " must be an array");
  return *array;
}

[[nodiscard]] const std::string &Text(const CanonicalValue &value,
                                      std::string_view path) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr)
    Fail(std::string(path) + " must be text");
  return *text;
}

[[nodiscard]] const CanonicalValue &Member(const CanonicalValue &value,
                                           std::string_view name,
                                           std::string_view path) {
  const auto *member = value.find_object_member(name);
  if (member == nullptr)
    Fail(std::string(path) + " is missing " + std::string(name));
  return *member;
}

[[nodiscard]] bool Identifier(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U || value.front() < 'a' ||
      value.front() > 'z')
    return false;
  return std::all_of(value.begin() + 1, value.end(), [](const char item) {
    return (item >= 'a' && item <= 'z') || (item >= '0' && item <= '9') ||
           item == '_' || item == '.' || item == '-';
  });
}

[[nodiscard]] std::string IdentifierValue(const CanonicalValue &value,
                                          const std::string &path) {
  const auto &text = Text(value, path);
  if (!Identifier(text))
    Fail(path + " must be a bounded lowercase identifier");
  return text;
}

[[nodiscard]] const CanonicalValue *OptionalObject(
    const CanonicalValue &root, std::string_view name,
    const std::string &path) {
  const auto *value = root.find_object_member(name);
  if (value != nullptr)
    static_cast<void>(Object(*value, path + "." + std::string(name)));
  return value;
}

[[nodiscard]] std::set<std::string, std::less<>> StringSet(
    const CanonicalValue &root, std::string_view name,
    const std::string &path) {
  std::set<std::string, std::less<>> result;
  const auto *value = root.find_object_member(name);
  if (value == nullptr)
    return result;
  const auto field_path = path + "." + std::string(name);
  const auto &values = Array(*value, field_path);
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (!result.emplace(IdentifierValue(
            values[index], field_path + "[" + std::to_string(index) + "]"))
             .second)
      Fail(field_path + " must not contain duplicates");
  }
  return result;
}

[[nodiscard]] ScenarioDeploymentCompatibilityDTO Compatibility(
    const CanonicalValue &provides_value, const std::string &path) {
  ScenarioDeploymentCompatibilityDTO result;
  const auto &provides = Array(provides_value, path + ".provides");
  if (provides.size() > 4096U)
    Fail(path + ".provides exceeds its fixed bound");
  for (std::size_t index = 0; index < provides.size(); ++index)
    result.provides.push_back(IdentifierValue(
        provides[index], path + ".provides[" + std::to_string(index) + "]"));
  if (!std::is_sorted(result.provides.begin(), result.provides.end()) ||
      std::adjacent_find(result.provides.begin(), result.provides.end()) !=
          result.provides.end())
    Fail(path + ".provides must be duplicate-free and canonical-sorted");
  return result;
}

} // namespace

ScenarioDeploymentCompatibilityDTO scenario_deployment_compatibility(
    const RevisionVerifiedDeploymentArtifact &deployment) {
  const auto *compatibility = OptionalObject(
      deployment.normalized_authored_root(), "compatibility", "deployment");
  if (compatibility == nullptr)
    Fail("scenario deployment has no compatibility projection");
  return Compatibility(
      Member(*compatibility, "provides", "deployment.compatibility"),
      "deployment.compatibility");
}

void verify_scenario_program_admission(
    const CanonicalValue &program, const core::RetainedBundleDTO &bundle,
    const ScenarioDeploymentCompatibilityDTO &deployment_compatibility,
    const ScenarioAssistantMode assistant_mode) {
  CanonicalValue::Array provides;
  for (const auto &item : deployment_compatibility.provides)
    provides.emplace_back(item);
  static_cast<void>(Compatibility(CanonicalValue(std::move(provides)),
                                  "scenario.deployment_compatibility"));

  if (Text(Member(program, "program_profile", "Program"),
           "Program.program_profile") != core::kProgramProfile)
    Fail("scenario Program does not use manyforge-cell-v1");
  const auto &catalog = Member(program, "catalog", "Program");
  if (Text(Member(catalog, "api_version", "Program.catalog"),
           "Program.catalog.api_version") != "manyforge.node_catalog.v3" ||
      Text(Member(catalog, "content_version", "Program.catalog"),
           "Program.catalog.content_version") !=
          bundle.catalogs.node_environment.content_version)
    Fail("scenario Program selects another retained node catalog");

  std::set<std::string, std::less<>> requirements;
  std::optional<std::string> program_assistant;
  if (const auto *compatibility = OptionalObject(program, "compatibility", "Program");
      compatibility != nullptr) {
    requirements = StringSet(*compatibility, "requires", "Program.compatibility");
    if (const auto *assistant = compatibility->find_object_member("assistant");
        assistant != nullptr)
      program_assistant = Text(*assistant, "Program.compatibility.assistant");
  }
  const std::set<std::string, std::less<>> provided(
      deployment_compatibility.provides.begin(),
      deployment_compatibility.provides.end());
  if (!std::includes(provided.begin(), provided.end(), requirements.begin(),
                     requirements.end()))
    Fail("scenario deployment does not provide every Program requirement");

  const bool assistant_enabled = bundle.assistant.has_value();
  if (assistant_mode == ScenarioAssistantMode::kRequired &&
      (!assistant_enabled || program_assistant == "forbidden"))
    Fail("required scenario assistant mode is not admissible");
  if (assistant_mode == ScenarioAssistantMode::kForbidden &&
      (assistant_enabled || program_assistant == "required"))
    Fail("forbidden scenario assistant mode is not admissible");
  if (assistant_mode == ScenarioAssistantMode::kOptional &&
      program_assistant != "optional")
    Fail("optional scenario assistant mode requires an optional Program");
}

} // namespace manyforge::deployment
