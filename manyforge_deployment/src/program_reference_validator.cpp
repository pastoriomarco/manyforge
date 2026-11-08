#include <manyforge_deployment/program_reference_validator.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>

#include "program_io_condition_compiler_internal.hpp"

namespace manyforge::deployment {
namespace {

[[noreturn]] void invalid(const std::string &message) {
  throw ProgramContractError("reference validation: " + message);
}

const CanonicalValue &member(const CanonicalValue &value,
                             std::string_view key) {
  const auto *result =
      value.is_object() ? value.find_object_member(key) : nullptr;
  if (result == nullptr)
    invalid("missing " + std::string(key));
  return *result;
}

std::string pointer_token(std::string_view value) {
  std::string result;
  for (const char ch : value) {
    if (ch == '~')
      result += "~0";
    else if (ch == '/')
      result += "~1";
    else
      result += ch;
  }
  return result;
}

CanonicalValue text(std::string_view value) {
  return CanonicalValue(std::string(value));
}

class Validator final {
public:
  Validator(const RetainedBundle &bundle,
            const std::optional<VerifiedRetainedModelMap> &map,
            const ICatalogArtifactSource &catalog_source, bool draft)
      : bundle_(bundle), map_(map), source_(catalog_source), draft_(draft),
        catalog_(qualified_program_catalog(
            bundle.value().catalogs.node_environment)) {
    if (!verify_retained_bundle_identity(bundle_))
      invalid("invalid retained bundle");
    if (map_ &&
        map_->query().retained_bundle_id != bundle_.retained_bundle_id())
      invalid("model map belongs to another retained bundle");
    const auto catalog =
        ResolveCompiledIoSignalCatalog(bundle_.value().cell_graph);
    if (!catalog.ok())
      invalid(catalog.error().message);
    signals_ = catalog.value();
  }

  std::vector<ProgramReferenceDiagnostic> run(const CanonicalValue &program) {
    if (!program.is_object() || !member(program, "api_version").is_string() ||
        member(program, "api_version").as_string() != "manyforge.program.v3")
      invalid("expected a structurally validated v3 Program");
    program_ = &program;
    if (const auto *scene = program.find_object_member("scene")) {
      if (!scene->is_array())
        invalid("scene must be an array");
      for (const auto &row : scene->as_array()) {
        const auto &id = member(row, "scene_resource_id");
        if (!id.is_string())
          invalid("scene resource identity must be a string");
        scene_ids_.insert(id.as_string());
      }
    }
    if (const auto *forest = program.find_object_member("forest")) {
      if (!forest->is_array())
        invalid("forest must be an array");
      for (std::size_t i = 0; i < forest->as_array().size(); ++i)
        node(forest->as_array()[i], "/forest/" + std::to_string(i), 0);
    } else if (const auto *tree = program.find_object_member("tree")) {
      node(*tree, "/tree", 0);
    } else {
      invalid("missing Program tree or forest");
    }
    non_node_references(program);
    sort_and_deduplicate();
    return rows_;
  }

private:
  void sort_and_deduplicate() {
    struct OrderedRow final {
      ProgramReferenceDiagnostic row;
      std::string canonical_payload;
    };
    std::vector<OrderedRow> ordered;
    ordered.reserve(rows_.size());
    for (auto &row : rows_) {
      auto payload = canonical_json(row.payload);
      ordered.push_back({std::move(row), std::move(payload)});
    }
    const auto key = [](const OrderedRow &entry) {
      return std::tie(entry.row.location.path, entry.row.location.parameter,
                      entry.row.code, entry.canonical_payload,
                      entry.row.location.node_instance_id);
    };
    std::sort(ordered.begin(), ordered.end(), [&](const auto &left, const auto &right) {
      return key(left) < key(right);
    });
    rows_.clear();
    const OrderedRow *previous = nullptr;
    for (auto &entry : ordered) {
      if (previous != nullptr && key(*previous) == key(entry)) continue;
      // Keep the last emitted sort key intact until the next comparison.
      rows_.push_back(entry.row);
      previous = &entry;
    }
  }

  void emit(std::string code, const ProgramReferenceLocation &location,
            CanonicalValue::Object payload) {
    rows_.push_back(
        {std::move(code), location, CanonicalValue(std::move(payload))});
  }

  bool has_component(const std::string &component) const {
    const auto &components = bundle_.value().cell_graph.controllable_components;
    return std::any_of(
        components.begin(), components.end(), [&](const auto &row) {
          return row.controllable_component_id.value() == component;
        });
  }

  std::optional<ProgramBindingUnresolvedReason>
  target_status(const std::string &component, const std::string &target) {
    const auto key = std::pair{component, target};
    if (const auto found = target_status_.find(key);
        found != target_status_.end())
      return found->second;
    const auto result =
        probe_program_target(core::ControllableComponentId(component),
                             core::CommandTargetId(target), bundle_, source_);
    target_status_.emplace(key, result);
    return result;
  }

