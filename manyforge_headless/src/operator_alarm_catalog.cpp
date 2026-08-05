#include "manyforge_headless/operator_alarm_catalog.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/artifact_codec.hpp>

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;
constexpr std::size_t kMaximumDefinitionTextBytes = 1024U;
constexpr std::size_t kMaximumDefinitionIdentityBytes = 256U;
constexpr std::size_t kMaximumRuntimeModes = 32U;

[[noreturn]] void Fail(const std::string &detail) {
  throw OperatorAlarmCatalogError("operator alarm catalog: " + detail);
}

void Require(const bool condition, const std::string &detail) {
  if (!condition) {
    Fail(detail);
  }
}

const CanonicalValue &RequiredMember(const CanonicalValue &root,
                                     const std::string_view name) {
  const auto *member = root.find_object_member(name);
  Require(member != nullptr, "missing field " + std::string(name));
  return *member;
}

void RequireClosedObject(
    const CanonicalValue &root,
    const std::initializer_list<std::string_view> field_names,
    const std::string_view label) {
  Require(root.is_object() && root.as_object().size() == field_names.size(),
          std::string(label) + " is not the closed v1 shape");
  for (const auto name : field_names) {
    static_cast<void>(RequiredMember(root, name));
  }
}

std::string RequiredString(const CanonicalValue &root,
                           const std::string_view name,
                           const std::size_t maximum_bytes) {
  const auto *value =
      std::get_if<std::string>(&RequiredMember(root, name).storage());
  Require(value != nullptr && !value->empty() && value->size() <= maximum_bytes,
          "field " + std::string(name) +
              " must be one bounded non-empty string");
  Require(content_identity::is_utf8_nfc(*value),
          "field " + std::string(name) + " must be NFC UTF-8");
  return *value;
}

bool RequiredBool(const CanonicalValue &root, const std::string_view name) {
  const auto *value = std::get_if<bool>(&RequiredMember(root, name).storage());
  Require(value != nullptr, "field " + std::string(name) + " must be boolean");
  return *value;
}

std::uint64_t RequiredUnsigned(const CanonicalValue &root,
                               const std::string_view name,
                               const bool nonzero) {
  const auto *value =
      std::get_if<double>(&RequiredMember(root, name).storage());
  Require(value != nullptr && std::isfinite(*value) && *value >= 0.0 &&
              std::floor(*value) == *value &&
              *value <= static_cast<double>(kMaximumSafeInteger) &&
              (!nonzero || *value != 0.0),
          "field " + std::string(name) + " must be an I-JSON safe integer");
  return static_cast<std::uint64_t>(*value);
}

std::vector<std::string>
RequiredStringArray(const CanonicalValue &root, const std::string_view name,
                    const std::size_t minimum_size,
                    const std::size_t maximum_size,
                    const std::size_t maximum_item_bytes) {
  const auto *values =
      std::get_if<CanonicalValue::Array>(&RequiredMember(root, name).storage());
  Require(values != nullptr && values->size() >= minimum_size &&
              values->size() <= maximum_size,
          "field " + std::string(name) + " violates its array bound");
  std::vector<std::string> result;
  result.reserve(values->size());
  std::set<std::string> unique;
  for (const auto &item : *values) {
    const auto *value = std::get_if<std::string>(&item.storage());
    Require(value != nullptr && !value->empty() &&
                value->size() <= maximum_item_bytes &&
                content_identity::is_utf8_nfc(*value),
            "field " + std::string(name) + " contains an invalid string");
    Require(unique.insert(*value).second,
            "field " + std::string(name) + " contains a duplicate");
    result.push_back(*value);
  }
  return result;
}

bool IsDottedIdentifier(const std::string_view value) noexcept {
  if (value.empty() || value.front() < 'a' || value.front() > 'z') {
    return false;
  }
  bool saw_dot = false;
  bool after_dot = false;
  for (const char character : value) {
    if (character == '.') {
      if (after_dot) {
        return false;
      }
      saw_dot = true;
      after_dot = true;
      continue;
    }
    const bool valid = (character >= 'a' && character <= 'z') ||
                       (character >= '0' && character <= '9') ||
                       character == '_';
    if (!valid || (after_dot && !(character >= 'a' && character <= 'z'))) {
      return false;
    }
    after_dot = false;
  }
  return saw_dot && !after_dot;
}

bool IsRuntimeMode(const std::string_view value) noexcept {
  if (value.empty() || value.front() < 'a' || value.front() > 'z') {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](const char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_';
  });
}

void ValidateMessageTemplate(const std::string_view value) {
  constexpr std::string_view kPlaceholders[] = {
      "{subject_kind}",
      "{subject_id}",
      "{resource_ids}",
  };
  std::size_t offset = 0U;
  while (offset < value.size()) {
    if (value[offset] == '}') {
      Fail("message_template contains an unmatched closing brace");
    }
    if (value[offset] != '{') {
      ++offset;
      continue;
    }
    const auto matched = std::find_if(
        std::begin(kPlaceholders), std::end(kPlaceholders),
        [value, offset](const auto placeholder) {
          return value.substr(offset, placeholder.size()) == placeholder;
        });
    Require(matched != std::end(kPlaceholders),
            "message_template contains an unsupported placeholder");
    offset += matched->size();
  }
}

