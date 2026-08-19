#include <algorithm>
#include <array>
#include <exception>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/runtime_contract_registry.hpp"
#include "reference_runtime_unit_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kRuntimeProfileConfigurationSchemaId =
    "manyforge.value_schema.runtime.empty.v1";
constexpr std::string_view kRuntimeProfileConfigurationSchemaRevision =
    "sha256:03457d535d31818fedaa7e44f48432a08feb705e4d22c9a8f5e50977c8dbf785";

[[nodiscard]] const CanonicalValue::Object &
require_object(const CanonicalValue &value, std::string_view field) {
  const auto *object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail_retained_bundle(std::string(field) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array &
require_array(const CanonicalValue &value, std::string_view field) {
  const auto *array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail_retained_bundle(std::string(field) + " must be an array");
  }
  return *array;
}

[[nodiscard]] const CanonicalValue &
require_member(const CanonicalValue::Object &object, std::string_view name,
               std::string_view field) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [name](const auto &member) { return member.first == name; });
  if (found == object.end()) {
    fail_retained_bundle(std::string(field) + " is missing " +
                         std::string(name));
  }
  return found->second;
}

[[nodiscard]] const std::string &require_string(const CanonicalValue &value,
                                                std::string_view field) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail_retained_bundle(std::string(field) + " must be a string");
  }
  return *text;
}

[[nodiscard]] std::string require_identity(const CanonicalValue::Object &object,
                                           std::string_view name,
                                           std::string_view field) {
  const std::string value =
      require_string(require_member(object, name, field),
                     std::string(field) + "." + std::string(name));
  require_retained_portable_identity(value, std::string(field) + "." +
                                                std::string(name));
  return value;
}

[[nodiscard]] std::string require_digest(const CanonicalValue::Object &object,
                                         std::string_view name,
                                         std::string_view field) {
  const std::string value =
      require_string(require_member(object, name, field),
                     std::string(field) + "." + std::string(name));
  require_retained_digest(value, std::string(field) + "." + std::string(name));
  return value;
}

void require_exact_fields(const CanonicalValue::Object &object,
                          std::initializer_list<std::string_view> expected,
                          std::string_view field) {
  if (object.size() != expected.size()) {
    fail_retained_bundle(std::string(field) +
                         " has an incorrect closed field count");
  }
  for (const auto &[name, unused] : object) {
    (void)unused;
    if (std::find(expected.begin(), expected.end(), name) == expected.end()) {
      fail_retained_bundle(std::string(field) + " contains unknown member " +
                           name);
    }
  }
  for (const auto name : expected) {
    static_cast<void>(require_member(object, name, field));
  }
}

[[nodiscard]] std::string contract_revision(std::string_view domain,
                                            std::string_view description) {
  std::string preimage("manyforge.reference-runtime-contract.v1");
  preimage.push_back('\0');
  preimage.append(domain);
  preimage.push_back('\0');
  preimage.append(description);
  return sha256_digest(preimage);
}

[[nodiscard]] const core::RuntimeUnitDTO &
require_runtime_unit(const core::CellGraphDTO &graph,
                     const core::RuntimeUnitId &runtime_unit_id) {
  const auto found =
      std::find_if(graph.runtime_units.begin(), graph.runtime_units.end(),
                   [&](const auto &unit) {
                     return unit.runtime_unit_id == runtime_unit_id;
                   });
  if (found == graph.runtime_units.end()) {
    fail_retained_bundle(
        "reference runtime product has no exact retained graph subject");
  }
  return *found;
}

[[nodiscard]] const core::RetainedRuntimeUnitProductsDTO &
require_runtime_product_group(const core::RetainedBundleDTO &bundle,
                              const core::RuntimeUnitId &runtime_unit_id) {
  const auto found =
      std::find_if(bundle.compiled_products.runtime_unit_products.begin(),
                   bundle.compiled_products.runtime_unit_products.end(),
                   [&](const auto &group) {
                     return group.runtime_unit_id == runtime_unit_id;
                   });
  if (found == bundle.compiled_products.runtime_unit_products.end() ||
      found->products.size() != 1U) {
    fail_retained_bundle(
        "reference runtime product has no exact retained product group");
  }
  return *found;
}

[[nodiscard]] const core::RetainedArtifactRefDTO &
require_artifact(const core::RetainedProductDTO &product,
                 std::string_view role) {
  const core::RetainedArtifactRefDTO *match = nullptr;
  for (const auto &artifact : product.artifact_refs) {
    if (artifact.artifact_role_id.value() != role) {
      continue;
    }
    if (match != nullptr) {
      fail_retained_bundle(
          "reference runtime product repeats an artifact role");
    }
    match = &artifact;
  }
  if (match == nullptr) {
    fail_retained_bundle("reference runtime product omits an artifact role");
  }
  return *match;
}

} // namespace