  std::optional<ProgramBindingUnresolvedReason>
  capability_status(const std::string &component, const std::string &target,
                    const core::ProgramCapabilityRequirementDTO &requirement) {
    const auto key =
        std::tuple{component, target, requirement.capability_id.value(),
                   requirement.capability_definition_revision.value()};
    if (const auto found = capability_status_.find(key);
        found != capability_status_.end())
      return found->second;
    if (!map_)
      invalid("target capability requires retained model map");
    const auto result = probe_program_target_capability(
        core::ControllableComponentId(component), core::CommandTargetId(target),
        requirement, bundle_, *map_, source_);
    capability_status_.emplace(key, result);
    return result;
  }

  const std::vector<ProgramSignalMatch> &signal_matches(
      const std::string &component, const std::string &name) {
    const auto key = std::pair{component, name};
    if (const auto found = signal_matches_.find(key); found != signal_matches_.end())
      return found->second;
    return signal_matches_
        .emplace(key, FindProgramSignalMatches(
                          signals_, {core::ControllableComponentId(component), name}))
        .first->second;
  }

  void require_input_signal(const CanonicalValue &value,
                            const ProgramReferenceLocation &location) {
    if (!value.is_object()) return;
    const auto *component = value.find_object_member("component_id");
    const auto *signal = value.find_object_member("signal_name");
    if (!component || !component->is_string() || !signal || !signal->is_string())
      return;
    const auto &matches = signal_matches(component->as_string(), signal->as_string());
    if (matches.size() != 1U) return;
    const auto &descriptor = matches.front().descriptor;
    if (descriptor.direction != core::IoSignalDirection::kInput)
      emit("signal_direction_mismatch", location,
           {{"component_id", text(component->as_string())},
            {"signal_name", text(signal->as_string())},
            {"expected_direction", text("input")},
            {"actual_direction", text("output")}});
  }

  void reference(const CanonicalValue *value, std::string_view kind,
                 const ProgramReferenceLocation &location) {
    const std::string field = kind == "target_reference"   ? "command_target_id"
                              : kind == "signal_reference" ? "signal_name"
                              : kind == "frame_reference"  ? "frame_id"
                                                          : "collision_body_id";
    if (value != nullptr && !value->is_object())
      invalid("reference must be an object");
    CanonicalValue::Array missing;
    if (value == nullptr || !value->is_object() ||
        value->find_object_member("component_id") == nullptr)
      missing.emplace_back("component_id");
    if (value == nullptr || !value->is_object() ||
        value->find_object_member(field) == nullptr)
      missing.emplace_back(field);
    if (!missing.empty()) {
      if (!draft_)
        invalid("required reference member is absent");
      emit("reference_required", location,
           {{"reference_kind", text(kind.substr(0, kind.size() - 10U))},
            {"missing_members", CanonicalValue(std::move(missing))}});
      return;
    }
    const auto &component_value = member(*value, "component_id");
    const auto &identity_value = member(*value, field);
    if (!component_value.is_string() || !identity_value.is_string())
      invalid("reference members must be strings");
    const auto &component = component_value.as_string();
    const auto &identity = identity_value.as_string();
    if (!has_component(component)) {
      emit("component_reference_missing", location,
           {{"component_id", text(component)}});
      return;
    }
    if (kind == "target_reference") {
      const auto result = target_status(component, identity);
      if (result == ProgramBindingUnresolvedReason::kTargetMissing)
        emit("command_target_missing", location,
             {{"component_id", text(component)},
              {"command_target_id", text(identity)}});
      return;
    }
    if (kind == "signal_reference") {
      std::vector<std::string> matches;
      for (const auto &match : signal_matches(component, identity))
        matches.push_back(match.retained->runtime_unit_id.value() + "/" +
                          match.retained->command_target_id.value());
      check_matches(kind, component, field, identity, location, matches,
                    "signal_missing");
      return;
    }
    std::vector<std::string> matches;
    CanonicalValue::Array qualified_candidates;
    if (!map_)
      invalid("frame or collision body requires retained model map");
    if (kind == "frame_reference") {
      for (const auto &frame : find_program_frame_matches(
               {core::ControllableComponentId(component), core::FrameRoleId(identity)}, *map_)) {
        matches.push_back(frame.model_instance_id.value());
        qualified_candidates.emplace_back(CanonicalValue::Object{
            {"reference", CanonicalValue(CanonicalValue::Object{
                 {"component_id", text(component)}, {"frame_id", text(identity)}})},
            {"model_instance_id", text(frame.model_instance_id.value())},
            {"model_revision", text(frame.model_revision.value())},
            {"compiled_frame_name", text(frame.compiled_frame_name)}});
      }
    } else {
      for (const auto &body : find_program_collision_body_matches(
               {core::ControllableComponentId(component), core::CollisionBodyRoleId(identity)}, *map_)) {
        matches.push_back(body.model_instance_id.value());
        CanonicalValue::Array names;
        for (const auto &name : body.compiled_collision_body_names) names.push_back(text(name));
        qualified_candidates.emplace_back(CanonicalValue::Object{
            {"reference", CanonicalValue(CanonicalValue::Object{
                 {"component_id", text(component)}, {"collision_body_id", text(identity)}})},
            {"model_instance_id", text(body.model_instance_id.value())},
            {"model_revision", text(body.model_revision.value())},
            {"compiled_collision_body_names", CanonicalValue(std::move(names))}});
      }
    }
    check_matches(kind, component, field, identity, location, matches,
                  kind == "frame_reference" ? "frame_missing"
                                            : "collision_body_missing",
                  std::move(qualified_candidates));
  }

