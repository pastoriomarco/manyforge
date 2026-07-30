#include "manyforge_deployment/site_overlay_resolution.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <utf8proc.h>

#include "content_identity_internal.hpp"
#include "compiler_semantic_limits_internal.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kSiteOverlayApiVersion =
    "manyforge.site_overlay.v1";
const detail::ContentIdentitySpec kSiteOverlayIdentitySpec{
    "site-overlay-revision",
    std::string(kSiteOverlayApiVersion),
    "site_overlay_revision",
    detail::ContentIdentityRootShape::kDocument,
};

struct NormalizedSiteOverlayValueRow final {
  core::SiteOverlayValueRef value_ref;
  std::optional<core::ExternalResourceKind> external_resource_kind;
  CanonicalValue normalized_row;
};

[[noreturn]] void fail(std::string_view path, std::string_view reason) {
  throw SiteOverlayResolutionError(std::string(path) + ": " +
                                   std::string(reason));
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

[[nodiscard]] const std::string& require_string(const CanonicalValue& value,
                                                std::string_view path) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail(path, "expected string");
  }
  return *text;
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue::Object& object, std::string_view name,
    std::string_view path) {
  const auto iterator = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
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
      value.size() > detail::kMaximumPortableIdentityBytes ||
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

void require_overlay_text(std::string_view value, std::string_view path,
                          bool require_non_empty) {
  if (require_non_empty && value.empty()) {
    fail(path, "must not be empty");
  }

  utf8proc_ssize_t offset = 0;
  while (offset < static_cast<utf8proc_ssize_t>(value.size())) {
    utf8proc_int32_t code_point = 0;
    const auto consumed = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(value.data()) + offset,
        static_cast<utf8proc_ssize_t>(value.size()) - offset, &code_point);
    if (consumed <= 0) {
      fail(path, "contains invalid UTF-8");
    }
    offset += consumed;
    const auto category = utf8proc_category(code_point);
    if (code_point == 0 || category == UTF8PROC_CATEGORY_CC ||
        category == UTF8PROC_CATEGORY_CF) {
      fail(path, "must not contain NUL, Unicode control, or format characters");
    }
  }
}

void require_digest(std::string_view value, std::string_view path) {
  if (!is_sha256_digest(value)) {
    fail(path, "expected lowercase sha256:<64 hex> digest");
  }
}

[[nodiscard]] core::ExternalResourceKind parse_external_resource_kind(
    const CanonicalValue& value, std::string_view path) {
  const std::string& text = require_string(value, path);
  if (text == "model_endpoint") {
    return core::ExternalResourceKind::kModelEndpoint;
  }
  if (text == "content_store") {
    return core::ExternalResourceKind::kContentStore;
  }
  if (text == "data_source") {
    return core::ExternalResourceKind::kDataSource;
  }
  if (text == "service_endpoint") {
    return core::ExternalResourceKind::kServiceEndpoint;
  }
  fail(path, "resource kind is outside the closed v1 enum");
}

void require_external_resource_kind(core::ExternalResourceKind kind,
                                    std::string_view path) {
  switch (kind) {
    case core::ExternalResourceKind::kModelEndpoint:
    case core::ExternalResourceKind::kContentStore:
    case core::ExternalResourceKind::kDataSource:
    case core::ExternalResourceKind::kServiceEndpoint:
      return;
  }
  fail(path, "resource kind is outside the closed v1 enum");
}

