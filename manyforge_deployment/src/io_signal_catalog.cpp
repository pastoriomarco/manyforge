#include <manyforge_deployment/io_signal_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace manyforge::deployment {
namespace {

struct InvalidConfiguration final : std::runtime_error {
  using std::runtime_error::runtime_error;
};

using Object = core::JsonValueDTO::Object;
using Array = core::JsonValueDTO::Array;

const core::JsonValueDTO &member(const Object &object, std::string_view key) {
  const auto found = object.find(key);
  if (found == object.end())
    throw InvalidConfiguration("io_signal_configuration_invalid");
  return found->second;
}

template <class T> const T &value(const core::JsonValueDTO &input) {
  const auto *result = std::get_if<T>(&input.value);
  if (!result)
    throw InvalidConfiguration("io_signal_configuration_invalid");
  return *result;
}

void fields(const Object &object,
            std::initializer_list<std::string_view> required,
            std::initializer_list<std::string_view> optional = {}) {
  for (const auto key : required)
    (void)member(object, key);
  for (const auto &[key, unused] : object) {
    (void)unused;
    if (std::find(required.begin(), required.end(), key) == required.end() &&
        std::find(optional.begin(), optional.end(), key) == optional.end())
      throw InvalidConfiguration("io_signal_configuration_invalid");
  }
}

std::string text(const Object &object, std::string_view key,
                 bool identity = true) {
  const auto &result = value<std::string>(member(object, key));
  if (result.empty() || result.size() > (identity ? 128U : 4096U) ||
      result.find('\0') != std::string::npos)
    throw InvalidConfiguration("io_signal_configuration_invalid");
  if (identity) {
    const auto alpha_numeric = [](unsigned char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9');
    };
    if (!alpha_numeric(result.front()) ||
        !std::all_of(result.begin(), result.end(), [&](unsigned char c) {
          return alpha_numeric(c) || c == '.' || c == '_' || c == '-';
        }))
      throw InvalidConfiguration("io_signal_configuration_invalid");
  }
  return result;
}

std::uint64_t counter(const Object &object, std::string_view key,
                      std::uint64_t maximum) {
  const auto number = value<double>(member(object, key));
  if (!std::isfinite(number) || number < 0.0 || std::trunc(number) != number ||
      number > static_cast<double>(maximum))
    throw InvalidConfiguration("io_signal_configuration_invalid");
  return static_cast<std::uint64_t>(number);
}

std::string absolute_address(const Object &provider, std::string_view field) {
  const auto result = text(provider, field, false);
  // ROS tokens are absolute and already resolved: no private names,
  // substitutions, empty tokens or namespace-dependent aliases survive.
  if (result.front() != '/' || result.size() == 1U || result.back() == '/')
    throw InvalidConfiguration("io_signal_provider_address_invalid");
  bool token_start = true;
  for (const unsigned char c : std::string_view(result).substr(1U)) {
    if (c == '/') {
      if (token_start)
        throw InvalidConfiguration("io_signal_provider_address_invalid");
      token_start = true;
      continue;
    }
    const bool alpha =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    if (!alpha && (token_start || c < '0' || c > '9'))
      throw InvalidConfiguration("io_signal_provider_address_invalid");
    token_start = false;
  }
  return result;
}

std::string provider_selector(const Object &provider, std::string_view field) {
  const auto result = text(provider, field, false);
  if (!content_identity::is_utf8_nfc(result))
    throw InvalidConfiguration("io_signal_provider_selector_invalid");
  return result;
}

core::ResourceId output_resource(const std::string &profile,
                                 const std::string &channel) {
  using content_identity::CanonicalValue;
  // All local providers use the existing host ROS context. This host-scoped
  // reservation deliberately excludes role, binding and component aliases.
  const auto digest = content_identity::framed_sha256(
      "io-output-resource", "manyforge.io_output_channel.v1",
      CanonicalValue(
          CanonicalValue::Object{{"profile", CanonicalValue(profile)},
                                 {"channel", CanonicalValue(channel)}}));
  return core::ResourceId("io.output." + digest.substr(7U));
}

core::ResourceId
typed_output_resource(const IoSignalProviderConfiguration &address) {
  using content_identity::CanonicalValue;
  CanonicalValue::Object key{{"profile", CanonicalValue(address.profile)}};
  if (address.profile == "ur_set_io") {
    key.emplace_back("service", CanonicalValue(*address.service));
    key.emplace_back("function", CanonicalValue("set_digital_out"));
    key.emplace_back("pin", CanonicalValue(static_cast<double>(*address.pin)));
  } else {
    key.emplace_back("topic", CanonicalValue(*address.topic));
    key.emplace_back("group", CanonicalValue(*address.group));
    key.emplace_back("interface_name", CanonicalValue(*address.interface_name));
  }
  const auto digest = content_identity::framed_sha256(
      "io-output-resource", "manyforge.io_output_channel.v1",
      CanonicalValue(std::move(key)));
  return core::ResourceId("io.output." + digest.substr(7U));
}

} // namespace

