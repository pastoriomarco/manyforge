#include "manyforge_deployment/activation_content_collection.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "catalog_artifact_store_internal.hpp"

namespace manyforge::deployment {
namespace {

// Liveness is decided by name: every store names its entries by a 64-digit
// lowercase hex digest (a catalog artifact by the digest its opaque key
// carries), and every reference to an entry spells that digest. A token is a
// maximal run of exactly 64 lowercase hex digits in a live document.
constexpr std::size_t kHexLength = 64U;
constexpr std::string_view kWriterLockFilename = ".activation-content.lock";
constexpr std::string_view kAdmissionPrefix = "canonical-empty-admission";

struct Namespace final {
  std::string_view directory;
  std::string_view data_directory;
  std::string_view suffix;
};

constexpr Namespace kBundles{"retained-bundles", "sha256", ".json"};
constexpr Namespace kPrograms{"programs", "sha256", ".json"};
constexpr Namespace kBindings{"program-bindings", "sha256", ".json"};
constexpr Namespace kRawArtifacts{"raw-artifacts", "sha256", ".blob"};
constexpr Namespace kCatalogArtifacts{
    detail::kCatalogArtifactNamespaceDirectory,
    detail::kCatalogArtifactDataDirectory, detail::kCatalogArtifactFileSuffix};

using Names = std::unordered_set<std::string>;

[[nodiscard]] bool is_hex(char character) noexcept {
  return (character >= '0' && character <= '9') ||
         (character >= 'a' && character <= 'f');
}

void add_tokens(std::string_view bytes, Names &tokens) {
  std::size_t index = 0U;
  while (index < bytes.size()) {
    if (!is_hex(bytes[index])) {
      ++index;
      continue;
    }
    std::size_t end = index;
    while (end < bytes.size() && is_hex(bytes[end]))
      ++end;
    if (end - index == kHexLength)
      tokens.emplace(bytes.substr(index, kHexLength));
    index = end;
  }
}

[[nodiscard]] Names tokens_of(std::string_view bytes) {
  Names tokens;
  add_tokens(bytes, tokens);
  return tokens;
}

void add_digest(std::string_view digest, Names &names) {
  if (digest.size() >= kHexLength)
    add_tokens(digest.substr(digest.size() - kHexLength), names);
}

/// A live document that cannot be read fails the whole collection before
/// anything is removed: its references would otherwise be lost.
[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot open live activation content " +
                             path.string());
  std::string bytes{std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>()};
  if (input.bad())
    throw std::runtime_error("cannot read live activation content " +
                             path.string());
  return bytes;
}

[[nodiscard]] std::filesystem::path data_directory(
    const std::filesystem::path &root, const Namespace &space) {
  return root / std::string(space.directory) /
         std::string(space.data_directory);
}

/// The entries of one directory; none when it does not exist yet.
[[nodiscard]] std::vector<std::filesystem::path>
list_directory(const std::filesystem::path &directory) {
  std::vector<std::filesystem::path> result;
  std::error_code error;
  for (std::filesystem::directory_iterator entry(directory, error), end;
       !error && entry != end; entry.increment(error))
    result.push_back(entry->path());
  if (error && error != std::errc::no_such_file_or_directory)
    throw std::filesystem::filesystem_error("cannot list activation content",
                                            directory, error);
  return result;
}

/// The digest of each committed entry (temporaries and anything else are not
/// entries and are left to their store).
[[nodiscard]] std::vector<std::string>
committed_entries(const std::filesystem::path &root, const Namespace &space) {
  std::vector<std::string> result;
  for (const auto &path : list_directory(data_directory(root, space))) {
    const auto name = path.filename().string();
    if (name.size() == kHexLength + space.suffix.size() &&
        name.ends_with(space.suffix) &&
        tokens_of(name.substr(0U, kHexLength)).size() == 1U)
      result.push_back(name.substr(0U, kHexLength));
  }
  return result;
}

[[nodiscard]] std::filesystem::path entry_path(const std::filesystem::path &root,
                                               const Namespace &space,
                                               std::string_view hex) {
  return data_directory(root, space) /
         (std::string(hex) + std::string(space.suffix));
}

/// The digest tokens the opaque key of one catalog artifact carries, read
/// from its envelope header only.
[[nodiscard]] Names catalog_key_tokens(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  std::string header(detail::kCatalogArtifactEnvelopeMagic.size() +
                         sizeof(std::uint64_t),
                     '\0');
  if (!input.read(header.data(), static_cast<std::streamsize>(header.size())))
    return {};
  std::size_t offset = detail::kCatalogArtifactEnvelopeMagic.size();
  const auto key_size = detail::read_u64_be(header, offset);
  if (!header.starts_with(detail::kCatalogArtifactEnvelopeMagic) ||
      !key_size.has_value() ||
      *key_size > detail::kMaximumCatalogArtifactKeyBytes)
    return {};
  std::string key(static_cast<std::size_t>(*key_size), '\0');
  if (!input.read(key.data(), static_cast<std::streamsize>(key.size())))
    return {};
  return tokens_of(key);
}

/// Open and flock one lock file; -1 when a non-blocking lock is busy.
[[nodiscard]] int lock_file(const std::filesystem::path &path, int operation) {
  const int descriptor =
      ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (descriptor < 0)
    throw std::runtime_error("cannot open activation content lock " +
                             path.string() + ": " + std::strerror(errno));
  while (::flock(descriptor, operation) != 0) {
    const int saved = errno;
    if (saved == EINTR)
      continue;
    ::close(descriptor);
    if (saved == EWOULDBLOCK)
      return -1;
    throw std::runtime_error("cannot lock activation content " +
                             path.string() + ": " + std::strerror(saved));
  }
  return descriptor;
}

class Flock final {
public:
  Flock(const std::filesystem::path &path, int operation)
      : descriptor_(lock_file(path, operation)) {}
  ~Flock() {
    if (descriptor_ >= 0)
      ::close(descriptor_);
  }
  Flock(const Flock &) = delete;
  Flock &operator=(const Flock &) = delete;

