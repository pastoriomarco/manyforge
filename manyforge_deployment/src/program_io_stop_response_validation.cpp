#include <manyforge_deployment/program_io_stop_response_validation.hpp>

#include <algorithm>
#include <manyforge_core/io_rule_stop_response.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <map>
#include <set>
#include <stdexcept>

#include "control_domain_control_internal.hpp"

namespace manyforge::deployment {
namespace {
class Invalid final : public std::runtime_error {
public:
  explicit Invalid(std::string reason)
      : std::runtime_error(std::move(reason)) {}
};
const CanonicalValue &member(const CanonicalValue &value,
                             std::string_view name) {
  const auto *found = value.find_object_member(name);
  if (!found)
    throw Invalid("io_rule_stop_declaration_invalid");
  return *found;
}
void exact_inputs(const ProgramBindingContract &program,
                  const ProgramBinding &binding, const RetainedBundle &bundle) {
  const auto &bound = binding.value();
  if (bound.program_id != program.value().program_id ||
      bound.program_revision != program.value().program_revision ||
      bound.node_environment != program.value().node_environment ||
      bound.retained_bundle_id != bundle.retained_bundle_id() ||
      bound.graph_revision != bundle.value().cell_graph.graph_revision)
    throw Invalid("program_capture_product_mismatch");
}

template <class Visitor>
void nodes(const CanonicalValue &node, const Visitor &visit) {
  const auto &id = member(node, "node_id").as_string();
  if (id == "motion.move_until_signal" || id == "motion.probe_search") {
    const auto &parameters = member(node, "parameters");
    if (parameters.find_object_member("interrupt_policy_id"))
      throw Invalid("interrupt_node_policy_reference_refused");
    if (parameters.find_object_member("fallback_outcome"))
      throw Invalid("interrupt_node_fallback_outcome_refused");
    if (const auto *sources = node.find_object_member("value_sources"))
      for (const auto &[path, unused] : sources->as_object()) {
        (void)unused;
        for (const std::string_view literal :
             {"interrupt_condition", "target_role", "activation_mode",
              "stop_mode", "priority", "stop_entry_stage_id",
              "success_through_stage_id", "fallback_outcome"})
          if (path == literal || path.starts_with(std::string(literal) + ".") ||
              path.starts_with(std::string(literal) + "["))
            throw Invalid("io_rule_stop_selection_requires_literal");
      }
    visit(node);
  }
  for (const auto &child : member(node, "children").as_array())
    nodes(child, visit);
}

const core::ControlBindingDTO &node_binding(const CanonicalValue &node,
                                            const ProgramBinding &binding,
                                            const RetainedBundle &bundle) {
  const auto &parameters = member(node, "parameters");
  const auto &role = member(parameters, "target_role").as_string();
  const core::ProgramTargetRoleBindingDTO *target = nullptr;
  for (const auto &row : binding.value().target_role_bindings)
    if (row.target_role_id.value() == role) {
      if (target)
        throw Invalid("io_rule_stop_target_ambiguous");
      target = &row;
    }
  if (!target)
    throw Invalid("io_rule_stop_target_unbound");
  const core::ProgramControlBindingRefDTO *selected = nullptr;
  for (const auto &capability : target->capability_bindings)
    if (capability.capability_id.value() == "execution.trajectory.execute" &&
        capability.control_binding) {
      if (selected && *selected != *capability.control_binding)
        throw Invalid("io_rule_stop_target_ambiguous");
      selected = &*capability.control_binding;
    }
  if (!selected)
    throw Invalid("io_rule_stop_target_unbound");
  const auto &graph = bundle.value().cell_graph;
  const auto found = std::find_if(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [&](const auto &row) {
        return row.control_binding_id == selected->control_binding_id &&
               row.control_binding_revision ==
                   selected->control_binding_revision;
      });
  if (found == graph.control_bindings.end() ||
      found->controllable_component_id != target->controllable_component_id ||
      found->command_target_id != target->command_target_id)
    throw Invalid("io_rule_stop_target_unbound");
  return *found;
}

const core::RetainedProductDTO &product(const RetainedBundle &bundle,
                                        const core::ControlDomainId &domain) {
  const core::RetainedProductDTO *selected = nullptr;
  for (const auto &group :
       bundle.value().compiled_products.control_domain_products)
    if (group.control_domain_id == domain)
      for (const auto &candidate : group.products)
        if (candidate.product_kind ==
            core::RetainedProductKind::kControlDomainControl) {
          if (selected)
            throw Invalid("io_rule_stop_product_ambiguous");
          selected = &candidate;
        }
  if (!selected)
    throw Invalid("io_rule_stop_product_missing");
  return *selected;
}
const core::RetainedArtifactRefDTO &
control_ref(const core::RetainedProductDTO &product) {
  const core::RetainedArtifactRefDTO *selected = nullptr;
  for (const auto &ref : product.artifact_refs)
    if (ref.artifact_role_id.value() ==
        detail::kControlDomainControlArtifactRole) {
      if (selected)
        throw Invalid("io_rule_stop_product_ambiguous");
      selected = &ref;
    }
  if (!selected)
    throw Invalid("io_rule_stop_product_missing");
  return *selected;
}

core::ControlRouteId desired_route(const core::ControlBindingDTO &binding,
                                   const RetainedBundle &bundle) {
  const auto &domains = bundle.value().cell_graph.control_domains;
  const auto domain =
      std::find_if(domains.begin(), domains.end(), [&](const auto &row) {
        return row.control_domain_id == binding.control_domain_id;
      });
  if (domain == domains.end())
    throw Invalid("io_rule_stop_route_missing");
  const core::DesiredControlBindingRouteDTO *selected = nullptr;
  for (const auto &configuration :
       bundle.value().desired_control_configurations)
    if (configuration.control_domain_id == binding.control_domain_id &&
        configuration.control_domain_revision ==
            domain->control_domain_revision)
      for (const auto &route : configuration.binding_routes)
        if (route.control_binding_id == binding.control_binding_id &&
            route.control_binding_revision ==
                binding.control_binding_revision) {
          if (selected)
            throw Invalid("io_rule_stop_route_ambiguous");
          selected = &route;
        }
  if (!selected)
    throw Invalid("io_rule_stop_route_missing");
  return selected->control_route_id;
}

std::vector<core::IoRuleStopEntryRowDTO>
entry_rows(const CanonicalValue &stop) {
  std::vector<core::IoRuleStopEntryRowDTO> rows;
  for (const auto &stage : member(stop, "escalation_stages").as_array()) {
    const auto &kind = member(stage, "stage_kind").as_string();
    const auto stage_kind =
        kind == "nominal" ? core::StopEscalationStageKind::kNominal
        : kind == "controlled_fallback"
            ? core::StopEscalationStageKind::kControlledFallback
        : kind == "software_hard" ? core::StopEscalationStageKind::kSoftwareHard
        : kind == "safety_system_request"
            ? core::StopEscalationStageKind::kSafetySystemRequest
            : throw Invalid("io_rule_stop_declaration_invalid");
    const auto &command = member(stage, "containment_command");
    const auto &token = member(command, "command_kind").as_string();
    const auto command_kind =
        token == "motion_controlled_stop"
            ? core::StopContainmentCommandKind::kMotionControlledStop
        : token == "controller_goal_cancel"
            ? core::StopContainmentCommandKind::kControllerGoalCancel
        : token == "registered"
            ? core::StopContainmentCommandKind::kRegistered
            : throw Invalid("io_rule_stop_declaration_invalid");
    auto action = core::StopActionClass::kControlled;
    if (command_kind == core::StopContainmentCommandKind::kRegistered) {
      const auto *definition = core::find_registered_stop_command_definition(
          core::RegisteredStopCommandKindId(
              member(command, "registered_command_kind_id").as_string()));
      if (!definition)
        throw Invalid("io_rule_stop_declaration_invalid");
      action = definition->stop_action_class;
    }
    rows.push_back({core::StopEscalationStageId(
                        member(stage, "stop_escalation_stage_id").as_string()),
                    stage_kind, command_kind, action});
  }
  return rows;
}
const core::ControlBindingDTO &
qualified_binding(const RetainedBundle &bundle,
                  const core::ControlBindingId &binding_id,
                  const core::ControlBindingRevision &revision) {
  const auto &bindings = bundle.value().cell_graph.control_bindings;
  const auto found =
      std::find_if(bindings.begin(), bindings.end(), [&](const auto &row) {
        return row.control_binding_id == binding_id &&
               row.control_binding_revision == revision;
      });
  if (!verify_retained_bundle_identity(bundle) || found == bindings.end() ||
      std::count_if(bindings.begin(), bindings.end(), [&](const auto &row) {
        return row.control_binding_id == binding_id;
      }) != 1)
    throw Invalid("io_rule_stop_target_unbound");
  return *found;
}

CanonicalValue verified_control(const RetainedBundle &bundle,
                                const VerifiedRetainedModelMap &model_map,
                                const core::ControlBindingDTO &bound,
                                const PreparedActivationArtifact &loaded) {
  if (model_map.query().retained_bundle_id != bundle.retained_bundle_id())
    throw Invalid("io_rule_stop_model_map_mismatch");
  const auto &ref = control_ref(product(bundle, bound.control_domain_id));
  if (loaded.content_addressed_store_key != ref.content_addressed_store_key ||
      loaded.content_digest != ref.content_digest ||
      loaded.bytes.size() != ref.byte_size ||
      sha256_digest(loaded.bytes) != ref.content_digest.value())
    throw Invalid("io_rule_stop_product_missing");
  detail::require_control_domain_control_matches_retained_bundle(
      loaded.bytes, bundle.value(), bound.control_domain_id, {}, {}, {},
      model_map.control_bindings());
  return detail::parse_and_verify_control_domain_control(
      loaded.bytes, &bundle.value().cell_graph, model_map.control_bindings());
}

const CanonicalValue &qualified_stop_row(const CanonicalValue &control,
                                         const core::ControlBindingDTO &bound,
                                         const core::ControlRouteId &route) {
  const CanonicalValue *stop = nullptr;
  for (const auto &row : member(control, "stop_applicability").as_array())
    if (member(row, "control_binding_id").as_string() ==
            bound.control_binding_id.value() &&
        member(row, "control_binding_revision").as_string() ==
            bound.control_binding_revision.value() &&
        member(row, "control_route_id").as_string() == route.value()) {
      if (stop)
        throw Invalid("io_rule_stop_route_ambiguous");
      stop = &row;
    }
  if (!stop || member(*stop, "disposition").as_string() != "stoppable")
    throw Invalid("io_rule_stop_route_missing");
  return *stop;
}

core::ErrorDTO invalid(const std::exception &error) {
  return {"invalid_request",
          "Program I/O Stop response is not qualified",
          {{"reason", error.what()}}};
}
} // namespace

core::Result<std::vector<core::RetainedArtifactRefDTO>>
required_program_io_stop_artifacts(const ProgramBindingContract &program,
                                   const ProgramBinding &binding,
                                   const RetainedBundle &bundle) {
  using Reply = core::Result<std::vector<core::RetainedArtifactRefDTO>>;
  try {
    exact_inputs(program, binding, bundle);
    std::map<core::ContentAddressedStoreKey, core::RetainedArtifactRefDTO> refs;
    nodes(member(program.normalized_program_root(), "tree"),
          [&](const auto &node) {
            const auto &bound = node_binding(node, binding, bundle);
            const auto &ref =
                control_ref(product(bundle, bound.control_domain_id));
            const auto [found, inserted] =
                refs.emplace(ref.content_addressed_store_key, ref);
            if (!inserted && found->second != ref)
              throw Invalid("io_rule_stop_product_ambiguous");
          });
    std::vector<core::RetainedArtifactRefDTO> result;
    for (const auto &[key, ref] : refs) {
      (void)key;
      result.push_back(ref);
    }
    return Reply::Ok(std::move(result));
  } catch (const std::exception &error) {
    return Reply::Err(invalid(error));
  }
}

core::Result<core::RetainedArtifactRefDTO>
required_io_stop_artifact_for_binding(
    const RetainedBundle &bundle, const core::ControlBindingId &binding_id,
    const core::ControlBindingRevision &binding_revision) {
  using Reply = core::Result<core::RetainedArtifactRefDTO>;
  try {
    const auto &bound = qualified_binding(bundle, binding_id, binding_revision);
    return Reply::Ok(control_ref(product(bundle, bound.control_domain_id)));
  } catch (const std::exception &error) {
    return Reply::Err(invalid(error));
  }
}

core::Result<std::vector<core::IoRuleStopEntryRowDTO>>
read_qualified_io_stop_stage_rows(
    const RetainedBundle &bundle, const VerifiedRetainedModelMap &model_map,
    const core::ControlBindingId &binding_id,
    const core::ControlBindingRevision &binding_revision,
    const core::ControlRouteId &route_id,
    std::span<const PreparedActivationArtifact> raw_artifacts) {
  using Reply = core::Result<std::vector<core::IoRuleStopEntryRowDTO>>;
  try {
    const auto &bound = qualified_binding(bundle, binding_id, binding_revision);
    if (std::find(bound.legal_control_route_ids.begin(),
                  bound.legal_control_route_ids.end(),
                  route_id) == bound.legal_control_route_ids.end())
      throw Invalid("io_rule_stop_route_missing");
    const auto &ref = control_ref(product(bundle, bound.control_domain_id));
    const PreparedActivationArtifact *loaded = nullptr;
    for (const auto &candidate : raw_artifacts)
      if (candidate.content_addressed_store_key ==
          ref.content_addressed_store_key) {
        if (loaded)
          throw Invalid("io_rule_stop_product_ambiguous");
        loaded = &candidate;
      }
    if (!loaded)
      throw Invalid("io_rule_stop_product_missing");
    const auto control = verified_control(bundle, model_map, bound, *loaded);
    return Reply::Ok(entry_rows(qualified_stop_row(control, bound, route_id)));
  } catch (const std::exception &error) {
    return Reply::Err(invalid(error));
  }
}

core::Result<CanonicalValue> read_qualified_io_stop_response_metadata(
    const RetainedBundle &bundle, const VerifiedRetainedModelMap &model_map,
    const core::ControlBindingId &binding_id,
    const core::ControlBindingRevision &binding_revision,
    const core::ControlRouteId &route_id,
    const IVerifiedRetainedProductArtifactSource &artifact_source) {
  using Reply = core::Result<CanonicalValue>;
  try {
    const auto &bound = qualified_binding(bundle, binding_id, binding_revision);
    if (std::find(bound.legal_control_route_ids.begin(),
                  bound.legal_control_route_ids.end(),
                  route_id) == bound.legal_control_route_ids.end())
      throw Invalid("io_rule_stop_route_missing");
    const auto &compiled = product(bundle, bound.control_domain_id);
    const auto &ref = control_ref(compiled);
    const RetainedProductArtifactQuery query{
        bundle.retained_bundle_id(), compiled.product_id, compiled.product_kind,
        ref.artifact_role_id};
    const auto loaded = artifact_source.get_verified(query);
    if (!loaded || loaded->query() != query || loaded->artifact_ref() != ref ||
        loaded->product_contract_id() != compiled.product_contract_id ||
        loaded->product_contract_revision() !=
            compiled.product_contract_revision)
      throw Invalid("io_rule_stop_product_missing");
    const auto control =
        verified_control(bundle, model_map, bound,
                         {ref.content_addressed_store_key, ref.content_digest,
                          std::string(loaded->bytes())});
    const auto &stop = qualified_stop_row(control, bound, route_id);
    CanonicalValue::Array stages;
    for (const auto &row : member(stop, "escalation_stages").as_array())
      stages.emplace_back(CanonicalValue::Object{
          {"stage_id", member(row, "stop_escalation_stage_id")},
          {"stage_kind", member(row, "stage_kind")},
          {"command_kind",
           member(member(row, "containment_command"), "command_kind")}});
    CanonicalValue nominal(nullptr);
    const auto &chain = member(stop, "escalation_stages").as_array();
    if (!chain.empty())
      if (const auto *defaults =
              chain.front().find_object_member("default_contract_parameters");
          defaults && defaults->is_object())
        if (const auto *mode = defaults->find_object_member("stop_mode"))
          nominal = *mode;
    return Reply::Ok(CanonicalValue(CanonicalValue::Object{
        {"control_binding_id", CanonicalValue(binding_id.value())},
        {"control_binding_revision", CanonicalValue(binding_revision.value())},
        {"control_route_id", CanonicalValue(route_id.value())},
        {"nominal_stop_mode", std::move(nominal)},
        {"stages", CanonicalValue(std::move(stages))}}));
  } catch (const std::exception &error) {
    return Reply::Err(invalid(error));
  }
}

core::Result<void> validate_program_io_stop_responses(
    const ProgramBindingContract &program, const ProgramBinding &binding,
    const RetainedBundle &bundle, const VerifiedRetainedModelMap &model_map,
    std::span<const PreparedActivationArtifact> raw_artifacts) {
  try {
    exact_inputs(program, binding, bundle);
    if (model_map.query().retained_bundle_id != bundle.retained_bundle_id())
      throw Invalid("io_rule_stop_model_map_mismatch");
    std::map<core::ControlDomainId, CanonicalValue> controls;
    const auto refs =
        required_program_io_stop_artifacts(program, binding, bundle);
    if (!refs.ok())
      return core::Result<void>::Err(refs.error());
    nodes(member(program.normalized_program_root(), "tree"), [&](const auto
                                                                     &node) {
      const auto &bound = node_binding(node, binding, bundle);
      auto found = controls.find(bound.control_domain_id);
      if (found == controls.end()) {
        const auto &ref = control_ref(product(bundle, bound.control_domain_id));
        const PreparedActivationArtifact *loaded = nullptr;
        for (const auto &artifact : raw_artifacts)
          if (artifact.content_addressed_store_key ==
              ref.content_addressed_store_key) {
            if (loaded)
              throw Invalid("io_rule_stop_product_ambiguous");
            loaded = &artifact;
          }
        if (!loaded)
          throw Invalid("io_rule_stop_product_missing");
        found =
            controls
                .emplace(bound.control_domain_id,
                         verified_control(bundle, model_map, bound, *loaded))
                .first;
      }
      const auto route = desired_route(bound, bundle);
      const auto *stop = &qualified_stop_row(found->second, bound, route);
      const auto rows = entry_rows(*stop);
      const auto &parameters = member(node, "parameters");
      std::optional<core::StopEscalationStageId> entry;
      if (const auto *id = parameters.find_object_member("stop_entry_stage_id"))
        entry.emplace(id->as_string());
      std::optional<core::StopEscalationStageId> success_through;
      if (const auto *id =
              parameters.find_object_member("success_through_stage_id"))
        success_through.emplace(id->as_string());
      const auto selection =
          core::ResolveIoRuleSuccessThroughIndex(rows, entry, success_through);
      if (!selection.ok())
        throw Invalid(selection.error().details.at("reason"));
      core::MotionInterruptStopProfileDTO requested;
      const auto &mode = member(parameters, "stop_mode").as_string();
      if (mode == "soft_on_path")
        requested.stop_mode = core::MotionInterruptStopMode::kSoftOnPath;
      else if (mode == "spring_back_hold")
        requested.stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
      else if (mode == "hard_deceleration")
        requested.stop_mode = core::MotionInterruptStopMode::kHardDeceleration;
      else
        throw Invalid("io_rule_nominal_mode_mismatch");
      const auto &stages = member(*stop, "escalation_stages").as_array();
      // Exact saved nominal mode only. This does not invent/copy a full node
      // Stop profile or reinterpret a fallback stage as its nominal profile.
      const auto &defaults =
          member(stages.front(), "default_contract_parameters");
      const core::JsonObjectDTO actual{
          {"stop_mode",
           core::JsonValueDTO{member(defaults, "stop_mode").as_string()}}};
      const auto qualified =
          core::ValidateIoRuleNominalStopMode(actual, requested);
      if (!qualified.ok())
        throw Invalid(qualified.error().details.at("reason"));
    });
    return core::Result<void>::Ok();
  } catch (const std::exception &error) {
    return core::Result<void>::Err(invalid(error));
  }
}
} // namespace manyforge::deployment