core::Result<std::vector<RetainedIoSignalConfiguration>>
ResolveRetainedIoSignalCatalog(
    const core::CellGraphDTO &graph,
    const std::optional<core::RuntimeUnitId> &runtime_unit_id) {
  using Reply = core::Result<std::vector<RetainedIoSignalConfiguration>>;
  try {
    std::vector<RetainedIoSignalConfiguration> result;
    std::set<core::IoSignalId> identities;
    std::set<std::tuple<core::ControllableComponentId, core::CommandTargetId,
                        core::ResourceRoleId>>
        roles;
    std::map<core::InterruptSourceId,
             std::tuple<core::RuntimeUnitId, std::string,
                        std::optional<std::string>, std::size_t,
                        std::optional<std::string>, std::optional<std::string>,
                        std::optional<core::IoSignalLivenessMode>>>
        sources;
    for (const auto &unit : graph.runtime_units) {
      if (runtime_unit_id && unit.runtime_unit_id != *runtime_unit_id)
        continue;
      const auto declared = unit.configuration.find("io_signals");
      if (declared == unit.configuration.end())
        continue;
      const auto &rows = value<Array>(declared->second);
      if (rows.size() > 4096U - result.size())
        throw InvalidConfiguration("io_signal_configuration_invalid");
      for (const auto &item : rows) {
        const auto &row = value<Object>(item);
        const auto direction = text(row, "direction");
        const bool output = direction == "output";
        if (direction != "input" && !output)
          throw InvalidConfiguration("io_signal_direction_mismatch");
        if (output)
          fields(row, {"controllable_component_id", "command_target_id",
                       "resource_role_id", "signal_name", "source_id",
                       "value_type", "direction", "provider"});
        else
          fields(row, {"controllable_component_id", "command_target_id",
                       "resource_role_id", "signal_name", "source_id",
                       "value_type", "direction", "update_mode",
                       "transition_capability", "freshness", "provider"});
        const core::ControllableComponentId component_id(
            text(row, "controllable_component_id"));
        const core::CommandTargetId target_id(text(row, "command_target_id"));
        const core::ResourceRoleId role_id(text(row, "resource_role_id"));
        const auto component = std::find_if(
            graph.controllable_components.begin(),
            graph.controllable_components.end(), [&](const auto &c) {
              return c.controllable_component_id == component_id;
            });
        if (component == graph.controllable_components.end() ||
            std::find(component->runtime_unit_ids.begin(),
                      component->runtime_unit_ids.end(),
                      unit.runtime_unit_id) ==
                component->runtime_unit_ids.end())
          throw InvalidConfiguration("io_signal_owner_mismatch");
        if (text(row, "value_type") != "bool")
          throw InvalidConfiguration("io_signal_type_mismatch");
        if (output) {
          const auto &provider = value<Object>(member(row, "provider"));
          IoSignalProviderConfiguration address{
              text(provider, "profile"), {}, {}, {}, 0U};
          std::string channel;
          if (address.profile == "ros_set_bool") {
            fields(provider, {"profile", "service"});
            address.service = absolute_address(provider, "service");
            channel = *address.service;
          } else if (address.profile == "mock_output") {
            fields(provider, {"profile", "channel"});
            address.channel = text(provider, "channel");
            channel = *address.channel;
          } else if (address.profile == "ur_set_io") {
            fields(provider, {"profile", "service", "pin"});
            address.service = absolute_address(provider, "service");
            address.pin =
                static_cast<std::size_t>(counter(provider, "pin", 17U));
          } else if (address.profile == "ros2_control_gpio") {
            fields(provider, {"profile", "topic", "group", "interface_name"});
            address.topic = absolute_address(provider, "topic");
            address.group = provider_selector(provider, "group");
            address.interface_name =
                provider_selector(provider, "interface_name");
          } else {
            throw InvalidConfiguration("io_signal_provider_unsupported");
          }
          core::IoSignalDescriptorDTO descriptor{
              {component_id, text(row, "signal_name")},
              core::InterruptSourceId(text(row, "source_id")),
              core::IoSignalValueType::kBoolean,
              core::IoSignalDirection::kOutput,
              core::IoSignalUpdateMode::kChangeOnly,
              core::IoTransitionCapability::kSampledOnly,
              {core::IoFreshnessMode::kProviderValidity, {}},
              (address.profile == "ur_set_io" ||
               address.profile == "ros2_control_gpio")
                  ? typed_output_resource(address)
                  : output_resource(address.profile, channel)};
          if (!core::ComputeIoSignalDescriptorDigest(descriptor).ok())
            throw InvalidConfiguration("io_signal_descriptor_invalid");
          const auto owner = std::tuple{
              unit.runtime_unit_id,  address.profile, address.topic,
              address.ingress_depth, address.service, address.channel,
              descriptor.liveness};
          const auto [source, inserted] =
              sources.emplace(descriptor.source_id, owner);
          if ((!inserted && source->second != owner) ||
              !identities.insert(descriptor.signal_id).second ||
              !roles.emplace(component_id, target_id, role_id).second)
            throw InvalidConfiguration("io_signal_unbound");
          result.push_back({unit.runtime_unit_id, target_id, role_id,
                            std::move(descriptor), std::move(address)});
          continue;
        }
        const auto update = text(row, "update_mode");
        if (update != "sampled" && update != "change_only")
          throw InvalidConfiguration("io_signal_configuration_invalid");
        const auto transition = text(row, "transition_capability");
        if (transition != "sampled_only" && transition != "captured")
          throw InvalidConfiguration("io_signal_transition_unsupported");
        const auto &freshness = value<Object>(member(row, "freshness"));
        const auto freshness_mode = text(freshness, "mode");
        std::optional<core::Duration> maximum_age;
        if (freshness_mode == "sample_age") {
          fields(freshness, {"mode", "maximum_age_ns"});
          const auto age =
              counter(freshness, "maximum_age_ns", 9007199254740991ULL);
          if (age == 0U)
            throw InvalidConfiguration("io_signal_freshness_unsupported");
          maximum_age = static_cast<core::Duration>(age);
        } else if (freshness_mode == "provider_validity") {
          fields(freshness, {"mode"});
        } else {
          throw InvalidConfiguration("io_signal_freshness_unsupported");
        }
        core::IoSignalDescriptorDTO descriptor{
            {component_id, text(row, "signal_name")},
            core::InterruptSourceId(text(row, "source_id")),
            core::IoSignalValueType::kBoolean,
            direction == "input" ? core::IoSignalDirection::kInput
                                 : core::IoSignalDirection::kOutput,
            update == "sampled" ? core::IoSignalUpdateMode::kSampled
                                : core::IoSignalUpdateMode::kChangeOnly,
            transition == "sampled_only"
                ? core::IoTransitionCapability::kSampledOnly
                : core::IoTransitionCapability::kCaptured,
            {freshness_mode == "sample_age"
                 ? core::IoFreshnessMode::kSampleAge
                 : core::IoFreshnessMode::kProviderValidity,
             maximum_age},
            {}};
        const auto &provider = value<Object>(member(row, "provider"));
        IoSignalProviderConfiguration address{
            text(provider, "profile"), {}, {}, {}, 0U};
        if (address.profile == "mock") {
          fields(provider, {"profile", "initial_value", "ingress_depth"});
          descriptor.liveness = core::IoSignalLivenessMode::kInProcess;
          address.initial_value =
              value<bool>(member(provider, "initial_value"));
          if (descriptor.direction != core::IoSignalDirection::kInput ||
              descriptor.freshness.mode !=
                  core::IoFreshnessMode::kProviderValidity ||
              descriptor.update_mode != core::IoSignalUpdateMode::kChangeOnly ||
              descriptor.transition_capability !=
                  core::IoTransitionCapability::kSampledOnly)
            throw InvalidConfiguration("io_signal_provider_unsupported");
        } else if (address.profile == "ros_bool") {
          fields(provider, {"profile", "topic", "ingress_depth", "liveness"});
          address.topic = text(provider, "topic", false);
          const auto &liveness = value<Object>(member(provider, "liveness"));
          fields(liveness, {"mode"});
          const auto mode = text(liveness, "mode");
          if (mode == "periodic")
            descriptor.liveness = core::IoSignalLivenessMode::kPeriodic;
          else if (mode == "change_only")
            descriptor.liveness = core::IoSignalLivenessMode::kChangeOnly;
          else
            throw InvalidConfiguration("io_signal_liveness_unsupported");
        } else if (address.profile == "ur_io_states") {
          fields(provider, {"profile", "topic", "pin", "ingress_depth"});
          address.topic = absolute_address(provider, "topic");
          address.pin = static_cast<std::size_t>(counter(provider, "pin", 17U));
          descriptor.liveness = core::IoSignalLivenessMode::kPeriodic;
        } else if (address.profile == "ros2_control_gpio") {
          fields(provider, {"profile", "topic", "group", "interface_name",
                            "ingress_depth"});
          address.topic = absolute_address(provider, "topic");
          address.group = provider_selector(provider, "group");
          address.interface_name =
              provider_selector(provider, "interface_name");
          descriptor.liveness = core::IoSignalLivenessMode::kPeriodic;
        } else {
          throw InvalidConfiguration("io_signal_provider_unsupported");
        }
        const auto depth = counter(
            provider, "ingress_depth",
            std::min<std::uint64_t>(9007199254740991ULL,
                                    std::numeric_limits<std::size_t>::max()));
        if (depth == 0U)
          throw InvalidConfiguration("io_signal_configuration_invalid");
        address.ingress_depth = static_cast<std::size_t>(depth);
        if (address.profile != "mock" &&
            (descriptor.direction != core::IoSignalDirection::kInput ||
             descriptor.transition_capability !=
                 core::IoTransitionCapability::kSampledOnly ||
             (address.profile != "ros_bool" &&
              descriptor.update_mode != core::IoSignalUpdateMode::kSampled)))
          throw InvalidConfiguration("io_signal_transition_unsupported");
        if (!core::ComputeIoSignalDescriptorDigest(descriptor).ok())
          throw InvalidConfiguration("io_signal_liveness_freshness_mismatch");
        const auto owner =
            std::tuple{unit.runtime_unit_id,  address.profile, address.topic,
                       address.ingress_depth, address.service, address.channel,
                       descriptor.liveness};
        const auto [source, inserted] =
            sources.emplace(descriptor.source_id, owner);
        if ((!inserted && source->second != owner) ||
            !identities.insert(descriptor.signal_id).second ||
            !roles.emplace(component_id, target_id, role_id).second)
          throw InvalidConfiguration("io_signal_unbound");
        result.push_back({unit.runtime_unit_id, target_id, role_id,
                          std::move(descriptor), std::move(address)});
      }
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
      return std::tie(a.descriptor.signal_id.controllable_component_id, a.command_target_id,
                      a.resource_role_id) <
             std::tie(b.descriptor.signal_id.controllable_component_id, b.command_target_id,
                      b.resource_role_id);
    });
    return Reply::Ok(std::move(result));
  } catch (const InvalidConfiguration &error) {
    return Reply::Err({"invalid_request",
                       "Retained I/O signal configuration is invalid",
                       {{"reason", error.what()}}});
  }
}