OperatorAlarmDefinition DecodeDefinition(const CanonicalValue &root) {
  RequireClosedObject(root,
                      {"definition_id", "version", "title", "message_template",
                       "alarm_class", "priority", "priority_rationale",
                       "consequence", "operator_response", "scope_fields",
                       "set_authority", "clear_authority", "adapter_id",
                       "runtime_modes", "acknowledgement_required",
                       "acknowledgement_reason_required",
                       "requires_motion_attention", "max_evidence_refs"},
                      "alarm definition");
  auto definition_id =
      RequiredString(root, "definition_id", kMaximumDefinitionIdentityBytes);
  Require(IsDottedIdentifier(definition_id),
          "definition_id is not a lowercase dotted identifier");
  auto scope_fields = RequiredStringArray(root, "scope_fields", 3U, 3U,
                                          kMaximumDefinitionIdentityBytes);
  Require(scope_fields == std::vector<std::string>(
                              {"subject_kind", "subject_id", "resource_ids"}),
          "scope_fields must use the canonical ordered subject shape");
  auto alarm_class =
      RequiredString(root, "alarm_class", kMaximumDefinitionIdentityBytes);
  Require(alarm_class == "motion" || alarm_class == "scene" ||
              alarm_class == "runtime_health",
          "alarm_class is unsupported");
  auto priority =
      RequiredString(root, "priority", kMaximumDefinitionIdentityBytes);
  Require(priority == "critical" || priority == "high" ||
              priority == "medium" || priority == "low",
          "priority is unsupported");
  auto runtime_modes =
      RequiredStringArray(root, "runtime_modes", 1U, kMaximumRuntimeModes,
                          kMaximumDefinitionIdentityBytes);
  Require(std::all_of(runtime_modes.begin(), runtime_modes.end(),
                      [](const auto &mode) { return IsRuntimeMode(mode); }),
          "runtime_modes contains an invalid identifier");
  if (std::find(runtime_modes.begin(), runtime_modes.end(), "all") !=
      runtime_modes.end()) {
    Require(runtime_modes.size() == 1U,
            "runtime_modes all cannot be combined with another mode");
  }
  const auto max_evidence_refs =
      RequiredUnsigned(root, "max_evidence_refs", true);
  Require(max_evidence_refs <= 32U,
          "max_evidence_refs must be between 1 and 32");
  const bool acknowledgement_required =
      RequiredBool(root, "acknowledgement_required");
  Require(acknowledgement_required,
          "every initial alarm definition requires acknowledgement");
  auto message_template =
      RequiredString(root, "message_template", kMaximumDefinitionTextBytes);
  ValidateMessageTemplate(message_template);
  auto set_authority =
      RequiredString(root, "set_authority", kMaximumDefinitionIdentityBytes);
  auto clear_authority =
      RequiredString(root, "clear_authority", kMaximumDefinitionIdentityBytes);
  auto adapter_id =
      RequiredString(root, "adapter_id", kMaximumDefinitionIdentityBytes);
  Require(IsDottedIdentifier(set_authority) &&
              IsDottedIdentifier(clear_authority) &&
              IsDottedIdentifier(adapter_id),
          "set_authority, clear_authority, and adapter_id must be lowercase "
          "dotted identifiers");
  return {
      std::move(definition_id),
      RequiredUnsigned(root, "version", true),
      RequiredString(root, "title", kMaximumDefinitionTextBytes),
      std::move(message_template),
      std::move(alarm_class),
      std::move(priority),
      RequiredString(root, "priority_rationale", kMaximumDefinitionTextBytes),
      RequiredString(root, "consequence", kMaximumDefinitionTextBytes),
      RequiredString(root, "operator_response", kMaximumDefinitionTextBytes),
      std::move(set_authority),
      std::move(clear_authority),
      std::move(adapter_id),
      std::move(runtime_modes),
      acknowledgement_required,
      RequiredBool(root, "acknowledgement_reason_required"),
      RequiredBool(root, "requires_motion_attention"),
      max_evidence_refs,
  };
}

