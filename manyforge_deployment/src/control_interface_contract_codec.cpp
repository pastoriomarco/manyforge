#include "manyforge_deployment/control_interface_contract_codec.hpp"

#include <manyforge_core/types/catalog.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "catalog_record_referent_internal.hpp"
#include "content_identity_internal.hpp"

namespace manyforge::deployment {
namespace {

[[noreturn]] void fail(std::string message) {
  throw ControlInterfaceContractCodecError(std::move(message));
}

[[nodiscard]] const CanonicalValue& require_member(const CanonicalValue& object,
                                                   std::string_view name) {
  const auto* member = object.find_object_member(name);
  if (member == nullptr) {
    fail("control-interface contract normalized value is missing " + std::string(name));
  }
  return *member;
}

[[nodiscard]] const std::string& require_string(const CanonicalValue& value,
                                                std::string_view name) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail("control-interface contract normalized field is not a string: " + std::string(name));
  }
  return *text;
}

[[nodiscard]] const CanonicalValue::Array& require_array(const CanonicalValue& value,
                                                         std::string_view name) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail("control-interface contract normalized field is not an array: " + std::string(name));
  }
  return *array;
}

[[nodiscard]] std::vector<core::ResourceInterfaceClaimDTO> decode_claims(
    const CanonicalValue& value, std::string_view name) {
  const auto& array = require_array(value, name);
  std::vector<core::ResourceInterfaceClaimDTO> claims;
  claims.reserve(array.size());
  for (const auto& item : array) {
    claims.push_back(core::ResourceInterfaceClaimDTO{
        core::ResourceRoleId(
            require_string(require_member(item, "resource_role_id"), "resource_role_id")),
        require_string(require_member(item, "interface_name"), "interface_name"),
    });
  }
  return claims;
}

[[nodiscard]] std::vector<core::ResourceRoleId> decode_ordering(const CanonicalValue& value) {
  const auto& array = require_array(value, "ordering");
  std::vector<core::ResourceRoleId> ordering;
  ordering.reserve(array.size());
  for (const auto& item : array) {
    ordering.emplace_back(require_string(item, "ordering"));
  }
  return ordering;
}

struct ClaimRoleSummary final {
  std::vector<std::string> grouped_roles;
  std::set<std::string> used_roles;
};

[[nodiscard]] ClaimRoleSummary validate_claim_side(
    const std::vector<core::ResourceInterfaceClaimDTO>& claims,
    const std::vector<core::ResourceRoleId>& ordering, std::string_view side_name) {
  if (claims.size() > core::kMaxControlInterfaceContractItems) {
    fail(std::string(side_name) + " exceeds the control-interface item limit");
  }

  ClaimRoleSummary summary;
  std::set<std::pair<std::string, std::string>> unique_claims;
  std::optional<std::string> active_role;
  for (const auto& claim : claims) {
    const std::string& role = claim.resource_role_id.value();
    if (!unique_claims.emplace(role, claim.interface_name).second) {
      fail(std::string(side_name) + " contains a duplicate resource-role/interface pair");
    }
    if (!active_role.has_value() || *active_role != role) {
      if (!summary.used_roles.insert(role).second) {
        fail(std::string(side_name) + " contains a non-contiguous resource-role group");
      }
      summary.grouped_roles.push_back(role);
      active_role = role;
    }
  }

  std::vector<std::string> expected_groups;
  expected_groups.reserve(summary.grouped_roles.size());
  for (const auto& role : ordering) {
    if (summary.used_roles.contains(role.value())) {
      expected_groups.push_back(role.value());
    }
  }
  if (summary.grouped_roles != expected_groups) {
    fail(std::string(side_name) + " resource-role groups do not follow contract ordering");
  }
  return summary;
}

void validate_semantics(const core::ControlInterfaceContractDTO& contract) {
  if (contract.command_interfaces.empty() && contract.state_interfaces.empty()) {
    fail("control-interface contract cannot have both claim sides empty");
  }
  if (contract.ordering.size() > core::kMaxControlInterfaceContractItems) {
    fail("ordering exceeds the control-interface item limit");
  }

  std::set<std::string> ordered_roles;
  for (const auto& role : contract.ordering) {
    if (!ordered_roles.insert(role.value()).second) {
      fail("ordering contains a duplicate resource role");
    }
  }

  const auto command =
      validate_claim_side(contract.command_interfaces, contract.ordering, "command_interfaces");
  const auto state =
      validate_claim_side(contract.state_interfaces, contract.ordering, "state_interfaces");

  std::set<std::string> used_roles = command.used_roles;
  used_roles.insert(state.used_roles.begin(), state.used_roles.end());
  if (used_roles != ordered_roles) {
    fail("ordering does not equal the exact union of claimed resource roles");
  }
}

[[nodiscard]] core::ControlInterfaceContractDTO decode_normalized(
    const CanonicalValue& normalized) {
  core::ControlInterfaceContractDTO decoded{
      core::ControlInterfaceContractId(
          require_string(require_member(normalized, "control_interface_contract_id"),
                         "control_interface_contract_id")),
      core::ControlInterfaceRevision(require_string(
          require_member(normalized, "control_interface_revision"), "control_interface_revision")),
      decode_claims(require_member(normalized, "command_interfaces"), "command_interfaces"),
      decode_claims(require_member(normalized, "state_interfaces"), "state_interfaces"),
      decode_ordering(require_member(normalized, "ordering")),
  };
  validate_semantics(decoded);
  return decoded;
}

}  // namespace

QualifiedControlInterfaceContract resolve_and_qualify_control_interface_contract(
    const core::ControlInterfaceContractCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key, std::string_view stored_bytes) {
  UnqualifiedSchemaNormalizedArtifactCandidate normalized = [&reference, &resolved_store_key,
                                                             stored_bytes]() {
    try {
      return detail::resolve_and_normalize_exact_catalog_referent(
          reference, resolved_store_key, stored_bytes, core::kControlInterfaceContractApiVersion,
          "control-interface contract");
    } catch (const detail::ExactCatalogReferentError& error) {
      fail(error.what());
    }
  }();

  const detail::ContentIdentitySpec identity_spec{
      detail::catalog_record_identity_domain("control_interface_contract"),
      std::string(core::kControlInterfaceContractApiVersion),
      "control_interface_revision",
  };
  if (!detail::verify_content_identity(normalized.normalized_root(), identity_spec)) {
    fail(
        "control_interface_revision does not match normalized semantic "
        "content");
  }

  auto decoded = decode_normalized(normalized.normalized_root());
  if (decoded.control_interface_contract_id != reference.record_id ||
      decoded.control_interface_revision != reference.record_revision) {
    fail(
        "control-interface contract typed ID/revision selector does not "
        "match its referent");
  }
  return QualifiedControlInterfaceContract(
      reference, std::move(decoded), normalized.normalized_root());
}

}  // namespace manyforge::deployment
