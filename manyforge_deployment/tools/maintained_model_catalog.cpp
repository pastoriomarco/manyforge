#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "activation_prepared_model_internal.hpp"

namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

constexpr std::size_t kMaxMaintainedSourceArtifacts = 4096U;

enum class Mode { kCheck, kPrint, kUpdate };

struct Options final {
  Mode mode;
  std::filesystem::path description;
  std::filesystem::path workspace_root;
  std::map<std::string, std::filesystem::path, std::less<>> prepared_roots;
  std::optional<std::string> selected_family;
};

struct FamilySource final {
  std::string family_id;
  std::string model_asset_id;
  std::filesystem::path catalog_source;
  std::filesystem::path urdf_source;
  std::filesystem::path historical_urdf_source;
  std::filesystem::path srdf_source;
  std::filesystem::path historical_srdf_source;
  std::filesystem::path resource_root;
  std::string accepted_package_uri_prefix;
  std::string urdf_projection_kind;
  std::string srdf_projection_kind;
  deployment::CanonicalValue semantics;
};

struct GeneratedFamily final {
  FamilySource source;
  deployment::detail::ActivationPreparedModel model;
};

[[noreturn]] void fail(std::string message) {
  throw std::invalid_argument(std::move(message));
}

[[nodiscard]] std::filesystem::path
require_plain_absolute_directory(const std::filesystem::path &path,
                                 std::string_view label) {
  if (!path.is_absolute() || std::filesystem::is_symlink(path) ||
      !std::filesystem::is_directory(path)) {
    fail(std::string(label) + " must select one plain absolute directory");
  }
  return path.lexically_normal();
}

[[nodiscard]] std::filesystem::path
require_safe_relative_path(std::string_view text, std::string_view label) {
  const std::filesystem::path path(text);
  if (text.empty() || path.is_absolute() || path != path.lexically_normal() ||
      std::find(path.begin(), path.end(), std::filesystem::path("..")) !=
          path.end()) {
    fail(std::string(label) + " must be a normalized root-local path");
  }
  return path;
}

[[nodiscard]] std::string read_plain_file(const std::filesystem::path &path,
                                          std::uintmax_t byte_limit,
                                          std::string_view label) {
  if (std::filesystem::is_symlink(path) ||
      !std::filesystem::is_regular_file(path)) {
    fail(std::string(label) + " must select one plain file: " + path.string());
  }
  const auto size = std::filesystem::file_size(path);
  if (size == 0U || size > byte_limit) {
    fail(std::string(label) +
         " is empty or exceeds its byte bound: " + path.string());
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    fail("cannot open " + std::string(label) + ": " + path.string());
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!input && !bytes.empty()) {
    fail("cannot read " + std::string(label) + ": " + path.string());
  }
  return bytes;
}

[[nodiscard]] const deployment::CanonicalValue::Object &
require_object(const deployment::CanonicalValue &value,
               std::string_view label) {
  const auto *object =
      std::get_if<deployment::CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(std::string(label) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const deployment::CanonicalValue::Array &
require_array(const deployment::CanonicalValue &value, std::string_view label) {
  const auto *array =
      std::get_if<deployment::CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(std::string(label) + " must be an array");
  }
  return *array;
}

[[nodiscard]] const deployment::CanonicalValue &
require_member(const deployment::CanonicalValue &value, std::string_view name,
               std::string_view label) {
  (void)require_object(value, label);
  const auto *member = value.find_object_member(name);
  if (member == nullptr) {
    fail(std::string(label) + " is missing " + std::string(name));
  }
  return *member;
}

[[nodiscard]] const std::string &
require_text(const deployment::CanonicalValue &value, std::string_view label) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr || text->empty()) {
    fail(std::string(label) + " must be non-empty text");
  }
  return *text;
}

