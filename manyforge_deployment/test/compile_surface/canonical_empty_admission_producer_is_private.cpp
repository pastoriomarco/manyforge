#include <manyforge_deployment/canonical_empty_admission.hpp>

#include <filesystem>

int main() {
  manyforge::deployment::generate_canonical_empty_package(
      std::filesystem::path{"caller-selected-root"});
  return 0;
}
