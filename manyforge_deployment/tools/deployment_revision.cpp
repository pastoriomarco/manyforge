#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/deployment_authoring.hpp>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace deployment = manyforge::deployment;

enum class Mode { kCheck, kPrint, kUpdate };

struct Options final {
  Mode mode;
  std::filesystem::path input;
  std::optional<bool> migrate_controlled_stop_jerk;
  std::map<std::string, std::string> import_options;
};

[[nodiscard]] Options parse_options(int argc, char **argv) {
  constexpr auto usage =
      "usage: manyforge_deployment_revision "
      "(--check|--print|--update) --input <deployment.yaml> "
      "[--migrate-controlled-stop-jerk on|off | "
      "--import-moveit-jerk <joint_limits.yaml> --source-id <package/path> "
      "--runtime-unit <id> --component <id> --target <id> "
      "--joints <source-joint,...>]";
  if (argc < 4 || argc % 2 != 0 || std::string_view(argv[2]) != "--input" ||
      std::string_view(argv[3]).empty()) {
    throw std::invalid_argument(usage);
  }
  const std::string_view mode(argv[1]);
  std::optional<bool> migration;
  std::map<std::string, std::string> imports;
  if (argc == 6 &&
      std::string_view(argv[4]) == "--migrate-controlled-stop-jerk") {
    const std::string_view choice(argv[5]);
    if (std::string_view(argv[4]) != "--migrate-controlled-stop-jerk" ||
        (choice != "on" && choice != "off") || mode == "--check")
      throw std::invalid_argument(usage);
    migration = choice == "on";
  } else if (argc > 4) {
    for (int index = 4; index < argc; index += 2) {
      const std::string key(argv[index]);
      if (std::string_view(argv[index + 1]).empty() ||
          (key != "--import-moveit-jerk" && key != "--source-id" &&
           key != "--runtime-unit" && key != "--component" &&
           key != "--target" && key != "--joints") ||
          !imports.emplace(key, argv[index + 1]).second)
        throw std::invalid_argument(usage);
    }
    if (imports.size() != 6U || mode == "--check")
      throw std::invalid_argument(usage);
  }
  if (mode == "--check") {
    return Options{Mode::kCheck, argv[3], migration, imports};
  }
  if (mode == "--print") {
    return Options{Mode::kPrint, argv[3], migration, imports};
  }
  if (mode == "--update") {
    return Options{Mode::kUpdate, argv[3], migration, imports};
  }
  throw std::invalid_argument(usage);
}

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  if (std::filesystem::is_symlink(path)) {
    throw std::invalid_argument(
        "deployment source must not be a symbolic link");
  }
  const auto size = std::filesystem::file_size(path);
  if (size > deployment::kMaxArtifactEncodedBytes) {
    throw std::invalid_argument(
        "deployment source exceeds the artifact byte bound");
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot open deployment source: " + path.string());
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream && !bytes.empty()) {
    throw std::runtime_error("cannot read deployment source: " + path.string());
  }
  return bytes;
}

[[nodiscard]] std::string pretty_json(std::string_view canonical) {
  std::string result;
  result.reserve(canonical.size() + canonical.size() / 4U);
  std::size_t indentation = 0U;
  bool in_string = false;
  bool escaped = false;
  const auto newline = [&result, &indentation] {
    result.push_back('\n');
    result.append(indentation * 2U, ' ');
  };

  for (std::size_t index = 0U; index < canonical.size(); ++index) {
    const char byte = canonical[index];
    if (in_string) {
      result.push_back(byte);
      if (escaped) {
        escaped = false;
      } else if (byte == '\\') {
        escaped = true;
      } else if (byte == '"') {
        in_string = false;
      }
      continue;
    }
    if (byte == '"') {
      in_string = true;
      result.push_back(byte);
      continue;
    }
    switch (byte) {
    case '{':
    case '[': {
      result.push_back(byte);
      const char close = byte == '{' ? '}' : ']';
      if (index + 1U < canonical.size() && canonical[index + 1U] != close) {
        ++indentation;
        newline();
      }
      break;
    }
    case '}':
    case ']': {
      const char open = byte == '}' ? '{' : '[';
      if (index > 0U && canonical[index - 1U] != open) {
        --indentation;
        newline();
      }
      result.push_back(byte);
      break;
    }
    case ',':
      result.push_back(byte);
      newline();
      break;
    case ':':
      result.append(": ");
      break;
    default:
      result.push_back(byte);
      break;
    }
  }
  if (in_string || escaped || indentation != 0U) {
    throw std::logic_error("canonical JSON formatter lost structural state");
  }
  result.push_back('\n');
  return result;
}

void replace_file_atomically(const std::filesystem::path &path,
                             std::string_view bytes) {
  std::filesystem::path temporary = path;
  temporary += ".manyforge-deployment-revision." +
               std::to_string(static_cast<long long>(::getpid())) + ".tmp";
  if (std::filesystem::exists(temporary)) {
    throw std::runtime_error("refusing to overwrite temporary file: " +
                             temporary.string());
  }
  try {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw std::runtime_error("cannot create temporary deployment source: " +
                               temporary.string());
    }
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) {
      throw std::runtime_error("cannot write temporary deployment source: " +
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

    if (options.mode == Mode::kCheck) {
      const auto verified = deployment::verify_deployment_draft(bytes);
      std::cout << verified.deployment_revision << '\n';
      return 0;
    }

    const auto produced = [&] {
      if (!options.import_options.empty()) {
        const auto &selected = options.import_options;
        std::vector<std::string> joints;
        std::istringstream names(selected.at("--joints"));
        for (std::string joint; std::getline(names, joint, ',');)
          joints.push_back(joint);
        if (selected.at("--joints").back() == ',')
          joints.emplace_back();
        return deployment::import_moveit_angular_jerk_limits(
            bytes, read_file(selected.at("--import-moveit-jerk")),
            selected.at("--source-id"), selected.at("--runtime-unit"),
            selected.at("--component"), selected.at("--target"), joints);
      }
      return options.migrate_controlled_stop_jerk.has_value()
                 ? deployment::migrate_controlled_stop_jerk_choice(
                       bytes, *options.migrate_controlled_stop_jerk)
                 : deployment::compile_deployment_draft(bytes);
    }();
    if (options.mode == Mode::kPrint) {
      std::cout << produced.deployment_revision << '\n';
      return 0;
    }

    const std::string updated = pretty_json(produced.canonical_json);
    const auto verified = deployment::verify_deployment_draft(updated);
    if (verified.deployment_revision != produced.deployment_revision ||
        verified.canonical_json != produced.canonical_json) {
      throw std::logic_error(
          "updated deployment source did not independently verify");
    }
    replace_file_atomically(options.input, updated);
    std::cout << produced.deployment_revision << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "manyforge_deployment_revision: " << error.what() << '\n';
    return 2;
  }
}