  [[nodiscard]] bool held() const noexcept { return descriptor_ >= 0; }

private:
  int descriptor_;
};

/// Delete every committed entry of one store not named in `live`, under that
/// store's own repository lock (its readers and its index refresh take it).
void remove_unreferenced(const std::filesystem::path &root,
                         const Namespace &space, const Names &live,
                         ActivationContentCollection &result) {
  const auto directory = root / std::string(space.directory);
  if (!std::filesystem::is_directory(directory))
    return;
  const Flock store_lock(directory / ".repository.lock", LOCK_EX);
  const auto entries = committed_entries(root, space);
  result.examined_entries += entries.size();
  for (const auto &hex : entries) {
    if (live.contains(hex))
      continue;
    const auto path = entry_path(root, space, hex);
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    std::error_code remove_error;
    if (std::filesystem::remove(path, remove_error)) {
      ++result.removed_entries;
      if (!size_error)
        result.removed_bytes += static_cast<std::uint64_t>(size);
    }
  }
}

} // namespace

ActivationContentCollection collect_unreferenced_activation_content(
    const std::filesystem::path &activation_root,
    const ActiveActivationContent &active) {
  ActivationContentCollection result;
  const Flock writers(activation_root / std::string(kWriterLockFilename),
                      LOCK_EX | LOCK_NB);
  if (!writers.held())
    return result;
  result.collected = true;
  const auto read_live = [&](const std::filesystem::path &path) {
    auto bytes = read_file(path);
    result.read_bytes += bytes.size();
    return bytes;
  };

  // Roots: the active configuration, the retained prepared activations and
  // the canonical-empty admissions. Each names its bundle, Program and
  // binding (and the artifacts it carries) by digest.
  Names roots;
  for (const auto &bundle : active.retained_bundle_ids)
    add_digest(bundle.value(), roots);
  if (active.program_revision)
    add_digest(active.program_revision->value(), roots);
  if (active.program_binding_id)
    add_digest(active.program_binding_id->value(), roots);
  std::vector<std::filesystem::path> root_documents;
  for (const auto &path :
       list_directory(activation_root / "prepared-activations"))
    if (path.extension() == ".json")
      root_documents.push_back(path);
  for (const auto &path : list_directory(activation_root)) {
    const auto name = path.filename().string();
    if (name.starts_with(kAdmissionPrefix) && name.ends_with(".json"))
      root_documents.push_back(path);
  }
  Names tokens;
  for (const auto &path : root_documents)
    add_tokens(read_live(path), tokens);
  roots.insert(tokens.begin(), tokens.end());

  // Live bundles, Programs and bindings come from the roots only (a bundle
  // naming another bundle never keeps it); their documents and every live
  // artifact then name the raw and catalog artifacts they need.
  for (const auto *space : {&kBundles, &kPrograms, &kBindings})
    for (const auto &hex : committed_entries(activation_root, *space))
      if (roots.contains(hex))
        add_tokens(read_live(entry_path(activation_root, *space, hex)), tokens);

  const auto raw = committed_entries(activation_root, kRawArtifacts);
  std::vector<std::pair<std::string, Names>> catalog;
  for (auto &hex : committed_entries(activation_root, kCatalogArtifacts)) {
    auto key_tokens = catalog_key_tokens(
        entry_path(activation_root, kCatalogArtifacts, hex));
    catalog.emplace_back(std::move(hex), std::move(key_tokens));
  }
  Names live_raw;
  Names live_catalog;
  for (bool changed = true; changed;) {
    changed = false;
    for (const auto &hex : raw)
      if (!live_raw.contains(hex) && tokens.contains(hex)) {
        live_raw.insert(hex);
        add_tokens(read_live(entry_path(activation_root, kRawArtifacts, hex)),
                   tokens);
        changed = true;
      }
    for (const auto &[hex, key_tokens] : catalog) {
      if (live_catalog.contains(hex))
        continue;
      // A key that carries no digest cannot be traced; it is kept.
      bool referenced = key_tokens.empty();
      for (const auto &token : key_tokens)
        referenced = referenced || tokens.contains(token);
      if (referenced) {
        live_catalog.insert(hex);
        add_tokens(
            read_live(entry_path(activation_root, kCatalogArtifacts, hex)),
            tokens);
        changed = true;
      }
    }
  }

  remove_unreferenced(activation_root, kBundles, roots, result);
  remove_unreferenced(activation_root, kPrograms, roots, result);
  remove_unreferenced(activation_root, kBindings, roots, result);
  remove_unreferenced(activation_root, kRawArtifacts, live_raw, result);
  remove_unreferenced(activation_root, kCatalogArtifacts, live_catalog, result);
  return result;
}

ActivationContentWriterLock::ActivationContentWriterLock(
    const std::filesystem::path &activation_root)
    : descriptor_(lock_file(activation_root / std::string(kWriterLockFilename),
                            LOCK_SH)) {}

ActivationContentWriterLock::~ActivationContentWriterLock() {
  ::close(descriptor_);
}

} // namespace manyforge::deployment
