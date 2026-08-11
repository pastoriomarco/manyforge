#include "canonical_empty_admission_internal.hpp"

#include <algorithm>
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
#include "manyforge_deployment/native_execution_runtime_contract.hpp"
#include "manyforge_deployment/retention_qualified_deployment.hpp"
#include "manyforge_deployment/site_overlay_resolution.hpp"
#include "posix_durability_internal.hpp"
#include "retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

namespace posix = posix_durability;

constexpr std::string_view kAdmissionFilename =
    "canonical-empty-admission.json";
constexpr std::string_view kPlaceholderDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

[[noreturn]] void fail(std::string_view message) {
  throw CanonicalEmptyAdmissionError(std::string(message));
}

[[nodiscard]] std::exception_ptr
make_admission_error(std::string_view message) {
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

[[nodiscard]] const CanonicalValue &
require_member(const CanonicalValue &value, const std::string_view name,
               const std::string_view label) {
  const auto *member = value.find_object_member(name);
  if (member == nullptr) {
    fail(std::string(label) + " is missing " + std::string(name));
  }
  return *member;
}

[[nodiscard]] const CanonicalValue::Array &
require_array(const CanonicalValue &value, const std::string_view label) {
  const auto *array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(std::string(label) + " must be an array");
  }
  return *array;
}

[[nodiscard]] std::filesystem::path
require_root_local_filename(const std::filesystem::path &filename) {
  const std::string text = filename.generic_string();
  if (text.empty() || filename.is_absolute() ||
      filename != filename.filename() || text == "." || text == ".." ||
      text.size() > 240U ||
      !std::all_of(text.begin(), text.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' ||
               character == '_' || character == '.';
      })) {
    fail("canonical-empty admission output must be one bounded root-local "
         "filename");
  }
  return filename;
}

[[nodiscard]] RetentionQualifiedDeployment
compile_fixed_deployment(const RevisionVerifiedDeploymentArtifact &deployment) {
  const CompositionQualifiedCellIr composition = compose_cell_occurrences(
      deployment.graph(), std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});
  const QualifiedSelectedSiteOverlay selected_overlay =
      select_site_overlay_for_deployment(
          deployment.applicability_inputs().value().site_overlay_ref,
          deployment.applicability_inputs().value().external_resources,
          nullptr);
  const ApplicabilityQualifiedCellIr applicability = qualify_cell_applicability(
      composition, deployment.applicability_inputs(),
      std::span<const QualifiedRuntimeProfile>{},
      std::span<const QualifiedCommandSchema>{},
      std::span<const QualifiedCapabilityDefinition>{},
      std::span<const QualifiedControlInterfaceContract>{},
      std::span<const QualifiedControllerTopology>{},
      std::span<const QualifiedProviderManifest>{},
      std::span<const QualifiedStopEvidencePolicy>{},
      std::span<const BuildQualifiedValueSchemaEntry>{},
      std::span<const QualifiedCommissioningEvidenceRef>{},
      selected_overlay);
  return qualify_deployment_for_retention(deployment,
                                          compile_cell_ir(applicability));
}

