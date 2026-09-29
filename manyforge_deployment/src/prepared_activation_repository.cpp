#include "manyforge_deployment/prepared_activation_repository.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/program_binding_compiler.hpp"
#include "posix_durability_internal.hpp"

namespace manyforge::deployment {
namespace {

namespace posix = detail::posix_durability;

[[noreturn]] void fail(const std::string &message) {
  throw PreparedActivationInputError(message);
}

[[nodiscard]] std::exception_ptr make_error(std::string_view message) {
  return std::make_exception_ptr(
      PreparedActivationInputError(std::string(message)));
}

const posix::ErrorPolicy kPosixErrors{make_error};
constexpr posix::TrustedRootMessages kTrustedRootMessages{
    "prepared-activation trusted root must not be empty",
    "cannot resolve prepared-activation trusted root",
    "prepared-activation trusted root must be an existing plain directory",
};
constexpr posix::DirectoryMessages kDirectoryMessages{
    "cannot inspect prepared-activation repository directory",
    "prepared-activation repository path is not a plain directory",
    "cannot create prepared-activation repository directory",
    "concurrently created prepared-activation repository path is not a plain "
    "directory",
    "created prepared-activation repository directory is unsafe",
};
constexpr posix::DirectorySyncMessages kDirectorySyncMessages{
    "cannot open prepared-activation directory for fsync",
    "cannot fsync prepared-activation directory",
};
constexpr posix::ReadMessages kReadMessages{
    "cannot open prepared activation",
    "cannot stat prepared activation",
    "prepared activation file violates its immutable byte bound",
    "prepared activation file changed during exact read",
    "prepared activation file grew during exact read",
};
constexpr posix::PublicationMessages kPublicationMessages{
    "cannot create prepared-activation temporary file",
    "cannot allocate a unique prepared-activation temporary file",
    "cannot write prepared-activation temporary file",
    "cannot fsync prepared-activation temporary file",
    "cannot close prepared-activation temporary file",
    "cannot publish prepared activation atomically",
    "cannot remove idempotent prepared-activation temporary file",
    "cannot remove prepared-activation temporary link",
};

[[nodiscard]] std::filesystem::path
prepared_path(const std::filesystem::path &root, const core::ContentDigest &id) {
  constexpr std::string_view kPrefix = "sha256:";
  if (!is_sha256_digest(id.value()) || !id.value().starts_with(kPrefix)) {
    fail("prepared activation id is not a canonical SHA-256 identity");
  }
  return root / (id.value().substr(kPrefix.size()) + ".json");
}

/// Exact-key catalog reads from the artifacts carried by the prepared value.
class PreparedPayloadCatalogSource final : public ICatalogArtifactSource {
public:
  explicit PreparedPayloadCatalogSource(
      const std::vector<PreparedActivationArtifact> &artifacts)
      : artifacts_(artifacts) {}

  [[nodiscard]] std::optional<std::string>
  get_verified(const core::ContentAddressedStoreKey &key,
               const core::ContentDigest &digest,
               const std::optional<std::uint64_t> size) const override {
    const auto found =
        std::find_if(artifacts_.begin(), artifacts_.end(), [&](const auto &value) {
          return value.content_addressed_store_key == key;
        });
    if (found == artifacts_.end() || found->content_digest != digest ||
        (size.has_value() && found->bytes.size() != *size) ||
        sha256_digest(found->bytes) != digest.value()) {
      return std::nullopt;
    }
    return found->bytes;
  }

private:
  const std::vector<PreparedActivationArtifact> &artifacts_;
};

/// Records every exact catalog read so the assembled value carries precisely
/// the closure its recompilation needs.
class RecordingCatalogSource final : public ICatalogArtifactSource {
public:
  explicit RecordingCatalogSource(const ICatalogArtifactSource &source)
      : source_(source) {}

