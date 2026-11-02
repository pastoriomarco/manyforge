#include "host_io_provider_installation.hpp"
#include "applied_program_blackboard_installation.hpp"
#include "host_io_signal_runtime.hpp"
#include "io_provider_lifecycle_product.hpp"

#include <algorithm>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace manyforge::roboplan_backend::host_detail {
namespace {
struct MissingModelStateSource final : std::runtime_error {
  MissingModelStateSource(const core::ControllableComponentId &robot,
                          const std::vector<std::string> &known_names)
      : std::runtime_error("a joint in the cell model has no state source (robot " +
                           robot.value() + ")") {
    constexpr std::size_t kMaxDetailBytes = 1024U;
    for (const auto &name : known_names) {
      const auto separator = known.empty() ? std::string{} : std::string(", ");
      if (known.size() + separator.size() + name.size() > kMaxDetailBytes) {
        truncated = true;
        break;
      }
      known += separator + name;
    }
  }
  std::string known;
  bool truncated{};
};

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

GeometricContactInputBinding geometric_contact_binding(
    Composition &root,
    const deployment::RetainedIoSignalConfiguration &row,
    const std::shared_ptr<core::IScheduler> &scheduler) {
  require(row.provider.robot_component_id && row.provider.target_scene_resource_id,
          "geometric contact selector is incomplete");
  const auto &robot = *row.provider.robot_component_id;
  Composition *view = nullptr;
  const auto all_views = views(root);
  for (auto *part : all_views)
    if (part->controllable_component_id == robot)
      view = part;
  require(view && view->runtime && view->direct_control_motion_binding &&
              view->ros2_control_runtime_handle && view->target_resolver &&
              view->runtime->motion_state_channel && scheduler,
          "geometric contact robot has no motion runtime observation");
  const auto collision_model = view->candidate_collision_model
                                   ? view->candidate_collision_model
                                   : view->runtime->collision_shared_model;
  require(collision_model != nullptr, "geometric contact robot has no collision model");
  auto publisher = view->candidate_collision_publisher
                       ? view->candidate_collision_publisher
                       : view->runtime->collision_publisher;
  auto scene_cache = view->candidate_geometric_contact_scene_cache
                         ? view->candidate_geometric_contact_scene_cache
                         : view->runtime->geometric_contact_scene_cache;
  require(publisher && scene_cache,
          "geometric contact robot has no collision scene");
  std::vector<std::shared_ptr<const core::ObservedRobotStatePublication>> observed_states;
  bool unavailable_coordinate = false;
  for (const auto &source : root.cell_frame_state_sources) {
    observed_states.push_back(source.numeric_publication);
    unavailable_coordinate |= source.frame_source.coordinate_authority !=
        RetainedSceneFrameCoordinateAuthority::kRequiredPublication;
  }
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
  if (query_joint_names.empty())
    throw MissingModelStateSource(robot, query_joint_names);
  require(engine->prepare_runtime_query(query_joint_names) ==
                  core::RuntimeCollisionQueryPrepareStatus::kReady,
          "geometric contact robot lacks current articulated coordinates");
  if (!engine->runtime_query_covers_full_configuration())
    throw MissingModelStateSource(robot, query_joint_names);
  auto read_contact_bodies = [handle = view->ros2_control_runtime_handle,
                              resolver = view->target_resolver,
                              binding = *view->direct_control_motion_binding]() {
    return resolver->ActiveTcpContactBodies(binding, handle->ReadActiveTcp().value);
  };
  return {row.descriptor.source_id,
          core::IoSourceIncarnation("installation-owned"),
          std::vector<core::IoSignalDescriptorDTO>{},
          std::move(read_contact_bodies),
          *row.provider.target_scene_resource_id,
          std::move(observed_states), std::move(publisher), engine,
          scheduler, view->runtime->collision_monitor_period_ns,
          view->runtime->runtime_collision_immediate_query_budget_ns,
          [engine](const core::CollisionWorldSnapshot &world,
                   const core::CollisionBodyId &body) {
            return engine->IsBodyStaticWrtGroup(world, body);
          }, unavailable_coordinate};
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
      require(entry != nullptr, "owned output channel is absent");
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
              "one output resource has multiple actual channels");
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
  if (const auto *missing = dynamic_cast<const MissingModelStateSource *>(&error))
    return {"io_provider_installation_failed", missing->what(),
            {{"known_source_joint_names", missing->known},
             {"known_source_joint_names_truncated", missing->truncated ? "true" : "false"}}};
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
    Composition &candidate, RetainedCellRuntime &cell,
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
    const auto catalog = deployment::ResolveCompiledIoSignalCatalog(
        candidate.bundle->value().cell_graph);
    if (!catalog.ok())
      return Reply::Err(catalog.error());
    const auto devices = deployment::ResolveRetainedIoProviderDevices(
        candidate.bundle->value().cell_graph);
    if (!devices.ok())
      return Reply::Err(devices.error());
    // The authored answer bound of each device's acknowledged outputs.
    const auto ack_timeout_ns = [&](const deployment::RetainedIoSignalConfiguration &row) {
      for (const auto &device : devices.value())
        if (device.runtime_unit_id == row.runtime_unit_id &&
            device.controllable_component_id == row.descriptor.signal_id.controllable_component_id)
          return device.output_source_ack_timeout_ns.value_or(0);
      return core::Duration{0};
    };
    const auto key_capacity = CompiledProgramBlackboardKeyCapacity(candidate);
    if (!key_capacity.ok())
      return Reply::Err(key_capacity.error());
    auto existing = existing_channels(actual_existing);
    std::map<core::InterruptSourceId, HostIoSignalRuntime::SourceInput> sources;
    std::map<core::ResourceId, std::vector<HostIoOutputAliasInstallation>>
        aliases;
    std::map<core::ControlDomainId, std::shared_ptr<IoProviderDriverRecord>>
        drivers;
    // Compiled system signals have no I/O driver or lifecycle: the runtime
    // owns their one in-process source.
    std::vector<deployment::RetainedIoSignalConfiguration> system_rows;
    // A machine's I/O member signals on the real lane (CONTRACTS §16): no
    // I/O driver either; their family's I/O update sink feeds them.
    std::vector<deployment::RetainedIoSignalConfiguration> adapter_rows;
    for (const auto &row : catalog.value()) {
      if (row.provider.profile == deployment::kSystemCarriedObjectProfile) {
        system_rows.push_back(row);
        continue;
      }
      if (row.provider.profile == deployment::kComponentAdapterIoProfile) {
        adapter_rows.push_back(row);
        continue;
      }
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
            require(source.rows.front().provider.robot_component_id ==
                            row.provider.robot_component_id &&
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
             candidate.publication_generation, ack_timeout_ns(row)});
        // A Mock output reports its written value back (CONTRACTS §7, D2).
        if (row.provider.profile == "mock_output")
          adapter_rows.push_back(row);
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
        sources.empty() && system_rows.empty() && adapter_rows.empty() &&
                key_capacity.value() == 0U
            ? std::shared_ptr<HostIoSignalRuntime>{}
            : HostIoSignalRuntime::Create(
                  cell.addressed_kernel().request_owner_epoch(),
                  candidate.publication_generation.value(), std::move(sources),
                  cell.clock(), cell.scheduler(), key_capacity.value(),
                  std::move(system_rows), std::move(adapter_rows));
    for (const auto &[resource, rows] : aliases) {
      static_cast<void>(rows);
      const auto old = existing.find(resource);
      if (old != existing.end())
        channels.emplace(resource, old->second.entry);
    }
    for (const auto &row : catalog.value()) {
      if (row.provider.profile != "mock_output" ||
          row.descriptor.direction != core::IoSignalDirection::kOutput)
        continue;
      const auto *provider = std::get_if<std::shared_ptr<MockBooleanOutputProvider>>(
          &channels.at(*row.descriptor.output_resource)->provider);
      require(runtime && provider && *provider, "Mock output channel is not synthetic");
      runtime->AddMockOutput(*provider, row.descriptor.signal_id);
    }
    for (const auto &row : catalog.value()) {
      if (!row.provider.echo_output_signal_name)
        continue;
      const core::IoSignalId output{
          row.descriptor.signal_id.controllable_component_id,
          *row.provider.echo_output_signal_name};
      const auto match = std::find_if(catalog.value().begin(), catalog.value().end(),
          [&](const auto &candidate_row) {
            return candidate_row.descriptor.signal_id == output &&
                   candidate_row.descriptor.direction == core::IoSignalDirection::kOutput;
          });
      require(runtime && match != catalog.value().end() &&
                  match->descriptor.output_resource && row.provider.echo_delay_ns,
              "Mock echo has no qualified input/output");
      const auto channel = channels.find(*match->descriptor.output_resource);
      require(channel != channels.end(), "Mock echo output channel is missing");
      const auto *provider = std::get_if<std::shared_ptr<MockBooleanOutputProvider>>(
          &channel->second->provider);
      require(provider && *provider, "Mock echo output is not synthetic");
      runtime->AddMockEcho(*provider, row.descriptor.signal_id,
                           *row.provider.echo_delay_ns);
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
              // preserves its immutable SetBool/GPIO selector tuple.
              require(mock ? profile == "mock_output"
                           : (profile == "ros_set_bool" ||
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
    // IO-1: the Deployment rows of the devices' optional `inputs` streams.
    const auto stream_rows =
        deployment::ComponentStreamParameterOverrides(*candidate.bundle);
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
      std::optional<std::int64_t> inputs_budget_ns;
      for (const auto &row : stream_rows)
        if (row.controllable_component_id ==
                driver->domain().provider()->provider.controllable_component_id &&
            row.stream_id == kIoDeviceInputsStreamId)
          inputs_budget_ns = row.budget_ns;
      const auto stream = driver->SetInputsStreamLocked(inputs_budget_ns);
      if (!stream.ok())
        return Reply::Err(stream.error());
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

} // namespace manyforge::roboplan_backend::host_detail
