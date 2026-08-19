#include <gtest/gtest.h>

#include <algorithm>
#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

constexpr std::string_view kFamily = "native-family";

[[nodiscard]] RuntimeCapabilityContractRefDTO Capability(
    std::string id = "planning.motion.joint_goal") {
  const auto* definition = find_builtin_capability_definition(id);
  EXPECT_NE(definition, nullptr);
  return {CapabilityId(std::move(id)),
          CapabilityDefinitionRevision(
              std::string(definition->capability_definition_revision))};
}

[[nodiscard]] RuntimeContractOfferDTO Offer() {
  const auto result = MakeRuntimeContractOffer(
      RuntimeImplementationFamilyId(std::string(kFamily)),
      {ApiContractId("api.motion"), ApiContractId("api.state")},
      {Capability("planning.motion.joint_goal"),
       Capability("state.observed.current")});
  EXPECT_TRUE(result.ok());
  return result.value();
}

[[nodiscard]] RuntimeContractRequirementDTO Requirement() {
  const auto result = MakeRuntimeContractRequirement(
      RuntimeImplementationFamilyId(std::string(kFamily)),
      {ApiContractId("api.motion")},
      {Capability("planning.motion.joint_goal")});
  EXPECT_TRUE(result.ok());
  return result.value();
}

TEST(RuntimeContractManifestCodec, UsesFixedVersionsAndDeterministicDigestVector) {
  const auto offer = Offer();
  const auto requirement = Requirement();
  const auto manifest_set_result = MakeRuntimeContractManifestSet({offer});
  ASSERT_TRUE(manifest_set_result.ok());
  ASSERT_TRUE(VerifyRuntimeContractOffer(offer).ok());
  ASSERT_TRUE(VerifyRuntimeContractRequirement(requirement).ok());
  ASSERT_TRUE(VerifyRuntimeContractManifestSet(manifest_set_result.value()).ok());
  EXPECT_EQ(offer.offer_digest.value(),
            "sha256:63a4994aebb34bfa8488c9eb0ff24a642e28105be1e28024042eec71e2504c9b");
  EXPECT_EQ(requirement.requirement_digest.value(),
            "sha256:23f6b72cdf22ecbe419dca3c1a40a2b1e2059a5ab5fdacde3945cb0cb7c7f623");
  EXPECT_EQ(manifest_set_result.value().manifest_set_digest.value(),
            "sha256:04824092c00fb2f0e3a3009919f3c6b4a8a337c8e79fda97593e29a09cb3af46");
  EXPECT_EQ(kRuntimeContractOfferApiVersion,
            "manyforge.runtime_contract_offer.v1");
  EXPECT_EQ(kRuntimeContractRequirementApiVersion,
            "manyforge.runtime_contract_requirement.v1");
  EXPECT_EQ(kRuntimeContractManifestSetApiVersion,
            "manyforge.runtime_contract_manifest_set.v1");
}

TEST(RuntimeContractManifestCodec, SortsAtCanonicalConstructionAndRoundTrips) {
  const auto result = MakeRuntimeContractOffer(
      RuntimeImplementationFamilyId("native-family"),
      {ApiContractId("api.state"), ApiContractId("api.motion")},
      {Capability("state.observed.current"),
       Capability("planning.motion.joint_goal")});
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.value().provided_api_contract_ids.front().value(),
            "api.motion");
  ASSERT_EQ(result.value().provided_capabilities.front().capability_id.value(),
            "planning.motion.joint_goal");

  const auto encoded = EncodeRuntimeContractOffer(result.value());
  ASSERT_TRUE(encoded.ok());
  const auto decoded = DecodeRuntimeContractOffer(encoded.value());
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value(), result.value());
}

TEST(RuntimeContractManifestCodec, RequiresExactCapabilityRegistryRevision) {
  auto offer = Offer();
  offer.provided_capabilities.front().capability_definition_revision =
      CapabilityDefinitionRevision("sha256:" + std::string(64U, '0'));
  EXPECT_FALSE(VerifyRuntimeContractOffer(offer).ok());

  auto unknown = Offer();
  unknown.provided_capabilities.front().capability_id =
      CapabilityId("capability.unknown");
  EXPECT_FALSE(VerifyRuntimeContractOffer(unknown).ok());
}

