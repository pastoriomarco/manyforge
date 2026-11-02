#include <algorithm>
#include <exception>
#include <initializer_list>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

class Invalid final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

[[noreturn]] void fail(std::string reason) { throw Invalid(std::move(reason)); }

template <typename Value>
[[nodiscard]] Result<Value> reject(std::string code, std::string message,
                                    std::string reason) {
  return Result<Value>::Err(
      ErrorDTO{std::move(code), std::move(message), {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool portable_identifier(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U) {
    return false;
  }
  const auto alphanumeric = [](const unsigned char character) noexcept {
    return (character >= static_cast<unsigned char>('A') &&
            character <= static_cast<unsigned char>('Z')) ||
           (character >= static_cast<unsigned char>('a') &&
            character <= static_cast<unsigned char>('z')) ||
           (character >= static_cast<unsigned char>('0') &&
            character <= static_cast<unsigned char>('9'));
  };
  if (!alphanumeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [&](const char character) {
    return alphanumeric(static_cast<unsigned char>(character)) || character == '.' ||
           character == '_' || character == '-';
  });
}

const CanonicalValue::Object& object(const CanonicalValue& value) {
  if (!value.is_object()) {
    fail("object_required");
  }
  return value.as_object();
}

const CanonicalValue& member(const CanonicalValue& value, std::string_view name) {
  const auto* found = value.find_object_member(name);
  if (found == nullptr) {
    fail("required_member_missing");
  }
  return *found;
}

void exact(const CanonicalValue& value,
           std::initializer_list<std::string_view> names) {
  const auto& fields = object(value);
  if (fields.size() != names.size()) {
    fail("unknown_or_missing_member");
  }
  for (const auto name : names) {
    static_cast<void>(member(value, name));
  }
}

std::string text(const CanonicalValue& value) {
  if (!value.is_string()) {
    fail("string_required");
  }
  return value.as_string();
}

CanonicalValue capability_ref_value(const RuntimeCapabilityContractRefDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"capability_id", CanonicalValue(value.capability_id.value())},
      {"capability_definition_revision",
       CanonicalValue(value.capability_definition_revision.value())}});
}

CanonicalValue::Array capability_refs_value(
    const std::vector<RuntimeCapabilityContractRefDTO>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(capability_ref_value(value));
  }
  return result;
}

CanonicalValue::Array api_contract_ids_value(
    const std::vector<ApiContractId>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value.value());
  }
  return result;
}

CanonicalValue offer_content_value(const RuntimeContractOfferDTO& offer) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kRuntimeContractOfferApiVersion))},
      {"provided_api_contract_ids",
       CanonicalValue(api_contract_ids_value(offer.provided_api_contract_ids))},
      {"provided_capabilities",
       CanonicalValue(capability_refs_value(offer.provided_capabilities))},
      {"runtime_implementation_id",
       CanonicalValue(offer.runtime_implementation_id.value())}});
}

CanonicalValue offer_value(const RuntimeContractOfferDTO& offer) {
  auto value = offer_content_value(offer).as_object();
  value.emplace_back("offer_digest", CanonicalValue(offer.offer_digest.value()));
  return CanonicalValue(std::move(value));
}

CanonicalValue requirement_content_value(
    const RuntimeContractRequirementDTO& requirement) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kRuntimeContractRequirementApiVersion))},
      {"required_api_contract_ids",
       CanonicalValue(api_contract_ids_value(requirement.required_api_contract_ids))},
      {"required_capabilities",
       CanonicalValue(capability_refs_value(requirement.required_capabilities))},
      {"runtime_implementation_id",
       CanonicalValue(requirement.runtime_implementation_id.value())}});
}

CanonicalValue requirement_value(
    const RuntimeContractRequirementDTO& requirement) {
  auto value = requirement_content_value(requirement).as_object();
  value.emplace_back("requirement_digest",
                     CanonicalValue(requirement.requirement_digest.value()));
  return CanonicalValue(std::move(value));
}

CanonicalValue manifest_set_content_value(
    const RuntimeContractManifestSetDTO& manifest_set) {
  CanonicalValue::Array offers;
  offers.reserve(manifest_set.offers.size());
  for (const auto& offer : manifest_set.offers) {
    offers.emplace_back(offer_value(offer));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kRuntimeContractManifestSetApiVersion))},
      {"offers", CanonicalValue(std::move(offers))}});
}

