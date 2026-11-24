#include <manyforge_core/collision_rule_codec.hpp>

#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

void append_attached_object(CanonicalValue::Object& fields,
                            const CollisionRuleAttachedObjectSelection& selected) {
  fields.emplace_back("kind", CanonicalValue("attached_object"));
  fields.emplace_back("component_id", CanonicalValue(selected.component_id.value()));
  if (selected.collision_body_role_id) {
    fields.emplace_back("collision_body_role_id", CanonicalValue(*selected.collision_body_role_id));
  }
}

[[nodiscard]] CanonicalValue selection_value(const CollisionRuleSelectionDTO& selection) {
  return std::visit(
      [](const auto& selected) {
        using Selected = std::decay_t<decltype(selected)>;
        CanonicalValue::Object fields;
        if constexpr (std::is_same_v<Selected, CollisionRuleComponentSelection>) {
          fields.emplace_back("kind", CanonicalValue("component"));
          fields.emplace_back("component_id", CanonicalValue(selected.component_id.value()));
        } else if constexpr (std::is_same_v<Selected, CollisionRuleCollisionBodySelection>) {
          fields.emplace_back("kind", CanonicalValue("collision_body"));
          fields.emplace_back("component_id", CanonicalValue(selected.body.component_id.value()));
          fields.emplace_back("collision_body_id",
                              CanonicalValue(selected.body.collision_body_id.value()));
        } else if constexpr (std::is_same_v<Selected, CollisionRuleLinkSelection>) {
          fields.emplace_back("kind", CanonicalValue("link"));
          fields.emplace_back("component_id", CanonicalValue(selected.component_id.value()));
          fields.emplace_back("link", CanonicalValue(selected.link));
        } else if constexpr (std::is_same_v<Selected, CollisionRuleAttachedObjectSelection>) {
          append_attached_object(fields, selected);
        } else if constexpr (std::is_same_v<Selected, CollisionRuleSceneObjectsSelection>) {
          fields.emplace_back("kind", CanonicalValue("scene_objects"));
        } else if constexpr (std::is_same_v<Selected, CollisionRuleSceneObjectSelection>) {
          fields.emplace_back("kind", CanonicalValue("scene_object"));
          fields.emplace_back("scene_resource_id",
                              CanonicalValue(selected.scene_resource_id.value()));
        } else if constexpr (std::is_same_v<Selected, CollisionRuleModelGroupSelection>) {
          fields.emplace_back("kind", CanonicalValue("model_group"));
          fields.emplace_back("component_id", CanonicalValue(selected.component_id.value()));
          fields.emplace_back("group", CanonicalValue(selected.group));
        } else {
          fields.emplace_back("kind", CanonicalValue("active_tcp_contact_bodies"));
        }
        return CanonicalValue(std::move(fields));
      },
      selection);
}

[[nodiscard]] CanonicalValue selection_value(const CollisionRuleSelection& selection) {
  return std::visit(
      [](const auto& selected) {
        using Selected = std::decay_t<decltype(selected)>;
        CanonicalValue::Object fields;
        if constexpr (std::is_same_v<Selected, CollisionRuleComponentSelection>) {
          fields.emplace_back("kind", CanonicalValue("component"));
          fields.emplace_back("component_id", CanonicalValue(selected.component_id.value()));
        } else if constexpr (std::is_same_v<Selected, CollisionRuleResolvedBodiesSelection>) {
          CanonicalValue::Array ids;
          ids.reserve(selected.collision_body_ids.size());
          for (const auto& id : selected.collision_body_ids) ids.emplace_back(id);
          fields.emplace_back("kind", CanonicalValue("collision_bodies"));
          fields.emplace_back("collision_body_ids", CanonicalValue(std::move(ids)));
        } else if constexpr (std::is_same_v<Selected, CollisionRuleLinkSelection>) {
          fields.emplace_back("kind", CanonicalValue("link"));
          fields.emplace_back("component_id", CanonicalValue(selected.component_id.value()));
          fields.emplace_back("link", CanonicalValue(selected.link));
        } else if constexpr (std::is_same_v<Selected, CollisionRuleAttachedObjectSelection>) {
          append_attached_object(fields, selected);
        } else if constexpr (std::is_same_v<Selected,
                                            CollisionRuleResolvedAttachedObjectSelection>) {
          CanonicalValue::Array ids;
          ids.reserve(selected.parent_body_ids.size());
          for (const auto& id : selected.parent_body_ids) ids.emplace_back(id);
          fields.emplace_back("kind", CanonicalValue("attached_object"));
          fields.emplace_back("parent_body_ids", CanonicalValue(std::move(ids)));
        } else if constexpr (std::is_same_v<Selected, CollisionRuleSceneObjectsSelection>) {
          fields.emplace_back("kind", CanonicalValue("scene_objects"));
        } else {
          fields.emplace_back("kind", CanonicalValue("scene_object"));
          fields.emplace_back("scene_resource_id",
                              CanonicalValue(selected.scene_resource_id.value()));
        }
        return CanonicalValue(std::move(fields));
      },
      selection);
}

