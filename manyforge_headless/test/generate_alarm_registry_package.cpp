#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "canonical_empty_admission_internal.hpp"
#include "lite6_retained_consumer_fixture.hpp"

namespace {

struct Arguments final {
  std::filesystem::path package_root;
  std::string cell_id;
  std::filesystem::path bundle_ids_output;
};

[[nodiscard]] Arguments ParseArguments(const int argc, char **argv) {
  if (argc != 7 || std::string_view(argv[1]) != "--package-root" ||
      std::string_view(argv[2]).empty() ||
      std::string_view(argv[3]) != "--cell-id" ||
      std::string_view(argv[4]).empty() ||
      std::string_view(argv[5]) != "--bundle-ids-output" ||
      std::string_view(argv[6]).empty()) {
    throw std::invalid_argument(
        "usage: generate_alarm_registry_package --package-root <path> "
        "--cell-id <id> --bundle-ids-output <path>");
  }
  return {argv[2], argv[4], argv[6]};
}

} // namespace

/// Successive active configurations of one cell for the alarm resource
/// registry: one Lite6, as its replacement two same-type Lite6 instances,
/// and the cell's canonical-empty configuration (no controlled components).
/// Their bundle IDs are written one per line in that order.
int main(int argc, char **argv) {
  try {
    const auto arguments = ParseArguments(argc, argv);
    std::filesystem::create_directories(arguments.package_root);
    // Generated first: it materializes into the still-empty root, exactly as
    // its own package generator does.
    const auto empty =
        manyforge::deployment::detail::generate_canonical_empty_package(
            arguments.package_root);
    if (empty.retained_bundle().value().cell_graph.cell_id.value() !=
        arguments.cell_id) {
      throw std::invalid_argument(
          "the canonical-empty configuration belongs to another cell");
    }
    const auto single = manyforge::deployment::test::
        materialize_lite6_native_motion_retained_consumer_fixture_for_cell(
            arguments.package_root, arguments.cell_id);
    const auto dual = manyforge::deployment::test::
        materialize_two_lite6_same_type_native_motion_retained_consumer_fixture_for_cell(
            arguments.package_root, arguments.cell_id);
    std::ofstream stream(arguments.bundle_ids_output,
                         std::ios::binary | std::ios::trunc);
    stream << single.bundle->retained_bundle_id().value() << '\n'
           << dual.bundle->retained_bundle_id().value() << '\n'
           << empty.retained_bundle_id().value() << '\n';
    stream.flush();
    if (!stream) {
      throw std::runtime_error("cannot write alarm registry bundle IDs");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