CanonicalValue manifest_set_value(
    const RuntimeContractManifestSetDTO& manifest_set) {
  auto value = manifest_set_content_value(manifest_set).as_object();
  value.emplace_back("manifest_set_digest",
                     CanonicalValue(manifest_set.manifest_set_digest.value()));
  return CanonicalValue(std::move(value));
}

[[nodiscard]] bool capability_ref_less(
    const RuntimeCapabilityContractRefDTO& left,
    const RuntimeCapabilityContractRefDTO& right) noexcept {
  if (left.capability_id != right.capability_id) {
    return left.capability_id < right.capability_id;
  }
  return left.capability_definition_revision < right.capability_definition_revision;
}

[[nodiscard]] bool capability_ref_valid(
    const RuntimeCapabilityContractRefDTO& value) noexcept {
  if (!portable_identifier(value.capability_id.value()) ||
      !content_identity::is_sha256_digest(
          value.capability_definition_revision.value())) {
    return false;
  }
  const auto* definition = find_builtin_capability_definition(value.capability_id);
  return definition != nullptr;
}

[[nodiscard]] bool api_contract_ids_valid(
    const std::vector<ApiContractId>& values) noexcept {
  if (values.size() > kMaxRuntimeContractApiContractIds ||
      std::any_of(values.begin(), values.end(), [](const auto& value) {
        return !portable_identifier(value.value());
      })) {
    return false;
  }
  return std::is_sorted(values.begin(), values.end()) &&
         std::adjacent_find(values.begin(), values.end()) == values.end();
}

[[nodiscard]] bool capability_refs_valid(
    const std::vector<RuntimeCapabilityContractRefDTO>& values) noexcept {
  if (values.size() > kMaxRuntimeContractCapabilityRefs ||
      std::any_of(values.begin(), values.end(),
                  [](const auto& value) { return !capability_ref_valid(value); })) {
    return false;
  }
  return std::is_sorted(values.begin(), values.end(), capability_ref_less) &&
         std::adjacent_find(values.begin(), values.end(),
                            [](const auto& left, const auto& right) {
                              return left.capability_id == right.capability_id;
                            }) == values.end();
}

[[nodiscard]] bool offer_fields_valid(const RuntimeContractOfferDTO& offer,
                                      bool require_digest) noexcept {
  return portable_identifier(offer.runtime_implementation_id.value()) &&
         (!require_digest ||
          content_identity::is_sha256_digest(offer.offer_digest.value())) &&
         api_contract_ids_valid(offer.provided_api_contract_ids) &&
         capability_refs_valid(offer.provided_capabilities);
}

[[nodiscard]] bool requirement_fields_valid(
    const RuntimeContractRequirementDTO& requirement,
    bool require_digest) noexcept {
  return portable_identifier(requirement.runtime_implementation_id.value()) &&
         (!require_digest ||
          content_identity::is_sha256_digest(
              requirement.requirement_digest.value())) &&
         api_contract_ids_valid(requirement.required_api_contract_ids) &&
         capability_refs_valid(requirement.required_capabilities);
}

[[nodiscard]] bool manifest_set_fields_valid(
    const RuntimeContractManifestSetDTO& manifest_set) noexcept {
  if (manifest_set.offers.empty() ||
      manifest_set.offers.size() > kMaxRuntimeContractManifestOffers ||
      !content_identity::is_sha256_digest(
          manifest_set.manifest_set_digest.value())) {
    return false;
  }
  for (const auto& offer : manifest_set.offers) {
    if (!offer_fields_valid(offer, true)) {
      return false;
    }
  }
  return std::is_sorted(
             manifest_set.offers.begin(), manifest_set.offers.end(),
             [](const auto& left, const auto& right) {
               return left.runtime_implementation_id <
                      right.runtime_implementation_id;
             }) &&
         std::adjacent_find(
             manifest_set.offers.begin(), manifest_set.offers.end(),
             [](const auto& left, const auto& right) {
               return left.runtime_implementation_id ==
                      right.runtime_implementation_id;
             }) == manifest_set.offers.end();
}

[[nodiscard]] RuntimeContractOfferDigest computed_offer_digest(
    const RuntimeContractOfferDTO& offer) {
  return RuntimeContractOfferDigest(content_identity::framed_sha256(
      kRuntimeContractOfferIdentityDomain, kRuntimeContractOfferApiVersion,
      offer_content_value(offer)));
}

