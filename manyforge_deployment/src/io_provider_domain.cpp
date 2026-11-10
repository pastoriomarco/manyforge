#include <manyforge_deployment/io_provider_domain.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>

#include "content_identity_internal.hpp"
#include <algorithm>
#include <manyforge_content_identity/content_identity.hpp>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace manyforge::deployment {
namespace {

using Value = CanonicalValue;
using Object = Value::Object;
using Array = Value::Array;

[[noreturn]] void fail(const char *reason) {
  throw std::invalid_argument(reason);
}

Value json_value(const core::JsonValueDTO &value) {
  return std::visit(
      [](const auto &item) -> Value {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, core::JsonValueDTO::Array>) {
          Array result;
          for (const auto &row : item)
            result.push_back(json_value(row));
          return Value(std::move(result));
        } else if constexpr (std::is_same_v<T, core::JsonValueDTO::Object>) {
          Object result;
          for (const auto &[key, row] : item)
            result.emplace_back(key, json_value(row));
          return Value(std::move(result));
        } else {
          return Value(item);
        }
      },
      value.value);
}

const core::RuntimeUnitDTO &unit_for(const core::CellGraphDTO &graph,
                                     const core::RuntimeUnitId &id) {
  const auto found = std::find_if(
      graph.runtime_units.begin(), graph.runtime_units.end(),
      [&](const auto &unit) { return unit.runtime_unit_id == id; });
  if (found == graph.runtime_units.end())
    fail("io_provider_runtime_missing");
  return *found;
}

Array member_rows(const core::CellGraphDTO &graph,
                  const core::RuntimeUnitId &unit,
                  const std::set<std::string> &names,
                  const core::ControllableComponentId &component) {
  const auto &config = unit_for(graph, unit).configuration;
  const auto rows = config.find("io_signals");
  if (rows == config.end())
    fail("io_provider_signal_membership_missing");
  const auto *array =
      std::get_if<core::JsonValueDTO::Array>(&rows->second.value);
  if (!array)
    fail("io_provider_signal_membership_invalid");
  Array selected;
  for (const auto &row : *array) {
    const auto value = json_value(row);
    const auto *owner = value.find_object_member("controllable_component_id");
    const auto *name = value.find_object_member("signal_name");
    if (owner && name && owner->as_string() == component.value() &&
        names.contains(name->as_string()))
      selected.push_back(value);
  }
  if (selected.size() != names.size())
    fail("io_provider_signal_membership_invalid");
  std::sort(selected.begin(), selected.end(), [](const auto &a, const auto &b) {
    return a.find_object_member("signal_name")->as_string() <
           b.find_object_member("signal_name")->as_string();
  });
  return selected;
}

/// Pose sources and AI models have no device-configured lifecycle limit
/// field: the default component lifecycle budget.
constexpr core::Duration kModelFreeLifecycleCommandTimeoutNs = kDefaultLifecycleBudgetNs;

/// The core provider domain still carries a lifecycle plan catalog digest;
/// with no plan catalog, the provider's manifest revision fills it.
core::ContentDigest lifecycle_digest(const core::ProviderManifestRevision &revision) {
  return core::ContentDigest(revision.value());
}

struct Owner final {
  core::ProviderInstanceId instance;
  core::ProviderManifestId manifest;
  core::ProviderManifestRevision revision;
  core::EndpointRoleId endpoint;
  friend bool operator==(const Owner &, const Owner &) = default;
};

