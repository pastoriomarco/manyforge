#include "manyforge_deployment/closed_schema.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "closed_schema_internal.hpp"
#include "content_identity_internal.hpp"
#include "embedded_capability_definition_schema.hpp"
#include "embedded_component_type_schema.hpp"
#include "embedded_control_interface_contract_schema.hpp"
#include "embedded_controller_topology_schema.hpp"
#include "embedded_empty_runtime_configuration_schema.hpp"
#include "embedded_empty_provider_contract_schema.hpp"
#include "embedded_model_asset_schema.hpp"
#include "embedded_provider_manifest_schema.hpp"
#include "embedded_runtime_profile_schema.hpp"
#include "embedded_stop_evidence_policy_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment {
namespace {

enum class NodeKind : std::uint8_t { kScalar, kObject, kArray, kLocalRef, kTaggedUnion };
enum class ScalarType : std::uint8_t { kNull, kBoolean, kNumber, kInteger, kString };
enum class ConstraintKind : std::uint8_t {
  kMinimum,
  kMaximum,
  kExclusiveMinimum,
  kExclusiveMaximum,
  kMinLength,
  kMaxLength,
  kStringFormat,
  kEnum,
};
enum class CollectionKind : std::uint8_t { kOrdered, kScalarSet, kKeyedRecordSet };

struct Constraint final {
  ConstraintKind kind;
  CanonicalValue value;
  CanonicalValue::Array values;
  std::vector<std::string> canonical_values;
};

struct SchemaNode;

struct Field final {
  std::string field_id;
  bool required;
  bool materialized_default;
  CanonicalValue default_value;
  std::unique_ptr<SchemaNode> value_schema;
  std::size_t required_or_default_index = std::numeric_limits<std::size_t>::max();
};

struct Alternative final {
  std::string tag_value;
  std::unique_ptr<SchemaNode> value_schema;
};

struct SchemaNode final {
  NodeKind kind;
  ScalarType scalar_type = ScalarType::kNull;
  std::vector<Constraint> constraints;
  std::vector<Field> fields;
  std::size_t min_items = 0U;
  std::size_t max_items = 0U;
  CollectionKind collection_kind = CollectionKind::kOrdered;
  std::vector<std::vector<std::string>> key_paths;
  std::unique_ptr<SchemaNode> item_schema;
  std::string definition_id;
  const SchemaNode* resolved_target = nullptr;
  std::string tag_field;
  std::vector<Alternative> alternatives;
  std::vector<std::size_t> required_or_default_fields;
  std::size_t materialized_default_count = 0U;
};

struct Definition final {
  std::string definition_id;
  std::unique_ptr<SchemaNode> value_schema;
};

using ConstDefinitionMap = std::map<std::string, const SchemaNode*, std::less<>>;

[[noreturn]] void fail(std::string_view label, std::string message) {
  throw ClosedSchemaError(std::string(label) + ": " + std::move(message));
}

[[nodiscard]] const CanonicalValue::Object& require_object(const CanonicalValue& value,
                                                           std::string_view label) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(label, "expected object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array& require_array(const CanonicalValue& value,
                                                         std::string_view label,
                                                         std::size_t minimum = 0U,
                                                         std::size_t maximum =
                                                             kMaxClosedSchemaArrayItems) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(label, "expected array");
  }
  if (array->size() < minimum || array->size() > maximum) {
    fail(label, "item count outside [" + std::to_string(minimum) + ", " +
                    std::to_string(maximum) + "]");
  }
  return *array;
}

[[nodiscard]] const CanonicalValue& require_member(const CanonicalValue::Object& object,
                                                   std::string_view name,
                                                   std::string_view label) {
  const auto found =
      std::find_if(object.begin(), object.end(), [name](const auto& member) {
        return member.first == name;
      });
  if (found == object.end()) {
    fail(label, "missing required field " + std::string(name));
  }
  return found->second;
}

void require_exact_fields(const CanonicalValue::Object& object,
                          std::initializer_list<std::string_view> expected,
                          std::string_view label) {
  if (object.size() != expected.size()) {
    fail(label, "exact fields required");
  }
  for (const auto name : expected) {
    (void)require_member(object, name, label);
  }
}

[[nodiscard]] const std::string& require_string(const CanonicalValue& value,
                                                std::string_view label) {
  const auto* string = std::get_if<std::string>(&value.storage());
  if (string == nullptr) {
    fail(label, "expected string");
  }
  return *string;
}

[[nodiscard]] double require_number(const CanonicalValue& value, std::string_view label,
                                    bool require_integer) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || (require_integer && std::trunc(*number) != *number)) {
    fail(label, require_integer ? "expected integer" : "expected number");
  }
  if (std::trunc(*number) == *number &&
      std::abs(*number) > static_cast<double>(kMaxArtifactSafeInteger)) {
    fail(label, "integral number exceeds I-JSON safe range");
  }
  return *number;
}

[[nodiscard]] std::size_t require_bounded_size(const CanonicalValue& value,
                                               std::string_view label, std::size_t minimum,
                                               std::size_t maximum) {
  const double number = require_number(value, label, true);
  if (number < static_cast<double>(minimum) || number > static_cast<double>(maximum)) {
    fail(label, "integer outside [" + std::to_string(minimum) + ", " +
                    std::to_string(maximum) + "]");
  }
  return static_cast<std::size_t>(number);
}

[[nodiscard]] std::size_t utf8_code_points(std::string_view value) {
  std::size_t count = 0U;
  utf8proc_ssize_t offset = 0;
  while (offset < static_cast<utf8proc_ssize_t>(value.size())) {
    utf8proc_int32_t code_point = 0;
    const auto consumed =
        utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(value.data()) + offset,
                         static_cast<utf8proc_ssize_t>(value.size()) - offset, &code_point);
    if (consumed <= 0) {
      throw ClosedSchemaError("invalid UTF-8 reached closed-schema validation");
    }
    offset += consumed;
    ++count;
  }
  return count;
}

[[nodiscard]] bool is_portable_id(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U) {
    return false;
  }
  const auto alphanumeric = [](unsigned char byte) {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
           (byte >= '0' && byte <= '9');
  };
  if (!alphanumeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [&](unsigned char byte) {
    return alphanumeric(byte) || byte == '.' || byte == '_' || byte == '-';
  });
}

