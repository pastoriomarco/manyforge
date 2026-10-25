#include "manyforge_component_manager/isaac_site_settings.hpp"

#include <algorithm>
#include <initializer_list>
#include <manyforge_deployment/artifact_codec.hpp>
#include <utility>

namespace manyforge::component_manager {
namespace {

using manyforge::deployment::CanonicalValue;

[[noreturn]] void Fail(const std::string &reason) {
  throw IsaacSiteSettingsError(reason);
}

const CanonicalValue::Object &RequireObject(const CanonicalValue &value,
                                            const std::string &label) {
  if (!value.is_object())
    Fail(label + " must be an object");
  return value.as_object();
}

void RequireFields(const CanonicalValue &value, const std::string &label,
                   std::initializer_list<std::string_view> fields) {
  for (const auto &[name, ignored] : RequireObject(value, label)) {
    static_cast<void>(ignored);
    if (std::find(fields.begin(), fields.end(), name) == fields.end()) {
      Fail(label + " has unknown field " + name);
    }
  }
  for (const auto field : fields) {
    if (value.find_object_member(field) == nullptr) {
      Fail(label + " is missing " + std::string(field));
    }
  }
}

const CanonicalValue &Member(const CanonicalValue &value,
                             std::string_view name) {
  return *value.find_object_member(name);
}

std::string Text(const CanonicalValue &value, const std::string &label) {
  if (!value.is_string())
    Fail(label + " must be text");
  return value.as_string();
}

double Number(const CanonicalValue &value, const std::string &label) {
  if (!value.is_number())
    Fail(label + " must be a number");
  return value.as_number();
}

} // namespace

IsaacSiteSettings::IsaacSiteSettings(
    double observer_configuration_timeout,
    std::map<std::string, IsaacSitePoseObserver, std::less<>> pose_observers)
    : observer_configuration_timeout_s_(observer_configuration_timeout),
      pose_observers_(std::move(pose_observers)) {}

double IsaacSiteSettings::observer_configuration_timeout_s() const noexcept {
  return observer_configuration_timeout_s_;
}

const IsaacSitePoseObserver *
IsaacSiteSettings::FindPoseObserver(std::string_view cell_id) const noexcept {
  const auto found = pose_observers_.find(cell_id);
  return found == pose_observers_.end() ? nullptr : &found->second;
}

IsaacSiteSettings ParseIsaacSiteSettings(std::string_view bytes) {
  CanonicalValue root;
  try {
    root = manyforge::deployment::parse_strict_json_model_yaml(bytes).root();
  } catch (const std::exception &error) {
    Fail("cannot decode Isaac site settings: " + std::string(error.what()));
  }
  RequireFields(root, "Isaac site settings",
                {"schema", "observer_configuration_timeout_s",
                 "pose_observers_by_cell"});
  if (Text(Member(root, "schema"), "Isaac site settings schema") !=
      "manyforge.isaac_attach_site.v1") {
    Fail("Isaac site settings schema must be manyforge.isaac_attach_site.v1");
  }
  const double timeout = Number(Member(root, "observer_configuration_timeout_s"),
                                "observer_configuration_timeout_s");
  const std::string observers_label = "Isaac site observers";
  std::map<std::string, IsaacSitePoseObserver, std::less<>> observers;
  for (const auto &[id, value] : RequireObject(
           Member(root, "pose_observers_by_cell"), observers_label)) {
    const std::string label = "Isaac site pose_observers_by_cell." + id;
    RequireFields(value, label, {"site_config"});
    observers.emplace(id,
                      IsaacSitePoseObserver{Text(Member(value, "site_config"),
                                                 label + ".site_config")});
  }
  return IsaacSiteSettings(timeout, std::move(observers));
}

} // namespace manyforge::component_manager