[[nodiscard]] RevisionVerifiedDeploymentArtifact
derive_cell_scoped_canonical_empty_deployment_impl(
    const RevisionVerifiedDeploymentArtifact &first_deployment,
    const std::span<const QualifiedRuntimeProfile> qualified_runtime_profiles) {
  const auto &graph = first_deployment.graph().graph();
  if (graph.runtime_units.size() != 1U ||
      !graph.runtime_units.front().dependency_runtime_unit_ids.empty() ||
      graph.runtime_units.front().runtime_profile_id.value() !=
          native_execution_runtime_contract::kRuntimeProfileId) {
    fail("cell-scoped canonical-empty derivation requires one independent "
         "native execution host runtime");
  }
  const auto &runtime = graph.runtime_units.front();
  const auto profile = std::find_if(
      qualified_runtime_profiles.begin(), qualified_runtime_profiles.end(),
      [&](const auto &candidate) {
        return candidate.value().runtime_profile_id ==
                   runtime.runtime_profile_id &&
               candidate.value().runtime_profile_revision ==
                   runtime.runtime_profile_revision;
      });
  if (profile == qualified_runtime_profiles.end()) {
    fail("cell-scoped canonical-empty derivation lacks the exact qualified "
         "runtime profile");
  }

  const auto &authored = first_deployment.normalized_authored_root();
  const auto &catalog_records = require_array(
      require_member(require_member(authored, "catalogs", "deployment"),
                     "records", "deployment.catalogs"),
      "deployment.catalogs.records");
  if (catalog_records.size() != first_deployment.catalog_records().size()) {
    fail("cell-scoped canonical-empty derivation found a divergent catalog "
         "projection");
  }
  CanonicalValue::Array selected_catalog_records;
  for (std::size_t index = 0U;
       index < first_deployment.catalog_records().size(); ++index) {
    const auto *record = std::get_if<core::RuntimeProfileCatalogRecordRefDTO>(
        &first_deployment.catalog_records()[index]);
    if (record != nullptr && record->record_id == runtime.runtime_profile_id &&
        record->record_revision == runtime.runtime_profile_revision) {
      selected_catalog_records.push_back(catalog_records[index]);
    }
  }
  if (selected_catalog_records.size() != 1U) {
    fail("cell-scoped canonical-empty derivation did not select exactly one "
         "runtime-profile source record");
  }

  const auto &authored_runtime_units =
      require_array(require_member(first_deployment.normalized_authored_graph(),
                                   "runtime_units", "deployment.graph"),
                    "deployment.graph.runtime_units");
  if (authored_runtime_units.size() != 1U) {
    fail("cell-scoped canonical-empty derivation found a divergent runtime "
         "projection");
  }

  const std::string deployment_id =
      first_deployment.deployment_artifact_id().value() + ".canonical_empty";
  CanonicalValue::Object root{
      {"api_version", CanonicalValue(std::string("manyforge.deployment.v1"))},
      {"deployment_artifact_id", CanonicalValue(deployment_id)},
      {"deployment_revision", CanonicalValue(std::string(kPlaceholderDigest))},
      {"cell_id", CanonicalValue(first_deployment.cell_id().value())},
      {"name", CanonicalValue(deployment_id)},
      {"catalogs",
       CanonicalValue(CanonicalValue::Object{
           {"records", CanonicalValue(std::move(selected_catalog_records))},
       })},
      {"graph",
       CanonicalValue(CanonicalValue::Object{
           {"runtime_units", CanonicalValue(CanonicalValue::Array{
                                 authored_runtime_units.front(),
                             })},
           {"model_instances", CanonicalValue(CanonicalValue::Array{})},
           {"controllable_components", CanonicalValue(CanonicalValue::Array{})},
           {"control_domains", CanonicalValue(CanonicalValue::Array{})},
           {"control_bindings", CanonicalValue(CanonicalValue::Array{})},
           {"component_attachments", CanonicalValue(CanonicalValue::Array{})},
       })},
      {"desired_control_configurations",
       CanonicalValue(CanonicalValue::Array{})},
      {"capability_provider_bindings", CanonicalValue(CanonicalValue::Array{})},
  };
  if (const auto *compatibility = authored.find_object_member("compatibility");
      compatibility != nullptr) {
    root.emplace_back("compatibility", *compatibility);
  }
  return compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(
          canonical_json(CanonicalValue(std::move(root)))),
      qualified_runtime_profiles);
}

} // namespace

RevisionVerifiedDeploymentArtifact
derive_cell_scoped_canonical_empty_deployment(
    const RevisionVerifiedDeploymentArtifact &first_deployment,
    const std::span<const QualifiedRuntimeProfile> qualified_runtime_profiles) {
  try {
    return derive_cell_scoped_canonical_empty_deployment_impl(
        first_deployment, qualified_runtime_profiles);
  } catch (const CanonicalEmptyAdmissionError &) {
    throw;
  } catch (const std::exception &error) {
    fail(std::string("cell-scoped canonical-empty derivation failed: ") +
         error.what());
  }
}