  void condition(const CanonicalValue &value,
                 const ProgramReferenceLocation &location, std::size_t depth) {
    if (depth > 8U || !value.is_object() || value.as_object().size() != 1U)
      invalid("invalid interrupt condition");
    const auto &[op, operand] = value.as_object().front();
    if (op == "all" || op == "any") {
      if (!operand.is_array())
        invalid("invalid interrupt group");
      for (std::size_t i = 0; i < operand.as_array().size(); ++i) {
        auto nested = location;
        nested.path += "/" + op + "/" + std::to_string(i);
        condition(operand.as_array()[i], nested, depth + 1U);
      }
    } else if (op == "not") {
      auto nested = location;
      nested.path += "/not";
      condition(operand, nested, depth + 1U);
    } else if (op == "compare") {
      const auto &compared = member(operand, "operand");
      if (const auto *signal = compared.is_object()
                                   ? compared.find_object_member("signal")
                                   : nullptr) {
        auto nested = location;
        nested.path += "/compare/operand/signal";
        reference(signal, "signal_reference", nested);
        require_input_signal(*signal, nested);
        const auto *component = signal->find_object_member("component_id");
        const auto *name = signal->find_object_member("signal_name");
        if (component && component->is_string() && name && name->is_string()) {
          const auto &matches = signal_matches(component->as_string(), name->as_string());
          if (matches.size() == 1U) {
            const auto checked = detail::check_program_signal_comparison(
                matches.front().descriptor, member(operand, "op"), member(operand, "value"));
            if (!checked.ok()) {
              const auto reason = checked.error().details.find("reason");
              if (reason != checked.error().details.end() && reason->second == "io_signal_type_mismatch")
                emit("signal_type_mismatch", nested,
                     {{"component_id", text(component->as_string())},
                      {"signal_name", text(name->as_string())},
                      {"message", text(checked.error().message)}});
              else
                invalid(checked.error().message);
            }
          }
        }
      }
    } else {
      invalid("invalid interrupt operator");
    }
  }

