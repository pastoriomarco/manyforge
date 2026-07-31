#include "manyforge_deployment/canonical_empty_admission.hpp"

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "build_manifest_internal.hpp"
#include "canonical_empty_admission_internal.hpp"
#include "content_identity_internal.hpp"
#include "embedded_canonical_empty_deployment.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/build_manifest.hpp"
#include "manyforge_deployment/cell_applicability.hpp"
#include "manyforge_deployment/cell_compiler.hpp"
#include "manyforge_deployment/cell_composition.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/deployment_artifact_codec.hpp"
#include "manyforge_deployment/retention_qualified_deployment.hpp"
#include "manyforge_deployment/site_overlay_resolution.hpp"
#include "posix_durability_internal.hpp"
#include "retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

namespace posix = detail::posix_durability;

constexpr std::string_view kCanonicalDeploymentArtifactId =
    "c1_build_authority";
constexpr std::string_view kCanonicalCellId =
    "c1_build_authority_cell";
constexpr std::string_view kAdmissionFilename =
    "canonical-empty-admission.json";
constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

[[noreturn]] void fail(std::string_view message) {
  throw CanonicalEmptyAdmissionError(std::string(message));
}

[[nodiscard]] std::exception_ptr make_admission_error(
    std::string_view message) {
  return std::make_exception_ptr(
      CanonicalEmptyAdmissionError(std::string(message)));
}

const posix::ErrorPolicy kPosixErrors{make_admission_error};
constexpr posix::TrustedRootMessages kTrustedRootMessages{
    "canonical-empty package root must not be empty",
    "cannot resolve canonical-empty package root",
    "canonical-empty package root must be an existing plain directory",
};
constexpr posix::ReadMessages kAdmissionReadMessages{
    "cannot open canonical-empty admission record",
    "cannot stat canonical-empty admission record",
    "canonical-empty admission record violates its 16 KiB bound",
    "canonical-empty admission record changed during exact read",
    "canonical-empty admission record grew during exact read",
};
constexpr posix::DirectorySyncMessages kAdmissionDirectorySyncMessages{
    "cannot open canonical-empty package root for fsync",
    "cannot fsync canonical-empty package root",
};
constexpr posix::PublicationMessages kAdmissionPublicationMessages{
    "cannot create canonical-empty admission temporary file",
    "cannot allocate a unique canonical-empty admission temporary file",
    "cannot write canonical-empty admission temporary file",
    "cannot fsync canonical-empty admission temporary file",
    "cannot close canonical-empty admission temporary file",
    "cannot publish canonical-empty admission atomically",
    "cannot remove idempotent canonical-empty admission temporary file",
    "cannot remove canonical-empty admission temporary link",
};

[[nodiscard]] std::filesystem::path normalize_package_root(
    const std::filesystem::path& root) {
  try {
    return posix::normalize_existing_plain_root(
        root, kPosixErrors, kTrustedRootMessages);
  } catch (const CanonicalEmptyAdmissionError&) {
    throw;
  } catch (const std::exception& error) {
    fail(std::string("canonical-empty package root is unavailable: ") +
         error.what());
  }
}

