#include <manyforge_deployment/cell_collision_rule_members.hpp>

#include <algorithm>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>

#include <manyforge_deployment/collision_rule_codec.hpp>
#include <manyforge_deployment/component_spec.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::deployment {
namespace {

using Object = CanonicalValue::Object;
using Array = CanonicalValue::Array;
using Member = CollisionRuleMember;

/// The source name of the SRDF group compiled as `compiled_group_name` in
/// `model_instance_id`; empty when the map has none.
[[nodiscard]] std::string source_group_name(const VerifiedRetainedModelMap &model_map,
                                            const core::ModelInstanceId &model_instance_id,
                                            const std::string &compiled_group_name) {
  for (const auto &symbol : model_map.symbols()) {
    if (symbol.source.kind == OccurrenceSymbolKind::kSrdfGroup &&
        symbol.source.model_instance_id == model_instance_id &&
        symbol.compiled_name == compiled_group_name)
      return symbol.source.source_name;
  }
  return {};
}

void add_links(std::vector<Member> &members, const std::string &component_id,
               const std::set<std::string> &links) {
  for (const auto &link : links) members.push_back({Member::Kind::kLink, component_id, link});
}

[[nodiscard]] CanonicalValue member_value(const Member &member) {
  switch (member.kind) {
    case Member::Kind::kLink:
      return CanonicalValue(Object{{"component_id", CanonicalValue(member.component_id)},
                                   {"kind", CanonicalValue("link")},
                                   {"link", CanonicalValue(member.link)}});
    case Member::Kind::kAttachedObject:
      return CanonicalValue(Object{{"component_id", CanonicalValue(member.component_id)},
                                   {"kind", CanonicalValue("attached_object")}});
    case Member::Kind::kSceneObjects:
      break;
  }
  return CanonicalValue(Object{{"kind", CanonicalValue("scene_objects")}});
}

[[nodiscard]] std::string member_text(const Member &member) {
  switch (member.kind) {
    case Member::Kind::kLink:
      return member.component_id + "/" + member.link;
    case Member::Kind::kAttachedObject:
      return "object carried by " + member.component_id;
    case Member::Kind::kSceneObjects:
      break;
  }
  return "Scene objects";
}

struct ResolvedRule final {
  const core::CollisionRuleDTO *rule = nullptr;
  std::vector<Member> a;
  std::vector<Member> b;
};

[[nodiscard]] ResolvedRule resolve(const core::CollisionRuleDTO &rule,
                                   const CellCollisionFacts &facts) {
  return {&rule, collision_rule_side_members(rule.a, facts),
          collision_rule_side_members(rule.b, facts)};
}

[[nodiscard]] CanonicalValue members_value(const std::vector<Member> &members) {
  Array result;
  result.reserve(members.size());
  for (const auto &member : members) result.push_back(member_value(member));
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue state_value(const ResolvedRule &resolved) {
  return CanonicalValue(Object{
      {"a", members_value(resolved.a)},
      {"b", members_value(resolved.b)},
      {"rule", encode_collision_rules({*resolved.rule}).as_array().front()},
  });
}

}  // namespace

CellCollisionFacts cell_collision_facts(const VerifiedRetainedModelMap &model_map) {
  CellCollisionFacts facts;
  for (const auto &body : model_map.model_collision_bodies()) {
    if (!body.owner_component_id) continue;
    auto &component = facts.components[body.owner_component_id->value()];
    auto &role = component.collision_bodies[body.collision_body_role_id.value()];
    for (const auto &link : body.compiled_link_names) {
      component.links.insert(link);
      role.insert(link);
    }
  }
  for (const auto &target : model_map.targets()) {
    auto &component = facts.components[target.controllable_component_id.value()];
    for (const auto &group : target.model_groups) {
      const auto name =
          source_group_name(model_map, group.model_instance_id, group.compiled_group_name);
      if (name.empty()) continue;
      auto &links = component.model_groups[name];
      links.insert(group.expanded_compiled_link_names.begin(),
                   group.expanded_compiled_link_names.end());
    }
  }
  return facts;
}

std::vector<std::string> model_group_compiled_links(
    const VerifiedRetainedModelMap &model_map, const core::ControllableComponentId &component_id,
    const std::string_view group) {
  std::set<std::string> links;
  for (const auto &target : model_map.targets()) {
    if (target.controllable_component_id != component_id) continue;
    for (const auto &candidate : target.model_groups) {
      if (source_group_name(model_map, candidate.model_instance_id,
                            candidate.compiled_group_name) != group)
        continue;
      links.insert(candidate.expanded_compiled_link_names.begin(),
                   candidate.expanded_compiled_link_names.end());
    }
  }
  return {links.begin(), links.end()};
}

std::vector<Member> collision_rule_side_members(
    const std::vector<core::CollisionRuleSelectionDTO> &side, const CellCollisionFacts &facts) {
  std::vector<Member> members;
  const auto component_facts = [&](const std::string &component_id)
      -> const CellCollisionComponentFacts * {
    const auto found = facts.components.find(component_id);
    return found == facts.components.end() ? nullptr : &found->second;
  };
  for (const auto &selection : side) {
    std::visit(
        [&](const auto &value) {
          using Selection = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Selection, core::CollisionRuleComponentSelection>) {
            if (const auto *component = component_facts(value.component_id.value()))
              add_links(members, value.component_id.value(), component->links);
          } else if constexpr (std::is_same_v<Selection,
                                              core::CollisionRuleCollisionBodySelection>) {
            const auto &id = value.body.component_id.value();
            const auto *component = component_facts(id);
            if (component == nullptr) return;
            const auto role = component->collision_bodies.find(value.body.collision_body_id.value());
            if (role != component->collision_bodies.end()) add_links(members, id, role->second);
          } else if constexpr (std::is_same_v<Selection, core::CollisionRuleModelGroupSelection>) {
            const auto &id = value.component_id.value();
            const auto *component = component_facts(id);
            if (component == nullptr) return;
            const auto group = component->model_groups.find(value.group);
            if (group == component->model_groups.end()) return;
            for (const auto &link : group->second)
              if (component->links.contains(link))
                members.push_back({Member::Kind::kLink, id, link});
          } else if constexpr (std::is_same_v<Selection, core::CollisionRuleLinkSelection>) {
            members.push_back({Member::Kind::kLink, value.component_id.value(), value.link});
          } else if constexpr (std::is_same_v<Selection,
                                              core::CollisionRuleAttachedObjectSelection>) {
            members.push_back({Member::Kind::kAttachedObject, value.component_id.value(), {}});
          } else if constexpr (std::is_same_v<Selection,
                                              core::CollisionRuleSceneObjectsSelection>) {
            members.push_back({Member::Kind::kSceneObjects, {}, {}});
          }
          // One Scene object and the TCP's contact bodies exist per move;
          // the Deployment's rules cannot name them.
        },
        selection);
  }
  std::sort(members.begin(), members.end());
  members.erase(std::unique(members.begin(), members.end()), members.end());
  return members;
}

