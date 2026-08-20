#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/retained_bundle.hpp>

#include "catalog_artifact_store_internal.hpp"
#include "scoped_procfd_root.hpp"

namespace manyforge::deployment {
namespace {

using namespace std::string_view_literals;

static_assert(
    std::is_base_of_v<ICatalogArtifactStore,
                      FilesystemCatalogArtifactStore>);
static_assert(
    std::is_base_of_v<ICatalogArtifactSource,
                      FilesystemVerifiedCatalogArtifactSource>);
static_assert(
    !std::is_copy_constructible_v<FilesystemCatalogArtifactStore>);
static_assert(
    !std::is_move_constructible_v<FilesystemCatalogArtifactStore>);

class ScopedTrustedRoot final {
 public:
  ScopedTrustedRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("manyforge-catalog-artifact-store-" +
               std::to_string(static_cast<long long>(::getpid())) + "-" +
               std::to_string(sequence_.fetch_add(1U)))) {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
    if (!std::filesystem::create_directory(path_, error) || error) {
      throw std::logic_error("test could not create trusted root");
    }
  }

  ~ScopedTrustedRoot() {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  inline static std::atomic<std::uint64_t> sequence_{0U};
  std::filesystem::path path_;
};

[[nodiscard]] std::filesystem::path data_directory(
    const std::filesystem::path& root) {
  return root /
         std::string(detail::kCatalogArtifactNamespaceDirectory) /
         std::string(detail::kCatalogArtifactDataDirectory);
}

[[nodiscard]] std::filesystem::path artifact_path(
    const std::filesystem::path& root, std::string_view key) {
  const auto hex = detail::catalog_artifact_storage_hex(key);
  if (!hex.has_value()) {
    throw std::logic_error("test key is invalid");
  }
  return data_directory(root) /
         (*hex + std::string(detail::kCatalogArtifactFileSuffix));
}

void write_file(const std::filesystem::path& path,
                std::string_view bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.good()) {
    throw std::logic_error("test could not open file");
  }
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream.good()) {
    throw std::logic_error("test could not write file");
  }
}

[[nodiscard]] std::string read_file(
    const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.good()) {
    throw std::logic_error("test could not read file");
  }
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

struct ProcessRaceResult final {
  std::size_t inserted{0U};
  std::size_t idempotent{0U};
  std::size_t failed{0U};
};

template <typename Put>
[[nodiscard]] ProcessRaceResult run_process_race(
    std::size_t process_count, Put put) {
  int start_pipe[2]{-1, -1};
  if (::pipe(start_pipe) != 0) {
    throw std::runtime_error("test could not create process barrier");
  }
  std::vector<pid_t> children;
  children.reserve(process_count);
  for (std::size_t index = 0U; index < process_count; ++index) {
    const pid_t child = ::fork();
    if (child < 0) {
      throw std::runtime_error("test could not fork writer");
    }
    if (child == 0) {
      (void)::close(start_pipe[1]);
      char token = '\0';
      ssize_t count = 0;
      do {
        count = ::read(start_pipe[0], &token, 1U);
      } while (count < 0 && errno == EINTR);
      (void)::close(start_pipe[0]);
      if (count != 1) {
        ::_exit(22);
      }
      try {
        ::_exit(put(index) ? 20 : 21);
      } catch (...) {
        ::_exit(22);
      }
    }
    children.push_back(child);
  }
  (void)::close(start_pipe[0]);
  const std::string tokens(process_count, 'x');
  std::size_t offset = 0U;
  while (offset < tokens.size()) {
    const ssize_t written =
        ::write(start_pipe[1], tokens.data() + offset,
                tokens.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      throw std::runtime_error("test could not release process barrier");
    }
    offset += static_cast<std::size_t>(written);
  }
  (void)::close(start_pipe[1]);

  ProcessRaceResult result;
  for (const pid_t child : children) {
    int status = 0;
    if (::waitpid(child, &status, 0) != child ||
        !WIFEXITED(status)) {
      ++result.failed;
    } else if (WEXITSTATUS(status) == 20) {
      ++result.inserted;
    } else if (WEXITSTATUS(status) == 21) {
      ++result.idempotent;
    } else {
      ++result.failed;
    }
  }
  return result;
}