void require_shape(const deployment::CanonicalValue &value,
                   std::span<const std::string_view> expected,
                   std::string_view label) {
  const auto &object = require_object(value, label);
  std::set<std::string_view, std::less<>> remaining(expected.begin(),
                                                    expected.end());
  for (const auto &[name, unused] : object) {
    (void)unused;
    if (remaining.erase(name) == 0U) {
      fail(std::string(label) + " contains unknown field " + name);
    }
  }
  if (!remaining.empty()) {
    fail(std::string(label) + " is missing field " +
         std::string(*remaining.begin()));
  }
}

void replace_file_atomically(const std::filesystem::path &path,
                             std::string_view bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::filesystem::path temporary = path;
  temporary += ".manyforge-model-catalog." +
               std::to_string(static_cast<long long>(::getpid())) + ".tmp";
  if (std::filesystem::exists(temporary)) {
    fail("refusing to overwrite temporary catalog source: " +
         temporary.string());
  }
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      fail("cannot create temporary catalog source: " + temporary.string());
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
      fail("cannot write temporary catalog source: " + temporary.string());
    }
    std::filesystem::rename(temporary, path);
  } catch (...) {
    std::error_code ignored;
    (void)std::filesystem::remove(temporary, ignored);
    throw;
  }
}

[[nodiscard]] Options parse_options(int argc, char **argv) {
  if (argc < 8) {
    fail("usage: manyforge_maintained_model_catalog "
         "(--check|--print|--update) --description <file> "
         "--workspace-root <directory> "
         "--prepared <family-id>=<directory> [--prepared ...] "
         "[--family <family-id>]");
  }
  const std::string_view mode(argv[1]);
  Options result{mode == "--check"   ? Mode::kCheck
                 : mode == "--print" ? Mode::kPrint
                 : mode == "--update"
                     ? Mode::kUpdate
                     : throw std::invalid_argument("unknown mode"),
                 {},
                 {},
                 {},
                 std::nullopt};
  for (int index = 2; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (index + 1 >= argc) {
      fail("authoring option is missing its value: " + std::string(option));
    }
    const std::string value(argv[++index]);
    if (option == "--description") {
      if (!result.description.empty()) {
        fail("--description may be supplied only once");
      }
      result.description = value;
    } else if (option == "--workspace-root") {
      if (!result.workspace_root.empty()) {
        fail("--workspace-root may be supplied only once");
      }
      result.workspace_root = value;
    } else if (option == "--prepared") {
      const auto separator = value.find('=');
      if (separator == std::string::npos || separator == 0U ||
          separator + 1U == value.size() ||
          !result.prepared_roots
               .emplace(value.substr(0U, separator),
                        value.substr(separator + 1U))
               .second) {
        fail("--prepared requires a unique family-id=directory value");
      }
    } else if (option == "--family") {
      if (result.selected_family.has_value() || value.empty()) {
        fail("--family requires one non-empty value");
      }
      result.selected_family = value;
    } else {
      fail("unknown authoring option: " + std::string(option));
    }
  }
  if (result.description.empty() || result.workspace_root.empty() ||
      result.prepared_roots.empty()) {
    fail("description, workspace root, and prepared roots are required");
  }
  if (result.mode == Mode::kPrint && !result.selected_family.has_value()) {
    fail("--print requires --family");
  }
  return result;
}