[[nodiscard]] bool is_api_version(std::string_view value) noexcept {
  if (!value.starts_with("manyforge.")) {
    return false;
  }
  std::size_t start = 0U;
  std::size_t segment_index = 0U;
  while (start <= value.size()) {
    const auto end = value.find('.', start);
    const auto segment =
        value.substr(start, end == std::string_view::npos ? value.size() - start
                                                          : end - start);
    if (segment_index == 0U) {
      if (segment != "manyforge") {
        return false;
      }
    } else if (end == std::string_view::npos) {
      if (segment_index < 2U || segment.size() < 2U ||
          segment.front() != 'v' ||
          !std::all_of(segment.begin() + 1, segment.end(),
                       [](unsigned char byte) {
                         return byte >= '0' && byte <= '9';
                       })) {
        return false;
      }
      return segment.size() == 2U || segment[1] != '0';
    } else if (segment.empty() || segment.front() < 'a' ||
               segment.front() > 'z' ||
               !std::all_of(segment.begin() + 1, segment.end(),
                            [](unsigned char byte) {
                              return (byte >= 'a' && byte <= 'z') ||
                                     (byte >= '0' && byte <= '9') ||
                                     byte == '_';
                            })) {
      return false;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1U;
    ++segment_index;
  }
  return false;
}

[[nodiscard]] std::string require_portable_id(const CanonicalValue& value,
                                              std::string_view label) {
  const auto& text = require_string(value, label);
  if (!is_portable_id(text)) {
    fail(label, "expected portable v1 identifier");
  }
  return text;
}

[[nodiscard]] CanonicalValue object(
    std::initializer_list<std::pair<std::string, CanonicalValue>> members) {
  return CanonicalValue(CanonicalValue::Object(members));
}

[[nodiscard]] std::string_view scalar_type_name(ScalarType type) noexcept {
  switch (type) {
    case ScalarType::kNull:
      return "null";
    case ScalarType::kBoolean:
      return "boolean";
    case ScalarType::kNumber:
      return "number";
    case ScalarType::kInteger:
      return "integer";
    case ScalarType::kString:
      return "string";
  }
  return {};
}

[[nodiscard]] std::string_view constraint_kind_name(ConstraintKind kind) noexcept {
  switch (kind) {
    case ConstraintKind::kMinimum:
      return "minimum";
    case ConstraintKind::kMaximum:
      return "maximum";
    case ConstraintKind::kExclusiveMinimum:
      return "exclusive_minimum";
    case ConstraintKind::kExclusiveMaximum:
      return "exclusive_maximum";
    case ConstraintKind::kMinLength:
      return "min_length";
    case ConstraintKind::kMaxLength:
      return "max_length";
    case ConstraintKind::kStringFormat:
      return "string_format";
    case ConstraintKind::kEnum:
      return "enum";
  }
  return {};
}

[[nodiscard]] std::string_view collection_kind_name(CollectionKind kind) noexcept {
  switch (kind) {
    case CollectionKind::kOrdered:
      return "ordered";
    case CollectionKind::kScalarSet:
      return "scalar_set";
    case CollectionKind::kKeyedRecordSet:
      return "keyed_record_set";
  }
  return {};
}

[[nodiscard]] ScalarType parse_scalar_type(std::string_view value, std::string_view label) {
  if (value == "null") {
    return ScalarType::kNull;
  }
  if (value == "boolean") {
    return ScalarType::kBoolean;
  }
  if (value == "number") {
    return ScalarType::kNumber;
  }
  if (value == "integer") {
    return ScalarType::kInteger;
  }
  if (value == "string") {
    return ScalarType::kString;
  }
  fail(label, "unsupported scalar type " + std::string(value));
}

[[nodiscard]] ConstraintKind parse_constraint_kind(std::string_view value,
                                                   std::string_view label) {
  if (value == "minimum") {
    return ConstraintKind::kMinimum;
  }
  if (value == "maximum") {
    return ConstraintKind::kMaximum;
  }
  if (value == "exclusive_minimum") {
    return ConstraintKind::kExclusiveMinimum;
  }
  if (value == "exclusive_maximum") {
    return ConstraintKind::kExclusiveMaximum;
  }
  if (value == "min_length") {
    return ConstraintKind::kMinLength;
  }
  if (value == "max_length") {
    return ConstraintKind::kMaxLength;
  }
  if (value == "string_format") {
    return ConstraintKind::kStringFormat;
  }
  if (value == "enum") {
    return ConstraintKind::kEnum;
  }
  fail(label, "unsupported constraint " + std::string(value));
}

[[nodiscard]] CanonicalValue normalize_scalar(const SchemaNode& schema,
                                              const CanonicalValue& value,
                                              std::string_view label);
[[nodiscard]] bool canonical_bytes_less(std::string_view left, std::string_view right);

[[nodiscard]] Constraint parse_constraint(const CanonicalValue& value, ScalarType scalar_type,
                                          std::string_view label) {
  const auto& node = require_object(value, label);
  const auto kind = parse_constraint_kind(
      require_string(require_member(node, "kind", label), std::string(label) + ".kind"), label);
  if (kind == ConstraintKind::kEnum) {
    require_exact_fields(node, {"kind", "values"}, label);
    const auto& authored =
        require_array(require_member(node, "values", label), std::string(label) + ".values", 1U,
                      kMaxClosedSchemaEnumValues);
    SchemaNode unconstrained{};
    unconstrained.kind = NodeKind::kScalar;
    unconstrained.scalar_type = scalar_type;
    std::vector<std::pair<std::string, CanonicalValue>> keyed;
    keyed.reserve(authored.size());
    std::set<std::string, std::less<>> seen;
    for (std::size_t index = 0U; index < authored.size(); ++index) {
      auto item = normalize_scalar(unconstrained, authored[index],
                                   std::string(label) + ".values[" + std::to_string(index) + "]");
      auto key = canonical_json(item);
      if (!seen.insert(key).second) {
        fail(label, "duplicate enum value");
      }
      keyed.emplace_back(std::move(key), std::move(item));
    }
    std::sort(keyed.begin(), keyed.end(), [](const auto& left, const auto& right) {
      return canonical_bytes_less(left.first, right.first);
    });
    CanonicalValue::Array normalized;
    std::vector<std::string> canonical_values;
    normalized.reserve(keyed.size());
    canonical_values.reserve(keyed.size());
    for (auto& [key, item] : keyed) {
      canonical_values.push_back(std::move(key));
      normalized.push_back(std::move(item));
    }
    return {kind, CanonicalValue(nullptr), std::move(normalized),
            std::move(canonical_values)};
  }

  require_exact_fields(node, {"kind", "value"}, label);
  const auto& authored = require_member(node, "value", label);
  if (kind == ConstraintKind::kMinimum || kind == ConstraintKind::kMaximum ||
      kind == ConstraintKind::kExclusiveMinimum || kind == ConstraintKind::kExclusiveMaximum) {
    if (scalar_type != ScalarType::kInteger && scalar_type != ScalarType::kNumber) {
      fail(label, std::string(constraint_kind_name(kind)) + " requires integer or number scalar");
    }
    return {kind,
            CanonicalValue(require_number(authored, std::string(label) + ".value",
                                          scalar_type == ScalarType::kInteger)),
            {},
            {}};
  }
  if (kind == ConstraintKind::kMinLength || kind == ConstraintKind::kMaxLength) {
    if (scalar_type != ScalarType::kString) {
      fail(label, std::string(constraint_kind_name(kind)) + " requires string scalar");
    }
    return {kind,
            CanonicalValue(static_cast<double>(require_bounded_size(
                authored, std::string(label) + ".value", 0U,
                kMaxClosedSchemaStringCodePoints))),
            {},
            {}};
  }
  if (scalar_type != ScalarType::kString) {
    fail(label, "string_format requires string scalar");
  }
  const auto& format = require_string(authored, std::string(label) + ".value");
  if (format != "portable_id" && format != "sha256_digest" && format != "api_version") {
    fail(std::string(label) + ".value", "unregistered string format " + format);
  }
  return {kind, CanonicalValue(format), {}, {}};
}

struct SchemaBudget final {
  std::size_t nodes = 0U;

  void consume(std::string_view label, std::size_t depth) {
    if (depth > kMaxArtifactNestingDepth) {
      fail(label, "schema nesting exceeds " + std::to_string(kMaxArtifactNestingDepth));
    }
    ++nodes;
    if (nodes > kMaxArtifactNodeCount) {
      fail(label, "schema node count exceeds " + std::to_string(kMaxArtifactNodeCount));
    }
  }
};

[[nodiscard]] std::unique_ptr<SchemaNode> parse_node(const CanonicalValue& value,
                                                     std::string label, SchemaBudget& budget,
                                                     std::size_t depth);

[[nodiscard]] CollectionKind parse_collection(
    const CanonicalValue& value, std::string_view label,
    std::vector<std::vector<std::string>>& key_paths) {
  const auto& node = require_object(value, label);
  const auto& kind =
      require_string(require_member(node, "kind", label), std::string(label) + ".kind");
  if (kind == "ordered" || kind == "scalar_set") {
    require_exact_fields(node, {"kind"}, label);
    return kind == "ordered" ? CollectionKind::kOrdered : CollectionKind::kScalarSet;
  }
  if (kind != "keyed_record_set") {
    fail(std::string(label) + ".kind", "unsupported collection kind " + kind);
  }
  require_exact_fields(node, {"kind", "key_paths"}, label);
  const auto& paths =
      require_array(require_member(node, "key_paths", label), std::string(label) + ".key_paths",
                    1U, kMaxClosedSchemaKeyPaths);
  std::set<std::vector<std::string>> seen;
  for (std::size_t path_index = 0U; path_index < paths.size(); ++path_index) {
    const auto path_label =
        std::string(label) + ".key_paths[" + std::to_string(path_index) + "]";
    const auto& segments =
        require_array(paths[path_index], path_label, 1U,
                      kMaxClosedSchemaKeyPathSegments);
    std::vector<std::string> normalized;
    normalized.reserve(segments.size());
    for (std::size_t index = 0U; index < segments.size(); ++index) {
      normalized.push_back(require_portable_id(
          segments[index], path_label + "[" + std::to_string(index) + "]"));
    }
    if (!seen.insert(normalized).second) {
      fail(label, "duplicate key path");
    }
    key_paths.push_back(std::move(normalized));
  }
  return CollectionKind::kKeyedRecordSet;
}

[[nodiscard]] std::unique_ptr<SchemaNode> parse_node(const CanonicalValue& value,
                                                     std::string label, SchemaBudget& budget,
                                                     std::size_t depth) {
  budget.consume(label, depth);
  const auto& authored = require_object(value, label);
  const auto& kind =
      require_string(require_member(authored, "kind", label), label + ".kind");
  auto result = std::make_unique<SchemaNode>();
  if (kind == "scalar") {
    require_exact_fields(authored, {"kind", "scalar_type", "constraints"}, label);
    result->kind = NodeKind::kScalar;
    result->scalar_type =
        parse_scalar_type(require_string(require_member(authored, "scalar_type", label),
                                         label + ".scalar_type"),
                          label + ".scalar_type");
    const auto& constraints =
        require_array(require_member(authored, "constraints", label), label + ".constraints", 0U,
                      kMaxClosedSchemaConstraints);
    std::set<ConstraintKind> seen;
    for (std::size_t index = 0U; index < constraints.size(); ++index) {
      auto constraint =
          parse_constraint(constraints[index], result->scalar_type,
                           label + ".constraints[" + std::to_string(index) + "]");
      if (!seen.insert(constraint.kind).second) {
        fail(label + ".constraints", "duplicate constraint kind");
      }
      result->constraints.push_back(std::move(constraint));
    }
    std::sort(result->constraints.begin(), result->constraints.end(),
              [](const Constraint& left, const Constraint& right) {
                return constraint_kind_name(left.kind) < constraint_kind_name(right.kind);
              });
    return result;
  }
  if (kind == "object") {
    require_exact_fields(authored, {"kind", "fields"}, label);
    result->kind = NodeKind::kObject;
    const auto& fields =
        require_array(require_member(authored, "fields", label), label + ".fields", 0U,
                      kMaxClosedSchemaFields);
    std::set<std::string, std::less<>> seen;
    for (std::size_t index = 0U; index < fields.size(); ++index) {
      const auto field_label = label + ".fields[" + std::to_string(index) + "]";
      const auto& field = require_object(fields[index], field_label);
      require_exact_fields(field, {"field_id", "presence", "default", "value_schema"},
                           field_label);
      auto field_id =
          require_portable_id(require_member(field, "field_id", field_label),
                              field_label + ".field_id");
      if (!seen.insert(field_id).second) {
        fail(label + ".fields", "duplicate field_id " + field_id);
      }
      const auto& presence =
          require_string(require_member(field, "presence", field_label),
                         field_label + ".presence");
      if (presence != "required" && presence != "optional") {
        fail(field_label + ".presence", "expected required or optional");
      }
      const auto& default_object =
          require_object(require_member(field, "default", field_label), field_label + ".default");
      const auto& default_kind =
          require_string(require_member(default_object, "kind", field_label + ".default"),
                         field_label + ".default.kind");
      bool materialized = false;
      CanonicalValue default_value(nullptr);
      if (default_kind == "absent") {
        require_exact_fields(default_object, {"kind"}, field_label + ".default");
      } else if (default_kind == "materialized") {
        require_exact_fields(default_object, {"kind", "value"}, field_label + ".default");
        if (presence != "optional") {
          fail(field_label + ".default", "required fields cannot materialize defaults");
        }
        materialized = true;
        default_value = require_member(default_object, "value", field_label + ".default");
      } else {
        fail(field_label + ".default.kind", "expected absent or materialized");
      }
      result->fields.push_back(
          {std::move(field_id), presence == "required", materialized, std::move(default_value),
           parse_node(require_member(field, "value_schema", field_label),
                      field_label + ".value_schema", budget, depth + 1U)});
    }
    std::sort(result->fields.begin(), result->fields.end(),
              [](const Field& left, const Field& right) {
                return left.field_id < right.field_id;
              });
    for (std::size_t index = 0U; index < result->fields.size(); ++index) {
      auto& field = result->fields[index];
      if (!field.required && !field.materialized_default) {
        continue;
      }
      field.required_or_default_index = result->required_or_default_fields.size();
      result->required_or_default_fields.push_back(index);
      if (field.materialized_default) {
        ++result->materialized_default_count;
      }
    }
    return result;
  }
  if (kind == "array") {
    require_exact_fields(authored, {"kind", "min_items", "max_items", "collection", "item_schema"},
                         label);
    result->kind = NodeKind::kArray;
    result->min_items = require_bounded_size(require_member(authored, "min_items", label),
                                             label + ".min_items", 0U,
                                             kMaxClosedSchemaArrayItems);
    result->max_items = require_bounded_size(require_member(authored, "max_items", label),
                                             label + ".max_items", 0U,
                                             kMaxClosedSchemaArrayItems);
    if (result->min_items > result->max_items) {
      fail(label, "min_items exceeds max_items");
    }
    result->collection_kind =
        parse_collection(require_member(authored, "collection", label), label + ".collection",
                         result->key_paths);
    result->item_schema =
        parse_node(require_member(authored, "item_schema", label), label + ".item_schema", budget,
                   depth + 1U);
    return result;
  }
  if (kind == "local_ref") {
    require_exact_fields(authored, {"kind", "definition_id"}, label);
    result->kind = NodeKind::kLocalRef;
    result->definition_id =
        require_portable_id(require_member(authored, "definition_id", label),
                            label + ".definition_id");
    return result;
  }
  if (kind != "tagged_union") {
    fail(label + ".kind", "unsupported node kind " + kind);
  }
  require_exact_fields(authored, {"kind", "tag_field", "alternatives"}, label);
  result->kind = NodeKind::kTaggedUnion;
  result->tag_field =
      require_portable_id(require_member(authored, "tag_field", label), label + ".tag_field");
  const auto& alternatives =
      require_array(require_member(authored, "alternatives", label), label + ".alternatives", 1U,
                    kMaxClosedSchemaAlternatives);
  std::set<std::string, std::less<>> seen;
  for (std::size_t index = 0U; index < alternatives.size(); ++index) {
    const auto alternative_label = label + ".alternatives[" + std::to_string(index) + "]";
    const auto& alternative = require_object(alternatives[index], alternative_label);
    require_exact_fields(alternative, {"tag_value", "value_schema"}, alternative_label);
    auto tag = require_portable_id(require_member(alternative, "tag_value", alternative_label),
                                   alternative_label + ".tag_value");
    if (!seen.insert(tag).second) {
      fail(label + ".alternatives", "duplicate tag_value " + tag);
    }
    result->alternatives.push_back(
        {std::move(tag), parse_node(require_member(alternative, "value_schema", alternative_label),
                                    alternative_label + ".value_schema", budget, depth + 1U)});
  }
  std::sort(result->alternatives.begin(), result->alternatives.end(),
            [](const Alternative& left, const Alternative& right) {
              return left.tag_value < right.tag_value;
            });
  return result;
}

[[nodiscard]] const SchemaNode& resolve_node(const SchemaNode& authored,
                                             const ConstDefinitionMap& definitions) {
  (void)definitions;
  if (authored.kind != NodeKind::kLocalRef) {
    return authored;
  }
  if (authored.resolved_target == nullptr) {
    throw ClosedSchemaError("definitions: unresolved local_ref reached qualified use");
  }
  return *authored.resolved_target;
}

[[nodiscard]] const Field* find_schema_field(
    const std::vector<Field>& fields, std::string_view field_id) noexcept {
  const auto found = std::lower_bound(
      fields.begin(), fields.end(), field_id,
      [](const Field& field, std::string_view candidate) {
        return field.field_id < candidate;
      });
  return found == fields.end() || found->field_id != field_id ? nullptr
                                                              : &*found;
}

[[nodiscard]] const CanonicalValue* find_portable_object_member(
    const CanonicalValue::Object& object, std::string_view field_id) noexcept {
  // Schema field IDs are portable ASCII. Their byte order is identical to
  // RFC 8785 UTF-16 order relative to every valid object member name.
  const auto found = std::lower_bound(
      object.begin(), object.end(), field_id,
      [](const auto& member, std::string_view candidate) {
        return member.first < candidate;
      });
  return found == object.end() || found->first != field_id ? nullptr
                                                           : &found->second;
}

}  // namespace