Owner signal_owner(const core::CellGraphDTO &graph,
                   const RetainedIoSignalConfiguration &signal,
                   std::span<const core::CapabilityProviderBindingDTO> bindings,
                   std::span<const QualifiedRuntimeProfile> profiles,
                   std::span<const QualifiedProviderManifest> manifests,
                   bool structural_only) {
  std::optional<Owner> owner;
  for (const auto &binding : bindings) {
    const auto *target =
        std::get_if<core::ComponentTargetCapabilityProviderTargetDTO>(
            &binding.target);
    if (!target ||
        target->controllable_component_id !=
            signal.descriptor.signal_id.controllable_component_id ||
        target->command_target_id != signal.command_target_id)
      continue;
    const bool output =
        signal.descriptor.direction == core::IoSignalDirection::kOutput;
    if (output ? binding.capability_id.value() != "io.gpio.set_output"
               : (binding.capability_id.value() != "io.gpio.get_input" &&
                  binding.capability_id.value() != "io.gpio.wait_for_input"))
      continue;
    if (!ValidateNonCoordinateProviderTarget(graph, binding).ok())
      fail("io_provider_target_unqualified");
    const auto manifest = std::find_if(
        manifests.begin(), manifests.end(), [&](const auto &value) {
          return value.value().provider_manifest_id ==
                     binding.provider_manifest_id &&
                 value.value().provider_manifest_revision ==
                     binding.provider_manifest_revision;
        });
    if (!structural_only &&
        (manifest == manifests.end() ||
         !std::any_of(manifest->value().offered_capabilities.begin(),
                      manifest->value().offered_capabilities.end(),
                      [&](const auto &offer) {
                        return offer.capability_id == binding.capability_id &&
                               offer.capability_definition_revision ==
                                   binding.capability_definition_revision &&
                               std::binary_search(
                                   offer.required_endpoint_role_ids.begin(),
                                   offer.required_endpoint_role_ids.end(),
                                   core::EndpointRoleId("io_signal_service"));
                      })))
      fail("io_provider_manifest_offer_mismatch");
    if (structural_only && binding.provider_manifest_id.value() !=
                               kIoProviderManifestId)
      fail("io_provider_manifest_offer_mismatch");
    for (const auto &endpoint : binding.endpoint_bindings) {
      if (endpoint.endpoint_role_id.value() != "io_signal_service")
        continue;
      const auto *unit = std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
      if (endpoint.source_kind !=
              core::CapabilityEndpointSourceKind::kRuntimeUnit ||
          !unit || *unit != signal.runtime_unit_id)
        fail("io_provider_endpoint_owner_mismatch");
      const auto &runtime = unit_for(graph, *unit);
      const auto profile = std::find_if(
          profiles.begin(), profiles.end(), [&](const auto &value) {
            return value.value().runtime_profile_id ==
                       runtime.runtime_profile_id &&
                   value.value().runtime_profile_revision ==
                       runtime.runtime_profile_revision;
          });
      if (profile == profiles.end() ||
          !std::binary_search(profile->value().source_endpoint_role_ids.begin(),
                              profile->value().source_endpoint_role_ids.end(),
                              endpoint.source_endpoint_role_id))
        fail("io_provider_endpoint_unqualified");
      Owner candidate{
          binding.provider_instance_id, binding.provider_manifest_id,
          binding.provider_manifest_revision, endpoint.source_endpoint_role_id};
      if (owner && *owner != candidate)
        fail("io_provider_ownership_ambiguous");
      owner = std::move(candidate);
    }
  }
  if (!owner)
    fail("io_provider_ownership_missing");
  return *owner;
}

} // namespace

CanonicalValue IoProviderDomainRecord(const core::ControlDomainDTO &domain) {
  const auto *provider = domain.provider();
  if (!provider)
    fail("io_provider_domain_kind_invalid");
  Array units;
  for (const auto &unit : domain.runtime_unit_ids)
    units.emplace_back(unit.value());
  Array names;
  for (const auto &name : provider->signal_names)
    names.emplace_back(name);
  return Value(Object{
      {"control_domain_id", Value(domain.control_domain_id.value())},
      {"control_domain_revision",
       Value(domain.control_domain_revision.value())},
      {"runtime_unit_ids", Value(std::move(units))},
      {"control_coordinator_endpoint_role",
       Value(domain.control_coordinator_endpoint_role.value())},
      {"containment_granularity",
       Value(std::string(core::to_string(domain.containment_granularity)))},
      {"kind", Value("provider")},
      {"provider",
       Value(Object{{"controllable_component_id",
                     Value(provider->controllable_component_id.value())},
                    {"provider_runtime_unit_id",
                     Value(provider->provider_runtime_unit_id.value())},
                    {"provider_instance_id",
                     Value(provider->provider_instance_id.value())},
                    {"provider_manifest_id",
                     Value(provider->provider_manifest_id.value())},
                    {"provider_manifest_revision",
                     Value(provider->provider_manifest_revision.value())},
                    {"lifecycle_plan_catalog_digest",
                     Value(provider->lifecycle_plan_catalog_digest.value())},
                    {"signal_configuration_digest",
                     Value(provider->signal_configuration_digest.value())},
                    {"signal_names", Value(std::move(names))},
                    {"lifecycle_command_timeout_ns",
                     Value(static_cast<double>(
                         provider->lifecycle_command_timeout_ns))}})}});
}