  [[nodiscard]] std::optional<std::string>
  get_verified(const core::ContentAddressedStoreKey &key,
               const core::ContentDigest &digest,
               const std::optional<std::uint64_t> size) const override {
    auto bytes = source_.get_verified(key, digest, size);
    if (bytes.has_value() &&
        std::none_of(recorded_.begin(), recorded_.end(), [&](const auto &value) {
          return value.content_addressed_store_key == key;
        })) {
      recorded_.push_back({key, digest, *bytes});
    }
    return bytes;
  }

  [[nodiscard]] const std::vector<PreparedActivationArtifact> &recorded() const {
    return recorded_;
  }

private:
  const ICatalogArtifactSource &source_;
  mutable std::vector<PreparedActivationArtifact> recorded_;
};

[[nodiscard]] std::string base64(std::string_view bytes) {
  static constexpr std::string_view kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve(((bytes.size() + 2U) / 3U) * 4U);
  for (std::size_t index = 0U; index < bytes.size(); index += 3U) {
    const auto remaining = bytes.size() - index;
    const std::uint32_t chunk =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index])) << 16U) |
        (remaining > 1U
             ? static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 1U])) << 8U
             : 0U) |
        (remaining > 2U
             ? static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 2U]))
             : 0U);
    output.push_back(kAlphabet[(chunk >> 18U) & 0x3fU]);
    output.push_back(kAlphabet[(chunk >> 12U) & 0x3fU]);
    output.push_back(remaining > 1U ? kAlphabet[(chunk >> 6U) & 0x3fU] : '=');
    output.push_back(remaining > 2U ? kAlphabet[chunk & 0x3fU] : '=');
  }
  return output;
}

} // namespace

CanonicalValue
prepared_activation_artifact_rows(std::vector<PreparedActivationArtifact> artifacts) {
  std::ranges::sort(artifacts, {}, [](const auto &artifact) {
    return artifact.content_addressed_store_key.value();
  });
  constexpr std::size_t kChunkBytes = 192U * 1024U;
  CanonicalValue::Array rows;
  rows.reserve(artifacts.size());
  for (const auto &artifact : artifacts) {
    if (artifact.bytes.empty()) {
      fail("prepared activation contains an empty artifact");
    }
    CanonicalValue::Array chunks;
    for (std::size_t offset = 0U; offset < artifact.bytes.size();
         offset += kChunkBytes) {
      chunks.emplace_back(base64(std::string_view(artifact.bytes)
                                     .substr(offset, std::min(kChunkBytes,
                                                              artifact.bytes.size() -
                                                                  offset))));
    }
    rows.emplace_back(CanonicalValue::Object{
        {"byte_size", CanonicalValue(static_cast<double>(artifact.bytes.size()))},
        {"chunks", CanonicalValue(std::move(chunks))},
        {"content_addressed_store_key",
         CanonicalValue(artifact.content_addressed_store_key.value())},
        {"content_digest", CanonicalValue(artifact.content_digest.value())},
        {"encoding", CanonicalValue("base64")},
    });
  }
  return CanonicalValue(std::move(rows));
}

PreparedActivationInput assemble_prepared_activation(
    const PreparedActivationAssemblyInput &input, const RetainedBundle &bundle,
    const VerifiedRetainedModelMap &model_map,
    const ProgramBindingContract &program, const CanonicalValue &role_assignment,
    const ICatalogArtifactSource &catalog,
    const IRawArtifactStore &raw_artifacts) {
  return assemble_prepared_activation(
      input, bundle, std::optional<VerifiedRetainedModelMap>{model_map},
      program, role_assignment, catalog, raw_artifacts);
}

