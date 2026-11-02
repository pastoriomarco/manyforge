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

TEST(AssistantModeProjection, AssistantWithoutAiModelComponentIsUnavailable) {
  // The fixture retains assistant settings but no AI model component: no mode
  // can name a model endpoint.
  const auto bundle = test::LoadAssistantAuthoringBundle();

  try {
    (void)ProjectAssistantMode(*bundle, "composer-assistant");
    FAIL() << "assistant mode without an AI model component unexpectedly projected";
  } catch (const AssistantModeProjectionError& error) {
    EXPECT_EQ(error.code(), "assistant_ai_model_absent");
  }
}

TEST(AssistantModeProjection, IndexPublishesEnabledModesAndTheAbsentAiModel) {
  const auto bundle = test::LoadAssistantAuthoringBundle();

  const auto index = ProjectAssistantModeIndex(*bundle);
  const auto bytes = content_identity::canonical_json(index);

  EXPECT_NE(bytes.find("\"schema_version\":\"manyforge.assistant_mode_index.v1\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"mode\":\"composer-assistant\""), std::string::npos);
  EXPECT_NE(bytes.find("\"ai_model_component_id\":null"), std::string::npos);
  EXPECT_EQ(bytes.find("tool_id"), std::string::npos);
  EXPECT_EQ(bytes.find("provider_alias"), std::string::npos);
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
