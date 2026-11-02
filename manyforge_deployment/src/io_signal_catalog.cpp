#include <manyforge_deployment/io_signal_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <variant>

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

// An identity token: 1-128 bytes of `[A-Za-z0-9._-]`, starting alphanumeric.
bool identity_token(std::string_view token) {
  const auto alpha_numeric = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
  };
  return !token.empty() && token.size() <= 128U &&
         alpha_numeric(token.front()) &&
         std::all_of(token.begin(), token.end(), [&](unsigned char c) {
           return alpha_numeric(c) || c == '.' || c == '_' || c == '-';
         });
}

std::string text(const Object &object, std::string_view key,
                 bool identity = true) {
  const auto &result = value<std::string>(member(object, key));
  if (result.empty() || result.size() > (identity ? 128U : 4096U) ||
      result.find('\0') != std::string::npos ||
      (identity && !identity_token(result)))
    throw InvalidConfiguration("io_signal_configuration_invalid");
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

content_identity::CanonicalValue canonical(const core::JsonValueDTO &input) {
  using content_identity::CanonicalValue;
  return std::visit(
      [](const auto &typed) -> CanonicalValue {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, Array>) {
          CanonicalValue::Array rows;
          for (const auto &row : typed) rows.push_back(canonical(row));
          return CanonicalValue(std::move(rows));
        } else if constexpr (std::is_same_v<T, Object>) {
          CanonicalValue::Object rows;
          for (const auto &[name, row] : typed) rows.emplace_back(name, canonical(row));
          return CanonicalValue(std::move(rows));
        } else {
          return CanonicalValue(typed);
        }
      },
      input.value);
}

core::ResourceId
typed_output_resource(const IoSignalProviderConfiguration &address) {
  using content_identity::CanonicalValue;
  CanonicalValue::Object key{{"profile", CanonicalValue(address.profile)}};
  if (address.profile == "ros2_control_gpio") {
    key.emplace_back("topic", CanonicalValue(*address.topic));
    key.emplace_back("group", CanonicalValue(*address.group));
    key.emplace_back("interface_name", CanonicalValue(*address.interface_name));
  } else {
    // An adapter output: its whole provider object is its channel.
    key.emplace_back("adapter_fields",
                     canonical(core::JsonValueDTO{address.adapter_fields}));
  }
  const auto digest = content_identity::framed_sha256(
      "io-output-resource", "manyforge.io_output_channel.v1",
      CanonicalValue(std::move(key)));
  return core::ResourceId("io.output." + digest.substr(7U));
}

