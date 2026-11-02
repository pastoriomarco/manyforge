#include "manyforge_headless/node_environment_projection.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_deployment/artifact_codec.hpp>

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

constexpr std::size_t kMaximumProjectionInputBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumTextBytes = 1024U;
constexpr std::size_t kMaximumRegistryRows = 4096U;
constexpr std::size_t kMaximumPackageRows = 256U;
constexpr std::size_t kMaximumSourceRows = 65536U;
constexpr std::string_view kManifestApiVersion =
    "manyforge.node_environment_manifest.v1";
constexpr std::string_view kCatalogApiVersion = "manyforge.node_catalog.v2";

[[noreturn]] void Fail(const std::string &detail) {
  throw NodeEnvironmentProjectionError("node environment projection: " +
                                       detail);
}

void Require(const bool condition, const std::string &detail) {
  if (!condition) {
    Fail(detail);
  }
}

const CanonicalValue &RequiredMember(const CanonicalValue &root,
                                     const std::string_view name) {
  const auto *member = root.find_object_member(name);
  Require(member != nullptr, "missing field " + std::string(name));
  return *member;
}

void RequireClosedObject(const CanonicalValue &root,
                         const std::initializer_list<std::string_view> fields,
                         const std::string_view label) {
  Require(root.is_object() && root.as_object().size() == fields.size(),
          std::string(label) + " is not the closed shape");
  for (const auto field : fields) {
    static_cast<void>(RequiredMember(root, field));
  }
}

std::string
RequiredString(const CanonicalValue &root, const std::string_view name,
               const std::size_t maximum_bytes = kMaximumTextBytes) {
  const auto *value =
      std::get_if<std::string>(&RequiredMember(root, name).storage());
  Require(value != nullptr && !value->empty() &&
              value->size() <= maximum_bytes &&
              content_identity::is_utf8_nfc(*value),
          "field " + std::string(name) + " must be one bounded NFC string");
  return *value;
}

const CanonicalValue::Array &RequiredArray(const CanonicalValue &root,
                                           const std::string_view name,
                                           const std::size_t maximum_rows) {
  const auto *values =
      std::get_if<CanonicalValue::Array>(&RequiredMember(root, name).storage());
  Require(values != nullptr && !values->empty() &&
              values->size() <= maximum_rows,
          "field " + std::string(name) + " violates its array bound");
  return *values;
}

bool IsPortableIdentifier(const std::string_view value) noexcept {
  if (value.empty() || value.front() < 'a' || value.front() > 'z') {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](const char value) {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
           value == '_' || value == '.' || value == '-';
  });
}

std::string RequiredIdentifier(const CanonicalValue &root,
                               const std::string_view name) {
  auto value = RequiredString(root, name);
  Require(IsPortableIdentifier(value),
          "field " + std::string(name) +
              " must be a portable lowercase identifier");
  return value;
}

std::string RequiredDigest(const CanonicalValue &root,
                           const std::string_view name) {
  auto value = RequiredString(root, name, 71U);
  Require(content_identity::is_sha256_digest(value),
          "field " + std::string(name) +
              " must be an exact lowercase SHA-256 digest");
  return value;
}

CanonicalValue WithoutMember(const CanonicalValue &root,
                             const std::string_view excluded) {
  Require(root.is_object(), "identity root is not an object");
  CanonicalValue::Object result;
  result.reserve(root.as_object().size() - 1U);
  bool found = false;
  for (const auto &[name, value] : root.as_object()) {
    if (name == excluded) {
      found = true;
    } else {
      result.emplace_back(name, value);
    }
  }
  Require(found, "identity root is missing " + std::string(excluded));
  return CanonicalValue(std::move(result));
}

std::string ReadRegularFile(const std::filesystem::path &path) {
  const int descriptor =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    Fail("cannot open " + path.string() + ": " +
         std::system_error(errno, std::generic_category()).what());
  }
  struct Descriptor final {
    int value;
    ~Descriptor() { static_cast<void>(::close(value)); }
  } file{descriptor};
  struct stat status {};
  if (::fstat(file.value, &status) != 0) {
    Fail("cannot inspect " + path.string() + ": " +
         std::system_error(errno, std::generic_category()).what());
  }
  Require(S_ISREG(status.st_mode) && status.st_size > 0 &&
              static_cast<std::uint64_t>(status.st_size) <=
                  kMaximumProjectionInputBytes,
          path.string() + " must be one bounded regular file");
  std::string bytes;
  bytes.reserve(static_cast<std::size_t>(status.st_size));
  char buffer[8192];
  while (bytes.size() < static_cast<std::size_t>(status.st_size)) {
    const auto count = ::read(file.value, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      Fail("cannot read " + path.string() + ": " +
           std::system_error(errno, std::generic_category()).what());
    }
    if (count == 0) {
      break;
    }
    bytes.append(buffer, static_cast<std::size_t>(count));
    Require(bytes.size() <= kMaximumProjectionInputBytes,
            path.string() + " exceeded its byte bound while reading");
  }
  Require(bytes.size() == static_cast<std::size_t>(status.st_size),
          path.string() + " changed size while being read");
  return bytes;
}

