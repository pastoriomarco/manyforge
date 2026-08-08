#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "content_identity_internal.hpp"

namespace {

namespace deployment = manyforge::deployment;

enum class Mode { kCheck, kPrint, kUpdate };

struct Options final {
  Mode mode;
  std::filesystem::path input;
};

[[nodiscard]] Options parse_options(int argc, char** argv) {
  if (argc != 4 || std::string_view(argv[2]) != "--input" || std::string_view(argv[3]).empty()) {
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

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
  const auto size = std::filesystem::file_size(path);
  if (size > deployment::kMaxArtifactEncodedBytes) {
    throw std::invalid_argument("Program source exceeds the artifact byte bound");
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

[[nodiscard]] const std::string& require_string_member(const deployment::CanonicalValue& root,
                                                       std::string_view name) {
  const auto* value = root.find_object_member(name);
  if (value == nullptr) {
    throw std::invalid_argument("Program source is missing " + std::string(name));
  }
  const auto* text = std::get_if<std::string>(&value->storage());
  if (text == nullptr) {
    throw std::invalid_argument("Program source member is not text: " + std::string(name));
  }
  return *text;
}

struct RevisionCandidate final {
  std::string declared;
  std::string computed;
};

[[nodiscard]] const deployment::CanonicalValue& require_program_root(
    const deployment::ParsedArtifactCandidate& parsed) {
  const deployment::CanonicalValue& root = parsed.root();
  if (!std::holds_alternative<deployment::CanonicalValue::Object>(root.storage())) {
    throw std::invalid_argument("Program source root must be an object");
  }
  if (require_string_member(root, "api_version") != "manyforge.program.v1") {
    throw std::invalid_argument("Program source api_version must be manyforge.program.v1");
  }
  const std::string& declared = require_string_member(root, "program_revision");
  if (!deployment::is_sha256_digest(declared)) {
    throw std::invalid_argument(
        "Program source program_revision must be a lowercase sha256 digest");
  }
  return root;
}

[[nodiscard]] std::vector<RevisionCandidate> compute_contact_policy_revisions(
    const deployment::CanonicalValue& root) {
  const auto* policies_value = root.find_object_member("contact_policies");
  if (policies_value == nullptr) {
    return {};
  }
  const auto* policies = std::get_if<deployment::CanonicalValue::Array>(&policies_value->storage());
  if (policies == nullptr) {
    throw std::invalid_argument("Program contact_policies must be an array");
  }
  std::vector<RevisionCandidate> revisions;
  revisions.reserve(policies->size());
  for (const auto& policy : *policies) {
    if (!std::holds_alternative<deployment::CanonicalValue::Object>(policy.storage())) {
      throw std::invalid_argument("Program contact policy must be an object");
    }
    if (require_string_member(policy, "api_version") != "manyforge.contact_policy.v1") {
      throw std::invalid_argument(
          "Program contact policy api_version must be "
          "manyforge.contact_policy.v1");
    }
    const std::string& declared = require_string_member(policy, "version");
    if (!deployment::is_sha256_digest(declared)) {
      throw std::invalid_argument(
          "Program contact policy version must be a lowercase sha256 digest");
    }
    revisions.push_back(RevisionCandidate{
        declared,
        deployment::detail::compute_content_identity(
            policy,
            deployment::detail::ContentIdentitySpec{
                .identity_domain = "catalog-record-revision/contact_policy",
                .api_version = "manyforge.contact_policy.v1",
                .own_field = "version",
            }),
    });
  }
  return revisions;
}

[[nodiscard]] RevisionCandidate compute_program_revision(const deployment::CanonicalValue& root) {
  const std::string& declared = require_string_member(root, "program_revision");
  return RevisionCandidate{
      declared,
      deployment::detail::compute_content_identity(root,
                                                   deployment::detail::ContentIdentitySpec{
                                                       .identity_domain = "program-revision",
                                                       .api_version = "manyforge.program.v1",
                                                       .own_field = "program_revision",
                                                   }),
  };
}

struct SourceRevisions final {
  RevisionCandidate program;
  std::vector<RevisionCandidate> contact_policies;
};

[[nodiscard]] SourceRevisions compute_revisions(std::string_view bytes) {
  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  const deployment::CanonicalValue& root = require_program_root(parsed);
  return SourceRevisions{compute_program_revision(root), compute_contact_policy_revisions(root)};
}

[[nodiscard]] std::string replace_revision_line(std::string_view bytes, std::string_view revision) {
  constexpr std::string_view prefix = "program_revision:";
  std::size_t match_start = std::string_view::npos;
  std::size_t match_end = std::string_view::npos;
  std::size_t cursor = 0U;
  while (cursor <= bytes.size()) {
    const std::size_t end = bytes.find('\n', cursor);
    const std::size_t line_end = end == std::string_view::npos ? bytes.size() : end;
    const std::string_view line = bytes.substr(cursor, line_end - cursor);
    if (line.starts_with(prefix)) {
      if (match_start != std::string_view::npos) {
        throw std::invalid_argument(
            "Program source contains multiple "
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
    throw std::invalid_argument(
        "Program source must spell program_revision as "
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

[[nodiscard]] std::string replace_contact_policy_revision_lines(
    std::string_view bytes, const std::vector<RevisionCandidate>& revisions) {
  if (revisions.empty()) {
    return std::string(bytes);
  }
  constexpr std::string_view section = "contact_policies:";
  constexpr std::string_view prefix = "    version:";
  std::string updated;
  updated.reserve(bytes.size());
  bool in_section = false;
  std::size_t revision_index = 0U;
  std::size_t cursor = 0U;
  while (cursor <= bytes.size()) {
    const std::size_t newline = bytes.find('\n', cursor);
    const std::size_t line_end = newline == std::string_view::npos ? bytes.size() : newline;
    const std::string_view line = bytes.substr(cursor, line_end - cursor);
    if (line == section) {
      in_section = true;
    } else if (in_section && !line.empty() && line.front() != ' ' && line.front() != '#') {
      in_section = false;
    }
    if (in_section && line.starts_with(prefix)) {
      if (revision_index >= revisions.size()) {
        throw std::invalid_argument(
            "Program source contains too many contact-policy version lines");
      }
      updated.append(prefix);
      updated.push_back(' ');
      updated.append(revisions[revision_index++].computed);
    } else {
      updated.append(line);
    }
    if (newline == std::string_view::npos) {
      break;
    }
    updated.push_back('\n');
    cursor = newline + 1U;
  }
  if (revision_index != revisions.size()) {
    throw std::invalid_argument(
        "Program source must spell every contact-policy version as a plain "
        "field in contact_policies");
  }
  return updated;
}

void require_current_contact_policy_revisions(const std::filesystem::path& input,
                                              const std::vector<RevisionCandidate>& revisions) {
  for (std::size_t index = 0U; index < revisions.size(); ++index) {
    if (revisions[index].declared != revisions[index].computed) {
      throw std::invalid_argument(input.string() + ": stale contact_policies[" +
                                  std::to_string(index) + "].version; run --update");
    }
  }
}

void replace_file_atomically(const std::filesystem::path& path, std::string_view bytes) {
  std::filesystem::path temporary = path;
  temporary +=
      ".manyforge-program-revision." + std::to_string(static_cast<long long>(::getpid())) + ".tmp";
  if (std::filesystem::exists(temporary)) {
    throw std::runtime_error("refusing to overwrite temporary file: " + temporary.string());
  }
  try {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw std::runtime_error("cannot create temporary Program source: " + temporary.string());
    }
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) {
      throw std::runtime_error("cannot write temporary Program source: " + temporary.string());
    }
    std::filesystem::permissions(temporary, std::filesystem::status(path).permissions());
    std::filesystem::rename(temporary, path);
  } catch (...) {
    std::error_code ignored;
    (void)std::filesystem::remove(temporary, ignored);
    throw;
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    const std::string bytes = read_file(options.input);
    SourceRevisions revisions = compute_revisions(bytes);
    if (options.mode == Mode::kCheck) {
      require_current_contact_policy_revisions(options.input, revisions.contact_policies);
      if (revisions.program.declared != revisions.program.computed) {
        std::cerr << options.input.string() << ": stale program_revision; expected "
                  << revisions.program.computed << '\n';
        return 2;
      }
      std::cout << revisions.program.computed << '\n';
      return 0;
    }
    if (options.mode == Mode::kPrint) {
      require_current_contact_policy_revisions(options.input, revisions.contact_policies);
      std::cout << revisions.program.computed << '\n';
      return 0;
    }

    std::string updated = replace_contact_policy_revision_lines(bytes, revisions.contact_policies);
    revisions = compute_revisions(updated);
    require_current_contact_policy_revisions(options.input, revisions.contact_policies);
    updated = replace_revision_line(updated, revisions.program.computed);
    const SourceRevisions verified = compute_revisions(updated);
    require_current_contact_policy_revisions(options.input, verified.contact_policies);
    if (verified.program.declared != verified.program.computed ||
        verified.program.computed != revisions.program.computed) {
      throw std::logic_error("updated Program revision did not verify");
    }
    replace_file_atomically(options.input, updated);
    std::cout << revisions.program.computed << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "manyforge_program_revision: " << error.what() << '\n';
    return 1;
  }
}