namespace detail {

struct CompiledClosedSchemaData final {
  core::SchemaId schema_id{std::string{}};
  core::SchemaRevision schema_revision{std::string{}};
  ClosedSchemaPurpose schema_purpose = ClosedSchemaPurpose::kArtifactDocument;
  ClosedSchemaRootShape root_shape = ClosedSchemaRootShape::kObject;
  std::string root_definition_id;
  std::vector<Definition> definitions;
  ConstDefinitionMap definitions_by_id;
  CanonicalValue normalized_descriptor;
};

}  // namespace detail


namespace {

[[nodiscard]] const Constraint* find_constraint(const SchemaNode& node,
                                                ConstraintKind kind) noexcept {
  const auto found =
      std::find_if(node.constraints.begin(), node.constraints.end(),
                   [kind](const Constraint& constraint) { return constraint.kind == kind; });
  return found == node.constraints.end() ? nullptr : &*found;
}

[[nodiscard]] bool scalar_equal(const CanonicalValue& left, const CanonicalValue& right) {
  return canonical_json(left) == canonical_json(right);
}

[[nodiscard]] bool canonical_bytes_less(std::string_view left, std::string_view right) {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(), [](char left_byte, char right_byte) {
        return static_cast<unsigned char>(left_byte) <
               static_cast<unsigned char>(right_byte);
      });
}

[[nodiscard]] bool canonical_key_tuple_less(const std::vector<std::string>& left,
                                            const std::vector<std::string>& right) {
  const auto common = std::min(left.size(), right.size());
  for (std::size_t index = 0U; index < common; ++index) {
    if (left[index] == right[index]) {
      continue;
    }
    return canonical_bytes_less(left[index], right[index]);
  }
  return left.size() < right.size();
}

[[nodiscard]] CanonicalValue normalize_scalar(const SchemaNode& schema,
                                              const CanonicalValue& value,
                                              std::string_view label) {
  CanonicalValue normalized;
  switch (schema.scalar_type) {
    case ScalarType::kNull:
      if (!std::holds_alternative<std::nullptr_t>(value.storage())) {
        fail(label, "expected null");
      }
      normalized = CanonicalValue(nullptr);
      break;
    case ScalarType::kBoolean: {
      const auto* boolean = std::get_if<bool>(&value.storage());
      if (boolean == nullptr) {
        fail(label, "expected boolean");
      }
      normalized = CanonicalValue(*boolean);
      break;
    }
    case ScalarType::kNumber:
      normalized = CanonicalValue(require_number(value, label, false));
      break;
    case ScalarType::kInteger:
      normalized = CanonicalValue(require_number(value, label, true));
      break;
    case ScalarType::kString: {
      const auto& text = require_string(value, label);
      if (utf8_code_points(text) > kMaxClosedSchemaStringCodePoints) {
        fail(label, "string exceeds " + std::to_string(kMaxClosedSchemaStringCodePoints) +
                        " code points");
      }
      normalized = CanonicalValue(text);
      break;
    }
  }

  const auto* minimum = find_constraint(schema, ConstraintKind::kMinimum);
  const auto* maximum = find_constraint(schema, ConstraintKind::kMaximum);
  const auto* exclusive_minimum =
      find_constraint(schema, ConstraintKind::kExclusiveMinimum);
  const auto* exclusive_maximum =
      find_constraint(schema, ConstraintKind::kExclusiveMaximum);
  if (minimum != nullptr &&
      require_number(normalized, label, schema.scalar_type == ScalarType::kInteger) <
          std::get<double>(minimum->value.storage())) {
    fail(label, "below minimum");
  }
  if (maximum != nullptr &&
      require_number(normalized, label, schema.scalar_type == ScalarType::kInteger) >
          std::get<double>(maximum->value.storage())) {
    fail(label, "above maximum");
  }
  if (exclusive_minimum != nullptr &&
      require_number(normalized, label, schema.scalar_type == ScalarType::kInteger) <=
          std::get<double>(exclusive_minimum->value.storage())) {
    fail(label, "not above exclusive_minimum");
  }
  if (exclusive_maximum != nullptr &&
      require_number(normalized, label, schema.scalar_type == ScalarType::kInteger) >=
          std::get<double>(exclusive_maximum->value.storage())) {
    fail(label, "not below exclusive_maximum");
  }

  const auto* min_length = find_constraint(schema, ConstraintKind::kMinLength);
  const auto* max_length = find_constraint(schema, ConstraintKind::kMaxLength);
  if (min_length != nullptr || max_length != nullptr) {
    const auto length = utf8_code_points(std::get<std::string>(normalized.storage()));
    if (min_length != nullptr &&
        length < static_cast<std::size_t>(std::get<double>(min_length->value.storage()))) {
      fail(label, "shorter than min_length");
    }
    if (max_length != nullptr &&
        length > static_cast<std::size_t>(std::get<double>(max_length->value.storage()))) {
      fail(label, "longer than max_length");
    }
  }

  const auto* string_format = find_constraint(schema, ConstraintKind::kStringFormat);
  if (string_format != nullptr) {
    const auto& format = std::get<std::string>(string_format->value.storage());
    const auto& text = std::get<std::string>(normalized.storage());
    if (format == "portable_id" && !is_portable_id(text)) {
      fail(label, "expected portable_id");
    }
    if (format == "sha256_digest" && !is_sha256_digest(text)) {
      fail(label, "expected sha256_digest");
    }
    if (format == "api_version" && !is_api_version(text)) {
      fail(label, "expected api_version");
    }
  }

  const auto* enumeration = find_constraint(schema, ConstraintKind::kEnum);
  if (enumeration != nullptr) {
    const auto key = canonical_json(normalized);
    const auto found = std::lower_bound(
        enumeration->canonical_values.begin(), enumeration->canonical_values.end(), key,
        [](const std::string& allowed, const std::string& candidate) {
          return canonical_bytes_less(allowed, candidate);
        });
    if (found == enumeration->canonical_values.end() || *found != key) {
      fail(label, "value is outside enum");
    }
  }
  return normalized;
}