CanonicalValue LoadStrictFile(const std::filesystem::path &path) {
  try {
    return manyforge::deployment::parse_strict_json_model_yaml(
               ReadRegularFile(path))
        .root();
  } catch (const NodeEnvironmentProjectionError &) {
    throw;
  } catch (const std::exception &error) {
    Fail("cannot decode " + path.string() + ": " + error.what());
  }
}

struct CatalogIdentity final {
  std::string catalog_id;
  std::string content_version;
  std::string catalog_digest;
  std::set<std::string> owner_neutral_node_ids;
};

bool IsOwnerNeutralOwnerApi(const std::string_view owner_api) noexcept {
  return owner_api == "geometry.compare" ||
         owner_api == "orchestration.blackboard" ||
         owner_api == "orchestration.clock" ||
         owner_api == "orchestration.control_flow";
}

CatalogIdentity VerifyCatalog(const CanonicalValue &catalog) {
  RequireClosedObject(catalog,
                      {"api_version", "content_version", "catalog_id",
                       "catalog_digest", "schema_types", "entries",
                       "skill_cross_references"},
                      "node catalog");
  Require(RequiredString(catalog, "api_version") == kCatalogApiVersion,
          "node catalog api_version is unsupported");
  const auto catalog_id = RequiredIdentifier(catalog, "catalog_id");
  const auto content_version = RequiredString(catalog, "content_version");
  const auto declared_digest = RequiredDigest(catalog, "catalog_digest");
  static_cast<void>(RequiredMember(catalog, "schema_types").as_object());
  std::set<std::string> node_ids;
  std::set<std::string> owner_neutral_node_ids;
  for (const auto &entry :
       RequiredArray(catalog, "entries", kMaximumRegistryRows)) {
    const auto node_id = RequiredIdentifier(entry, "id");
    Require(node_ids.insert(node_id).second,
            "node catalog contains a duplicate node ID");
    const auto &execution = RequiredMember(entry, "execution");
    const auto owner_api = RequiredIdentifier(execution, "owner_api");
    if (IsOwnerNeutralOwnerApi(owner_api)) {
      owner_neutral_node_ids.insert(node_id);
    }
  }
  const auto *cross_references = std::get_if<CanonicalValue::Array>(
      &RequiredMember(catalog, "skill_cross_references").storage());
  Require(cross_references != nullptr,
          "skill_cross_references must be an array");
  // The identity is the digest computed from the content; a stale stored
  // digest is not a refusal (the Python loader reports it).
  static_cast<void>(declared_digest);
  auto computed =
      content_identity::sha256_digest(content_identity::canonical_json(
          WithoutMember(catalog, "catalog_digest")));
  return {catalog_id, content_version, std::move(computed),
          std::move(owner_neutral_node_ids)};
}

struct RegistryRow final {
  std::string node_id;
  std::string engine_id;
  std::string implementation_id;
  std::string executable_package_id;

  friend bool operator==(const RegistryRow &, const RegistryRow &) = default;
  friend auto operator<=>(const RegistryRow &, const RegistryRow &) = default;
};

struct ManifestIdentity final {
  std::string environment_digest;
  CatalogIdentity catalog;
  std::vector<RegistryRow> registry;
};

