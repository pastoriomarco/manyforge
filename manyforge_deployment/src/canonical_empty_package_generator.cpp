#include "canonical_empty_admission_internal.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "build_manifest_internal.hpp"
#include "manyforge_deployment/build_manifest.hpp"
#include "manyforge_deployment/cell_applicability.hpp"
#include "manyforge_deployment/cell_compiler.hpp"
#include "manyforge_deployment/cell_composition.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/runtime_profile_codec.hpp"
#include "manyforge_deployment/retention_qualified_deployment.hpp"
#include "manyforge_deployment/ros2_control_execution_runtime_contract.hpp"
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
compile_fixed_deployment(
    const RevisionVerifiedDeploymentArtifact &deployment,
    std::span<const QualifiedRuntimeProfile> profiles = {}) {
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
      profiles,
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
  const bool execution_profile =
      graph.runtime_units.size() == 1U &&
      graph.runtime_units.front().runtime_profile_id.value() ==
          ros2_control_execution_runtime_contract::kRuntimeProfileId;
  if (!execution_profile ||
      !graph.runtime_units.front().dependency_runtime_unit_ids.empty()) {
    fail("cell-scoped canonical-empty derivation requires one independent "
         "execution host runtime");
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
  auto empty_runtime_fields = authored_runtime_units.front().as_object();
  // Component-owned I/O declarations cannot survive removal of their owners.
  // Keep every other execution-host setting exactly as authored.
  for (auto &[name, value] : empty_runtime_fields) {
    if (name != "configuration") continue;
    auto configuration = value.as_object();
    for (auto &[field, setting] : configuration)
      if (field == "io_signals" || field == "io_provider_devices")
        setting = CanonicalValue(CanonicalValue::Array{});
    value = CanonicalValue(std::move(configuration));
  }
  const CanonicalValue empty_runtime_unit(std::move(empty_runtime_fields));

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
                                 empty_runtime_unit,
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

namespace {

CanonicalEmptyRetainedBundleAdmission generate_ros_empty_package_impl(
    const std::filesystem::path &output_root, const core::CellId &cell_id,
    std::string_view runtime_profile_bytes) {
  using Object = CanonicalValue::Object;
  const std::string_view lane = "ROS";
  const auto profile_id = ros2_control_execution_runtime_contract::kRuntimeProfileId;
  // Validate before creating any artifact repositories or admission locator.
  const auto &cell_text = cell_id.value();
  if (cell_text.empty() || cell_text.size() > 128U ||
      cell_text.front() < 'a' || cell_text.front() > 'z' ||
      !std::all_of(cell_text.begin(), cell_text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '.';
      })) {
    fail(std::string(lane) + " bootstrap cell_id must be a bounded portable identifier");
  }
  const auto root = normalize_canonical_empty_package_root(output_root);
  const auto parsed = parse_strict_json_model_yaml(runtime_profile_bytes);
  const auto &profile_root = parsed.root();
  const auto kind = core::CatalogRecordKind::kRuntimeProfile;
  const auto *schema = build_qualified_closed_schema_registry()
                           .find_catalog_schema_family(kind);
  if (schema == nullptr || schema->compiled_schema() == nullptr) {
    fail(std::string(lane) + " bootstrap runtime profile schema is unavailable");
  }
  const std::string digest = sha256_digest(runtime_profile_bytes);
  const core::RuntimeProfileCatalogRecordRefDTO reference{
      core::RuntimeProfileId(require_member(profile_root, "runtime_profile_id", "profile").as_string()),
      core::RuntimeProfileRevision(require_member(profile_root, "runtime_profile_revision", "profile").as_string()),
      core::SchemaId("manyforge.runtime_profile.v1"),
      schema->compiled_schema()->schema_revision(),
      core::ContentDigest(digest),
      core::ContentAddressedStoreKey(
          std::string("catalog/ros-bootstrap/") + digest.substr(7U)),
  };
  const auto profile = resolve_and_qualify_runtime_profile(
      reference, reference.content_addressed_store_key, runtime_profile_bytes);
  if (profile.value().runtime_profile_id.value() != profile_id) {
    fail("ROS bootstrap requires the ROS execution profile");
  }
  const std::vector<QualifiedRuntimeProfile> profiles{profile};
  const CanonicalValue catalog_reference(Object{
      {"record_kind", CanonicalValue("runtime_profile")},
      {"record_id", CanonicalValue(reference.record_id.value())},
      {"record_revision", CanonicalValue(reference.record_revision.value())},
      {"schema_id", CanonicalValue(reference.schema_id.value())},
      {"schema_revision", CanonicalValue(reference.schema_revision.value())},
      {"content_digest", CanonicalValue(digest)},
      {"content_addressed_store_key", CanonicalValue(reference.content_addressed_store_key.value())},
  });
  // These are workbench infrastructure settings, not controller defaults.
  // An incompatible infrastructure change remains a pre-effect Apply refusal.
  const auto authored_deployment =
      ros2_control_execution_runtime_contract::empty_host_deployment(
          cell_id, catalog_reference, "ros_workbench_bootstrap",
          "ROS workbench infrastructure");
  const auto deployment = compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(canonical_json(authored_deployment)), profiles);
  const auto qualified = compile_fixed_deployment(deployment, profiles);
  FilesystemBuildManifestRepository manifests(root);
  FilesystemRawArtifactStore artifacts(root);
  FilesystemRetainedBundleRepository bundles(root);
  FilesystemCatalogArtifactStore catalogs(root);
  class ProfileSource final : public ICompilerInputArtifactSource {
   public:
    ProfileSource(const core::RuntimeProfileCatalogRecordRefDTO &ref,
                  std::string_view value) : reference_(ref), bytes_(value) {}
    std::optional<std::string> get_verified(
        const core::ContentAddressedStoreKey &key,
        const core::ContentDigest &expected,
        std::optional<std::uint64_t> size) const override {
      if (key != reference_.content_addressed_store_key ||
          expected != reference_.content_digest ||
          (size.has_value() && *size != bytes_.size())) return std::nullopt;
      return bytes_;
    }
   private:
    core::RuntimeProfileCatalogRecordRefDTO reference_;
    std::string bytes_;
  } source(reference, runtime_profile_bytes);
  const auto bundle = RetainedBundleMaterializer::materialize_store_and_reload_production(
      qualified, manifests, source, {}, catalogs, artifacts, bundles);
  if (bundle == nullptr) fail(std::string(lane) + " bootstrap retained product is unavailable");
  const auto &runtime_id =
      bundle->value().cell_graph.runtime_units.front().runtime_unit_id;
  static_cast<void>(ros2_control_execution_runtime_contract::require_configuration(
      *bundle, runtime_id));
  return publish_cell_scoped_canonical_empty_admission(
      root, cell_scoped_canonical_empty_admission_filename(cell_id), *bundle);
}

} // namespace

CanonicalEmptyRetainedBundleAdmission generate_ros_empty_package(
    const std::filesystem::path &output_root, const core::CellId &cell_id,
    std::string_view runtime_profile_bytes) {
  return generate_ros_empty_package_impl(output_root, cell_id,
                                         runtime_profile_bytes);
}

} // namespace manyforge::deployment::detail