// An adapter profile's provider object without its `profile` (and, for an
// input, its generic `ingress_depth`): opaque here, the adapter validates it.
Object adapter_fields(const Object &provider) {
  Object result = provider;
  result.erase("profile");
  result.erase("ingress_depth");
  return result;
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
             std::tuple<core::RuntimeUnitId,
                        std::string, std::optional<std::string>, std::size_t,
                        std::optional<std::string>,
                        std::optional<std::string>,
                        std::optional<core::IoSignalLivenessMode>,
                        std::optional<core::SceneResourceId>,
                        std::optional<core::ControllableComponentId>>>
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
                       "value_type", "direction", "provider"},
                 {"controller_owned"});
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
        // `<output>.changed_outside` names the core-held signal (CONTRACTS §7).
        if (text(row, "signal_name").ends_with(core::kChangedOutsideSignalSuffix))
          throw InvalidConfiguration("io_system_signal_conflict");
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
          } else if (address.profile == "ros2_control_gpio") {
            fields(provider, {"profile", "topic", "group", "interface_name"});
            address.topic = absolute_address(provider, "topic");
            address.group = provider_selector(provider, "group");
            address.interface_name =
                provider_selector(provider, "interface_name");
          } else if (!IsGenericIoProviderProfile(address.profile)) {
            // An output has no ingress queue.
            if (provider.contains("ingress_depth"))
              throw InvalidConfiguration("io_signal_configuration_invalid");
            address.adapter_fields = adapter_fields(provider);
          } else {
            throw InvalidConfiguration("io_signal_provider_unsupported");
          }
          // A machine's I/O bank output (CONTRACTS §16): one channel per
          // signal of its component.
          const bool component_adapter =
              address.profile == kComponentAdapterIoProfile;
          if (component_adapter)
            channel = component_id.value() + "/" + text(row, "signal_name");
          core::IoSignalDescriptorDTO descriptor{
              {component_id, text(row, "signal_name")},
              core::InterruptSourceId(text(row, "source_id")),
              core::IoSignalValueType::kBoolean,
              core::IoSignalDirection::kOutput,
              core::IoSignalUpdateMode::kChangeOnly,
              core::IoTransitionCapability::kSampledOnly,
              {core::IoFreshnessMode::kProviderValidity, {}},
              (address.profile == "ros2_control_gpio" ||
               (!IsGenericIoProviderProfile(address.profile) && !component_adapter))
                  ? typed_output_resource(address)
                  : output_resource(address.profile, channel)};
          // CONTRACTS §7: `controller_owned` authored, default false.
          // Read-back: a machine's I/O bank reports its outputs, and Mock
          // outputs report the written value back (D2); ros_set_bool and
          // ros2_control_gpio outputs have none.
          descriptor.readback =
              component_adapter || address.profile == "mock_output";
          if (const auto owned = row.find("controller_owned"); owned != row.end())
            descriptor.controller_owned = value<bool>(owned->second);
          if (!core::ComputeIoSignalDescriptorDigest(descriptor).ok())
            throw InvalidConfiguration("io_signal_descriptor_invalid");
          const auto owner = std::tuple{
              unit.runtime_unit_id,  address.profile, address.topic,
              address.ingress_depth, address.service, address.channel,
              descriptor.liveness,
              address.target_scene_resource_id, address.robot_component_id};
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
          fields(provider, {"profile", "initial_value", "ingress_depth"},
                 {"echo"});
          descriptor.liveness = core::IoSignalLivenessMode::kInProcess;
          address.initial_value =
              value<bool>(member(provider, "initial_value"));
          if (const auto echo = provider.find("echo"); echo != provider.end()) {
            const auto &rule = value<Object>(echo->second);
            fields(rule, {"output_signal_name", "delay_ms"});
            address.echo_output_signal_name = text(rule, "output_signal_name");
            address.echo_delay_ns = static_cast<core::Duration>(
                counter(rule, "delay_ms", 9'007'199'254ULL) * 1'000'000ULL);
          }
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
        } else if (address.profile == "ros2_control_gpio") {
          fields(provider, {"profile", "topic", "group", "interface_name",
                            "ingress_depth"});
          address.topic = absolute_address(provider, "topic");
          address.group = provider_selector(provider, "group");
          address.interface_name =
              provider_selector(provider, "interface_name");
          descriptor.liveness = core::IoSignalLivenessMode::kPeriodic;
        } else if (address.profile == "geometric_contact") {
          fields(provider,
                 {"profile", "ingress_depth", "robot_component_id",
                  "target_scene_resource_id"});
          address.robot_component_id = core::ControllableComponentId(
              text(provider, "robot_component_id"));
          address.target_scene_resource_id = core::SceneResourceId(
              provider_selector(provider, "target_scene_resource_id"));
          if (freshness_mode != "sample_age")
            throw InvalidConfiguration("io_signal_provider_unsupported");
          descriptor.liveness = core::IoSignalLivenessMode::kPeriodic;
        } else if (!IsGenericIoProviderProfile(address.profile)) {
          // An adapter input, a machine's I/O bank included: sampled
          // periodically by its adapter (the xArm `xarm_io_states` stream).
          address.adapter_fields = adapter_fields(provider);
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
        if (address.profile == "geometric_contact" &&
            (descriptor.freshness.mode !=
                 core::IoFreshnessMode::kSampleAge ||
             descriptor.update_mode != core::IoSignalUpdateMode::kSampled))
          throw InvalidConfiguration("io_signal_provider_unsupported");
        if (!core::ComputeIoSignalDescriptorDigest(descriptor).ok())
          throw InvalidConfiguration("io_signal_liveness_freshness_mismatch");
        const auto owner = std::tuple{
            unit.runtime_unit_id,  address.profile, address.topic,
            address.ingress_depth, address.service, address.channel,
            descriptor.liveness,
            address.target_scene_resource_id, address.robot_component_id};
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
    for (const auto &row : result) {
      if (!row.provider.echo_output_signal_name)
        continue;
      const core::IoSignalId output{
          row.descriptor.signal_id.controllable_component_id,
          *row.provider.echo_output_signal_name};
      const auto found = std::find_if(result.begin(), result.end(),
          [&](const auto &candidate) {
            return candidate.descriptor.signal_id == output &&
                   candidate.descriptor.direction == core::IoSignalDirection::kOutput &&
                   candidate.provider.profile == "mock_output";
          });
      if (found == result.end())
        throw InvalidConfiguration("io_mock_echo_output_not_found");
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

bool IsGenericIoProviderProfile(const std::string_view profile) noexcept {
  return profile == "mock" || profile == "mock_output" ||
         profile == "ros_bool" || profile == "ros_set_bool" ||
         profile == "ros2_control_gpio" || profile == "geometric_contact" ||
         profile == kSystemCarriedObjectProfile;
}

bool IsAcknowledgedIoOutputProfile(const std::string_view profile) noexcept {
  return profile == "ros_set_bool" ||
         (!IsGenericIoProviderProfile(profile) &&
          profile != kComponentAdapterIoProfile);
}

std::string CarriedObjectLostSignalName(const std::string_view command_target_id) {
  return std::string(kCarriedObjectLostSignalPrefix) +
         std::string(command_target_id);
}

const RetainedIoSignalConfiguration *FindChangedOutsideOutput(
    std::span<const RetainedIoSignalConfiguration> signals,
    const core::ControllableComponentId &component,
    std::string_view signal_name) {
  if (!signal_name.ends_with(core::kChangedOutsideSignalSuffix))
    return nullptr;
  signal_name.remove_suffix(core::kChangedOutsideSignalSuffix.size());
  const auto found =
      std::find_if(signals.begin(), signals.end(), [&](const auto &row) {
        return row.descriptor.direction == core::IoSignalDirection::kOutput &&
               row.descriptor.signal_id.controllable_component_id == component &&
               row.descriptor.signal_id.name == signal_name;
      });
  return found == signals.end() ? nullptr : &*found;
}

core::Result<std::vector<RetainedIoSignalConfiguration>>
ResolveCompiledIoSignalCatalog(const core::CellGraphDTO &graph) {
  using Reply = core::Result<std::vector<RetainedIoSignalConfiguration>>;
  auto authored = ResolveRetainedIoSignalCatalog(graph);
  if (!authored.ok())
    return authored;
  auto result = std::move(authored.value());
  // Only authored rows can collide with a system row; the generated rows
  // share the reserved source by design (SX-205).
  const auto authored_count = static_cast<std::ptrdiff_t>(result.size());
  std::set<std::pair<core::ControllableComponentId, core::CommandTargetId>>
      grippers;
  for (const auto &binding : graph.control_bindings) {
    if (binding.capability_id.value() != kGripperCommandCapabilityId ||
        !grippers.emplace(binding.controllable_component_id,
                          binding.command_target_id)
             .second)
      continue;
    core::IoSignalDescriptorDTO descriptor{
        {binding.controllable_component_id,
         CarriedObjectLostSignalName(binding.command_target_id.value())},
        core::InterruptSourceId(std::string(kCarriedObjectLostSourceId)),
        core::IoSignalValueType::kBoolean,
        core::IoSignalDirection::kInput,
        core::IoSignalUpdateMode::kChangeOnly,
        core::IoTransitionCapability::kSampledOnly,
        {core::IoFreshnessMode::kProviderValidity, {}},
        {}};
    descriptor.liveness = core::IoSignalLivenessMode::kInProcess;
    const bool conflict = std::any_of(
        result.begin(), result.begin() + authored_count, [&](const auto &row) {
          return row.descriptor.signal_id == descriptor.signal_id ||
                 row.descriptor.source_id == descriptor.source_id ||
                 (row.descriptor.signal_id.controllable_component_id ==
                      binding.controllable_component_id &&
                  row.command_target_id == binding.command_target_id &&
                  row.resource_role_id.value() ==
                      kCarriedObjectLostResourceRole);
        });
    if (conflict || !core::ComputeIoSignalDescriptorDigest(descriptor).ok())
      return Reply::Err({"invalid_request",
                         "Retained I/O signal configuration is invalid",
                         {{"reason", "io_system_signal_conflict"}}});
    result.push_back(
        {binding.provider_runtime_unit_id, binding.command_target_id,
         core::ResourceRoleId(std::string(kCarriedObjectLostResourceRole)),
         std::move(descriptor),
         IoSignalProviderConfiguration{
             std::string(kSystemCarriedObjectProfile), {}, {}, {},
             kCarriedObjectLostIngressDepth}});
  }
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return std::tie(a.descriptor.signal_id.controllable_component_id,
                    a.command_target_id, a.resource_role_id) <
           std::tie(b.descriptor.signal_id.controllable_component_id,
                    b.command_target_id, b.resource_role_id);
  });
  return Reply::Ok(std::move(result));
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
          if (IsAcknowledgedIoOutputProfile(profile) &&
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
              {"lifecycle_command_timeout_s", "member_role",
               "output_source_ack_timeout_s", "owner_component_id"});
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
                          core::JsonValueDTO{core::kDefaultLifecycleCommandTimeoutS});
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
                   {"lifecycle_command_timeout_s", core::JsonValueDTO{core::kDefaultLifecycleCommandTimeoutS}}});
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
               {"member_role", "output_source_ack_timeout_s",
                "owner_component_id"});
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
                     IsAcknowledgedIoOutputProfile(signal.provider.profile);
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
        // CONTRACTS §16: a machine member's owner and role.
        const auto optional_text = [&](std::string_view key) {
          return row.contains(key) ? std::optional(text(row, key)) : std::nullopt;
        };
        result.push_back({unit.runtime_unit_id, component,
                          static_cast<core::Duration>(ns), output_budget_ns,
                          optional_text("owner_component_id"),
                          optional_text("member_role")});
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