core::Result<void> MaterializeRetainedIoProviderDevices(
    core::CellGraphDTO &graph,
    const std::optional<core::RuntimeUnitId> &runtime_unit_id) {
  try {
    std::size_t total_devices = 0U;
    for (auto &unit : graph.runtime_units) {
      if (runtime_unit_id && unit.runtime_unit_id != *runtime_unit_id)
        continue;
      std::set<core::ControllableComponentId> generic_devices;
      std::set<core::ControllableComponentId> ros_output_devices;
      const auto signals = unit.configuration.find("io_signals");
      if (signals != unit.configuration.end()) {
        const auto &rows = value<Array>(signals->second);
        if (rows.size() > 4096U)
          throw InvalidConfiguration(
              "io_provider_device_configuration_invalid");
        for (const auto &item : rows) {
          const auto &row = value<Object>(item);
          const auto &provider = value<Object>(member(row, "provider"));
          const auto profile = text(provider, "profile");
          if (profile != "mock" && profile != "ros_bool" &&
              profile != "mock_output" && profile != "ros_set_bool" &&
              profile != "ur_io_states" && profile != "ur_set_io" &&
              profile != "ros2_control_gpio")
            throw InvalidConfiguration("io_signal_provider_unsupported");
          const core::ControllableComponentId component(
              text(row, "controllable_component_id"));
          const auto owner = std::find_if(
              graph.controllable_components.begin(),
              graph.controllable_components.end(), [&](const auto &candidate) {
                return candidate.controllable_component_id == component &&
                       std::find(candidate.runtime_unit_ids.begin(),
                                 candidate.runtime_unit_ids.end(),
                                 unit.runtime_unit_id) !=
                           candidate.runtime_unit_ids.end();
              });
          if (owner == graph.controllable_components.end())
            throw InvalidConfiguration("io_signal_owner_mismatch");
          generic_devices.insert(component);
          if ((profile == "ros_set_bool" || profile == "ur_set_io") &&
              text(row, "direction") == "output")
            ros_output_devices.insert(component);
        }
      }
      if (generic_devices.size() > 4096U - total_devices)
        throw InvalidConfiguration("io_provider_device_configuration_invalid");
      total_devices += generic_devices.size();
      std::map<core::ControllableComponentId, Object> devices;
      const auto declared = unit.configuration.find("io_provider_devices");
      if (declared != unit.configuration.end()) {
        const auto &rows = value<Array>(declared->second);
        if (rows.size() > 4096U)
          throw InvalidConfiguration(
              "io_provider_device_configuration_invalid");
        for (const auto &item : rows) {
          auto row = value<Object>(item);
          fields(
              row, {"controllable_component_id"},
              {"lifecycle_command_timeout_s", "output_source_ack_timeout_s"});
          const core::ControllableComponentId component(
              text(row, "controllable_component_id"));
          if (!generic_devices.contains(component))
            throw InvalidConfiguration(
                "io_provider_device_configuration_invalid");
          if (row.contains("output_source_ack_timeout_s") &&
              !ros_output_devices.contains(component))
            throw InvalidConfiguration(
                "io_provider_device_output_budget_unused");
          row.try_emplace("lifecycle_command_timeout_s",
                          core::JsonValueDTO{30.0});
          if (!devices.emplace(component, std::move(row)).second)
            throw InvalidConfiguration(
                "io_provider_device_configuration_invalid");
        }
      }
      for (const auto &component : generic_devices)
        devices.try_emplace(
            component,
            Object{{"controllable_component_id",
                    core::JsonValueDTO{component.value()}},
                   {"lifecycle_command_timeout_s", core::JsonValueDTO{30.0}}});
      if (!generic_devices.empty() || declared != unit.configuration.end()) {
        Array normalized;
        for (auto &[component, row] : devices) {
          if (ros_output_devices.contains(component))
            row.try_emplace("output_source_ack_timeout_s",
                            core::JsonValueDTO{0.5});
          normalized.push_back(core::JsonValueDTO{std::move(row)});
        }
        unit.configuration["io_provider_devices"] =
            core::JsonValueDTO{std::move(normalized)};
      }
    }
    return core::Result<void>::Ok();
  } catch (const InvalidConfiguration &error) {
    return core::Result<void>::Err({"invalid_request",
                                    "Retained I/O device defaults are invalid",
                                    {{"reason", error.what()}}});
  }
}