[[nodiscard]] RuntimeContractRequirementDigest computed_requirement_digest(
    const RuntimeContractRequirementDTO& requirement) {
  return RuntimeContractRequirementDigest(content_identity::framed_sha256(
      kRuntimeContractRequirementIdentityDomain,
      kRuntimeContractRequirementApiVersion,
      requirement_content_value(requirement)));
}

[[nodiscard]] RuntimeContractManifestSetDigest computed_manifest_set_digest(
    const RuntimeContractManifestSetDTO& manifest_set) {
  return RuntimeContractManifestSetDigest(content_identity::framed_sha256(
      kRuntimeContractManifestSetIdentityDomain,
      kRuntimeContractManifestSetApiVersion,
      manifest_set_content_value(manifest_set)));
}

RuntimeCapabilityContractRefDTO decode_capability_ref(
    const CanonicalValue& value) {
  exact(value, {"capability_definition_revision", "capability_id"});
  return {CapabilityId(text(member(value, "capability_id"))),
          CapabilityDefinitionRevision(
              text(member(value, "capability_definition_revision")))};
}

std::vector<RuntimeCapabilityContractRefDTO> decode_capability_refs(
    const CanonicalValue& value) {
  if (!value.is_array()) {
    fail("array_required");
  }
  if (value.as_array().size() > kMaxRuntimeContractCapabilityRefs) {
    fail("capability_reference_limit_exceeded");
  }
  std::vector<RuntimeCapabilityContractRefDTO> result;
  result.reserve(value.as_array().size());
  for (const auto& item : value.as_array()) {
    result.push_back(decode_capability_ref(item));
  }
  return result;
}

template <typename Id>
std::vector<Id> decode_api_contract_ids(const CanonicalValue& value) {
  if (!value.is_array()) {
    fail("array_required");
  }
  if (value.as_array().size() > kMaxRuntimeContractApiContractIds) {
    fail("api_contract_limit_exceeded");
  }
  std::vector<Id> result;
  result.reserve(value.as_array().size());
  for (const auto& item : value.as_array()) {
    result.emplace_back(text(item));
  }
  return result;
}

RuntimeContractOfferDTO decode_offer_value(const CanonicalValue& value) {
  exact(value, {"api_version", "offer_digest", "provided_api_contract_ids",
                "provided_capabilities", "runtime_implementation_id"});
  if (text(member(value, "api_version")) != kRuntimeContractOfferApiVersion) {
    fail("api_version_mismatch");
  }
  return {
      RuntimeContractOfferDigest(text(member(value, "offer_digest"))),
      RuntimeImplementationFamilyId(
          text(member(value, "runtime_implementation_id"))),
      decode_api_contract_ids<ApiContractId>(
          member(value, "provided_api_contract_ids")),
      decode_capability_refs(member(value, "provided_capabilities"))};
}

RuntimeContractRequirementDTO decode_requirement_value(
    const CanonicalValue& value) {
  exact(value, {"api_version", "required_api_contract_ids",
                "required_capabilities", "requirement_digest",
                "runtime_implementation_id"});
  if (text(member(value, "api_version")) !=
      kRuntimeContractRequirementApiVersion) {
    fail("api_version_mismatch");
  }
  return {
      RuntimeContractRequirementDigest(
          text(member(value, "requirement_digest"))),
      RuntimeImplementationFamilyId(
          text(member(value, "runtime_implementation_id"))),
      decode_api_contract_ids<ApiContractId>(
          member(value, "required_api_contract_ids")),
      decode_capability_refs(member(value, "required_capabilities"))};
}

RuntimeContractManifestSetDTO decode_manifest_set_value(
    const CanonicalValue& value) {
  exact(value, {"api_version", "manifest_set_digest", "offers"});
  if (text(member(value, "api_version")) !=
      kRuntimeContractManifestSetApiVersion) {
    fail("api_version_mismatch");
  }
  const auto& encoded_offers = member(value, "offers");
  if (!encoded_offers.is_array()) {
    fail("array_required");
  }
  if (encoded_offers.as_array().size() > kMaxRuntimeContractManifestOffers) {
    fail("manifest_offer_limit_exceeded");
  }
  std::vector<RuntimeContractOfferDTO> offers;
  offers.reserve(encoded_offers.as_array().size());
  for (const auto& item : encoded_offers.as_array()) {
    offers.push_back(decode_offer_value(item));
  }
  return {RuntimeContractManifestSetDigest(
              text(member(value, "manifest_set_digest"))),
          std::move(offers)};
}