TEST(FilesystemCatalogArtifactStoreTest,
     OpaqueNonDigestUnsafeKeyRoundTripsExactBytesAcrossRestart) {
  ScopedTrustedRoot root;
  const std::string key{
      "vendor://catalog/../../outside?record=one\0tail", 46U};
  const core::ContentAddressedStoreKey exact_key(key);
  const std::string payload{"exact\0catalog\0payload", 21U};
  const core::ContentDigest digest(sha256_digest(payload));

  StoredCatalogArtifact stored{
      core::ContentAddressedStoreKey("placeholder"),
      digest,
      0U,
      false,
  };
  {
    FilesystemCatalogArtifactStore store(root.path());
    stored = store.put_if_absent(exact_key, digest, payload);
    EXPECT_EQ(stored.content_addressed_store_key, exact_key);
    EXPECT_EQ(stored.content_digest, digest);
    EXPECT_EQ(stored.byte_size, payload.size());
    EXPECT_TRUE(stored.inserted);
    EXPECT_FALSE(
        store.put_if_absent(exact_key, digest, payload).inserted);
    EXPECT_EQ(store.size(), 1U);
    EXPECT_EQ(store.retained_bytes(), payload.size());
  }

  ASSERT_TRUE(std::filesystem::is_regular_file(
      artifact_path(root.path(), key)));
  const std::string filename =
      artifact_path(root.path(), key).filename().string();
  EXPECT_EQ(filename.size(),
            64U + detail::kCatalogArtifactFileSuffix.size());
  EXPECT_EQ(filename.find('/'), std::string::npos);

  FilesystemCatalogArtifactStore reopened(root.path());
  EXPECT_EQ(reopened.size(), 1U);
  EXPECT_EQ(reopened.retained_bytes(), payload.size());
  EXPECT_EQ(reopened.get_verified(exact_key, digest), payload);
  EXPECT_EQ(reopened.get_verified(exact_key, digest, payload.size()),
            payload);
  EXPECT_FALSE(reopened
                   .get_verified(exact_key, digest,
                                 payload.size() + 1U)
                   .has_value());
  EXPECT_FALSE(reopened
                   .get_verified(
                       exact_key,
                       core::ContentDigest(sha256_digest("different")))
                   .has_value());
  EXPECT_FALSE(reopened
                   .get_verified(
                       core::ContentAddressedStoreKey("other-key"),
                       digest)
                   .has_value());
}

TEST(FilesystemVerifiedCatalogArtifactSourceTest,
     ProcfdPointReadIsVerifiedAndDoesNotCreateAnAbsentNamespace) {
  ScopedTrustedRoot root;
  const core::ContentAddressedStoreKey key("catalog-key");
  const std::string payload("immutable catalog payload");
  const core::ContentDigest digest(sha256_digest(payload));
  {
    FilesystemCatalogArtifactStore writer(root.path());
    ASSERT_TRUE(writer.put_if_absent(key, digest, payload).inserted);
  }

  const auto entry_count_before = static_cast<std::size_t>(std::distance(
      std::filesystem::recursive_directory_iterator(root.path()),
      std::filesystem::recursive_directory_iterator()));
  test::ScopedProcfdRoot procfd(root.path());
  const FilesystemVerifiedCatalogArtifactSource source(procfd.path());
  test::ScopedPathReplacement replacement(root.path());
  EXPECT_EQ(source.get_verified(key, digest, payload.size()), payload);
  EXPECT_FALSE(source
                   .get_verified(
                       core::ContentAddressedStoreKey("absent-key"),
                       digest)
                   .has_value());
  EXPECT_EQ(static_cast<std::size_t>(std::distance(
      std::filesystem::recursive_directory_iterator(root.path()),
                std::filesystem::recursive_directory_iterator())),
            0U);
  EXPECT_EQ(static_cast<std::size_t>(std::distance(
                std::filesystem::recursive_directory_iterator(
                    replacement.backing()),
                std::filesystem::recursive_directory_iterator())),
            entry_count_before);

  ScopedTrustedRoot empty_root;
  test::ScopedProcfdRoot empty_procfd(empty_root.path());
  const FilesystemVerifiedCatalogArtifactSource empty_source(
      empty_procfd.path());
  EXPECT_FALSE(empty_source.get_verified(key, digest).has_value());
  EXPECT_TRUE(std::filesystem::is_empty(empty_root.path()));

  ScopedTrustedRoot unsafe_root;
  const auto outside = unsafe_root.path().parent_path() /
                       (unsafe_root.path().filename().string() + "-outside");
  std::filesystem::create_directory(outside);
  std::filesystem::create_directory_symlink(
      outside,
      unsafe_root.path() /
          std::string(detail::kCatalogArtifactNamespaceDirectory));
  test::ScopedProcfdRoot unsafe_procfd(unsafe_root.path());
  const FilesystemVerifiedCatalogArtifactSource unsafe_source(
      unsafe_procfd.path());
  EXPECT_THROW(static_cast<void>(unsafe_source.get_verified(key, digest)),
               RetainedBundleError);
  EXPECT_TRUE(std::filesystem::is_empty(outside));
  std::filesystem::remove_all(outside);
}