  void non_node_references(const CanonicalValue &program) {
    const auto check_typed = [&](const CanonicalValue &row,
                                 std::string_view field,
                                 const std::string &path) {
      const auto *type = row.find_object_member("type");
      const auto *value = row.find_object_member(field);
      if (!type || !type->is_string() || !value)
        return;
      const char *schema_name =
          type->as_string() == "tcp_entry" ? "tcp_entry" : nullptr;
      if (schema_name) {
        const auto *definition =
            catalog_.schema_types.find_object_member(schema_name);
        if (!definition)
          invalid("missing tcp entry schema");
        walk(value, *definition, {path + "/" + std::string(field), {}, {}}, 0U);
        mounted_frame(*value, {path + "/" + std::string(field), {}, {}});
      } else if (type->as_string() == "stored_pose") {
        const auto *pose_ref = value->find_object_member("frame_ref");
        const auto *definition =
            catalog_.schema_types.find_object_member("pose_frame_ref");
        if (!definition)
          invalid("missing pose frame schema");
        if (pose_ref)
          walk(pose_ref, *definition,
               {path + "/" + std::string(field) + "/frame_ref", {}, {}}, 0U);
      }
    };
    if (const auto *parameters = program.find_object_member("parameters")) {
      if (!parameters->is_array())
        invalid("Program parameters must be an array");
      for (std::size_t i = 0; i < parameters->as_array().size(); ++i)
        check_typed(parameters->as_array()[i], "default",
                    "/parameters/" + std::to_string(i));
    }
    if (const auto *blackboard = program.find_object_member("blackboard")) {
      const auto *keys = blackboard->find_object_member("keys");
      if (keys) {
        if (!keys->is_array())
          invalid("blackboard keys must be an array");
        for (std::size_t i = 0; i < keys->as_array().size(); ++i) {
          const auto &row = keys->as_array()[i];
          const std::string path = "/blackboard/keys/" + std::to_string(i);
          check_typed(row, "initial", path);
          if (const auto *frames = row.find_object_member("allowed_frames")) {
            const auto *definition =
                catalog_.schema_types.find_object_member("pose_frame_ref");
            if (!definition || !frames->is_array())
              invalid("invalid allowed frames");
            for (std::size_t j = 0; j < frames->as_array().size(); ++j)
              walk(&frames->as_array()[j], *definition,
                   {path + "/allowed_frames/" + std::to_string(j), {}, {}}, 0U);
          }
        }
      }
    }
    if (const auto *scene = program.find_object_member("scene")) {
      for (std::size_t i = 0; i < scene->as_array().size(); ++i) {
        const auto &row = scene->as_array()[i];
        const auto *pose = row.find_object_member("pose");
        if (!pose || !pose->is_object())
          continue;
        const auto *frame_ref = pose->find_object_member("frame_ref");
        if (!frame_ref || !frame_ref->is_object())
          continue;
        const auto *kind = frame_ref->find_object_member("kind");
        if (kind && kind->is_string() &&
            (kind->as_string() == "component_frame" ||
             kind->as_string() == "target_frame")) {
          reference(frame_ref->find_object_member("frame"), "frame_reference",
                    {"/scene/" + std::to_string(i) + "/pose/frame_ref/frame",
                     {},
                     {}});
        }
      }
    }
    if (const auto *policies = program.find_object_member("contact_policies")) {
      if (!policies->is_array())
        invalid("contact policies must be an array");
      for (std::size_t i = 0; i < policies->as_array().size(); ++i) {
        const auto *zones = policies->as_array()[i].find_object_member("zones");
        if (!zones || !zones->is_array())
          continue;
        for (std::size_t j = 0; j < zones->as_array().size(); ++j) {
          const auto *pairs =
              zones->as_array()[j].find_object_member("body_pairs");
          if (!pairs || !pairs->is_array())
            continue;
          for (std::size_t k = 0; k < pairs->as_array().size(); ++k) {
            const auto &pair = pairs->as_array()[k];
            for (const auto side : {"body_a", "body_b"}) {
              const auto *selector = pair.find_object_member(side);
              if (!selector || !selector->is_object())
                continue;
              const auto *kind = selector->find_object_member("kind");
              if (!kind || !kind->is_string() ||
                  kind->as_string() != "component_model_body")
                continue;
              reference(selector->find_object_member("body"),
                        "collision_body_reference",
                        {"/contact_policies/" + std::to_string(i) + "/zones/" +
                             std::to_string(j) + "/body_pairs/" +
                             std::to_string(k) + "/" + side + "/body",
                         {},
                         {}});
            }
          }
        }
      }
    }
  }

  void mounted_frame(const CanonicalValue &value,
                     const ProgramReferenceLocation &location,
                     bool nested_frame = true) {
    if (!map_ || !value.is_object())
      return;
    const auto *target = value.find_object_member("target");
    const auto *frame = value.find_object_member("frame");
    if (!target || !frame || !target->is_object() || !frame->is_object())
      return;
    const auto *tc = target->find_object_member("component_id");
    const auto *ti = target->find_object_member("command_target_id");
    const auto *fc = frame->find_object_member("component_id");
    const auto *fi = frame->find_object_member("frame_id");
    if (!tc || !ti || !fc || !fi || !tc->is_string() || !ti->is_string() ||
        !fc->is_string() || !fi->is_string())
      return;
    if (!has_component(tc->as_string()) || !has_component(fc->as_string()))
      return;
    if (target_status(tc->as_string(), ti->as_string()))
      return;
    const auto matches = find_program_frame_matches(
        {core::ControllableComponentId(fc->as_string()), core::FrameRoleId(fi->as_string())}, *map_);
    if (matches.size() != 1U)
      return;
    const auto &physical = matches.front();
    const auto *target_resolution =
        map_->find_target(core::ControllableComponentId(tc->as_string()),
                          core::CommandTargetId(ti->as_string()));
    if (!target_resolution)
      invalid("missing exact retained target model map");
    const bool selected_by_target = std::any_of(
        target_resolution->frames.begin(), target_resolution->frames.end(),
        [&](const auto &entry) {
          const auto *resolved =
              std::get_if<CompositionComponentModelFrame>(&entry.frame);
          return resolved &&
                 resolved->controllable_component_id.value() ==
                     fc->as_string() &&
                 resolved->model_instance_id == physical.model_instance_id &&
                 resolved->model_revision == physical.model_revision &&
                 resolved->compiled_frame_name == physical.compiled_frame_name;
        });
    if (!selected_by_target ||
        !map_->kinematic_index().carries(
            core::ControllableComponentId(tc->as_string()),
            core::ControllableComponentId(fc->as_string()))) {
      auto nested = location;
      if (nested_frame) nested.path += "/frame";
      emit("frame_not_mounted", nested,
           {{"component_id", text(fc->as_string())},
            {"frame_id", text(fi->as_string())},
            {"target_component_id", text(tc->as_string())},
            {"command_target_id", text(ti->as_string())}});
    }
  }

