#include "native_execution_runtime_unit_internal.hpp"
#include <manyforge_deployment/execution_runtime_contract.hpp>

#include <algorithm>
#include <cmath>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <set>
#include <string_view>

namespace manyforge::deployment {
namespace {
namespace execution = execution_runtime_contract;

const core::JsonValueDTO &member(const core::JsonObjectDTO &object,
                                 std::string_view key) {
  const auto found = object.find(key);
  if (found == object.end())
    throw RetainedBundleError("ordinary jerk settings missing " + std::string(key));
  return found->second;
}

template <typename T>
const T &value(const core::JsonObjectDTO &object, std::string_view key) {
  const auto *result = std::get_if<T>(&member(object, key).value);
  if (!result)
    throw RetainedBundleError("ordinary jerk settings invalid " + std::string(key));
  return *result;
}

bool positive(double number) { return std::isfinite(number) && number > 0.0; }

std::optional<std::string>
invalid_settings(const execution::OrdinaryMotionJerkSettings &settings) {
  if (!positive(settings.preset_reference_time_s))
    return "ordinary jerk preset reference is invalid";
  std::set<std::string> seen;
  for (const auto &setting : settings.joint_limits) {
    if (setting.source_joint_name.empty() ||
        !seen.insert(setting.source_joint_name).second ||
        (setting.override_rad_s3 && !positive(*setting.override_rad_s3)))
      return "ordinary jerk setting has an empty/duplicate joint or invalid "
             "override";
    if (!setting.imported)
      continue;
    const auto &source = *setting.imported;
    if (!positive(source.maximum_rad_s3) || source.source_id.empty() ||
        !core::IsExactLowercaseSha256Digest(source.source_digest.value()) ||
        (source.machine_required && (!source.enabled || !settings.enabled)))
      return "imported jerk limit is invalid, lacks provenance, or a required "
             "limit is disabled";
    if (source.enabled && setting.override_rad_s3 &&
        *setting.override_rad_s3 > source.maximum_rad_s3)
      return "ordinary jerk override exceeds the imported ceiling for " +
             setting.source_joint_name;
  }
  return std::nullopt;
}

core::Result<core::OrdinaryMotionJerkPolicy> invalid(std::string message) {
  return core::Result<core::OrdinaryMotionJerkPolicy>::Err(
      core::ErrorDTO{core::kErrorInvalidRequest,
                     std::move(message),
                     {{"reason", "ordinary_motion_jerk_settings_invalid"}}});
}
} // namespace

execution::OrdinaryMotionJerkSettings detail::decode_ordinary_motion_jerk_settings(
    const core::JsonObjectDTO &source) {
  execution::OrdinaryMotionJerkSettings result{
      value<bool>(source, "enabled"),
      value<double>(source, "preset_reference_time_s"),
      {}};
  for (const auto &item :
       value<core::JsonValueDTO::Array>(source, "joint_limits")) {
    const auto *row = std::get_if<core::JsonObjectDTO>(&item.value);
    if (!row)
      throw RetainedBundleError("ordinary jerk settings require joint records");
    execution::JointJerkSetting setting{
        value<std::string>(*row, "source_joint_name"), {}, {}};
    if (row->contains("override_rad_s3"))
      setting.override_rad_s3 = value<double>(*row, "override_rad_s3");
    if (row->contains("imported")) {
      const auto &imported = value<core::JsonObjectDTO>(*row, "imported");
      setting.imported = execution::ImportedJointJerkLimit{
          value<double>(imported, "maximum_rad_s3"),
          value<bool>(imported, "enabled"),
          value<bool>(imported, "machine_required"),
          value<std::string>(imported, "source_id"),
          core::ContentDigest(value<std::string>(imported, "source_digest"))};
    }
    result.joint_limits.push_back(std::move(setting));
  }
  if (const auto error = invalid_settings(result))
    throw RetainedBundleError(*error);
  return result;
}

core::Result<core::OrdinaryMotionJerkPolicy>
execution::resolve_ordinary_motion_jerk_policy(
    const OrdinaryMotionJerkSettings &settings,
    std::span<const core::RuntimeResourceBindingDTO> resources,
    const core::JointLimitsDTO &model_limits) {
  if (const auto error = invalid_settings(settings))
    return invalid(*error);
  if (settings.joint_limits.size() > resources.size())
    return invalid("ordinary jerk joint count is invalid");
  for (const auto &setting : settings.joint_limits) {
    const auto resource =
        std::find_if(resources.begin(), resources.end(), [&](const auto &row) {
          return row.source_joint_name == setting.source_joint_name;
        });
    if (resource == resources.end())
      return invalid("ordinary jerk setting has an unknown joint");
  }

  core::OrdinaryMotionJerkPolicy policy{settings.enabled, false, {}};
  for (const auto &resource : resources) {
    if (!resource.source_joint_name ||
        resource.coordinate_kind != core::CoordinateKind::kAngular)
      return invalid(
          "ordinary native jerk limits require angular model joints");
    const auto setting = std::find_if(
        settings.joint_limits.begin(), settings.joint_limits.end(),
        [&](const auto &row) {
          return row.source_joint_name == resource.source_joint_name;
        });
    std::optional<double> maximum;
    if (setting != settings.joint_limits.end()) {
      maximum = setting->override_rad_s3;
      if (setting->imported && setting->imported->enabled) {
        policy.machine_required |= setting->imported->machine_required;
        if (!maximum)
          maximum = setting->imported->maximum_rad_s3;
      }
    }
    if (!maximum && settings.enabled) {
      const auto model = std::find(model_limits.joint_names.begin(),
                                   model_limits.joint_names.end(),
                                   resource.compiled_resource_name);
      const auto index =
          static_cast<std::size_t>(model - model_limits.joint_names.begin());
      if (model == model_limits.joint_names.end() ||
          index >= model_limits.acceleration_max_rad_s2.size() ||
          !positive(model_limits.acceleration_max_rad_s2[index]))
        return invalid(
            "ordinary jerk preset requires the model acceleration for " +
            *resource.source_joint_name);
      maximum = core::motion_jerk_preset_limit(
          model_limits.acceleration_max_rad_s2[index],
          settings.preset_reference_time_s);
    }
    if (maximum) {
      if (!positive(*maximum))
        return invalid(
            "resolved ordinary jerk limit is not finite and positive");
      policy.limits.emplace(*resource.source_joint_name,
                            core::AngularJerkDTO{*maximum});
    }
  }
  return core::Result<core::OrdinaryMotionJerkPolicy>::Ok(std::move(policy));
}
} // namespace manyforge::deployment
