#include "host_io_provider_installation.hpp"
#include "applied_program_blackboard_installation.hpp"
#include "host_io_signal_runtime.hpp"
#include "io_provider_lifecycle_product.hpp"

#include <algorithm>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace manyforge::roboplan_backend::host_detail {
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::vector<Composition *> views(Composition &root) {
  std::vector<Composition *> result{&root};
  for (const auto &part : root.participant_compositions) {
    require(part != nullptr, "I/O component view is absent");
    result.push_back(part.get());
  }
  std::set<core::ControllableComponentId> components;
  for (const auto *view : result)
    require(components.insert(view->controllable_component_id).second,
            "I/O component view is repeated");
  return result;
}
std::shared_ptr<IoProviderDriverRecord>
driver_for(const Composition &root,
           const deployment::RetainedIoSignalConfiguration &row) {
  std::shared_ptr<IoProviderDriverRecord> result;
  const auto include = [&](const Composition &view) {
    if (view.controllable_component_id !=
            row.descriptor.signal_id.controllable_component_id ||
        !view.io_provider_drivers)
      return;
    for (const auto &driver : *view.io_provider_drivers) {
      require(driver && driver->domain().provider(),
              "I/O actual driver is absent");
      const auto &domain = driver->domain().provider()->provider;
      if (domain.controllable_component_id ==
              row.descriptor.signal_id.controllable_component_id &&
          domain.provider_runtime_unit_id == row.runtime_unit_id &&
          std::binary_search(domain.signal_names.begin(),
                             domain.signal_names.end(),
                             row.descriptor.signal_id.name)) {
        require(!result, "I/O signal has multiple actual driver owners");
        result = driver;
      }
    }
  };
  include(root);
  for (const auto &part : root.participant_compositions)
    include(*part);
  require(result != nullptr,
          "I/O signal has no actual lifecycle driver adapter");
  return result;
}

Composition &endpoint_view(Composition &root,
                           const deployment::IoSignalProviderConfiguration::
                               GeometricContactEndpoint &endpoint) {
  Composition *result = nullptr;
  const auto include = [&](Composition &view) {
    if (view.controllable_component_id != endpoint.controllable_component_id)
      return;
    require(result == nullptr,
            "geometric contact endpoint component is repeated");
    result = &view;
  };
  include(root);
  for (const auto &part : root.participant_compositions) {
    require(part != nullptr, "geometric contact endpoint view is absent");
    include(*part);
  }
  require(result != nullptr,
          "geometric contact endpoint component is unavailable");
  return *result;
}