[[nodiscard]] std::string_view evidence_name(const ContactReleaseEvidenceMode mode) noexcept {
  return mode == ContactReleaseEvidenceMode::kSignedPenetration ? "signed_penetration"
                                                               : "binary_contact";
}

void append_value(CanonicalValue::Object& fields, const CollisionRuleValue& value) {
  if (const auto* clearance = std::get_if<CollisionClearanceRule>(&value)) {
    fields.emplace_back("clearance_m", CanonicalValue(clearance->clearance_m));
  } else if (const auto* contact = std::get_if<CollisionContactRule>(&value)) {
    CanonicalValue::Object contact_fields;
    if (contact->max_penetration_m) {
      contact_fields.emplace_back("max_penetration_m", CanonicalValue(*contact->max_penetration_m));
    }
    contact_fields.emplace_back("end_from_fraction", CanonicalValue(contact->end_from_fraction));
    if (contact->max_compression_step_m) {
      contact_fields.emplace_back("max_compression_step_m",
                                  CanonicalValue(*contact->max_compression_step_m));
    }
    if (contact->spring_group) {
      const auto& group = *contact->spring_group;
      // RFC 8785 has no negative zero.
      const auto axis = [&](const std::size_t index) {
        return CanonicalValue(group.axis[index] == 0.0 ? 0.0 : group.axis[index]);
      };
      contact_fields.emplace_back(
          "spring_group",
          CanonicalValue(CanonicalValue::Object{
              {"component_id", CanonicalValue(group.component_id.value())},
              {"element_id", CanonicalValue(group.element_id)},
              {"axis", CanonicalValue(CanonicalValue::Array{axis(0U), axis(1U), axis(2U)})}}));
    }
    fields.emplace_back("contact", CanonicalValue(std::move(contact_fields)));
  } else {
    const auto& release = std::get<CollisionReleaseRule>(value);
    fields.emplace_back(
        "release",
        CanonicalValue(CanonicalValue::Object{
            {"release_evidence_mode",
             CanonicalValue(std::string(evidence_name(release.release_evidence_mode)))},
            {"enforce_max_start_penetration",
             CanonicalValue(release.enforce_max_start_penetration)},
            {"max_start_penetration_m", CanonicalValue(release.max_start_penetration_m)},
            {"release_tolerance_m", CanonicalValue(release.release_tolerance_m)},
            {"min_release_progress_m", CanonicalValue(release.min_release_progress_m)},
            {"require_separation_by_end", CanonicalValue(release.require_separation_by_end)},
            {"require_strictly_decreasing_penetration",
             CanonicalValue(release.require_strictly_decreasing_penetration)},
            {"allow_recontact", CanonicalValue(release.allow_recontact)}}));
  }
}

template <typename Rule>
[[nodiscard]] CanonicalValue rules_value(const std::vector<Rule>& rules) {
  CanonicalValue::Array rows;
  rows.reserve(rules.size());
  for (const auto& rule : rules) {
    CanonicalValue::Object fields{{"rule_id", CanonicalValue(rule.rule_id)}};
    if (!rule.a.empty() || !rule.b.empty()) {
      for (const auto& [name, side] : {std::pair{"a", &rule.a}, std::pair{"b", &rule.b}}) {
        CanonicalValue::Array selections;
        selections.reserve(side->size());
        for (const auto& selection : *side) selections.push_back(selection_value(selection));
        fields.emplace_back(name, CanonicalValue(std::move(selections)));
      }
    }
    append_value(fields, rule.value);
    rows.emplace_back(std::move(fields));
  }
  return CanonicalValue(std::move(rows));
}

struct DecodeFailure final : std::invalid_argument {
  using std::invalid_argument::invalid_argument;
};

[[noreturn]] void fail(const std::string_view reason) { throw DecodeFailure(std::string(reason)); }