[[nodiscard]] CanonicalValue parse_site_overlay_value(
    const CanonicalValue& value, std::string_view path) {
  const auto& object = require_object(value, path);
  const std::string kind_path = child_path(path, "value_kind");
  const std::string& kind =
      require_string(require_member(object, "value_kind", path), kind_path);

  if (kind == "secret_reference") {
    require_exact_fields(object,
                         {"value_kind", "secret_reference_id"}, path);
    const std::string secret_ref_path =
        child_path(path, "secret_reference_id");
    const std::string& secret_ref =
        require_string(require_member(object, "secret_reference_id", path),
                       secret_ref_path);
    require_overlay_text(secret_ref, secret_ref_path, true);
    return CanonicalValue(CanonicalValue::Object{
        {"value_kind", CanonicalValue("secret_reference")},
        {"secret_reference_id", CanonicalValue(secret_ref)},
    });
  }

  require_exact_fields(object, {"value_kind", "value"}, path);
  const CanonicalValue& payload = require_member(object, "value", path);
  const std::string payload_path = child_path(path, "value");
  if (kind == "string") {
    const std::string& text = require_string(payload, payload_path);
    require_overlay_text(text, payload_path, false);
    return CanonicalValue(CanonicalValue::Object{
        {"value_kind", CanonicalValue("string")},
        {"value", CanonicalValue(text)},
    });
  }
  if (kind == "boolean") {
    const auto* boolean = std::get_if<bool>(&payload.storage());
    if (boolean == nullptr) {
      fail(payload_path, "expected boolean");
    }
    return CanonicalValue(CanonicalValue::Object{
        {"value_kind", CanonicalValue("boolean")},
        {"value", CanonicalValue(*boolean)},
    });
  }
  if (kind == "integer") {
    const auto* number = std::get_if<double>(&payload.storage());
    if (number == nullptr || !std::isfinite(*number) ||
        std::trunc(*number) != *number ||
        std::abs(*number) >
            static_cast<double>(kMaxArtifactSafeInteger)) {
      fail(payload_path, "expected exact safe JSON integer");
    }
    return CanonicalValue(CanonicalValue::Object{
        {"value_kind", CanonicalValue("integer")},
        {"value", CanonicalValue(*number)},
    });
  }
  if (kind == "number") {
    const auto* number = std::get_if<double>(&payload.storage());
    if (number == nullptr || !std::isfinite(*number)) {
      fail(payload_path, "expected finite JSON number");
    }
    return CanonicalValue(CanonicalValue::Object{
        {"value_kind", CanonicalValue("number")},
        {"value", CanonicalValue(*number)},
    });
  }
  if (kind == "string_list") {
    const auto& source_items = require_array(payload, payload_path);
    if (source_items.size() > core::kMaxSiteOverlayValueRows) {
      fail(payload_path,
           "string-list count exceeds the 4096-item site-overlay ceiling");
    }
    CanonicalValue::Array items;
    items.reserve(source_items.size());
    for (std::size_t index = 0; index < source_items.size(); ++index) {
      const std::string item_path = index_path(payload_path, index);
      const std::string& item =
          require_string(source_items[index], item_path);
      require_overlay_text(item, item_path, false);
      items.emplace_back(item);
    }
    return CanonicalValue(CanonicalValue::Object{
        {"value_kind", CanonicalValue("string_list")},
        {"value", CanonicalValue(std::move(items))},
    });
  }
  fail(kind_path, "value kind is outside the closed v1 union");
}

[[nodiscard]] NormalizedSiteOverlayValueRow parse_site_overlay_row(
    const CanonicalValue& value, std::string_view path) {
  const auto& object = require_object(value, path);
  const std::string semantic_path = child_path(path, "semantic_kind");
  const std::string& semantic_kind =
      require_string(require_member(object, "semantic_kind", path),
                     semantic_path);
  const std::string value_ref_path =
      child_path(path, "site_overlay_value_ref");
  const std::string& value_ref =
      require_string(require_member(object, "site_overlay_value_ref", path),
                     value_ref_path);
  require_portable_token(value_ref, value_ref_path);

  if (semantic_kind == "external_resource") {
    require_exact_fields(
        object,
        {"site_overlay_value_ref", "semantic_kind", "resource_kind", "value"},
        path);
    CanonicalValue normalized_value = parse_site_overlay_value(
        require_member(object, "value", path), child_path(path, "value"));
    const core::ExternalResourceKind resource_kind =
        parse_external_resource_kind(
            require_member(object, "resource_kind", path),
            child_path(path, "resource_kind"));
    return {
        core::SiteOverlayValueRef(value_ref),
        resource_kind,
        CanonicalValue(CanonicalValue::Object{
            {"site_overlay_value_ref", CanonicalValue(value_ref)},
            {"semantic_kind", CanonicalValue("external_resource")},
            {"resource_kind",
             CanonicalValue(
                 std::string(core::to_string(resource_kind)))},
            {"value", std::move(normalized_value)},
        }),
    };
  }
  if (semantic_kind == "configuration_value") {
    require_exact_fields(
        object,
        {"site_overlay_value_ref", "semantic_kind", "value"}, path);
    CanonicalValue normalized_value = parse_site_overlay_value(
        require_member(object, "value", path), child_path(path, "value"));
    return {
        core::SiteOverlayValueRef(value_ref),
        std::nullopt,
        CanonicalValue(CanonicalValue::Object{
            {"site_overlay_value_ref", CanonicalValue(value_ref)},
            {"semantic_kind", CanonicalValue("configuration_value")},
            {"value", std::move(normalized_value)},
        }),
    };
  }
  fail(semantic_path, "semantic kind is outside the closed v1 union");
}