std::optional<core::ErrorDTO> cell_collision_rules_overlap(
    const std::vector<core::CollisionRuleDTO> &rules, const CellCollisionFacts &facts) {
  std::map<std::pair<Member, Member>, const core::CollisionRuleDTO *> held;
  for (const auto &rule : rules) {
    if (rule.a.empty() && rule.b.empty()) continue;
    const auto resolved = resolve(rule, facts);
    for (const auto &first : resolved.a) {
      for (const auto &second : resolved.b) {
        if (first == second) continue;
        auto pair = first < second ? std::pair{first, second} : std::pair{second, first};
        const auto [found, inserted] = held.emplace(pair, &rule);
        if (inserted || found->second == &rule) continue;
        const auto body_a = member_text(pair.first);
        const auto body_b = member_text(pair.second);
        return core::ErrorDTO{
            core::kErrorInvalidCollisionRule,
            "Collision rules '" + found->second->rule_id + "' and '" + rule.rule_id +
                "' hold the same pair: " + body_a + " and " + body_b,
            {{"reason", "collision_rules_overlap"},
             {"collision_rule_id", found->second->rule_id},
             {"other_collision_rule_id", rule.rule_id},
             {"body_a", body_a},
             {"body_b", body_b}}};
      }
    }
  }
  return std::nullopt;
}