PreparedActivationInput assemble_prepared_activation(
    const PreparedActivationAssemblyInput &input, const RetainedBundle &bundle,
    const std::optional<VerifiedRetainedModelMap> &model_map,
    const ProgramBindingContract &program, const CanonicalValue &role_assignment,
    const ICatalogArtifactSource &catalog,
    const IRawArtifactStore &raw_artifacts) {
  const auto &maps = bundle.value().compiled_products.symbol_role_maps;
  if (maps.size() > 1U || maps.empty() != !model_map.has_value()) {
    fail("prepared activation model map differs from candidate products");
  }
  std::vector<PreparedActivationArtifact> raw_artifacts_to_embed;
  if (model_map.has_value()) {
    const auto &map_product = maps.front();
    if (map_product.artifact_refs.size() != 1U ||
        model_map->query().retained_bundle_id != bundle.retained_bundle_id() ||
        model_map->query().product_id != map_product.product_id) {
      fail("prepared activation model map is not the candidate's exact product");
    }
    const auto &map_ref = map_product.artifact_refs.front();
    auto map_bytes = raw_artifacts.get_verified(
        map_ref.content_addressed_store_key, map_ref.content_digest,
        map_ref.byte_size);
    if (!map_bytes.has_value()) {
      fail("prepared activation model-map artifact is unavailable");
    }
    raw_artifacts_to_embed.push_back(
        {map_ref.content_addressed_store_key, map_ref.content_digest,
         std::move(*map_bytes)});
  } else if (!bundle.value().cell_graph.controllable_components.empty() ||
             !bundle.value().cell_graph.model_instances.empty()) {
    fail("prepared activation cannot omit model data for a populated candidate");
  }
  const RecordingCatalogSource recording(catalog);
  const auto outcome = compile_program_binding_outcome(
      program, parse_program_role_assignment(role_assignment), bundle, model_map,
      recording);
  CanonicalValue program_outcome(nullptr);
  CanonicalValue program_binding(nullptr);
  if (outcome.binding != nullptr) {
    program_binding = outcome.binding->normalized_root();
    program_outcome = CanonicalValue(CanonicalValue::Object{
        {"kind", CanonicalValue("bound")},
        {"program_binding_id",
         CanonicalValue(outcome.binding->program_binding_id().value())},
    });
  } else {
    program_outcome = CanonicalValue(CanonicalValue::Object{
        {"kind", CanonicalValue("non_executable")},
        {"outcome", parse_strict_json_model_yaml(EncodeProgramNonExecutableOutcome(
                        *outcome.non_executable))
                        .root()},
    });
  }
  CanonicalValue::Object identity{
      {"activation_payload",
       CanonicalValue(CanonicalValue::Object{
           {"catalog_artifacts",
            prepared_activation_artifact_rows(recording.recorded())},
           {"program", program.normalized_program_root()},
           {"program_binding", std::move(program_binding)},
           {"program_role_assignment", role_assignment},
           {"raw_artifacts",
            prepared_activation_artifact_rows(
                std::move(raw_artifacts_to_embed))},
           {"retained_bundle", bundle.normalized_root()},
       })},
      {"assistant_mode", CanonicalValue(input.assistant_mode)},
      {"captured_input_digest", CanonicalValue(input.captured_input_digest.value())},
      {"deployment_artifact_id",
       CanonicalValue(bundle.value().deployment_artifact_id.value())},
      {"deployment_revision", CanonicalValue(bundle.value().deployment_revision.value())},
      {"graph_revision", CanonicalValue(bundle.value().cell_graph.graph_revision.value())},
      {"node_environment_digest",
       CanonicalValue(bundle.value().catalogs.node_environment.environment_digest.value())},
      {"program_id", CanonicalValue(program.value().program_id.value())},
      {"program_outcome", std::move(program_outcome)},
      {"program_revision", CanonicalValue(program.value().program_revision.value())},
      {"retained_bundle_id", CanonicalValue(bundle.retained_bundle_id().value())},
      {"scenario_id", CanonicalValue(input.scenario_id)},
      {"schema_version", CanonicalValue(std::string(kPreparedActivationInputSchemaVersion))},
      {"source_document_digest", CanonicalValue(input.source_document_digest.value())},
      {"typed_parameter_overrides", input.typed_parameter_overrides},
  };
  const auto prepared_id =
      sha256_digest(canonical_json(CanonicalValue(identity)));
  identity.emplace_back("prepared_activation_id", CanonicalValue(prepared_id));
  auto prepared = parse_prepared_activation_input(CanonicalValue(std::move(identity)));
  verify_prepared_program_outcome(prepared);
  return prepared;
}