[[nodiscard]] const CanonicalValue::Object& require_object(
    const CanonicalValue& value, std::string_view field) {
  const auto* object =
      std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(std::string(field) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue::Object& object, std::string_view name,
    std::string_view field) {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  if (found == object.end()) {
    fail(std::string(field) + " is missing required member " +
         std::string(name));
  }
  return found->second;
}

[[nodiscard]] const std::string& require_string(
    const CanonicalValue& value, std::string_view field) {
  const auto* string = std::get_if<std::string>(&value.storage());
  if (string == nullptr) {
    fail(std::string(field) + " must be a string");
  }
  return *string;
}

void require_exact_fields(
    const CanonicalValue::Object& object,
    std::initializer_list<std::string_view> expected,
    std::string_view field) {
  if (object.size() != expected.size()) {
    fail(std::string(field) + " has an incorrect closed field count");
  }
  for (const auto& [name, unused] : object) {
    (void)unused;
    if (std::find(expected.begin(), expected.end(), name) ==
        expected.end()) {
      fail(std::string(field) + " contains unknown member " + name);
    }
  }
  for (const std::string_view name : expected) {
    (void)require_member(object, name, field);
  }
}

void require_digest(std::string_view value, std::string_view field) {
  if (!is_sha256_digest(value)) {
    fail(std::string(field) +
         " must be an exact lowercase SHA-256 digest");
  }
}

struct CanonicalEmptyAdmissionRecord final {
  std::string canonical_bytes;
  std::string admission_digest;
  std::string cell_id;
  std::string deployment_artifact_id;
  std::string deployment_revision;
  std::string graph_revision;
  std::string retained_bundle_id;
  std::string environment_digest;
  std::string compiler_contract_digest;
  std::string compiler_build_digest;
};

[[nodiscard]] detail::ContentIdentitySpec admission_identity_spec() {
  return {
      .identity_domain = "canonical-empty-admission",
      .api_version =
          std::string(kCanonicalEmptyAdmissionApiVersion),
      .own_field = "admission_digest",
  };
}

[[nodiscard]] CanonicalValue admission_projection(
    std::string_view admission_digest,
    const RetainedBundle& bundle) {
  const auto& value = bundle.value();
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kCanonicalEmptyAdmissionApiVersion))},
      {"admission_digest",
       CanonicalValue(std::string(admission_digest))},
      {"cell_id", CanonicalValue(value.cell_graph.cell_id.value())},
      {"deployment_artifact_id",
       CanonicalValue(value.deployment_artifact_id.value())},
      {"deployment_revision",
       CanonicalValue(value.deployment_revision.value())},
      {"graph_revision",
       CanonicalValue(value.cell_graph.graph_revision.value())},
      {"retained_bundle_id",
       CanonicalValue(value.retained_bundle_id.value())},
      {"environment_digest",
       CanonicalValue(
           value.catalogs.node_environment.environment_digest.value())},
      {"compiler_contract_digest",
       CanonicalValue(
           value.provenance.compiler_contract_digest.value())},
      {"compiler_build_digest",
       CanonicalValue(value.provenance.compiler_build_digest.value())},
  });
}

[[nodiscard]] CanonicalEmptyAdmissionRecord make_admission_record(
    const RetainedBundle& bundle) {
  CanonicalValue placeholder =
      admission_projection(kZeroDigest, bundle);
  const std::string digest =
      detail::compute_content_identity(
          placeholder, admission_identity_spec());
  CanonicalValue root = admission_projection(digest, bundle);
  std::string bytes = canonical_json(root);
  if (bytes.size() > kMaxCanonicalEmptyAdmissionBytes) {
    throw std::logic_error(
        "build-owned canonical-empty admission exceeds its fixed bound");
  }
  return {
      std::move(bytes),
      digest,
      bundle.value().cell_graph.cell_id.value(),
      bundle.value().deployment_artifact_id.value(),
      bundle.value().deployment_revision.value(),
      bundle.value().cell_graph.graph_revision.value(),
      bundle.retained_bundle_id().value(),
      bundle.value().catalogs.node_environment.environment_digest.value(),
      bundle.value().provenance.compiler_contract_digest.value(),
      bundle.value().provenance.compiler_build_digest.value(),
  };
}