ManifestIdentity VerifyManifest(const CanonicalValue &manifest) {
  RequireClosedObject(manifest,
                      {"api_version", "environment_digest", "catalog",
                       "executable_registry", "executable_packages",
                       "source_coverage"},
                      "node environment manifest");
  Require(RequiredString(manifest, "api_version") == kManifestApiVersion,
          "node environment manifest api_version is unsupported");
  const auto environment_digest =
      RequiredDigest(manifest, "environment_digest");
  const auto &catalog = RequiredMember(manifest, "catalog");
  RequireClosedObject(
      catalog,
      {"api_version", "catalog_id", "content_version", "catalog_digest"},
      "node environment manifest catalog");
  Require(RequiredString(catalog, "api_version") == kCatalogApiVersion,
          "node environment manifest catalog api_version is unsupported");
  CatalogIdentity catalog_identity{
      RequiredIdentifier(catalog, "catalog_id"),
      RequiredString(catalog, "content_version"),
      RequiredDigest(catalog, "catalog_digest"),
      {},
  };

  std::vector<RegistryRow> registry;
  const auto &registry_values =
      RequiredArray(manifest, "executable_registry", kMaximumRegistryRows);
  registry.reserve(registry_values.size());
  std::set<std::pair<std::string, std::string>> node_engine_keys;
  for (const auto &value : registry_values) {
    RequireClosedObject(
        value,
        {"node_id", "engine_id", "implementation_id", "executable_package_id"},
        "executable registry row");
    RegistryRow row{
        RequiredIdentifier(value, "node_id"),
        RequiredIdentifier(value, "engine_id"),
        RequiredIdentifier(value, "implementation_id"),
        RequiredIdentifier(value, "executable_package_id"),
    };
    Require(node_engine_keys.emplace(row.node_id, row.engine_id).second,
            "executable registry contains a duplicate node/engine key");
    registry.push_back(std::move(row));
  }
  Require(std::is_sorted(registry.begin(), registry.end()),
          "executable registry is not canonical");

  std::set<std::string> package_ids;
  std::vector<std::tuple<std::string, std::string, std::string, std::string>>
      packages;
  for (const auto &value :
       RequiredArray(manifest, "executable_packages", kMaximumPackageRows)) {
    RequireClosedObject(value,
                        {"executable_package_id", "package_version",
                         "package_format", "content_digest"},
                        "executable package row");
    auto row =
        std::make_tuple(RequiredIdentifier(value, "executable_package_id"),
                        RequiredString(value, "package_version"),
                        RequiredString(value, "package_format"),
                        RequiredDigest(value, "content_digest"));
    Require(package_ids.insert(std::get<0>(row)).second,
            "executable package table contains a duplicate ID");
    packages.push_back(std::move(row));
  }
  Require(std::is_sorted(packages.begin(), packages.end()),
          "executable package table is not canonical");

  std::set<std::pair<std::string, std::string>> source_keys;
  std::vector<std::tuple<std::string, std::string, std::string>> sources;
  for (const auto &value :
       RequiredArray(manifest, "source_coverage", kMaximumSourceRows)) {
    RequireClosedObject(
        value, {"executable_package_id", "source_id", "content_digest"},
        "source coverage row");
    auto row =
        std::make_tuple(RequiredIdentifier(value, "executable_package_id"),
                        RequiredString(value, "source_id"),
                        RequiredDigest(value, "content_digest"));
    Require(source_keys.emplace(std::get<0>(row), std::get<1>(row)).second,
            "source coverage contains a duplicate package/source key");
    sources.push_back(std::move(row));
  }
  Require(std::is_sorted(sources.begin(), sources.end()),
          "source coverage is not canonical");
  std::set<std::string> referenced_packages;
  for (const auto &row : registry) {
    referenced_packages.insert(row.executable_package_id);
  }
  for (const auto &row : sources) {
    referenced_packages.insert(std::get<0>(row));
  }
  Require(referenced_packages == package_ids,
          "node environment package join is not exact and total");

  const auto computed = content_identity::framed_sha256(
      "node-environment-digest", kManifestApiVersion,
      WithoutMember(manifest, "environment_digest"));
  Require(content_identity::digest_equal(computed, environment_digest),
          "node environment digest verification failed");
  return {environment_digest, std::move(catalog_identity), std::move(registry)};
}