TEST(FilesystemCatalogArtifactStoreTest,
     RejectsInvalidIdentityAndEnforcesPayloadBoundsWithoutEviction) {
  ScopedTrustedRoot root;
  EXPECT_THROW(
      static_cast<void>(
          FilesystemCatalogArtifactStore(root.path(), 0U, 1U, 1U)),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemCatalogArtifactStore(root.path(), 1U, 0U, 1U)),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemCatalogArtifactStore(root.path(), 1U, 1U, 2U)),
      RetainedBundleError);

  FilesystemCatalogArtifactStore store(root.path(), 2U, 4U, 4U);
  const core::ContentDigest abc_digest(sha256_digest("abc"));
  EXPECT_THROW(
      static_cast<void>(store.put_if_absent(
          core::ContentAddressedStoreKey(""), abc_digest, "abc")),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(store.put_if_absent(
          core::ContentAddressedStoreKey(
              std::string(kMaxArtifactScalarBytes + 1U, 'k')),
          abc_digest, "abc")),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(store.put_if_absent(
          core::ContentAddressedStoreKey("invalid-digest"),
          core::ContentDigest("not-a-digest"), "abc")),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(store.put_if_absent(
          core::ContentAddressedStoreKey("digest-mismatch"),
          abc_digest, "abd")),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(store.put_if_absent(
          core::ContentAddressedStoreKey("too-large"),
          core::ContentDigest(sha256_digest("abcde")), "abcde")),
      RetainedBundleError);
  EXPECT_EQ(store.size(), 0U);

  EXPECT_TRUE(
      store.put_if_absent(core::ContentAddressedStoreKey("first"),
                          abc_digest, "abc")
          .inserted);
  EXPECT_THROW(
      static_cast<void>(store.put_if_absent(
          core::ContentAddressedStoreKey("first"),
          core::ContentDigest(sha256_digest("abd")), "abd")),
      std::runtime_error);
  EXPECT_TRUE(
      store.put_if_absent(
               core::ContentAddressedStoreKey("second"),
               core::ContentDigest(sha256_digest("d")), "d")
          .inserted);
  EXPECT_EQ(store.retained_bytes(), 4U);
  EXPECT_THROW(
      static_cast<void>(store.put_if_absent(
          core::ContentAddressedStoreKey("third"),
          core::ContentDigest(sha256_digest("")), "")),
      RetainedBundleError);
  EXPECT_EQ(store.size(), 2U);
  EXPECT_EQ(store.retained_bytes(), 4U);
}

TEST(FilesystemCatalogArtifactStoreTest,
     ReopenReappliesCountAndLogicalPayloadByteBounds) {
  ScopedTrustedRoot root;
  {
    FilesystemCatalogArtifactStore store(root.path(), 2U, 8U, 4U);
    EXPECT_TRUE(
        store.put_if_absent(
                 core::ContentAddressedStoreKey("bounded-A"),
                 core::ContentDigest(sha256_digest("aaaa")), "aaaa")
            .inserted);
    EXPECT_TRUE(
        store.put_if_absent(
                 core::ContentAddressedStoreKey("bounded-B"),
                 core::ContentDigest(sha256_digest("bbbb")), "bbbb")
            .inserted);
    EXPECT_EQ(store.retained_bytes(), 8U);
  }
  EXPECT_THROW(
      static_cast<void>(
          FilesystemCatalogArtifactStore(root.path(), 1U, 8U, 4U)),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemCatalogArtifactStore(root.path(), 2U, 7U, 4U)),
      RetainedBundleError);
  FilesystemCatalogArtifactStore exact(root.path(), 2U, 8U, 4U);
  EXPECT_EQ(exact.size(), 2U);
  EXPECT_EQ(exact.retained_bytes(), 8U);
}

