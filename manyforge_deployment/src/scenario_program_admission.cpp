#include <manyforge_deployment/scenario_program_admission.hpp>

#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_core/types/program_contract.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>

namespace manyforge::deployment {
namespace {

[[noreturn]] void Fail(const std::string &message) {
  throw ScenarioProgramAdmissionError(message);
}

[[nodiscard]] const CanonicalValue::Object &Object(
    const CanonicalValue &value, const std::string &path) {
  const auto *object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr)
    Fail(path + " must be a mapping");
  return *object;
}

[[nodiscard]] const CanonicalValue::Array &Array(
    const CanonicalValue &value, const std::string &path) {
  const auto *array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr)
    Fail(path + " must be an array");
  return *array;
}

[[nodiscard]] const std::string &Text(const CanonicalValue &value,
                                      const std::string &path) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr)
    Fail(path + " must be text");
  return *text;
}

[[nodiscard]] const CanonicalValue &Member(const CanonicalValue &value,
                                           std::string_view name,
                                           const std::string &path) {
  const auto *member = value.find_object_member(name);
  if (member == nullptr)
    Fail(path + " is missing " + std::string(name));
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
    const CanonicalValue &value, const std::string &path) {
  const auto &object = Object(value, path);
  if (object.size() != 2U || value.find_object_member("lane") == nullptr ||
      value.find_object_member("provides") == nullptr)
    Fail(path + " must contain exactly lane and provides");
  ScenarioDeploymentCompatibilityDTO result{
      IdentifierValue(Member(value, "lane", path), path + ".lane"), {}};
  if (result.lane != "manyforge-only" && result.lane != "isaac-sim" &&
      result.lane != "real-robot")
    Fail(path + ".lane is unsupported");
  const auto &provides = Array(Member(value, "provides", path), path + ".provides");
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

[[nodiscard]] double Number(const CanonicalValue &value,
                            const std::string &path) {
  const auto *number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number))
    Fail(path + " must be a finite number");
  return *number;
}

[[nodiscard]] std::size_t Utf8Length(std::string_view value) noexcept {
  return static_cast<std::size_t>(
      std::count_if(value.begin(), value.end(), [](const unsigned char item) {
        return (item & 0xc0U) != 0x80U;
      }));
}

void ValidateOverride(const CanonicalValue &value,
                      const CanonicalValue &declaration,
                      const std::set<std::string, std::less<>> &resources,
                      const std::string &path) {
  const auto &type = Text(Member(declaration, "type", path), path + ".type");
  if (type == "bool") {
    if (std::get_if<bool>(&value.storage()) == nullptr)
      Fail(path + " does not match bool");
  } else if (type == "float64") {
    static_cast<void>(Number(value, path));
  } else if (type == "int64") {
    const auto number = Number(value, path);
    if (std::trunc(number) != number || std::abs(number) > 9007199254740991.0)
      Fail(path + " does not match int64");
  } else if (type == "scene_resource" || type == "string") {
    const auto &text = Text(value, path);
    if (text.empty() || (type == "scene_resource" && !resources.contains(text)))
      Fail(path + " does not match " + type);
  } else if (type == "string_list") {
    for (const auto &item : Array(value, path))
      static_cast<void>(Text(item, path));
  } else if (type == "cartesian_path") {
    const auto &items = Array(value, path);
    if (items.size() < 2U)
      Fail(path + " does not match cartesian_path");
    for (const auto &item : items)
      static_cast<void>(Object(item, path));
  } else if (type == "coordinate_position" || type == "model_response" ||
             type == "pick_planning_result" ||
             type == "scene_selection_context" ||
             type == "scene_selection_result" || type == "stored_pose") {
    static_cast<void>(Object(value, path));
  } else {
    Fail(path + " has an unsupported Program parameter type");
  }

  const auto *constraints = declaration.find_object_member("constraints");
  if (constraints == nullptr)
    return;
  if (type == "float64" || type == "int64") {
    const auto number = Number(value, path);
    for (const auto &[name, bound_value] : Object(*constraints, path + ".constraints")) {
      const auto bound = Number(bound_value, path + ".constraints." + name);
      const bool valid = (name == "min_inclusive" && number >= bound) ||
                         (name == "min_exclusive" && number > bound) ||
                         (name == "max_inclusive" && number <= bound) ||
                         (name == "max_exclusive" && number < bound);
      if (!valid)
        Fail(path + " violates " + name);
    }
  } else if (type == "string" || type == "string_list") {
    const auto length = type == "string" ? Utf8Length(Text(value, path))
                                           : Array(value, path).size();
    for (const auto &[name, bound_value] : Object(*constraints, path + ".constraints")) {
      const auto bound = Number(bound_value, path + ".constraints." + name);
      const bool valid =
          (name == "min_length" && static_cast<double>(length) >= bound) ||
          (name == "max_length" && static_cast<double>(length) <= bound);
      if (!valid)
        Fail(path + " violates " + name);
    }
  }
}