[[nodiscard]] auto key_tuple(
    const core::SiteOverlayExternalResourceKeyDTO& key) {
  return std::tuple(key.site_overlay_value_ref,
                    core::to_string(key.resource_kind));
}

}  // namespace

VerifiedSiteOverlayArtifactProjection
resolve_and_verify_site_overlay_artifact_projection(
    const core::SiteOverlayRefDTO& expected_reference,
    std::string_view stored_bytes) {
  require_portable_token(expected_reference.site_overlay_id.value(),
                         "site_overlay_ref.site_overlay_id");
  require_digest(expected_reference.site_overlay_revision.value(),
                 "site_overlay_ref.site_overlay_revision");
  require_digest(expected_reference.non_secret_digest.value(),
                 "site_overlay_ref.non_secret_digest");

  ParsedArtifactCandidate parsed = [&stored_bytes]() {
    try {
      return parse_strict_json_model_yaml(stored_bytes);
    } catch (const ArtifactCodecError& error) {
      fail("$", error.what());
    }
  }();
  const auto& root = require_object(parsed.root(), "$");
  require_exact_fields(
      root,
      {"api_version", "site_overlay_id", "site_overlay_revision", "values"},
      "$");

  const std::string& api_version =
      require_string(require_member(root, "api_version", "$"),
                     "$.api_version");
  if (api_version != kSiteOverlayApiVersion) {
    fail("$.api_version", "expected manyforge.site_overlay.v1");
  }
  const std::string& site_overlay_id =
      require_string(require_member(root, "site_overlay_id", "$"),
                     "$.site_overlay_id");
  require_portable_token(site_overlay_id, "$.site_overlay_id");
  const std::string& site_overlay_revision =
      require_string(require_member(root, "site_overlay_revision", "$"),
                     "$.site_overlay_revision");
  require_digest(site_overlay_revision, "$.site_overlay_revision");

  const auto& source_values =
      require_array(require_member(root, "values", "$"), "$.values");
  if (source_values.size() > core::kMaxSiteOverlayValueRows) {
    fail("$.values",
         "value count exceeds the 4096-row site-overlay ceiling");
  }
  std::vector<NormalizedSiteOverlayValueRow> values;
  values.reserve(source_values.size());
  for (std::size_t index = 0; index < source_values.size(); ++index) {
    values.push_back(parse_site_overlay_row(
        source_values[index], index_path("$.values", index)));
  }
  std::sort(values.begin(), values.end(),
            [](const auto& left, const auto& right) {
              return left.value_ref < right.value_ref;
            });
  if (std::adjacent_find(
          values.begin(), values.end(),
          [](const auto& left, const auto& right) {
            return left.value_ref == right.value_ref;
          }) != values.end()) {
    fail("$.values", "duplicate site_overlay_value_ref");
  }

  CanonicalValue::Array normalized_values;
  normalized_values.reserve(values.size());
  std::vector<core::SiteOverlayExternalResourceKeyDTO> projected_keys;
  projected_keys.reserve(values.size());
  for (auto& value : values) {
    if (value.external_resource_kind.has_value()) {
      projected_keys.push_back({
          value.value_ref,
          *value.external_resource_kind,
      });
    }
    normalized_values.push_back(std::move(value.normalized_row));
  }
  CanonicalValue normalized_root(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kSiteOverlayApiVersion))},
      {"site_overlay_id", CanonicalValue(site_overlay_id)},
      {"site_overlay_revision", CanonicalValue(site_overlay_revision)},
      {"values", CanonicalValue(std::move(normalized_values))},
  });

  bool revision_matches = false;
  try {
    revision_matches =
        detail::verify_content_identity(normalized_root,
                                        kSiteOverlayIdentitySpec);
  } catch (const CanonicalValueError& error) {
    fail("$.site_overlay_revision", error.what());
  }
  if (!revision_matches) {
    fail("$.site_overlay_revision",
         "does not match the exact normalized site-overlay artifact");
  }
  if (site_overlay_id != expected_reference.site_overlay_id.value() ||
      site_overlay_revision !=
          expected_reference.site_overlay_revision.value()) {
    fail("site_overlay_ref",
         "selected site-overlay id or revision does not match its artifact");
  }

  const std::string normalized_digest =
      sha256_digest(canonical_json(normalized_root));
  if (normalized_digest != expected_reference.non_secret_digest.value()) {
    fail("site_overlay_ref.non_secret_digest",
         "does not match the complete normalized site-overlay artifact");
  }

  return VerifiedSiteOverlayArtifactProjection(
      expected_reference, std::move(projected_keys));
}