[[nodiscard]] std::vector<FamilySource> load_families(const Options &options) {
  const std::string description_bytes =
      read_plain_file(options.description, deployment::kMaxArtifactEncodedBytes,
                      "maintained source description");
  const auto parsed =
      deployment::parse_strict_json_model_yaml(description_bytes);
  constexpr std::array<std::string_view, 7U> kDescriptionFields{
      "api_version",
      "assistant_profiles_source",
      "deployments",
      "historical_program_sources",
      "historical_source_commit",
      "model_families",
      "runtime_restoration_profiles"};
  require_shape(parsed.root(), kDescriptionFields, "description");
  if (require_text(require_member(parsed.root(), "api_version", "description"),
                   "description.api_version") !=
      "manyforge.maintained_deployment_sources.v1") {
    fail("maintained source description has an unsupported API version");
  }
  const auto &rows = require_array(
      require_member(parsed.root(), "model_families", "description"),
      "description.model_families");

  std::vector<FamilySource> result;
  result.reserve(rows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const auto &row = rows[index];
    const std::string label =
        "description.model_families[" + std::to_string(index) + "]";
    constexpr std::array<std::string_view, 5U> kFamilyFields{
        "catalog_source", "model_asset_id", "model_family_id", "semantics",
        "source"};
    require_shape(row, kFamilyFields, label);
    const auto &source = require_member(row, "source", label);
    constexpr std::array<std::string_view, 8U> kSourceFields{
        "accepted_package_uri_prefix",
        "historical_srdf_path",
        "historical_urdf_path",
        "resource_root",
        "srdf_path",
        "srdf_projection_kind",
        "urdf_path",
        "urdf_projection_kind",
    };
    require_shape(source, kSourceFields, label + ".source");
    const auto &semantics = require_member(row, "semantics", label);
    constexpr std::array<std::string_view, 6U> kSemanticFields{
        "collision_body_role_bindings", "frame_role_bindings",
        "instance_cardinality",         "model_group_role_bindings",
        "resource_role_bindings",       "root_frame_role_id"};
    require_shape(semantics, kSemanticFields, label + ".semantics");

    const auto root_local = [&options, &label](const std::string &value,
                                               std::string_view field) {
      return options.workspace_root /
             require_safe_relative_path(value,
                                        label + "." + std::string(field));
    };
    result.push_back(FamilySource{
        require_text(require_member(row, "model_family_id", label),
                     label + ".model_family_id"),
        require_text(require_member(row, "model_asset_id", label),
                     label + ".model_asset_id"),
        root_local(require_text(require_member(row, "catalog_source", label),
                                label + ".catalog_source"),
                   "catalog_source"),
        root_local(require_text(require_member(source, "urdf_path", label),
                                label + ".source.urdf_path"),
                   "source.urdf_path"),
        root_local(
            require_text(require_member(source, "historical_urdf_path", label),
                         label + ".source.historical_urdf_path"),
            "source.historical_urdf_path"),
        root_local(require_text(require_member(source, "srdf_path", label),
                                label + ".source.srdf_path"),
                   "source.srdf_path"),
        root_local(
            require_text(require_member(source, "historical_srdf_path", label),
                         label + ".source.historical_srdf_path"),
            "source.historical_srdf_path"),
        root_local(require_text(require_member(source, "resource_root", label),
                                label + ".source.resource_root"),
                   "source.resource_root"),
        require_text(
            require_member(source, "accepted_package_uri_prefix", label),
            label + ".source.accepted_package_uri_prefix"),
        require_text(require_member(source, "urdf_projection_kind", label),
                     label + ".source.urdf_projection_kind"),
        require_text(require_member(source, "srdf_projection_kind", label),
                     label + ".source.srdf_projection_kind"),
        semantics,
    });
  }
  if (result.empty()) {
    fail("maintained source description has no model families");
  }
  std::sort(result.begin(), result.end(),
            [](const FamilySource &left, const FamilySource &right) {
              return left.family_id < right.family_id;
            });
  for (std::size_t index = 1U; index < result.size(); ++index) {
    if (result[index - 1U].family_id == result[index].family_id ||
        result[index - 1U].model_asset_id == result[index].model_asset_id) {
      fail("maintained model families have duplicate identities");
    }
  }
  return result;
}