  void target_body(const CanonicalValue &target, const CanonicalValue &body,
                   const ProgramReferenceLocation &location) {
    if (!map_ || !target.is_object() || !body.is_object()) return;
    const auto *tc = target.find_object_member("component_id");
    const auto *ti = target.find_object_member("command_target_id");
    const auto *bc = body.find_object_member("component_id");
    const auto *bi = body.find_object_member("collision_body_id");
    if (!tc || !ti || !bc || !bi || !tc->is_string() || !ti->is_string() ||
        !bc->is_string() || !bi->is_string() || !has_component(tc->as_string()) ||
        !has_component(bc->as_string()) || target_status(tc->as_string(), ti->as_string()))
      return;
    const auto matches = find_program_collision_body_matches(
        {core::ControllableComponentId(bc->as_string()), core::CollisionBodyRoleId(bi->as_string())}, *map_);
    if (matches.size() != 1U) return;
    const auto *selected = map_->find_target(core::ControllableComponentId(tc->as_string()),
                                             core::CommandTargetId(ti->as_string()));
    if (!selected) invalid("missing exact retained target model map");
    const auto &physical = matches.front();
    const bool provided = !physical.compiled_collision_body_names.empty() &&
        std::any_of(selected->collision_bodies.begin(), selected->collision_bodies.end(),
                    [&](const auto &candidate) {
          return candidate.model_instance_id == physical.model_instance_id &&
              std::all_of(physical.compiled_collision_body_names.begin(),
                          physical.compiled_collision_body_names.end(), [&](const auto &name) {
                return std::find(candidate.compiled_link_names.begin(), candidate.compiled_link_names.end(), name) !=
                       candidate.compiled_link_names.end();
              });
        });
    if (!provided || !map_->kinematic_index().carries(
                         core::ControllableComponentId(tc->as_string()),
                         core::ControllableComponentId(bc->as_string())))
      emit("collision_body_not_mounted", location,
           {{"component_id", text(bc->as_string())}, {"collision_body_id", text(bi->as_string())},
            {"target_component_id", text(tc->as_string())}, {"command_target_id", text(ti->as_string())}});
  }

  void check_matches(std::string_view kind, const std::string &component,
                     const std::string &field, const std::string &identity,
                     const ProgramReferenceLocation &location,
                     std::vector<std::string> &matches,
                     std::string_view missing_code,
                     CanonicalValue::Array qualified_candidates = {}) {
    if (matches.empty()) {
      emit(std::string(missing_code), location,
           {{"component_id", text(component)}, {field, text(identity)}});
    } else if (matches.size() > 1U) {
      std::sort(matches.begin(), matches.end());
      CanonicalValue::Array candidates;
      for (const auto &candidate : matches)
        candidates.emplace_back(candidate);
      CanonicalValue::Object payload{
          {"reference_kind", text(kind.substr(0, kind.size() - 10U))},
          {"component_id", text(component)},
          {"identity", text(identity)},
          {"candidate_ids", CanonicalValue(std::move(candidates))}};
      if (!qualified_candidates.empty())
        payload.emplace_back("candidates", CanonicalValue(std::move(qualified_candidates)));
      emit("reference_ambiguous", location, std::move(payload));
    }
  }

  const CanonicalValue &schema(const CanonicalValue &original) const {
    const CanonicalValue *current = &original;
    for (std::size_t i = 0; i < 64U; ++i) {
      if (!current->is_object())
        invalid("catalog schema must be an object");
      const auto *ref = current->find_object_member("schema_ref");
      if (!ref)
        return *current;
      constexpr std::string_view prefix = "#/schema_types/";
      if (!ref->is_string() || !ref->as_string().starts_with(prefix))
        invalid("invalid catalog schema reference");
      current = catalog_.schema_types.find_object_member(
          ref->as_string().substr(prefix.size()));
      if (!current)
        invalid("missing catalog schema reference");
    }
    invalid("catalog schema reference depth exceeded");
  }

  bool condition_matches(const CanonicalValue &condition,
                         const CanonicalValue &object) const {
    if (!condition.is_object()) invalid("invalid catalog requirement condition");
    for (const auto &[key, expected] : condition.as_object()) {
      const auto *actual = object.find_object_member(key);
      if (!actual) return false;
      if (expected.is_array()) {
        if (std::find(expected.as_array().begin(), expected.as_array().end(), *actual) ==
            expected.as_array().end()) return false;
      } else if (*actual != expected) return false;
    }
    return true;
  }

  bool required_property(const CanonicalValue &parent_schema,
                         const CanonicalValue &property_schema,
                         const CanonicalValue &object,
                         std::string_view name) const {
    if (const auto *required = parent_schema.find_object_member("required")) {
      if (!required->is_array())
        invalid("invalid catalog required list");
      for (const auto &item : required->as_array())
        if (item.is_string() && item.as_string() == name)
          return true;
    }
    if (const auto *when =
            property_schema.find_object_member("required_when")) {
      return condition_matches(*when, object);
    }
    return false;
  }