void verify_prepared_program_outcome(const PreparedActivationInput &prepared) {
  static_cast<void>(recompile_prepared_program_outcome(prepared));
}

ProgramBindingOutcome
recompile_prepared_program_outcome(const PreparedActivationInput &prepared) {
  const auto &closure = prepared.closure();
  // The structural verifier proved exact absence of artifacts, identities,
  // assignments and overrides. There is no Program to compile or Scene to project.
  if (closure.program == nullptr) return {};
  const PreparedPayloadCatalogSource catalog(closure.catalog_artifacts);
  ProgramBindingOutcome recompiled;
  try {
    recompiled = compile_program_binding_outcome(
        *closure.program, prepared.program_role_assignment(),
        *closure.retained_bundle, closure.retained_model_map, catalog);
  } catch (const ProgramBindingCompilerError &error) {
    fail(std::string("prepared Program outcome does not recompile: ") +
         error.what());
  }
  const bool bound_matches =
      prepared.program_binding_id().has_value() && recompiled.binding != nullptr &&
      closure.program_binding != nullptr &&
      recompiled.binding->program_binding_id() == *prepared.program_binding_id() &&
      closure.program_binding->program_binding_id() ==
          *prepared.program_binding_id();
  const bool unbound_matches =
      prepared.program_outcome().has_value() &&
      recompiled.non_executable.has_value() &&
      *recompiled.non_executable == *prepared.program_outcome();
  if (!bound_matches && !unbound_matches) {
    fail("prepared Program outcome differs from its recompilation against the "
         "carried candidate");
  }
  return recompiled;
}

FilesystemPreparedActivationRepository::FilesystemPreparedActivationRepository(
    std::filesystem::path trusted_root)
    : root_(posix::normalize_existing_plain_root(trusted_root, kPosixErrors,
                                                 kTrustedRootMessages) /
            "prepared-activations") {}

bool FilesystemPreparedActivationRepository::put_if_absent(
    const PreparedActivationInput &prepared) const {
  posix::ensure_owned_plain_directory(root_, kPosixErrors, kDirectoryMessages,
                                      kDirectorySyncMessages);
  const auto bytes = canonical_json(prepared.normalized_root());
  if (bytes.size() > kMaxPreparedActivationBytes) {
    fail("prepared activation exceeds its stored byte bound");
  }
  return posix::publish_immutable_file_no_replace(
      prepared_path(root_, prepared.prepared_activation_id()), bytes,
      kMaxPreparedActivationBytes, kPosixErrors, kReadMessages,
      kDirectorySyncMessages, kPublicationMessages,
      [&bytes](const std::string_view existing) {
        if (existing != bytes) {
          fail("prepared activation identity collision");
        }
      });
}

std::shared_ptr<const PreparedActivationInput>
FilesystemPreparedActivationRepository::get_verified(
    const core::ContentDigest &prepared_activation_id) const {
  const auto bytes = posix::read_exact_regular_file(
      prepared_path(root_, prepared_activation_id), kMaxPreparedActivationBytes,
      true, kPosixErrors, kReadMessages);
  if (!bytes.has_value()) {
    return nullptr;
  }
  // One prepared activation is an aggregate transport/artifact: its payload
  // can contain many individually bounded model resources. Preserve the
  // prepared-activation aggregate bound instead of applying the 16 MiB limit
  // for a single authored artifact.
  const auto root = parse_strict_json_model_yaml(
                        *bytes,
                        static_cast<std::size_t>(kMaxPreparedActivationBytes))
                        .root();
  if (canonical_json(root) != *bytes) {
    fail("stored prepared activation is not canonical");
  }
  auto prepared = std::make_shared<const PreparedActivationInput>(
      parse_prepared_activation_input(root));
  if (prepared->prepared_activation_id() != prepared_activation_id) {
    fail("stored prepared activation has another identity");
  }
  verify_prepared_program_outcome(*prepared);
  return prepared;
}

} // namespace manyforge::deployment