namespace {

/// A model-free provider family with one provider domain per component:
/// Pose sources and AI models. Members are stream ids.
struct ModelFreeDomainKind final {
  std::string_view capability_id;
  std::string_view manifest_id;
  /// Typed refusal prefix and domain id prefix.
  std::string_view kind;
  std::string_view configuration_domain;
  std::string_view configuration_api;
};
constexpr ModelFreeDomainKind kPoseSourceDomainKind{
    "scene.pose.observe", kPoseSourceProviderManifestId, "pose_source",
    "pose-source-configuration", "manyforge.pose_source_configuration.v1"};
constexpr ModelFreeDomainKind kAiModelDomainKind{
    kAiModelCapabilityId, kAiModelProviderManifestId, "ai_model",
    "ai-model-configuration", "manyforge.ai_model_configuration.v1"};

[[noreturn]] void refuse(const ModelFreeDomainKind &kind,
                         std::string_view reason) {
  throw std::invalid_argument(std::string(kind.kind) + std::string(reason));
}

struct ModelFreeSource final {
  core::RuntimeUnitId runtime_unit_id;
  core::ControllableComponentId controllable_component_id;
  std::vector<std::string> stream_ids;
  core::JsonValueDTO row;
};

} // namespace

// One provider domain per component, owned through its target binding; the
// configuration digest covers the whole unit row.
static void derive_model_free_domains(
    const core::CellGraphDTO &graph,
    std::span<const core::CapabilityProviderBindingDTO> bindings,
    std::span<const QualifiedProviderManifest> manifests, bool structural_only,
    const ModelFreeDomainKind &kind, const std::vector<ModelFreeSource> &sources,
    IoProviderDomainDerivation &result) {
  for (const auto &source : sources) {
    std::optional<Owner> owner;
    for (const auto &binding : bindings) {
      const auto *target =
          std::get_if<core::ComponentTargetCapabilityProviderTargetDTO>(
              &binding.target);
      if (!target ||
          target->controllable_component_id !=
              source.controllable_component_id ||
          binding.capability_id.value() != kind.capability_id)
        continue;
      if (!ValidateNonCoordinateProviderTarget(graph, binding).ok())
        refuse(kind, "_target_unqualified");
      const auto manifest = std::find_if(
          manifests.begin(), manifests.end(), [&](const auto &value) {
            return value.value().provider_manifest_id ==
                       binding.provider_manifest_id &&
                   value.value().provider_manifest_revision ==
                       binding.provider_manifest_revision;
          });
      if (binding.provider_manifest_id.value() != kind.manifest_id ||
          (!structural_only &&
           (manifest == manifests.end() ||
            !std::any_of(manifest->value().offered_capabilities.begin(),
                         manifest->value().offered_capabilities.end(),
                         [&](const auto &offer) {
                           return offer.capability_id ==
                                      binding.capability_id &&
                                  offer.capability_definition_revision ==
                                      binding.capability_definition_revision;
                         }))))
        refuse(kind, "_manifest_offer_mismatch");
      Owner candidate{binding.provider_instance_id, binding.provider_manifest_id,
                      binding.provider_manifest_revision,
                      core::EndpointRoleId("ros2_control_execution_status")};
      if (owner && *owner != candidate)
        refuse(kind, "_ownership_ambiguous");
      owner = std::move(candidate);
    }
    if (!owner)
      refuse(kind, "_ownership_missing");
    const auto identity = content_identity::sha256_digest(canonical_json(
        Value(Object{{"runtime_unit_id", Value(source.runtime_unit_id.value())},
                     {"provider_instance_id",
                      Value(owner->instance.value())}})));
    const core::ControlDomainId id(std::string(kind.kind) + ".provider." +
                                   identity.substr(7));
    core::ProviderControlDomainDTO provider{
        source.controllable_component_id,
        source.runtime_unit_id,
        owner->instance,
        owner->manifest,
        owner->revision,
        lifecycle_digest(owner->revision),
        core::ContentDigest(content_identity::framed_sha256(
            kind.configuration_domain, kind.configuration_api,
            json_value(source.row))),
        source.stream_ids,
        kModelFreeLifecycleCommandTimeoutNs};
    if (!provider.locally_valid())
      refuse(kind, "_domain_invalid");
    core::ControlDomainDTO domain{
        id,
        core::ControlDomainRevision("sha256:" + std::string(64, '0')),
        {source.runtime_unit_id},
        owner->endpoint,
        core::ContainmentGranularity::kControlDomain,
        provider};
    domain.control_domain_revision =
        core::ControlDomainRevision(detail::compute_content_identity(
            IoProviderDomainRecord(domain),
            {"control-domain-revision", std::string(core::kCellGraphApiVersion),
             "control_domain_revision",
             detail::ContentIdentityRootShape::kNestedRecord}));
    result.provider_domains.push_back(std::move(domain));
  }
}

