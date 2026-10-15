#include <algorithm>
#include <cmath>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/deployment_authoring.hpp>
#include <optional>
#include <set>

#include "native_execution_runtime_unit_internal.hpp"

namespace manyforge::deployment {
namespace {

[[noreturn]] void Reject(const std::string &message) {
  throw DeploymentArtifactCodecError("MoveIt jerk import: " + message);
}

CanonicalValue &Member(CanonicalValue::Object &object, std::string_view key) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [&](const auto &row) { return row.first == key; });
  if (found == object.end())
    Reject("missing field " + std::string(key));
  return found->second;
}

const std::string &Text(const CanonicalValue &value, std::string_view key) {
  const auto *member = value.find_object_member(key);
  const auto *text =
      member ? std::get_if<std::string>(&member->storage()) : nullptr;
  if (!text)
    Reject("invalid text field " + std::string(key));
  return *text;
}

CanonicalValue MergeJoint(const CanonicalValue &previous,
                          const CanonicalValue *source, const std::string &name,
                          std::string_view source_id,
                          const std::string &digest) {
  bool enabled = false;
  std::optional<double> maximum;
  if (source != nullptr) {
    if (!std::holds_alternative<CanonicalValue::Object>(source->storage()))
      Reject("joint " + name + " must contain a limits mapping");
    if (const auto *flag = source->find_object_member("has_jerk_limits")) {
      const auto *value = std::get_if<bool>(&flag->storage());
      if (!value)
        Reject("has_jerk_limits must be boolean for " + name);
      enabled = *value;
    }
    if (const auto *cap = source->find_object_member("max_jerk");
        cap && !cap->is_null()) {
      const auto *value = std::get_if<double>(&cap->storage());
      if (!value || !std::isfinite(*value) || *value < 0)
        Reject("max_jerk must be finite and nonnegative for " + name);
      if (*value > 0)
        maximum = *value;
    }
    if (enabled && !maximum)
      Reject("enabled max_jerk must be positive for " + name);
  }
  const auto *prior_import = previous.find_object_member("imported");
  const bool required =
      prior_import &&
      std::get<bool>(
          prior_import->find_object_member("machine_required")->storage());
  if (required &&
      (!enabled || !maximum ||
       *maximum >
           std::get<double>(
               prior_import->find_object_member("maximum_rad_s3")->storage())))
    Reject("cannot remove, disable or weaken the required limit for " + name);
  if (const auto *override = previous.find_object_member("override_rad_s3");
      override && enabled && maximum &&
      std::get<double>(override->storage()) > *maximum)
    Reject("existing override exceeds the new imported ceiling for " + name);
  auto result = previous.as_object();
  std::erase_if(result,
                [](const auto &row) { return row.first == "imported"; });
  if (maximum)
    result.emplace_back(
        "imported", CanonicalValue(CanonicalValue::Object{
                        {"maximum_rad_s3", CanonicalValue(*maximum)},
                        {"enabled", CanonicalValue(enabled)},
                        {"machine_required", CanonicalValue(required)},
                        {"source_id", CanonicalValue(std::string(source_id))},
                        {"source_digest", CanonicalValue(digest)},
                    }));
  return CanonicalValue(std::move(result));
}
} // namespace

// DX-753: retain archived native cold-import guards without admitting a native runtime product.
CompiledDeploymentDraft import_moveit_angular_jerk_limits(
    std::string_view source, std::string_view moveit_source,
    std::string_view source_id, std::string_view runtime_unit_id,
    std::string_view component_id, std::string_view target_id,
    std::span<const std::string> source_joint_names) {
  if (source_id.empty() || source_id.size() > 4096U ||
      source_joint_names.empty() || source_joint_names.size() > 256U)
    Reject("source attribution and a bounded explicit source-joint selection "
           "are required");
  std::set<std::string> names;
  for (const auto &name : source_joint_names)
    if (name.empty() || name.size() > 1024U || !names.insert(name).second)
      Reject("source joints must be nonempty and unique");
  const auto imported = parse_strict_json_model_yaml(moveit_source);
  const auto *limits = imported.root().find_object_member("joint_limits");
  if (!limits ||
      !std::holds_alternative<CanonicalValue::Object>(limits->storage()))
    Reject("source must contain a joint_limits mapping");
  const auto compiled = compile_deployment_draft(source);
  auto root =
      parse_strict_json_model_yaml(compiled.canonical_json).root().as_object();
  auto graph = Member(root, "graph").as_object();
  auto runtimes = Member(graph, "runtime_units").as_array();
  const auto runtime =
      std::find_if(runtimes.begin(), runtimes.end(), [&](const auto &row) {
        return Text(row, "runtime_unit_id") == runtime_unit_id;
      });
  if (runtime == runtimes.end() || Text(*runtime, "runtime_profile_id") !=
                                       detail::kNativeExecutionRuntimeProfileId)
    Reject("runtime does not name the qualified native angular profile");
  auto runtime_fields = runtime->as_object();
  auto config = Member(runtime_fields, "configuration").as_object();
  auto targets = Member(config, "motion_target_limits").as_array();
  const auto target =
      std::find_if(targets.begin(), targets.end(), [&](const auto &row) {
        return Text(row, "controllable_component_id") == component_id &&
               Text(row, "command_target_id") == target_id;
      });
  if (target == targets.end())
    Reject("motion target is not declared by the selected runtime");
  auto fields = target->as_object();
  auto settings = Member(fields, "ordinary_motion_jerk").as_object();
  const auto previous = Member(settings, "joint_limits").as_array();
  std::set<std::string> prior_names;
  for (const auto &row : previous) {
    const auto &name = Text(row, "source_joint_name");
    if (!names.contains(name) || !prior_names.insert(name).second)
      Reject("selection omits an existing joint or the saved table has "
             "duplicates");
  }
  CanonicalValue::Array merged;
  const auto digest = sha256_digest(moveit_source);
  for (const auto &name : names) {
    const auto prior =
        std::find_if(previous.begin(), previous.end(), [&](const auto &row) {
          return Text(row, "source_joint_name") == name;
        });
    auto value =
        MergeJoint(prior != previous.end()
                       ? *prior
                       : CanonicalValue(CanonicalValue::Object{
                             {"source_joint_name", CanonicalValue(name)}}),
                   limits->find_object_member(name), name, source_id, digest);
    if (value.as_object().size() > 1U)
      merged.push_back(std::move(value));
  }
  Member(settings, "joint_limits") = CanonicalValue(std::move(merged));
  Member(fields, "ordinary_motion_jerk") = CanonicalValue(std::move(settings));
  *target = CanonicalValue(std::move(fields));
  Member(config, "motion_target_limits") = CanonicalValue(std::move(targets));
  Member(runtime_fields, "configuration") = CanonicalValue(std::move(config));
  *runtime = CanonicalValue(std::move(runtime_fields));
  Member(graph, "runtime_units") = CanonicalValue(std::move(runtimes));
  Member(root, "graph") = CanonicalValue(std::move(graph));
  return compile_deployment_draft(
      canonical_json(CanonicalValue(std::move(root))));
}
} // namespace manyforge::deployment
