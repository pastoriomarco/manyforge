#include "canonical_empty_admission_internal.hpp"

#include <exception>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "build_manifest_internal.hpp"
#include "manyforge_deployment/build_manifest.hpp"
#include "manyforge_deployment/cell_applicability.hpp"
#include "manyforge_deployment/cell_compiler.hpp"
#include "manyforge_deployment/cell_composition.hpp"
#include "manyforge_deployment/retention_qualified_deployment.hpp"
#include "manyforge_deployment/site_overlay_resolution.hpp"
#include "posix_durability_internal.hpp"
#include "retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

namespace posix = posix_durability;

constexpr std::string_view kAdmissionFilename =
    "canonical-empty-admission.json";

[[noreturn]] void fail(std::string_view message) {
  throw CanonicalEmptyAdmissionError(std::string(message));
}

[[nodiscard]] std::exception_ptr make_admission_error(
    std::string_view message) {
  return std::make_exception_ptr(
      CanonicalEmptyAdmissionError(std::string(message)));
}

const posix::ErrorPolicy kPosixErrors{make_admission_error};
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

[[nodiscard]] RetentionQualifiedDeployment compile_fixed_deployment(
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

}  // namespace

CanonicalEmptyRetainedBundleAdmission generate_canonical_empty_package(
    const std::filesystem::path& output_root) {
  try {
    const std::filesystem::path root =
        normalize_canonical_empty_package_root(output_root);

    // A visible commit record always wins. It is verified before any
    // repository constructor or other write-capable operation is reached.
    if (read_canonical_empty_admission_if_present(root).has_value()) {
      return FilesystemCanonicalEmptyRetainedBundleSource(root)
          .load_verified();
    }

    const RevisionVerifiedDeploymentArtifact deployment =
        load_canonical_empty_fixed_deployment();
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
    require_canonical_empty_bundle_matches_authorities(
        *independently_reloaded, deployment, context.node_environment(),
        context.compiler_contract_digest(),
        context.compiler_build_digest(), nullptr);

    CanonicalEmptyAdmissionRecord record =
        make_canonical_empty_admission_record(*independently_reloaded);
    const std::filesystem::path admission_path =
        root / std::string(kAdmissionFilename);
    (void)posix::publish_immutable_file_no_replace(
        admission_path, record.canonical_bytes,
        kMaxCanonicalEmptyAdmissionBytes, kPosixErrors,
        kAdmissionReadMessages, kAdmissionDirectorySyncMessages,
        kAdmissionPublicationMessages,
        [&record](std::string_view existing) {
          const CanonicalEmptyAdmissionRecord existing_record =
              parse_canonical_empty_admission_record(existing);
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

}  // namespace manyforge::deployment::detail