/// The object's members are exactly `required` plus any of `optional`.
void require_members(const CanonicalValue& value, std::initializer_list<std::string_view> required,
                     std::initializer_list<std::string_view> optional,
                     const std::string_view reason) {
  if (!value.is_object()) fail(reason);
  std::size_t known = 0U;
  for (const auto name : required) {
    if (value.find_object_member(name) == nullptr) fail(reason);
    ++known;
  }
  for (const auto name : optional) {
    if (value.find_object_member(name) != nullptr) ++known;
  }
  if (value.as_object().size() != known) fail(reason);
}

[[nodiscard]] const std::string& text(const CanonicalValue& value, const std::string_view name,
                                      const std::string_view reason) {
  const auto* member = value.find_object_member(name);
  if (member == nullptr || !member->is_string()) fail(reason);
  return member->as_string();
}

[[nodiscard]] CollisionRuleSelectionDTO decode_selection(const CanonicalValue& value) {
  constexpr std::string_view reason = "collision_rule_selection_invalid";
  if (!value.is_object()) fail(reason);
  const auto& kind = text(value, "kind", reason);
  if (kind == "component") {
    require_members(value, {"kind", "component_id"}, {}, reason);
    return CollisionRuleComponentSelection{
        ControllableComponentId(text(value, "component_id", reason))};
  }
  if (kind == "collision_body") {
    require_members(value, {"kind", "component_id", "collision_body_id"}, {}, reason);
    return CollisionRuleCollisionBodySelection{ProgramCollisionBodyReferenceDTO{
        ControllableComponentId(text(value, "component_id", reason)),
        CollisionBodyRoleId(text(value, "collision_body_id", reason))}};
  }
  if (kind == "link") {
    require_members(value, {"kind", "component_id", "link"}, {}, reason);
    return CollisionRuleLinkSelection{ControllableComponentId(text(value, "component_id", reason)),
                                      text(value, "link", reason)};
  }
  if (kind == "model_group") {
    require_members(value, {"kind", "component_id", "group"}, {}, reason);
    return CollisionRuleModelGroupSelection{
        ControllableComponentId(text(value, "component_id", reason)), text(value, "group", reason)};
  }
  if (kind == "attached_object") {
    require_members(value, {"kind", "component_id"}, {"collision_body_role_id"}, reason);
    CollisionRuleAttachedObjectSelection selection{
        ControllableComponentId(text(value, "component_id", reason))};
    if (value.find_object_member("collision_body_role_id") != nullptr) {
      selection.collision_body_role_id = text(value, "collision_body_role_id", reason);
    }
    return selection;
  }
  if (kind == "scene_objects") {
    require_members(value, {"kind"}, {}, reason);
    return CollisionRuleSceneObjectsSelection{};
  }
  if (kind == "scene_object") {
    require_members(value, {"kind", "scene_resource_id"}, {}, reason);
    return CollisionRuleSceneObjectSelection{
        SceneResourceId(text(value, "scene_resource_id", reason))};
  }
  if (kind == "active_tcp_contact_bodies") {
    require_members(value, {"kind"}, {}, reason);
    return CollisionRuleActiveTcpContactBodiesSelection{};
  }
  fail(reason);
}

[[nodiscard]] std::vector<CollisionRuleSelectionDTO> decode_side(const CanonicalValue& row,
                                                                 const std::string_view name) {
  const auto* side = row.find_object_member(name);
  if (side == nullptr) return {};
  if (!side->is_array() || side->as_array().empty()) fail("collision_rule_selection_invalid");
  std::vector<CollisionRuleSelectionDTO> selections;
  selections.reserve(side->as_array().size());
  for (const auto& selection : side->as_array()) selections.push_back(decode_selection(selection));
  return selections;
}

[[nodiscard]] double number(const CanonicalValue& value) {
  if (!value.is_number()) fail("collision_rule_value_invalid");
  return value.as_number();
}

[[nodiscard]] bool boolean(const CanonicalValue& value) {
  if (!value.is_bool()) fail("collision_rule_value_invalid");
  return value.as_bool();
}

