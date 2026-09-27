#include <filesystem>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "canonical_empty_admission_internal.hpp"

int main(int argc, char** argv) {
  if ((argc != 3 && argc != 7) ||
      std::string_view(argv[1]) != "--output-root" ||
      std::string_view(argv[2]).empty() ||
      (argc == 7 && (std::string_view(argv[3]) != "--cell-id" ||
                     std::string_view(argv[5]) != "--native-runtime-profile"))) {
    std::cerr
        << "usage: manyforge_generate_canonical_empty_package "
           "--output-root PATH [--cell-id ID --native-runtime-profile PATH]\n";
    return 2;
  }
  try {
    if (argc == 7) {
      const std::filesystem::path profile_path(argv[6]);
      if (std::filesystem::is_symlink(profile_path) ||
          !std::filesystem::is_regular_file(profile_path) ||
          std::filesystem::file_size(profile_path) > 1024U * 1024U) {
        throw std::invalid_argument("native runtime profile must be a bounded plain file");
      }
      std::ifstream input(profile_path, std::ios::binary);
      if (!input) throw std::runtime_error("cannot read native runtime profile");
      std::string bytes(1024U * 1024U + 1U, '\0');
      input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      bytes.resize(static_cast<std::size_t>(input.gcount()));
      if (input.bad() || bytes.empty() || bytes.size() > 1024U * 1024U) {
        throw std::invalid_argument("native runtime profile read is empty, failed or oversized");
      }
      const auto admission = manyforge::deployment::detail::generate_native_empty_package(
          std::filesystem::path(argv[2]), manyforge::core::CellId(argv[4]), bytes);
      std::cout << admission.canonical_bytes() << '\n';
      return 0;
    }
    const auto admission =
        manyforge::deployment::detail::
            generate_canonical_empty_package(
                std::filesystem::path(argv[2]));
    std::cout << admission.canonical_bytes() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "canonical-empty package generation failed: "
              << error.what() << '\n';
    return 1;
  }
}