TEST(FilesystemCatalogArtifactStoreTest,
     ReopenRejectsCorruptionSubstitutionAndFilenameKeyMismatch) {
  const auto run_case = [](auto corrupt) {
    ScopedTrustedRoot root;
    const std::string key_a = "opaque-A";
    const std::string key_b = "opaque-B";
    const std::string payload_a = "payload-A";
    const std::string payload_b = "payload-B";
    {
      FilesystemCatalogArtifactStore store(root.path());
      EXPECT_TRUE(
          store.put_if_absent(
                   core::ContentAddressedStoreKey(key_a),
                   core::ContentDigest(sha256_digest(payload_a)),
                   payload_a)
              .inserted);
      EXPECT_TRUE(
          store.put_if_absent(
                   core::ContentAddressedStoreKey(key_b),
                   core::ContentDigest(sha256_digest(payload_b)),
                   payload_b)
              .inserted);
    }
    corrupt(root.path(), key_a, key_b);
    EXPECT_THROW(
        static_cast<void>(
            FilesystemCatalogArtifactStore(root.path())),
        RetainedBundleError);
  };

  run_case([](const std::filesystem::path& root,
              const std::string& key_a, const std::string&) {
    std::string envelope = read_file(artifact_path(root, key_a));
    ASSERT_FALSE(envelope.empty());
    envelope.back() =
        envelope.back() == 'x' ? 'y' : 'x';
    write_file(artifact_path(root, key_a), envelope);
  });

  run_case([](const std::filesystem::path& root,
              const std::string& key_a, const std::string& key_b) {
    write_file(artifact_path(root, key_b),
               read_file(artifact_path(root, key_a)));
  });

  run_case([](const std::filesystem::path& root,
              const std::string& key_a, const std::string& key_b) {
    const std::string substituted_payload = "valid-substitute";
    write_file(
        artifact_path(root, key_a),
        detail::encode_catalog_artifact_envelope(
            key_b, sha256_digest(substituted_payload),
            substituted_payload));
  });
}

TEST(FilesystemCatalogArtifactStoreTest,
     ConcurrentSameIdentityPublishesOnceAndRestartAccountsExactly) {
  ScopedTrustedRoot root;
  {
    FilesystemCatalogArtifactStore initialize(
        root.path(), 1U, 1024U, 1024U);
  }
  const core::ContentAddressedStoreKey key(
      "opaque/concurrent/key");
  const std::string payload = "concurrent catalog payload";
  const core::ContentDigest digest(sha256_digest(payload));
  const ProcessRaceResult race = run_process_race(
      8U, [&](std::size_t) {
        FilesystemCatalogArtifactStore store(
            root.path(), 1U, 1024U, 1024U);
        return store.put_if_absent(key, digest, payload).inserted;
      });
  EXPECT_EQ(race.inserted, 1U);
  EXPECT_EQ(race.idempotent, 7U);
  EXPECT_EQ(race.failed, 0U);

  FilesystemCatalogArtifactStore reopened(
      root.path(), 1U, 1024U, 1024U);
  EXPECT_EQ(reopened.size(), 1U);
  EXPECT_EQ(reopened.retained_bytes(), payload.size());
  EXPECT_EQ(reopened.get_verified(key, digest, payload.size()),
            payload);
}

