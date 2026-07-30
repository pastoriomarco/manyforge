#include "deployment_catalog_codec_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "closed_schema_internal.hpp"
#include "compiler_semantic_limits_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

[[noreturn]] void fail(std::string_view path, std::string_view reason) {
  throw DeploymentCatalogCodecError(std::string(path) + ": " +
                                    std::string(reason));
}

void require_output_budget(const CanonicalValue& value,
                           std::string_view path) {
  try {
    validate_canonical_value_output_budget(value);
  } catch (const std::invalid_argument& error) {
    fail(path, error.what());
  }
}

[[nodiscard]] std::string child_path(std::string_view parent,
                                     std::string_view child) {
  return std::string(parent) + "." + std::string(child);
}

[[nodiscard]] std::string index_path(std::string_view parent,
                                     std::size_t index) {
  return std::string(parent) + "[" + std::to_string(index) + "]";
}

[[nodiscard]] const CanonicalValue::Object& require_object(
    const CanonicalValue& value, std::string_view path) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(path, "expected object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view path) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(path, "expected array");
  }
  return *array;
}

[[nodiscard]] const std::string& require_string(
    const CanonicalValue& value, std::string_view path) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail(path, "expected string");
  }
  return *text;
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue::Object& object, std::string_view name,
    std::string_view path) {
  const auto iterator =
      std::find_if(object.begin(), object.end(), [name](const auto& member) {
        return member.first == name;
      });
  if (iterator == object.end()) {
    fail(path, "missing required field '" + std::string(name) + "'");
  }
  return iterator->second;
}

void require_exact_fields(
    const CanonicalValue::Object& object,
    std::initializer_list<std::string_view> expected, std::string_view path) {
  for (const auto& [name, unused] : object) {
    (void)unused;
    if (std::find(expected.begin(), expected.end(), name) == expected.end()) {
      fail(path, "unknown field '" + name + "'");
    }
  }
  for (const std::string_view name : expected) {
    (void)require_member(object, name, path);
  }
}

[[nodiscard]] constexpr bool is_ascii_alphanumeric(
    unsigned char byte) noexcept {
  return (byte >= 'a' && byte <= 'z') ||
         (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9');
}

[[nodiscard]] bool is_portable_token(std::string_view value) noexcept {
  if (value.empty() ||
      value.size() > kMaximumPortableIdentityBytes ||
      !is_ascii_alphanumeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](unsigned char byte) {
    return is_ascii_alphanumeric(byte) || byte == '.' || byte == '_' ||
           byte == '-';
  });
}

void require_portable_token(std::string_view value, std::string_view path) {
  if (!is_portable_token(value)) {
    fail(path,
         "expected a 1..128 byte portable token beginning with an ASCII "
         "alphanumeric and containing only alphanumerics, '.', '_', or '-'");
  }
}

void require_digest(std::string_view value, std::string_view path) {
  if (!is_sha256_digest(value)) {
    fail(path, "expected lowercase sha256:<64 hex> digest");
  }
}

void require_store_key(std::string_view value, std::string_view path) {
  if (value.empty() || value.size() > kMaxArtifactScalarBytes) {
    fail(path, "expected a non-empty bounded content-store key");
  }
}

void require_catalog_record_count(std::size_t record_count) {
  if (record_count > kMaximumCatalogRecords) {
    fail("catalogs.records",
         "catalog record count exceeds the fixed v1 output budget");
  }
}

[[nodiscard]] const core::CatalogRecordTypeDescriptor& parse_kind(
    const CanonicalValue& value, std::string_view path) {
  const std::string& kind = require_string(value, path);
  const auto iterator = std::find_if(
      core::kCatalogRecordTypeRegistry.begin(),
      core::kCatalogRecordTypeRegistry.end(),
      [&kind](const core::CatalogRecordTypeDescriptor& descriptor) {
        return descriptor.serialized_kind == kind;
      });
  if (iterator == core::kCatalogRecordTypeRegistry.end()) {
    fail(path, "record kind is outside the closed v1 catalog registry");
  }
  return *iterator;
}