  void walk(const CanonicalValue *value, const CanonicalValue &definition,
            const ProgramReferenceLocation &location, std::size_t depth) {
    if (depth > 256U)
      invalid("reference schema depth exceeded");
    const auto &resolved = schema(definition);
    if (const auto *type = resolved.find_object_member("type");
        type && type->is_string() &&
        type->as_string() == "component_interrupt_condition") {
      if (value)
        condition(*value, location, 1U);
      return;
    }
    if (const auto *semantic = resolved.find_object_member("semantic_type");
        semantic && semantic->is_string()) {
      const auto &kind = semantic->as_string();
      if (kind == "target_reference" || kind == "signal_reference" ||
          kind == "frame_reference" || kind == "collision_body_reference") {
        reference(value, kind, location);
        return;
      }
      if (kind == "scene_resource" && value && value->is_string() &&
          !scene_ids_.contains(value->as_string()))
        emit("scene_resource_missing", location,
             {{"scene_resource_id", text(value->as_string())}});
    }
    if (!value)
      return;
    if (const auto *ref = definition.find_object_member("schema_ref");
        ref && ref->is_string() &&
        (ref->as_string() == "#/schema_types/tcp_selector" ||
         ref->as_string() == "#/schema_types/tcp_entry"))
      mounted_frame(*value, location);
    if (const auto *ref = definition.find_object_member("schema_ref");
        ref && ref->is_string() &&
        ref->as_string() == "#/schema_types/mock_signal_simulation" &&
        value->is_object()) {
      if (const auto *signal = value->find_object_member("signal")) {
        auto nested = location;
        nested.path += "/signal";
        require_input_signal(*signal, nested);
      }
    }
    if (value->is_object()) {
      const auto *properties = resolved.find_object_member("properties");
      if (!properties || !properties->is_object())
        return;
      for (const auto &[name, child_schema] : properties->as_object()) {
        const auto *child = value->find_object_member(name);
        if (!child && !required_property(resolved, child_schema, *value, name))
          continue;
        auto nested = location;
        nested.path += "/" + pointer_token(name);
        walk(child, child_schema, nested, depth + 1U);
      }
    } else if (value->is_array()) {
      const auto *items = resolved.find_object_member("items");
      if (!items)
        return;
      for (std::size_t i = 0; i < value->as_array().size(); ++i) {
        auto nested = location;
        nested.path += "/" + std::to_string(i);
        walk(&value->as_array()[i], *items, nested, depth + 1U);
      }
    }
  }

