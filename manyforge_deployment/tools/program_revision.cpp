#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "content_identity_internal.hpp"

namespace {

namespace deployment = manyforge::deployment;

enum class Mode { kCheck, kPrint, kUpdate };

struct Options final {
  Mode mode;
  std::filesystem::path input;
};

[[nodiscard]] Options parse_options(int argc, char **argv) {
  if (argc != 4 || std::string_view(argv[2]) != "--input" ||
      std::string_view(argv[3]).empty()) {
    throw std::invalid_argument(
        "usage: manyforge_program_revision (--check|--print|--update) "
        "--input <program.yaml>");
  }
  const std::string_view mode(argv[1]);
  if (mode == "--check") {
    return Options{Mode::kCheck, argv[3]};
  }
  if (mode == "--print") {
    return Options{Mode::kPrint, argv[3]};
  }
  if (mode == "--update") {
    return Options{Mode::kUpdate, argv[3]};
  }
  throw std::invalid_argument(
      "usage: manyforge_program_revision (--check|--print|--update) "
      "--input <program.yaml>");
}

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  const auto size = std::filesystem::file_size(path);
  if (size > deployment::kMaxArtifactEncodedBytes) {
    throw std::invalid_argument(
        "Program source exceeds the artifact byte bound");
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot open Program source: " + path.string());
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream && !bytes.empty()) {
    throw std::runtime_error("cannot read Program source: " + path.string());
  }
  return bytes;
}

[[nodiscard]] const std::string &
require_string_member(const deployment::CanonicalValue &root,
                      std::string_view name) {
  const auto *value = root.find_object_member(name);
  if (value == nullptr) {
    throw std::invalid_argument("Program source is missing " +
                                std::string(name));
  }
  const auto *text = std::get_if<std::string>(&value->storage());
  if (text == nullptr) {
    throw std::invalid_argument("Program source member is not text: " +
                                std::string(name));
  }
  return *text;
}

struct RevisionCandidate final {
  std::string declared;
  std::string computed;
};

[[nodiscard]] const deployment::CanonicalValue &
require_program_root(const deployment::ParsedArtifactCandidate &parsed) {
  const deployment::CanonicalValue &root = parsed.root();
  if (!std::holds_alternative<deployment::CanonicalValue::Object>(
          root.storage())) {
    throw std::invalid_argument("Program source root must be an object");
  }
  deployment::require_supported_program_format(root);
  const std::string &declared = require_string_member(root, "program_revision");
  if (!deployment::is_sha256_digest(declared)) {
    throw std::invalid_argument(
        "Program source program_revision must be a lowercase sha256 digest");
  }
  return root;
}

[[nodiscard]] RevisionCandidate
compute_program_revision(const deployment::CanonicalValue &root) {
  const std::string &declared = require_string_member(root, "program_revision");
  return RevisionCandidate{
      declared,
      deployment::detail::compute_content_identity(
          deployment::normalize_program_authoring(root),
          deployment::detail::ContentIdentitySpec{
              .identity_domain = "program-revision",
              .api_version = std::string(manyforge::core::kProgramApiVersion),
              .own_field = "program_revision",
          }),
  };
}

[[nodiscard]] RevisionCandidate compute_revision(std::string_view bytes) {
  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  return compute_program_revision(require_program_root(parsed));
}

[[nodiscard]] std::string replace_revision_line(std::string_view bytes,
                                                std::string_view revision) {
  constexpr std::string_view prefix = "program_revision:";
  std::size_t match_start = std::string_view::npos;
  std::size_t match_end = std::string_view::npos;
  std::size_t cursor = 0U;
  while (cursor <= bytes.size()) {
    const std::size_t end = bytes.find('\n', cursor);
    const std::size_t line_end =
        end == std::string_view::npos ? bytes.size() : end;
    const std::string_view line = bytes.substr(cursor, line_end - cursor);
    if (line.starts_with(prefix)) {
      if (match_start != std::string_view::npos) {
        throw std::invalid_argument("Program source contains multiple "
                                    "top-level program_revision lines");
      }
      match_start = cursor;
      match_end = line_end;
    }
    if (end == std::string_view::npos) {
      break;
    }
    cursor = end + 1U;
  }
  if (match_start == std::string_view::npos) {
    throw std::invalid_argument("Program source must spell program_revision as "
                                "one plain top-level field");
  }
  std::string updated;
  updated.reserve(bytes.size());
  updated.append(bytes.substr(0U, match_start));
  updated.append(prefix);
  updated.push_back(' ');
  updated.append(revision);
  updated.append(bytes.substr(match_end));
  return updated;
}

void replace_file_atomically(const std::filesystem::path &path,
                             std::string_view bytes) {
  std::filesystem::path temporary = path;
  temporary += ".manyforge-program-revision." +
               std::to_string(static_cast<long long>(::getpid())) + ".tmp";
  if (std::filesystem::exists(temporary)) {
    throw std::runtime_error("refusing to overwrite temporary file: " +
                             temporary.string());
  }
  try {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw std::runtime_error("cannot create temporary Program source: " +
                               temporary.string());
    }
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) {
      throw std::runtime_error("cannot write temporary Program source: " +
                               temporary.string());
    }
    std::filesystem::permissions(temporary,
                                 std::filesystem::status(path).permissions());
    std::filesystem::rename(temporary, path);
  } catch (...) {
    std::error_code ignored;
    (void)std::filesystem::remove(temporary, ignored);
    throw;
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parse_options(argc, argv);
    const std::string bytes = read_file(options.input);
    const RevisionCandidate revision = compute_revision(bytes);
    if (options.mode == Mode::kCheck) {
      if (revision.declared != revision.computed) {
        std::cerr << options.input.string()
                  << ": stale program_revision; expected "
                  << revision.computed << '\n';
        return 2;
      }
      std::cout << revision.computed << '\n';
      return 0;
    }
    if (options.mode == Mode::kPrint) {
      std::cout << revision.computed << '\n';
      return 0;
    }

    const std::string updated = replace_revision_line(bytes, revision.computed);
    const RevisionCandidate verified = compute_revision(updated);
    if (verified.declared != verified.computed ||
        verified.computed != revision.computed) {
      throw std::logic_error("updated Program revision did not verify");
    }
    replace_file_atomically(options.input, updated);
    std::cout << revision.computed << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "manyforge_program_revision: " << error.what() << '\n';
    return 1;
  }
}