void validate_constraint_consistency(const SchemaNode& node, std::string_view label) {
  const auto* minimum = find_constraint(node, ConstraintKind::kMinimum);
  const auto* maximum = find_constraint(node, ConstraintKind::kMaximum);
  const auto* exclusive_minimum = find_constraint(node, ConstraintKind::kExclusiveMinimum);
  const auto* exclusive_maximum = find_constraint(node, ConstraintKind::kExclusiveMaximum);
  if (minimum != nullptr && exclusive_minimum != nullptr) {
    fail(label, "minimum and exclusive_minimum are mutually exclusive");
  }
  if (maximum != nullptr && exclusive_maximum != nullptr) {
    fail(label, "maximum and exclusive_maximum are mutually exclusive");
  }
  const Constraint* lower = minimum != nullptr ? minimum : exclusive_minimum;
  const Constraint* upper = maximum != nullptr ? maximum : exclusive_maximum;
  if (lower != nullptr && upper != nullptr) {
    const auto lower_value = std::get<double>(lower->value.storage());
    const auto upper_value = std::get<double>(upper->value.storage());
    if (lower_value > upper_value) {
      fail(label, "numeric lower bound exceeds upper bound");
    }
    if (lower_value == upper_value &&
        (lower->kind == ConstraintKind::kExclusiveMinimum ||
         upper->kind == ConstraintKind::kExclusiveMaximum)) {
      fail(label, "exclusive numeric bounds admit no value");
    }
  }
  const auto* min_length = find_constraint(node, ConstraintKind::kMinLength);
  const auto* max_length = find_constraint(node, ConstraintKind::kMaxLength);
  if (min_length != nullptr && max_length != nullptr &&
      std::get<double>(min_length->value.storage()) >
          std::get<double>(max_length->value.storage())) {
    fail(label, "min_length exceeds max_length");
  }

  const auto* enumeration = find_constraint(node, ConstraintKind::kEnum);
  if (enumeration != nullptr) {
    SchemaNode without_enum{};
    without_enum.kind = NodeKind::kScalar;
    without_enum.scalar_type = node.scalar_type;
    for (const auto& constraint : node.constraints) {
      if (constraint.kind != ConstraintKind::kEnum) {
        without_enum.constraints.push_back(constraint);
      }
    }
    for (std::size_t index = 0U; index < enumeration->values.size(); ++index) {
      (void)normalize_scalar(without_enum, enumeration->values[index],
                             std::string(label) + ".constraints.enum.values[" +
                                 std::to_string(index) + "]");
    }
  }
}

[[nodiscard]] std::set<std::string, std::less<>> node_refs(const SchemaNode& root) {
  std::set<std::string, std::less<>> refs;
  std::vector<const SchemaNode*> pending{&root};
  while (!pending.empty()) {
    const auto* node = pending.back();
    pending.pop_back();
    switch (node->kind) {
      case NodeKind::kLocalRef:
        refs.insert(node->definition_id);
        break;
      case NodeKind::kObject:
        for (const auto& field : node->fields) {
          pending.push_back(field.value_schema.get());
        }
        break;
      case NodeKind::kArray:
        pending.push_back(node->item_schema.get());
        break;
      case NodeKind::kTaggedUnion:
        for (const auto& alternative : node->alternatives) {
          pending.push_back(alternative.value_schema.get());
        }
        break;
      case NodeKind::kScalar:
        break;
    }
  }
  return refs;
}

[[nodiscard]] std::vector<std::string> validate_definition_graph(
    const ConstDefinitionMap& definitions, std::string_view root_definition_id) {
  std::map<std::string, std::vector<std::string>, std::less<>> edges;
  for (const auto& [definition_id, node] : definitions) {
    const auto refs = node_refs(*node);
    for (const auto& reference : refs) {
      if (!definitions.contains(reference)) {
        fail("definitions." + definition_id, "unknown local definition " + reference);
      }
    }
    edges.emplace(definition_id, std::vector<std::string>(refs.begin(), refs.end()));
  }

  std::map<std::string, std::uint8_t, std::less<>> state;
  std::vector<std::string> dependency_first;
  std::vector<std::string> starts{std::string(root_definition_id)};
  for (const auto& [definition_id, node] : definitions) {
    (void)node;
    if (definition_id != root_definition_id) {
      starts.push_back(definition_id);
    }
  }

  struct Frame final {
    std::string definition_id;
    std::size_t next_dependency;
  };
  for (const auto& start : starts) {
    if (state[start] == 2U) {
      continue;
    }
    state[start] = 1U;
    std::vector<std::string> path{start};
    std::map<std::string, std::size_t, std::less<>> positions{{start, 0U}};
    std::vector<Frame> stack{{start, 0U}};
    while (!stack.empty()) {
      auto& frame = stack.back();
      const auto& dependencies = edges.at(frame.definition_id);
      if (frame.next_dependency == dependencies.size()) {
        state[frame.definition_id] = 2U;
        dependency_first.push_back(frame.definition_id);
        positions.erase(frame.definition_id);
        path.pop_back();
        stack.pop_back();
        continue;
      }
      const auto dependency = dependencies[frame.next_dependency++];
      if (state[dependency] == 2U) {
        continue;
      }
      if (state[dependency] == 1U) {
        std::string cycle;
        for (std::size_t index = positions.at(dependency); index < path.size(); ++index) {
          if (!cycle.empty()) {
            cycle.append(" -> ");
          }
          cycle.append(path[index]);
        }
        cycle.append(" -> ");
        cycle.append(dependency);
        fail("definitions", "local_ref cycle is forbidden: " + cycle);
      }
      state[dependency] = 1U;
      positions[dependency] = path.size();
      path.push_back(dependency);
      stack.push_back({dependency, 0U});
    }
  }
  return dependency_first;
}

void cache_local_ref_targets(
    std::vector<Definition>& definitions, const ConstDefinitionMap& definitions_by_id,
    const std::vector<std::string>& dependency_first) {
  ConstDefinitionMap resolved_definitions;
  for (const auto& definition_id : dependency_first) {
    const auto* root = definitions_by_id.at(definition_id);
    if (root->kind == NodeKind::kLocalRef) {
      resolved_definitions.emplace(definition_id,
                                   resolved_definitions.at(root->definition_id));
    } else {
      resolved_definitions.emplace(definition_id, root);
    }
  }

  for (auto& definition : definitions) {
    std::vector<SchemaNode*> pending{definition.value_schema.get()};
    while (!pending.empty()) {
      auto* node = pending.back();
      pending.pop_back();
      switch (node->kind) {
        case NodeKind::kLocalRef:
          node->resolved_target = resolved_definitions.at(node->definition_id);
          break;
        case NodeKind::kObject:
          for (auto& field : node->fields) {
            pending.push_back(field.value_schema.get());
          }
          break;
        case NodeKind::kArray:
          pending.push_back(node->item_schema.get());
          break;
        case NodeKind::kTaggedUnion:
          for (auto& alternative : node->alternatives) {
            pending.push_back(alternative.value_schema.get());
          }
          break;
        case NodeKind::kScalar:
          break;
      }
    }
  }
}

[[nodiscard]] std::size_t expanded_node_depth(
    const SchemaNode& root, const std::map<std::string, std::size_t, std::less<>>& definition_depths) {
  switch (root.kind) {
    case NodeKind::kScalar:
      return 0U;
    case NodeKind::kLocalRef:
      return definition_depths.at(root.definition_id);
    case NodeKind::kObject: {
      std::size_t child_depth = 0U;
      for (const auto& field : root.fields) {
        child_depth = std::max(child_depth,
                               expanded_node_depth(*field.value_schema, definition_depths));
      }
      return child_depth + 1U;
    }
    case NodeKind::kArray:
      return expanded_node_depth(*root.item_schema, definition_depths) + 1U;
    case NodeKind::kTaggedUnion: {
      std::size_t child_depth = 0U;
      for (const auto& alternative : root.alternatives) {
        child_depth = std::max(
            child_depth, expanded_node_depth(*alternative.value_schema, definition_depths));
      }
      return child_depth;
    }
  }
  return 0U;
}