std::string ReadRegularFile(const std::filesystem::path &path) {
  const int descriptor =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    Fail(std::string("cannot open runtime catalog: ") +
         std::system_error(errno, std::generic_category()).what());
  }
  struct Descriptor final {
    int value;
    ~Descriptor() { static_cast<void>(::close(value)); }
  } file{descriptor};
  struct stat status {};
  if (::fstat(file.value, &status) != 0) {
    Fail(std::string("cannot inspect runtime catalog: ") +
         std::system_error(errno, std::generic_category()).what());
  }
  Require(S_ISREG(status.st_mode) && status.st_size > 0 &&
              static_cast<std::uint64_t>(status.st_size) <=
                  kMaximumOperatorAlarmCatalogBytes,
          "runtime catalog must be one bounded regular file");

  std::string bytes;
  bytes.reserve(static_cast<std::size_t>(status.st_size));
  char buffer[8192];
  while (bytes.size() < static_cast<std::size_t>(status.st_size)) {
    const auto count = ::read(file.value, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      Fail(std::string("cannot read runtime catalog: ") +
           std::system_error(errno, std::generic_category()).what());
    }
    if (count == 0) {
      break;
    }
    bytes.append(buffer, static_cast<std::size_t>(count));
    Require(bytes.size() <= kMaximumOperatorAlarmCatalogBytes,
            "runtime catalog exceeded its byte bound while reading");
  }
  Require(bytes.size() == static_cast<std::size_t>(status.st_size),
          "runtime catalog size changed while reading");
  return bytes;
}

} // namespace

OperatorAlarmCatalog::OperatorAlarmCatalog(
    std::string schema_version, std::string catalog_version,
    std::string descriptor_digest,
    std::vector<OperatorAlarmDefinition> definitions)
    : schema_version_(std::move(schema_version)),
      catalog_version_(std::move(catalog_version)),
      descriptor_digest_(std::move(descriptor_digest)),
      definitions_(std::move(definitions)) {}

OperatorAlarmCatalog
OperatorAlarmCatalog::Decode(const std::string_view yaml_bytes) {
  Require(!yaml_bytes.empty() &&
              yaml_bytes.size() <= kMaximumOperatorAlarmCatalogBytes,
          "document violates its byte bound");
  CanonicalValue root;
  try {
    root = deployment::parse_strict_json_model_yaml(yaml_bytes).root();
  } catch (const std::exception &error) {
    Fail("strict YAML decode failed: " + std::string(error.what()));
  }
  RequireClosedObject(
      root,
      {"schema_version", "catalog_version", "descriptor_digest", "definitions"},
      "catalog root");
  auto schema_version = RequiredString(root, "schema_version", 128U);
  Require(schema_version == kOperatorAlarmCatalogSchemaVersion,
          "schema_version is unsupported");
  auto catalog_version = RequiredString(root, "catalog_version", 128U);
  auto descriptor_digest = RequiredString(root, "descriptor_digest", 71U);
  Require(content_identity::is_sha256_digest(descriptor_digest),
          "descriptor_digest has invalid syntax");

  const auto &definitions_value = RequiredMember(root, "definitions");
  const auto *definitions =
      std::get_if<CanonicalValue::Array>(&definitions_value.storage());
  Require(definitions != nullptr && !definitions->empty() &&
              definitions->size() <= kMaximumOperatorAlarmDefinitions,
          "definitions violates its array bound");

  const CanonicalValue digest_value(CanonicalValue::Object{
      {"catalog_version", RequiredMember(root, "catalog_version")},
      {"definitions", definitions_value},
      {"schema_version", RequiredMember(root, "schema_version")},
  });
  const auto computed = content_identity::sha256_digest(
      content_identity::canonical_json(digest_value));
  Require(content_identity::digest_equal(computed, descriptor_digest),
          "descriptor_digest verification failed");

  std::vector<OperatorAlarmDefinition> decoded;
  decoded.reserve(definitions->size());
  std::set<std::string> definition_ids;
  for (const auto &value : *definitions) {
    auto definition = DecodeDefinition(value);
    Require(definition_ids.insert(definition.definition_id).second,
            "definition_id is duplicated");
    decoded.push_back(std::move(definition));
  }
  return OperatorAlarmCatalog(std::move(schema_version),
                              std::move(catalog_version),
                              std::move(descriptor_digest), std::move(decoded));
}

OperatorAlarmCatalog
OperatorAlarmCatalog::Load(const std::filesystem::path &path) {
  Require(!path.empty(), "runtime catalog path is empty");
  return Decode(ReadRegularFile(path));
}

const std::string &OperatorAlarmCatalog::schema_version() const noexcept {
  return schema_version_;
}

const std::string &OperatorAlarmCatalog::catalog_version() const noexcept {
  return catalog_version_;
}

const std::string &OperatorAlarmCatalog::descriptor_digest() const noexcept {
  return descriptor_digest_;
}

const std::vector<OperatorAlarmDefinition> &
OperatorAlarmCatalog::definitions() const noexcept {
  return definitions_;
}

const OperatorAlarmDefinition *OperatorAlarmCatalog::Find(
    const std::string_view definition_id) const noexcept {
  const auto found =
      std::find_if(definitions_.begin(), definitions_.end(),
                   [definition_id](const auto &definition) {
                     return definition.definition_id == definition_id;
                   });
  return found == definitions_.end() ? nullptr : &*found;
}

} // namespace manyforge::headless
