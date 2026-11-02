#pragma once

#include <fstream>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <memory>
#include <stdexcept>
#include <string>

namespace manyforge::headless::test {

[[nodiscard]] inline std::shared_ptr<const deployment::RetainedBundle>
LoadAssistantAuthoringBundle() {
  std::ifstream stream(MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE, std::ios::binary);
  std::string bundle_id;
  if (!stream || !std::getline(stream, bundle_id) || bundle_id.empty()) {
    throw std::logic_error("assistant fixture bundle ID is unavailable");
  }
  const deployment::FilesystemVerifiedRetainedBundleSource source(
      MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT);
  auto bundle = source.get_verified(core::RetainedBundleId(bundle_id));
  if (bundle == nullptr) {
    throw std::logic_error("assistant fixture retained bundle is unavailable");
  }
  return bundle;
}

}  // namespace manyforge::headless::test