std::string reference_runtime_profile_document() {
  CanonicalValue identity(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.runtime_profile.v1")},
      {"runtime_profile_id",
       CanonicalValue(std::string(kReferenceRuntimeProfileId))},
      {"configuration_schema_ref",
       CanonicalValue(CanonicalValue::Object{
           {"schema_id",
            CanonicalValue(std::string(kRuntimeProfileConfigurationSchemaId))},
           {"schema_revision",
            CanonicalValue(
                std::string(kRuntimeProfileConfigurationSchemaRevision))},
       })},
      {"process_contract",
       CanonicalValue(CanonicalValue::Object{
           {"lifecycle_control", CanonicalValue("manager_managed")},
       })},
      {"supervision_contract",
       CanonicalValue(CanonicalValue::Object{
           {"status_endpoint_role_id",
            CanonicalValue(std::string(kReferenceRuntimeStatusEndpointRoleId))},
       })},
      {"offered_runtime_role_ids",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(std::string(kReferenceRuntimeRoleId))})},
      {"source_endpoint_role_ids",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(
           std::string(kReferenceRuntimeStatusEndpointRoleId))})},
  });

  std::string preimage("manyforge.content-identity.v1");
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/runtime_profile");
  preimage.push_back('\0');
  preimage.append("manyforge.runtime_profile.v1");
  preimage.push_back('\0');
  preimage.append(canonical_json(identity));
  auto root = identity.as_object();
  root.emplace_back("runtime_profile_revision",
                    CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(root)));
}

std::string reference_runtime_profile_revision() {
  const CanonicalValue root =
      parse_strict_json_model_yaml(reference_runtime_profile_document()).root();
  return require_digest(require_object(root, "reference_runtime_profile"),
                        "runtime_profile_revision",
                        "reference_runtime_profile");
}

const std::string& reference_runtime_launchable_script() {
  static const std::string script = [] {
    const auto& runtime_contract = require_runtime_contract(
        core::RuntimeProfileId(std::string(kReferenceRuntimeProfileId)),
        core::RuntimeProfileRevision(reference_runtime_profile_revision()),
        core::ProductContractId(
            std::string(kReferenceRuntimeProductContractId)),
        reference_runtime_product_contract_revision());
    return std::string("#!/bin/sh\n") +
         "set -eu\n"
         "if [ \"$#\" -ne 8 ] || "
         "[ \"$1\" != \"--runtime-unit-id\" ] || "
         "[ \"$3\" != \"--runtime-epoch\" ] || "
         "[ \"$5\" != \"--product-identity\" ] || "
         "[ \"$7\" != \"--configuration-identity\" ]; then\n"
         "  exit 64\n"
         "fi\n"
         "runtime_unit_id=$2\n"
         "runtime_epoch=$4\n"
         "product_identity=$6\n"
         "configuration_identity=$8\n"
         "runtime_contract_offer_digest='" +
         runtime_contract.offer().offer_digest.value() + "'\n" +
         "source_report_sequence=0\n"
         "emit_status() {\n"
         "  source_report_sequence=$((source_report_sequence + 1))\n"
         "  printf '%s\\n' \"manyforge.reference-runtime-status.v2 "
         "$runtime_unit_id $runtime_epoch $source_report_sequence 1 serving "
         "healthy "
         "$runtime_contract_offer_digest $product_identity "
         "$configuration_identity manager_fixture "
         "closed_no_active_effect\"\n"
         "}\n"
         "emit_status\n"
         "while IFS= read -r command; do\n"
         "  case \"$command\" in\n"
         "    status) emit_status ;;\n"
         "    shutdown) exit 0 ;;\n"
         "    *) exit 65 ;;\n"
         "  esac\n"
         "done\n"
         "exit 0\n";
  }();
  return script;
}

std::string reference_runtime_implementation_revision() {
  return contract_revision(
      "runtime-implementation",
      "reference_process_lifecycle;role=manager_fixture;status=stdout-v2;"
      "embedded-runtime-contract-offer-digest");
}

std::string reference_runtime_process_adapter_revision() {
  return contract_revision(
      "process-adapter",
      "manager_managed;posix-sh;retained-script;fixed-argv;adapter-epoch;"
      "embedded-runtime-contract-offer-digest;stdin-status-shutdown");
}

std::string reference_runtime_retrieval_contract_revision() {
  return contract_revision(
      "retrieval-contract",
      "retained-raw-artifact;digest-and-size-verified-before-effect");
}

