#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "build_manifest_internal.hpp"
#include "manyforge_deployment/build_manifest.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace {

using manyforge::deployment::BuildManifestKind;
using manyforge::deployment::BuildQualifiedManifest;
using manyforge::deployment::detail::BuildQualifiedRetentionAuthority;
using manyforge::deployment::detail::BuildQualifiedRetentionContext;

constexpr std::array kManifestKinds{
    BuildManifestKind::kNodeEnvironment,
    BuildManifestKind::kCompilerContract,
    BuildManifestKind::kCompilerBuild,
};

[[nodiscard]] std::string_view manifest_symbol(
    BuildManifestKind kind) {
  switch (kind) {
    case BuildManifestKind::kNodeEnvironment:
      return "NodeEnvironment";
    case BuildManifestKind::kCompilerContract:
      return "CompilerContract";
    case BuildManifestKind::kCompilerBuild:
      return "CompilerBuild";
  }
  throw std::logic_error("unknown build manifest kind");
}

[[nodiscard]] std::string cpp_string_literal(std::string_view value) {
  std::ostringstream output;
  output << '"';
  for (const unsigned char byte : value) {
    switch (byte) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (byte >= 0x20U && byte <= 0x7eU) {
          output << static_cast<char>(byte);
        } else {
          output << '\\' << std::oct << std::setw(3)
                 << std::setfill('0')
                 << static_cast<unsigned int>(byte) << std::dec;
        }
        break;
    }
  }
  output << '"';
  return output.str();
}

void emit_byte_array(std::ostream& output, std::string_view symbol,
                     std::string_view bytes) {
  output << "inline constexpr std::array<std::uint8_t, "
         << bytes.size() << "U> k" << symbol
         << "ManifestBytes{{\n";
  constexpr std::size_t kBytesPerLine = 12U;
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    if (index % kBytesPerLine == 0U) {
      output << "    ";
    }
    output << "0x" << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<unsigned int>(
                  static_cast<unsigned char>(bytes[index]))
           << "U" << std::dec;
    if (index + 1U != bytes.size()) {
      output << ", ";
    }
    if ((index + 1U) % kBytesPerLine == 0U ||
        index + 1U == bytes.size()) {
      output << '\n';
    }
  }
  output << "}};\n\n";
}

[[nodiscard]] std::array<
    const BuildQualifiedManifest*, kManifestKinds.size()>
index_manifests(const BuildQualifiedRetentionContext& context) {
  std::array<const BuildQualifiedManifest*, kManifestKinds.size()> indexed{
      nullptr, nullptr, nullptr};
  for (const auto& manifest : context.manifests()) {
    if (manifest == nullptr) {
      throw std::logic_error(
          "build authority returned a null manifest");
    }
    const std::size_t index =
        static_cast<std::size_t>(manifest->key().manifest_kind);
    if (index >= indexed.size() || indexed[index] != nullptr) {
      throw std::logic_error(
          "build authority returned a duplicate or unknown manifest kind");
    }
    indexed[index] = manifest.get();
  }
  for (const BuildQualifiedManifest* manifest : indexed) {
    if (manifest == nullptr) {
      throw std::logic_error(
          "build authority omitted a required manifest kind");
    }
  }
  return indexed;
}

[[nodiscard]] std::string emit_profile() {
  const BuildQualifiedRetentionContext context =
      BuildQualifiedRetentionAuthority::qualify();
  const auto manifests = index_manifests(context);

  for (const BuildQualifiedManifest* manifest : manifests) {
    if (!manyforge::deployment::is_sha256_digest(
            manifest->key().content_digest.value()) ||
        manifest->canonical_bytes().empty()) {
      throw std::logic_error(
          "build authority returned an invalid manifest profile");
    }
  }

  const auto& node = context.node_environment();
  if (node.catalog_id.empty() || node.content_version.empty() ||
      !manyforge::deployment::is_sha256_digest(
          node.catalog_digest.value()) ||
      !manyforge::deployment::is_sha256_digest(
          node.environment_digest.value()) ||
      context.compiler_contract_digest() !=
          manifests[static_cast<std::size_t>(
              BuildManifestKind::kCompilerContract)]
              ->key()
              .content_digest ||
      context.compiler_build_digest() !=
          manifests[static_cast<std::size_t>(
              BuildManifestKind::kCompilerBuild)]
              ->key()
              .content_digest) {
    throw std::logic_error(
        "build authority returned inconsistent retained profile fields");
  }

  std::ostringstream output;
  output
      << "// Generated by manyforge_emit_build_qualified_admission_profile.\n"
      << "// Do not edit: every value is owned by the exact reader build.\n"
      << "#pragma once\n\n"
      << "#include <array>\n"
      << "#include <cstdint>\n"
      << "#include <string_view>\n\n"
      << "namespace manyforge::deployment::detail::"
         "generated_build_qualified_admission_profile {\n\n";

  for (const BuildManifestKind kind : kManifestKinds) {
    const BuildQualifiedManifest& manifest =
        *manifests[static_cast<std::size_t>(kind)];
    const std::string_view symbol = manifest_symbol(kind);
    emit_byte_array(output, symbol, manifest.canonical_bytes());
    output << "inline constexpr std::string_view k" << symbol
           << "ManifestDigest = "
           << cpp_string_literal(
                  manifest.key().content_digest.value())
           << ";\n\n";
  }

  output
      << "inline constexpr std::string_view kRetainedCatalogId = "
      << cpp_string_literal(node.catalog_id) << ";\n"
      << "inline constexpr std::string_view "
         "kRetainedCatalogContentVersion = "
      << cpp_string_literal(node.content_version) << ";\n"
      << "inline constexpr std::string_view kRetainedCatalogDigest = "
      << cpp_string_literal(node.catalog_digest.value()) << ";\n"
      << "inline constexpr std::string_view "
         "kRetainedEnvironmentDigest = "
      << cpp_string_literal(node.environment_digest.value()) << ";\n\n"
      << "}  // namespace manyforge::deployment::detail::"
         "generated_build_qualified_admission_profile\n";
  return output.str();
}

void write_profile(const std::filesystem::path& output_path,
                   std::string_view bytes) {
  if (output_path.empty()) {
    throw std::invalid_argument("output path must not be empty");
  }
  const std::filesystem::path temporary_path =
      output_path.string() + ".tmp";
  std::error_code ignored;
  std::filesystem::remove(temporary_path, ignored);

  try {
    {
      std::ofstream output(
          temporary_path, std::ios::binary | std::ios::trunc);
      if (!output) {
        throw std::runtime_error(
            "cannot open temporary generated profile");
      }
      output.write(
          bytes.data(), static_cast<std::streamsize>(bytes.size()));
      output.flush();
      if (!output) {
        throw std::runtime_error(
            "cannot write generated admission profile");
      }
    }
    std::filesystem::rename(temporary_path, output_path);
  } catch (...) {
    std::filesystem::remove(temporary_path, ignored);
    throw;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 || std::string_view(argv[1]) != "--output" ||
      std::string_view(argv[2]).empty()) {
    std::cerr
        << "usage: manyforge_emit_build_qualified_admission_profile "
           "--output PATH\n";
    return 2;
  }
  try {
    write_profile(std::filesystem::path(argv[2]), emit_profile());
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "build-qualified admission profile emission failed: "
              << error.what() << '\n';
    return 1;
  }
}
