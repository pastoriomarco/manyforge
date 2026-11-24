#include "manyforge_deployment/collision_rule_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>

namespace manyforge::deployment {
namespace {

using Array = CanonicalValue::Array;
using Object = CanonicalValue::Object;

constexpr std::size_t kMaximumIdentityBytes = 128U;
constexpr std::size_t kMaximumLinkNameBytes = 256U;
constexpr double kMaximumReleaseMetricM = 1000.0;

[[noreturn]] void fail(std::string_view reason, std::string_view path,
                       std::string_view message) {
  throw CollisionRuleCodecError(std::string(reason), std::string(path) + ": " +
                                                         std::string(message));
}

[[nodiscard]] std::string child(std::string_view path, std::string_view name) {
  return std::string(path) + "." + std::string(name);
}

[[nodiscard]] std::string item(std::string_view path, std::size_t index) {
  return std::string(path) + "[" + std::to_string(index) + "]";
}

[[nodiscard]] const Object &object(const CanonicalValue &value,
                                   std::string_view reason,
                                   std::string_view path) {
  const auto *result = std::get_if<Object>(&value.storage());
  if (result == nullptr) fail(reason, path, "expected an object");
  return *result;
}

[[nodiscard]] const Array &array(const CanonicalValue &value,
                                 std::string_view reason,
                                 std::string_view path) {
  const auto *result = std::get_if<Array>(&value.storage());
  if (result == nullptr) fail(reason, path, "expected a list");
  return *result;
}

// Every key of `value` is in `allowed`.
void require_keys(const Object &value,
                  std::initializer_list<std::string_view> allowed,
                  std::string_view reason, std::string_view path) {
  for (const auto &[key, unused] : value) {
    static_cast<void>(unused);
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
      fail(reason, path, "unknown key '" + key + "'");
  }
}

[[nodiscard]] const CanonicalValue *find(const Object &value,
                                         std::string_view name) {
  for (const auto &[key, member] : value)
    if (key == name) return &member;
  return nullptr;
}

[[nodiscard]] std::string identity(const Object &value, std::string_view name,
                                   std::size_t maximum_bytes,
                                   std::string_view reason,
                                   std::string_view path) {
  const auto *member = find(value, name);
  const auto *text =
      member ? std::get_if<std::string>(&member->storage()) : nullptr;
  if (text == nullptr || text->empty() || text->size() > maximum_bytes)
    fail(reason, child(path, name), "expected non-empty text within its bound");
  return *text;
}

[[nodiscard]] double number(const CanonicalValue &value, double minimum,
                            double maximum, std::string_view path) {
  const auto *result = std::get_if<double>(&value.storage());
  if (result == nullptr || !std::isfinite(*result) || *result < minimum ||
      *result > maximum)
    fail("collision_rule_value_invalid", path,
         "expected a number within its range");
  return *result;
}

[[nodiscard]] core::CollisionRuleSelectionDTO
selection(const CanonicalValue &value, CollisionRuleRows rows,
          std::string_view path,
          std::vector<CollisionRuleBlackboardSceneObject> &blackboard_objects) {
  constexpr std::string_view reason = "collision_rule_selection_invalid";
  const auto &fields = object(value, reason, path);
  const auto kind = identity(fields, "kind", kMaximumIdentityBytes, reason, path);
  const auto component = [&] {
    return core::ControllableComponentId(
        identity(fields, "component_id", kMaximumIdentityBytes, reason, path));
  };
  if (kind == "component") {
    require_keys(fields, {"kind", "component_id"}, reason, path);
    return core::CollisionRuleComponentSelection{component()};
  }
  if (kind == "collision_body") {
    require_keys(fields, {"kind", "component_id", "collision_body_id"}, reason,
                 path);
    return core::CollisionRuleCollisionBodySelection{
        {component(),
         core::CollisionBodyRoleId(identity(fields, "collision_body_id",
                                            kMaximumIdentityBytes, reason,
                                            path))}};
  }
  if (kind == "link") {
    require_keys(fields, {"kind", "component_id", "link"}, reason, path);
    return core::CollisionRuleLinkSelection{
        component(),
        identity(fields, "link", kMaximumLinkNameBytes, reason, path)};
  }
  if (rows == CollisionRuleRows::kCell && kind == "model_group") {
    require_keys(fields, {"kind", "component_id", "group"}, reason, path);
    return core::CollisionRuleModelGroupSelection{
        component(),
        identity(fields, "group", kMaximumLinkNameBytes, reason, path)};
  }
  if (kind == "attached_object") {
    require_keys(fields, {"kind", "component_id", "collision_body_role_id"},
                 reason, path);
    return core::CollisionRuleAttachedObjectSelection{
        component(),
        find(fields, "collision_body_role_id") != nullptr
            ? std::optional<std::string>(
                  identity(fields, "collision_body_role_id",
                           kMaximumIdentityBytes, reason, path))
            : std::nullopt};
  }
  if (kind == "scene_objects") {
    require_keys(fields, {"kind"}, reason, path);
    return core::CollisionRuleSceneObjectsSelection{};
  }
  if (rows != CollisionRuleRows::kCell && kind == "scene_object") {
    require_keys(fields,
                 {"kind", "scene_resource_id", "blackboard_key", "field"},
                 reason, path);
    const bool literal = find(fields, "scene_resource_id") != nullptr;
    const bool key = find(fields, "blackboard_key") != nullptr;
    if (literal == key)
      fail(reason, path,
           "a scene_object names exactly one of scene_resource_id and "
           "blackboard_key");
    const bool field = find(fields, "field") != nullptr;
    if (field && !key)
      fail(reason, path, "field is given only with blackboard_key");
    if (key) {
      blackboard_objects.push_back(
          {identity(fields, "blackboard_key", kMaximumIdentityBytes, reason,
                    path),
           field ? std::optional<std::string>(identity(
                       fields, "field", kMaximumIdentityBytes, reason, path))
                 : std::nullopt});
      return core::CollisionRuleSceneObjectSelection{
          core::SceneResourceId(std::string{})};
    }
    return core::CollisionRuleSceneObjectSelection{core::SceneResourceId(
        identity(fields, "scene_resource_id", kMaximumIdentityBytes, reason,
                 path))};
  }
  if (rows != CollisionRuleRows::kCell && kind == "active_tcp_contact_bodies") {
    require_keys(fields, {"kind"}, reason, path);
    return core::CollisionRuleActiveTcpContactBodiesSelection{};
  }
  fail(reason, child(path, "kind"),
       "'" + kind + "' is no selection of this rule list");
}

[[nodiscard]] std::vector<core::CollisionRuleSelectionDTO>
selections(const CanonicalValue &value, CollisionRuleRows rows,
           std::string_view path,
          std::vector<CollisionRuleBlackboardSceneObject> &blackboard_objects) {
  const auto &items = array(value, "collision_rule_selection_invalid", path);
  if (items.empty() || items.size() > core::kMaxCollisionRuleSelections)
    fail("collision_rule_selection_invalid", path,
         "expected 1 to " + std::to_string(core::kMaxCollisionRuleSelections) +
             " selections");
  std::vector<core::CollisionRuleSelectionDTO> result;
  result.reserve(items.size());
  for (std::size_t index = 0U; index < items.size(); ++index)
    result.push_back(
        selection(items[index], rows, item(path, index), blackboard_objects));
  return result;
}

// An absent `end_from_fraction` is the default end window
// (core::kDefaultContactEndFromFraction); 0 is the whole move.
[[nodiscard]] core::CollisionContactRule contact(const CanonicalValue &value,
                                                 std::string_view path) {
  constexpr std::string_view reason = "collision_rule_value_invalid";
  const auto &fields = object(value, reason, path);
  require_keys(fields, {"max_penetration_m", "end_from_fraction", "max_compression_step_m"},
               reason, path);
  core::CollisionContactRule result;
  if (const auto *depth = find(fields, "max_penetration_m"))
    result.max_penetration_m = number(*depth, 0.0, kMaximumReleaseMetricM,
                                      child(path, "max_penetration_m"));
  if (const auto *fraction = find(fields, "end_from_fraction"))
    result.end_from_fraction =
        number(*fraction, 0.0, 1.0, child(path, "end_from_fraction"));
  // A spring (OD-356): the step limit comes with its stroke.
  if (const auto *step = find(fields, "max_compression_step_m")) {
    result.max_compression_step_m = number(*step, 0.0, kMaximumReleaseMetricM,
                                           child(path, "max_compression_step_m"));
    if (!result.max_penetration_m || !(*result.max_compression_step_m > 0.0))
      fail(reason, child(path, "max_compression_step_m"),
           "needs max_penetration_m (the stroke) and a value above 0");
  }
  return result;
}

// Absent keys take the release envelope's defaults for its evidence mode:
// signed evidence checks a start limit of 0 and a decreasing depth; binary
// evidence carries no signed limit at all.
[[nodiscard]] core::CollisionReleaseRule release(const CanonicalValue &value,
                                                 std::string_view path) {
  constexpr std::string_view reason = "collision_rule_value_invalid";
  const auto &fields = object(value, reason, path);
  require_keys(fields,
               {"release_evidence_mode", "enforce_max_start_penetration",
                "max_start_penetration_m", "min_release_progress_m",
                "release_tolerance_m", "require_strictly_decreasing_penetration",
                "require_separation_by_end", "allow_recontact"},
               reason, path);
  bool binary = false;
  if (const auto *mode = find(fields, "release_evidence_mode")) {
    const auto *text = std::get_if<std::string>(&mode->storage());
    if (text == nullptr ||
        (*text != "signed_penetration" && *text != "binary_contact"))
      fail(reason, child(path, "release_evidence_mode"),
           "expected signed_penetration or binary_contact");
    binary = *text == "binary_contact";
  }
  const auto metric = [&](std::string_view name, double fallback) {
    const auto *member = find(fields, name);
    return member ? number(*member, 0.0, kMaximumReleaseMetricM,
                           child(path, name))
                  : fallback;
  };
  const auto flag = [&](std::string_view name, bool fallback) {
    const auto *member = find(fields, name);
    if (member == nullptr) return fallback;
    const auto *result = std::get_if<bool>(&member->storage());
    if (result == nullptr) fail(reason, child(path, name), "expected a boolean");
    return *result;
  };
  core::CollisionReleaseRule result;
  result.release_evidence_mode =
      binary ? core::ContactReleaseEvidenceMode::kBinaryContact
             : core::ContactReleaseEvidenceMode::kSignedPenetration;
  result.enforce_max_start_penetration =
      flag("enforce_max_start_penetration", !binary);
  result.max_start_penetration_m = metric("max_start_penetration_m", 0.0);
  result.min_release_progress_m = metric("min_release_progress_m", 0.0);
  result.release_tolerance_m = metric("release_tolerance_m", binary ? 0.0 : 1e-5);
  result.require_strictly_decreasing_penetration =
      flag("require_strictly_decreasing_penetration", !binary);
  result.require_separation_by_end = flag("require_separation_by_end", true);
  result.allow_recontact = flag("allow_recontact", false);
  if (binary &&
      (result.enforce_max_start_penetration ||
       result.max_start_penetration_m != 0.0 ||
       result.min_release_progress_m != 0.0 ||
       result.release_tolerance_m != 0.0 ||
       result.require_strictly_decreasing_penetration))
    fail(reason, path,
         "binary_contact evidence carries no start limit, progress, tolerance "
         "or decreasing-depth requirement");
  return result;
}

// A move row's `spring_slider {component_id, element_id}` (OD-371).
[[nodiscard]] core::ProgramCollisionBodyReferenceDTO
spring_slider(const CanonicalValue &value, std::string_view path) {
  constexpr std::string_view reason = "collision_rule_selection_invalid";
  const auto &fields = object(value, reason, path);
  require_keys(fields, {"component_id", "element_id"}, reason, path);
  return {core::ControllableComponentId(identity(
              fields, "component_id", kMaximumIdentityBytes, reason, path)),
          core::CollisionBodyRoleId(identity(
              fields, "element_id", kMaximumIdentityBytes, reason, path))};
}

[[nodiscard]] CanonicalValue
selection_value(const core::CollisionRuleSelectionDTO &selection) {
  return std::visit(
      [](const auto &value) {
        using Selection = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Selection,
                                     core::CollisionRuleComponentSelection>) {
          return CanonicalValue(Object{
              {"component_id", CanonicalValue(value.component_id.value())},
              {"kind", CanonicalValue("component")}});
        } else if constexpr (std::is_same_v<
                                 Selection,
                                 core::CollisionRuleCollisionBodySelection>) {
          return CanonicalValue(Object{
              {"collision_body_id",
               CanonicalValue(value.body.collision_body_id.value())},
              {"component_id", CanonicalValue(value.body.component_id.value())},
              {"kind", CanonicalValue("collision_body")}});
        } else if constexpr (std::is_same_v<Selection,
                                            core::CollisionRuleLinkSelection>) {
          return CanonicalValue(Object{
              {"component_id", CanonicalValue(value.component_id.value())},
              {"kind", CanonicalValue("link")},
              {"link", CanonicalValue(value.link)}});
        } else if constexpr (std::is_same_v<
                                 Selection,
                                 core::CollisionRuleAttachedObjectSelection>) {
          Object fields;
          if (value.collision_body_role_id.has_value())
            fields.emplace_back("collision_body_role_id",
                                CanonicalValue(*value.collision_body_role_id));
          fields.emplace_back("component_id",
                              CanonicalValue(value.component_id.value()));
          fields.emplace_back("kind", CanonicalValue("attached_object"));
          return CanonicalValue(std::move(fields));
        } else if constexpr (std::is_same_v<
                                 Selection,
                                 core::CollisionRuleSceneObjectsSelection>) {
          return CanonicalValue(
              Object{{"kind", CanonicalValue("scene_objects")}});
        } else if constexpr (std::is_same_v<
                                 Selection,
                                 core::CollisionRuleSceneObjectSelection>) {
          return CanonicalValue(Object{
              {"kind", CanonicalValue("scene_object")},
              {"scene_resource_id",
               CanonicalValue(value.scene_resource_id.value())}});
        } else if constexpr (std::is_same_v<
                                 Selection,
                                 core::CollisionRuleModelGroupSelection>) {
          return CanonicalValue(Object{
              {"component_id", CanonicalValue(value.component_id.value())},
              {"group", CanonicalValue(value.group)},
              {"kind", CanonicalValue("model_group")}});
        } else {
          return CanonicalValue(
              Object{{"kind", CanonicalValue("active_tcp_contact_bodies")}});
        }
      },
      selection);
}

[[nodiscard]] CanonicalValue
selections_value(const std::vector<core::CollisionRuleSelectionDTO> &values) {
  Array result;
  result.reserve(values.size());
  for (const auto &value : values) result.push_back(selection_value(value));
  return CanonicalValue(std::move(result));
}

} // namespace