std::string reference_runtime_status_endpoint_revision() {
  return contract_revision(
      "status-endpoint",
      "stdout-line;reference-runtime-status-v2;runtime-unit-id;runtime-epoch;"
      "source-sequence;critical-sequence;phase;health;product-identity;"
      "configuration-identity;runtime-role;command-surface;"
      "runtime-contract-offer-digest");
}

std::string
reference_runtime_product_id(const core::RuntimeUnitId &runtime_unit_id) {
  require_retained_portable_identity(runtime_unit_id.value(),
                                     "runtime_unit_id");
  std::string preimage("manyforge.runtime-unit-product-id.v1");
  preimage.push_back('\0');
  preimage.append(runtime_unit_id.value());
  return "runtime_unit_configuration_" + sha256_digest(preimage).substr(7U);
}

CanonicalValue
parse_and_verify_reference_runtime_manifest(std::string_view canonical_bytes) {
  if (canonical_bytes.empty() ||
      canonical_bytes.size() > kMaxReferenceRuntimeManifestBytes) {
    fail_retained_bundle(
        "reference runtime manifest is empty or exceeds its byte bound");
  }
  CanonicalValue root;
  try {
    root = parse_strict_json_model_yaml(canonical_bytes).root();
  } catch (const std::exception &error) {
    fail_retained_bundle(
        std::string("reference runtime manifest is invalid: ") + error.what());
  }
  if (canonical_json(root) != canonical_bytes) {
    fail_retained_bundle("reference runtime manifest is not canonical JSON");
  }
  const auto &object = require_object(root, "runtime_manifest");
  require_exact_fields(object,
                       {"api_version", "runtime_unit_id", "runtime_product_id",
                        "runtime_profile", "process_adapter",
                        "runtime_role_mappings", "source_endpoint_mappings",
                        "launchable_artifact", "platform_constraints",
                        "runtime_contract_offer"},
                       "runtime_manifest");
  if (require_string(require_member(object, "api_version", "runtime_manifest"),
                     "runtime_manifest.api_version") !=
      kReferenceRuntimeManifestApiVersion) {
    fail_retained_bundle(
        "reference runtime manifest has an unsupported API version");
  }
  const std::string runtime_unit_id =
      require_identity(object, "runtime_unit_id", "runtime_manifest");
  if (runtime_unit_id != kReferenceRuntimeUnitId ||
      require_identity(object, "runtime_product_id", "runtime_manifest") !=
          reference_runtime_product_id(core::RuntimeUnitId(runtime_unit_id))) {
    fail_retained_bundle(
        "reference runtime manifest has an unqualified subject identity");
  }
  const auto embedded_offer = decode_runtime_contract_offer_projection(
      require_member(object, "runtime_contract_offer", "runtime_manifest"),
      "runtime_manifest.runtime_contract_offer");
  const auto& runtime_contract = require_runtime_contract(
      core::RuntimeProfileId(std::string(kReferenceRuntimeProfileId)),
      core::RuntimeProfileRevision(reference_runtime_profile_revision()),
      core::ProductContractId(
          std::string(kReferenceRuntimeProductContractId)),
      reference_runtime_product_contract_revision());
  if (embedded_offer != runtime_contract.offer()) {
    fail_retained_bundle(
        "reference runtime embedded offer differs from package offer");
  }

  const auto &profile = require_object(
      require_member(object, "runtime_profile", "runtime_manifest"),
      "runtime_manifest.runtime_profile");
  require_exact_fields(profile,
                       {"runtime_profile_id", "runtime_profile_revision",
                        "implementation_id", "implementation_revision"},
                       "runtime_manifest.runtime_profile");
  if (require_identity(profile, "runtime_profile_id",
                       "runtime_manifest.runtime_profile") !=
          kReferenceRuntimeProfileId ||
      require_digest(profile, "runtime_profile_revision",
                     "runtime_manifest.runtime_profile") !=
          reference_runtime_profile_revision() ||
      require_identity(profile, "implementation_id",
                       "runtime_manifest.runtime_profile") !=
          kReferenceRuntimeImplementationId ||
      require_digest(profile, "implementation_revision",
                     "runtime_manifest.runtime_profile") !=
          reference_runtime_implementation_revision()) {
    fail_retained_bundle(
        "reference runtime manifest selects an unqualified implementation");
  }

  const auto &adapter = require_object(
      require_member(object, "process_adapter", "runtime_manifest"),
      "runtime_manifest.process_adapter");
  require_exact_fields(adapter,
                       {"lifecycle_control", "process_adapter_contract_id",
                        "process_adapter_contract_revision",
                        "retrieval_contract_id", "retrieval_contract_revision"},
                       "runtime_manifest.process_adapter");
  if (require_string(require_member(adapter, "lifecycle_control",
                                    "runtime_manifest.process_adapter"),
                     "runtime_manifest.process_adapter.lifecycle_control") !=
          "manager_managed" ||
      require_identity(adapter, "process_adapter_contract_id",
                       "runtime_manifest.process_adapter") !=
          kReferenceRuntimeProcessAdapterContractId ||
      require_digest(adapter, "process_adapter_contract_revision",
                     "runtime_manifest.process_adapter") !=
          reference_runtime_process_adapter_revision() ||
      require_identity(adapter, "retrieval_contract_id",
                       "runtime_manifest.process_adapter") !=
          kReferenceRuntimeRetrievalContractId ||
      require_digest(adapter, "retrieval_contract_revision",
                     "runtime_manifest.process_adapter") !=
          reference_runtime_retrieval_contract_revision()) {
    fail_retained_bundle(
        "reference runtime manifest selects an unqualified process adapter");
  }

  const auto &roles = require_array(
      require_member(object, "runtime_role_mappings", "runtime_manifest"),
      "runtime_manifest.runtime_role_mappings");
  if (roles.size() != 1U) {
    fail_retained_bundle(
        "reference runtime manifest requires one total runtime-role mapping");
  }
  const auto &role = require_object(roles.front(), "runtime_role_mapping");
  require_exact_fields(role, {"runtime_role_id", "implementation_role_id"},
                       "runtime_role_mapping");
  if (require_identity(role, "runtime_role_id", "runtime_role_mapping") !=
          kReferenceRuntimeRoleId ||
      require_identity(role, "implementation_role_id",
                       "runtime_role_mapping") !=
          kReferenceRuntimeImplementationId) {
    fail_retained_bundle(
        "reference runtime manifest has an unqualified runtime-role map");
  }

  const auto &endpoints = require_array(
      require_member(object, "source_endpoint_mappings", "runtime_manifest"),
      "runtime_manifest.source_endpoint_mappings");
  if (endpoints.size() != 1U) {
    fail_retained_bundle(
        "reference runtime manifest requires one total source endpoint map");
  }
  const auto &endpoint =
      require_object(endpoints.front(), "source_endpoint_mapping");
  require_exact_fields(endpoint,
                       {"source_endpoint_role_id", "endpoint_contract_id",
                        "endpoint_contract_revision", "transport"},
                       "source_endpoint_mapping");
  if (require_identity(endpoint, "source_endpoint_role_id",
                       "source_endpoint_mapping") !=
          kReferenceRuntimeStatusEndpointRoleId ||
      require_identity(endpoint, "endpoint_contract_id",
                       "source_endpoint_mapping") !=
          kReferenceRuntimeStatusEndpointContractId ||
      require_digest(endpoint, "endpoint_contract_revision",
                     "source_endpoint_mapping") !=
          reference_runtime_status_endpoint_revision() ||
      require_string(
          require_member(endpoint, "transport", "source_endpoint_mapping"),
          "source_endpoint_mapping.transport") != "stdout_line") {
    fail_retained_bundle(
        "reference runtime manifest has an unqualified source endpoint map");
  }

  const auto &launchable = require_object(
      require_member(object, "launchable_artifact", "runtime_manifest"),
      "runtime_manifest.launchable_artifact");
  require_exact_fields(launchable,
                       {"artifact_role_id", "media_type", "content_digest"},
                       "runtime_manifest.launchable_artifact");
  if (require_identity(launchable, "artifact_role_id",
                       "runtime_manifest.launchable_artifact") !=
          kReferenceRuntimeLaunchableArtifactRole ||
      require_string(require_member(launchable, "media_type",
                                    "runtime_manifest.launchable_artifact"),
                     "runtime_manifest.launchable_artifact.media_type") !=
          kReferenceRuntimeLaunchableMediaType ||
      require_digest(launchable, "content_digest",
                     "runtime_manifest.launchable_artifact") !=
          sha256_digest(reference_runtime_launchable_script())) {
    fail_retained_bundle(
        "reference runtime manifest has an unqualified launchable artifact");
  }

  const auto &platform = require_object(
      require_member(object, "platform_constraints", "runtime_manifest"),
      "runtime_manifest.platform_constraints");
  require_exact_fields(platform, {"operating_system", "shell_contract"},
                       "runtime_manifest.platform_constraints");
  if (require_identity(platform, "operating_system",
                       "runtime_manifest.platform_constraints") !=
          kReferenceRuntimeOperatingSystem ||
      require_string(require_member(platform, "shell_contract",
                                    "runtime_manifest.platform_constraints"),
                     "runtime_manifest.platform_constraints.shell_contract") !=
          kReferenceRuntimeShellContract) {
    fail_retained_bundle(
        "reference runtime manifest has incompatible platform constraints");
  }
  return root;
}