[[nodiscard]] std::string remove_one_hpp_gripper_block(std::string historical) {
  const auto tag = historical.find("<gripper");
  if (tag == std::string::npos ||
      historical.find("<gripper", tag + 1U) != std::string::npos) {
    fail("HPP SRDF projection requires exactly one <gripper> element");
  }
  const auto line_start = historical.rfind('\n', tag);
  const auto begin = line_start == std::string::npos ? 0U : line_start + 1U;
  if (!std::all_of(
          historical.begin() + static_cast<std::ptrdiff_t>(begin),
          historical.begin() + static_cast<std::ptrdiff_t>(tag),
          [](unsigned char byte) { return byte == ' ' || byte == '\t'; })) {
    fail("HPP <gripper> must be one top-level line-aligned element");
  }
  const auto close = historical.find("</gripper>", tag);
  if (close == std::string::npos) {
    fail("HPP <gripper> element is not closed");
  }
  auto end = close + std::string_view("</gripper>").size();
  while (end < historical.size() &&
         (historical[end] == ' ' || historical[end] == '\t')) {
    ++end;
  }
  if (end < historical.size() && historical[end] == '\r') {
    ++end;
  }
  if (end < historical.size() && historical[end] == '\n') {
    ++end;
  }

  auto erase_begin = begin;
  if (begin > 0U) {
    const auto previous_line_end = begin - 1U;
    const auto previous_line_break =
        historical.rfind('\n', previous_line_end - 1U);
    const auto previous_line_start = previous_line_break == std::string::npos
                                         ? 0U
                                         : previous_line_break + 1U;
    if (std::all_of(historical.begin() +
                        static_cast<std::ptrdiff_t>(previous_line_start),
                    historical.begin() +
                        static_cast<std::ptrdiff_t>(previous_line_end),
                    [](unsigned char byte) {
                      return byte == ' ' || byte == '\t' || byte == '\r';
                    })) {
      erase_begin = previous_line_start;
    }
  }
  historical.erase(erase_begin, end - erase_begin);
  return historical;
}

void verify_srdf_projection(const FamilySource &family,
                            std::string_view selected_srdf) {
  const std::string historical = read_plain_file(
      family.historical_srdf_source,
      deployment::kMaxModelSourceXmlEntrypointBytes, "historical SRDF");
  if (family.srdf_projection_kind == "identity") {
    if (historical != selected_srdf) {
      fail(family.family_id + " declares a non-identity SRDF projection");
    }
    return;
  }
  if (family.srdf_projection_kind == "remove_hpp_gripper_extension") {
    if (remove_one_hpp_gripper_block(historical) != selected_srdf) {
      fail(family.family_id +
           " structural SRDF differs beyond the exact HPP gripper extension");
    }
    return;
  }
  fail(family.family_id + " selects an unknown SRDF projection kind");
}

void verify_urdf_projection(const FamilySource &family,
                            std::string_view selected_urdf) {
  std::string historical = read_plain_file(
      family.historical_urdf_source,
      deployment::kMaxModelSourceXmlEntrypointBytes, "historical URDF");
  if (family.urdf_projection_kind == "identity") {
    if (historical != selected_urdf) {
      fail(family.family_id + " declares a non-identity URDF projection");
    }
    return;
  }
  if (family.urdf_projection_kind == "remove_unused_xacro_namespace") {
    constexpr std::string_view kNamespace =
        " xmlns:xacro=\"http://www.ros.org/wiki/xacro\"";
    const auto position = historical.find(kNamespace);
    if (position == std::string::npos ||
        historical.find(kNamespace, position + kNamespace.size()) !=
            std::string::npos) {
      fail("unused xacro namespace projection requires one exact declaration");
    }
    historical.erase(position, kNamespace.size());
    if (historical != selected_urdf) {
      fail(family.family_id +
           " structural URDF differs beyond the unused xacro namespace");
    }
    return;
  }
  if (family.urdf_projection_kind == "remove_unsupported_collision_materials") {
    constexpr std::string_view kMaterial = R"urdf(
      <material name="">
        <color rgba="0.9 0.0 0.0 1"/>
      </material>)urdf";
    std::size_t removed = 0U;
    while (true) {
      const auto position = historical.find(kMaterial);
      if (position == std::string::npos) {
        break;
      }
      historical.erase(position, kMaterial.size());
      ++removed;
    }
    if (removed != 2U || historical != selected_urdf) {
      fail(family.family_id +
           " structural URDF differs beyond the two unsupported collision "
           "materials");
    }
    return;
  }
  fail(family.family_id + " selects an unknown URDF projection kind");
}