[[nodiscard]] CollisionRuleValue decode_value(const CanonicalValue& row) {
  constexpr std::string_view reason = "collision_rule_value_invalid";
  const auto* clearance = row.find_object_member("clearance_m");
  const auto* contact = row.find_object_member("contact");
  const auto* release = row.find_object_member("release");
  if ((clearance != nullptr ? 1 : 0) + (contact != nullptr ? 1 : 0) +
          (release != nullptr ? 1 : 0) !=
      1) {
    fail(reason);
  }
  if (clearance != nullptr) {
    return CollisionClearanceRule{number(*clearance)};
  }
  if (contact != nullptr) {
    require_members(*contact, {},
                    {"max_penetration_m", "end_from_fraction", "max_compression_step_m",
                     "spring_group"},
                    reason);
    CollisionContactRule rule;
    if (const auto* limit = contact->find_object_member("max_penetration_m")) {
      rule.max_penetration_m = number(*limit);
    }
    if (const auto* fraction = contact->find_object_member("end_from_fraction")) {
      rule.end_from_fraction = number(*fraction);
    }
    if (const auto* step = contact->find_object_member("max_compression_step_m")) {
      rule.max_compression_step_m = number(*step);
    }
    if (const auto* group = contact->find_object_member("spring_group")) {
      require_members(*group, {"component_id", "element_id", "axis"}, {}, reason);
      const auto* axis = group->find_object_member("axis");
      if (!axis->is_array() || axis->as_array().size() != 3U) fail(reason);
      rule.spring_group = CollisionSpringGroup{
          ControllableComponentId(text(*group, "component_id", reason)),
          text(*group, "element_id", reason),
          {number(axis->as_array()[0]), number(axis->as_array()[1]),
           number(axis->as_array()[2])}};
    }
    return rule;
  }
  require_members(*release,
                  {"release_evidence_mode", "enforce_max_start_penetration",
                   "max_start_penetration_m", "release_tolerance_m", "min_release_progress_m",
                   "require_separation_by_end", "require_strictly_decreasing_penetration",
                   "allow_recontact"},
                  {}, reason);
  CollisionReleaseRule rule;
  const auto& mode = text(*release, "release_evidence_mode", reason);
  if (mode == "signed_penetration") {
    rule.release_evidence_mode = ContactReleaseEvidenceMode::kSignedPenetration;
  } else if (mode == "binary_contact") {
    rule.release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  } else {
    fail(reason);
  }
  rule.enforce_max_start_penetration =
      boolean(*release->find_object_member("enforce_max_start_penetration"));
  rule.max_start_penetration_m = number(*release->find_object_member("max_start_penetration_m"));
  rule.release_tolerance_m = number(*release->find_object_member("release_tolerance_m"));
  rule.min_release_progress_m = number(*release->find_object_member("min_release_progress_m"));
  rule.require_separation_by_end =
      boolean(*release->find_object_member("require_separation_by_end"));
  rule.require_strictly_decreasing_penetration =
      boolean(*release->find_object_member("require_strictly_decreasing_penetration"));
  rule.allow_recontact = boolean(*release->find_object_member("allow_recontact"));
  return rule;
}

[[nodiscard]] ErrorDTO decode_error(const std::string& reason,
                                    const std::optional<std::size_t> rule_index) {
  ErrorDTO error{kErrorInvalidCollisionRule, "Collision rules are malformed: " + reason,
                 {{"reason", reason}}};
  if (rule_index) {
    error.details.emplace("rule_index", std::to_string(*rule_index));
  }
  return error;
}

}  // namespace

content_identity::CanonicalValue CollisionRulesValue(const std::vector<CollisionRuleDTO>& rules) {
  return rules_value(rules);
}

content_identity::CanonicalValue CollisionRulesValue(const std::vector<CollisionRule>& rules) {
  return rules_value(rules);
}

Result<std::vector<CollisionRuleDTO>> DecodeCollisionRules(
    const content_identity::CanonicalValue& value) {
  using Decoded = Result<std::vector<CollisionRuleDTO>>;
  if (!value.is_array()) {
    return Decoded::Err(decode_error("collision_rules_bound_exceeded", std::nullopt));
  }
  const auto& rows = value.as_array();
  if (rows.size() > kMaxCollisionRules) {
    return Decoded::Err(decode_error("collision_rules_bound_exceeded", std::nullopt));
  }
  std::vector<CollisionRuleDTO> rules;
  rules.reserve(rows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    try {
      const auto& row = rows[index];
      require_members(row, {"rule_id"}, {"a", "b", "clearance_m", "contact", "release"},
                      "collision_rule_selection_invalid");
      CollisionRuleDTO rule;
      rule.rule_id = text(row, "rule_id", "collision_rule_id_invalid");
      rule.a = decode_side(row, "a");
      rule.b = decode_side(row, "b");
      rule.value = decode_value(row);
      rules.push_back(std::move(rule));
    } catch (const DecodeFailure& failure) {
      return Decoded::Err(decode_error(failure.what(), index));
    }
  }
  if (const auto violation = collision_rules_shape_violation(rules); !violation.empty()) {
    return Decoded::Err(decode_error(std::string(violation), std::nullopt));
  }
  return Decoded::Ok(std::move(rules));
}

}  // namespace manyforge::core
