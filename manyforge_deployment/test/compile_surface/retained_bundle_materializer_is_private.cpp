#include <manyforge_deployment/retained_bundle.hpp>

int main() {
  const auto producer =
      &manyforge::deployment::detail::RetainedBundleMaterializer::materialize;
  (void)producer;
  return 0;
}