[[nodiscard]] std::vector<deployment::detail::PreparedModelSourceArtifact>
load_source_artifacts(const FamilySource &family,
                      const std::filesystem::path &prepared_root,
                      const deployment::CanonicalValue &manifest) {
  const auto &prepared =
      require_member(manifest, "prepared_source", "preparation manifest");
  const std::string urdf_uri = require_text(
      require_member(prepared, "urdf_logical_uri", "prepared_source"),
      "prepared_source.urdf_logical_uri");
  const auto &srdf_value =
      require_member(prepared, "srdf_logical_uri", "prepared_source");
  const auto *srdf_uri = std::get_if<std::string>(&srdf_value.storage());
  if (srdf_uri == nullptr) {
    fail("maintained model family requires one prepared SRDF");
  }

  std::map<std::string, std::filesystem::path, std::less<>> paths;
  paths.emplace(urdf_uri, prepared_root / "model.urdf");
  paths.emplace(*srdf_uri, prepared_root / "model.srdf");
  const auto &rewrites = require_array(
      require_member(manifest, "resource_rewrites", "preparation manifest"),
      "preparation manifest.resource_rewrites");
  for (const auto &rewrite : rewrites) {
    const std::string &logical_uri =
        require_text(require_member(rewrite, "asset_uri", "resource rewrite"),
                     "resource rewrite.asset_uri");
    const auto relative = require_safe_relative_path(
        require_text(
            require_member(rewrite, "relative_path", "resource rewrite"),
            "resource rewrite.relative_path"),
        "resource rewrite.relative_path");
    if (!paths.emplace(logical_uri, family.resource_root / relative).second) {
      fail("preparation manifest contains a duplicate artifact path");
    }
  }

  const auto &declarations =
      require_array(require_member(prepared, "artifacts", "prepared_source"),
                    "prepared_source.artifacts");
  if (declarations.empty() ||
      declarations.size() > kMaxMaintainedSourceArtifacts) {
    fail("prepared source artifact count is outside the maintained bound");
  }
  std::vector<deployment::detail::PreparedModelSourceArtifact> artifacts;
  artifacts.reserve(declarations.size());
  std::uint64_t aggregate_bytes = 0U;
  for (const auto &declaration : declarations) {
    const std::string &logical_uri = require_text(
        require_member(declaration, "logical_uri", "prepared artifact"),
        "prepared artifact.logical_uri");
    const std::string &media_type = require_text(
        require_member(declaration, "media_type", "prepared artifact"),
        "prepared artifact.media_type");
    const std::string &expected_digest = require_text(
        require_member(declaration, "content_digest", "prepared artifact"),
        "prepared artifact.content_digest");
    const auto found = paths.find(logical_uri);
    if (found == paths.end()) {
      fail("prepared artifact has no exact source path: " + logical_uri);
    }
    const auto source_size = std::filesystem::file_size(found->second);
    if (source_size >
        deployment::kMaxModelSourceClosureBytes - aggregate_bytes) {
      fail("prepared source closure exceeds the compiler-owned byte bound");
    }
    const std::string bytes = read_plain_file(
        found->second, deployment::kMaxModelSourceOpaqueArtifactBytes,
        "prepared source artifact");
    if (bytes.size() >
        deployment::kMaxModelSourceClosureBytes - aggregate_bytes) {
      fail("prepared source closure changed beyond its byte bound");
    }
    aggregate_bytes += static_cast<std::uint64_t>(bytes.size());
    if (deployment::sha256_digest(bytes) != expected_digest) {
      fail("prepared artifact bytes differ from the preparation receipt: " +
           logical_uri);
    }
    artifacts.push_back(deployment::detail::PreparedModelSourceArtifact{
        logical_uri, media_type, core::ContentDigest(expected_digest), bytes});
  }
  if (artifacts.size() != paths.size()) {
    fail("preparation receipt path/declaration sets are not exact");
  }
  std::sort(artifacts.begin(), artifacts.end(),
            [](const auto &left, const auto &right) {
              return left.logical_uri < right.logical_uri;
            });
  return artifacts;
}