GeometricContactInputBinding geometric_contact_binding(
    Composition &root,
    const deployment::RetainedIoSignalConfiguration &row,
    const std::shared_ptr<core::IScheduler> &scheduler) {
  require(row.provider.endpoint && row.provider.target_scene_resource_id,
          "geometric contact selector is incomplete");
  auto &view = endpoint_view(root, *row.provider.endpoint);
  const auto &selector = *row.provider.endpoint;
  const core::RuntimeParticipantBinding *selected_participant = nullptr;
  const auto select_participant = [&](const auto &binding) {
    if (!binding ||
        binding->value().controllable_component_id !=
            selector.controllable_component_id ||
        binding->value().command_target_id != selector.command_target_id)
      return;
    require(selected_participant == nullptr,
            "geometric contact endpoint target is repeated");
    selected_participant = &*binding;
  };
  select_participant(view.direct_control_motion_binding);
  select_participant(view.direct_control_gripper_binding);
  const auto collision_model = view.candidate_collision_model
                                   ? view.candidate_collision_model
                                   : view.runtime
                                         ? view.runtime->collision_shared_model
                                         : nullptr;
  require(view.runtime && selected_participant != nullptr &&
              view.runtime->motion_state_channel &&
              collision_model && scheduler,
          "geometric contact endpoint has no current runtime observation");
  const auto &participant = selected_participant->value();
  const auto frame = std::find_if(
      participant.frame_bindings.begin(), participant.frame_bindings.end(),
      [&](const auto &candidate) {
        return candidate.frame_role_id == selector.frame_role_id;
      });
  require(frame != participant.frame_bindings.end() &&
              !frame->contact_body_role_ids.empty(),
          "geometric contact endpoint has no declared contact bodies");
  core::RuntimeMotionEndpointBindingDTO endpoint{
      participant.target_role_id, participant.command_target_id, *frame, {}};
  for (const auto &role : frame->contact_body_role_ids) {
    const auto body = std::find_if(
        participant.collision_body_bindings.begin(),
        participant.collision_body_bindings.end(), [&](const auto &candidate) {
          return candidate.collision_body_role_id == role &&
                 candidate.model_instance_id == frame->model_instance_id &&
                 candidate.model_revision == frame->model_revision;
        });
    require(body != participant.collision_body_bindings.end() &&
                !body->compiled_collision_body_names.empty(),
            "geometric contact endpoint body is unresolved");
    std::vector<std::string> ids;
    ids.reserve(body->compiled_collision_body_names.size());
    for (std::size_t index = 0;
         index < body->compiled_collision_body_names.size(); ++index)
      ids.push_back(core::make_component_collision_body_id(
          participant.controllable_component_id, frame->model_instance_id,
          role, index));
    endpoint.contact_body_bindings.push_back(
        {participant.component_role_id, participant.target_role_id,
         participant.controllable_component_id, participant.command_target_id,
         std::nullopt, role, frame->model_instance_id, frame->model_revision,
         std::move(ids)});
  }
  auto endpoint_value = participant;
  endpoint_value.motion_endpoint = std::move(endpoint);
  auto verified_endpoint =
      core::verify_runtime_participant_binding(std::move(endpoint_value));
  require(verified_endpoint.ok() && view.target_resolver != nullptr,
          "geometric contact endpoint binding is invalid");
  auto projected_endpoint = view.target_resolver->BindCollisionBodyAliases(
      std::move(verified_endpoint.value()));
  require(projected_endpoint.ok() &&
              projected_endpoint.value().value().motion_endpoint.has_value(),
          "geometric contact endpoint body projection is unresolved");
  endpoint = *projected_endpoint.value().value().motion_endpoint;
  std::vector<std::shared_ptr<const core::ObservedRobotStatePublication>>
      observed_states;
  const auto include_observed = [&](const auto &channel) {
    if (channel)
      observed_states.emplace_back(
          channel,
          &channel->addressed_publication()->numeric_publication());
  };
  include_observed(view.runtime->motion_state_channel);
  include_observed(view.runtime->gripper_state_channel);
  auto publisher = view.candidate_collision_publisher
                       ? view.candidate_collision_publisher
                       : view.runtime->collision_publisher;
  require(publisher != nullptr,
          "geometric contact endpoint has no collision publication");
  auto scene_cache = view.candidate_geometric_contact_scene_cache
                         ? view.candidate_geometric_contact_scene_cache
                         : view.runtime->geometric_contact_scene_cache;
  require(scene_cache != nullptr,
          "geometric contact endpoint has no shared scene cache");
  auto engine = std::make_shared<collision::ManyForgeCollisionEngine>(
      collision_model, std::move(scene_cache));
  std::vector<std::string> query_joint_names;
  std::set<std::string, std::less<>> unique_joint_names;
  for (const auto &publication : observed_states)
    for (const auto &name : publication->canonical_joint_names()) {
      require(unique_joint_names.insert(name).second,
              "geometric contact observed coordinate is repeated");
      query_joint_names.push_back(name);
    }
  require(engine->prepare_runtime_query(query_joint_names) ==
                  core::RuntimeCollisionQueryPrepareStatus::kReady &&
              engine->runtime_query_covers_full_configuration(),
          "geometric contact endpoint lacks current articulated coordinates");
  return {row.descriptor.source_id,
          core::IoSourceIncarnation("installation-owned"),
          std::vector<core::IoSignalDescriptorDTO>{},
          std::move(endpoint),
          *row.provider.target_scene_resource_id,
          std::move(observed_states),
          std::move(publisher),
          std::move(engine),
          scheduler,
          view.runtime->collision_monitor_period_ns,
          view.runtime->runtime_collision_immediate_query_budget_ns,
          view.runtime->runtime_dynamic_body_max_age_cap_ns};
}
bool same_aliases(const std::vector<IoOutputAliasIdentity> &left,
                  const std::vector<IoOutputAliasIdentity> &right) {
  if (left.size() != right.size())
    return false;
  for (std::size_t i = 0; i < left.size(); ++i)
    if (left[i].signal_id != right[i].signal_id ||
        left[i].provider != right[i].provider ||
        !SameIoOutputDriverOwner(left[i].actual_driver_owner,
                                 right[i].actual_driver_owner))
      return false;
  return true;
}
struct ExistingChannel final {
  std::shared_ptr<HostIoOutputChannelEntry> entry;
  IoOutputAliasState actual;
  std::optional<std::string> gpio_topic;
};
std::map<core::ResourceId, ExistingChannel>
existing_channels(std::span<const CompositionIoResourcePredecessor> roots) {
  std::map<core::ResourceId, ExistingChannel> result;
  for (const auto &root : roots) {
    if (root.channels.empty())
      continue;
    require(root.bundle != nullptr, "owned output catalog is absent");
    const auto catalog = deployment::ResolveRetainedIoSignalCatalog(
        root.bundle->value().cell_graph);
    require(catalog.ok(), "owned output catalog is invalid");
    for (const auto &entry : root.channels) {
      require(entry && entry->engine,
              "owned output channel has no actual engine");
      const auto actual = std::visit(
          [](const auto &p) { return p->ReadAliasState(); }, entry->provider);
      if (actual.active_publication_generation != root.publication_generation)
        continue;
      std::vector<IoOutputAliasIdentity> expected;
      std::optional<std::string> gpio_topic;
      for (const auto &row : catalog.value()) {
        if (row.descriptor.direction != core::IoSignalDirection::kOutput ||
            row.descriptor.output_resource != entry->resource_id)
          continue;
        if (row.provider.profile == "ros2_control_gpio") {
          require(row.provider.topic &&
                      (!gpio_topic || gpio_topic == row.provider.topic),
                  "owned GPIO channel has conflicting sealed command topics");
          gpio_topic = row.provider.topic;
        }
        std::shared_ptr<IoProviderDriverRecord> driver;
        for (const auto &group : root.driver_groups) {
          require(group != nullptr, "owned output driver group is absent");
          for (const auto &record : *group) {
            require(record && record->domain().provider(),
                    "owned output driver is absent");
            const auto &domain = record->domain().provider()->provider;
            if (domain.controllable_component_id ==
                    row.descriptor.signal_id.controllable_component_id &&
                domain.provider_runtime_unit_id == row.runtime_unit_id &&
                std::binary_search(domain.signal_names.begin(),
                                   domain.signal_names.end(),
                                   row.descriptor.signal_id.name)) {
              require(!driver, "owned output alias repeats an actual driver");
              driver = record;
            }
          }
        }
        require(driver != nullptr, "owned output has no exact actual driver");
        expected.push_back(
            {row.descriptor.signal_id, driver->occurrence(), driver});
      }
      std::sort(expected.begin(), expected.end(),
                [](const auto &a, const auto &b) {
                  return a.signal_id < b.signal_id;
                });
      require(
          !expected.empty() && same_aliases(expected, actual.aliases),
          "owned output metadata differs from its exact sealed composition");
      const auto [found, inserted] =
          result.emplace(entry->resource_id, ExistingChannel{entry, actual, gpio_topic});
      require(inserted || found->second.entry == entry,
              "one output resource has multiple actual channel engines");
    }
  }
  for (const auto &root : roots)
    for (const auto &entry : root.channels)
      require(
          entry && result.contains(entry->resource_id) &&
              result.at(entry->resource_id).entry == entry,
          "output channel has no sealed predecessor/original candidate proof");
  return result;
}
core::ErrorDTO failure(const std::exception &error) {
  return {"io_provider_installation_failed", error.what(), {}};
}
} // namespace

