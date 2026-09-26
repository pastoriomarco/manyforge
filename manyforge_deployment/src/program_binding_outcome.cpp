#include "manyforge_deployment/program_binding_outcome.hpp"

#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/canonical_value.hpp>

namespace manyforge::deployment {
namespace {

using Array = CanonicalValue::Array;
using Object = CanonicalValue::Object;

constexpr std::string_view kOutcomeApiVersion =
    "manyforge.program-binding-outcome.v1";
constexpr std::string_view kOutcomeIdentityDomain =
    "manyforge.program-binding-outcome";

[[noreturn]] void Fail(const std::string &detail) {
  throw ProgramBindingOutcomeError(detail);
}

constexpr ProgramBindingUnresolvedReason kReasons[] = {
    ProgramBindingUnresolvedReason::kComponentMissing,
    ProgramBindingUnresolvedReason::kComponentIncompatible,
    ProgramBindingUnresolvedReason::kTargetMissing,
    ProgramBindingUnresolvedReason::kTargetIncompatible,
    ProgramBindingUnresolvedReason::kCapabilityUnavailable,
    ProgramBindingUnresolvedReason::kComponentRoleUnresolved,
};

template <typename Value>
[[nodiscard]] bool SortedUnique(const std::vector<Value> &values) {
  return std::adjacent_find(values.begin(), values.end(),
                            [](const auto &lhs, const auto &rhs) {
                              return !(lhs < rhs);
                            }) == values.end();
}

[[nodiscard]] auto RowKey(const ProgramBindingUnresolvedRoleDTO &row) {
  return std::make_tuple(row.component_role_id.value(),
                         row.target_role_id.has_value(),
                         row.target_role_id.has_value()
                             ? row.target_role_id->value()
                             : std::string{});
}

[[nodiscard]] CanonicalValue OptionalText(const std::optional<std::string> &value) {
  return value.has_value() ? CanonicalValue(*value) : CanonicalValue(nullptr);
}

[[nodiscard]] CanonicalValue OutcomeValue(const ProgramNonExecutableOutcomeDTO &value) {
  Array rows;
  for (const auto &row : value.unresolved_roles) {
    rows.emplace_back(Object{
        {"command_target_id",
         OptionalText(row.command_target_id.has_value()
                          ? std::optional(row.command_target_id->value())
                          : std::nullopt)},
        {"component_role_id", CanonicalValue(row.component_role_id.value())},
        {"controllable_component_id",
         CanonicalValue(row.controllable_component_id.value())},
        {"detail", CanonicalValue(row.detail)},
        {"reason", CanonicalValue(std::string(to_string(row.reason)))},
        {"target_role_id",
         OptionalText(row.target_role_id.has_value()
                          ? std::optional(row.target_role_id->value())
                          : std::nullopt)},
    });
  }
  Array nodes;
  for (const auto &node : value.affected_nodes) nodes.emplace_back(node.value());
  Array groups;
  for (const auto &group : value.affected_execution_groups) groups.emplace_back(group);
  return CanonicalValue(Object{
      {"affected_execution_groups", CanonicalValue(std::move(groups))},
      {"affected_nodes", CanonicalValue(std::move(nodes))},
      {"api_version", CanonicalValue(std::string(kOutcomeApiVersion))},
      {"graph_revision", CanonicalValue(value.graph_revision.value())},
      {"program_id", CanonicalValue(value.program_id.value())},
      {"program_revision", CanonicalValue(value.program_revision.value())},
      {"retained_bundle_id", CanonicalValue(value.retained_bundle_id.value())},
      {"role_assignment_digest", CanonicalValue(value.role_assignment_digest.value())},
      {"unresolved_roles", CanonicalValue(std::move(rows))},
  });
}

[[nodiscard]] const CanonicalValue &Required(const CanonicalValue &object,
                                             std::string_view name) {
  const auto *member = object.is_object() ? object.find_object_member(name) : nullptr;
  if (member == nullptr) Fail("Program outcome lacks member " + std::string(name));
  return *member;
}

[[nodiscard]] std::string Text(const CanonicalValue &object, std::string_view name) {
  const auto &value = Required(object, name);
  if (!value.is_string()) Fail("Program outcome member " + std::string(name) + " is not text");
  return value.as_string();
}

[[nodiscard]] std::optional<std::string> OptionalTextMember(const CanonicalValue &object,
                                                            std::string_view name) {
  const auto &value = Required(object, name);
  if (value.is_null()) return std::nullopt;
  if (!value.is_string()) Fail("Program outcome member " + std::string(name) + " is not text");
  return value.as_string();
}

[[nodiscard]] const CanonicalValue::Array &ArrayMember(const CanonicalValue &object,
                                                       std::string_view name) {
  const auto &value = Required(object, name);
  if (!value.is_array()) Fail("Program outcome member " + std::string(name) + " is not an array");
  return value.as_array();
}

[[nodiscard]] ProgramBindingUnresolvedReason DecodeReason(const std::string &text) {
  for (const auto reason : kReasons) {
    if (to_string(reason) == text) return reason;
  }
  Fail("Program outcome names an unknown unresolved reason");
}

}  // namespace

std::string_view to_string(const ProgramBindingUnresolvedReason reason) noexcept {
  switch (reason) {
    case ProgramBindingUnresolvedReason::kComponentMissing:
      return "component_missing";
    case ProgramBindingUnresolvedReason::kComponentIncompatible:
      return "component_incompatible";
    case ProgramBindingUnresolvedReason::kTargetMissing:
      return "target_missing";
    case ProgramBindingUnresolvedReason::kTargetIncompatible:
      return "target_incompatible";
    case ProgramBindingUnresolvedReason::kCapabilityUnavailable:
      return "capability_unavailable";
    case ProgramBindingUnresolvedReason::kComponentRoleUnresolved:
      return "component_role_unresolved";
  }
  return "";
}

bool ProgramBindingUnresolvedRoleDTO::valid() const noexcept {
  const bool target_row = target_role_id.has_value();
  const bool target_reason =
      reason == ProgramBindingUnresolvedReason::kTargetMissing ||
      reason == ProgramBindingUnresolvedReason::kTargetIncompatible ||
      reason == ProgramBindingUnresolvedReason::kCapabilityUnavailable ||
      reason == ProgramBindingUnresolvedReason::kComponentRoleUnresolved;
  return !component_role_id.empty() && !controllable_component_id.empty() &&
         (!target_row || !target_role_id->empty()) &&
         target_row == command_target_id.has_value() &&
         (!command_target_id.has_value() || !command_target_id->empty()) &&
         target_row == target_reason && !to_string(reason).empty() &&
         detail.size() <= kMaximumProgramBindingUnresolvedDetailBytes &&
         detail.find('\0') == std::string::npos;
}

bool ProgramNonExecutableOutcomeDTO::valid() const noexcept {
  return !program_id.empty() && !program_revision.empty() &&
         !retained_bundle_id.empty() && !graph_revision.empty() &&
         content_identity::is_sha256_digest(role_assignment_digest.value()) &&
         !unresolved_roles.empty() &&
         unresolved_roles.size() <= kMaximumProgramBindingUnresolvedRows &&
         std::all_of(unresolved_roles.begin(), unresolved_roles.end(),
                     [](const auto &row) { return row.valid(); }) &&
         std::adjacent_find(unresolved_roles.begin(), unresolved_roles.end(),
                            [](const auto &lhs, const auto &rhs) {
                              return !(RowKey(lhs) < RowKey(rhs));
                            }) == unresolved_roles.end() &&
         affected_nodes.size() <= kMaximumProgramBindingAffectedNodes &&
         SortedUnique(affected_nodes) &&
         affected_execution_groups.size() <= kMaximumProgramBindingAffectedGroups &&
         SortedUnique(affected_execution_groups) &&
         std::none_of(affected_execution_groups.begin(),
                      affected_execution_groups.end(),
                      [](const auto &group) { return group.empty() || group == "all"; });
}

std::string EncodeProgramNonExecutableOutcome(const ProgramNonExecutableOutcomeDTO &value) {
  if (!value.valid()) Fail("Program non-executable outcome is invalid");
  return canonical_json(OutcomeValue(value));
}

ProgramNonExecutableOutcomeDTO DecodeProgramNonExecutableOutcome(const std::string_view encoded) {
  try {
    const auto root = content_identity::parse_canonical_json(encoded);
    if (!root.is_object() || root.as_object().size() != 9U ||
        Text(root, "api_version") != kOutcomeApiVersion) {
      Fail("Program outcome envelope is not exact");
    }
    ProgramNonExecutableOutcomeDTO value{
        core::ProgramId(Text(root, "program_id")),
        core::ProgramRevision(Text(root, "program_revision")),
        core::RetainedBundleId(Text(root, "retained_bundle_id")),
        core::GraphRevision(Text(root, "graph_revision")),
        core::ContentDigest(Text(root, "role_assignment_digest")),
        {}, {}, {}};
    for (const auto &row : ArrayMember(root, "unresolved_roles")) {
      if (!row.is_object() || row.as_object().size() != 6U) {
        Fail("Program outcome row is not exact");
      }
      ProgramBindingUnresolvedRoleDTO decoded{
          core::ComponentRoleId(Text(row, "component_role_id")),
          std::nullopt,
          core::ControllableComponentId(Text(row, "controllable_component_id")),
          std::nullopt,
          ProgramBindingUnresolvedReason::kComponentMissing,
          {}};
      if (auto target = OptionalTextMember(row, "target_role_id")) {
        decoded.target_role_id = core::TargetRoleId(std::move(*target));
      }
      if (auto target = OptionalTextMember(row, "command_target_id")) {
        decoded.command_target_id = core::CommandTargetId(std::move(*target));
      }
      decoded.reason = DecodeReason(Text(row, "reason"));
      decoded.detail = Text(row, "detail");
      value.unresolved_roles.push_back(std::move(decoded));
    }
    for (const auto &node : ArrayMember(root, "affected_nodes")) {
      if (!node.is_string()) Fail("Program outcome node is not text");
      value.affected_nodes.emplace_back(node.as_string());
    }
    for (const auto &group : ArrayMember(root, "affected_execution_groups")) {
      if (!group.is_string()) Fail("Program outcome group is not text");
      value.affected_execution_groups.push_back(group.as_string());
    }
    if (!value.valid() || canonical_json(OutcomeValue(value)) != encoded) {
      Fail("Program outcome is not canonical or valid");
    }
    return value;
  } catch (const ProgramBindingOutcomeError &) {
    throw;
  } catch (const std::exception &error) {
    Fail(std::string("Program outcome cannot be decoded: ") + error.what());
  }
}

core::ContentDigest DigestProgramNonExecutableOutcome(
    const ProgramNonExecutableOutcomeDTO &value) {
  if (!value.valid()) Fail("Program non-executable outcome is invalid");
  return core::ContentDigest(content_identity::framed_sha256(
      kOutcomeIdentityDomain, kOutcomeApiVersion, OutcomeValue(value)));
}

core::ProgramRoleAssignmentDTO parse_program_role_assignment(
    const CanonicalValue &value) {
  const auto exact = [](const CanonicalValue &object,
                        const std::set<std::string> &names,
                        std::string_view label) {
    if (!object.is_object() || object.as_object().size() != names.size()) {
      Fail(std::string(label) + " is not an exact object");
    }
    for (const auto &[name, member] : object.as_object()) {
      if (!names.contains(name)) Fail(std::string(label) + " has an unknown field");
    }
  };
  exact(value, {"component_roles", "target_roles"}, "program_role_assignment");
  core::ProgramRoleAssignmentDTO result;
  std::set<std::string> component_roles;
  for (const auto &row : ArrayMember(value, "component_roles")) {
    exact(row, {"component_role_id", "controllable_component_id"},
          "component role assignment");
    const auto role = Text(row, "component_role_id");
    if (!component_roles.insert(role).second) {
      Fail("duplicate component role assignment");
    }
    result.component_roles.push_back(
        {core::ComponentRoleId(role),
         core::ControllableComponentId(Text(row, "controllable_component_id"))});
  }
  std::set<std::string> target_roles;
  for (const auto &row : ArrayMember(value, "target_roles")) {
    exact(row, {"target_role_id", "command_target_id"}, "target role assignment");
    const auto role = Text(row, "target_role_id");
    if (!target_roles.insert(role).second) {
      Fail("duplicate target role assignment");
    }
    result.target_roles.push_back(
        {core::TargetRoleId(role), core::CommandTargetId(Text(row, "command_target_id"))});
  }
  return result;
}

ProgramBindingAffectedScope program_binding_affected_scope(
    const core::ProgramExecutionTableDTO &execution_table,
    const std::vector<core::TargetRoleId> &target_roles) {
  const std::set<core::TargetRoleId> roles(target_roles.begin(), target_roles.end());
  std::set<std::uint32_t> indices;
  std::set<core::ProgramNodeInstanceId> nodes;
  for (std::uint32_t index = 0U; index < execution_table.nodes.size(); ++index) {
    const auto &node = execution_table.nodes[index];
    if (std::any_of(node.target_effects.begin(), node.target_effects.end(),
                    [&](const auto &effect) { return roles.contains(effect.target_role_id); })) {
      indices.insert(index);
      nodes.insert(node.instance_id);
    }
  }
  std::set<std::string> groups;
  for (const auto &selection : execution_table.named_selections) {
    if (std::any_of(selection.node_indices.begin(), selection.node_indices.end(),
                    [&](const auto index) { return indices.contains(index); })) {
      groups.insert(selection.group_id);
    }
  }
  return {{nodes.begin(), nodes.end()}, {groups.begin(), groups.end()}};
}

}  // namespace manyforge::deployment