CanonicalValue
StringArray(const std::initializer_list<std::string_view> values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto value : values) {
    result.emplace_back(CanonicalValue(std::string(value)));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue StringArray(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (auto &value : values) {
    result.emplace_back(CanonicalValue(std::move(value)));
  }
  return CanonicalValue(std::move(result));
}

// Project only offers for the capability versions understood by this build.
// Both collections belong to the same verified active retained bundle.
CanonicalValue CapabilityOffers(const manyforge::core::RetainedBundleDTO &bundle) {
  std::vector<std::string> offers;
  const auto add = [&offers](const auto &binding, const auto kind) {
    const auto *definition = manyforge::core::find_builtin_capability_definition(
        binding.capability_id.value());
    if (definition != nullptr &&
        definition->capability_definition_revision ==
            binding.capability_definition_revision.value() &&
        definition->provider_binding_kind == kind) {
      offers.push_back(binding.capability_id.value());
    }
  };
  for (const auto &binding : bundle.cell_graph.control_bindings) {
    add(binding, manyforge::core::CapabilityProviderBindingKind::kControlBinding);
  }
  for (const auto &binding : bundle.capability_provider_bindings) {
    const auto *definition = manyforge::core::find_builtin_capability_definition(
        binding.capability_id.value());
    if (definition != nullptr && definition->scope == binding.scope) {
      add(binding, manyforge::core::CapabilityProviderBindingKind::kCapabilityProviderBinding);
    }
  }
  return StringArray(std::move(offers));
}

struct DeferredAdapter final {
  std::string_view node_id;
  std::string_view reason_code;
  std::string_view milestone;
  std::string_view gate;
};

constexpr std::array<DeferredAdapter, 0U> kDeferredAdapters{};

const DeferredAdapter *DeferredDisposition(const std::string_view node_id) {
  const auto found = std::lower_bound(
      kDeferredAdapters.begin(), kDeferredAdapters.end(), node_id,
      [](const DeferredAdapter &row, const std::string_view candidate) {
        return row.node_id < candidate;
      });
  if (found == kDeferredAdapters.end() || found->node_id != node_id) {
    return nullptr;
  }
  return &*found;
}

CanonicalValue Availability(const std::vector<RegistryRow> &registry,
                            const std::set<std::string> &owner_neutral_node_ids,
                            const bool manager_runtime_converged) {
  CanonicalValue::Array implementations;
  implementations.reserve(registry.size());
  for (const auto &row : registry) {
    const auto *deferred = DeferredDisposition(row.node_id);
    const bool owner_health_available =
        deferred == nullptr && owner_neutral_node_ids.contains(row.node_id);
    std::vector<std::string> reason_codes{"program_binding_unselected"};
    if (!owner_health_available) {
      reason_codes.emplace_back("program_runtime_owner_unavailable");
    }
    CanonicalValue restoration_gates(CanonicalValue::Array{});
    if (deferred != nullptr) {
      reason_codes.emplace_back(deferred->reason_code);
      restoration_gates = StringArray({deferred->milestone, deferred->gate});
    }
    implementations.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"adapter_build_qualified", CanonicalValue(deferred == nullptr)},
        {"authoring_available", CanonicalValue(true)},
        {"binding_available", CanonicalValue(false)},
        {"engine_id", CanonicalValue(row.engine_id)},
        {"executable_package_id", CanonicalValue(row.executable_package_id)},
        {"execution_available", CanonicalValue(false)},
        {"implementation_id", CanonicalValue(row.implementation_id)},
        {"implementation_identity_resolved", CanonicalValue(true)},
        {"node_id", CanonicalValue(row.node_id)},
        {"owner_health_available", CanonicalValue(owner_health_available)},
        {"reason_codes", StringArray(std::move(reason_codes))},
        {"restoration_gates", std::move(restoration_gates)},
    }));
  }
  CanonicalValue::Array reason_codes;
  if (!manager_runtime_converged) {
    reason_codes.emplace_back(CanonicalValue("manager_runtime_unavailable"));
  }
  reason_codes.emplace_back(CanonicalValue("program_binding_unselected"));
  reason_codes.emplace_back(
      CanonicalValue("program_runtime_owner_unavailable"));
  return CanonicalValue(CanonicalValue::Object{
      {"binding_context", CanonicalValue("unselected")},
      {"execution_available", CanonicalValue(false)},
      {"implementations", CanonicalValue(std::move(implementations))},
      {"manager_runtime_converged", CanonicalValue(manager_runtime_converged)},
      {"portable_authoring_available", CanonicalValue(true)},
      {"reason_codes", CanonicalValue(std::move(reason_codes))},
      {"restoration_gates", StringArray({"I6", "N"})},
  });
}

} // namespace

struct ActiveNodeEnvironmentProjection::Storage final {
  explicit Storage(std::filesystem::path package,
                   std::filesystem::path catalog_path,
                   std::filesystem::path environment_manifest_path)
      : package_root(std::move(package)), catalog(LoadStrictFile(catalog_path)),
        catalog_identity(VerifyCatalog(catalog)),
        current_manifest(LoadStrictFile(environment_manifest_path)),
        current_identity(VerifyManifest(current_manifest)) {
    const auto status = std::filesystem::symlink_status(package_root);
    Require(std::filesystem::is_directory(status) &&
                !std::filesystem::is_symlink(status),
            "package root must be one existing non-symlink directory");
    Require(current_identity.catalog.catalog_id == catalog_identity.catalog_id &&
                current_identity.catalog.content_version ==
                    catalog_identity.content_version &&
                content_identity::digest_equal(
                    current_identity.catalog.catalog_digest,
                    catalog_identity.catalog_digest),
            "current node environment manifest and catalog disagree");
  }