TEST(RuntimeContractManifestCodec, RejectsMalformedIdsDigestsAndClosedFields) {
  EXPECT_FALSE(MakeRuntimeContractOffer(
                   RuntimeImplementationFamilyId("not/portable"), {}, {})
                   .ok());
  EXPECT_FALSE(MakeRuntimeContractRequirement(
                   RuntimeImplementationFamilyId("family"), {ApiContractId("api/invalid")},
                   {})
                   .ok());

  const auto offer = Offer();
  auto bad_digest = offer;
  bad_digest.offer_digest = RuntimeContractOfferDigest("sha256:" + std::string(63U, '0'));
  EXPECT_FALSE(VerifyRuntimeContractOffer(bad_digest).ok());

  const std::string unknown_field =
      "{\"api_version\":\"manyforge.runtime_contract_offer.v1\","
      "\"offer_digest\":\"sha256:" +
      std::string(64U, '0') +
      "\",\"provided_api_contract_ids\":[],\"provided_capabilities\":[],"
      "\"runtime_implementation_id\":\"native-family\",\"unknown\":true}";
  EXPECT_FALSE(DecodeRuntimeContractOffer(unknown_field).ok());

  const std::string wrong_version =
      "{\"api_version\":\"manyforge.runtime_contract_offer.v2\","
      "\"offer_digest\":\"sha256:" +
      std::string(64U, '0') +
      "\",\"provided_api_contract_ids\":[],\"provided_capabilities\":[],"
      "\"runtime_implementation_id\":\"native-family\"}";
  EXPECT_FALSE(DecodeRuntimeContractOffer(wrong_version).ok());
}

TEST(RuntimeContractManifestCodec,
     RejectsUnknownFieldsForRequirementAndManifestSet) {
  const auto requirement_encoding =
      EncodeRuntimeContractRequirement(Requirement());
  ASSERT_TRUE(requirement_encoding.ok());
  auto requirement_with_unknown_field = requirement_encoding.value();
  requirement_with_unknown_field.insert(
      requirement_with_unknown_field.size() - 1U, ",\"unknown\":true");
  EXPECT_FALSE(
      DecodeRuntimeContractRequirement(requirement_with_unknown_field).ok());

  const auto manifest_set_result = MakeRuntimeContractManifestSet({Offer()});
  ASSERT_TRUE(manifest_set_result.ok());
  const auto manifest_set_encoding =
      EncodeRuntimeContractManifestSet(manifest_set_result.value());
  ASSERT_TRUE(manifest_set_encoding.ok());
  auto manifest_set_with_unknown_field = manifest_set_encoding.value();
  manifest_set_with_unknown_field.insert(
      manifest_set_with_unknown_field.size() - 1U, ",\"unknown\":true");
  EXPECT_FALSE(DecodeRuntimeContractManifestSet(
                   manifest_set_with_unknown_field)
                   .ok());
}

TEST(RuntimeContractManifestCodec, RejectsDuplicateUnsortedAndOversizedRows) {
  auto duplicate = Offer();
  duplicate.provided_api_contract_ids = {ApiContractId("api.motion"),
                                         ApiContractId("api.motion")};
  EXPECT_FALSE(VerifyRuntimeContractOffer(duplicate).ok());
  duplicate.provided_api_contract_ids = {ApiContractId("api.state"),
                                         ApiContractId("api.motion")};
  EXPECT_FALSE(VerifyRuntimeContractOffer(duplicate).ok());

  std::vector<ApiContractId> too_many_apis;
  for (std::size_t index = 0U; index <= kMaxRuntimeContractApiContractIds; ++index) {
    too_many_apis.emplace_back("api." + std::to_string(index));
  }
  EXPECT_FALSE(MakeRuntimeContractOffer(RuntimeImplementationFamilyId("family"),
                                        std::move(too_many_apis), {})
                   .ok());

  std::vector<RuntimeCapabilityContractRefDTO> too_many_capabilities;
  for (std::size_t index = 0U; index <= kMaxRuntimeContractCapabilityRefs; ++index) {
    too_many_capabilities.push_back(Capability());
    too_many_capabilities.back().capability_id =
        CapabilityId("planning.motion.joint_goal");
  }
  EXPECT_FALSE(MakeRuntimeContractOffer(RuntimeImplementationFamilyId("family"), {},
                                        std::move(too_many_capabilities))
                   .ok());
}

