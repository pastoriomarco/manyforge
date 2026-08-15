#include "manyforge_deployment/canonical_empty_admission.hpp"

#include <algorithm>
#include <array>
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

#include "build_qualified_admission_profile_internal.hpp"
#include "canonical_empty_admission_internal.hpp"
#include "content_identity_internal.hpp"
#include "embedded_canonical_empty_deployment.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/deployment_artifact_codec.hpp"
#include "manyforge_deployment/native_execution_runtime_contract.hpp"
#include "manyforge_deployment/ros2_control_execution_runtime_contract.hpp"
#include "posix_durability_internal.hpp"

namespace manyforge::deployment {
namespace {

namespace posix = detail::posix_durability;

constexpr std::string_view kCanonicalDeploymentArtifactId =
    "c1_build_authority";
constexpr std::string_view kCanonicalCellId = "c1_build_authority_cell";
constexpr std::string_view kAdmissionFilename =
    "canonical-empty-admission.json";
constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

struct CanonicalEmptyExecutionRuntimeContract final {
  std::string_view runtime_profile_id;
  std::string_view product_contract_id;
  std::string_view runtime_role_id;
  std::string_view process_adapter_contract_id;
  std::string_view operating_system;
  std::string_view launch_environment_contract;
};

constexpr std::array<CanonicalEmptyExecutionRuntimeContract, 2U>
    kCanonicalEmptyExecutionRuntimeContracts{{
        {native_execution_runtime_contract::kRuntimeProfileId,
         native_execution_runtime_contract::kProductContractId,
         native_execution_runtime_contract::kRuntimeRoleId,
         native_execution_runtime_contract::kProcessAdapterContractId,
         native_execution_runtime_contract::kOperatingSystem,
         native_execution_runtime_contract::kLaunchEnvironmentContract},
        {ros2_control_execution_runtime_contract::kRuntimeProfileId,
         ros2_control_execution_runtime_contract::kProductContractId,
         ros2_control_execution_runtime_contract::kRuntimeRoleId,
         ros2_control_execution_runtime_contract::kProcessAdapterContractId,
         ros2_control_execution_runtime_contract::kOperatingSystem,
         ros2_control_execution_runtime_contract::kLaunchEnvironmentContract},
    }};

[[nodiscard]] const CanonicalEmptyExecutionRuntimeContract *
canonical_empty_execution_runtime_contract(std::string_view profile_id) {
  const auto found = std::find_if(
      kCanonicalEmptyExecutionRuntimeContracts.begin(),
      kCanonicalEmptyExecutionRuntimeContracts.end(),
      [profile_id](const auto &candidate) {
        return candidate.runtime_profile_id == profile_id;
      });
  return found == kCanonicalEmptyExecutionRuntimeContracts.end() ? nullptr
                                                                 : &*found;
}

[[noreturn]] void fail(std::string_view message) {
  throw CanonicalEmptyAdmissionError(std::string(message));
}

[[nodiscard]] std::exception_ptr
make_admission_error(std::string_view message) {
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
[[nodiscard]] std::filesystem::path
normalize_package_root(const std::filesystem::path &root) {
  try {
    return posix::normalize_existing_plain_root(root, kPosixErrors,
                                                kTrustedRootMessages);
  } catch (const CanonicalEmptyAdmissionError &) {
    throw;
  } catch (const std::exception &error) {
    fail(std::string("canonical-empty package root is unavailable: ") +
         error.what());
  }
}

[[nodiscard]] std::filesystem::path
normalize_admission_filename(const std::filesystem::path &filename) {
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
    fail("canonical-empty admission locator must be one bounded root-local "
         "filename");
  }
  return filename;
}

[[nodiscard]] const CanonicalValue::Object &
require_object(const CanonicalValue &value, std::string_view field) {
  const auto *object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(std::string(field) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue &
require_member(const CanonicalValue::Object &object, std::string_view name,
               std::string_view field) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [name](const auto &member) { return member.first == name; });
  if (found == object.end()) {
    fail(std::string(field) + " is missing required member " +
         std::string(name));
  }
  return found->second;
}

[[nodiscard]] const std::string &require_string(const CanonicalValue &value,
                                                std::string_view field) {
  const auto *string = std::get_if<std::string>(&value.storage());
  if (string == nullptr) {
    fail(std::string(field) + " must be a string");
  }
  return *string;
}

void require_exact_fields(const CanonicalValue::Object &object,
                          std::initializer_list<std::string_view> expected,
                          std::string_view field) {
  if (object.size() != expected.size()) {
    fail(std::string(field) + " has an incorrect closed field count");
  }
  for (const auto &[name, unused] : object) {
    (void)unused;
    if (std::find(expected.begin(), expected.end(), name) == expected.end()) {
      fail(std::string(field) + " contains unknown member " + name);
    }
  }
  for (const std::string_view name : expected) {
    (void)require_member(object, name, field);
  }
}

void require_digest(std::string_view value, std::string_view field) {
  if (!is_sha256_digest(value)) {
    fail(std::string(field) + " must be an exact lowercase SHA-256 digest");
  }
}

using detail::CanonicalEmptyAdmissionRecord;

[[nodiscard]] detail::ContentIdentitySpec admission_identity_spec() {
  return {
      .identity_domain = "canonical-empty-admission",
      .api_version = std::string(kCanonicalEmptyAdmissionApiVersion),
      .own_field = "admission_digest",
  };
}

[[nodiscard]] CanonicalValue
admission_projection(std::string_view admission_digest,
                     const RetainedBundle &bundle) {
  const auto &value = bundle.value();
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kCanonicalEmptyAdmissionApiVersion))},
      {"admission_digest", CanonicalValue(std::string(admission_digest))},
      {"cell_id", CanonicalValue(value.cell_graph.cell_id.value())},
      {"deployment_artifact_id",
       CanonicalValue(value.deployment_artifact_id.value())},
      {"deployment_revision",
       CanonicalValue(value.deployment_revision.value())},
      {"graph_revision",
       CanonicalValue(value.cell_graph.graph_revision.value())},
      {"retained_bundle_id", CanonicalValue(value.retained_bundle_id.value())},
      {"environment_digest",
       CanonicalValue(
           value.catalogs.node_environment.environment_digest.value())},
      {"compiler_contract_digest",
       CanonicalValue(value.provenance.compiler_contract_digest.value())},
      {"compiler_build_digest",
       CanonicalValue(value.provenance.compiler_build_digest.value())},
  });
}