template <typename Value, typename Decoder>
Result<Value> decode(std::string_view bytes, Decoder&& decoder,
                     std::string code) {
  try {
    const auto value = content_identity::parse_canonical_json(bytes);
    return Result<Value>::Ok(decoder(value));
  } catch (const std::exception& exception) {
    return reject<Value>(std::move(code),
                         "runtime contract manifest is not exact and closed",
                         exception.what());
  }
}

template <typename Value>
Result<Value> canonical_failure(const char* code, const char* reason) {
  return reject<Value>(code, "runtime contract manifest is not canonical",
                       reason);
}

}  // namespace

Result<RuntimeContractOfferDTO> MakeRuntimeContractOffer(
    RuntimeImplementationFamilyId runtime_implementation_id,
    std::vector<ApiContractId> provided_api_contract_ids,
    std::vector<RuntimeCapabilityContractRefDTO> provided_capabilities) {
  std::sort(provided_api_contract_ids.begin(), provided_api_contract_ids.end());
  std::sort(provided_capabilities.begin(), provided_capabilities.end(),
            capability_ref_less);
  RuntimeContractOfferDTO offer{
      RuntimeContractOfferDigest("pending"),
      std::move(runtime_implementation_id),
      std::move(provided_api_contract_ids),
      std::move(provided_capabilities)};
  if (!offer_fields_valid(offer, false)) {
    return reject<RuntimeContractOfferDTO>(
        "runtime_contract_offer_invalid",
        "runtime contract offer fields are not valid",
        "offer_not_locally_valid");
  }
  try {
    offer.offer_digest = computed_offer_digest(offer);
  } catch (const std::exception&) {
    return canonical_failure<RuntimeContractOfferDTO>(
        "runtime_contract_offer_not_canonical", "offer_content_not_canonical");
  }
  return Result<RuntimeContractOfferDTO>::Ok(std::move(offer));
}

Result<void> VerifyRuntimeContractOffer(
    const RuntimeContractOfferDTO& offer) {
  if (!offer_fields_valid(offer, true)) {
    return Result<void>::Err(
        ErrorDTO{"runtime_contract_offer_invalid",
                 "runtime contract offer fields are not valid",
                 {{"reason", "offer_not_locally_valid"}}});
  }
  try {
    if (computed_offer_digest(offer) != offer.offer_digest) {
      return Result<void>::Err(
          ErrorDTO{"runtime_contract_offer_digest_mismatch",
                   "runtime contract offer digest does not match its content",
                   {{"reason", "offer_digest_mismatch"}}});
    }
  } catch (const std::exception&) {
    return Result<void>::Err(
        ErrorDTO{"runtime_contract_offer_not_canonical",
                 "runtime contract offer cannot be represented canonically",
                 {{"reason", "offer_content_not_canonical"}}});
  }
  return Result<void>::Ok();
}

Result<std::string> EncodeRuntimeContractOffer(
    const RuntimeContractOfferDTO& offer) {
  const auto verified = VerifyRuntimeContractOffer(offer);
  if (!verified.ok()) {
    return Result<std::string>::Err(verified.error());
  }
  try {
    return Result<std::string>::Ok(
        content_identity::canonical_json(offer_value(offer)));
  } catch (const std::exception&) {
    return canonical_failure<std::string>("runtime_contract_offer_not_canonical",
                                           "offer_encoding_failed");
  }
}

Result<RuntimeContractOfferDTO> DecodeRuntimeContractOffer(
    std::string_view canonical_bytes) {
  auto result = decode<RuntimeContractOfferDTO>(
      canonical_bytes, decode_offer_value, "runtime_contract_offer_decode_failed");
  if (!result.ok()) {
    return result;
  }
  const auto verified = VerifyRuntimeContractOffer(result.value());
  if (!verified.ok()) {
    return Result<RuntimeContractOfferDTO>::Err(verified.error());
  }
  return result;
}