void validate_expanded_depths(const ConstDefinitionMap& definitions,
                              const std::vector<std::string>& dependency_first) {
  std::map<std::string, std::size_t, std::less<>> depths;
  for (const auto& definition_id : dependency_first) {
    const auto depth = expanded_node_depth(*definitions.at(definition_id), depths);
    if (depth > kMaxArtifactNestingDepth) {
      fail("definitions." + definition_id,
           "ref-expanded collection depth " + std::to_string(depth) + " exceeds " +
               std::to_string(kMaxArtifactNestingDepth));
    }
    depths.emplace(definition_id, depth);
  }
}

void validate_key_path(const SchemaNode& authored, const std::vector<std::string>& path,
                       const ConstDefinitionMap& definitions, std::string_view label) {
  const SchemaNode* current = &resolve_node(authored, definitions);
  for (std::size_t index = 0U; index < path.size(); ++index) {
    if (current->kind != NodeKind::kObject) {
      fail(label, "segment " + path[index] + " traverses a non-object schema");
    }
    const auto* found = find_schema_field(current->fields, path[index]);
    if (found == nullptr) {
      fail(label, "field " + path[index] + " is not declared");
    }
    if (!found->required) {
      fail(label, "key-path fields must be required");
    }
    current = &resolve_node(*found->value_schema, definitions);
    if (index + 1U == path.size() && current->kind != NodeKind::kScalar) {
      fail(label, "key path must terminate at scalar");
    }
  }
}

void validate_collection_shapes(const ConstDefinitionMap& definitions) {
  for (const auto& [definition_id, root] : definitions) {
    std::vector<const SchemaNode*> pending{root};
    while (!pending.empty()) {
      const auto* node = pending.back();
      pending.pop_back();
      if (node->kind == NodeKind::kScalar) {
        validate_constraint_consistency(*node, "definitions." + definition_id);
      } else if (node->kind == NodeKind::kObject) {
        for (const auto& field : node->fields) {
          pending.push_back(field.value_schema.get());
        }
      } else if (node->kind == NodeKind::kArray) {
        const auto& item = resolve_node(*node->item_schema, definitions);
        if (node->collection_kind == CollectionKind::kScalarSet &&
            item.kind != NodeKind::kScalar) {
          fail("definitions." + definition_id, "scalar_set item must resolve to scalar");
        }
        if (node->collection_kind == CollectionKind::kKeyedRecordSet &&
            item.kind != NodeKind::kObject) {
          fail("definitions." + definition_id,
               "keyed_record_set item must resolve to object");
        }
        for (std::size_t index = 0U; index < node->key_paths.size(); ++index) {
          validate_key_path(item, node->key_paths[index], definitions,
                            "definitions." + definition_id + ".key_paths[" +
                                std::to_string(index) + "]");
        }
        pending.push_back(node->item_schema.get());
      } else if (node->kind == NodeKind::kTaggedUnion) {
        for (const auto& alternative : node->alternatives) {
          const auto& branch = resolve_node(*alternative.value_schema, definitions);
          if (branch.kind != NodeKind::kObject) {
            fail("definitions." + definition_id,
                 "tagged-union branch must resolve to object");
          }
          const auto* tag = find_schema_field(branch.fields, node->tag_field);
          if (tag == nullptr || !tag->required) {
            fail("definitions." + definition_id,
                 "tagged-union branch must require tag field " + node->tag_field);
          }
          const auto& tag_schema = resolve_node(*tag->value_schema, definitions);
          const auto* enumeration = find_constraint(tag_schema, ConstraintKind::kEnum);
          if (tag_schema.kind != NodeKind::kScalar ||
              tag_schema.scalar_type != ScalarType::kString || enumeration == nullptr ||
              enumeration->values.size() != 1U ||
              !scalar_equal(enumeration->values.front(), CanonicalValue(alternative.tag_value))) {
            fail("definitions." + definition_id,
                 "branch tag field must be a single-value string enum matching tag_value");
          }
          pending.push_back(alternative.value_schema.get());
        }
      }
    }
  }
}

struct ValueBudget final {
  explicit ValueBudget(
      detail::CumulativeNormalizedValueBudget& cumulative_budget)
      : cumulative_budget(cumulative_budget) {}

  detail::CumulativeNormalizedValueBudget& cumulative_budget;

  void consume_node(std::string_view label) {
    if (cumulative_budget.nodes == kMaxArtifactNodeCount) {
      fail(label, "normalized value exceeds " + std::to_string(kMaxArtifactNodeCount) +
                      " total nodes");
    }
    ++cumulative_budget.nodes;
  }

  void consume_scalar(const CanonicalValue& value, std::string_view label) {
    consume_node(label);
    if (const auto* text = std::get_if<std::string>(&value.storage());
        text != nullptr && text->size() > kMaxArtifactScalarBytes) {
      fail(label, "UTF-8 scalar exceeds " + std::to_string(kMaxArtifactScalarBytes) + " bytes");
    }
    const auto encoded_size = canonical_json(value).size();
    if (encoded_size >
        kMaxNormalizedAggregateScalarBytes -
            cumulative_budget.scalar_bytes) {
      fail(label, "normalized scalar bytes exceed " +
                      std::to_string(kMaxNormalizedAggregateScalarBytes));
    }
    cumulative_budget.scalar_bytes += encoded_size;
  }
};

[[nodiscard]] const CanonicalValue& extract_key_path(const CanonicalValue& value,
                                                     const std::vector<std::string>& path,
                                                     std::string_view label) {
  const CanonicalValue* current = &value;
  for (const auto& segment : path) {
    const auto* object_value = std::get_if<CanonicalValue::Object>(&current->storage());
    if (object_value == nullptr) {
      fail(label, "keyed-record path traverses non-object");
    }
    current = find_portable_object_member(*object_value, segment);
    if (current == nullptr) {
      fail(label, "keyed-record path field is absent: " + segment);
    }
  }
  if (std::holds_alternative<CanonicalValue::Object>(current->storage()) ||
      std::holds_alternative<CanonicalValue::Array>(current->storage())) {
    fail(label, "keyed-record path must resolve to scalar");
  }
  return *current;
}

[[nodiscard]] CanonicalValue normalize_instance(const SchemaNode& authored,
                                                const CanonicalValue& value,
                                                const ConstDefinitionMap& definitions,
                                                std::string label, std::size_t depth,
                                                ValueBudget& budget) {
  const auto& node = resolve_node(authored, definitions);
  if (node.kind == NodeKind::kScalar) {
    auto normalized = normalize_scalar(node, value, label);
    budget.consume_scalar(normalized, label);
    return normalized;
  }
  if (node.kind == NodeKind::kObject) {
    const auto collection_depth = depth + 1U;
    if (collection_depth > kMaxArtifactNestingDepth) {
      fail(label, "instance collection nesting exceeds " +
                      std::to_string(kMaxArtifactNestingDepth));
    }
    const auto& source = require_object(value, label);
    budget.consume_node(label);
    CanonicalValue::Object normalized;
    normalized.reserve(source.size() + node.materialized_default_count);
    std::vector<bool> required_or_default_present(
        node.required_or_default_fields.size(), false);
    for (const auto& member : source) {
      const auto* found = find_schema_field(node.fields, member.first);
      if (found == nullptr) {
        fail(label, "unknown field " + member.first);
      }
      if (found->required_or_default_index != std::numeric_limits<std::size_t>::max()) {
        required_or_default_present[found->required_or_default_index] = true;
      }
      budget.consume_scalar(CanonicalValue(found->field_id), label + ".<key>");
      normalized.emplace_back(
          found->field_id,
          normalize_instance(*found->value_schema, member.second, definitions,
                             label + "." + found->field_id, collection_depth, budget));
    }
    for (const auto field_index : node.required_or_default_fields) {
      const auto& field = node.fields[field_index];
      if (required_or_default_present[field.required_or_default_index]) {
        continue;
      }
      if (field.required) {
        fail(label, "missing required field " + field.field_id);
      }
      budget.consume_scalar(CanonicalValue(field.field_id), label + ".<key>");
      normalized.emplace_back(
          field.field_id,
          normalize_instance(*field.value_schema, field.default_value, definitions,
                             label + "." + field.field_id + ".default",
                             collection_depth, budget));
    }
    return CanonicalValue(std::move(normalized));
  }
  if (node.kind == NodeKind::kArray) {
    const auto collection_depth = depth + 1U;
    if (collection_depth > kMaxArtifactNestingDepth) {
      fail(label, "instance collection nesting exceeds " +
                      std::to_string(kMaxArtifactNestingDepth));
    }
    const auto& source = require_array(value, label, node.min_items, node.max_items);
    budget.consume_node(label);
    CanonicalValue::Array normalized;
    normalized.reserve(source.size());
    for (std::size_t index = 0U; index < source.size(); ++index) {
      normalized.push_back(normalize_instance(*node.item_schema, source[index], definitions,
                                              label + "[" + std::to_string(index) + "]",
                                              collection_depth, budget));
    }
    if (node.collection_kind == CollectionKind::kOrdered) {
      return CanonicalValue(std::move(normalized));
    }
    if (node.collection_kind == CollectionKind::kScalarSet) {
      std::vector<std::pair<std::string, CanonicalValue>> keyed;
      keyed.reserve(normalized.size());
      std::set<std::string, std::less<>> seen;
      for (auto& item : normalized) {
        auto key = canonical_json(item);
        if (!seen.insert(key).second) {
          fail(label, "duplicate scalar-set value");
        }
        keyed.emplace_back(std::move(key), std::move(item));
      }
      std::sort(keyed.begin(), keyed.end(),
                [](const auto& left, const auto& right) {
                  return canonical_bytes_less(left.first, right.first);
                });
      CanonicalValue::Array result;
      result.reserve(keyed.size());
      for (auto& item : keyed) {
        result.push_back(std::move(item.second));
      }
      return CanonicalValue(std::move(result));
    }

    using Key = std::vector<std::string>;
    std::vector<std::pair<Key, CanonicalValue>> keyed;
    keyed.reserve(normalized.size());
    std::set<Key> seen;
    for (auto& item : normalized) {
      Key key;
      key.reserve(node.key_paths.size());
      for (const auto& path : node.key_paths) {
        key.push_back(canonical_json(extract_key_path(item, path, label)));
      }
      if (!seen.insert(key).second) {
        fail(label, "duplicate keyed-record identity");
      }
      keyed.emplace_back(std::move(key), std::move(item));
    }
    std::sort(keyed.begin(), keyed.end(),
              [](const auto& left, const auto& right) {
                return canonical_key_tuple_less(left.first, right.first);
              });
    CanonicalValue::Array result;
    result.reserve(keyed.size());
    for (auto& item : keyed) {
      result.push_back(std::move(item.second));
    }
    return CanonicalValue(std::move(result));
  }
  if (node.kind == NodeKind::kTaggedUnion) {
    const auto& source = require_object(value, label);
    const auto& discriminator =
        require_string(require_member(source, node.tag_field, label),
                       label + "." + node.tag_field);
    const auto found = std::lower_bound(
        node.alternatives.begin(), node.alternatives.end(), discriminator,
        [](const Alternative& alternative, const std::string& tag_value) {
          return alternative.tag_value < tag_value;
        });
    if (found == node.alternatives.end() || found->tag_value != discriminator) {
      fail(label + "." + node.tag_field, "unknown discriminator " + discriminator);
    }
    return normalize_instance(*found->value_schema, value, definitions, std::move(label), depth,
                              budget);
  }
  throw ClosedSchemaError("unresolved local_ref reached instance normalization");
}

