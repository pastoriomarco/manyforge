#include "manyforge_deployment/contact_policy_codec.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <manyforge_core/types/contact_policy.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "catalog_record_referent_internal.hpp"
#include "content_identity_internal.hpp"

namespace manyforge::deployment {
namespace {

[[noreturn]] void fail(std::string message) {
  throw ContactPolicyCodecError(std::move(message));
}

[[nodiscard]] const CanonicalValue &member(const CanonicalValue &object,
                                           std::string_view name) {
  const auto *value = object.find_object_member(name);
  if (value == nullptr) {
    fail("contact policy normalized value is missing " + std::string(name));
  }
  return *value;
}

[[nodiscard]] const std::string &text(const CanonicalValue &value,
                                      std::string_view name) {
  const auto *result = std::get_if<std::string>(&value.storage());
  if (result == nullptr) {
    fail("contact policy normalized field is not a string: " +
         std::string(name));
  }
  return *result;
}

[[nodiscard]] const CanonicalValue::Array &array(const CanonicalValue &value,
                                                 std::string_view name) {
  const auto *result = std::get_if<CanonicalValue::Array>(&value.storage());
  if (result == nullptr) {
    fail("contact policy normalized field is not an array: " +
         std::string(name));
  }
  return *result;
}

[[nodiscard]] double number(const CanonicalValue &value,
                            std::string_view name) {
  const auto *result = std::get_if<double>(&value.storage());
  if (result == nullptr || !std::isfinite(*result)) {
    fail("contact policy normalized field is not finite: " + std::string(name));
  }
  return *result;
}

[[nodiscard]] bool boolean(const CanonicalValue &value, std::string_view name) {
  const auto *result = std::get_if<bool>(&value.storage());
  if (result == nullptr) {
    fail("contact policy normalized field is not boolean: " +
         std::string(name));
  }
  return *result;
}

[[nodiscard]] core::ContactPolicyViolationResponseDTO
response(const CanonicalValue &value, std::string_view name) {
  const auto &token = text(value, name);
  if (token == "warn") {
    return core::ContactPolicyViolationResponseDTO::kWarn;
  }
  if (token == "soft_stop") {
    return core::ContactPolicyViolationResponseDTO::kSoftStop;
  }
  if (token == "hard_stop") {
    return core::ContactPolicyViolationResponseDTO::kHardStop;
  }
  fail("contact policy has an unknown violation response");
}

[[nodiscard]] core::ContactBodySelectorDTO
decode_selector(const CanonicalValue &value, std::string_view path) {
  const auto &kind = text(member(value, "kind"), std::string(path) + ".kind");
  if (kind == "component_model_body") {
    const auto &body = member(value, "body");
    return core::ContactComponentModelBodySelectorDTO{{
        core::ControllableComponentId(text(member(body, "component_id"),
                                           std::string(path) + ".body.component_id")),
        core::CollisionBodyRoleId(text(member(body, "collision_body_id"),
                                       std::string(path) + ".body.collision_body_id"))}};
  }
  if (kind == "scene_resource") {
    return core::ContactSceneResourceSelectorDTO{
        core::SceneResourceId(text(member(value, "scene_resource_id"),
                                   std::string(path) + ".scene_resource_id"))};
  }
  if (kind == "runtime_object") {
    return core::ContactRuntimeObjectSelectorDTO{
        core::RuntimeObjectId(text(member(value, "runtime_object_id"),
                                   std::string(path) + ".runtime_object_id"))};
  }
  if (kind == "motion_endpoint") {
    return core::ContactMotionEndpointSelectorDTO{};
  }
  fail("contact policy has an unknown body selector kind");
}

struct DecodedPair final {
  core::ContactBodyPairDTO value;
  std::string canonical_key;
};

[[nodiscard]] DecodedPair decode_pair(const CanonicalValue &value,
                                      std::string_view path) {
  const auto &body_a = member(value, "body_a");
  const auto &body_b = member(value, "body_b");
  const std::string body_a_key = canonical_json(body_a);
  const std::string body_b_key = canonical_json(body_b);
  if (body_a_key >= body_b_key) {
    fail("contact policy body pair must contain two distinct selectors in "
         "canonical order");
  }
  std::string key = body_a_key;
  key.push_back('\0');
  key.append(body_b_key);
  return {core::ContactBodyPairDTO{
              decode_selector(body_a, std::string(path) + ".body_a"),
              decode_selector(body_b, std::string(path) + ".body_b")},
          std::move(key)};
}

[[nodiscard]] std::optional<std::string>
optional_text(const CanonicalValue &value, std::string_view field,
              std::string_view path) {
  const auto *selected = value.find_object_member(field);
  if (selected == nullptr) {
    return std::nullopt;
  }
  return text(*selected, std::string(path) + "." + std::string(field));
}

struct DecodedZone final {
  core::ContactZoneDTO value;
  std::string zone_id;
  std::vector<std::string> pair_keys;
};

[[nodiscard]] DecodedZone decode_zone(const CanonicalValue &value,
                                      core::ContactPolicyModeDTO mode,
                                      std::size_t zone_index) {
  const std::string path = "zones[" + std::to_string(zone_index) + "]";
  const std::string zone_id = text(member(value, "zone_id"), path + ".zone_id");
  std::vector<core::ContactBodyPairDTO> pairs;
  std::vector<std::string> pair_keys;
  const auto &authored_pairs =
      array(member(value, "body_pairs"), path + ".body_pairs");
  pairs.reserve(authored_pairs.size());
  pair_keys.reserve(authored_pairs.size());
  for (std::size_t index = 0U; index < authored_pairs.size(); ++index) {
    auto decoded =
        decode_pair(authored_pairs[index],
                    path + ".body_pairs[" + std::to_string(index) + "]");
    if (!pair_keys.empty() && pair_keys.back() >= decoded.canonical_key) {
      fail("contact policy body_pairs must be unique and canonical-sorted");
    }
    pairs.push_back(std::move(decoded.value));
    pair_keys.push_back(std::move(decoded.canonical_key));
  }

  core::ContactZoneEnvelopeDTO envelope;
  if (mode == core::ContactPolicyModeDTO::kContactRelease) {
    const auto &token = text(member(value, "release_evidence_mode"),
                             path + ".release_evidence_mode");
    const auto evidence =
        token == "signed_penetration"
            ? core::ContactReleaseEvidenceModeDTO::kSignedPenetration
        : token == "binary_contact"
            ? core::ContactReleaseEvidenceModeDTO::kBinaryContact
            : throw ContactPolicyCodecError(
                  "contact policy has an unknown release evidence "
                  "mode");
    envelope = core::ContactReleaseEnvelopeDTO{
        evidence,
        boolean(member(value, "enforce_max_start_penetration"),
                path + ".enforce_max_start_penetration"),
        number(member(value, "max_start_penetration_m"),
               path + ".max_start_penetration_m"),
        number(member(value, "min_release_progress_m"),
               path + ".min_release_progress_m"),
        number(member(value, "release_tolerance_m"),
               path + ".release_tolerance_m"),
        boolean(member(value, "require_strictly_decreasing_penetration"),
                path + ".require_strictly_decreasing_penetration"),
        boolean(member(value, "require_separation_by_end"),
                path + ".require_separation_by_end"),
        boolean(member(value, "allow_recontact"), path + ".allow_recontact")};
  } else if (mode == core::ContactPolicyModeDTO::kBoundedPenetration) {
    envelope = core::ContactBoundedPenetrationEnvelopeDTO{
        number(member(value, "max_contact_penetration_m"),
               path + ".max_contact_penetration_m")};
  } else if (mode == core::ContactPolicyModeDTO::kContactGrasp) {
    envelope = core::ContactGraspEnvelopeDTO{
        number(member(value, "max_penetration_m"), path + ".max_penetration_m"),
        number(member(value, "terminal_window_start_fraction"),
               path + ".terminal_window_start_fraction")};
  } else {
    fail("reserved contact-policy modes cannot be qualified");
  }

  return {core::ContactZoneDTO{zone_id, std::move(pairs),
                               response(member(value, "violation_response"),
                                        path + ".violation_response"),
                               std::move(envelope),
                               optional_text(value, "notes", path)},
          zone_id, std::move(pair_keys)};
}

[[nodiscard]] core::ContactPolicyDTO
decode_normalized(const CanonicalValue &normalized) {
  const auto &mode_token = text(member(normalized, "mode"), "mode");
  core::ContactPolicyModeDTO mode;
  if (mode_token == "contact_release") {
    mode = core::ContactPolicyModeDTO::kContactRelease;
  } else if (mode_token == "bounded_penetration") {
    mode = core::ContactPolicyModeDTO::kBoundedPenetration;
  } else if (mode_token == "contact_grasp") {
    mode = core::ContactPolicyModeDTO::kContactGrasp;
  } else {
    fail("reserved contact-policy modes cannot be qualified");
  }

  const auto &sensing_token =
      text(member(normalized, "sensing_mode"), "sensing_mode");
  if (sensing_token != "geometry_only") {
    fail("the qualified contact-policy v1 modalities require geometry_only "
         "sensing");
  }

  std::vector<core::ContactZoneDTO> zones;
  std::set<std::string> all_pairs;
  const auto &zone_values = array(member(normalized, "zones"), "zones");
  zones.reserve(zone_values.size());
  std::string previous_zone_id;
  for (std::size_t index = 0U; index < zone_values.size(); ++index) {
    auto decoded = decode_zone(zone_values[index], mode, index);
    for (const auto &pair : decoded.value.body_pairs) {
      const bool endpoint_a =
          std::holds_alternative<core::ContactMotionEndpointSelectorDTO>(
              pair.body_a);
      const bool endpoint_b =
          std::holds_alternative<core::ContactMotionEndpointSelectorDTO>(
              pair.body_b);
      if ((endpoint_a || endpoint_b) &&
          ((mode != core::ContactPolicyModeDTO::kContactGrasp &&
            mode != core::ContactPolicyModeDTO::kContactRelease) ||
           zone_values.size() != 1U || decoded.value.body_pairs.size() != 1U ||
           !((endpoint_a &&
              std::holds_alternative<core::ContactRuntimeObjectSelectorDTO>(
                  pair.body_b)) ||
             (endpoint_b &&
              std::holds_alternative<core::ContactRuntimeObjectSelectorDTO>(
                  pair.body_a))))) {
        fail("motion_endpoint requires one exclusive grasp or release pair with a "
             "runtime object");
      }
    }
    if (index != 0U && previous_zone_id >= decoded.zone_id) {
      fail("contact policy zones must be unique and canonical-sorted by "
           "zone_id");
    }
    previous_zone_id = decoded.zone_id;
    for (const auto &pair_key : decoded.pair_keys) {
      if (!all_pairs.insert(pair_key).second) {
        fail("contact policy has a body pair in more than one zone");
      }
      if (all_pairs.size() > core::kMaxContactPolicyBodyPairs) {
        fail("contact policy aggregate body-pair count exceeds its fixed "
             "bound");
      }
    }
    zones.push_back(std::move(decoded.value));
  }

  return core::ContactPolicyDTO{
      core::ContactPolicyId(text(member(normalized, "policy_id"), "policy_id")),
      core::ContactPolicyRevision(
          text(member(normalized, "version"), "version")),
      optional_text(normalized, "description", "contact_policy"),
      mode,
      core::ContactPolicySensingModeDTO::kGeometryOnly,
      response(member(normalized, "default_unmatched_response"),
               "default_unmatched_response"),
      std::move(zones)};
}

} // namespace

QualifiedContactPolicy resolve_and_qualify_contact_policy(
    const core::ContactPolicyCatalogRecordRefDTO &reference,
    const core::ContentAddressedStoreKey &resolved_store_key,
    std::string_view stored_bytes) {
  UnqualifiedSchemaNormalizedArtifactCandidate normalized =
      [&reference, &resolved_store_key, stored_bytes]() {
        try {
          return detail::resolve_and_normalize_exact_catalog_referent(
              reference, resolved_store_key, stored_bytes,
              core::kContactPolicyApiVersion, "contact policy");
        } catch (const std::exception &error) {
          fail(error.what());
        }
      }();

  const detail::ContentIdentitySpec identity_spec{
      detail::catalog_record_identity_domain("contact_policy"),
      std::string(core::kContactPolicyApiVersion), "version"};
  if (!detail::verify_content_identity(normalized.normalized_root(),
                                       identity_spec)) {
    fail("contact policy version does not match normalized semantic content");
  }

  auto decoded = decode_normalized(normalized.normalized_root());
  if (decoded.contact_policy_id != reference.record_id ||
      decoded.contact_policy_revision != reference.record_revision) {
    fail("contact-policy typed ID/revision selector does not match its "
         "referent");
  }
  return QualifiedContactPolicy(reference, std::move(decoded),
                                normalized.normalized_root());
}

} // namespace manyforge::deployment