[[nodiscard]] CanonicalEmptyAdmissionRecord
make_admission_record(const RetainedBundle &bundle) {
  CanonicalValue placeholder = admission_projection(kZeroDigest, bundle);
  const std::string digest =
      detail::compute_content_identity(placeholder, admission_identity_spec());
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

[[nodiscard]] CanonicalEmptyAdmissionRecord
parse_admission_record(std::string_view bytes) {
  if (bytes.empty() || bytes.size() > kMaxCanonicalEmptyAdmissionBytes) {
    fail("canonical-empty admission record violates its 16 KiB bound");
  }
  const ParsedArtifactCandidate parsed = parse_strict_json_model_yaml(bytes);
  CanonicalValue root = parsed.root();
  const auto &object = require_object(root, "canonical_empty_admission");
  require_exact_fields(object,
                       {"api_version", "admission_digest", "cell_id",
                        "deployment_artifact_id", "deployment_revision",
                        "graph_revision", "retained_bundle_id",
                        "environment_digest", "compiler_contract_digest",
                        "compiler_build_digest"},
                       "canonical_empty_admission");

  const auto field = [&object](std::string_view name) -> const std::string & {
    return require_string(
        require_member(object, name, "canonical_empty_admission"),
        std::string("canonical_empty_admission.") + std::string(name));
  };
  const std::string &api_version = field("api_version");
  if (api_version != kCanonicalEmptyAdmissionApiVersion) {
    fail("canonical-empty admission api_version is not exact v1");
  }
  const std::string &admission_digest = field("admission_digest");
  const std::string &cell_id = field("cell_id");
  const std::string &deployment_artifact_id = field("deployment_artifact_id");
  const std::string &deployment_revision = field("deployment_revision");
  const std::string &graph_revision = field("graph_revision");
  const std::string &retained_bundle_id = field("retained_bundle_id");
  const std::string &environment_digest = field("environment_digest");
  const std::string &compiler_contract_digest =
      field("compiler_contract_digest");
  const std::string &compiler_build_digest = field("compiler_build_digest");

  for (const auto &[value, name] :
       {std::pair<std::string_view, std::string_view>{admission_digest,
                                                      "admission_digest"},
        {deployment_revision, "deployment_revision"},
        {graph_revision, "graph_revision"},
        {retained_bundle_id, "retained_bundle_id"},
        {environment_digest, "environment_digest"},
        {compiler_contract_digest, "compiler_contract_digest"},
        {compiler_build_digest, "compiler_build_digest"}}) {
    require_digest(value, std::string("canonical_empty_admission.") +
                              std::string(name));
  }
  if (!detail::verify_content_identity(root, admission_identity_spec())) {
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

[[nodiscard]] RevisionVerifiedDeploymentArtifact load_fixed_deployment() {
  const auto deployment = verify_deployment_artifact_revision(
      parse_strict_json_model_yaml(detail::kEmbeddedCanonicalEmptyDeployment),
      std::span<const QualifiedRuntimeProfile>{});
  if (deployment.deployment_artifact_id().value() !=
          kCanonicalDeploymentArtifactId ||
      deployment.cell_id().value() != kCanonicalCellId) {
    throw std::logic_error(
        "embedded canonical-empty deployment has the wrong fixed identity");
  }
  return deployment;
}

void require_bundle_matches_authorities(
    const RetainedBundle &bundle,
    const RevisionVerifiedDeploymentArtifact &deployment,
    const core::RetainedNodeEnvironmentDTO &node_environment,
    const core::ContentDigest &compiler_contract_digest,
    const core::ContentDigest &compiler_build_digest,
    const CanonicalEmptyAdmissionRecord *record) {
  const auto &value = bundle.value();
  if (!verify_retained_bundle_identity(bundle) ||
      bundle.retained_bundle_id() != value.retained_bundle_id ||
      value.deployment_artifact_id != deployment.deployment_artifact_id() ||
      value.deployment_revision != deployment.deployment_revision() ||
      value.cell_graph != deployment.graph().graph() ||
      value.cell_graph.cell_id != deployment.cell_id() ||
      value.catalogs.node_environment != node_environment ||
      value.provenance.compiler_contract_digest != compiler_contract_digest ||
      value.provenance.compiler_build_digest != compiler_build_digest) {
    fail("canonical-empty retained bundle differs from its fixed source or "
         "current build authority");
  }
  if (record == nullptr) {
    return;
  }
  if (record->cell_id != value.cell_graph.cell_id.value() ||
      record->deployment_artifact_id != value.deployment_artifact_id.value() ||
      record->deployment_revision != value.deployment_revision.value() ||
      record->graph_revision != value.cell_graph.graph_revision.value() ||
      record->retained_bundle_id != value.retained_bundle_id.value() ||
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

void require_bundle_matches_record_and_authorities(
    const RetainedBundle &bundle,
    const core::RetainedNodeEnvironmentDTO &node_environment,
    const core::ContentDigest &compiler_contract_digest,
    const core::ContentDigest &compiler_build_digest,
    const CanonicalEmptyAdmissionRecord &record) {
  const auto &value = bundle.value();
  if (!verify_retained_bundle_identity(bundle) ||
      bundle.retained_bundle_id() != value.retained_bundle_id ||
      value.catalogs.node_environment != node_environment ||
      value.provenance.compiler_contract_digest != compiler_contract_digest ||
      value.provenance.compiler_build_digest != compiler_build_digest ||
      record.cell_id != value.cell_graph.cell_id.value() ||
      record.deployment_artifact_id != value.deployment_artifact_id.value() ||
      record.deployment_revision != value.deployment_revision.value() ||
      record.graph_revision != value.cell_graph.graph_revision.value() ||
      record.retained_bundle_id != value.retained_bundle_id.value() ||
      record.environment_digest !=
          value.catalogs.node_environment.environment_digest.value() ||
      record.compiler_contract_digest !=
          value.provenance.compiler_contract_digest.value() ||
      record.compiler_build_digest !=
          value.provenance.compiler_build_digest.value()) {
    fail("canonical-empty admission record or retained bundle differs from "
         "the current build authority");
  }
}

void require_canonical_empty_shape(const RetainedBundle &bundle,
                                   const std::filesystem::path &package_root,
                                   const bool fixed_golden) {
  const auto &value = bundle.value();
  const auto &graph = value.cell_graph;
  if (!graph.model_instances.empty() ||
      !graph.controllable_components.empty() ||
      !graph.control_domains.empty() || !graph.control_bindings.empty() ||
      !graph.component_attachments.empty() ||
      !value.desired_control_configurations.empty() ||
      !value.capability_provider_bindings.empty() ||
      !value.compiled_products.control_domain_products.empty() ||
      !value.compiled_products.symbol_role_maps.empty() ||
      value.selected_site_overlay_ref.has_value() ||
      !value.external_resources.empty() || value.assistant.has_value()) {
    fail("canonical-empty admission contains active cell semantics");
  }
  if (fixed_golden) {
    if (!graph.runtime_units.empty() ||
        !value.compiled_products.runtime_unit_products.empty() ||
        !value.catalogs.records.empty()) {
      fail("fixed canonical-empty golden contains a runtime unit");
    }
    return;
  }

  if (graph.runtime_units.size() != 1U ||
      value.compiled_products.runtime_unit_products.size() != 1U ||
      value.catalogs.records.size() != 1U) {
    fail("cell-scoped canonical-empty admission must retain exactly one "
         "execution host runtime");
  }
  const auto &runtime = graph.runtime_units.front();
  const auto *profile = std::get_if<core::RuntimeProfileCatalogRecordRefDTO>(
      &value.catalogs.records.front());
  const auto &product_group =
      value.compiled_products.runtime_unit_products.front();
  const auto *runtime_contract = canonical_empty_execution_runtime_contract(
      runtime.runtime_profile_id.value());
  if (runtime_contract == nullptr ||
      !runtime.dependency_runtime_unit_ids.empty() || profile == nullptr ||
      profile->record_id != runtime.runtime_profile_id ||
      profile->record_revision != runtime.runtime_profile_revision ||
      product_group.runtime_unit_id != runtime.runtime_unit_id ||
      product_group.products.size() != 1U ||
      product_group.products.front().product_contract_id.value() !=
          runtime_contract->product_contract_id) {
    fail("cell-scoped canonical-empty admission retains a foreign or "
         "incomplete execution host runtime");
  }
  FilesystemVerifiedRetainedProductArtifactSource artifacts(package_root);
  const auto product =
      verify_runtime_unit_product(bundle, runtime.runtime_unit_id, artifacts);
  if (!product.has_value() ||
      product->product_contract_id().value() !=
          runtime_contract->product_contract_id ||
      product->runtime_role_id() != runtime_contract->runtime_role_id ||
      product->process_adapter_contract_id() !=
          runtime_contract->process_adapter_contract_id ||
      product->operating_system() != runtime_contract->operating_system ||
      product->launch_environment_contract() !=
          runtime_contract->launch_environment_contract) {
    fail("cell-scoped canonical-empty execution host product is not exactly "
         "build-qualified");
  }
}

[[nodiscard]] std::optional<std::string>
read_admission_if_present(const std::filesystem::path &root,
                          const std::filesystem::path &filename) {
  return posix::read_exact_regular_file(
      root / normalize_admission_filename(filename),
      kMaxCanonicalEmptyAdmissionBytes, true, kPosixErrors,
      kAdmissionReadMessages);
}

} // namespace

namespace detail {

std::filesystem::path
normalize_canonical_empty_package_root(const std::filesystem::path &root) {
  return normalize_package_root(root);
}

CanonicalEmptyAdmissionRecord
make_canonical_empty_admission_record(const RetainedBundle &bundle) {
  return make_admission_record(bundle);
}

CanonicalEmptyAdmissionRecord
parse_canonical_empty_admission_record(const std::string_view bytes) {
  return parse_admission_record(bytes);
}

RevisionVerifiedDeploymentArtifact load_canonical_empty_fixed_deployment() {
  return load_fixed_deployment();
}

void require_canonical_empty_bundle_matches_authorities(
    const RetainedBundle &bundle,
    const RevisionVerifiedDeploymentArtifact &deployment,
    const core::RetainedNodeEnvironmentDTO &node_environment,
    const core::ContentDigest &compiler_contract_digest,
    const core::ContentDigest &compiler_build_digest,
    const CanonicalEmptyAdmissionRecord *record) {
  require_bundle_matches_authorities(bundle, deployment, node_environment,
                                     compiler_contract_digest,
                                     compiler_build_digest, record);
}

std::optional<std::string> read_canonical_empty_admission_if_present(
    const std::filesystem::path &root,
    const std::filesystem::path &admission_filename) {
  return read_admission_if_present(root, admission_filename);
}

} // namespace detail

FilesystemCanonicalEmptyRetainedBundleSource::
    FilesystemCanonicalEmptyRetainedBundleSource(
        std::filesystem::path trusted_package_root)
    : FilesystemCanonicalEmptyRetainedBundleSource(
          std::move(trusted_package_root),
          std::filesystem::path(kAdmissionFilename)) {}

FilesystemCanonicalEmptyRetainedBundleSource::
    FilesystemCanonicalEmptyRetainedBundleSource(
        std::filesystem::path trusted_package_root,
        std::filesystem::path admission_filename)
    : trusted_package_root_(normalize_package_root(trusted_package_root)),
      admission_filename_(normalize_admission_filename(admission_filename)) {}

CanonicalEmptyRetainedBundleAdmission
FilesystemCanonicalEmptyRetainedBundleSource::load_verified() const {
  try {
    const std::filesystem::path current_root =
        normalize_package_root(trusted_package_root_);
    if (current_root != trusted_package_root_) {
      fail("canonical-empty package root changed after construction");
    }

    const auto bytes =
        read_admission_if_present(current_root, admission_filename_);
    if (!bytes.has_value()) {
      fail("canonical-empty admission record is missing");
    }
    CanonicalEmptyAdmissionRecord record = parse_admission_record(*bytes);
    const bool fixed_golden =
        record.cell_id == kCanonicalCellId &&
        record.deployment_artifact_id == kCanonicalDeploymentArtifactId;

    const detail::BuildQualifiedAdmissionProfileAuthority profile =
        detail::verify_build_qualified_admission_profile(current_root);
    FilesystemVerifiedRetainedBundleSource bundle_source(current_root);
    const auto bundle = bundle_source.get_verified(
        core::RetainedBundleId(record.retained_bundle_id));
    if (bundle == nullptr) {
      fail("canonical-empty retained bundle is missing");
    }
    require_bundle_matches_record_and_authorities(
        *bundle, profile.node_environment(), profile.compiler_contract_digest(),
        profile.compiler_build_digest(), record);
    if (fixed_golden) {
      const RevisionVerifiedDeploymentArtifact deployment =
          load_fixed_deployment();
      if (record.deployment_revision !=
              deployment.deployment_revision().value() ||
          record.graph_revision !=
              deployment.graph().graph().graph_revision.value()) {
        fail("canonical-empty admission differs from the exact embedded "
             "deployment");
      }
      require_bundle_matches_authorities(
          *bundle, deployment, profile.node_environment(),
          profile.compiler_contract_digest(), profile.compiler_build_digest(),
          &record);
    }
    require_canonical_empty_shape(*bundle, current_root, fixed_golden);

    return CanonicalEmptyRetainedBundleAdmission(
        core::ContentDigest(std::move(record.admission_digest)),
        core::CellId(std::move(record.cell_id)),
        core::DeploymentArtifactId(std::move(record.deployment_artifact_id)),
        core::DeploymentRevision(std::move(record.deployment_revision)),
        core::GraphRevision(std::move(record.graph_revision)),
        core::RetainedBundleId(std::move(record.retained_bundle_id)),
        core::ContentDigest(std::move(record.environment_digest)),
        core::ContentDigest(std::move(record.compiler_contract_digest)),
        core::ContentDigest(std::move(record.compiler_build_digest)),
        std::move(bundle), std::move(record.canonical_bytes));
  } catch (const CanonicalEmptyAdmissionError &) {
    throw;
  } catch (const std::exception &error) {
    fail(std::string("canonical-empty package verification failed: ") +
         error.what());
  }
}

std::filesystem::path
cell_scoped_canonical_empty_admission_filename(const core::CellId &cell_id) {
  if (cell_id.empty()) {
    throw CanonicalEmptyAdmissionError(
        "canonical-empty admission cell identity cannot be empty");
  }
  return normalize_admission_filename("canonical-empty-admission." +
                                      cell_id.value() + ".json");
}

} // namespace manyforge::deployment