void ValidateOverrides(const CanonicalValue &program,
                       const CanonicalValue &overrides) {
  std::set<std::string, std::less<>> resources;
  if (const auto *scene = program.find_object_member("scene"); scene != nullptr)
    for (const auto &resource : Array(*scene, "Program.scene"))
      resources.emplace(IdentifierValue(
          Member(resource, "scene_resource_id", "Program.scene[]"),
          "Program.scene[].scene_resource_id"));

  std::map<std::string, const CanonicalValue *, std::less<>> declarations;
  if (const auto *parameters = program.find_object_member("parameters");
      parameters != nullptr)
    for (const auto &declaration : Array(*parameters, "Program.parameters")) {
      const auto id = IdentifierValue(
          Member(declaration, "parameter_id", "Program.parameters[]"),
          "Program.parameters[].parameter_id");
      if (!declarations.emplace(id, &declaration).second)
        Fail("Program parameters contain a duplicate identifier");
    }

  for (const auto &[id, value] : Object(overrides, "typed_parameter_overrides")) {
    const auto found = declarations.find(id);
    if (found == declarations.end())
      Fail("typed_parameter_overrides selects an undeclared Program parameter");
    if (Text(Member(*found->second, "runtime_mutability", "Program parameter"),
             "Program parameter.runtime_mutability") == "immutable")
      Fail("typed_parameter_overrides selects an immutable Program parameter");
    ValidateOverride(value, *found->second, resources,
                     "typed_parameter_overrides." + id);
  }
}

} // namespace

ScenarioDeploymentCompatibilityDTO scenario_deployment_compatibility(
    const RevisionVerifiedDeploymentArtifact &deployment) {
  const auto *compatibility = OptionalObject(
      deployment.normalized_authored_root(), "compatibility", "deployment");
  if (compatibility == nullptr)
    Fail("scenario deployment has no compatibility projection");
  CanonicalValue::Object projection{
      {"lane", Member(*compatibility, "lane", "deployment.compatibility")},
      {"provides", Member(*compatibility, "provides", "deployment.compatibility")},
  };
  return Compatibility(CanonicalValue(std::move(projection)),
                       "deployment.compatibility");
}

void verify_scenario_program_admission(
    const CanonicalValue &program, const core::RetainedBundleDTO &bundle,
    const ScenarioDeploymentCompatibilityDTO &deployment_compatibility,
    const ScenarioAssistantMode assistant_mode,
    const CanonicalValue &typed_parameter_overrides) {
  CanonicalValue::Array provides;
  for (const auto &item : deployment_compatibility.provides)
    provides.emplace_back(item);
  static_cast<void>(Compatibility(
      CanonicalValue(CanonicalValue::Object{
          {"lane", CanonicalValue(deployment_compatibility.lane)},
          {"provides", CanonicalValue(std::move(provides))},
      }),
      "scenario.deployment_compatibility"));

  if (Text(Member(program, "program_profile", "Program"),
           "Program.program_profile") != core::kComposableCellProgramProfile)
    Fail("scenario Program does not use composable-cell-v1");
  const auto &catalog = Member(program, "catalog", "Program");
  if (Text(Member(catalog, "api_version", "Program.catalog"),
           "Program.catalog.api_version") != "manyforge.node_catalog.v2" ||
      Text(Member(catalog, "content_version", "Program.catalog"),
           "Program.catalog.content_version") !=
          bundle.catalogs.node_environment.content_version)
    Fail("scenario Program selects another retained node catalog");

  std::set<std::string, std::less<>> lanes;
  std::set<std::string, std::less<>> requirements;
  std::optional<std::string> program_assistant;
  if (const auto *compatibility = OptionalObject(program, "compatibility", "Program");
      compatibility != nullptr) {
    lanes = StringSet(*compatibility, "lanes", "Program.compatibility");
    requirements = StringSet(*compatibility, "requires", "Program.compatibility");
    if (const auto *assistant = compatibility->find_object_member("assistant");
        assistant != nullptr)
      program_assistant = Text(*assistant, "Program.compatibility.assistant");
  }
  if (!lanes.empty() && !lanes.contains(deployment_compatibility.lane))
    Fail("scenario deployment lane is incompatible with the Program");
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
  ValidateOverrides(program, typed_parameter_overrides);
}

} // namespace manyforge::deployment
