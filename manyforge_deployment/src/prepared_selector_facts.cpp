#include "prepared_selector_facts_internal.hpp"
#include "prepared_catalog_source_internal.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_core/types/io_signal.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_io_stop_response_validation.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>

namespace manyforge::deployment::detail {
namespace {
using V = CanonicalValue;
using Target = core::ProgramTargetReferenceDTO;
using Signal = core::ProgramSignalReferenceDTO;
using Frame = core::ProgramFrameReferenceDTO;
using Body = core::ProgramCollisionBodyReferenceDTO;

V target_value(const Target& value) {
  return V(V::Object{{"component_id", V(value.component_id.value())},
                     {"command_target_id", V(value.command_target_id.value())}});
}
V signal_value(const Signal& value) {
  return V(V::Object{{"component_id", V(value.component_id.value())},
                     {"signal_name", V(value.signal_name)}});
}
V frame_value(const Frame& value) {
  return V(V::Object{{"component_id", V(value.component_id.value())},
                     {"frame_id", V(value.frame_id.value())}});
}
V body_value(const Body& value) {
  return V(V::Object{{"component_id", V(value.component_id.value())},
                     {"collision_body_id", V(value.collision_body_id.value())}});
}
V targets_value(const std::set<Target>& values) {
  V::Array result;
  for (const auto& value : values) result.push_back(target_value(value));
  return V(std::move(result));
}
std::string direction(core::IoSignalDirection value) {
  switch (value) {
    case core::IoSignalDirection::kInput: return "input";
    case core::IoSignalDirection::kOutput: return "output";
  }
  throw std::invalid_argument("candidate signal direction is unknown");
}
std::string update_mode(core::IoSignalUpdateMode value) {
  switch (value) {
    case core::IoSignalUpdateMode::kSampled: return "sampled";
    case core::IoSignalUpdateMode::kChangeOnly: return "change_only";
  }
  throw std::invalid_argument("candidate signal update mode is unknown");
}
std::string transition(core::IoTransitionCapability value) {
  switch (value) {
    case core::IoTransitionCapability::kSampledOnly: return "sampled_only";
    case core::IoTransitionCapability::kCaptured: return "captured";
  }
  throw std::invalid_argument("candidate signal transition capability is unknown");
}
V liveness(const std::optional<core::IoSignalLivenessMode>& value) {
  if (!value) return V(nullptr);
  switch (*value) {
    case core::IoSignalLivenessMode::kInProcess: return V("in_process");
    case core::IoSignalLivenessMode::kPeriodic: return V("periodic");
    case core::IoSignalLivenessMode::kChangeOnly: return V("change_only");
  }
  throw std::invalid_argument("candidate signal liveness is unknown");
}

// This is the same retained group/source-order and non-mimic resource join as
// SourceModelProjection's commandable_source_joint_names; no runtime state.
V source_joint_names(const Target& selected, const VerifiedRetainedModelMap& map) {
  std::set<std::pair<core::ModelInstanceId, std::string>> commandable;
  for (const auto& binding : map.control_bindings()) {
    if (binding.controllable_component_id != selected.component_id ||
        binding.command_target_id != selected.command_target_id) continue;
    for (const auto& resource : binding.resources)
      for (const auto& coordinate : resource.coordinates)
        if (!coordinate.derived_mimic)
          commandable.emplace(coordinate.model_instance_id, coordinate.source_joint_name);
  }
  V::Array result;
  std::set<std::string> seen;
  const auto* target = map.find_target(selected.component_id, selected.command_target_id);
  if (!target) return V(std::move(result));
  for (const auto& group : target->model_groups) {
    for (const auto& compiled : group.expanded_compiled_joint_names) {
      const OccurrenceSymbolRewrite* match = nullptr;
      for (const auto& symbol : map.symbols()) {
        if (symbol.source.model_instance_id != group.model_instance_id ||
            symbol.source.kind != OccurrenceSymbolKind::kJoint || symbol.compiled_name != compiled)
          continue;
        if (match) throw std::invalid_argument("candidate joint symbol is ambiguous");
        match = &symbol;
      }
      if (!match) throw std::invalid_argument("candidate joint symbol is absent");
      const auto& name = match->source.source_name;
      if (commandable.contains({group.model_instance_id, name}) && seen.insert(name).second)
        result.emplace_back(name);
    }
  }
  return V(std::move(result));
}
V stop_routes(const Target& target, const PreparedActivationMaterialization& candidate) {
  const auto& graph = candidate.bundle->value().cell_graph;
  std::vector<const core::ControlBindingDTO*> bindings;
  for (const auto& binding : graph.control_bindings)
    if (binding.controllable_component_id == target.component_id &&
        binding.command_target_id == target.command_target_id)
      bindings.push_back(&binding);
  std::sort(bindings.begin(), bindings.end(), [](const auto* a, const auto* b) {
    return std::tie(a->control_binding_id, a->control_binding_revision) <
           std::tie(b->control_binding_id, b->control_binding_revision);
  });
  V::Array routes;
  for (const auto* binding : bindings) {
    const core::ControlDomainDTO* domain = nullptr;
    for (const auto& value : graph.control_domains) {
      if (value.control_domain_id != binding->control_domain_id) continue;
      if (domain) throw std::invalid_argument("candidate control domain is ambiguous");
      domain = &value;
    }
    if (!domain) throw std::invalid_argument("candidate control domain is absent");
    const auto desired = qualified_desired_control_route(candidate.bundle->value(), *domain, *binding);
    const auto rows = read_qualified_io_stop_stage_rows(*candidate.bundle, *candidate.model_map,
        binding->control_binding_id, binding->control_binding_revision, desired.second,
        candidate.raw_artifacts);
    V::Array stages;
    V reason(nullptr);
    if (rows.ok()) {
      for (const auto& stage : rows.value()) {
        stages.emplace_back(V::Object{
            {"stage_id", V(stage.stop_escalation_stage_id.value())},
            {"stage_kind", V(std::string(core::to_string(stage.stage_kind)))},
            {"command_kind", V(std::string(core::to_string(stage.command_kind)))},
            {"stop_action_class", V(std::string(core::to_string(stage.stop_action_class)))}});
      }
    } else {
      V::Object details;
      for (const auto& [key, value] : rows.error().details) details.emplace_back(key, V(value));
      reason = V(V::Object{{"code", V(rows.error().code)}, {"message", V(rows.error().message)},
                           {"details", V(std::move(details))}});
    }
    routes.emplace_back(V::Object{{"capability_id", V(binding->capability_id.value())},
        {"control_binding_id", V(binding->control_binding_id.value())},
        {"control_binding_revision", V(binding->control_binding_revision.value())},
        {"control_route_id", V(desired.second.value())}, {"available", V(rows.ok())},
        {"reason", std::move(reason)}, {"stages", V(std::move(stages))}});
  }
  return V(std::move(routes));
}
}  // namespace

CanonicalValue prepared_target_default_tcp(
    const VerifiedRetainedModelMap& model,
    const core::ProgramTargetReferenceDTO& target) {
  const auto* authored = model.kinematic_index().default_tcp_of(target.component_id);
  if (!authored) return V(nullptr);
  const auto* selected = model.find_target(target.component_id, authored->command_target_id);
  if (!selected) throw std::invalid_argument("candidate default TCP target is absent");
  const auto* mapped = find_target_frame_role(*selected, authored->frame_role_id);
  const auto* frame = mapped ? std::get_if<CompositionComponentModelFrame>(&mapped->frame) : nullptr;
  if (!frame || !model.kinematic_index().carries(target.component_id, frame->controllable_component_id))
    throw std::invalid_argument("candidate default TCP is absent or unmounted");
  return V(V::Object{
      {"target", target_value({target.component_id, authored->command_target_id})},
      {"frame", frame_value({frame->controllable_component_id, authored->frame_role_id})}});
}

CanonicalValue prepared_selector_facts(const PreparedActivationMaterialization& candidate) {
  if (!candidate.bundle || !candidate.model_map)
    throw std::invalid_argument("candidate has no qualified retained model map");
  const auto& bundle = *candidate.bundle;
  const auto& model = *candidate.model_map;
  const PreparedCatalogSource source(candidate.catalog_artifacts);
  V::Array components;
  V::Array targets;
  struct TargetFacts { Target target; V capabilities; V joints; bool motion; };
  std::vector<TargetFacts> target_facts;
  std::vector<const core::ControllableComponentDTO*> ordered_components;
  for (const auto& component : bundle.value().cell_graph.controllable_components)
    ordered_components.push_back(&component);
  std::sort(ordered_components.begin(), ordered_components.end(), [](const auto* a, const auto* b) {
    return a->controllable_component_id < b->controllable_component_id;
  });
  for (const auto* component_ptr : ordered_components) {
    const auto& component = *component_ptr;
    components.emplace_back(V::Object{{"component_id", V(component.controllable_component_id.value())}});
    auto type = qualified_program_component_type(component.controllable_component_id, bundle, source);
    std::sort(type.command_targets.begin(), type.command_targets.end(), [](const auto& a, const auto& b) {
      return a.command_target_id < b.command_target_id;
    });
    for (auto& selected : type.command_targets) {
      const Target target{component.controllable_component_id, selected.command_target_id};
      std::sort(selected.capabilities.begin(), selected.capabilities.end(), [](const auto& a, const auto& b) {
        return a.capability_id < b.capability_id;
      });
      std::vector<core::ProgramCapabilityRequirementDTO> requirements;
      for (const auto& capability : selected.capabilities)
        requirements.push_back({capability.capability_id, capability.capability_definition_revision,
                                core::ProgramCapabilityDisposition::kRequired});
      const auto statuses = probe_program_target_capabilities(target.component_id, target.command_target_id,
                                                               requirements, bundle, model, source);
      if (statuses.size() != requirements.size())
        throw std::logic_error("candidate capability probe size mismatch");
      V::Array capabilities;
      bool motion = false;
      for (std::size_t i = 0; i < requirements.size(); ++i) {
        if (requirements[i].capability_id.value() == "execution.trajectory.execute" && !statuses[i])
          motion = true;
        capabilities.emplace_back(V::Object{
            {"capability_id", V(requirements[i].capability_id.value())},
            {"available", V(!statuses[i].has_value())},
            {"reason", statuses[i] ? V(std::string(to_string(*statuses[i]))) : V(nullptr)}});
      }
      target_facts.push_back({target, V(std::move(capabilities)), source_joint_names(target, model), motion});
    }
  }

  for (auto& fact : target_facts) {
    V::Array tcps;
    if (fact.motion) {
      const auto inventory = qualified_candidate_robot_tcps(fact.target, bundle, model, source);
      for (const auto& tcp : inventory) {
        const auto& frame = tcp.entry.frame;
        const bool here = model.kinematic_index().carries(fact.target.component_id, frame.component_id);
        const bool elsewhere = !here && std::any_of(target_facts.begin(), target_facts.end(), [&](const auto& other) {
          return other.motion && other.target.component_id != fact.target.component_id &&
                 model.kinematic_index().carries(other.target.component_id, frame.component_id);
        });
        tcps.emplace_back(V::Object{{"entry", V(V::Object{{"target", target_value(tcp.entry.target)},
                {"frame", frame_value(tcp.entry.frame)}})},
            {"end_effector", V(tcp.end_effector)}, {"component_default", V(tcp.component_default)},
            {"mounted_here", V(here)}, {"mounted_elsewhere", V(elsewhere)}, {"available", V(here)},
            {"reason", here ? V(nullptr) : V("tcp_tool_not_mounted")}});
      }
    }
    auto default_tcp = prepared_target_default_tcp(model, fact.target);
    targets.emplace_back(V::Object{{"target", target_value(fact.target)},
        {"capabilities", std::move(fact.capabilities)}, {"source_joint_names", std::move(fact.joints)},
        {"tcp_entries", V(std::move(tcps))}, {"default_tcp", std::move(default_tcp)},
        {"stop_routes", stop_routes(fact.target, candidate)}});
  }

  const auto io = ResolveCompiledIoSignalCatalog(bundle.value().cell_graph);
  if (!io.ok()) throw std::invalid_argument("candidate I/O catalog is invalid: " + io.error().message);
  std::set<Signal> signal_refs;
  for (const auto& row : io.value()) {
    const auto& id = row.descriptor.signal_id;
    signal_refs.insert({id.controllable_component_id, id.name});
    if (core::HoldsChangedOutside(row.descriptor))
      signal_refs.insert({id.controllable_component_id, id.name + std::string(core::kChangedOutsideSignalSuffix)});
  }
  V::Array signals;
  for (const auto& ref : signal_refs) {
    const auto matches = FindProgramSignalMatches(io.value(), ref);
    if (matches.empty()) throw std::logic_error("candidate signal disappeared during projection");
    const bool ambiguous = matches.size() != 1U;
    V target(nullptr), value_type(nullptr), dir(nullptr), mode(nullptr), transition_capability(nullptr), live(nullptr), provider(nullptr);
    if (!ambiguous) {
      const auto& match = matches.front();
      if (match.descriptor.value_type != core::IoSignalValueType::kBoolean)
        throw std::invalid_argument("candidate signal type is unsupported");
      target = target_value({ref.component_id, match.retained->command_target_id});
      value_type = V("bool");
      dir = V(direction(match.descriptor.direction));
      mode = V(update_mode(match.descriptor.update_mode));
      transition_capability = V(transition(match.descriptor.transition_capability));
      live = liveness(match.descriptor.liveness);
      if (!match.changed_outside) provider = V(match.retained->provider.profile);
    }
    signals.emplace_back(V::Object{{"reference", signal_value(ref)}, {"ambiguous", V(ambiguous)},
        {"target", std::move(target)}, {"value_type", std::move(value_type)}, {"direction", std::move(dir)},
        {"update_mode", std::move(mode)}, {"transition_capability", std::move(transition_capability)},
        {"liveness", std::move(live)}, {"provider_profile", std::move(provider)}});
  }

  std::set<Frame> frame_aliases;
  for (const auto& target : model.targets())
    for (const auto& row : target.frames)
      if (const auto* frame = std::get_if<CompositionComponentModelFrame>(&row.frame);
          frame && frame->controllable_component_id == target.controllable_component_id)
        frame_aliases.insert({target.controllable_component_id, row.frame_role_id});
  std::set<Frame> model_frames;
  for (const auto& path : model.frame_paths()) {
    if (const auto* frame = std::get_if<CompositionComponentModelFrame>(&path.terminal_frame))
      model_frames.insert({frame->controllable_component_id, frame->frame_role_id});
  }
  std::vector<Frame> frame_refs(frame_aliases.begin(), frame_aliases.end());
  for (const auto& frame : model_frames)
    if (!frame_aliases.contains(frame)) frame_refs.push_back(frame);
  V::Array frames;
  for (const auto& ref : frame_refs) {
    const auto matches = find_program_frame_matches(ref, model);
    const bool ambiguous = matches.size() != 1U;
    std::set<Target> moving;
    if (!ambiguous) {
      for (const auto& target : model.targets()) {
        if (!model.kinematic_index().carries(target.controllable_component_id, ref.component_id)) continue;
        for (const auto& row : target.frames) {
          const auto* frame = std::get_if<CompositionComponentModelFrame>(&row.frame);
          if (frame && frame->controllable_component_id == ref.component_id &&
              frame->model_instance_id == matches.front().model_instance_id && frame->model_revision == matches.front().model_revision &&
              frame->compiled_frame_name == matches.front().compiled_frame_name)
            moving.insert({target.controllable_component_id, target.command_target_id});
        }
      }
    }
    frames.emplace_back(V::Object{{"reference", frame_value(ref)}, {"ambiguous", V(ambiguous)},
                                  {"moving_targets", targets_value(moving)}});
  }
  std::set<Body> body_aliases;
  for (const auto& target : model.targets())
    for (const auto& body : target.collision_bodies)
      body_aliases.insert({target.controllable_component_id, body.collision_body_role_id});
  std::set<Body> model_bodies;
  for (const auto& body : model.model_collision_bodies())
    if (body.owner_component_id)
      model_bodies.insert({*body.owner_component_id, body.collision_body_role_id});
  std::vector<Body> body_refs(body_aliases.begin(), body_aliases.end());
  for (const auto& body : model_bodies)
    if (!body_aliases.contains(body)) body_refs.push_back(body);
  V::Array bodies;
  for (const auto& ref : body_refs) {
    const auto matches = find_program_collision_body_matches(ref, model);
    const bool ambiguous = matches.size() != 1U;
    std::set<Target> moving;
    if (!ambiguous) {
      for (const auto& target : model.targets())
        for (const auto& body : target.collision_bodies)
          if (model.kinematic_index().carries(target.controllable_component_id, ref.component_id) &&
              body.model_instance_id == matches.front().model_instance_id &&
              !matches.front().compiled_collision_body_names.empty() &&
              std::all_of(matches.front().compiled_collision_body_names.begin(),
                          matches.front().compiled_collision_body_names.end(),
                          [&](const auto& name) {
                            return std::find(body.compiled_link_names.begin(),
                                             body.compiled_link_names.end(), name) !=
                                   body.compiled_link_names.end();
                          }))
            moving.insert({target.controllable_component_id, target.command_target_id});
    }
    bodies.emplace_back(V::Object{{"reference", body_value(ref)}, {"ambiguous", V(ambiguous)},
                                  {"moving_targets", targets_value(moving)}});
  }
  return V(V::Object{{"components", V(std::move(components))}, {"targets", V(std::move(targets))},
                     {"signals", V(std::move(signals))}, {"frames", V(std::move(frames))},
                     {"bodies", V(std::move(bodies))}});
}
}  // namespace manyforge::deployment::detail