[[nodiscard]] CanonicalEmptyAdmissionRecord parse_admission_record(
    std::string_view bytes) {
  if (bytes.empty() ||
      bytes.size() > kMaxCanonicalEmptyAdmissionBytes) {
    fail("canonical-empty admission record violates its 16 KiB bound");
  }
  const ParsedArtifactCandidate parsed =
      parse_strict_json_model_yaml(bytes);
  CanonicalValue root = parsed.root();
  const auto& object =
      require_object(root, "canonical_empty_admission");
  require_exact_fields(
      object,
      {"api_version", "admission_digest", "cell_id",
       "deployment_artifact_id", "deployment_revision",
       "graph_revision", "retained_bundle_id",
       "environment_digest", "compiler_contract_digest",
       "compiler_build_digest"},
      "canonical_empty_admission");

  const auto field = [&object](std::string_view name)
      -> const std::string& {
    return require_string(
        require_member(object, name, "canonical_empty_admission"),
        std::string("canonical_empty_admission.") +
            std::string(name));
  };
  const std::string& api_version = field("api_version");
  if (api_version != kCanonicalEmptyAdmissionApiVersion) {
    fail("canonical-empty admission api_version is not exact v1");
  }
  const std::string& admission_digest = field("admission_digest");
  const std::string& cell_id = field("cell_id");
  const std::string& deployment_artifact_id =
      field("deployment_artifact_id");
  const std::string& deployment_revision =
      field("deployment_revision");
  const std::string& graph_revision = field("graph_revision");
  const std::string& retained_bundle_id =
      field("retained_bundle_id");
  const std::string& environment_digest =
      field("environment_digest");
  const std::string& compiler_contract_digest =
      field("compiler_contract_digest");
  const std::string& compiler_build_digest =
      field("compiler_build_digest");

  for (const auto& [value, name] :
       {std::pair<std::string_view, std::string_view>{
            admission_digest, "admission_digest"},
        {deployment_revision, "deployment_revision"},
        {graph_revision, "graph_revision"},
        {retained_bundle_id, "retained_bundle_id"},
        {environment_digest, "environment_digest"},
        {compiler_contract_digest, "compiler_contract_digest"},
        {compiler_build_digest, "compiler_build_digest"}}) {
    require_digest(
        value,
        std::string("canonical_empty_admission.") +
            std::string(name));
  }
  if (!detail::verify_content_identity(
          root, admission_identity_spec())) {
    fail("canonical-empty admission digest does not match its exact "
         "record");
  }
  const std::string canonical_bytes = canonical_json(root);
  if (canonical_bytes != bytes) {
    fail("canonical-empty admission record is not exact RFC 8785 JSON");
  }
  return {
      canonical_bytes,
      std::string(admission_digest),
      std::string(cell_id),
      std::string(deployment_artifact_id),
      std::string(deployment_revision),
      std::string(graph_revision),
      std::string(retained_bundle_id),
      std::string(environment_digest),
      std::string(compiler_contract_digest),
      std::string(compiler_build_digest),
  };
}

[[nodiscard]] RevisionVerifiedDeploymentArtifact
load_fixed_deployment() {
  const auto deployment = verify_deployment_artifact_revision(
      parse_strict_json_model_yaml(
          detail::kEmbeddedCanonicalEmptyDeployment),
      std::span<const QualifiedRuntimeProfile>{});
  if (deployment.deployment_artifact_id().value() !=
          kCanonicalDeploymentArtifactId ||
      deployment.cell_id().value() != kCanonicalCellId) {
    throw std::logic_error(
        "embedded canonical-empty deployment has the wrong fixed identity");
  }
  return deployment;
}

[[nodiscard]] RetentionQualifiedDeployment
compile_fixed_deployment(
    const RevisionVerifiedDeploymentArtifact& deployment) {
  const CompositionQualifiedCellIr composition =
      compose_cell_occurrences(
          deployment.graph(),
          std::span<const SourceVerifiedModelAsset>{},
          std::span<const QualifiedComponentType>{});
  const QualifiedSelectedSiteOverlay selected_overlay =
      select_site_overlay_for_deployment(
          deployment.applicability_inputs().value().site_overlay_ref,
          deployment.applicability_inputs().value().external_resources,
          nullptr);
  const ApplicabilityQualifiedCellIr applicability =
      qualify_cell_applicability(
          composition, deployment.applicability_inputs(),
          std::span<const QualifiedRuntimeProfile>{},
          std::span<const QualifiedCapabilityDefinition>{},
          std::span<const QualifiedControlInterfaceContract>{},
          std::span<const QualifiedControllerTopology>{},
          std::span<const QualifiedProviderManifest>{},
          std::span<const QualifiedStopEvidencePolicy>{},
          selected_overlay);
  return qualify_deployment_for_retention(
      deployment, compile_cell_ir(applicability));
}