  void node_capabilities(const CanonicalValue &entry,
                         const CanonicalValue &parameters,
                         const std::string &path, const std::string &instance) {
    const auto &requirements = member(entry, "required_capabilities");
    const auto &definitions = member(entry, "parameters");
    if (!requirements.is_array() || !definitions.is_array())
      invalid("invalid catalog capability requirements");
    const auto effective =
        [&](const std::string &id) -> const CanonicalValue * {
      if (const auto *selected = parameters.find_object_member(id))
        return selected;
      for (const auto &definition : definitions.as_array()) {
        const auto &name = member(definition, "id");
        if (name.is_string() && name.as_string() == id)
          return definition.find_object_member("default");
      }
      invalid("capability selector is absent from catalog");
    };
    for (const auto &requirement : requirements.as_array()) {
      if (const auto *when = requirement.find_object_member("when")) {
        const auto &id = member(*when, "parameter");
        if (!id.is_string())
          invalid("invalid capability condition");
        const auto *selected = effective(id.as_string());
        if (!selected || *selected != member(*when, "equals"))
          continue;
      }
      const auto &id_value = member(requirement, "capability_id");
      const auto &scope_value = member(requirement, "scope");
      if (!id_value.is_string() || !scope_value.is_string())
        invalid("invalid capability identity or scope");
      const auto &id = id_value.as_string();
      const auto *descriptor = core::find_builtin_capability_definition(id);
      if (!descriptor)
        invalid("unknown catalog capability");
      const CanonicalValue required_parameters(CanonicalValue::Object{});
      if (scope_value.as_string() == "cell_global") {
        auto found = cell_capability_status_.find(id);
        if (found == cell_capability_status_.end()) {
          const core::ProgramCapabilityRequirementDTO wanted{
              core::CapabilityId(id),
              core::CapabilityDefinitionRevision(std::string(descriptor->capability_definition_revision)),
              core::ProgramCapabilityDisposition::kRequired};
          found = cell_capability_status_
                      .emplace(id, probe_program_cell_capability(wanted, bundle_, source_))
                      .first;
        }
        if (!found->second)
          emit("cell_capability_missing", {path, instance, {}},
               {{"capability_id", text(id)},
                {"required_parameters", required_parameters}});
        continue;
      }
      const auto &selector = member(requirement, "target_parameter");
      if (!selector.is_string())
        invalid("invalid target capability selector");
      const auto *selected = effective(selector.as_string());
      if (!selected || !selected->is_object())
        continue;
      const auto *component = selected->find_object_member("component_id");
      if (!component || !component->is_string() ||
          !has_component(component->as_string()))
        continue;
      const auto *target = selected->find_object_member("command_target_id");
      const auto *signal = selected->find_object_member("signal_name");
      std::optional<ProgramSignalMatch> signal_match;
      if (signal && signal->is_string()) {
        const auto matches = signal_matches(component->as_string(), signal->as_string());
        if (matches.size() != 1U)
          continue;
        signal_match = matches.front();
      }
      if ((!target || !target->is_string()) && !signal_match)
        continue;
      const auto &target_id = signal_match ? signal_match->retained->command_target_id.value()
                                           : target->as_string();
      if (target_status(component->as_string(), target_id))
        continue;
      const ProgramReferenceLocation location{
          path + "/parameters/" + pointer_token(selector.as_string()), instance,
          selector.as_string()};
      if (signal_match && (id == "io.gpio.get_input" ||
                         id == "io.gpio.wait_for_input" ||
                         id == "io.gpio.set_output")) {
        const auto expected = id == "io.gpio.set_output"
                                  ? core::IoSignalDirection::kOutput
                                  : core::IoSignalDirection::kInput;
        const auto actual = signal_match->descriptor.direction;
        if (actual != expected)
          emit("signal_direction_mismatch", location,
               {{"component_id", text(component->as_string())},
                {"signal_name", text(signal->as_string())},
                {"expected_direction",
                 text(expected == core::IoSignalDirection::kInput ? "input"
                                                                  : "output")},
                {"actual_direction",
                 text(actual == core::IoSignalDirection::kInput ? "input"
                                                                : "output")}});
        if (expected == core::IoSignalDirection::kInput &&
            signal_match->retained->provider.profile == kSystemCarriedObjectProfile)
          continue;
      }
      const core::ProgramCapabilityRequirementDTO wanted{
          core::CapabilityId(id),
          core::CapabilityDefinitionRevision(
              std::string(descriptor->capability_definition_revision)),
          core::ProgramCapabilityDisposition::kRequired};
      const auto status =
          capability_status(component->as_string(), target_id, wanted);
      if (status == ProgramBindingUnresolvedReason::kCapabilityUnavailable)
        emit("capability_missing", location,
             {{"component_id", text(component->as_string())},
              {"command_target_id", text(target_id)},
              {"capability_id", text(id)},
              {"required_parameters", required_parameters}});
    }
  }