Result<RuntimeContractRequirementDTO> MakeRuntimeContractRequirement(
    RuntimeImplementationFamilyId runtime_implementation_id,
    std::vector<ApiContractId> required_api_contract_ids,
    std::vector<RuntimeCapabilityContractRefDTO> required_capabilities) {
  std::sort(required_api_contract_ids.begin(), required_api_contract_ids.end());
  std::sort(required_capabilities.begin(), required_capabilities.end(),
            capability_ref_less);
  RuntimeContractRequirementDTO requirement{
      RuntimeContractRequirementDigest("pending"),
      std::move(runtime_implementation_id),
      std::move(required_api_contract_ids),
      std::move(required_capabilities)};
  if (!requirement_fields_valid(requirement, false)) {
    return reject<RuntimeContractRequirementDTO>(
        "runtime_contract_requirement_invalid",
        "runtime contract requirement fields are not valid",
        "requirement_not_locally_valid");
  }
  try {
    requirement.requirement_digest = computed_requirement_digest(requirement);
  } catch (const std::exception&) {
    return canonical_failure<RuntimeContractRequirementDTO>(
        "runtime_contract_requirement_not_canonical",
        "requirement_content_not_canonical");
  }
  return Result<RuntimeContractRequirementDTO>::Ok(std::move(requirement));
}

Result<void> VerifyRuntimeContractRequirement(
    const RuntimeContractRequirementDTO& requirement) {
  if (!requirement_fields_valid(requirement, true)) {
    return Result<void>::Err(
        ErrorDTO{"runtime_contract_requirement_invalid",
                 "runtime contract requirement fields are not valid",
                 {{"reason", "requirement_not_locally_valid"}}});
  }
  try {
    if (computed_requirement_digest(requirement) !=
        requirement.requirement_digest) {
      return Result<void>::Err(
          ErrorDTO{
              "runtime_contract_requirement_digest_mismatch",
              "runtime contract requirement digest does not match its content",
              {{"reason", "requirement_digest_mismatch"}}});
    }
  } catch (const std::exception&) {
    return Result<void>::Err(
        ErrorDTO{"runtime_contract_requirement_not_canonical",
                 "runtime contract requirement cannot be represented canonically",
                 {{"reason", "requirement_content_not_canonical"}}});
  }
  return Result<void>::Ok();
}

Result<std::string> EncodeRuntimeContractRequirement(
    const RuntimeContractRequirementDTO& requirement) {
  const auto verified = VerifyRuntimeContractRequirement(requirement);
  if (!verified.ok()) {
    return Result<std::string>::Err(verified.error());
  }
  try {
    return Result<std::string>::Ok(
        content_identity::canonical_json(requirement_value(requirement)));
  } catch (const std::exception&) {
    return canonical_failure<std::string>(
        "runtime_contract_requirement_not_canonical",
        "requirement_encoding_failed");
  }
}

Result<RuntimeContractRequirementDTO> DecodeRuntimeContractRequirement(
    std::string_view canonical_bytes) {
  auto result = decode<RuntimeContractRequirementDTO>(
      canonical_bytes, decode_requirement_value,
      "runtime_contract_requirement_decode_failed");
  if (!result.ok()) {
    return result;
  }
  const auto verified = VerifyRuntimeContractRequirement(result.value());
  if (!verified.ok()) {
    return Result<RuntimeContractRequirementDTO>::Err(verified.error());
  }
  return result;
}

Result<RuntimeContractManifestSetDTO> MakeRuntimeContractManifestSet(
    std::vector<RuntimeContractOfferDTO> offers) {
  for (const auto& offer : offers) {
    const auto verified = VerifyRuntimeContractOffer(offer);
    if (!verified.ok()) {
      return Result<RuntimeContractManifestSetDTO>::Err(verified.error());
    }
  }
  std::sort(offers.begin(), offers.end(), [](const auto& left, const auto& right) {
    return left.runtime_implementation_id < right.runtime_implementation_id;
  });
  RuntimeContractManifestSetDTO manifest_set{
      RuntimeContractManifestSetDigest("pending"), std::move(offers)};
  if (manifest_set.offers.empty() ||
      manifest_set.offers.size() > kMaxRuntimeContractManifestOffers ||
      std::adjacent_find(
          manifest_set.offers.begin(), manifest_set.offers.end(),
          [](const auto& left, const auto& right) {
            return left.runtime_implementation_id ==
                   right.runtime_implementation_id;
          }) != manifest_set.offers.end()) {
    return reject<RuntimeContractManifestSetDTO>(
        "runtime_contract_manifest_set_invalid",
        "runtime contract manifest set offers are not valid",
        "manifest_set_offers_not_locally_valid");
  }
  try {
    manifest_set.manifest_set_digest = computed_manifest_set_digest(manifest_set);
  } catch (const std::exception&) {
    return canonical_failure<RuntimeContractManifestSetDTO>(
        "runtime_contract_manifest_set_not_canonical",
        "manifest_set_content_not_canonical");
  }
  return Result<RuntimeContractManifestSetDTO>::Ok(std::move(manifest_set));
}

