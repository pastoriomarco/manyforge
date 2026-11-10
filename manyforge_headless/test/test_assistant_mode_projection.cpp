#include <gtest/gtest.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>

#include "assistant_authoring_fixture.hpp"
#include "manyforge_headless/assistant_mode_projection.hpp"

namespace manyforge::headless {
namespace {

namespace deployment = manyforge::deployment;
namespace content_identity = manyforge::content_identity;

TEST(AssistantModeProjection, CanonicalEmptyBundleHasNoAssistantComponent) {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  const auto admission = source.load_verified();

  try {
    (void)ProjectAssistantMode(admission.retained_bundle(), "composer-assistant");
    FAIL() << "canonical-empty bundle unexpectedly projected a mode";
  } catch (const AssistantModeProjectionError& error) {
    EXPECT_EQ(error.code(), "assistant_component_absent");
  }
  // Its index is empty.
  const auto bytes =
      content_identity::canonical_json(ProjectAssistantModeIndex(admission.retained_bundle()));
  EXPECT_NE(bytes.find("\"modes\":[]"), std::string::npos);
  EXPECT_NE(bytes.find("\"assistant_component_id\":null"), std::string::npos);
  EXPECT_NE(bytes.find("\"assistant_projection_digest\":null"), std::string::npos);
}

TEST(AssistantModeProjection, ModesWithoutAnAssistantComponentAreAbsent) {
  // The fixture retains assistant modes but no assistant component (no
  // `assistant.bridge.request` provider): no mode is read, also an unknown one.
  const auto bundle = test::LoadAssistantAuthoringBundle();

  for (const auto* mode : {"composer-assistant", "unknown"}) {
    try {
      (void)ProjectAssistantMode(*bundle, mode);
      FAIL() << "assistant mode without an assistant component unexpectedly projected";
    } catch (const AssistantModeProjectionError& error) {
      EXPECT_EQ(error.code(), "assistant_component_absent");
    }
  }
}

TEST(AssistantModeProjection, IndexWithoutAnAssistantComponentHasNoMode) {
  const auto bundle = test::LoadAssistantAuthoringBundle();

  const auto index = ProjectAssistantModeIndex(*bundle);
  const auto bytes = content_identity::canonical_json(index);

  EXPECT_NE(bytes.find("\"schema_version\":\"manyforge.assistant_mode_index.v2\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"modes\":[]"), std::string::npos);
  EXPECT_NE(bytes.find("\"assistant_component_id\":null"), std::string::npos);
  EXPECT_NE(bytes.find("\"ai_model_component_id\":null"), std::string::npos);
  EXPECT_EQ(bytes.find("tool_id"), std::string::npos);
  EXPECT_EQ(bytes.find("provider"), std::string::npos);
}

}  // namespace
}  // namespace manyforge::headless