CanonicalValue cell_collision_rule_changes(const std::vector<core::CollisionRuleDTO> &current,
                                           const CellCollisionFacts &current_facts,
                                           const std::vector<core::CollisionRuleDTO> &candidate,
                                           const CellCollisionFacts &candidate_facts) {
  const auto find = [](const std::vector<core::CollisionRuleDTO> &rules, const std::string &id)
      -> const core::CollisionRuleDTO * {
    const auto found = std::find_if(rules.begin(), rules.end(),
                                    [&](const auto &rule) { return rule.rule_id == id; });
    return found == rules.end() ? nullptr : &*found;
  };
  const auto row = [](const std::string &id, std::string_view change, CanonicalValue now,
                      CanonicalValue was) {
    return CanonicalValue(Object{{"change", CanonicalValue(std::string(change))},
                                 {"now", std::move(now)},
                                 {"rule_id", CanonicalValue(id)},
                                 {"was", std::move(was)}});
  };
  Array rows;
  for (const auto &rule : candidate) {
    const auto now = resolve(rule, candidate_facts);
    const auto *previous = find(current, rule.rule_id);
    if (previous == nullptr) {
      rows.push_back(row(rule.rule_id, "added", state_value(now), CanonicalValue(nullptr)));
      continue;
    }
    const auto was = resolve(*previous, current_facts);
    if (*previous == rule && was.a == now.a && was.b == now.b) continue;
    rows.push_back(row(rule.rule_id, "changed", state_value(now), state_value(was)));
  }
  for (const auto &rule : current) {
    if (find(candidate, rule.rule_id) != nullptr) continue;
    rows.push_back(row(rule.rule_id, "removed", CanonicalValue(nullptr),
                       state_value(resolve(rule, current_facts))));
  }
  return CanonicalValue(std::move(rows));
}