[[nodiscard]] GeneratedFamily
generate_family(const FamilySource &family,
                const std::filesystem::path &prepared_root) {
  const std::string manifest_bytes = read_plain_file(
      prepared_root / "preparation-manifest.json",
      deployment::kMaxArtifactEncodedBytes, "preparation manifest");
  const auto manifest =
      deployment::parse_strict_json_model_yaml(manifest_bytes).root();
  if (deployment::canonical_json(manifest) != manifest_bytes) {
    fail("preparation manifest is not canonical JSON");
  }
  const std::string selected_srdf = read_plain_file(
      family.srdf_source, deployment::kMaxModelSourceXmlEntrypointBytes,
      "selected SRDF source");
  verify_srdf_projection(family, selected_srdf);
  const std::string selected_urdf = read_plain_file(
      family.urdf_source, deployment::kMaxModelSourceXmlEntrypointBytes,
      "selected URDF source");
  verify_urdf_projection(family, selected_urdf);
  auto artifacts = load_source_artifacts(family, prepared_root, manifest);
  auto model = deployment::detail::qualify_activation_prepared_model(
      family.model_asset_id, family.semantics,
      family.accepted_package_uri_prefix, manifest_bytes, selected_urdf,
      selected_srdf, std::move(artifacts));
  return GeneratedFamily{family, std::move(model)};
}

void run(const Options &raw_options) {
  Options options = raw_options;
  options.workspace_root = require_plain_absolute_directory(
      options.workspace_root, "workspace root");
  if (!options.description.is_absolute()) {
    fail("description must be an absolute path");
  }
  for (auto &[unused, root] : options.prepared_roots) {
    (void)unused;
    root = require_plain_absolute_directory(root, "prepared root");
  }

  const auto families = load_families(options);
  bool selected = false;
  std::vector<GeneratedFamily> generated_families;
  generated_families.reserve(families.size());
  for (const auto &family : families) {
    if (options.selected_family.has_value() &&
        family.family_id != *options.selected_family) {
      continue;
    }
    selected = true;
    const auto prepared = options.prepared_roots.find(family.family_id);
    if (prepared == options.prepared_roots.end()) {
      fail("no prepared structural closure was supplied for " +
           family.family_id);
    }
    generated_families.push_back(generate_family(family, prepared->second));
  }
  if (!selected) {
    fail("selected model family does not exist in the maintained description");
  }

  // Validate and qualify every selected family before changing any source.
  // Per-file replacement remains atomic; a later-family validation failure can
  // therefore never leave an earlier catalog refreshed in isolation.
  for (const auto &generated : generated_families) {
    if (options.mode == Mode::kPrint) {
      std::cout << generated.model.catalog_bytes;
      continue;
    }
    if (options.mode == Mode::kUpdate) {
      replace_file_atomically(generated.source.catalog_source,
                              generated.model.catalog_bytes);
      continue;
    }
    const std::string checked_in = read_plain_file(
        generated.source.catalog_source, deployment::kMaxArtifactEncodedBytes,
        "maintained ModelAsset catalog source");
    if (checked_in != generated.model.catalog_bytes) {
      fail(generated.source.family_id +
           " maintained ModelAsset catalog source is stale; run "
           "manyforge_maintained_model_catalog --update");
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    run(parse_options(argc, argv));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "manyforge_maintained_model_catalog: " << error.what() << '\n';
    return 2;
  }
}