template <typename Record>
[[nodiscard]] core::CatalogRecordRefDTO parse_typed_record(
    const CanonicalValue::Object& object,
    const core::CatalogRecordTypeDescriptor& descriptor,
    std::string_view path) {
  const std::string id_path = child_path(path, "record_id");
  const std::string revision_path = child_path(path, "record_revision");
  const std::string schema_id_path = child_path(path, "schema_id");
  const std::string schema_revision_path =
      child_path(path, "schema_revision");
  const std::string content_digest_path =
      child_path(path, "content_digest");
  const std::string store_key_path =
      child_path(path, "content_addressed_store_key");

  const std::string& record_id =
      require_string(require_member(object, "record_id", path), id_path);
  require_portable_token(record_id, id_path);

  const std::string& record_revision = require_string(
      require_member(object, "record_revision", path), revision_path);
  require_digest(record_revision, revision_path);

  const std::string& schema_id =
      require_string(require_member(object, "schema_id", path),
                     schema_id_path);
  if (schema_id != descriptor.schema_family) {
    fail(schema_id_path,
         "expected exact schema family '" +
             std::string(descriptor.schema_family) + "'");
  }

  const std::string& schema_revision =
      require_string(require_member(object, "schema_revision", path),
                     schema_revision_path);
  require_digest(schema_revision, schema_revision_path);

  const std::string& content_digest =
      require_string(require_member(object, "content_digest", path),
                     content_digest_path);
  require_digest(content_digest, content_digest_path);

  const std::string& store_key = require_string(
      require_member(object, "content_addressed_store_key", path),
      store_key_path);
  require_store_key(store_key, store_key_path);

  return Record{
      typename Record::IdType(record_id),
      typename Record::RevisionType(record_revision),
      core::SchemaId(schema_id),
      core::SchemaRevision(schema_revision),
      core::ContentDigest(content_digest),
      core::ContentAddressedStoreKey(store_key),
  };
}

[[nodiscard]] core::CatalogRecordRefDTO parse_record(
    const CanonicalValue& value, std::string_view path) {
  const auto& object = require_object(value, path);
  require_exact_fields(
      object,
      {"record_kind", "record_id", "record_revision", "schema_id",
       "schema_revision", "content_digest",
       "content_addressed_store_key"},
      path);
  const auto& descriptor =
      parse_kind(require_member(object, "record_kind", path),
                 child_path(path, "record_kind"));

  switch (descriptor.record_kind) {
    case core::CatalogRecordKind::kModelAsset:
      return parse_typed_record<core::ModelAssetCatalogRecordRefDTO>(
          object, descriptor, path);
    case core::CatalogRecordKind::kRuntimeProfile:
      return parse_typed_record<core::RuntimeProfileCatalogRecordRefDTO>(
          object, descriptor, path);
    case core::CatalogRecordKind::kComponentType:
      return parse_typed_record<core::ComponentTypeCatalogRecordRefDTO>(
          object, descriptor, path);
    case core::CatalogRecordKind::kControlInterfaceContract:
      return parse_typed_record<
          core::ControlInterfaceContractCatalogRecordRefDTO>(
          object, descriptor, path);
    case core::CatalogRecordKind::kControllerTopology:
      return parse_typed_record<core::ControllerTopologyCatalogRecordRefDTO>(
          object, descriptor, path);
    case core::CatalogRecordKind::kProviderManifest:
      return parse_typed_record<core::ProviderManifestCatalogRecordRefDTO>(
          object, descriptor, path);
    case core::CatalogRecordKind::kCommandSchema:
      return parse_typed_record<core::CommandSchemaCatalogRecordRefDTO>(
          object, descriptor, path);
    case core::CatalogRecordKind::kCapabilityDefinition:
      return parse_typed_record<
          core::CapabilityDefinitionCatalogRecordRefDTO>(
          object, descriptor, path);
    case core::CatalogRecordKind::kContactPolicy:
      return parse_typed_record<core::ContactPolicyCatalogRecordRefDTO>(
          object, descriptor, path);
    case core::CatalogRecordKind::kStopEvidencePolicy:
      return parse_typed_record<
          core::StopEvidencePolicyCatalogRecordRefDTO>(
          object, descriptor, path);
    case core::CatalogRecordKind::kRuntimePolicy:
      return parse_typed_record<core::RuntimePolicyCatalogRecordRefDTO>(
          object, descriptor, path);
  }
  fail(child_path(path, "record_kind"),
       "record kind is outside the closed v1 catalog registry");
}

