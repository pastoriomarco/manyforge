#include <manyforge_deployment/program_reference_facts.hpp>

#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>

namespace manyforge::deployment {

std::set<core::ControllableComponentId> referenced_program_components(
    const core::ProgramBindingContractDTO& contract) {
  std::set<core::ControllableComponentId> components;
  for (const auto& row : contract.target_requirements) components.insert(row.target.component_id);
  for (const auto& row : contract.frame_requirements) components.insert(row.component_id);
  for (const auto& row : contract.signal_requirements) components.insert(row.component_id);
  for (const auto& row : contract.collision_body_requirements) components.insert(row.component_id);
  return components;
}

std::set<core::ProgramFrameReferenceDTO> program_component_frame_names(
    const std::set<core::ControllableComponentId>& components,
    const VerifiedRetainedModelMap& retained_model_map) {
  std::set<core::ProgramFrameReferenceDTO> names;
  for (const auto& target : retained_model_map.targets()) {
    if (!components.contains(target.controllable_component_id)) continue;
    for (const auto& row : target.frames) {
      const auto* frame = std::get_if<CompositionComponentModelFrame>(&row.frame);
      if (frame && frame->controllable_component_id == target.controllable_component_id)
        names.insert({target.controllable_component_id, row.frame_role_id});
    }
  }
  for (const auto& path : retained_model_map.frame_paths()) {
    const auto* frame = std::get_if<CompositionComponentModelFrame>(&path.terminal_frame);
    if (frame && components.contains(frame->controllable_component_id))
      names.insert({frame->controllable_component_id, frame->frame_role_id});
  }
  return names;
}

std::vector<core::ProgramFrameBindingDTO> find_program_frame_matches(
    const core::ProgramFrameReferenceDTO& reference,
    const VerifiedRetainedModelMap& retained_model_map) {
  std::vector<core::ProgramFrameBindingDTO> matches;
  const auto add_frame = [&](const CompositionComponentModelFrame& frame) {
    if (frame.controllable_component_id != reference.component_id) return;
    core::ProgramFrameBindingDTO row{reference, frame.model_instance_id,
                                     frame.model_revision, frame.compiled_frame_name};
    if (std::find(matches.begin(), matches.end(), row) == matches.end())
      matches.push_back(std::move(row));
  };
  // A component type's target-local frame name is an alias for its retained
  // physical frame. It need not equal the model's own frame ID.
  for (const auto& target : retained_model_map.targets()) {
    if (target.controllable_component_id != reference.component_id) continue;
    for (const auto& alias : target.frames) {
      if (alias.frame_role_id != reference.frame_id) continue;
      if (const auto* frame = std::get_if<CompositionComponentModelFrame>(&alias.frame))
        add_frame(*frame);
    }
  }
  // Model frame IDs also belong to their component, including components
  // whose model has no command target.
  for (const auto& path : retained_model_map.frame_paths()) {
    const auto* frame = std::get_if<CompositionComponentModelFrame>(&path.terminal_frame);
    if (!frame || frame->controllable_component_id != reference.component_id ||
        frame->frame_role_id != reference.frame_id) continue;
    add_frame(*frame);
  }
  std::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
    return std::tie(left.model_instance_id, left.model_revision, left.compiled_frame_name) <
           std::tie(right.model_instance_id, right.model_revision, right.compiled_frame_name);
  });
  return matches;
}

std::vector<core::ProgramCollisionBodyBindingDTO> find_program_collision_body_matches(
    const core::ProgramCollisionBodyReferenceDTO& reference,
    const VerifiedRetainedModelMap& retained_model_map) {
  std::vector<core::ProgramCollisionBodyBindingDTO> matches;
  const auto add_body = [&](const core::ModelInstanceId& instance,
                            const core::ModelRevision& revision,
                            const std::vector<std::string>& names) {
    core::ProgramCollisionBodyBindingDTO row{reference, instance, revision, names};
    if (std::find(matches.begin(), matches.end(), row) == matches.end())
      matches.push_back(std::move(row));
  };
  for (const auto& target : retained_model_map.targets()) {
    if (target.controllable_component_id != reference.component_id) continue;
    for (const auto& association : target.collision_bodies) {
      if (association.collision_body_role_id != reference.collision_body_id) continue;
      for (const auto& model : retained_model_map.model_occurrences()) {
        if (!model.owner_component_id || *model.owner_component_id != reference.component_id ||
            model.model_instance_id != association.model_instance_id) continue;
        add_body(model.model_instance_id, model.model_revision, association.compiled_link_names);
      }
    }
  }
  for (const auto& body : retained_model_map.model_collision_bodies()) {
    if (body.owner_component_id && *body.owner_component_id == reference.component_id &&
        body.collision_body_role_id == reference.collision_body_id)
      add_body(body.model_instance_id, body.model_revision, body.compiled_link_names);
  }
  std::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
    return std::tie(left.model_instance_id, left.model_revision, left.compiled_collision_body_names) <
           std::tie(right.model_instance_id, right.model_revision, right.compiled_collision_body_names);
  });
  return matches;
}

[[nodiscard]] CanonicalValue CaptureProgramFrameFacts(
    const core::ProgramBindingContractDTO &contract,
    const VerifiedRetainedModelMap &model_map,
    const std::string &record_digest, const std::string &binding_id,
    const std::string &program_revision, const std::string &bundle_id,
    const std::string &graph_revision) {
  const auto components = referenced_program_components(contract);
  CanonicalValue::Array component_rows;
  for (const auto &component : components) component_rows.emplace_back(component.value());
  CanonicalValue::Array frame_rows;
  for (const auto &reference : program_component_frame_names(components, model_map)) {
    const auto matches = find_program_frame_matches(reference, model_map);
    if (matches.empty()) continue;
    frame_rows.emplace_back(CanonicalValue::Object{
        {"reference", CanonicalValue(CanonicalValue::Object{
            {"component_id", CanonicalValue(reference.component_id.value())},
            {"frame_id", CanonicalValue(reference.frame_id.value())}})},
        {"match_count", CanonicalValue(matches.size() == 1U ? 1.0 : 2.0)}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"applied_record_digest", CanonicalValue(record_digest)},
      {"program_binding_id", CanonicalValue(binding_id)},
      {"program_revision", CanonicalValue(program_revision)},
      {"retained_bundle_id", CanonicalValue(bundle_id)},
      {"graph_revision", CanonicalValue(graph_revision)},
      {"components", CanonicalValue(std::move(component_rows))},
      {"frames", CanonicalValue(std::move(frame_rows))}});
}

}  // namespace manyforge::deployment