core::Result<std::vector<RetainedIoProviderDeviceConfiguration>>
ResolveRetainedIoProviderDevices(
    const core::CellGraphDTO &graph,
    const std::optional<core::RuntimeUnitId> &runtime_unit_id) {
  using Reply =
      core::Result<std::vector<RetainedIoProviderDeviceConfiguration>>;
  const bool has_devices = std::any_of(
      graph.runtime_units.begin(), graph.runtime_units.end(),
      [&](const auto &unit) {
        return (!runtime_unit_id || unit.runtime_unit_id == *runtime_unit_id) &&
               unit.configuration.contains("io_provider_devices");
      });
  if (!has_devices)
    return Reply::Ok({});
  const auto catalog = ResolveRetainedIoSignalCatalog(graph, runtime_unit_id);
  if (!catalog.ok())
    return Reply::Err(catalog.error());
  try {
    std::vector<RetainedIoProviderDeviceConfiguration> result;
    for (const auto &unit : graph.runtime_units) {
      if (runtime_unit_id && unit.runtime_unit_id != *runtime_unit_id)
        continue;
      const auto declared = unit.configuration.find("io_provider_devices");
      if (declared == unit.configuration.end())
        continue;
      const auto &rows = value<Array>(declared->second);
      if (rows.size() > 4096U - result.size())
        throw InvalidConfiguration("io_provider_device_configuration_invalid");
      std::set<core::ControllableComponentId> components;
      for (const auto &item : rows) {
        const auto &row = value<Object>(item);
        fields(row,
               {"controllable_component_id", "lifecycle_command_timeout_s"},
               {"output_source_ack_timeout_s"});
        const core::ControllableComponentId component(
            text(row, "controllable_component_id"));
        const bool configured = std::any_of(
            catalog.value().begin(), catalog.value().end(),
            [&](const auto &signal) {
              return signal.runtime_unit_id == unit.runtime_unit_id &&
                     signal.descriptor.signal_id.controllable_component_id == component;
            });
        if (!configured || !components.insert(component).second)
          throw InvalidConfiguration(
              "io_provider_device_configuration_invalid");
        const auto seconds =
            value<double>(member(row, "lifecycle_command_timeout_s"));
        if (!std::isfinite(seconds) || seconds <= 0.0)
          throw InvalidConfiguration(
              "io_provider_device_configuration_invalid");
        const auto ns =
            std::ceil(static_cast<long double>(seconds) * 1'000'000'000.0L);
        if (ns < 1.0L || ns > core::kMaxLifecycleTransactionTimeoutNs)
          throw InvalidConfiguration(
              "io_provider_device_configuration_invalid");
        const bool ros_output = std::any_of(
            catalog.value().begin(), catalog.value().end(),
            [&](const auto &signal) {
              return signal.runtime_unit_id == unit.runtime_unit_id &&
                     signal.descriptor.signal_id.controllable_component_id == component &&
                     signal.descriptor.direction ==
                         core::IoSignalDirection::kOutput &&
                     (signal.provider.profile == "ros_set_bool" ||
                      signal.provider.profile == "ur_set_io");
            });
        const auto output_budget = row.find("output_source_ack_timeout_s");
        if (ros_output != (output_budget != row.end()))
          throw InvalidConfiguration(
              "io_provider_device_output_budget_missing_or_unused");
        std::optional<core::Duration> output_budget_ns;
        if (output_budget != row.end()) {
          const auto output_seconds = value<double>(output_budget->second);
          if (!std::isfinite(output_seconds) || output_seconds <= 0.0)
            throw InvalidConfiguration(
                "io_provider_device_output_budget_invalid");
          const auto output_ns = std::ceil(
              static_cast<long double>(output_seconds) * 1'000'000'000.0L);
          if (output_ns < 1.0L ||
              output_ns > static_cast<long double>(
                              std::numeric_limits<core::Duration>::max()))
            throw InvalidConfiguration(
                "io_provider_device_output_budget_invalid");
          output_budget_ns = static_cast<core::Duration>(output_ns);
        }
        result.push_back({unit.runtime_unit_id, component,
                          static_cast<core::Duration>(ns), output_budget_ns});
      }
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
      return std::tie(a.runtime_unit_id, a.controllable_component_id) <
             std::tie(b.runtime_unit_id, b.controllable_component_id);
    });
    return Reply::Ok(std::move(result));
  } catch (const InvalidConfiguration &error) {
    return Reply::Err({"invalid_request",
                       "Retained I/O device configuration is invalid",
                       {{"reason", error.what()}}});
  }
}

core::Result<void> ValidateNonCoordinateProviderTarget(
    const core::CellGraphDTO &graph,
    const core::CapabilityProviderBindingDTO &provider) {
  const auto invalid = [] {
    return core::Result<void>::Err(
        {"invalid_request",
         "Non-coordinate provider target is not bound to its sealed signal "
         "owners",
         {{"reason", "io_signal_provider_target_mismatch"}}});
  };
  const auto *target =
      std::get_if<core::ComponentTargetCapabilityProviderTargetDTO>(
          &provider.target);
  if (!target || provider.scope != core::CapabilityScope::kTargetBinding ||
      provider.endpoint_bindings.empty())
    return invalid();
  const auto component =
      std::find_if(graph.controllable_components.begin(),
                   graph.controllable_components.end(), [&](const auto &row) {
                     return row.controllable_component_id ==
                            target->controllable_component_id;
                   });
  if (component == graph.controllable_components.end() ||
      component->component_type_revision != target->component_type_revision)
    return invalid();
  const auto catalog = ResolveRetainedIoSignalCatalog(graph);
  if (!catalog.ok())
    return core::Result<void>::Err(catalog.error());
  std::set<core::RuntimeUnitId> owners;
  const auto capability = provider.capability_id.value();
  const auto direction = capability == "io.gpio.set_output"
                             ? core::IoSignalDirection::kOutput
                             : core::IoSignalDirection::kInput;
  if (capability != "io.gpio.set_output" && capability != "io.gpio.get_input" &&
      capability != "io.gpio.wait_for_input")
    return invalid();
  for (const auto &signal : catalog.value()) {
    if (signal.descriptor.signal_id.controllable_component_id ==
            target->controllable_component_id &&
        signal.command_target_id == target->command_target_id &&
        signal.descriptor.direction == direction)
      owners.insert(signal.runtime_unit_id);
  }
  if (owners.empty())
    return invalid();
  std::set<core::RuntimeUnitId> sources;
  for (const auto &endpoint : provider.endpoint_bindings) {
    const auto *unit = std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
    if (endpoint.source_kind !=
            core::CapabilityEndpointSourceKind::kRuntimeUnit ||
        !unit || !owners.contains(*unit))
      return invalid();
    sources.insert(*unit);
  }
  return sources == owners ? core::Result<void>::Ok() : invalid();
}

} // namespace manyforge::deployment
