#include <manyforge_roboplan/component_adapter_registry.hpp>
#include <manyforge_roboplan/controller_launch_artifacts.hpp>

#include <ament_index_cpp/get_resource.hpp>
#include <ament_index_cpp/get_resources.hpp>
#include <dlfcn.h>

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {

struct ComponentAdapterRegistry::Library final {
  void *handle{};
  ~Library() {
    if (handle != nullptr) static_cast<void>(::dlclose(handle));
  }
};

namespace {

core::ErrorDTO load_failed(const std::string &family, std::string message) {
  return {"component_adapter_load_failed", std::move(message), {{"adapter_family", family}}};
}

std::string trimmed(std::string text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  const auto last = text.find_last_not_of(" \t\r\n");
  return first == std::string::npos ? std::string() : text.substr(first, last - first + 1U);
}

}  // namespace

ComponentAdapterRegistry::ComponentAdapterRegistry(std::vector<ComponentAdapterFamily> families) {
  for (const auto &family : families) families_.emplace(std::string(family.family), family);
}

ComponentAdapterRegistry::ComponentAdapterRegistry(ComponentAdapterRegistry &&) noexcept = default;
ComponentAdapterRegistry &ComponentAdapterRegistry::operator=(ComponentAdapterRegistry &&) noexcept =
    default;
ComponentAdapterRegistry::~ComponentAdapterRegistry() = default;

core::Result<ComponentAdapterRegistry> ComponentAdapterRegistry::LoadInstalled() {
  using Reply = core::Result<ComponentAdapterRegistry>;
  const std::string type(kComponentAdapterResourceType);
  ComponentAdapterRegistry registry({});
  for (const auto &[name, prefix] : ament_index_cpp::get_resources_by_name(type)) {
    const auto resource = ament_index_cpp::get_resource(type, name);
    if (!resource.resourcePath.has_value())
      return Reply::Err(load_failed(name, "The adapter's index entry can't be read"));
    const auto library = prefix / "lib" / trimmed(resource.contents);
    auto loaded = std::make_shared<Library>();
    loaded->handle = ::dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (loaded->handle == nullptr)
      return Reply::Err(load_failed(name, "The adapter library " + library.string() +
                                              " can't be loaded: " + ::dlerror()));
    const auto entry = reinterpret_cast<ComponentAdapterFamilyEntry>(
        ::dlsym(loaded->handle, std::string(kComponentAdapterFamilySymbol).c_str()));
    const auto *family = entry != nullptr ? entry() : nullptr;
    if (family == nullptr || family->make == nullptr || family->family != name)
      return Reply::Err(load_failed(
          name, "The adapter library " + library.string() + " doesn't export family " + name));
    registry.families_.emplace(name, *family);
    registry.libraries_.push_back(std::move(loaded));
  }
  return Reply::Ok(std::move(registry));
}

core::Result<ComponentAdapters> ComponentAdapterRegistry::Make(
    const ComponentAdapterContext &context) const {
  using Reply = core::Result<ComponentAdapters>;
  const auto &family = context.row.adapter;
  const auto found = families_.find(family);
  if (found == families_.end())
    return Reply::Err({"component_adapter_unknown",
                       "No adapter is installed for '" + family + "'",
                       {{"component_id", context.component_id.value()},
                        {"adapter_family", family}}});
  try {
    auto adapters = found->second.make(context);
    if (adapters.component) return Reply::Ok(std::move(adapters));
    return Reply::Err({"component_adapter_invalid",
                       "The " + family + " adapter made no adapter",
                       {{"component_id", context.component_id.value()},
                        {"adapter_family", family}}});
  } catch (const std::invalid_argument &error) {
    return Reply::Err({"component_adapter_invalid", error.what(),
                       {{"component_id", context.component_id.value()},
                        {"adapter_family", family}}});
  }
}

LaunchContribution ComponentAdapterRegistry::LaunchContributionOf(const ModelLaneRow &row,
                                                                  const bool arm) const {
  const auto found = families_.find(row.adapter);
  if (found == families_.end() || found->second.launch_contribution == nullptr)
    throw ControllerLaunchArtifactError("no installed adapter contributes a launch for '" +
                                        row.adapter + "'");
  try {
    const auto vendor = row.adapter_keys.find("vendor");
    if (vendor == row.adapter_keys.end()) return found->second.launch_contribution(row, arm);
    // A vendor's model on another lane (Mock, Isaac): the vendor's controllers
    // on the lane's hardware.
    const auto source = families_.find(vendor->second);
    if (source == families_.end() || source->second.controllers == nullptr)
      throw ControllerLaunchArtifactError("no installed adapter supplies the controllers of '" +
                                          vendor->second + "'");
    auto lane_row = row;
    lane_row.vendor_controllers_yaml = source->second.controllers(row);
    return found->second.launch_contribution(lane_row, arm);
  } catch (const std::invalid_argument &error) {
    throw ControllerLaunchArtifactError(error.what());
  }
}

const ComponentAdapterFamily *ComponentAdapterRegistry::Find(const std::string_view adapter) const {
  const auto found = families_.find(adapter);
  return found == families_.end() ? nullptr : &found->second;
}

std::vector<std::string> ComponentAdapterRegistry::families() const {
  std::vector<std::string> names;
  for (const auto &[name, family] : families_) names.push_back(name);
  return names;
}

}  // namespace manyforge::roboplan_backend