std::string PoseSourceStreamId(std::string_view object_id,
                               std::string_view policy_id) {
  if (policy_id == "scene")
    return "poses/" + std::string(object_id);
  return "poses/" + std::string(object_id) + "/" + std::string(policy_id);
}

core::Result<std::vector<RetainedPoseSourceConfiguration>>
ResolveRetainedPoseSources(
    const core::CellGraphDTO &graph,
    const std::optional<core::RuntimeUnitId> &runtime_unit_id) {
  using Reply = core::Result<std::vector<RetainedPoseSourceConfiguration>>;
  const auto invalid = [](const char *reason) {
    return InvalidConfiguration(reason);
  };
  const auto binding_id = [&](const Object &binding, std::string_view key) {
    const auto *id = std::get_if<std::string>(&member(binding, key).value);
    if (!id || id->empty() ||
        !std::all_of(id->begin(), id->end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '.' || c == '-';
        }))
      throw invalid("pose_source_binding_id_invalid");
    return *id;
  };
  try {
    std::vector<RetainedPoseSourceConfiguration> result;
    for (const auto &unit : graph.runtime_units) {
      if (runtime_unit_id && unit.runtime_unit_id != *runtime_unit_id)
        continue;
      const auto declared = unit.configuration.find("pose_sources");
      if (declared == unit.configuration.end())
        continue;
      for (const auto &item : value<Array>(declared->second)) {
        const auto &row = value<Object>(item);
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
          throw invalid("pose_source_owner_mismatch");
        std::vector<const Object *> bindings;
        if (const auto joint = row.find("joint_state"); joint != row.end()) {
          const auto &joint_state = value<Object>(joint->second);
          std::set<std::string> names;
          for (const auto &name :
               value<Array>(member(joint_state, "required_joint_names")))
            if (!names.insert(value<std::string>(name)).second)
              throw invalid("pose_source_joint_names_duplicate");
          bindings.push_back(&joint_state);
        }
        for (const auto key : {"simulation_entities", "detection3d_sources"})
          for (const auto &binding : value<Array>(member(row, key)))
            bindings.push_back(&value<Object>(binding));
        if (bindings.empty())
          throw invalid("pose_source_bindings_empty");
        std::vector<std::string> streams;
        for (const auto *binding : bindings) {
          auto stream = PoseSourceStreamId(binding_id(*binding, "object_id"),
                                           binding_id(*binding, "policy_id"));
          if (stream.size() > 64U)
            throw invalid("pose_source_binding_id_invalid");
          streams.push_back(std::move(stream));
        }
        std::sort(streams.begin(), streams.end());
        if (std::adjacent_find(streams.begin(), streams.end()) != streams.end())
          throw invalid("pose_source_binding_duplicate");
        result.push_back({unit.runtime_unit_id, component, std::move(streams),
                          item});
      }
    }
    return Reply::Ok(std::move(result));
  } catch (const InvalidConfiguration &error) {
    return Reply::Err({"invalid_request",
                       "Retained pose-source configuration is invalid",
                       {{"reason", error.what()}}});
  }
}