void validate_record(const core::CatalogRecordRefDTO& record,
                     std::string_view path) {
  const auto& descriptor = core::catalog_record_type_descriptor(record);
  const std::string id_path = child_path(path, "record_id");
  const std::string revision_path = child_path(path, "record_revision");
  const std::string schema_id_path = child_path(path, "schema_id");
  const std::string schema_revision_path =
      child_path(path, "schema_revision");
  const std::string content_digest_path =
      child_path(path, "content_digest");
  const std::string store_key_path =
      child_path(path, "content_addressed_store_key");

  require_portable_token(core::catalog_record_id_value(record), id_path);
  require_digest(core::catalog_record_revision_value(record), revision_path);
  std::visit(
      [&](const auto& typed) {
        if (typed.schema_id.value() != descriptor.schema_family) {
          fail(schema_id_path,
               "expected exact schema family '" +
                   std::string(descriptor.schema_family) + "'");
        }
        require_digest(typed.schema_revision.value(), schema_revision_path);
        require_digest(typed.content_digest.value(), content_digest_path);
        require_store_key(typed.content_addressed_store_key.value(),
                          store_key_path);
      },
      record);
}

void consume_scalar_bytes(std::size_t encoded_bytes,
                          std::size_t& aggregate,
                          std::string_view path) {
  if (encoded_bytes > kMaxArtifactEncodedBytes - aggregate) {
    fail(path, "catalog scalar bytes exceed the fixed v1 output budget");
  }
  aggregate += encoded_bytes;
}

void consume_ascii_scalar(std::string_view value, std::size_t& aggregate,
                          std::string_view path) {
  // Every caller has already constrained this value to printable ASCII with
  // no JSON escaping. The surrounding quotation marks are part of the same
  // scalar accounting used by validate_canonical_value_output_budget().
  consume_scalar_bytes(value.size() + 2U, aggregate, path);
}

void consume_general_string_scalar(std::string_view value,
                                   std::size_t& aggregate,
                                   std::string_view path) {
  try {
    const std::size_t encoded_bytes =
        canonical_json(CanonicalValue(std::string(value))).size();
    consume_scalar_bytes(encoded_bytes, aggregate, path);
  } catch (const std::invalid_argument& error) {
    fail(path, error.what());
  }
}

void preflight_encode_budget(
    std::span<const core::CatalogRecordRefDTO> records) {
  static constexpr std::array<std::string_view, 7U> kFieldNames{
      "record_kind",
      "record_id",
      "record_revision",
      "schema_id",
      "schema_revision",
      "content_digest",
      "content_addressed_store_key",
  };

  std::size_t scalar_bytes = 0U;
  consume_ascii_scalar("records", scalar_bytes, "catalogs.records");
  for (std::size_t index = 0U; index < records.size(); ++index) {
    const std::string path = index_path("catalogs.records", index);
    const auto& record = records[index];
    validate_record(record, path);
    for (const std::string_view field_name : kFieldNames) {
      consume_ascii_scalar(field_name, scalar_bytes, path);
    }

    const auto& descriptor = core::catalog_record_type_descriptor(record);
    consume_ascii_scalar(descriptor.serialized_kind, scalar_bytes,
                         child_path(path, "record_kind"));
    consume_ascii_scalar(core::catalog_record_id_value(record), scalar_bytes,
                         child_path(path, "record_id"));
    consume_ascii_scalar(core::catalog_record_revision_value(record),
                         scalar_bytes,
                         child_path(path, "record_revision"));
    std::visit(
        [&](const auto& typed) {
          consume_ascii_scalar(typed.schema_id.value(), scalar_bytes,
                               child_path(path, "schema_id"));
          consume_ascii_scalar(typed.schema_revision.value(), scalar_bytes,
                               child_path(path, "schema_revision"));
          consume_ascii_scalar(typed.content_digest.value(), scalar_bytes,
                               child_path(path, "content_digest"));
          consume_general_string_scalar(
              typed.content_addressed_store_key.value(), scalar_bytes,
              child_path(path, "content_addressed_store_key"));
        },
        record);
  }
}