void require_bundle_matches_authorities(
    const RetainedBundle& bundle,
    const RevisionVerifiedDeploymentArtifact& deployment,
    const detail::BuildQualifiedRetentionContext& context,
    const CanonicalEmptyAdmissionRecord* record) {
  const auto& value = bundle.value();
  if (!verify_retained_bundle_identity(bundle) ||
      bundle.retained_bundle_id() != value.retained_bundle_id ||
      value.deployment_artifact_id !=
          deployment.deployment_artifact_id() ||
      value.deployment_revision != deployment.deployment_revision() ||
      value.cell_graph != deployment.graph().graph() ||
      value.cell_graph.cell_id != deployment.cell_id() ||
      value.catalogs.node_environment != context.node_environment() ||
      value.provenance.compiler_contract_digest !=
          context.compiler_contract_digest() ||
      value.provenance.compiler_build_digest !=
          context.compiler_build_digest()) {
    fail("canonical-empty retained bundle differs from its fixed source or "
         "current build authority");
  }
  if (record == nullptr) {
    return;
  }
  if (record->cell_id != value.cell_graph.cell_id.value() ||
      record->deployment_artifact_id !=
          value.deployment_artifact_id.value() ||
      record->deployment_revision !=
          value.deployment_revision.value() ||
      record->graph_revision !=
          value.cell_graph.graph_revision.value() ||
      record->retained_bundle_id !=
          value.retained_bundle_id.value() ||
      record->environment_digest !=
          value.catalogs.node_environment.environment_digest.value() ||
      record->compiler_contract_digest !=
          value.provenance.compiler_contract_digest.value() ||
      record->compiler_build_digest !=
          value.provenance.compiler_build_digest.value()) {
    fail("canonical-empty admission record cross-binds unequal package "
         "identities");
  }
}

[[nodiscard]] std::optional<std::string>
read_admission_if_present(const std::filesystem::path& root) {
  return posix::read_exact_regular_file(
      root / std::string(kAdmissionFilename),
      kMaxCanonicalEmptyAdmissionBytes, true, kPosixErrors,
      kAdmissionReadMessages);
}

}  // namespace

FilesystemCanonicalEmptyRetainedBundleSource::
    FilesystemCanonicalEmptyRetainedBundleSource(
        std::filesystem::path trusted_package_root)
    : trusted_package_root_(
          normalize_package_root(trusted_package_root)) {}

CanonicalEmptyRetainedBundleAdmission
FilesystemCanonicalEmptyRetainedBundleSource::load_verified() const {
  try {
    const std::filesystem::path current_root =
        normalize_package_root(trusted_package_root_);
    if (current_root != trusted_package_root_) {
      fail("canonical-empty package root changed after construction");
    }

    const auto bytes = read_admission_if_present(current_root);
    if (!bytes.has_value()) {
      fail("canonical-empty admission record is missing");
    }
    CanonicalEmptyAdmissionRecord record =
        parse_admission_record(*bytes);
    if (record.cell_id != kCanonicalCellId ||
        record.deployment_artifact_id !=
            kCanonicalDeploymentArtifactId) {
      fail("canonical-empty admission does not identify the fixed "
           "build-owned source");
    }

    const RevisionVerifiedDeploymentArtifact deployment =
        load_fixed_deployment();
    if (record.deployment_revision !=
            deployment.deployment_revision().value() ||
        record.graph_revision !=
            deployment.graph().graph().graph_revision.value()) {
      fail("canonical-empty admission differs from the exact embedded "
           "deployment");
    }

    const detail::BuildQualifiedRetentionContext context =
        detail::BuildQualifiedRetentionAuthority::qualify_existing(
            current_root);
    FilesystemVerifiedRetainedBundleSource bundle_source(current_root);
    const auto bundle = bundle_source.get_verified(
        core::RetainedBundleId(record.retained_bundle_id));
    if (bundle == nullptr) {
      fail("canonical-empty retained bundle is missing");
    }
    require_bundle_matches_authorities(
        *bundle, deployment, context, &record);

    return CanonicalEmptyRetainedBundleAdmission(
        core::ContentDigest(std::move(record.admission_digest)),
        core::CellId(std::move(record.cell_id)),
        core::DeploymentArtifactId(
            std::move(record.deployment_artifact_id)),
        core::DeploymentRevision(
            std::move(record.deployment_revision)),
        core::GraphRevision(std::move(record.graph_revision)),
        core::RetainedBundleId(
            std::move(record.retained_bundle_id)),
        core::ContentDigest(
            std::move(record.environment_digest)),
        core::ContentDigest(
            std::move(record.compiler_contract_digest)),
        core::ContentDigest(
            std::move(record.compiler_build_digest)),
        std::move(bundle), std::move(record.canonical_bytes));
  } catch (const CanonicalEmptyAdmissionError&) {
    throw;
  } catch (const std::exception& error) {
    fail(std::string("canonical-empty package verification failed: ") +
         error.what());
  }
}

