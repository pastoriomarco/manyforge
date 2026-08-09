#include <gtest/gtest.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>

#include "assistant_authoring_fixture.hpp"
#include "manyforge_headless/assistant_mode_projection.hpp"

namespace manyforge::headless {
namespace {

namespace deployment = manyforge::deployment;
namespace content_identity = manyforge::content_identity;

TEST(AssistantModeProjection, CanonicalEmptyBundleHasNoAssistantAuthority) {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  const auto admission = source.load_verified();

  try {
    (void)ProjectAssistantMode(admission.retained_bundle(), "composer-assistant");
    FAIL() << "canonical-empty bundle unexpectedly projected a mode";
  } catch (const AssistantModeProjectionError& error) {
    EXPECT_EQ(error.code(), "assistant_mode_projection_unavailable");
  }
}

TEST(AssistantModeProjection,
     RetainedComposerModePublishesExactDraftDescriptorsWithoutGrantingEffects) {
  const auto bundle = test::LoadAssistantAuthoringBundle();

  const auto projection = ProjectAssistantMode(*bundle, "composer-assistant");
  const auto bytes = content_identity::canonical_json(projection);

  EXPECT_NE(bytes.find("\"schema_version\":\"manyforge.assistant_mode_projection.v1\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"mode\":\"composer-assistant\""), std::string::npos);
  EXPECT_NE(bytes.find("\"tool_id\":\"program_read\""), std::string::npos);
  EXPECT_NE(bytes.find("\"tool_id\":\"scene_draft_add_object\""), std::string::npos);
  EXPECT_NE(bytes.find("\"effect\":\"composer_draft_mutating\""), std::string::npos);
  EXPECT_NE(bytes.find("\"input_schema_hash\":\"sha256:"), std::string::npos);
  EXPECT_EQ(bytes.find("runtime_mutating"), std::string::npos);
  EXPECT_EQ(bytes.find("availability_granted"), std::string::npos);
}

TEST(AssistantModeProjection, UnknownModeIsRejectedBeforePublication) {
  const auto bundle = test::LoadAssistantAuthoringBundle();

  try {
    (void)ProjectAssistantMode(*bundle, "unknown");
    FAIL() << "unknown assistant mode unexpectedly projected";
  } catch (const AssistantModeProjectionError& error) {
    EXPECT_EQ(error.code(), "assistant_mode_not_found");
  }
}

}  // namespace
}  // namespace manyforge::headless
