#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "lite6_retained_consumer_fixture.hpp"

namespace {

struct Arguments final {
  std::filesystem::path package_root;
  std::filesystem::path bundle_id_output;
};

[[nodiscard]] Arguments ParseArguments(const int argc, char** argv) {
  if (argc != 5 || std::string_view(argv[1]) != "--package-root" ||
      std::string_view(argv[2]).empty() || std::string_view(argv[3]) != "--bundle-id-output" ||
      std::string_view(argv[4]).empty()) {
    throw std::invalid_argument(
        "usage: generate_assistant_authoring_package --package-root <path> "
        "--bundle-id-output <path>");
  }
  return {std::filesystem::path(argv[2]), std::filesystem::path(argv[4])};
}

void WriteBundleId(const std::filesystem::path& path, const std::string_view bundle_id) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot create assistant fixture bundle-ID file");
  }
  stream << bundle_id << '\n';
  stream.flush();
  if (!stream) {
    throw std::runtime_error("cannot write assistant fixture bundle-ID file");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto arguments = ParseArguments(argc, argv);
    std::filesystem::create_directories(arguments.package_root);
    const auto fixture =
        manyforge::deployment::test::materialize_lite6_composer_assistant_retained_consumer_fixture(
            arguments.package_root);
    WriteBundleId(arguments.bundle_id_output, fixture.bundle->retained_bundle_id().value());
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