core::Result<std::vector<RetainedAiModelConfiguration>>
ResolveRetainedAiModels(
    const core::CellGraphDTO &graph,
    const std::optional<core::RuntimeUnitId> &runtime_unit_id) {
  using Reply = core::Result<std::vector<RetainedAiModelConfiguration>>;
  try {
    std::vector<RetainedAiModelConfiguration> result;
    for (const auto &unit : graph.runtime_units) {
      if (runtime_unit_id && unit.runtime_unit_id != *runtime_unit_id)
        continue;
      const auto declared = unit.configuration.find("ai_models");
      if (declared == unit.configuration.end())
        continue;
      for (const auto &item : value<Array>(declared->second)) {
        const auto &row = value<Object>(item);
        const core::ControllableComponentId component(
            text(row, "controllable_component_id"));
        if (std::none_of(
                graph.controllable_components.begin(),
                graph.controllable_components.end(), [&](const auto &owner) {
                  return owner.controllable_component_id == component &&
                         std::find(owner.runtime_unit_ids.begin(),
                                   owner.runtime_unit_ids.end(),
                                   unit.runtime_unit_id) !=
                             owner.runtime_unit_ids.end();
                }))
          throw InvalidConfiguration("ai_model_owner_mismatch");
        const auto api_key_env = row.find("api_key_env");
        result.push_back(
            {unit.runtime_unit_id, component,
             value<std::string>(member(row, "base_url")),
             value<std::string>(member(row, "model")),
             api_key_env == row.end()
                 ? std::nullopt
                 : std::optional<std::string>(
                       value<std::string>(api_key_env->second)),
             item});
      }
    }
    return Reply::Ok(std::move(result));
  } catch (const InvalidConfiguration &error) {
    const std::string reason = error.what();
    return Reply::Err({"invalid_request",
                       "Retained AI model configuration is invalid",
                       {{"reason", reason == "io_signal_configuration_invalid"
                                       ? std::string("ai_model_configuration_invalid")
                                       : reason}}});
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
  if (!target || provider.scope != core::CapabilityScope::kTargetBinding)
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
  if (provider.capability_id.value() == "scene.pose.observe") {
    const auto sources = ResolveRetainedPoseSources(graph);
    if (!sources.ok())
      return core::Result<void>::Err(sources.error());
    const bool configured = std::any_of(
        sources.value().begin(), sources.value().end(), [&](const auto &row) {
          return row.controllable_component_id ==
                 target->controllable_component_id;
        });
    return configured && target->command_target_id.value() == "poses" &&
                   provider.endpoint_bindings.empty()
               ? core::Result<void>::Ok()
               : invalid();
  }
  if (provider.capability_id.value() == kAiModelCapabilityId) {
    const auto models = ResolveRetainedAiModels(graph);
    if (!models.ok())
      return core::Result<void>::Err(models.error());
    const bool configured = std::any_of(
        models.value().begin(), models.value().end(), [&](const auto &row) {
          return row.controllable_component_id ==
                 target->controllable_component_id;
        });
    return configured &&
                   target->command_target_id.value() == kAiModelTargetId &&
                   provider.endpoint_bindings.empty()
               ? core::Result<void>::Ok()
               : invalid();
  }
  if (provider.endpoint_bindings.empty())
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