DecodedCollisionRules decode_collision_rules(const CanonicalValue &value,
                                             CollisionRuleRows rows,
                                             std::string_view path) {
  const auto &items = array(value, "collision_rule_value_invalid", path);
  if (items.size() > core::kMaxCollisionRules)
    fail("collision_rules_bound_exceeded", path,
         "at most " + std::to_string(core::kMaxCollisionRules) + " rules");
  DecodedCollisionRules result;
  result.rows.reserve(items.size());
  std::set<std::string, std::less<>> rule_ids;
  bool everything_row = false;
  for (std::size_t index = 0U; index < items.size(); ++index) {
    const std::string row_path = item(path, index);
    const auto &fields =
        object(items[index], "collision_rule_value_invalid", row_path);
    require_keys(fields,
                 {"rule_id", "a", "b", "clearance_m", "contact", "release",
                  "leg", "spring_slider"},
                 "collision_rule_value_invalid", row_path);
    DecodedCollisionRuleRow row;
    row.rule.rule_id = identity(fields, "rule_id", core::kMaxCollisionRuleIdBytes,
                                "collision_rule_id_invalid", row_path);
    if (!rule_ids.insert(row.rule.rule_id).second)
      fail("collision_rule_id_duplicate", child(row_path, "rule_id"),
           "rule id '" + row.rule.rule_id + "' is used twice");

    const auto *a = find(fields, "a");
    const auto *b = find(fields, "b");
    const auto *slider = find(fields, "spring_slider");
    const auto *clearance = find(fields, "clearance_m");
    const auto *contact_value = find(fields, "contact");
    const auto *release_value = find(fields, "release");
    if (slider != nullptr) {
      // A row that switches a spring slider on: the slider in place of `a`,
      // `b`, and a contact holding end_from_fraction only.
      if (rows == CollisionRuleRows::kCell)
        fail("collision_rule_selection_invalid", child(row_path, "spring_slider"),
             "spring_slider is available on a move's rules only");
      if (a != nullptr || b == nullptr)
        fail("collision_rule_selection_invalid", row_path,
             "a spring_slider row has b and no a");
      if (clearance != nullptr || release_value != nullptr)
        fail("collision_rule_value_invalid", row_path,
             "a spring_slider row has at most a contact with end_from_fraction");
      row.spring_slider = spring_slider(*slider, child(row_path, "spring_slider"));
      row.rule.b = selections(*b, rows, child(row_path, "b"),
                              result.blackboard_scene_objects);
      core::CollisionContactRule value;
      if (contact_value != nullptr) {
        const auto contact_path = child(row_path, "contact");
        require_keys(object(*contact_value, "collision_rule_value_invalid",
                            contact_path),
                     {"end_from_fraction"}, "collision_rule_value_invalid",
                     contact_path);
        value = contact(*contact_value, contact_path);
      }
      row.rule.value = value;
    } else {
      if ((a == nullptr) != (b == nullptr))
        fail("collision_rule_selection_invalid", row_path,
             "a and b are given together");
      if (a != nullptr) {
        row.rule.a = selections(*a, rows, child(row_path, "a"),
                                result.blackboard_scene_objects);
        row.rule.b = selections(*b, rows, child(row_path, "b"),
                                result.blackboard_scene_objects);
      }

      if ((clearance != nullptr) + (contact_value != nullptr) +
              (release_value != nullptr) !=
          1)
        fail("collision_rule_value_invalid", row_path,
             "a rule has exactly one of clearance_m, contact and release");
      if (clearance != nullptr) {
        row.rule.value = core::CollisionClearanceRule{
            number(*clearance, 0.0, core::kMaxCollisionClearanceM,
                   child(row_path, "clearance_m"))};
      } else if (contact_value != nullptr) {
        row.rule.value = contact(*contact_value, child(row_path, "contact"));
      } else {
        row.rule.value = release(*release_value, child(row_path, "release"));
      }

      if (a == nullptr) {
        if (clearance == nullptr)
          fail("collision_rule_value_invalid", row_path,
               "a rule without selections sets clearance_m only");
        if (everything_row)
          fail("collision_rule_selection_invalid", row_path,
               "only one rule may be without selections");
        if (rows == CollisionRuleRows::kCell && index != 0U)
          fail("collision_rule_selection_invalid", row_path,
               "the rule without selections is the first rule");
        everything_row = true;
      } else if (rows == CollisionRuleRows::kCell && index == 0U) {
        fail("collision_rule_selection_invalid", row_path,
             "the first rule of a Deployment has no selections");
      }
    }

    const auto *leg = find(fields, "leg");
    if (rows == CollisionRuleRows::kSequence) {
      const auto *text =
          leg ? std::get_if<std::string>(&leg->storage()) : nullptr;
      if (text == nullptr ||
          (*text != "approach" && *text != "descend" && *text != "retreat"))
        fail("collision_rule_value_invalid", child(row_path, "leg"),
             "expected approach, descend or retreat");
      row.leg = *text;
    } else if (leg != nullptr) {
      fail("collision_rule_value_invalid", child(row_path, "leg"),
           "only a pick or place sequence rule names a leg");
    }
    result.rows.push_back(std::move(row));
  }
  if (rows == CollisionRuleRows::kCell && !everything_row)
    fail("collision_rule_selection_invalid", path,
         "a Deployment's first rule is the rule without selections");
  return result;
}