Result<void> VerifyRuntimeContractManifestSet(
    const RuntimeContractManifestSetDTO& manifest_set) {
  if (!manifest_set_fields_valid(manifest_set)) {
    return Result<void>::Err(
        ErrorDTO{"runtime_contract_manifest_set_invalid",
                 "runtime contract manifest set fields are not valid",
                 {{"reason", "manifest_set_not_locally_valid"}}});
  }
  for (const auto& offer : manifest_set.offers) {
    const auto verified = VerifyRuntimeContractOffer(offer);
    if (!verified.ok()) {
      return Result<void>::Err(verified.error());
    }
  }
  try {
    if (computed_manifest_set_digest(manifest_set) !=
        manifest_set.manifest_set_digest) {
      return Result<void>::Err(
          ErrorDTO{
              "runtime_contract_manifest_set_digest_mismatch",
              "runtime contract manifest set digest does not match its content",
              {{"reason", "manifest_set_digest_mismatch"}}});
    }
  } catch (const std::exception&) {
    return Result<void>::Err(
        ErrorDTO{"runtime_contract_manifest_set_not_canonical",
                 "runtime contract manifest set cannot be represented canonically",
                 {{"reason", "manifest_set_content_not_canonical"}}});
  }
  return Result<void>::Ok();
}

Result<std::string> EncodeRuntimeContractManifestSet(
    const RuntimeContractManifestSetDTO& manifest_set) {
  const auto verified = VerifyRuntimeContractManifestSet(manifest_set);
  if (!verified.ok()) {
    return Result<std::string>::Err(verified.error());
  }
  try {
    return Result<std::string>::Ok(
        content_identity::canonical_json(manifest_set_value(manifest_set)));
  } catch (const std::exception&) {
    return canonical_failure<std::string>(
        "runtime_contract_manifest_set_not_canonical",
        "manifest_set_encoding_failed");
  }
}

Result<RuntimeContractManifestSetDTO> DecodeRuntimeContractManifestSet(
    std::string_view canonical_bytes) {
  auto result = decode<RuntimeContractManifestSetDTO>(
      canonical_bytes, decode_manifest_set_value,
      "runtime_contract_manifest_set_decode_failed");
  if (!result.ok()) {
    return result;
  }
  const auto verified = VerifyRuntimeContractManifestSet(result.value());
  if (!verified.ok()) {
    return Result<RuntimeContractManifestSetDTO>::Err(verified.error());
  }
  return result;
}

Result<void> CheckRuntimeContractCompatibility(
    const RuntimeContractOfferDTO& offer,
    const RuntimeContractRequirementDTO& requirement) {
  const auto offer_verified = VerifyRuntimeContractOffer(offer);
  if (!offer_verified.ok()) {
    return Result<void>::Err(offer_verified.error());
  }
  const auto requirement_verified = VerifyRuntimeContractRequirement(requirement);
  if (!requirement_verified.ok()) {
    return Result<void>::Err(requirement_verified.error());
  }
  for (const auto& required : requirement.required_api_contract_ids) {
    if (!std::binary_search(offer.provided_api_contract_ids.begin(),
                            offer.provided_api_contract_ids.end(), required)) {
      return Result<void>::Err(
          ErrorDTO{"runtime_contract_incompatible",
                   "runtime contract offer is missing a required API contract",
                   {{"reason", "required_api_contract_missing"},
                    {"api_contract_id", required.value()}}});
    }
  }
  for (const auto& required : requirement.required_capabilities) {
    const auto offered = std::lower_bound(
        offer.provided_capabilities.begin(), offer.provided_capabilities.end(),
        required.capability_id,
        [](const RuntimeCapabilityContractRefDTO& candidate,
           const CapabilityId& capability_id) {
          return candidate.capability_id < capability_id;
        });
    if (offered == offer.provided_capabilities.end() ||
        offered->capability_id != required.capability_id) {
      return Result<void>::Err(
          ErrorDTO{
              "runtime_contract_incompatible",
              "runtime contract offer is missing a required capability",
              {{"reason", "required_capability_missing"},
               {"capability_id", required.capability_id.value()}}});
    }
  }
  return Result<void>::Ok();
}

}  // namespace manyforge::core