TEST(FilesystemCatalogArtifactStoreTest,
     ReopenCleansBoundedCrashTemporaryAndRejectsUnsafeEntries) {
  ScopedTrustedRoot root;
  const std::string key = "crash-residue-key";
  const std::string payload = "crash-residue-payload";
  {
    FilesystemCatalogArtifactStore store(root.path(), 2U, 1024U, 1024U);
    EXPECT_TRUE(
        store.put_if_absent(
                 core::ContentAddressedStoreKey(key),
                 core::ContentDigest(sha256_digest(payload)), payload)
            .inserted);
  }
  const std::filesystem::path final_path =
      artifact_path(root.path(), key);
  const std::filesystem::path stale_path =
      final_path.parent_path() /
      ("." + final_path.filename().string() + ".tmp.999.1");
  write_file(stale_path, read_file(final_path));
  ASSERT_TRUE(std::filesystem::exists(stale_path));
  {
    FilesystemCatalogArtifactStore reopened(
        root.path(), 2U, 1024U, 1024U);
    EXPECT_EQ(reopened.size(), 1U);
    EXPECT_FALSE(std::filesystem::exists(stale_path));
  }

  const std::filesystem::path unsafe_target =
      root.path() / "unsafe-target";
  write_file(unsafe_target, "target");
  ASSERT_TRUE(std::filesystem::remove(final_path));
  std::filesystem::create_symlink(unsafe_target, final_path);
  EXPECT_THROW(
      static_cast<void>(FilesystemCatalogArtifactStore(
          root.path(), 2U, 1024U, 1024U)),
      RetainedBundleError);
}

TEST(FilesystemCatalogArtifactStoreTest,
     InheritedInstanceRejectsAfterForkAndFreshChildInstanceWorks) {
  ScopedTrustedRoot root;
  FilesystemCatalogArtifactStore inherited(
      root.path(), 2U, 1024U, 1024U);
  const core::ContentAddressedStoreKey parent_key("parent-key");
  const core::ContentDigest parent_digest(sha256_digest("parent"));
  ASSERT_TRUE(
      inherited.put_if_absent(parent_key, parent_digest, "parent").inserted);

  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    const auto rejects = [](auto operation) {
      try {
        operation();
      } catch (const RetainedBundleError& error) {
        return std::string_view(error.what()) ==
               "durable retention store instances must be reopened "
               "after fork";
      } catch (...) {
        return false;
      }
      return false;
    };
    if (!rejects([&] {
          static_cast<void>(inherited.put_if_absent(
              core::ContentAddressedStoreKey("child-inherited"),
              core::ContentDigest(sha256_digest("child")), "child"));
        }) ||
        !rejects([&] {
          static_cast<void>(
              inherited.get_verified(parent_key, parent_digest));
        }) ||
        !rejects([&] { static_cast<void>(inherited.size()); }) ||
        !rejects(
            [&] { static_cast<void>(inherited.retained_bytes()); })) {
      ::_exit(22);
    }
    try {
      FilesystemCatalogArtifactStore fresh(
          root.path(), 2U, 1024U, 1024U);
      const core::ContentAddressedStoreKey child_key("child-fresh");
      const std::string child_payload = "child";
      const core::ContentDigest child_digest(
          sha256_digest(child_payload));
      const auto stored =
          fresh.put_if_absent(child_key, child_digest, child_payload);
      const auto read =
          fresh.get_verified(child_key, child_digest,
                             child_payload.size());
      ::_exit(stored.inserted && read == child_payload ? 20 : 22);
    } catch (...) {
      ::_exit(22);
    }
  }

  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 20);
  EXPECT_EQ(inherited.size(), 2U);
  EXPECT_EQ(inherited.retained_bytes(),
            std::string_view("parent").size() +
                std::string_view("child").size());
}

TEST(DurableFileCollectionRegressionTest,
     ExistingDigestDerivedRawStoreKeepsExactAccountingAndReads) {
  ScopedTrustedRoot root;
  const std::string payload{"raw\0payload", 11U};
  StoredRawArtifact stored{
      core::ContentDigest(sha256_digest("")),
      core::ContentAddressedStoreKey(
          "sha256/" + std::string(64U, '0')),
      0U,
      false,
  };
  {
    FilesystemRawArtifactStore store(root.path(), 1U, 11U, 11U);
    stored = store.put_if_absent(payload);
    EXPECT_TRUE(stored.inserted);
    EXPECT_EQ(store.size(), 1U);
    EXPECT_EQ(store.retained_bytes(), payload.size());
  }
  FilesystemRawArtifactStore reopened(root.path(), 1U, 11U, 11U);
  EXPECT_EQ(reopened.get_verified(
                stored.content_addressed_store_key,
                stored.content_digest, stored.byte_size),
            payload);
  EXPECT_EQ(reopened.retained_bytes(), payload.size());
}

}  // namespace
}  // namespace manyforge::deployment