[[nodiscard]] CanonicalValue normalize_value(
    const detail::CompiledClosedSchemaData& schema,
    const CanonicalValue& candidate) {
  detail::CumulativeNormalizedValueBudget cumulative_budget;
  ValueBudget budget(cumulative_budget);
  return normalize_instance(
      *schema.definitions_by_id.at(schema.root_definition_id), candidate,
      schema.definitions_by_id, schema.schema_id.value(), 0U, budget);
}

void validate_value_output_budget(const CanonicalValue& value,
                                  std::string label, std::size_t depth,
                                  ValueBudget& budget) {
  if (const auto* object_value =
          std::get_if<CanonicalValue::Object>(&value.storage());
      object_value != nullptr) {
    const auto collection_depth = depth + 1U;
    if (collection_depth > kMaxArtifactNestingDepth) {
      fail(label, "value collection nesting exceeds " +
                      std::to_string(kMaxArtifactNestingDepth));
    }
    budget.consume_node(label);
    for (std::size_t index = 0U; index < object_value->size(); ++index) {
      const auto member_label =
          label + ".members[" + std::to_string(index) + "]";
      budget.consume_scalar(
          CanonicalValue((*object_value)[index].first),
          member_label + ".key");
      validate_value_output_budget((*object_value)[index].second,
                                   member_label + ".value",
                                   collection_depth, budget);
    }
    return;
  }
  if (const auto* array_value =
          std::get_if<CanonicalValue::Array>(&value.storage());
      array_value != nullptr) {
    const auto collection_depth = depth + 1U;
    if (collection_depth > kMaxArtifactNestingDepth) {
      fail(label, "value collection nesting exceeds " +
                      std::to_string(kMaxArtifactNestingDepth));
    }
    budget.consume_node(label);
    for (std::size_t index = 0U; index < array_value->size(); ++index) {
      validate_value_output_budget(
          (*array_value)[index],
          label + "[" + std::to_string(index) + "]", collection_depth,
          budget);
    }
    return;
  }
  budget.consume_scalar(value, label);
}

void normalize_defaults(std::vector<Definition>& definitions,
                        const ConstDefinitionMap& definitions_by_id) {
  detail::CumulativeNormalizedValueBudget cumulative_budget;
  ValueBudget budget(cumulative_budget);
  for (auto& definition : definitions) {
    std::vector<SchemaNode*> pending{definition.value_schema.get()};
    while (!pending.empty()) {
      auto* node = pending.back();
      pending.pop_back();
      if (node->kind == NodeKind::kObject) {
        for (auto& field : node->fields) {
          pending.push_back(field.value_schema.get());
          if (field.materialized_default) {
            field.default_value =
                normalize_instance(*field.value_schema, field.default_value, definitions_by_id,
                                   "definitions." + definition.definition_id + "." +
                                       field.field_id + ".default",
                                   0U, budget);
          }
        }
      } else if (node->kind == NodeKind::kArray) {
        pending.push_back(node->item_schema.get());
      } else if (node->kind == NodeKind::kTaggedUnion) {
        for (auto& alternative : node->alternatives) {
          pending.push_back(alternative.value_schema.get());
        }
      }
    }
  }
}

void validate_root_api_version(const SchemaNode& authored, const ConstDefinitionMap& definitions,
                               std::string_view schema_id) {
  const auto& root = resolve_node(authored, definitions);
  std::vector<const SchemaNode*> roots;
  if (root.kind == NodeKind::kTaggedUnion) {
    for (const auto& alternative : root.alternatives) {
      roots.push_back(&resolve_node(*alternative.value_schema, definitions));
    }
  } else {
    roots.push_back(&root);
  }
  for (const auto* branch : roots) {
    if (branch->kind != NodeKind::kObject) {
      throw ClosedSchemaError(
          "root definition and every root tagged-union branch must resolve to object");
    }
    const auto found =
        std::find_if(branch->fields.begin(), branch->fields.end(),
                     [](const Field& field) { return field.field_id == "api_version"; });
    if (found == branch->fields.end() || !found->required) {
      throw ClosedSchemaError("every root object must require api_version");
    }
    const auto& api_schema = resolve_node(*found->value_schema, definitions);
    const auto* enumeration = find_constraint(api_schema, ConstraintKind::kEnum);
    if (api_schema.kind != NodeKind::kScalar ||
        api_schema.scalar_type != ScalarType::kString || enumeration == nullptr ||
        enumeration->values.size() != 1U ||
        !scalar_equal(enumeration->values.front(), CanonicalValue(std::string(schema_id)))) {
      throw ClosedSchemaError(
          "root api_version must be a required single-value string enum equal to schema_id");
    }
  }
}

[[nodiscard]] CanonicalValue constraint_value(const Constraint& constraint) {
  if (constraint.kind == ConstraintKind::kEnum) {
    return object({{"kind", CanonicalValue(std::string(constraint_kind_name(constraint.kind)))},
                   {"values", CanonicalValue(constraint.values)}});
  }
  return object({{"kind", CanonicalValue(std::string(constraint_kind_name(constraint.kind)))},
                 {"value", constraint.value}});
}

[[nodiscard]] CanonicalValue schema_node_value(const SchemaNode& node) {
  if (node.kind == NodeKind::kScalar) {
    CanonicalValue::Array constraints;
    constraints.reserve(node.constraints.size());
    for (const auto& constraint : node.constraints) {
      constraints.push_back(constraint_value(constraint));
    }
    return object({{"kind", CanonicalValue("scalar")},
                   {"scalar_type", CanonicalValue(std::string(scalar_type_name(node.scalar_type)))},
                   {"constraints", CanonicalValue(std::move(constraints))}});
  }
  if (node.kind == NodeKind::kObject) {
    CanonicalValue::Array fields;
    fields.reserve(node.fields.size());
    for (const auto& field : node.fields) {
      const auto default_value =
          field.materialized_default
              ? object({{"kind", CanonicalValue("materialized")}, {"value", field.default_value}})
              : object({{"kind", CanonicalValue("absent")}});
      fields.push_back(
          object({{"field_id", CanonicalValue(field.field_id)},
                  {"presence", CanonicalValue(field.required ? "required" : "optional")},
                  {"default", default_value},
                  {"value_schema", schema_node_value(*field.value_schema)}}));
    }
    return object({{"kind", CanonicalValue("object")},
                   {"fields", CanonicalValue(std::move(fields))}});
  }
  if (node.kind == NodeKind::kArray) {
    CanonicalValue collection;
    if (node.collection_kind == CollectionKind::kKeyedRecordSet) {
      CanonicalValue::Array paths;
      for (const auto& path : node.key_paths) {
        CanonicalValue::Array segments;
        for (const auto& segment : path) {
          segments.emplace_back(segment);
        }
        paths.emplace_back(std::move(segments));
      }
      collection =
          object({{"kind", CanonicalValue("keyed_record_set")},
                  {"key_paths", CanonicalValue(std::move(paths))}});
    } else {
      collection =
          object({{"kind", CanonicalValue(std::string(collection_kind_name(
                                node.collection_kind)))}});
    }
    return object(
        {{"kind", CanonicalValue("array")},
         {"min_items", CanonicalValue(static_cast<double>(node.min_items))},
         {"max_items", CanonicalValue(static_cast<double>(node.max_items))},
         {"collection", std::move(collection)},
         {"item_schema", schema_node_value(*node.item_schema)}});
  }
  if (node.kind == NodeKind::kLocalRef) {
    return object({{"kind", CanonicalValue("local_ref")},
                   {"definition_id", CanonicalValue(node.definition_id)}});
  }
  CanonicalValue::Array alternatives;
  alternatives.reserve(node.alternatives.size());
  for (const auto& alternative : node.alternatives) {
    alternatives.push_back(
        object({{"tag_value", CanonicalValue(alternative.tag_value)},
                {"value_schema", schema_node_value(*alternative.value_schema)}}));
  }
  return object({{"kind", CanonicalValue("tagged_union")},
                 {"tag_field", CanonicalValue(node.tag_field)},
                 {"alternatives", CanonicalValue(std::move(alternatives))}});
}