QualifiedSelectedSiteOverlay select_site_overlay_for_deployment(
    const std::optional<core::SiteOverlayRefDTO>& deployment_reference,
    std::span<const core::ExternalResourceDTO> external_resources,
    const VerifiedSiteOverlayArtifactProjection* selected_projection) {
  if (external_resources.size() > core::kMaxDeploymentApplicabilityRows) {
    fail("external_resources",
         "resource count exceeds the 4096-row deployment ceiling");
  }

  if (!deployment_reference.has_value()) {
    if (selected_projection != nullptr) {
      fail("site_overlay_ref",
           "outer resolver supplied a site overlay when the deployment "
           "selected none");
    }
    if (!external_resources.empty()) {
      fail("external_resources",
           "external resources require a selected site_overlay_ref");
    }
    return QualifiedSelectedSiteOverlay(std::nullopt);
  }

  if (selected_projection == nullptr) {
    fail("site_overlay_ref",
         "deployment selected a site overlay but the outer resolver supplied "
         "none");
  }
  if (*deployment_reference != selected_projection->reference()) {
    fail("site_overlay_ref",
         "selected site-overlay id, revision, or non-secret digest does not "
         "match the deployment reference");
  }

  const auto keys = selected_projection->external_resource_keys();
  for (std::size_t index = 0; index < external_resources.size(); ++index) {
    const auto& resource = external_resources[index];
    const std::string path =
        "external_resources[" + std::to_string(index) + "]";
    require_portable_token(resource.site_overlay_value_ref.value(),
                           path + ".site_overlay_value_ref");
    require_external_resource_kind(resource.resource_kind,
                                   path + ".resource_kind");

    const core::SiteOverlayExternalResourceKeyDTO expected{
        resource.site_overlay_value_ref,
        resource.resource_kind,
    };
    const auto iterator = std::lower_bound(
        keys.begin(), keys.end(), expected,
        [](const auto& left, const auto& right) {
          return key_tuple(left) < key_tuple(right);
        });
    if (iterator == keys.end() || key_tuple(*iterator) != key_tuple(expected)) {
      fail(path,
           "site-overlay value reference and resource kind do not resolve "
           "through the selected projection");
    }
  }

  return QualifiedSelectedSiteOverlay(*selected_projection);
}

}  // namespace manyforge::deployment
