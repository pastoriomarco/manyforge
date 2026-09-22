#include "activation_prepared_model_closure_internal.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <manyforge_deployment/model_source.hpp>

namespace manyforge::deployment::detail {
namespace {

using Array = CanonicalValue::Array;
using Object = CanonicalValue::Object;

constexpr std::string_view kSchema =
    "manyforge.activation_prepared_model_closure.v1";
constexpr std::size_t kMaximumComponents = 128U;

[[noreturn]] void fail(std::string message) {
  throw std::invalid_argument("prepared_model_closure " + std::move(message));
}

[[nodiscard]] const Object &object(const CanonicalValue &value,
                                   std::string_view label) {
  const auto *result = std::get_if<Object>(&value.storage());
  if (result == nullptr) fail(std::string(label) + " must be an object");
  return *result;
}

[[nodiscard]] const Array &array(const CanonicalValue &value,
                                 std::string_view label) {
  const auto *result = std::get_if<Array>(&value.storage());
  if (result == nullptr) fail(std::string(label) + " must be an array");
  return *result;
}

[[nodiscard]] const CanonicalValue &member(const CanonicalValue &value,
                                           std::string_view name,
                                           std::string_view label) {
  const auto *result = value.find_object_member(name);
  if (result == nullptr) {
    fail(std::string(label) + " is missing " + std::string(name));
  }
  return *result;
}

[[nodiscard]] const std::string &text(const CanonicalValue &value,
                                      std::string_view label) {
  const auto *result = std::get_if<std::string>(&value.storage());
  if (result == nullptr || result->empty()) {
    fail(std::string(label) + " must be non-empty text");
  }
  return *result;
}

void require_fields(const CanonicalValue &value,
                    std::initializer_list<std::string_view> expected,
                    std::string_view label) {
  std::set<std::string, std::less<>> actual;
  for (const auto &[name, unused] : object(value, label)) {
    static_cast<void>(unused);
    actual.insert(name);
  }
  const std::set<std::string, std::less<>> wanted(expected.begin(),
                                                  expected.end());
  if (actual != wanted) {
    for (const auto &name : wanted) {
      if (!actual.contains(name)) {
        fail(std::string(label) + " is missing " + name);
      }
    }
    for (const auto &name : actual) {
      if (!wanted.contains(name)) {
        fail(std::string(label) + " has unexpected field " + name);
      }
    }
  }
}

[[nodiscard]] bool portable_id(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U || value.front() < 'a' ||
      value.front() > 'z') {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '.' ||
           character == '_' || character == '-';
  });
}

[[nodiscard]] CapturedModelSourceArtifact parse_entrypoint(
    const CanonicalValue &value, std::string logical_uri,
    std::string_view label) {
  auto result = parse_activation_model_source_transfer_artifact(
      value, std::move(logical_uri), kMaxModelSourceXmlEntrypointBytes, label);
  if (result.media_type != "application/xml") {
    fail(std::string(label) + " must use application/xml");
  }
  return result;
}

} // namespace

std::vector<PreparedComponentModelSource>
parse_activation_prepared_model_closure(const CanonicalValue &value) {
  require_fields(value, {"components", "schema_version"}, "root");
  if (text(member(value, "schema_version", "root"), "schema_version") !=
      kSchema) {
    fail("has an unsupported schema version");
  }
  const auto &components = array(member(value, "components", "root"),
                                 "components");
  if (components.empty() || components.size() > kMaximumComponents) {
    fail("has an invalid component count");
  }
  std::vector<PreparedComponentModelSource> result;
  result.reserve(components.size());
  std::set<std::string, std::less<>> component_ids;
  std::string previous_component;
  for (const auto &component : components) {
    require_fields(component,
                   {"accepted_package_uri_prefix", "component_id",
                    "model_asset_id", "preparation_receipt",
                    "prepared_entrypoints"},
                   "component");
    const auto &component_id =
        text(member(component, "component_id", "component"),
             "component.component_id");
    const auto &model_asset_id =
        text(member(component, "model_asset_id", "component"),
             "component.model_asset_id");
    const auto &package_prefix =
        text(member(component, "accepted_package_uri_prefix", "component"),
             "component.accepted_package_uri_prefix");
    if (!portable_id(component_id) || !portable_id(model_asset_id) ||
        !component_ids.insert(component_id).second ||
        (!previous_component.empty() && component_id < previous_component)) {
      fail("components are not uniquely canonical-sorted portable identifiers");
    }
    previous_component = component_id;
    if (!package_prefix.starts_with("package://") ||
        !package_prefix.ends_with('/')) {
      fail(component_id + " has an invalid accepted package URI prefix");
    }
    const auto &receipt = member(component, "preparation_receipt", "component");
    static_cast<void>(object(receipt, "component.preparation_receipt"));
    const auto &entrypoints =
        member(component, "prepared_entrypoints", "component");
    const auto &entrypoint_members = object(entrypoints, "prepared_entrypoints");
    if (entrypoint_members.empty() || entrypoint_members.size() > 2U ||
        entrypoints.find_object_member("urdf") == nullptr ||
        (entrypoint_members.size() == 2U &&
         entrypoints.find_object_member("srdf") == nullptr)) {
      fail(component_id + ".prepared_entrypoints has an invalid shape");
    }
    auto urdf = parse_entrypoint(
        member(entrypoints, "urdf", "prepared_entrypoints"),
        "asset://" + model_asset_id + "/model.urdf",
        component_id + ".prepared_entrypoints.urdf");
    std::optional<CapturedModelSourceArtifact> srdf;
    if (const auto *candidate = entrypoints.find_object_member("srdf");
        candidate != nullptr) {
      srdf = parse_entrypoint(
          *candidate, "asset://" + model_asset_id + "/model.srdf",
          component_id + ".prepared_entrypoints.srdf");
    }
    result.push_back({component_id, model_asset_id, package_prefix, receipt,
                      std::move(urdf), std::move(srdf)});
  }
  return result;
}

} // namespace manyforge::deployment::detail