CanonicalValue cell_mesh_depth_reports(const std::vector<core::CollisionRuleDTO> &rules,
                                       const std::vector<RetainedIoSignalConfiguration> &io_signals,
                                       const CellCollisionFacts &facts,
                                       const CellOpenMeshes &open) {
  Array rows;
  // One source's open meshes: its links and its Scene objects, each once.
  const auto report = [&](std::string_view source_key, const std::string &source,
                          std::set<std::pair<std::string, std::string>> links,
                          std::set<std::string> scene_objects, bool all_scene_objects) {
    if (all_scene_objects) {
      for (const auto &entry : open.scene_objects) scene_objects.insert(entry.first);
    }
    const auto row = [&](CanonicalValue member, const std::string &uri) {
      rows.push_back(CanonicalValue(Object{{"code", CanonicalValue("mesh_not_closed")},
                                           {std::string(source_key), CanonicalValue(source)},
                                           {"member", std::move(member)},
                                           {"mesh_uri", CanonicalValue(uri)}}));
    };
    for (const auto &[component_id, link] : links) {
      const auto found = open.links.find(link);
      if (found == open.links.end()) continue;
      row(CanonicalValue(Object{{"component_id", CanonicalValue(component_id)},
                                {"kind", CanonicalValue("link")},
                                {"link", CanonicalValue(link)}}),
          found->second);
    }
    for (const auto &id : scene_objects) {
      const auto found = open.scene_objects.find(id);
      if (found == open.scene_objects.end()) continue;
      row(CanonicalValue(Object{{"kind", CanonicalValue("scene_object")},
                                {"scene_resource_id", CanonicalValue(id)}}),
          found->second);
    }
  };

  for (const auto &rule : rules) {
    const auto *contact = std::get_if<core::CollisionContactRule>(&rule.value);
    const auto *release = std::get_if<core::CollisionReleaseRule>(&rule.value);
    const bool depth =
        (contact != nullptr &&
         (contact->max_penetration_m || contact->max_compression_step_m)) ||
        (release != nullptr && release->release_evidence_mode ==
                                   core::ContactReleaseEvidenceMode::kSignedPenetration);
    if (!depth) continue;
    std::set<std::pair<std::string, std::string>> links;
    bool all_scene_objects = false;
    for (const auto *side : {&rule.a, &rule.b}) {
      for (const auto &member : collision_rule_side_members(*side, facts)) {
        if (member.kind == Member::Kind::kLink) links.emplace(member.component_id, member.link);
        all_scene_objects |= member.kind == Member::Kind::kSceneObjects;
      }
    }
    report("collision_rule_id", rule.rule_id, std::move(links), {}, all_scene_objects);
  }

  for (const auto &signal : io_signals) {
    const auto &provider = signal.provider;
    if (provider.profile != "geometric_contact" || !provider.trip_depth_m) continue;
    std::set<std::pair<std::string, std::string>> links;
    std::set<std::string> scene_objects;
    bool all_scene_objects = false;
    if (provider.target_scene_resource_id && !provider.target_scene_resource_id->value().empty())
      scene_objects.insert(provider.target_scene_resource_id->value());
    for (const auto *side : {&provider.contact_a, &provider.contact_b}) {
      for (const auto &selection : *side) {
        std::visit(
            [&](const auto &value) {
              using Selection = std::decay_t<decltype(value)>;
              if constexpr (std::is_same_v<Selection, core::CollisionRuleComponentSelection>) {
                const auto component = facts.components.find(value.component_id.value());
                if (component == facts.components.end()) return;
                for (const auto &link : component->second.links)
                  links.emplace(value.component_id.value(), link);
              } else if constexpr (std::is_same_v<Selection,
                                                  core::CollisionRuleCollisionBodySelection>) {
                const auto &id = value.body.component_id.value();
                const auto component = facts.components.find(id);
                if (component == facts.components.end()) return;
                const auto role =
                    component->second.collision_bodies.find(value.body.collision_body_id.value());
                if (role == component->second.collision_bodies.end()) return;
                for (const auto &link : role->second) links.emplace(id, link);
              } else if constexpr (std::is_same_v<Selection, core::CollisionRuleLinkSelection>) {
                links.emplace(value.component_id.value(), value.link);
              } else if constexpr (std::is_same_v<Selection,
                                                  core::CollisionRuleSceneObjectsSelection>) {
                all_scene_objects = true;
              } else if constexpr (std::is_same_v<Selection,
                                                  core::CollisionRuleSceneObjectSelection>) {
                scene_objects.insert(value.scene_resource_id.value());
              }
              // A carried object exists per move.
            },
            selection);
      }
    }
    const auto &signal_id = signal.descriptor.signal_id;
    report("signal_id", signal_id.controllable_component_id.value() + "/" + signal_id.name,
           std::move(links),
           std::move(scene_objects), all_scene_objects);
  }
  return CanonicalValue(std::move(rows));
}

std::vector<core::CollisionRuleDTO> bundle_cell_collision_rules(const RetainedBundle &bundle) {
  const auto context = cell_context(bundle);
  // The cell has one execution host; its planner settings hold the rules.
  if (!context.planner.is_object() || context.planner.as_object().empty()) return {};
  return planner_settings(context,
                          core::RuntimeUnitId(context.planner.as_object().front().first))
      .collision_rules;
}

}  // namespace manyforge::deployment