[[nodiscard]] CanonicalValue descriptor_value(const detail::CompiledClosedSchemaData& data) {
  CanonicalValue::Array definitions;
  definitions.reserve(data.definitions.size());
  for (const auto& definition : data.definitions) {
    definitions.push_back(
        object({{"definition_id", CanonicalValue(definition.definition_id)},
                {"value_schema", schema_node_value(*definition.value_schema)}}));
  }
  return object(
      {{"api_version", CanonicalValue(std::string(kClosedSchemaApiVersion))},
       {"schema_id", CanonicalValue(data.schema_id.value())},
       {"schema_revision", CanonicalValue(data.schema_revision.value())},
       {"schema_purpose",
        CanonicalValue(data.schema_purpose == ClosedSchemaPurpose::kArtifactDocument
                           ? "artifact_document"
                           : "value")},
       {"root_definition_id", CanonicalValue(data.root_definition_id)},
       {"definitions", CanonicalValue(std::move(definitions))}});
}

}  // namespace

namespace detail {

void validate_canonical_value_output_budget(const CanonicalValue& value) {
  CumulativeNormalizedValueBudget cumulative_budget;
  validate_canonical_value_output_budget_cumulatively(
      value, cumulative_budget);
}

void validate_canonical_value_output_budget_cumulatively(
    const CanonicalValue& value,
    CumulativeNormalizedValueBudget& cumulative_budget) {
  ValueBudget budget(cumulative_budget);
  validate_value_output_budget(value, "canonical_value", 0U, budget);
}

}  // namespace detail

CompiledClosedSchema::CompiledClosedSchema(
    std::shared_ptr<const detail::CompiledClosedSchemaData> data)
    : data_(std::move(data)) {}

const core::SchemaId& CompiledClosedSchema::schema_id() const noexcept {
  return data_->schema_id;
}

const core::SchemaRevision& CompiledClosedSchema::schema_revision() const noexcept {
  return data_->schema_revision;
}

ClosedSchemaPurpose CompiledClosedSchema::schema_purpose() const noexcept {
  return data_->schema_purpose;
}

ClosedSchemaRootShape CompiledClosedSchema::root_shape() const noexcept {
  return data_->root_shape;
}

const CanonicalValue& CompiledClosedSchema::normalized_descriptor() const noexcept {
  return data_->normalized_descriptor;
}

CompiledClosedSchema compile_closed_schema_descriptor(
    const ParsedArtifactCandidate& candidate) {
  const auto& root = require_object(candidate.root(), "schema");
  require_exact_fields(root,
                       {"api_version", "schema_id", "schema_revision", "schema_purpose",
                        "root_definition_id", "definitions"},
                       "schema");
  const auto& api_version =
      require_string(require_member(root, "api_version", "schema"), "schema.api_version");
  if (api_version != kClosedSchemaApiVersion) {
    fail("schema.api_version",
         "expected " + std::string(kClosedSchemaApiVersion));
  }
  const auto& schema_id_text =
      require_string(require_member(root, "schema_id", "schema"), "schema.schema_id");
  if (schema_id_text.size() > 128U || !is_api_version(schema_id_text)) {
    fail("schema.schema_id", "expected versioned ManyForge API family");
  }
  const auto& schema_revision_text =
      require_string(require_member(root, "schema_revision", "schema"),
                     "schema.schema_revision");
  if (!is_sha256_digest(schema_revision_text)) {
    fail("schema.schema_revision", "expected sha256:<64 lowercase hex>");
  }
  auto data = std::make_shared<detail::CompiledClosedSchemaData>();
  data->schema_id = core::SchemaId(schema_id_text);
  data->schema_revision = core::SchemaRevision(schema_revision_text);
  const auto& schema_purpose =
      require_string(require_member(root, "schema_purpose", "schema"),
                     "schema.schema_purpose");
  if (schema_purpose == "artifact_document") {
    data->schema_purpose = ClosedSchemaPurpose::kArtifactDocument;
  } else if (schema_purpose == "value") {
    data->schema_purpose = ClosedSchemaPurpose::kValue;
  } else {
    fail("schema.schema_purpose", "expected artifact_document or value");
  }
  data->root_definition_id =
      require_portable_id(require_member(root, "root_definition_id", "schema"),
                          "schema.root_definition_id");

  const auto& authored_definitions =
      require_array(require_member(root, "definitions", "schema"), "schema.definitions", 1U,
                    kMaxClosedSchemaDefinitions);
  SchemaBudget budget;
  std::set<std::string, std::less<>> seen;
  data->definitions.reserve(authored_definitions.size());
  for (std::size_t index = 0U; index < authored_definitions.size(); ++index) {
    const auto label = "schema.definitions[" + std::to_string(index) + "]";
    const auto& definition = require_object(authored_definitions[index], label);
    require_exact_fields(definition, {"definition_id", "value_schema"}, label);
    auto definition_id =
        require_portable_id(require_member(definition, "definition_id", label),
                            label + ".definition_id");
    if (!seen.insert(definition_id).second) {
      fail("schema.definitions", "duplicate definition_id " + definition_id);
    }
    data->definitions.push_back(
        {std::move(definition_id),
         parse_node(require_member(definition, "value_schema", label), label + ".value_schema",
                    budget, 0U)});
  }
  std::sort(data->definitions.begin(), data->definitions.end(),
            [](const Definition& left, const Definition& right) {
              return left.definition_id < right.definition_id;
            });

  for (auto& definition : data->definitions) {
    data->definitions_by_id.emplace(definition.definition_id, definition.value_schema.get());
  }
  if (!data->definitions_by_id.contains(data->root_definition_id)) {
    fail("schema.root_definition_id",
         "unknown definition " + data->root_definition_id);
  }
  const auto dependency_first =
      validate_definition_graph(data->definitions_by_id, data->root_definition_id);
  cache_local_ref_targets(data->definitions, data->definitions_by_id,
                          dependency_first);
  validate_expanded_depths(data->definitions_by_id, dependency_first);
  validate_collection_shapes(data->definitions_by_id);
  normalize_defaults(data->definitions, data->definitions_by_id);
  const auto& resolved_root =
      resolve_node(*data->definitions_by_id.at(data->root_definition_id),
                   data->definitions_by_id);
  switch (resolved_root.kind) {
    case NodeKind::kScalar:
      data->root_shape = ClosedSchemaRootShape::kScalar;
      break;
    case NodeKind::kObject:
      data->root_shape = ClosedSchemaRootShape::kObject;
      break;
    case NodeKind::kArray:
      data->root_shape = ClosedSchemaRootShape::kArray;
      break;
    case NodeKind::kTaggedUnion:
      data->root_shape = ClosedSchemaRootShape::kTaggedUnion;
      break;
    case NodeKind::kLocalRef:
      throw ClosedSchemaError("unresolved local_ref reached root-shape qualification");
  }
  if (data->schema_purpose == ClosedSchemaPurpose::kArtifactDocument) {
    validate_root_api_version(*data->definitions_by_id.at(data->root_definition_id),
                              data->definitions_by_id, data->schema_id.value());
  }
  data->normalized_descriptor = descriptor_value(*data);

  const detail::ContentIdentitySpec identity_spec{
      "schema-revision", std::string(kClosedSchemaApiVersion), "schema_revision"};
  bool verified = false;
  try {
    verified = detail::verify_content_identity(data->normalized_descriptor, identity_spec);
  } catch (const CanonicalValueError& error) {
    throw ClosedSchemaError(std::string("schema.schema_revision: ") + error.what());
  }
  if (!verified) {
    fail("schema.schema_revision", "digest mismatch");
  }
  return CompiledClosedSchema(std::move(data));
}

UnqualifiedSchemaNormalizedArtifactCandidate
validate_and_normalize_unqualified_artifact_candidate(
    const CompiledClosedSchema& schema, const ParsedArtifactCandidate& candidate) {
  auto normalized = normalize_value(*schema.data_, candidate.root());
  return UnqualifiedSchemaNormalizedArtifactCandidate(
      schema.data_->schema_id, schema.data_->schema_revision,
      schema.data_->schema_purpose, std::move(normalized));
}

NonGraphQualifiedSchemaNormalizedValue
validate_and_normalize_build_qualified_value(
    const BuildQualifiedValueSchemaEntry& schema,
    const CanonicalValue& candidate) {
  const auto& compiled_schema = schema.compiled_schema();
  return NonGraphQualifiedSchemaNormalizedValue(
      schema.schema_id(), schema.schema_revision(),
      normalize_value(*compiled_schema.data_, candidate));
}

struct BuildQualifiedClosedSchemaRegistry::Storage final {
  std::vector<BuildCatalogSchemaEntry> catalog_schemas;
  std::vector<BuildQualifiedValueSchemaEntry> value_schemas;
  CanonicalValue normalized_projection;
  core::ContentDigest qualification_registry_digest{
      "sha256:0000000000000000000000000000000000000000000000000000000000000000"};
};