void require_reference_runtime_product_matches_retained_bundle(
    std::string_view manifest_bytes, std::string_view launchable_bytes,
    const core::RetainedBundleDTO &retained_bundle,
    const core::RuntimeUnitId &runtime_unit_id) {
  const CanonicalValue manifest =
      parse_and_verify_reference_runtime_manifest(manifest_bytes);
  if (launchable_bytes != reference_runtime_launchable_script() ||
      launchable_bytes.size() > kMaxReferenceRuntimeLaunchableBytes) {
    fail_retained_bundle(
        "reference runtime launchable bytes differ from the build-owned "
        "artifact");
  }
  const auto &unit =
      require_runtime_unit(retained_bundle.cell_graph, runtime_unit_id);
  const auto &root = require_object(manifest, "runtime_manifest");
  const auto &profile = require_object(
      require_member(root, "runtime_profile", "runtime_manifest"),
      "runtime_manifest.runtime_profile");
  if (runtime_unit_id.value() != kReferenceRuntimeUnitId ||
      unit.runtime_profile_id.value() != kReferenceRuntimeProfileId ||
      unit.runtime_profile_revision.value() !=
          reference_runtime_profile_revision() ||
      !unit.dependency_runtime_unit_ids.empty() ||
      !unit.configuration.empty() ||
      require_identity(root, "runtime_unit_id", "runtime_manifest") !=
          unit.runtime_unit_id.value() ||
      require_identity(profile, "runtime_profile_id",
                       "runtime_manifest.runtime_profile") !=
          unit.runtime_profile_id.value() ||
      require_digest(profile, "runtime_profile_revision",
                     "runtime_manifest.runtime_profile") !=
          unit.runtime_profile_revision.value()) {
    fail_retained_bundle(
        "reference runtime manifest differs from its retained graph/profile");
  }

  const auto &group =
      require_runtime_product_group(retained_bundle, runtime_unit_id);
  const auto &product = group.products.front();
  if (product.product_id.value() !=
          reference_runtime_product_id(runtime_unit_id) ||
      product.product_kind !=
          core::RetainedProductKind::kRuntimeUnitConfiguration ||
      product.product_contract_id.value() !=
          kReferenceRuntimeProductContractId ||
      product.product_contract_revision !=
          reference_runtime_product_contract_revision() ||
      product.artifact_refs.size() != 2U) {
    fail_retained_bundle(
        "reference runtime retained product has an incoherent identity");
  }
  const auto &manifest_ref =
      require_artifact(product, kReferenceRuntimeManifestArtifactRole);
  const auto &launchable_ref =
      require_artifact(product, kReferenceRuntimeLaunchableArtifactRole);
  if (manifest_ref.media_type != kReferenceRuntimeManifestMediaType ||
      manifest_ref.content_digest.value() != sha256_digest(manifest_bytes) ||
      manifest_ref.byte_size != manifest_bytes.size() ||
      launchable_ref.media_type != kReferenceRuntimeLaunchableMediaType ||
      launchable_ref.content_digest.value() !=
          sha256_digest(launchable_bytes) ||
      launchable_ref.byte_size != launchable_bytes.size()) {
    fail_retained_bundle(
        "reference runtime product artifact table differs from its bytes");
  }

  bool source_found = false;
  for (const auto &source : retained_bundle.provenance.source_records) {
    std::visit(
        [&](const auto &typed) {
          using Source = std::remove_cvref_t<decltype(typed)>;
          if constexpr (Source::kRecordKind ==
                        core::CatalogRecordKind::kRuntimeProfile) {
            if (typed.record_id == unit.runtime_profile_id &&
                typed.revision == unit.runtime_profile_revision) {
              if (source_found) {
                fail_retained_bundle(
                    "reference runtime profile source is duplicated");
              }
              source_found = true;
            }
          }
        },
        source);
  }
  const auto output = std::find_if(
      retained_bundle.provenance.output_records.begin(),
      retained_bundle.provenance.output_records.end(),
      [&](const auto &row) { return row.product_id == product.product_id; });
  if (!source_found ||
      output == retained_bundle.provenance.output_records.end() ||
      output->product_kind != product.product_kind ||
      output->content_digest != retained_product_output_digest(product)) {
    fail_retained_bundle(
        "reference runtime product lacks exact source/output provenance");
  }
}

} // namespace manyforge::deployment::detail