TEST(RuntimeContractManifestCodec, CompatibilityAcceptsExactAndStrictSuperset) {
  const auto offer = Offer();
  const auto requirement = Requirement();
  EXPECT_TRUE(CheckRuntimeContractCompatibility(offer, requirement).ok());

  auto wrong_family = requirement;
  wrong_family.runtime_implementation_id =
      RuntimeImplementationFamilyId("other-family");
  EXPECT_FALSE(CheckRuntimeContractCompatibility(offer, wrong_family).ok());

  auto missing_api = requirement;
  missing_api.required_api_contract_ids = {ApiContractId("api.missing")};
  const auto rebuilt_missing_api = MakeRuntimeContractRequirement(
      missing_api.runtime_implementation_id,
      missing_api.required_api_contract_ids, missing_api.required_capabilities);
  ASSERT_TRUE(rebuilt_missing_api.ok());
  EXPECT_FALSE(CheckRuntimeContractCompatibility(offer, rebuilt_missing_api.value()).ok());

  auto missing_capability = requirement;
  missing_capability.required_capabilities = {
      Capability("planning.motion.pose_goal")};
  const auto rebuilt_missing_capability = MakeRuntimeContractRequirement(
      missing_capability.runtime_implementation_id,
      missing_capability.required_api_contract_ids,
      missing_capability.required_capabilities);
  ASSERT_TRUE(rebuilt_missing_capability.ok());
  EXPECT_FALSE(
      CheckRuntimeContractCompatibility(offer, rebuilt_missing_capability.value()).ok());
}

TEST(RuntimeContractManifestCodec, ManifestSetIsBoundedSortedAndNested) {
  const auto first = Offer();
  auto second_result = MakeRuntimeContractOffer(
      RuntimeImplementationFamilyId("other-family"), {ApiContractId("api.other")},
      {Capability("scene.object.query")});
  ASSERT_TRUE(second_result.ok());
  const auto second = second_result.value();

  const auto set_result = MakeRuntimeContractManifestSet({second, first});
  ASSERT_TRUE(set_result.ok());
  ASSERT_EQ(set_result.value().offers.front().runtime_implementation_id.value(),
            "native-family");
  const auto encoded = EncodeRuntimeContractManifestSet(set_result.value());
  ASSERT_TRUE(encoded.ok());
  const auto decoded = DecodeRuntimeContractManifestSet(encoded.value());
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value(), set_result.value());

  auto tampered = set_result.value();
  tampered.offers.front().offer_digest =
      RuntimeContractOfferDigest("sha256:" + std::string(64U, 'f'));
  EXPECT_FALSE(VerifyRuntimeContractManifestSet(tampered).ok());

  EXPECT_FALSE(MakeRuntimeContractManifestSet({}).ok());
  EXPECT_FALSE(MakeRuntimeContractManifestSet({first, first}).ok());
  EXPECT_FALSE(VerifyRuntimeContractManifestSet(
                   RuntimeContractManifestSetDTO{
                       RuntimeContractManifestSetDigest(
                           "sha256:" + std::string(64U, '0')),
                       {second, first}})
                   .ok());

  std::vector<RuntimeContractOfferDTO> too_many_offers;
  for (std::size_t index = 0U; index <= kMaxRuntimeContractManifestOffers; ++index) {
    const auto generated = MakeRuntimeContractOffer(
        RuntimeImplementationFamilyId("family-" + std::to_string(index)), {}, {});
    ASSERT_TRUE(generated.ok());
    too_many_offers.push_back(generated.value());
  }
  EXPECT_FALSE(MakeRuntimeContractManifestSet(std::move(too_many_offers)).ok());
}

}  // namespace
}  // namespace manyforge::core