BuildQualifiedClosedSchemaRegistry::BuildQualifiedClosedSchemaRegistry() {
  struct QualifiedCatalogSchemaSource final {
    core::CatalogRecordKind record_kind;
    std::string_view relative_path;
    std::string_view descriptor_yaml;
  };
  struct QualifiedValueSchemaSource final {
    std::string_view relative_path;
    std::string_view descriptor_yaml;
  };
  const std::array<QualifiedCatalogSchemaSource, 8U> qualified_sources{{
      {core::CatalogRecordKind::kCapabilityDefinition,
       "catalog/manyforge.capability_definition.v1.yaml",
       detail::kEmbeddedCapabilityDefinitionSchema},
      {core::CatalogRecordKind::kComponentType,
       "catalog/manyforge.component_type.v1.yaml",
       detail::kEmbeddedComponentTypeSchema},
      {core::CatalogRecordKind::kControlInterfaceContract,
       "catalog/manyforge.control_interface_contract.v1.yaml",
       detail::kEmbeddedControlInterfaceContractSchema},
      {core::CatalogRecordKind::kControllerTopology,
       "catalog/manyforge.controller_topology.v1.yaml",
       detail::kEmbeddedControllerTopologySchema},
      {core::CatalogRecordKind::kModelAsset,
       "catalog/manyforge.model_asset.v1.yaml",
       detail::kEmbeddedModelAssetSchema},
      {core::CatalogRecordKind::kProviderManifest,
       "catalog/manyforge.provider_manifest.v1.yaml",
       detail::kEmbeddedProviderManifestSchema},
      {core::CatalogRecordKind::kRuntimeProfile,
       "catalog/manyforge.runtime_profile.v1.yaml",
       detail::kEmbeddedRuntimeProfileSchema},
      {core::CatalogRecordKind::kStopEvidencePolicy,
       "catalog/manyforge.stop_evidence_policy.v1.yaml",
       detail::kEmbeddedStopEvidencePolicySchema},
  }};
  const std::array<QualifiedValueSchemaSource, 2U> qualified_value_sources{{
      {"value/manyforge.value_schema.provider_contract.empty.v1.yaml",
       detail::kEmbeddedEmptyProviderContractSchema},
      {"value/manyforge.value_schema.runtime.empty.v1.yaml",
       detail::kEmbeddedEmptyRuntimeConfigurationSchema},
  }};

  auto storage = std::make_shared<Storage>();
  storage->catalog_schemas.reserve(core::kCatalogRecordTypeRegistry.size());
  for (const auto& descriptor : core::kCatalogRecordTypeRegistry) {
    const auto source =
        std::find_if(qualified_sources.begin(), qualified_sources.end(),
                     [&](const QualifiedCatalogSchemaSource& candidate) {
                       return candidate.record_kind == descriptor.record_kind;
                     });
    std::shared_ptr<const CompiledClosedSchema> compiled_schema;
    std::string relative_path;
    std::string descriptor_bytes;
    if (source != qualified_sources.end()) {
      compiled_schema = std::make_shared<const CompiledClosedSchema>(
          compile_closed_schema_descriptor(
              parse_strict_json_model_yaml(source->descriptor_yaml)));
      relative_path = source->relative_path;
      descriptor_bytes = source->descriptor_yaml;
    }
    storage->catalog_schemas.push_back(BuildCatalogSchemaEntry(
        descriptor.record_kind, core::SchemaId(std::string(descriptor.schema_family)),
        compiled_schema == nullptr ? CatalogSchemaQualification::kUnqualified
                                   : CatalogSchemaQualification::kQualified,
        std::move(compiled_schema), std::move(relative_path),
        std::move(descriptor_bytes)));
  }
  std::sort(storage->catalog_schemas.begin(), storage->catalog_schemas.end(),
            [](const BuildCatalogSchemaEntry& left, const BuildCatalogSchemaEntry& right) {
              return core::to_string(left.record_kind()) <
                     core::to_string(right.record_kind());
            });

  storage->value_schemas.reserve(qualified_value_sources.size());
  for (const auto& source : qualified_value_sources) {
    auto compiled_schema = std::make_shared<const CompiledClosedSchema>(
        compile_closed_schema_descriptor(
            parse_strict_json_model_yaml(source.descriptor_yaml)));
    storage->value_schemas.push_back(BuildQualifiedValueSchemaEntry(
        compiled_schema->schema_id(), compiled_schema->schema_revision(),
        std::move(compiled_schema), std::string(source.relative_path),
        std::string(source.descriptor_yaml)));
  }
  std::sort(
      storage->value_schemas.begin(), storage->value_schemas.end(),
      [](const BuildQualifiedValueSchemaEntry& left,
         const BuildQualifiedValueSchemaEntry& right) {
        return std::tie(left.schema_id().value(), left.schema_revision().value()) <
               std::tie(right.schema_id().value(), right.schema_revision().value());
      });

  CanonicalValue::Array examples;
  examples.push_back(object(
      {{"schema_id",
        CanonicalValue("manyforge.closed_schema.example_command.v1")},
       {"schema_revision",
        CanonicalValue(
            "sha256:37985bd40f1c50cca9a82e3952bf407ddecca0d1c32d7787cd903bfffbf5fce7")},
       {"relative_path",
        CanonicalValue(
            "examples/manyforge.closed_schema.example_command.v1.yaml")},
       {"qualification", CanonicalValue("dialect_example_only")}}));
  examples.push_back(object(
      {{"schema_id",
        CanonicalValue("manyforge.closed_schema.example_inventory.v1")},
       {"schema_revision",
        CanonicalValue(
            "sha256:dd5840f1d78b171d86631d73b333625011c81fcae9b54221ab3ea63c65446c55")},
       {"relative_path",
        CanonicalValue(
            "examples/manyforge.closed_schema.example_inventory.v1.yaml")},
       {"qualification", CanonicalValue("dialect_example_only")}}));

  CanonicalValue::Array catalog_schemas;
  catalog_schemas.reserve(storage->catalog_schemas.size());
  for (const auto& entry : storage->catalog_schemas) {
    if (entry.qualification() == CatalogSchemaQualification::kQualified) {
      catalog_schemas.push_back(object(
          {{"record_kind",
            CanonicalValue(std::string(core::to_string(entry.record_kind())))},
           {"schema_id", CanonicalValue(entry.schema_id().value())},
           {"schema_revision",
            CanonicalValue(entry.compiled_schema()->schema_revision().value())},
           {"relative_path", CanonicalValue(std::string(entry.relative_path()))},
           {"qualification", CanonicalValue("qualified")}}));
      continue;
    }
    catalog_schemas.push_back(object(
        {{"record_kind",
          CanonicalValue(std::string(core::to_string(entry.record_kind())))},
         {"schema_id", CanonicalValue(entry.schema_id().value())},
         {"qualification", CanonicalValue("unqualified")}}));
  }
  CanonicalValue::Array value_schemas;
  value_schemas.reserve(storage->value_schemas.size());
  for (const auto& entry : storage->value_schemas) {
    value_schemas.push_back(object(
        {{"schema_id", CanonicalValue(entry.schema_id().value())},
         {"schema_revision", CanonicalValue(entry.schema_revision().value())},
         {"relative_path", CanonicalValue(std::string(entry.relative_path()))},
         {"qualification", CanonicalValue("qualified")}}));
  }
  storage->normalized_projection = object(
      {{"api_version",
        CanonicalValue("manyforge.closed_schema_registry.v1")},
       {"dialect_api_version",
        CanonicalValue(std::string(kClosedSchemaApiVersion))},
       {"bootstrap_contract_digest",
        CanonicalValue(std::string(kClosedSchemaBootstrapContractDigest))},
       {"example_schemas", CanonicalValue(std::move(examples))},
       {"catalog_schemas", CanonicalValue(std::move(catalog_schemas))},
       {"value_schemas", CanonicalValue(std::move(value_schemas))}});
  storage->qualification_registry_digest =
      core::ContentDigest(sha256_digest(canonical_json(storage->normalized_projection)));
  storage_ = std::move(storage);
}

std::span<const BuildCatalogSchemaEntry>
BuildQualifiedClosedSchemaRegistry::catalog_schemas() const noexcept {
  return storage_->catalog_schemas;
}

std::span<const BuildQualifiedValueSchemaEntry>
BuildQualifiedClosedSchemaRegistry::value_schemas() const noexcept {
  return storage_->value_schemas;
}

const BuildCatalogSchemaEntry*
BuildQualifiedClosedSchemaRegistry::find_catalog_schema_family(
    core::CatalogRecordKind kind) const noexcept {
  const auto found =
      std::find_if(storage_->catalog_schemas.begin(), storage_->catalog_schemas.end(),
                   [kind](const BuildCatalogSchemaEntry& entry) {
                     return entry.record_kind() == kind;
                   });
  return found == storage_->catalog_schemas.end() ? nullptr : &*found;
}

const BuildCatalogSchemaEntry*
BuildQualifiedClosedSchemaRegistry::find_qualified_catalog_schema(
    core::CatalogRecordKind kind, const core::SchemaId& schema_id,
    const core::SchemaRevision& schema_revision) const noexcept {
  const auto* entry = find_catalog_schema_family(kind);
  if (entry == nullptr ||
      entry->qualification() != CatalogSchemaQualification::kQualified ||
      entry->schema_id() != schema_id || entry->compiled_schema() == nullptr ||
      entry->compiled_schema()->schema_revision() != schema_revision) {
    return nullptr;
  }
  return entry;
}

const BuildQualifiedValueSchemaEntry*
BuildQualifiedClosedSchemaRegistry::find_qualified_value_schema(
    const core::SchemaId& schema_id,
    const core::SchemaRevision& schema_revision) const noexcept {
  const auto found =
      std::find_if(storage_->value_schemas.begin(), storage_->value_schemas.end(),
                   [&](const BuildQualifiedValueSchemaEntry& entry) {
                     return entry.schema_id() == schema_id &&
                            entry.schema_revision() == schema_revision;
                   });
  return found == storage_->value_schemas.end() ? nullptr : &*found;
}

const CanonicalValue&
BuildQualifiedClosedSchemaRegistry::normalized_projection() const noexcept {
  return storage_->normalized_projection;
}

const core::ContentDigest&
BuildQualifiedClosedSchemaRegistry::qualification_registry_digest() const noexcept {
  return storage_->qualification_registry_digest;
}

const BuildQualifiedClosedSchemaRegistry& build_qualified_closed_schema_registry() {
  static const BuildQualifiedClosedSchemaRegistry registry;
  return registry;
}

}  // namespace manyforge::deployment