std::vector<core::CollisionRuleDTO>
decode_cell_collision_rules(const CanonicalValue &value,
                            std::string_view path) {
  auto decoded = decode_collision_rules(value, CollisionRuleRows::kCell, path);
  std::vector<core::CollisionRuleDTO> result;
  result.reserve(decoded.rows.size());
  for (auto &row : decoded.rows) result.push_back(std::move(row.rule));
  return result;
}

CanonicalValue
encode_collision_rules(const std::vector<core::CollisionRuleDTO> &rules) {
  Array rows;
  rows.reserve(rules.size());
  for (const auto &rule : rules) {
    Object row;
    if (!rule.a.empty()) row.emplace_back("a", selections_value(rule.a));
    if (!rule.b.empty()) row.emplace_back("b", selections_value(rule.b));
    if (const auto *clearance =
            std::get_if<core::CollisionClearanceRule>(&rule.value)) {
      row.emplace_back("clearance_m", CanonicalValue(clearance->clearance_m));
    } else if (const auto *contact_rule =
                   std::get_if<core::CollisionContactRule>(&rule.value)) {
      Object fields;
      fields.emplace_back("end_from_fraction",
                          CanonicalValue(contact_rule->end_from_fraction));
      if (contact_rule->max_penetration_m.has_value())
        fields.emplace_back("max_penetration_m",
                            CanonicalValue(*contact_rule->max_penetration_m));
      if (contact_rule->max_compression_step_m.has_value())
        fields.emplace_back("max_compression_step_m",
                            CanonicalValue(*contact_rule->max_compression_step_m));
      row.emplace_back("contact", CanonicalValue(std::move(fields)));
    } else {
      const auto &value = std::get<core::CollisionReleaseRule>(rule.value);
      row.emplace_back(
          "release",
          CanonicalValue(Object{
              {"allow_recontact", CanonicalValue(value.allow_recontact)},
              {"enforce_max_start_penetration",
               CanonicalValue(value.enforce_max_start_penetration)},
              {"max_start_penetration_m",
               CanonicalValue(value.max_start_penetration_m)},
              {"min_release_progress_m",
               CanonicalValue(value.min_release_progress_m)},
              {"release_evidence_mode",
               CanonicalValue(value.release_evidence_mode ==
                                      core::ContactReleaseEvidenceMode::
                                          kBinaryContact
                                  ? "binary_contact"
                                  : "signed_penetration")},
              {"release_tolerance_m",
               CanonicalValue(value.release_tolerance_m)},
              {"require_separation_by_end",
               CanonicalValue(value.require_separation_by_end)},
              {"require_strictly_decreasing_penetration",
               CanonicalValue(value.require_strictly_decreasing_penetration)},
          }));
    }
    row.emplace_back("rule_id", CanonicalValue(rule.rule_id));
    rows.emplace_back(std::move(row));
  }
  return CanonicalValue(std::move(rows));
}

} // namespace manyforge::deployment