  const CanonicalValue &Manifest(const std::string &environment_digest) const {
    std::scoped_lock lock(mutex);
    if (const auto found = manifests.find(environment_digest);
        found != manifests.end()) {
      return found->second;
    }
    Require(content_identity::is_sha256_digest(environment_digest),
            "active environment digest is invalid");
    const auto path = package_root / "build-manifests" / "node_environment" /
                      (environment_digest.substr(7U) + ".json");
    const auto bytes = ReadRegularFile(path);
    CanonicalValue manifest;
    try {
      manifest =
          manyforge::deployment::parse_strict_json_model_yaml(bytes).root();
    } catch (const std::exception &error) {
      Fail("cannot decode retained node environment manifest: " +
           std::string(error.what()));
    }
    Require(content_identity::canonical_json(manifest) == bytes,
            "retained node environment manifest is not canonical JSON");
    const auto identity = VerifyManifest(manifest);
    Require(content_identity::digest_equal(identity.environment_digest,
                                           environment_digest),
            "retained node environment manifest key/content mismatch");
    return manifests.emplace(environment_digest, std::move(manifest))
        .first->second;
  }

  std::filesystem::path package_root;
  CanonicalValue catalog;
  CatalogIdentity catalog_identity;
  CanonicalValue current_manifest;
  ManifestIdentity current_identity;
  mutable std::mutex mutex;
  mutable std::map<std::string, CanonicalValue> manifests;
};

ActiveNodeEnvironmentProjection::ActiveNodeEnvironmentProjection(
    std::filesystem::path package_root, std::filesystem::path node_catalog_path,
    std::filesystem::path node_environment_manifest_path)
    : storage_(std::make_unique<Storage>(std::move(package_root),
                                         std::move(node_catalog_path),
                                         std::move(node_environment_manifest_path))) {}

ActiveNodeEnvironmentProjection::~ActiveNodeEnvironmentProjection() = default;

CanonicalValue ActiveNodeEnvironmentProjection::Project(
    const manyforge::deployment::RetainedBundle &active_bundle,
    const bool manager_runtime_converged) const {
  Require(manyforge::deployment::verify_retained_bundle_identity(active_bundle),
          "active retained bundle failed identity verification");
  const auto &environment = active_bundle.value().catalogs.node_environment;
  const auto &manifest =
      storage_->Manifest(environment.environment_digest.value());
  const auto manifest_identity = VerifyManifest(manifest);
  Require(manifest_identity.catalog.catalog_id == environment.catalog_id &&
              manifest_identity.catalog.content_version ==
                  environment.content_version &&
              content_identity::digest_equal(
                  manifest_identity.catalog.catalog_digest,
                  environment.catalog_digest.value()),
          "active retained bundle and node environment manifest disagree");

  return CanonicalValue(CanonicalValue::Object{
      {"availability",
       Availability(storage_->current_identity.registry,
                    storage_->catalog_identity.owner_neutral_node_ids,
                    manager_runtime_converged)},
      {"capability_offers", CapabilityOffers(active_bundle.value())},
      {"catalog", storage_->catalog},
      {"catalog_digest",
       CanonicalValue(storage_->catalog_identity.catalog_digest)},
      {"catalog_id", CanonicalValue(storage_->catalog_identity.catalog_id)},
      {"content_version",
       CanonicalValue(storage_->catalog_identity.content_version)},
      {"environment_digest",
       CanonicalValue(storage_->current_identity.environment_digest)},
      {"graph_revision",
       CanonicalValue(active_bundle.value().cell_graph.graph_revision.value())},
      {"manifest", storage_->current_manifest},
      {"package_environment",
       CanonicalValue(CanonicalValue::Object{
           {"catalog_digest", CanonicalValue(environment.catalog_digest.value())},
           {"catalog_id", CanonicalValue(environment.catalog_id)},
           {"content_version", CanonicalValue(environment.content_version)},
           {"environment_digest",
            CanonicalValue(environment.environment_digest.value())},
       })},
      {"retained_bundle_id",
       CanonicalValue(active_bundle.retained_bundle_id().value())},
      {"schema_version",
       CanonicalValue(std::string(kNodeEnvironmentProjectionApiVersion))},
  });
}

} // namespace manyforge::headless
