#include <gtest/gtest.h>

#include <manyforge_core/types/typed_error.hpp>

#include <stdexcept>

namespace manyforge::core {
namespace {

TEST(TypedErrorTest, CarriesTheErrorDtoAndProducer) {
  const TypedError error("apply_admission_failed", "Kernel refused",
                         {{"reason", "expected_generation_stale"}}, "kernel");
  EXPECT_STREQ(error.what(), "Kernel refused");
  EXPECT_EQ(error.code(), "apply_admission_failed");
  EXPECT_EQ(error.producer(), "kernel");
  EXPECT_EQ(error.error(),
            (ErrorDTO{"apply_admission_failed", "Kernel refused",
                      {{"reason", "expected_generation_stale"}}}));
  const TypedError copied(error.error());
  EXPECT_EQ(copied.error(), error.error());
  EXPECT_TRUE(copied.producer().empty());
}

TEST(TypedErrorTest, CauseKeepsTheTypedCodeThroughAStdExceptionCatch) {
  try {
    throw TypedError("stale_generation", "claim moved",
                     {{"reason", "expected_generation_stale"}}, "kernel");
  } catch (const std::exception &error) {
    const auto cause = CauseOf(error, "apply_admission_failed", "component_manager");
    EXPECT_EQ(cause.error.code, "stale_generation");
    EXPECT_EQ(cause.error.message, "claim moved");
    EXPECT_EQ(cause.error.details.at("reason"), "expected_generation_stale");
    EXPECT_EQ(cause.producer, "kernel");
  }
}

TEST(TypedErrorTest, TypedCauseWithoutProducerIsTheBoundary) {
  const TypedError error("actual_installed_state_unknown", "inventory is foreign");
  const auto cause = CauseOf(error, "apply_fence_evidence_failed", "component_manager");
  EXPECT_EQ(cause.error.code, "actual_installed_state_unknown");
  EXPECT_EQ(cause.producer, "component_manager");
}

TEST(TypedErrorTest, UntypedExceptionFallsBackToTheBoundaryCode) {
  const std::runtime_error error("disk is fine, text only");
  const auto cause = CauseOf(error, "configuration_publication_failed", "component_manager");
  EXPECT_EQ(cause.error,
            (ErrorDTO{"configuration_publication_failed", "disk is fine, text only", {}}));
  EXPECT_EQ(cause.producer, "component_manager");
}

TEST(TypedErrorTest, RequestInvalidNamesTheFieldPathAndItsCode) {
  const auto error = RequestInvalid("request_id", "missing", "command is missing field request_id");
  EXPECT_EQ(error.code(), "invalid_request");
  EXPECT_EQ(error.details(), (std::map<std::string, std::string>{{"request_id", "missing"}}));
  EXPECT_STREQ(error.what(), "command is missing field request_id");
}

}  // namespace
}  // namespace manyforge::core