  void node(const CanonicalValue &value, const std::string &path,
            std::size_t depth) {
    if (depth > 256U || ++nodes_ > core::kMaxProgramTreeNodes)
      invalid("tree bound exceeded");
    const auto &node_id = member(value, "node_id");
    const auto &instance = member(value, "instance_id");
    const auto &parameters = member(value, "parameters");
    if (!node_id.is_string() || !instance.is_string() ||
        !parameters.is_object())
      invalid("invalid node shape");
    const auto found = catalog_.entries.find(node_id.as_string());
    if (found == catalog_.entries.end())
      invalid("unknown catalog node");
    const auto &definitions = member(found->second, "parameters");
    if (!definitions.is_array())
      invalid("invalid catalog parameter list");
    if (node_id.as_string() == "io.wait_for_input") {
      const auto *edge = parameters.find_object_member("edge");
      if (edge && edge->is_string() && edge->as_string() != "level")
        emit("io_watch_condition_unsupported",
             {path + "/parameters/edge", instance.as_string(), std::string("edge")},
             {{"edge", text(edge->as_string())}});
    }
    for (const auto &definition : definitions.as_array()) {
      const auto &id = member(definition, "id");
      if (!id.is_string())
        invalid("invalid catalog parameter id");
      const auto *selected = parameters.find_object_member(id.as_string());
      if (!selected)
        selected = definition.find_object_member("default");
      if (!selected) {
        const auto *required = definition.find_object_member("required");
        if (!required || !required->is_bool() || !required->as_bool())
          continue;
      }
      const auto *parameter_schema = definition.find_object_member("schema");
      if (!parameter_schema)
        parameter_schema = &definition;
      walk(selected, *parameter_schema,
           {path + "/parameters/" + pointer_token(id.as_string()),
            instance.as_string(), id.as_string()},
           0);
      // The key-typed schema takes its type from a literal blackboard key,
      // so a stored-pose value needs its frame checked explicitly.
      const auto &resolved = schema(*parameter_schema);
      const auto *from_key = resolved.find_object_member("value_type_from_key");
      const auto *key_name = from_key && from_key->is_string()
                                 ? parameters.find_object_member(from_key->as_string())
                                 : nullptr;
      const auto *blackboard = program_->find_object_member("blackboard");
      const auto *keys = blackboard ? blackboard->find_object_member("keys") : nullptr;
      if (selected && key_name && key_name->is_string() && keys && keys->is_array()) {
        for (const auto &key : keys->as_array()) {
          const auto *name = key.find_object_member("key");
          const auto *type = key.find_object_member("type");
          if (!name || !name->is_string() || name->as_string() != key_name->as_string() ||
              !type || !type->is_string() || type->as_string() != "stored_pose")
            continue;
          const auto *frame = selected->find_object_member("frame_ref");
          const auto *definition = catalog_.schema_types.find_object_member("pose_frame_ref");
          if (!definition) invalid("missing pose frame schema");
          if (frame)
            walk(frame, *definition,
                 {path + "/parameters/" + pointer_token(id.as_string()) + "/frame_ref",
                  instance.as_string(), id.as_string()}, 0U);
          break;
        }
      }
    }
    if (const auto *target = parameters.find_object_member("target")) {
      if (const auto *base = parameters.find_object_member("base_frame")) {
        const CanonicalValue pair(CanonicalValue::Object{{"target", *target}, {"frame", *base}});
        mounted_frame(pair, {path + "/parameters/base_frame", instance.as_string(),
                             std::string("base_frame")}, false);
      }
    }
    if (const auto *carrier = parameters.find_object_member("carrier_frame_ref")) {
      const auto *target = parameters.find_object_member("tool_target");
      if (!target) target = parameters.find_object_member("target");
      if (target) {
        const CanonicalValue pair(CanonicalValue::Object{{"target", *target}, {"frame", *carrier}});
        mounted_frame(pair, {path + "/parameters/carrier_frame_ref", instance.as_string(),
                             std::string("carrier_frame_ref")}, false);
      }
    }
    if (const auto *bodies = parameters.find_object_member("touch_bodies"); bodies && bodies->is_array()) {
      const auto *target = parameters.find_object_member("tool_target");
      if (!target) target = parameters.find_object_member("target");
      if (target)
        for (std::size_t i = 0; i < bodies->as_array().size(); ++i)
          target_body(*target, bodies->as_array()[i],
                      {path + "/parameters/touch_bodies/" + std::to_string(i),
                       instance.as_string(), std::string("touch_bodies")});
    }
    node_capabilities(found->second, parameters, path, instance.as_string());
    if (node_id.as_string() == "io.clear_outside_change") {
      const auto *selected = parameters.find_object_member("signal");
      const auto *component = selected ? selected->find_object_member("component_id") : nullptr;
      const auto *name = selected ? selected->find_object_member("signal_name") : nullptr;
      if (component && name && component->is_string() && name->is_string() &&
          has_component(component->as_string())) {
        const auto matches = signal_matches(component->as_string(), name->as_string());
        if (matches.size() == 1U &&
            matches.front().descriptor.direction != core::IoSignalDirection::kOutput)
          emit("signal_direction_mismatch",
               {path + "/parameters/signal", instance.as_string(), "signal"},
               {{"component_id", text(component->as_string())}, {"signal_name", text(name->as_string())},
                {"expected_direction", text("output")}, {"actual_direction", text("input")}});
      }
    }
    const auto &children = member(value, "children");
    if (!children.is_array())
      invalid("node children must be an array");
    for (std::size_t i = 0; i < children.as_array().size(); ++i)
      node(children.as_array()[i], path + "/children/" + std::to_string(i),
           depth + 1U);
  }

  const RetainedBundle &bundle_;
  const CanonicalValue *program_{};
  const std::optional<VerifiedRetainedModelMap> &map_;
  const ICatalogArtifactSource &source_;
  bool draft_;
  QualifiedProgramCatalogView catalog_;
  std::vector<RetainedIoSignalConfiguration> signals_;
  std::map<std::pair<std::string, std::string>, std::vector<ProgramSignalMatch>>
      signal_matches_;
  std::map<std::pair<std::string, std::string>,
           std::optional<ProgramBindingUnresolvedReason>>
      target_status_;
  std::map<std::tuple<std::string, std::string, std::string, std::string>,
           std::optional<ProgramBindingUnresolvedReason>>
      capability_status_;
  std::map<std::string, bool> cell_capability_status_;
  std::set<std::string> scene_ids_;
  std::size_t nodes_{};
  std::vector<ProgramReferenceDiagnostic> rows_;
};

} // namespace

std::vector<ProgramReferenceDiagnostic> validate_program_references(
    const CanonicalValue &program, bool draft,
    const RetainedBundle &retained_bundle,
    const std::optional<VerifiedRetainedModelMap> &retained_model_map,
    const ICatalogArtifactSource &catalog_artifact_store) {
  require_supported_program_format(program);
  return Validator(retained_bundle, retained_model_map, catalog_artifact_store,
                   draft)
      .run(program);
}

} // namespace manyforge::deployment