std::vector<CompositionIoResourcePredecessor>
CaptureCompositionIoResourcePredecessors(
    std::span<const Composition *const> roots) {
  std::vector<CompositionIoResourcePredecessor> result;
  for (const auto *root : roots) {
    if (!root)
      continue;
    CompositionIoResourcePredecessor receipt{root->bundle,
                                             root->publication_generation,
                                             {},
                                             root->io_output_channels,
                                             root->io_signal_runtime};
    if (root->io_provider_drivers)
      receipt.driver_groups.push_back(root->io_provider_drivers);
    for (const auto &view : root->participant_compositions) {
      require(view != nullptr, "owned I/O component view is absent");
      if (view->io_provider_drivers)
        receipt.driver_groups.push_back(view->io_provider_drivers);
    }
    result.push_back(std::move(receipt));
  }
  return result;
}

core::Result<void> PrepareCompositionIoProviderResources(
    Composition &candidate, RetainedNativeCellRuntime &cell,
    rclcpp::Node::SharedPtr generic_node,
    std::span<const CompositionIoResourcePredecessor> actual_existing) {
  using Reply = core::Result<void>;
  try {
    if (candidate.io_resource_preparation &&
        candidate.io_resource_preparation->publication_generation ==
            candidate.publication_generation) {
      if (candidate.io_resource_preparation->complete)
        return Reply::Ok();
      return Reply::Err({"io_resource_preparation_incomplete",
                         "The original preparation failed after its candidate "
                         "resources were pinned",
                         {}});
    }
    const auto catalog = deployment::ResolveRetainedIoSignalCatalog(
        candidate.bundle->value().cell_graph);
    if (!catalog.ok())
      return Reply::Err(catalog.error());
    const auto key_capacity = CompiledProgramBlackboardKeyCapacity(candidate);
    if (!key_capacity.ok())
      return Reply::Err(key_capacity.error());
    auto existing = existing_channels(actual_existing);
    std::map<core::InterruptSourceId, HostIoSignalRuntime::SourceInput> sources;
    std::map<core::ResourceId, std::vector<HostIoOutputAliasInstallation>>
        aliases;
    std::map<core::ControlDomainId, std::shared_ptr<IoProviderDriverRecord>>
        drivers;
    for (const auto &row : catalog.value()) {
      const auto driver = driver_for(candidate, row);
      if (row.descriptor.direction == core::IoSignalDirection::kOutput &&
          driver->resources_installed())
        require(row.descriptor.output_resource &&
                    existing.contains(*row.descriptor.output_resource),
                "retained driver has no qualified actual output channel");
      const auto [known, inserted] =
          drivers.emplace(driver->domain().control_domain_id, driver);
      require(inserted || known->second == driver,
              "I/O domain has conflicting actual owners");
      if (row.descriptor.direction == core::IoSignalDirection::kInput) {
        auto &source = sources[row.descriptor.source_id];
        require(source.rows.empty() || source.driver == driver,
                "I/O source spans driver owners");
        source.driver = driver;
        source.installation_occurrence = driver->runtime_unit_epoch();
        source.node = generic_node;
        source.rows.push_back(row);
        if (row.provider.profile == "geometric_contact") {
          if (!source.geometric_contact)
            source.geometric_contact =
                geometric_contact_binding(candidate, row, cell.scheduler());
          else
            require(source.geometric_contact->endpoint.frame.frame_role_id ==
                            row.provider.endpoint->frame_role_id &&
                        source.geometric_contact->target_scene_resource_id ==
                            *row.provider.target_scene_resource_id,
                    "geometric contact source changes its selector");
          source.geometric_contact->signals.push_back(row.descriptor);
        }
      } else {
        require(row.descriptor.output_resource.has_value(),
                "output has no canonical resource");
        auto delivery = driver->AcquireDelivery();
        aliases[*row.descriptor.output_resource].push_back(
            {row, driver->occurrence(),
             driver->driver_observation_generation_locked(), driver,
             candidate.publication_generation});
      }
    }
    // These labels come from the same sealed catalog/actual alias proof above.
    // The helper only borrows the leaf's actual opaque whole-topic pin; the
    // leaf independently rechecks ROS context/address on construction.
    std::vector<HostIoGpioTopicPin> captured_topics;
    for (const auto &[resource, channel] : existing) {
      static_cast<void>(resource);
      if (!channel.gpio_topic) continue;
      const auto *provider =
          std::get_if<std::shared_ptr<RosBooleanOutputProvider>>(&channel.entry->provider);
      require(provider && *provider && (*provider)->gpio_topic(),
              "owned GPIO channel has no actual shared topic pin");
      captured_topics.push_back({*channel.gpio_topic, (*provider)->gpio_topic()});
    }
    std::vector<HostIoOutputAliasInstallation> fresh_aliases;
    for (const auto &[resource, rows] : aliases)
      if (!existing.contains(resource))
        fresh_aliases.insert(fresh_aliases.end(), rows.begin(), rows.end());
    auto created = CreateHostIoOutputChannels(std::move(fresh_aliases),
                                              generic_node, *cell.clock(),
                                              captured_topics);
    if (!created.ok())
      return Reply::Err(created.error());
    std::map<core::ResourceId, std::shared_ptr<HostIoOutputChannelEntry>>
        channels;
    for (auto &entry : created.value()) {
      const auto id = entry.resource_id;
      channels.emplace(
          id, std::make_shared<HostIoOutputChannelEntry>(std::move(entry)));
    }
    auto runtime =
        sources.empty() && key_capacity.value() == 0U
            ? std::shared_ptr<HostIoSignalRuntime>{}
            : HostIoSignalRuntime::Create(
                  cell.addressed_kernel().request_owner_epoch(),
                  candidate.publication_generation.value(), std::move(sources),
                  cell.clock(), cell.scheduler(), key_capacity.value());
    for (const auto &[resource, rows] : aliases) {
      static_cast<void>(rows);
      const auto old = existing.find(resource);
      if (old != existing.end())
        channels.emplace(resource, old->second.entry);
    }
    std::vector<std::shared_ptr<HostIoOutputChannelEntry>> candidate_channels;
    candidate_channels.reserve(channels.size());
    for (const auto &[resource, entry] : channels) {
      static_cast<void>(resource);
      candidate_channels.push_back(entry);
    }
    candidate.io_resource_preparation = CompositionIoResourcePreparation{
        candidate.publication_generation, false};
    candidate.io_output_channels = std::move(candidate_channels);
    candidate.io_signal_runtime = runtime;
    for (const auto &[resource, rows] : aliases) {
      const auto old = existing.find(resource);
      if (old == existing.end())
        continue;
      const auto replaced = std::visit(
          [&](const auto &provider) {
            using Provider =
                typename std::decay_t<decltype(provider)>::element_type;
            using Alias = std::conditional_t<
                std::is_same_v<Provider, MockBooleanOutputProvider>,
                MockBooleanOutputAlias, RosBooleanOutputAlias>;
            const bool mock =
                std::is_same_v<Provider, MockBooleanOutputProvider>;
            std::vector<Alias> installed;
            for (const auto &row : rows) {
              const auto &profile = row.row.provider.profile;
              // The sealed canonical resource includes the complete profile
              // and typed address. Matching that same actual channel above
              // preserves its immutable SetBool/UR/GPIO selector tuple.
              require(mock ? profile == "mock_output"
                           : (profile == "ros_set_bool" ||
                              profile == "ur_set_io" ||
                              profile == "ros2_control_gpio"),
                      "retained output channel changed its immutable profile");
              installed.push_back({row.row.descriptor.signal_id, row.provider,
                                   row.prepared_driver_generation,
                                   row.actual_driver_owner});
            }
            return provider->RebindAliases(old->second.actual.aliases,
                                           std::move(installed),
                                           candidate.publication_generation);
          },
          old->second.entry->provider);
      if (!replaced.ok())
        return Reply::Err(replaced.error());
    }
    for (const auto &[resource, old] : existing) {
      if (aliases.contains(resource))
        continue;
      for (const auto &alias : old.actual.aliases)
        std::visit(
            [&](const auto &provider) {
              provider->ShutdownOccurrence(alias.provider,
                                           alias.actual_driver_owner);
            },
            old.entry->provider);
    }
    // After allocating/validating the candidate, retire old timers before any
    // source handoff. Actual borrowed leaf/driver lifetimes remain retained.
    std::set<HostIoSignalRuntime *> retired;
    for (const auto &root : actual_existing)
      if (root.runtime && root.runtime != runtime &&
          retired.insert(root.runtime.get()).second)
        root.runtime->Shutdown();
    for (const auto &[id, driver] : drivers) {
      std::vector<IoProviderInputSourceInstallation> inputs;
      if (runtime) {
        const auto found = runtime->driver_input_installations().find(driver);
        if (found != runtime->driver_input_installations().end())
          inputs = found->second;
      }
      std::vector<IoProviderOutputChannelInstallation> outputs;
      for (const auto &[resource, rows] : aliases) {
        std::vector<deployment::RetainedIoSignalConfiguration> own;
        for (const auto &row : rows)
          if (row.provider.control_domain_id == id)
            own.push_back(row.row);
        if (!own.empty())
          outputs.push_back(
              {resource, std::move(own), channels.at(resource)->provider});
      }
      const bool installed = driver->resources_installed();
      if (installed && inputs.empty()) {
        // An output-only retained record has no source incarnation or timer
        // owner to hand off. Its actual channel/publication rebind above is
        // the Apply effect; do not invent an input merely to reset its model.
        const auto &membership = driver->domain().provider()->provider;
        const bool has_input = std::any_of(
            catalog.value().begin(), catalog.value().end(),
            [&](const auto &row) {
              return row.descriptor.direction ==
                         core::IoSignalDirection::kInput &&
                     row.descriptor.signal_id.controllable_component_id ==
                         membership.controllable_component_id &&
                     row.runtime_unit_id ==
                         membership.provider_runtime_unit_id &&
                     std::binary_search(membership.signal_names.begin(),
                                        membership.signal_names.end(),
                                        row.descriptor.signal_id.name);
            });
        require(!has_input,
                "retained input driver has no actual ingress installation");
        continue;
      }
      auto delivery = driver->AcquireDelivery();
      auto cancel = runtime ? runtime->cancellation_hook(driver)
                            : std::function<void()>{[] {}};
      auto lifecycle = runtime ? runtime->input_lifecycle_hook(driver)
                               : IoProviderDriverRecord::InputLifecycleHook{};
      auto acquisition =
          runtime ? runtime->input_acquisition_hook(driver)
                  : IoProviderDriverRecord::InputAcquisitionHook{};
      const auto prepared =
          installed
              ? driver->RebindInputsLocked(std::move(inputs), std::move(cancel),
                                           std::move(lifecycle),
                                           std::move(acquisition))
              : driver->InstallResourcesLocked(
                    std::move(inputs), std::move(outputs), std::move(cancel),
                    std::move(lifecycle), std::move(acquisition));
      if (!prepared.ok())
        return Reply::Err(prepared.error());
    }
    for (const auto &root : actual_existing)
      if (root.runtime && !root.runtime->driver_input_installations().empty()) {
        const auto drained = root.runtime->DrainPending();
        if (!drained.ok())
          return Reply::Err(drained.error());
      }
    candidate.io_resource_preparation->complete = true;
    return Reply::Ok();
  } catch (const std::exception &error) {
    return Reply::Err(failure(error));
  }
}