[[nodiscard]] std::vector<core::CatalogRecordRefDTO>
canonicalize_records(std::vector<core::CatalogRecordRefDTO> records,
                     std::string_view path) {
  for (std::size_t index = 0U; index < records.size(); ++index) {
    validate_record(records[index], index_path(path, index));
  }

  const core::CanonicalCatalogRecordRefLess less;
  std::sort(records.begin(), records.end(), less);
  for (std::size_t index = 1U; index < records.size(); ++index) {
    const auto& previous = records[index - 1U];
    const auto& current = records[index];
    if (!less(previous, current) && !less(current, previous)) {
      if (previous == current) {
        fail(path,
             "duplicate complete catalog-record identity and metadata");
      }
      fail(path,
           "catalog-record identity has conflicting reference metadata");
    }
  }
  return records;
}

[[nodiscard]] CanonicalValue encode_record(
    const core::CatalogRecordRefDTO& record) {
  const auto& descriptor = core::catalog_record_type_descriptor(record);
  return std::visit(
      [&descriptor](const auto& typed) {
        return CanonicalValue(CanonicalValue::Object{
            {"record_kind",
             CanonicalValue(std::string(descriptor.serialized_kind))},
            {"record_id", CanonicalValue(typed.record_id.value())},
            {"record_revision",
             CanonicalValue(typed.record_revision.value())},
            {"schema_id", CanonicalValue(typed.schema_id.value())},
            {"schema_revision",
             CanonicalValue(typed.schema_revision.value())},
            {"content_digest",
             CanonicalValue(typed.content_digest.value())},
            {"content_addressed_store_key",
             CanonicalValue(typed.content_addressed_store_key.value())},
        });
      },
      record);
}

}  // namespace

std::vector<core::CatalogRecordRefDTO> decode_deployment_catalogs(
    const CanonicalValue& catalogs) {
  require_output_budget(catalogs, "catalogs");
  const auto& object = require_object(catalogs, "catalogs");
  require_exact_fields(object, {"records"}, "catalogs");
  const auto& record_values =
      require_array(require_member(object, "records", "catalogs"),
                    "catalogs.records");
  // A valid row has one object node, seven member-name scalar nodes, and
  // seven value scalar nodes. Reject a cardinality that no valid catalog can
  // satisfy before reserving the substantially larger typed-record variant.
  require_catalog_record_count(record_values.size());

  std::vector<core::CatalogRecordRefDTO> records;
  records.reserve(record_values.size());
  for (std::size_t index = 0U; index < record_values.size(); ++index) {
    records.push_back(
        parse_record(record_values[index],
                     index_path("catalogs.records", index)));
  }
  return canonicalize_records(std::move(records), "catalogs.records");
}

CanonicalValue encode_deployment_catalogs(
    std::span<const core::CatalogRecordRefDTO> records) {
  // Reject before copying the caller span. The budget counts each row object,
  // its seven member-name scalars, and its seven value scalars. The enclosing
  // object, its member-name scalar, and the records array add three nodes.
  require_catalog_record_count(records.size());
  // Validate the complete scalar budget before copying the caller's
  // potentially large store-key strings into a sortable working table.
  preflight_encode_budget(records);
  auto canonical_records = canonicalize_records(
      std::vector<core::CatalogRecordRefDTO>(records.begin(), records.end()),
      "catalogs.records");

  CanonicalValue::Array encoded;
  encoded.reserve(canonical_records.size());
  for (const auto& record : canonical_records) {
    encoded.push_back(encode_record(record));
  }
  CanonicalValue result(
      CanonicalValue::Object{{"records", CanonicalValue(std::move(encoded))}});
  require_output_budget(result, "catalogs");
  return result;
}

}  // namespace manyforge::deployment::detail
