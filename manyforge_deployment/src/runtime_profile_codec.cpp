#include "manyforge_deployment/runtime_profile_codec.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "catalog_record_referent_internal.hpp"
#include "content_identity_internal.hpp"
#include "manyforge_deployment/closed_schema.hpp"

namespace manyforge::deployment {
namespace {

[[noreturn]] void fail(std::string message) {
  throw RuntimeProfileCodecError(std::move(message));
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue& object, std::string_view name) {
  const auto* member = object.find_object_member(name);
  if (member == nullptr) {
    fail("runtime-profile normalized value is missing " + std::string(name));
  }
  return *member;
}

[[nodiscard]] const std::string& require_string(
    const CanonicalValue& value, std::string_view name) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail("runtime-profile normalized field is not a string: " +
         std::string(name));
  }
  return *text;
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view name) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail("runtime-profile normalized field is not an array: " +
         std::string(name));
  }
  return *array;
}

[[nodiscard]] core::ValueSchemaRefDTO decode_value_schema_ref(
    const CanonicalValue& value) {
  return {
      core::SchemaId(require_string(require_member(value, "schema_id"),
                                    "configuration_schema_ref.schema_id")),
      core::SchemaRevision(require_string(
          require_member(value, "schema_revision"),
          "configuration_schema_ref.schema_revision")),
  };
}

[[nodiscard]] core::RuntimeLifecycleControl decode_lifecycle_control(
    const CanonicalValue& value) {
  const auto& token = require_string(
      require_member(value, "lifecycle_control"),
      "process_contract.lifecycle_control");
  if (token == "manager_managed") {
    return core::RuntimeLifecycleControl::kManagerManaged;
  }
  if (token == "externally_managed") {
    return core::RuntimeLifecycleControl::kExternallyManaged;
  }
  fail("runtime-profile lifecycle_control is outside the closed enum");
}

template <typename StrongId>
[[nodiscard]] std::vector<StrongId> decode_role_ids(
    const CanonicalValue& value, std::string_view field_name) {
  const auto& source = require_array(value, field_name);
  std::vector<StrongId> roles;
  roles.reserve(source.size());
  for (const auto& item : source) {
    roles.emplace_back(require_string(item, field_name));
  }
  return roles;
}

[[nodiscard]] core::RuntimeProfileDTO decode_normalized(
    const CanonicalValue& normalized) {
  auto source_endpoint_role_ids = decode_role_ids<core::EndpointRoleId>(
      require_member(normalized, "source_endpoint_role_ids"),
      "source_endpoint_role_ids");
  core::RuntimeSupervisionContractDTO supervision_contract{
      core::EndpointRoleId(require_string(
          require_member(require_member(normalized, "supervision_contract"),
                         "status_endpoint_role_id"),
          "supervision_contract.status_endpoint_role_id"))};
  if (!std::binary_search(source_endpoint_role_ids.begin(),
                          source_endpoint_role_ids.end(),
                          supervision_contract.status_endpoint_role_id)) {
    fail("runtime-profile status_endpoint_role_id must occur in "
         "source_endpoint_role_ids");
  }

  return {
      core::RuntimeProfileId(require_string(
          require_member(normalized, "runtime_profile_id"),
          "runtime_profile_id")),
      core::RuntimeProfileRevision(require_string(
          require_member(normalized, "runtime_profile_revision"),
          "runtime_profile_revision")),
      decode_value_schema_ref(
          require_member(normalized, "configuration_schema_ref")),
      core::RuntimeProcessContractDTO{decode_lifecycle_control(
          require_member(normalized, "process_contract"))},
      std::move(supervision_contract),
      decode_role_ids<core::RuntimeRoleId>(
          require_member(normalized, "offered_runtime_role_ids"),
          "offered_runtime_role_ids"),
      std::move(source_endpoint_role_ids),
  };
}

void validate_configuration_schema(
    const core::ValueSchemaRefDTO& schema_ref) {
  const auto* selected =
      build_qualified_closed_schema_registry().find_qualified_value_schema(
          schema_ref.schema_id, schema_ref.schema_revision);
  if (selected == nullptr) {
    fail("runtime-profile selects an unqualified configuration value schema");
  }
  const auto root_shape = selected->compiled_schema().root_shape();
  if (root_shape != ClosedSchemaRootShape::kObject &&
      root_shape != ClosedSchemaRootShape::kTaggedUnion) {
    fail("runtime-profile configuration value schema must have an object or "
         "tagged-union root");
  }
}

}  // namespace

QualifiedRuntimeProfile resolve_and_qualify_runtime_profile(
    const core::RuntimeProfileCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes) {
  UnqualifiedSchemaNormalizedArtifactCandidate normalized =
      [&reference, &resolved_store_key, stored_bytes]() {
        try {
          return detail::resolve_and_normalize_exact_catalog_referent(
              reference, resolved_store_key, stored_bytes,
              core::kRuntimeProfileApiVersion, "runtime profile");
        } catch (const detail::ExactCatalogReferentError& error) {
          fail(error.what());
        }
      }();

  const detail::ContentIdentitySpec identity_spec{
      detail::catalog_record_identity_domain("runtime_profile"),
      std::string(core::kRuntimeProfileApiVersion),
      "runtime_profile_revision"};
  if (!detail::verify_content_identity(normalized.normalized_root(),
                                       identity_spec)) {
    fail("runtime_profile_revision does not match normalized semantic content");
  }

  auto decoded = decode_normalized(normalized.normalized_root());
  validate_configuration_schema(decoded.configuration_schema_ref);
  if (decoded.runtime_profile_id != reference.record_id ||
      decoded.runtime_profile_revision != reference.record_revision) {
    fail("runtime-profile typed ID/revision selector does not match its "
         "referent");
  }
  return QualifiedRuntimeProfile(reference, std::move(decoded),
                                 normalized.normalized_root());
}

}  // namespace manyforge::deployment