core::Result<void> BindCompositionIoOutputParticipants(
    Composition &candidate,
    const IoProviderConstructionGenerations &actual_generations) {
  using Reply = core::Result<void>;
  try {
    const auto catalog = deployment::ResolveRetainedIoSignalCatalog(
        candidate.bundle->value().cell_graph);
    if (!catalog.ok())
      return Reply::Err(catalog.error());
    const auto owners = views(candidate);
    for (auto *owner : owners)
      owner->io_output_registrations.clear();
    std::vector<std::shared_ptr<const deployment::ProgramBinding>> bindings =
        candidate.direct_control_io_program_bindings;
    if (candidate.program_binding)
      bindings.push_back(candidate.program_binding);
    std::set<core::ProgramBindingId> seen;
    std::vector<core::ProgramBindingId> intrinsic;
    for (const auto &binding : candidate.direct_control_io_program_bindings)
      intrinsic.push_back(binding->program_binding_id());
    for (const auto &binding : bindings) {
      if (!seen.insert(binding->program_binding_id()).second)
        continue;
      for (const auto &target : binding->value().target_role_bindings) {
        if (std::none_of(target.capability_bindings.begin(),
                         target.capability_bindings.end(), [](const auto &cap) {
                           return cap.capability_id.value() ==
                                  "io.gpio.set_output";
                         }))
          continue;
        require(candidate.target_resolver != nullptr,
                "output has no sealed target resolver");
        auto sealed_target = candidate.target_resolver->ResolveBoundTarget(
            *binding, target.target_role_id,
            core::CapabilityId("io.gpio.set_output"));
        if (!sealed_target.ok())
          return Reply::Err(sealed_target.error());
        for (const auto &role : target.signal_role_bindings) {
          if (role.direction != core::IoSignalDirection::kOutput)
            continue;
          const auto row = std::find_if(
              catalog.value().begin(), catalog.value().end(),
              [&](const auto &value) {
                return value.descriptor.signal_id.controllable_component_id ==
                           target.controllable_component_id &&
                       value.command_target_id == target.command_target_id &&
                       value.resource_role_id == role.resource_role_id &&
                       value.descriptor.signal_id.name == role.signal_name;
              });
          require(row != catalog.value().end() &&
                      row->descriptor.output_resource,
                  "output role has no exact sealed signal/resource");
          const auto driver = driver_for(candidate, *row);
          const auto generation =
              actual_generations.find(target.controllable_component_id);
          require(generation != actual_generations.end() &&
                      generation->second.nonzero(),
                  "output binding has no actual CCLC construction generation");
          core::RuntimeProviderDomainConfigurationReadbackDTO closed{
              candidate.bundle->retained_bundle_id(),
              candidate.bundle->value().cell_graph.graph_revision,
              generation->second,
              driver->domain(),
              driver->occurrence(),
              false,
              0U,
              0U};
          auto verified = core::verify_runtime_participant_binding(
              deployment::build_runtime_io_output_binding_dto(
                  sealed_target.value(), role.resource_role_id,
                  *candidate.bundle, closed));
          if (!verified.ok())
            return Reply::Err(verified.error());
          const auto channel = std::find_if(
              candidate.io_output_channels.begin(),
              candidate.io_output_channels.end(), [&](const auto &entry) {
                return entry->resource_id == *row->descriptor.output_resource;
              });
          require(channel != candidate.io_output_channels.end() &&
                      (*channel)->engine,
                  "output has no actual canonical channel engine");
          const auto owner =
              std::find_if(owners.begin(), owners.end(), [&](const auto *view) {
                return view->controllable_component_id ==
                       target.controllable_component_id;
              });
          require(owner != owners.end(), "output has no actual component view");
          (*owner)->io_output_registrations.push_back(
              {.binding = std::move(verified.value()),
               .stop_readiness = {},
               .retained_default_stop_limits = {},
               .stop_node_category =
                   core::MotionInterruptNodeCategory::kGeneric,
               .engine = (*channel)->engine.get(),
               .stop_execution_port = nullptr,
               .participant_policy = {},
               .prepared_motion_engine = (*channel)->engine.get(),
               .default_operator_stop_profile = {},
               .default_fault_stop_profile = {},
               .deployment_maximum_tcp_linear_speed_mps = {},
               .intrinsic_direct_control =
                   std::find(intrinsic.begin(), intrinsic.end(),
                             binding->program_binding_id()) != intrinsic.end(),
               .direct_control_joint_limits = {}});
        }
      }
    }
    std::vector<core::RuntimeParticipantBinding> composed;
    for (const auto *owner : owners)
      for (const auto &registration : owner->component_registrations())
        composed.push_back(registration.binding);
    if (candidate.target_resolver) {
      // Input-only direct bindings have no execution rows and are not grants.
      std::erase_if(intrinsic, [&](const auto &id) {
        return std::none_of(composed.begin(), composed.end(),
                            [&](const auto &value) {
                              return value.value().program_binding_id == id;
                            });
      });
      candidate.target_resolver =
          candidate.target_resolver->WithComposedParticipants(composed,
                                                              intrinsic);
      candidate.candidate_participant_resolver_factory =
          [resolver = candidate.target_resolver](auto current) {
            auto product =
                resolver->WithComposedParticipants(std::move(current));
            return RetainedRuntimeParticipantResolver{
                [product](const auto &operation, auto index) {
                  return product->ResolveExecutionParticipant(operation, index);
                }};
          };
    }
    return Reply::Ok();
  } catch (const std::exception &error) {
    return Reply::Err(failure(error));
  }
}
} // namespace manyforge::roboplan_backend::host_detail