namespace detail {

CanonicalEmptyRetainedBundleAdmission
generate_canonical_empty_package(
    const std::filesystem::path& output_root) {
  try {
    const std::filesystem::path root =
        normalize_package_root(output_root);

    // A visible commit record always wins. It is verified before any
    // repository constructor or other write-capable operation is reached.
    if (read_admission_if_present(root).has_value()) {
      return FilesystemCanonicalEmptyRetainedBundleSource(root)
          .load_verified();
    }

    const RevisionVerifiedDeploymentArtifact deployment =
        load_fixed_deployment();
    const RetentionQualifiedDeployment qualified =
        compile_fixed_deployment(deployment);

    FilesystemBuildManifestRepository manifest_repository(root);
    FilesystemRawArtifactStore artifact_store(root);
    FilesystemRetainedBundleRepository bundle_repository(root);
    const auto produced =
        RetainedBundleMaterializer::
            materialize_store_and_reload_production(
                qualified, manifest_repository, artifact_store,
                bundle_repository);
    if (produced == nullptr) {
      throw std::logic_error(
          "canonical-empty materializer returned null");
    }

    const BuildQualifiedRetentionContext context =
        BuildQualifiedRetentionAuthority::qualify_existing(root);
    FilesystemVerifiedRetainedBundleSource bundle_source(root);
    const auto independently_reloaded =
        bundle_source.get_verified(produced->retained_bundle_id());
    if (independently_reloaded == nullptr ||
        independently_reloaded->canonical_bytes() !=
            produced->canonical_bytes()) {
      fail("canonical-empty generator did not independently reload its "
           "exact package closure");
    }
    require_bundle_matches_authorities(
        *independently_reloaded, deployment, context, nullptr);

    CanonicalEmptyAdmissionRecord record =
        make_admission_record(*independently_reloaded);
    const std::filesystem::path admission_path =
        root / std::string(kAdmissionFilename);
    (void)posix::publish_immutable_file_no_replace(
        admission_path, record.canonical_bytes,
        kMaxCanonicalEmptyAdmissionBytes, kPosixErrors,
        kAdmissionReadMessages, kAdmissionDirectorySyncMessages,
        kAdmissionPublicationMessages,
        [&record](std::string_view existing) {
          const CanonicalEmptyAdmissionRecord existing_record =
              parse_admission_record(existing);
          if (existing != record.canonical_bytes ||
              existing_record.admission_digest !=
                  record.admission_digest) {
            fail("canonical-empty admission publication collided with a "
                 "different immutable record");
          }
        });

    return FilesystemCanonicalEmptyRetainedBundleSource(root)
        .load_verified();
  } catch (const CanonicalEmptyAdmissionError&) {
    throw;
  } catch (const std::exception& error) {
    fail(std::string("canonical-empty package generation failed: ") +
         error.what());
  }
}

}  // namespace detail
}  // namespace manyforge::deployment
