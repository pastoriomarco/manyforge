#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "native_execution_runtime_unit_internal.hpp"

namespace {

namespace core = manyforge::core;
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
        "usage: manyforge_deployment_revision "
        "(--check|--print|--update) --input <deployment.yaml>");
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
      "usage: manyforge_deployment_revision "
      "(--check|--print|--update) --input <deployment.yaml>");
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

[[nodiscard]] const std::string &
require_string_member(const deployment::CanonicalValue &root,
                      std::string_view name) {
  const auto *value = root.find_object_member(name);
  if (value == nullptr) {
    throw std::invalid_argument("source is missing " + std::string(name));
  }
  const auto *text = std::get_if<std::string>(&value->storage());
  if (text == nullptr) {
    throw std::invalid_argument("source member is not text: " +
                                std::string(name));
  }
  return *text;
}

[[nodiscard]] deployment::QualifiedRuntimeProfile
qualified_native_runtime_profile() {
  const std::string bytes =
      deployment::detail::native_execution_runtime_profile_document();
  const deployment::ParsedArtifactCandidate parsed =
      deployment::parse_strict_json_model_yaml(bytes);
  const std::string &revision =
      require_string_member(parsed.root(), "runtime_profile_revision");
  const auto *schema =
      deployment::build_qualified_closed_schema_registry()
          .find_catalog_schema_family(core::CatalogRecordKind::kRuntimeProfile);
  if (schema == nullptr ||
      schema->qualification() !=
          deployment::CatalogSchemaQualification::kQualified ||
      schema->compiled_schema() == nullptr) {
    throw std::logic_error(
        "native runtime profile schema is not build-qualified");
  }

  const core::ContentDigest digest(deployment::sha256_digest(bytes));
  const core::ContentAddressedStoreKey store_key(
      "sha256/" + digest.value().substr(std::string_view("sha256:").size()));
  const core::RuntimeProfileCatalogRecordRefDTO reference{
      core::RuntimeProfileId(
          std::string(deployment::detail::kNativeExecutionRuntimeProfileId)),
      core::RuntimeProfileRevision(revision),
      core::SchemaId(std::string(core::kRuntimeProfileApiVersion)),
      schema->compiled_schema()->schema_revision(),
      digest,
      store_key};
  return deployment::resolve_and_qualify_runtime_profile(reference, store_key,
                                                         bytes);
}

[[nodiscard]] bool
selects_native_runtime_profile(const deployment::CanonicalValue &root) {
  const auto *graph = root.find_object_member("graph");
  const auto *graph_object =
      graph == nullptr
          ? nullptr
          : std::get_if<deployment::CanonicalValue::Object>(&graph->storage());
  if (graph_object == nullptr) {
    return false;
  }
  const auto runtime_units = std::find_if(
      graph_object->begin(), graph_object->end(),
      [](const auto &member) { return member.first == "runtime_units"; });
  if (runtime_units == graph_object->end()) {
    return false;
  }
  const auto *rows = std::get_if<deployment::CanonicalValue::Array>(
      &runtime_units->second.storage());
  if (rows == nullptr) {
    return false;
  }
  for (const auto &row : *rows) {
    const auto *profile = row.find_object_member("runtime_profile_id");
    const auto *profile_id =
        profile == nullptr ? nullptr
                           : std::get_if<std::string>(&profile->storage());
    if (profile_id != nullptr &&
        *profile_id == deployment::detail::kNativeExecutionRuntimeProfileId) {
      return true;
    }
  }
  return false;
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
    const deployment::ParsedArtifactCandidate parsed =
        deployment::parse_strict_json_model_yaml(bytes);
    std::vector<deployment::QualifiedRuntimeProfile> profile_storage;
    if (selects_native_runtime_profile(parsed.root())) {
      profile_storage.push_back(qualified_native_runtime_profile());
    }
    const std::span<const deployment::QualifiedRuntimeProfile> profiles(
        profile_storage);

    if (options.mode == Mode::kCheck) {
      const auto verified =
          deployment::verify_deployment_artifact_revision(parsed, profiles);
      std::cout << verified.deployment_revision().value() << '\n';
      return 0;
    }

    const auto produced =
        deployment::compute_deployment_artifact_revision(parsed, profiles);
    if (options.mode == Mode::kPrint) {
      std::cout << produced.deployment_revision().value() << '\n';
      return 0;
    }

    const std::string updated = pretty_json(
        deployment::canonical_json(produced.normalized_authored_root()));
    const auto verified = deployment::verify_deployment_artifact_revision(
        deployment::parse_strict_json_model_yaml(updated), profiles);
    if (verified.deployment_revision() != produced.deployment_revision() ||
        verified.normalized_authored_root() !=
            produced.normalized_authored_root()) {
      throw std::logic_error(
          "updated deployment source did not independently verify");
    }
    replace_file_atomically(options.input, updated);
    std::cout << produced.deployment_revision().value() << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "manyforge_deployment_revision: " << error.what() << '\n';
    return 2;
  }
}