static void derive_pose_source_and_ai_model_domains(
    const core::CellGraphDTO &graph,
    std::span<const core::CapabilityProviderBindingDTO> bindings,
    std::span<const QualifiedProviderManifest> manifests, bool structural_only,
    IoProviderDomainDerivation &result) {
  const auto poses = ResolveRetainedPoseSources(graph);
  if (!poses.ok())
    throw std::invalid_argument(poses.error().details.at("reason"));
  std::vector<ModelFreeSource> pose_rows;
  for (const auto &source : poses.value())
    pose_rows.push_back({source.runtime_unit_id,
                         source.controllable_component_id, source.stream_ids,
                         source.row});
  derive_model_free_domains(graph, bindings, manifests, structural_only,
                            kPoseSourceDomainKind, pose_rows, result);
  const auto models = ResolveRetainedAiModels(graph);
  if (!models.ok())
    throw std::invalid_argument(models.error().details.at("reason"));
  std::vector<ModelFreeSource> model_rows;
  for (const auto &model : models.value())
    model_rows.push_back(
        {model.runtime_unit_id,
         model.controllable_component_id,
         {std::string(
             ros2_control_execution_runtime_contract::kAiModelStreamId)},
         model.row});
  derive_model_free_domains(graph, bindings, manifests, structural_only,
                            kAiModelDomainKind, model_rows, result);
}

