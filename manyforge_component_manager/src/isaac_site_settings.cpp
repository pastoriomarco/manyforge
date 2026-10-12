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
    double startup_timeout, double service_timeout,
    std::map<std::string, IsaacSiteWorld, std::less<>> worlds,
    std::map<std::string, IsaacSitePoseObserver, std::less<>> pose_observers)
    : scene_startup_timeout_s_(startup_timeout),
      scene_service_timeout_s_(service_timeout), worlds_(std::move(worlds)),
      pose_observers_(std::move(pose_observers)) {}

double IsaacSiteSettings::scene_startup_timeout_s() const noexcept {
  return scene_startup_timeout_s_;
}

double IsaacSiteSettings::scene_service_timeout_s() const noexcept {
  return scene_service_timeout_s_;
}

const IsaacSiteWorld &
IsaacSiteSettings::ResolveWorld(std::string_view logical_world) const {
  const auto found = worlds_.find(logical_world);
  if (found == worlds_.end()) {
    Fail("unknown Isaac world '" + std::string(logical_world) +
         "' in site settings");
  }
  return found->second;
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
                {"schema", "scene_startup_timeout_s", "scene_service_timeout_s",
                 "worlds", "pose_observers_by_cell"});
  if (Text(Member(root, "schema"), "Isaac site settings schema") !=
      "manyforge.isaac_site.v1") {
    Fail("Isaac site settings schema must be manyforge.isaac_site.v1");
  }
  const double startup = Number(Member(root, "scene_startup_timeout_s"),
                                "scene_startup_timeout_s");
  const double service = Number(Member(root, "scene_service_timeout_s"),
                                "scene_service_timeout_s");
  const std::string worlds_label = "Isaac site worlds";
  std::map<std::string, IsaacSiteWorld, std::less<>> worlds;
  for (const auto &[id, value] :
       RequireObject(Member(root, "worlds"), worlds_label)) {
    const std::string label = "Isaac site worlds." + id;
    RequireFields(value, label, {"world_uri", "expected_world_name"});
    worlds.emplace(id, IsaacSiteWorld{Text(Member(value, "world_uri"),
                                           label + ".world_uri"),
                                      Text(Member(value, "expected_world_name"),
                                           label + ".expected_world_name")});
  }
  const std::string observers_label = "Isaac site observers";
  std::map<std::string, IsaacSitePoseObserver, std::less<>> observers;
  for (const auto &[id, value] : RequireObject(
           Member(root, "pose_observers_by_cell"), observers_label)) {
    const std::string label = "Isaac site pose_observers_by_cell." + id;
    RequireFields(value, label, {"site_config", "additional_required_topics"});
    const auto &topics = Member(value, "additional_required_topics");
    if (!topics.is_array()) {
      Fail(label + ".additional_required_topics must be an array");
    }
    std::vector<std::string> required_topics;
    for (const auto &topic : topics.as_array()) {
      required_topics.push_back(
          Text(topic, label + ".additional_required_topics"));
    }
    observers.emplace(id,
                      IsaacSitePoseObserver{Text(Member(value, "site_config"),
                                                 label + ".site_config"),
                                            std::move(required_topics)});
  }
  return IsaacSiteSettings(startup, service, std::move(worlds),
                           std::move(observers));
}

} // namespace manyforge::component_manager
