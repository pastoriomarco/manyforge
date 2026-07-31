#include <filesystem>
#include <iostream>
#include <string_view>

#include "canonical_empty_admission_internal.hpp"

int main(int argc, char** argv) {
  if (argc != 3 ||
      std::string_view(argv[1]) != "--output-root" ||
      std::string_view(argv[2]).empty()) {
    std::cerr
        << "usage: manyforge_generate_canonical_empty_package "
           "--output-root PATH\n";
    return 2;
  }
  try {
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