static core::Result<IoProviderDomainDerivation>
derive_domains(const core::CellGraphDTO &graph,
               std::span<const core::CapabilityProviderBindingDTO> bindings,
               std::span<const QualifiedRuntimeProfile> profiles,
               std::span<const QualifiedProviderManifest> manifests,
               bool structural_only) {
  using Result = core::Result<IoProviderDomainDerivation>;
  try {
    const auto signals = ResolveRetainedIoSignalCatalog(graph);
    if (!signals.ok())
      return Result::Err(signals.error());
    const auto devices = ResolveRetainedIoProviderDevices(graph);
    if (!devices.ok())
      return Result::Err(devices.error());
    // These are exact qualified referents, not first-match fallbacks.
    std::set<
        std::pair<core::ProviderManifestId, core::ProviderManifestRevision>>
        manifest_keys;
    for (const auto &manifest : manifests)
      if (!manifest_keys
               .emplace(manifest.value().provider_manifest_id,
                        manifest.value().provider_manifest_revision)
               .second)
        fail("io_provider_manifest_ambiguous");
    std::set<std::pair<core::RuntimeProfileId, core::RuntimeProfileRevision>>
        profile_keys;
    for (const auto &profile : profiles)
      if (!profile_keys
               .emplace(profile.value().runtime_profile_id,
                        profile.value().runtime_profile_revision)
               .second)
        fail("io_provider_runtime_profile_ambiguous");
    struct Group final {
      Owner owner;
      core::ControllableComponentId component;
      std::set<std::string> names;
    };
    std::map<std::pair<core::RuntimeUnitId, core::ProviderInstanceId>, Group>
        groups;
    IoProviderDomainDerivation result;
    for (const auto &signal : signals.value()) {
      const auto& profile = signal.provider.profile;
      // Every provider but the in-process ones is a device connection.
      const bool ros_connection = profile != "mock" &&
                                  profile != "mock_output" &&
                                  profile != "geometric_contact";
      // A ROS I/O connection owns its actual component and lifecycle. A
      // separate provider domain inside a physical controller component would
      // still couple both through the component's Enable/Disable actions.
      // The actual graph binding proves the conflict; a profile never selects
      // an arm/controller domain or implies a remote process restart.
      if (ros_connection &&
          std::any_of(graph.control_bindings.begin(),
                      graph.control_bindings.end(), [&](const auto &binding) {
                        return binding.controllable_component_id ==
                               signal.descriptor.signal_id.controllable_component_id;
                      }))
        fail("io_ros_connection_component_has_controller_binding");
      auto owner = signal_owner(graph, signal, bindings, profiles, manifests,
                                structural_only);
      const auto key = std::pair{signal.runtime_unit_id, owner.instance};
      const auto [entry, inserted] = groups.try_emplace(
          key, Group{owner, signal.descriptor.signal_id.controllable_component_id, {}});
      if ((!inserted && entry->second.owner != owner) ||
          entry->second.component != signal.descriptor.signal_id.controllable_component_id)
        fail("io_provider_component_ambiguous");
      entry->second.names.insert(signal.descriptor.signal_id.name);
    }
    if (groups.size() != devices.value().size())
      fail("io_provider_device_policy_coverage_mismatch");
    derive_pose_source_and_ai_model_domains(graph, bindings, manifests,
                                            structural_only, result);
    for (const auto &[key, group] : groups) {
      const auto device = std::find_if(
          devices.value().begin(), devices.value().end(), [&](const auto &row) {
            return row.runtime_unit_id == key.first &&
                   row.controllable_component_id == group.component;
          });
      if (device == devices.value().end())
        fail("io_provider_device_policy_missing");
      const auto identity = content_identity::sha256_digest(canonical_json(
          Value(Object{{"runtime_unit_id", Value(key.first.value())},
                       {"provider_instance_id", Value(key.second.value())}})));
      const core::ControlDomainId id("io.provider." + identity.substr(7));
      Object signal_configuration{
          {"io_signals",
           Value(member_rows(graph, key.first, group.names, group.component))},
          {"lifecycle_command_timeout_ns",
           Value(static_cast<double>(device->lifecycle_command_timeout_ns))}};
      if (device->output_source_ack_timeout_ns)
        signal_configuration.emplace_back(
            "output_source_ack_timeout_ns",
            Value(std::to_string(*device->output_source_ack_timeout_ns)));
      core::ProviderControlDomainDTO provider{
          group.component,
          key.first,
          group.owner.instance,
          group.owner.manifest,
          group.owner.revision,
          lifecycle_digest(group.owner.revision),
          core::ContentDigest(content_identity::sha256_digest(
              canonical_json(Value(std::move(signal_configuration))))),
          std::vector<std::string>(group.names.begin(), group.names.end()),
          device->lifecycle_command_timeout_ns};
      if (!provider.locally_valid())
        fail("io_provider_domain_invalid");
      core::ControlDomainDTO domain{
          id,
          core::ControlDomainRevision("sha256:" + std::string(64, '0')),
          {key.first},
          group.owner.endpoint,
          core::ContainmentGranularity::kControlDomain,
          provider};
      domain.control_domain_revision =
          core::ControlDomainRevision(detail::compute_content_identity(
              IoProviderDomainRecord(domain),
              {"control-domain-revision",
               std::string(core::kCellGraphApiVersion),
               "control_domain_revision",
               detail::ContentIdentityRootShape::kNestedRecord}));
      result.provider_domains.push_back(std::move(domain));
      for (const auto &name : group.names)
        result.signal_domains.push_back({{group.component, name}, id});
    }
    std::sort(result.provider_domains.begin(), result.provider_domains.end(),
              [](const auto &a, const auto &b) {
                return a.control_domain_id < b.control_domain_id;
              });
    std::sort(
        result.signal_domains.begin(), result.signal_domains.end(),
        [](const auto &a, const auto &b) { return a.signal_id < b.signal_id; });
    return Result::Ok(std::move(result));
  } catch (const std::exception &error) {
    return Result::Err({"invalid_request",
                        "I/O provider-domain derivation failed",
                        {{"reason", error.what()}}});
  }
}