CanonicalEmptyRetainedBundleAdmission
publish_cell_scoped_canonical_empty_admission(
    const std::filesystem::path &package_root,
    const std::filesystem::path &admission_filename,
    const RetainedBundle &canonical_empty_bundle) {
  try {
    const std::filesystem::path root =
        normalize_canonical_empty_package_root(package_root);
    const auto filename = require_root_local_filename(admission_filename);
    if (filename != cell_scoped_canonical_empty_admission_filename(
                        canonical_empty_bundle.value().cell_graph.cell_id)) {
      fail("cell-scoped canonical-empty admission filename does not match its "
           "cell identity");
    }
    CanonicalEmptyAdmissionRecord record =
        make_canonical_empty_admission_record(canonical_empty_bundle);
    (void)posix::publish_immutable_file_no_replace(
        root / filename, record.canonical_bytes,
        kMaxCanonicalEmptyAdmissionBytes, kPosixErrors, kAdmissionReadMessages,
        kAdmissionDirectorySyncMessages, kAdmissionPublicationMessages,
        [&record](const std::string_view existing) {
          const CanonicalEmptyAdmissionRecord existing_record =
              parse_canonical_empty_admission_record(existing);
          if (existing != record.canonical_bytes ||
              existing_record.admission_digest != record.admission_digest) {
            fail("cell-scoped canonical-empty admission publication collided "
                 "with a different immutable record");
          }
        });
    return FilesystemCanonicalEmptyRetainedBundleSource(root, filename)
        .load_verified();
  } catch (const CanonicalEmptyAdmissionError &) {
    throw;
  } catch (const std::exception &error) {
    fail(std::string("cell-scoped canonical-empty admission publication "
                     "failed: ") +
         error.what());
  }
}

CanonicalEmptyRetainedBundleAdmission
generate_canonical_empty_package(const std::filesystem::path &output_root) {
  try {
    const std::filesystem::path root =
        normalize_canonical_empty_package_root(output_root);

    // A visible commit record always wins. It is verified before any
    // repository constructor or other write-capable operation is reached.
    if (read_canonical_empty_admission_if_present(root).has_value()) {
      return FilesystemCanonicalEmptyRetainedBundleSource(root).load_verified();
    }

    const RevisionVerifiedDeploymentArtifact deployment =
        load_canonical_empty_fixed_deployment();
    const RetentionQualifiedDeployment qualified =
        compile_fixed_deployment(deployment);

    FilesystemBuildManifestRepository manifest_repository(root);
    FilesystemRawArtifactStore artifact_store(root);
    FilesystemRetainedBundleRepository bundle_repository(root);
    const detail::RejectingCompilerInputArtifactSource input_source;
    detail::RejectingCatalogArtifactStore catalog_store;
    const auto produced =
        RetainedBundleMaterializer::materialize_store_and_reload_production(
            qualified, manifest_repository, input_source, {}, catalog_store,
            artifact_store, bundle_repository);
    if (produced == nullptr) {
      throw std::logic_error("canonical-empty materializer returned null");
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
        context.compiler_contract_digest(), context.compiler_build_digest(),
        nullptr);

    CanonicalEmptyAdmissionRecord record =
        make_canonical_empty_admission_record(*independently_reloaded);
    const std::filesystem::path admission_path =
        root / std::string(kAdmissionFilename);
    (void)posix::publish_immutable_file_no_replace(
        admission_path, record.canonical_bytes,
        kMaxCanonicalEmptyAdmissionBytes, kPosixErrors, kAdmissionReadMessages,
        kAdmissionDirectorySyncMessages, kAdmissionPublicationMessages,
        [&record](std::string_view existing) {
          const CanonicalEmptyAdmissionRecord existing_record =
              parse_canonical_empty_admission_record(existing);
          if (existing != record.canonical_bytes ||
              existing_record.admission_digest != record.admission_digest) {
            fail("canonical-empty admission publication collided with a "
                 "different immutable record");
          }
        });

    return FilesystemCanonicalEmptyRetainedBundleSource(root).load_verified();
  } catch (const CanonicalEmptyAdmissionError &) {
    throw;
  } catch (const std::exception &error) {
    fail(std::string("canonical-empty package generation failed: ") +
         error.what());
  }
}

} // namespace manyforge::deployment::detail