core::Result<IoProviderDomainDerivation> DeriveIoProviderDomains(
    const core::CellGraphDTO &graph,
    std::span<const core::CapabilityProviderBindingDTO> bindings,
    std::span<const QualifiedRuntimeProfile> profiles,
    std::span<const QualifiedProviderManifest> manifests) {
  return derive_domains(graph, bindings, profiles, manifests, false);
}

static bool lifecycle_binding_coverage(
    std::span<const core::ControlDomainDTO> domains,
    std::span<const core::CapabilityProviderBindingDTO> bindings) {
  for (const auto &domain : domains) {
    const auto *provider = domain.provider();
    if (!provider)
      continue;
    for (const auto capability :
         {"component.lifecycle.observe", "component.lifecycle.prepare",
          "component.lifecycle.enable", "component.lifecycle.disable",
          "component.lifecycle.recover"}) {
      const auto matches = std::count_if(
          bindings.begin(), bindings.end(), [&](const auto &binding) {
            const auto *target =
                std::get_if<core::ControlDomainCapabilityProviderTargetDTO>(
                    &binding.target);
            return binding.capability_id.value() == capability &&
                   binding.scope == core::CapabilityScope::kControlDomain &&
                   target && target->provider() &&
                   target->control_domain_id == domain.control_domain_id &&
                   target->control_domain_revision ==
                       domain.control_domain_revision &&
                   binding.provider_instance_id ==
                       provider->provider_instance_id &&
                   binding.provider_manifest_id ==
                       provider->provider_manifest_id &&
                   binding.provider_manifest_revision ==
                       provider->provider_manifest_revision;
          });
      if (matches != 1)
        return false;
    }
  }
  return true;
}

core::Result<void> ValidateIoProviderDomains(
    const core::CellGraphDTO &graph,
    std::span<const core::CapabilityProviderBindingDTO> bindings,
    std::span<const QualifiedRuntimeProfile> profiles,
    std::span<const QualifiedProviderManifest> manifests) {
  const auto derived =
      DeriveIoProviderDomains(graph, bindings, profiles, manifests);
  if (!derived.ok())
    return core::Result<void>::Err(derived.error());
  std::vector<core::ControlDomainDTO> actual;
  for (const auto &domain : graph.control_domains)
    if (domain.provider())
      actual.push_back(domain);
  std::sort(actual.begin(), actual.end(), [](const auto &a, const auto &b) {
    return a.control_domain_id < b.control_domain_id;
  });
  if (actual != derived.value().provider_domains ||
      !lifecycle_binding_coverage(actual, bindings))
    return core::Result<void>::Err(
        {"invalid_request",
         "Provider domains differ from actual ownership",
         {{"reason", "io_provider_domain_projection_mismatch"}}});
  return core::Result<void>::Ok();
}

core::Result<void> ValidateIoProviderDomainProjection(
    const core::CellGraphDTO &graph,
    std::span<const core::CapabilityProviderBindingDTO> bindings,
    std::span<const QualifiedRuntimeProfile> profiles) {
  const auto derived = derive_domains(graph, bindings, profiles, {}, true);
  if (!derived.ok())
    return core::Result<void>::Err(derived.error());
  std::vector<core::ControlDomainDTO> actual;
  for (const auto &domain : graph.control_domains)
    if (domain.provider())
      actual.push_back(domain);
  std::sort(actual.begin(), actual.end(), [](const auto &a, const auto &b) {
    return a.control_domain_id < b.control_domain_id;
  });
  if (actual != derived.value().provider_domains ||
      !lifecycle_binding_coverage(actual, bindings))
    return core::Result<void>::Err(
        {"invalid_request",
         "Provider domains differ from structural ownership",
         {{"reason", "io_provider_domain_projection_mismatch"}}});
  return core::Result<void>::Ok();
}

} // namespace manyforge::deployment
