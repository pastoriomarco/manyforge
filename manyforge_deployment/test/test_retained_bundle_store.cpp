#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/retention_qualified_deployment.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>

#include "retained_bundle_internal.hpp"
#include "runtime_only_retention_fixture.hpp"

namespace {

std::atomic<bool> g_fail_directory_fsync{false};
std::atomic<std::uint64_t> g_fsync_device{0U};
std::atomic<std::uint64_t> g_fsync_inode{0U};
std::atomic<std::uint64_t> g_matching_fsync_count{0U};
std::atomic<std::uint64_t> g_fail_on_matching_fsync{1U};

class ScopedDirectoryFsyncFailure final {
 public:
  explicit ScopedDirectoryFsyncFailure(
      const std::filesystem::path& directory,
      std::uint64_t fail_on_matching_fsync = 1U) {
    if (fail_on_matching_fsync == 0U) {
      throw std::logic_error(
          "test fsync fault occurrence must be positive");
    }
    struct stat status {};
    if (::stat(directory.c_str(), &status) != 0 ||
        !S_ISDIR(status.st_mode)) {
      throw std::logic_error(
          "test could not stat directory for fsync fault");
    }
    g_fsync_device.store(
        static_cast<std::uint64_t>(status.st_dev));
    g_fsync_inode.store(
        static_cast<std::uint64_t>(status.st_ino));
    g_matching_fsync_count.store(0U);
    g_fail_on_matching_fsync.store(fail_on_matching_fsync);
    g_fail_directory_fsync.store(true);
  }

  ~ScopedDirectoryFsyncFailure() {
    g_fail_directory_fsync.store(false);
  }

  ScopedDirectoryFsyncFailure(
      const ScopedDirectoryFsyncFailure&) = delete;
  ScopedDirectoryFsyncFailure& operator=(
      const ScopedDirectoryFsyncFailure&) = delete;
};

}  // namespace

extern "C" int __real_fsync(int fd);

extern "C" int __wrap_fsync(int fd) {
  if (g_fail_directory_fsync.load()) {
    struct stat status {};
    if (::fstat(fd, &status) == 0 &&
        static_cast<std::uint64_t>(status.st_dev) ==
            g_fsync_device.load() &&
        static_cast<std::uint64_t>(status.st_ino) ==
            g_fsync_inode.load()) {
      const std::uint64_t occurrence =
          g_matching_fsync_count.fetch_add(1U) + 1U;
      if (occurrence == g_fail_on_matching_fsync.load()) {
        errno = EIO;
        return -1;
      }
    }
  }
  return __real_fsync(fd);
}

namespace manyforge::deployment {
namespace {

using namespace std::string_view_literals;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

template <typename Source>
concept HasBundleInsertion =
    requires(Source& source, std::shared_ptr<const RetainedBundle> bundle) {
      source.put_if_absent(std::move(bundle));
    };

template <typename Store>
concept HasActiveAliasMutation =
    requires(Store& store, const core::RetainedBundleId& bundle_id) {
      store.set_active(bundle_id);
    };

static_assert(!HasBundleInsertion<IVerifiedRetainedBundleSource>);
static_assert(!HasActiveAliasMutation<FilesystemRawArtifactStore>);
static_assert(
    !HasActiveAliasMutation<FilesystemRetainedBundleRepository>);
static_assert(std::is_base_of_v<
              IVerifiedRetainedBundleSource,
              FilesystemVerifiedRetainedBundleSource>);
static_assert(std::is_base_of_v<
              IVerifiedRetainedProductArtifactSource,
              FilesystemVerifiedRetainedProductArtifactSource>);
static_assert(!std::is_base_of_v<
              IVerifiedRetainedBundleSource,
              FilesystemRetainedBundleRepository>);
static_assert(!std::is_default_constructible_v<
              VerifiedRetainedProductArtifact>);
static_assert(!std::is_aggregate_v<
              VerifiedRetainedProductArtifact>);
static_assert(std::is_constructible_v<
              FilesystemVerifiedRetainedBundleSource,
              std::filesystem::path>);
static_assert(std::is_constructible_v<
              FilesystemVerifiedRetainedProductArtifactSource,
              std::filesystem::path>);
static_assert(!std::is_constructible_v<
              FilesystemVerifiedRetainedBundleSource,
              const FilesystemRetainedBundleRepository&,
              const FilesystemRawArtifactStore&>);

class ScopedTrustedRoot final {
 public:
  ScopedTrustedRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("manyforge-retention-store-" +
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

[[nodiscard]] std::string empty_graph() {
  return "{\"runtime_units\":[],\"model_instances\":[],"
         "\"controllable_components\":[],\"control_domains\":[],"
         "\"control_bindings\":[],\"component_attachments\":[]}";
}

[[nodiscard]] RetentionQualifiedDeployment qualify_empty(
    std::string_view cell_id) {
  const std::string document =
      "{\"api_version\":\"manyforge.deployment.v1\","
      "\"deployment_artifact_id\":\"i3_durable_store_fixture\","
      "\"deployment_revision\":\"" +
      std::string(kZeroDigest) + "\",\"cell_id\":\"" +
      std::string(cell_id) +
      "\",\"name\":\"I3 durable store fixture\","
      "\"catalogs\":{\"records\":[]},\"graph\":" +
      empty_graph() +
      ",\"desired_control_configurations\":[],"
      "\"capability_provider_bindings\":[]}";
  const auto deployment = compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(document),
      std::span<const QualifiedRuntimeProfile>{});
  const auto composition = compose_cell_occurrences(
      deployment.graph(), std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});
  const auto selected_overlay = select_site_overlay_for_deployment(
      deployment.applicability_inputs().value().site_overlay_ref,
      deployment.applicability_inputs().value().external_resources,
      nullptr);
  const auto applicability = qualify_cell_applicability(
      composition, deployment.applicability_inputs(), {}, {}, {}, {}, {},
      {}, selected_overlay);
  return qualify_deployment_for_retention(
      deployment, compile_cell_ir(applicability));
}

[[nodiscard]] std::shared_ptr<const RetainedBundle>
materialize_durable_bundle(
    const std::filesystem::path& root, std::string_view cell_id) {
  FilesystemBuildManifestRepository manifest_repository(
      root);
  FilesystemRawArtifactStore raw_store(root);
  FilesystemRetainedBundleRepository bundle_repository(root);
  return detail::RetainedBundleMaterializer::
      materialize_store_and_reload_production(
          qualify_empty(cell_id), manifest_repository, raw_store,
          bundle_repository);
}

[[nodiscard]] std::shared_ptr<const RetainedBundle> make_sealed_bundle(
    std::string_view cell_id) {
  ScopedTrustedRoot root;
  return materialize_durable_bundle(root.path(), cell_id);
}

[[nodiscard]] std::filesystem::path raw_path(
    const std::filesystem::path& root,
    const StoredRawArtifact& stored) {
  return root / "raw-artifacts" / "sha256" /
         (stored.content_digest.value().substr(7U) + ".blob");
}

[[nodiscard]] std::filesystem::path raw_path(
    const std::filesystem::path& root,
    const core::RetainedArtifactRefDTO& artifact) {
  return root / "raw-artifacts" / "sha256" /
         (artifact.content_digest.value().substr(7U) + ".blob");
}

[[nodiscard]] std::filesystem::path bundle_path(
    const std::filesystem::path& root,
    const core::RetainedBundleId& bundle_id) {
  return root / "retained-bundles" / "sha256" /
         (bundle_id.value().substr(7U) + ".json");
}

void write_file(const std::filesystem::path& path,
                std::string_view bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.good()) {
    throw std::logic_error("test could not open retention-store file");
  }
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream.good()) {
    throw std::logic_error("test could not write retention-store file");
  }
}

void write_file_and_fsync(const std::filesystem::path& path,
                          std::string_view bytes) {
  write_file(path, bytes);
  const int fd =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    throw std::logic_error(
        "test could not reopen retention-store residue");
  }
  const int sync_result = ::fsync(fd);
  const int close_result = ::close(fd);
  if (sync_result != 0 || close_result != 0) {
    throw std::logic_error(
        "test could not fsync retention-store residue");
  }
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
      throw std::runtime_error("test could not fork retention writer");
    }
    if (child == 0) {
      (void)::close(start_pipe[1]);
      char token = '\0';
      ssize_t read_count = 0;
      do {
        read_count = ::read(start_pipe[0], &token, 1U);
      } while (read_count < 0 && errno == EINTR);
      (void)::close(start_pipe[0]);
      if (read_count != 1) {
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
  std::string tokens(process_count, 'x');
  std::size_t offset = 0U;
  while (offset < tokens.size()) {
    const ssize_t written =
        ::write(start_pipe[1], tokens.data() + offset,
                tokens.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      (void)::close(start_pipe[1]);
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
      continue;
    }
    switch (WEXITSTATUS(status)) {
      case 20:
        ++result.inserted;
        break;
      case 21:
        ++result.idempotent;
        break;
      default:
        ++result.failed;
        break;
    }
  }
  return result;
}

void expect_no_thread_failures(
    std::span<const std::exception_ptr> failures) {
  for (std::size_t index = 0U; index < failures.size(); ++index) {
    if (failures[index] == nullptr) {
      continue;
    }
    try {
      std::rethrow_exception(failures[index]);
    } catch (const std::exception& error) {
      ADD_FAILURE() << "worker " << index << " failed: " << error.what();
    } catch (...) {
      ADD_FAILURE() << "worker " << index
                    << " failed with a non-standard exception";
    }
  }
}

}  // namespace

TEST(FilesystemRawArtifactStoreTest,
     DurablePutReopenAndVerifiedReadUseExactDigestKey) {
  ScopedTrustedRoot root;
  StoredRawArtifact stored{
      core::ContentDigest(std::string(kZeroDigest)),
      core::ContentAddressedStoreKey(
          "sha256/" + std::string(64U, '0')),
      0U,
      false,
  };
  {
    FilesystemRawArtifactStore store(root.path());
    stored = store.put_if_absent("durable\0raw\0bytes"sv);
    EXPECT_TRUE(stored.inserted);
    EXPECT_FALSE(store.put_if_absent("durable\0raw\0bytes"sv).inserted);
    EXPECT_EQ(store.size(), 1U);
    EXPECT_EQ(store.retained_bytes(), stored.byte_size);
  }
  {
    FilesystemRawArtifactStore reopened(root.path());
    EXPECT_EQ(reopened.size(), 1U);
    EXPECT_EQ(reopened.retained_bytes(), stored.byte_size);
    EXPECT_EQ(reopened.get_verified(
                  stored.content_addressed_store_key,
                  stored.content_digest, stored.byte_size),
              "durable\0raw\0bytes"sv);
    EXPECT_FALSE(reopened.get_verified(
        stored.content_addressed_store_key, stored.content_digest,
        stored.byte_size + 1U));
    EXPECT_FALSE(reopened.get_verified(
        core::ContentAddressedStoreKey("sha256/" +
                                         std::string(64U, 'f')),
        stored.content_digest, stored.byte_size));
  }
}

TEST(FilesystemRawArtifactStoreTest,
     CrossProcessSameContentHasOneInsertionAndExactAccounting) {
  ScopedTrustedRoot root;
  {
    FilesystemRawArtifactStore initialize(root.path(), 1U, 1024U, 1024U);
  }
  const std::string bytes = "cross-process immutable raw artifact";
  const ProcessRaceResult race = run_process_race(
      8U, [&](std::size_t) {
        FilesystemRawArtifactStore store(
            root.path(), 1U, 1024U, 1024U);
        return store.put_if_absent(bytes).inserted;
      });
  EXPECT_EQ(race.inserted, 1U);
  EXPECT_EQ(race.idempotent, 7U);
  EXPECT_EQ(race.failed, 0U);

  FilesystemRawArtifactStore reopened(root.path(), 1U, 1024U, 1024U);
  EXPECT_EQ(reopened.size(), 1U);
  EXPECT_EQ(reopened.retained_bytes(), bytes.size());
}

TEST(FilesystemRawArtifactStoreTest,
     CrossProcessDistinctContentSerializesOneEntryBound) {
  ScopedTrustedRoot root;
  constexpr std::array<std::string_view, 2U> kValues{
      "distinct-process-A", "distinct-process-B"};
  {
    FilesystemRawArtifactStore initialize(
        root.path(), 1U, 64U, 64U);
  }

  int start_pipe[2]{-1, -1};
  int ready_pipe[2]{-1, -1};
  ASSERT_EQ(::pipe(start_pipe), 0);
  ASSERT_EQ(::pipe(ready_pipe), 0);
  std::array<pid_t, 2U> children{};
  for (std::size_t index = 0U; index < children.size(); ++index) {
    children[index] = ::fork();
    ASSERT_GE(children[index], 0);
    if (children[index] != 0) {
      continue;
    }
    (void)::close(start_pipe[1]);
    (void)::close(ready_pipe[0]);
    try {
      FilesystemRawArtifactStore store(
          root.path(), 1U, 64U, 64U);
      const char ready = 'r';
      if (::write(ready_pipe[1], &ready, 1U) != 1) {
        ::_exit(22);
      }
      (void)::close(ready_pipe[1]);
      char token = '\0';
      ssize_t read_count = 0;
      do {
        read_count = ::read(start_pipe[0], &token, 1U);
      } while (read_count < 0 && errno == EINTR);
      (void)::close(start_pipe[0]);
      if (read_count != 1) {
        ::_exit(22);
      }
      const StoredRawArtifact stored =
          store.put_if_absent(kValues[index]);
      ::_exit(stored.inserted ? 20 : 22);
    } catch (const RetainedBundleError& error) {
      ::_exit(std::string_view(error.what()) ==
                      "durable retention repository bound is exhausted"
                  ? 21
                  : 22);
    } catch (...) {
      ::_exit(22);
    }
  }
  (void)::close(start_pipe[0]);
  (void)::close(ready_pipe[1]);
  std::array<char, 2U> ready_tokens{};
  std::size_t ready_offset = 0U;
  while (ready_offset < ready_tokens.size()) {
    const ssize_t count =
        ::read(ready_pipe[0], ready_tokens.data() + ready_offset,
               ready_tokens.size() - ready_offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    ASSERT_GT(count, 0);
    ready_offset += static_cast<std::size_t>(count);
  }
  (void)::close(ready_pipe[0]);
  const std::array<char, 2U> tokens{'x', 'x'};
  ASSERT_EQ(::write(start_pipe[1], tokens.data(), tokens.size()),
            static_cast<ssize_t>(tokens.size()));
  (void)::close(start_pipe[1]);

  std::size_t inserted = 0U;
  std::size_t bounded = 0U;
  std::size_t failed = 0U;
  for (const pid_t child : children) {
    int status = 0;
    if (::waitpid(child, &status, 0) != child ||
        !WIFEXITED(status)) {
      ++failed;
      continue;
    }
    if (WEXITSTATUS(status) == 20) {
      ++inserted;
    } else if (WEXITSTATUS(status) == 21) {
      ++bounded;
    } else {
      ++failed;
    }
  }
  EXPECT_EQ(inserted, 1U);
  EXPECT_EQ(bounded, 1U);
  EXPECT_EQ(failed, 0U);

  FilesystemRawArtifactStore reopened(
      root.path(), 1U, 64U, 64U);
  EXPECT_EQ(reopened.size(), 1U);
  EXPECT_EQ(reopened.retained_bytes(), kValues.front().size());
  std::size_t present = 0U;
  for (const std::string_view value : kValues) {
    const std::string digest = sha256_digest(value);
    const auto bytes = reopened.get_verified(
        core::ContentAddressedStoreKey(
            "sha256/" + digest.substr(7U)),
        core::ContentDigest(digest),
        static_cast<std::uint64_t>(value.size()));
    if (bytes.has_value()) {
      ++present;
      EXPECT_EQ(*bytes, value);
    }
  }
  EXPECT_EQ(present, 1U);
}

TEST(FilesystemRawArtifactStoreTest,
     InheritedStoreRejectsAfterForkAndFreshChildStoreWorks) {
  ScopedTrustedRoot root;
  FilesystemRawArtifactStore inherited(root.path(), 2U, 1024U, 1024U);
  FilesystemRetainedBundleRepository inherited_repository(root.path());
  const StoredRawArtifact parent_artifact =
      inherited.put_if_absent("parent artifact");
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    const auto rejects_inherited_use = [](auto operation) {
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
    if (!rejects_inherited_use([&] {
          static_cast<void>(
              inherited.put_if_absent("inherited child write"));
        }) ||
        !rejects_inherited_use([&] {
          static_cast<void>(inherited.get_verified(
              parent_artifact.content_addressed_store_key,
              parent_artifact.content_digest,
              parent_artifact.byte_size));
        }) ||
        !rejects_inherited_use([&] {
          static_cast<void>(inherited.get_verified(
              core::ContentAddressedStoreKey("not-a-key"),
              core::ContentDigest(std::string(kZeroDigest)), 0U));
        }) ||
        !rejects_inherited_use(
            [&] { static_cast<void>(inherited.size()); }) ||
        !rejects_inherited_use(
            [&] { static_cast<void>(inherited.retained_bytes()); }) ||
        !rejects_inherited_use([&] {
          static_cast<void>(
              inherited_repository.put_if_absent(nullptr));
        }) ||
        !rejects_inherited_use([&] {
          static_cast<void>(
              inherited_repository.get_verified_envelope(
                  core::RetainedBundleId("not-a-digest")));
        }) ||
        !rejects_inherited_use([&] {
          static_cast<void>(inherited_repository.size());
        }) ||
        !rejects_inherited_use([&] {
          static_cast<void>(inherited_repository.retained_bytes());
        })) {
      ::_exit(22);
    }
    try {
      FilesystemRawArtifactStore fresh(
          root.path(), 2U, 1024U, 1024U);
      FilesystemVerifiedRetainedBundleSource fresh_source(root.path());
      if (fresh_source.get_verified(
              core::RetainedBundleId("not-a-digest")) != nullptr) {
        ::_exit(22);
      }
      const StoredRawArtifact child_artifact =
          fresh.put_if_absent("fresh child write");
      const auto reloaded = fresh.get_verified(
          child_artifact.content_addressed_store_key,
          child_artifact.content_digest, child_artifact.byte_size);
      ::_exit(child_artifact.inserted && reloaded.has_value() &&
                      *reloaded == "fresh child write"
                  ? 20
                  : 22);
    } catch (...) {
      ::_exit(22);
    }
  }

  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 20);
  EXPECT_EQ(inherited.size(), 2U);
  EXPECT_EQ(inherited_repository.size(), 0U);
}

TEST(FilesystemRawArtifactStoreTest,
     VerifiedPointReadDoesNotRehashUnrelatedStoreContents) {
  ScopedTrustedRoot root;
  FilesystemRawArtifactStore store(root.path());
  const StoredRawArtifact selected =
      store.put_if_absent("selected immutable bytes");
  const StoredRawArtifact unrelated =
      store.put_if_absent("unrelated immutable bytes");
  write_file(raw_path(root.path(), unrelated),
             "corrupt unrelated bytes");

  EXPECT_EQ(
      store.get_verified(selected.content_addressed_store_key,
                         selected.content_digest, selected.byte_size),
      "selected immutable bytes");
  EXPECT_THROW(static_cast<void>(store.size()), RetainedBundleError);
}

TEST(FilesystemRawArtifactStoreTest,
     ConcurrentDistinctContentRetainsEveryEntryWithoutAccountingRace) {
  constexpr std::size_t kThreadCount = 12U;
  ScopedTrustedRoot root;
  std::vector<std::string> values;
  std::uint64_t expected_bytes = 0U;
  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    values.push_back("durable-distinct-" + std::to_string(index) +
                     std::string(index + 1U, 'x'));
    expected_bytes +=
        static_cast<std::uint64_t>(values.back().size());
  }
  std::vector<std::unique_ptr<FilesystemRawArtifactStore>> stores;
  stores.reserve(kThreadCount);
  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    stores.push_back(std::make_unique<FilesystemRawArtifactStore>(
        root.path(), kThreadCount, expected_bytes, expected_bytes));
  }
  std::barrier<> start(
      static_cast<std::ptrdiff_t>(kThreadCount + 1U));
  std::vector<std::exception_ptr> failures(kThreadCount);
  std::vector<std::uint8_t> inserted(kThreadCount, 0U);
  std::vector<std::thread> workers;
  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    workers.emplace_back([&, index] {
      start.arrive_and_wait();
      try {
        inserted[index] = stores[index]
                                  ->put_if_absent(values[index])
                                  .inserted
                              ? 1U
                              : 0U;
      } catch (...) {
        failures[index] = std::current_exception();
      }
    });
  }
  start.arrive_and_wait();
  for (auto& worker : workers) {
    worker.join();
  }
  expect_no_thread_failures(failures);
  EXPECT_TRUE(
      std::all_of(inserted.begin(), inserted.end(),
                  [](std::uint8_t value) { return value == 1U; }));

  FilesystemRawArtifactStore reopened(
      root.path(), kThreadCount, expected_bytes, expected_bytes);
  EXPECT_EQ(reopened.size(), kThreadCount);
  EXPECT_EQ(reopened.retained_bytes(), expected_bytes);
}

TEST(FilesystemRawArtifactStoreTest,
     RejectsInvalidBoundsUnsafeRootsAndCorruptCommittedBytes) {
  ScopedTrustedRoot root;
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRawArtifactStore(root.path(), 0U, 1U, 1U)),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRawArtifactStore(root.path(), 1U, 0U, 1U)),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRawArtifactStore(root.path(), 1U, 1U, 2U)),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRawArtifactStore(root.path() / "missing")),
      RetainedBundleError);

  const auto symlink = root.path() / "root-symlink";
  std::filesystem::create_directory(root.path() / "symlink-target");
  std::filesystem::create_directory_symlink(
      root.path() / "symlink-target", symlink);
  EXPECT_THROW(
      static_cast<void>(FilesystemRawArtifactStore(symlink)),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRawArtifactStore(symlink / "")),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRetainedBundleRepository(symlink)),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRetainedBundleRepository(symlink / "")),
      RetainedBundleError);

  {
    ScopedTrustedRoot bounded_root;
    FilesystemRawArtifactStore bounded(
        bounded_root.path(), 1U, 1U, 1U);
    EXPECT_THROW(
        static_cast<void>(bounded.put_if_absent("too-large")),
        RetainedBundleError);
    EXPECT_EQ(bounded.size(), 0U);
  }

  StoredRawArtifact stored{
      core::ContentDigest(std::string(kZeroDigest)),
      core::ContentAddressedStoreKey(
          "sha256/" + std::string(64U, '0')),
      0U,
      false,
  };
  {
    FilesystemRawArtifactStore store(root.path(), 1U, 16U, 16U);
    stored = store.put_if_absent("abcd");
    EXPECT_THROW(static_cast<void>(store.put_if_absent("x")),
                 RetainedBundleError);
  }
  write_file(raw_path(root.path(), stored), "abce");
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRawArtifactStore(root.path(), 1U, 16U, 16U)),
      RetainedBundleError);
}

TEST(FilesystemRawArtifactStoreTest,
     ReopenPreflightsCountAndBytesBeforeHashingOutOfBudgetEntry) {
  const auto expect_exact_bound_rejection =
      [](const std::filesystem::path& root, std::size_t max_count,
         std::uint64_t max_total_bytes,
         std::uint64_t max_single_bytes) {
        try {
          FilesystemRawArtifactStore reopened(
              root, max_count, max_total_bytes, max_single_bytes);
          static_cast<void>(reopened);
          FAIL() << "out-of-budget repository unexpectedly reopened";
        } catch (const RetainedBundleError& error) {
          EXPECT_EQ(
              std::string_view(error.what()),
              "persistent durable retention repository exceeds bounds");
        } catch (const std::exception& error) {
          FAIL() << "wrong rejection type: " << error.what();
        }
      };

  {
    ScopedTrustedRoot root;
    FilesystemRawArtifactStore store(root.path(), 2U, 64U, 32U);
    EXPECT_TRUE(
        store.put_if_absent(std::string(32U, 'a')).inserted);
    EXPECT_TRUE(
        store.put_if_absent(std::string(32U, 'b')).inserted);
    expect_exact_bound_rejection(root.path(), 1U, 64U, 32U);
  }

  {
    ScopedTrustedRoot root;
    StoredRawArtifact artifact{
        core::ContentDigest(std::string(kZeroDigest)),
        core::ContentAddressedStoreKey(
            "sha256/" + std::string(64U, '0')),
        0U,
        false,
    };
    {
      FilesystemRawArtifactStore store(root.path(), 1U, 32U, 32U);
      artifact =
          store.put_if_absent(std::string(32U, 'a'));
    }
    write_file(raw_path(root.path(), artifact),
               std::string(32U, 'c'));
    // The corrupt entry exceeds the tighter bound. Preflight must reject its
    // size before any SHA-256 pass can report byte corruption.
    expect_exact_bound_rejection(root.path(), 1U, 16U, 16U);
  }
}

TEST(FilesystemRetainedBundleRepositoryTest,
     DurableInsertionReopenAndReadOnlySourceReconstructSealedValue) {
  ScopedTrustedRoot root;
  const auto bundle = materialize_durable_bundle(
      root.path(), "durable_bundle_reopen");
  {
    FilesystemRetainedBundleRepository repository(root.path());
    EXPECT_FALSE(repository.put_if_absent(bundle));
    EXPECT_EQ(repository.size(), 1U);
    EXPECT_EQ(repository.retained_bytes(),
              bundle->canonical_bytes().size());
    EXPECT_NE(
        repository.get_verified_envelope(
            bundle->retained_bundle_id()),
        nullptr);
  }
  {
    FilesystemVerifiedRetainedBundleSource verified_source(root.path());
    const IVerifiedRetainedBundleSource& source = verified_source;
    const auto loaded =
        source.get_verified(bundle->retained_bundle_id());
    ASSERT_NE(loaded, nullptr);
    EXPECT_NE(loaded.get(), bundle.get());
    EXPECT_EQ(loaded->retained_bundle_id(),
              bundle->retained_bundle_id());
    EXPECT_EQ(loaded->value(), bundle->value());
    EXPECT_EQ(loaded->canonical_bytes(), bundle->canonical_bytes());
    EXPECT_TRUE(verify_retained_bundle_identity(*loaded));
    EXPECT_EQ(source.get_verified(
                  core::RetainedBundleId("not-a-digest")),
              nullptr);
  }
}

TEST(FilesystemVerifiedRetainedBundleSourceTest,
     MissingOrCorruptReferencedRawArtifactFailsClosed) {
  {
    ScopedTrustedRoot root;
    const auto bundle = materialize_durable_bundle(
        root.path(), "durable_bundle_missing_artifact");
    const auto& artifacts =
        bundle->value()
            .compiled_products.planning_collision_model.artifact_refs;
    ASSERT_EQ(artifacts.size(), 2U);
    FilesystemVerifiedRetainedBundleSource source(root.path());
    ASSERT_TRUE(std::filesystem::remove(
        raw_path(root.path(), artifacts.front())));
    EXPECT_THROW(
        static_cast<void>(
            source.get_verified(bundle->retained_bundle_id())),
        RetainedBundleError);
  }

  {
    ScopedTrustedRoot root;
    const auto bundle = materialize_durable_bundle(
        root.path(), "durable_bundle_corrupt_artifact");
    const auto& artifacts =
        bundle->value()
            .compiled_products.planning_collision_model.artifact_refs;
    ASSERT_EQ(artifacts.size(), 2U);
    FilesystemVerifiedRetainedBundleSource source(root.path());
    write_file(raw_path(root.path(), artifacts.back()),
               "corrupt retained artifact");
    EXPECT_THROW(
        static_cast<void>(
            source.get_verified(bundle->retained_bundle_id())),
        RetainedBundleError);
  }
}

TEST(FilesystemVerifiedRetainedProductArtifactSourceTest,
     ExactTupleReturnsEchoedMetadataAndIndependentlyVerifiedBytes) {
  ScopedTrustedRoot root;
  const auto bundle = materialize_durable_bundle(
      root.path(), "durable_product_point_read");
  const auto& product =
      bundle->value().compiled_products.planning_collision_model;
  ASSERT_EQ(product.artifact_refs.size(), 2U);

  const RetainedProductArtifactQuery urdf_query{
      bundle->retained_bundle_id(), product.product_id,
      product.product_kind, core::ArtifactRoleId("urdf")};
  const FilesystemVerifiedRetainedProductArtifactSource source(
      root.path());
  const auto first = source.get_verified(urdf_query);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->query(), urdf_query);
  EXPECT_EQ(first->product_contract_id(),
            product.product_contract_id);
  EXPECT_EQ(first->product_contract_revision(),
            product.product_contract_revision);
  const auto expected_urdf = std::find_if(
      product.artifact_refs.begin(), product.artifact_refs.end(),
      [](const core::RetainedArtifactRefDTO& artifact) {
        return artifact.artifact_role_id.value() == "urdf";
      });
  ASSERT_NE(expected_urdf, product.artifact_refs.end());
  EXPECT_EQ(first->artifact_ref(), *expected_urdf);
  EXPECT_EQ(first->bytes().size(), expected_urdf->byte_size);
  EXPECT_EQ(sha256_digest(first->bytes()),
            expected_urdf->content_digest.value());
  EXPECT_NE(first->bytes().find("<robot"), std::string_view::npos);

  const FilesystemVerifiedRetainedProductArtifactSource reopened(
      root.path());
  const auto second = reopened.get_verified(urdf_query);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->query(), first->query());
  EXPECT_EQ(second->product_contract_id(),
            first->product_contract_id());
  EXPECT_EQ(second->product_contract_revision(),
            first->product_contract_revision());
  EXPECT_EQ(second->artifact_ref(), first->artifact_ref());
  EXPECT_EQ(second->bytes(), first->bytes());
}

TEST(FilesystemVerifiedRetainedProductArtifactSourceTest,
     CorruptOrSubstitutedSemanticBundleFailsClosed) {
  {
    ScopedTrustedRoot root;
    const auto bundle = materialize_durable_bundle(
        root.path(), "durable_product_corrupt_bundle");
    const auto& product =
        bundle->value().compiled_products.planning_collision_model;
    const RetainedProductArtifactQuery query{
        bundle->retained_bundle_id(), product.product_id,
        product.product_kind, product.artifact_refs.front().artifact_role_id};
    const FilesystemVerifiedRetainedProductArtifactSource source(
        root.path());

    std::string corrupted(bundle->canonical_bytes());
    ASSERT_FALSE(corrupted.empty());
    corrupted[corrupted.size() / 2U] =
        corrupted[corrupted.size() / 2U] == 'x' ? 'y' : 'x';
    write_file(
        bundle_path(root.path(), bundle->retained_bundle_id()),
        corrupted);
    EXPECT_THROW(
        static_cast<void>(source.get_verified(query)),
        RetainedBundleError);
  }

  {
    ScopedTrustedRoot root;
    const auto bundle = materialize_durable_bundle(
        root.path(), "durable_product_substituted_bundle");
    const auto substitute =
        make_sealed_bundle("durable_product_substitute");
    const auto& product =
        bundle->value().compiled_products.planning_collision_model;
    const RetainedProductArtifactQuery query{
        bundle->retained_bundle_id(), product.product_id,
        product.product_kind, product.artifact_refs.front().artifact_role_id};
    const FilesystemVerifiedRetainedProductArtifactSource source(
        root.path());

    write_file(
        bundle_path(root.path(), bundle->retained_bundle_id()),
        substitute->canonical_bytes());
    EXPECT_THROW(
        static_cast<void>(source.get_verified(query)),
        RetainedBundleError);
  }
}

TEST(FilesystemVerifiedRetainedProductArtifactSourceTest,
     UnknownBundleProductKindOrRoleIsAbsent) {
  ScopedTrustedRoot root;
  const auto bundle = materialize_durable_bundle(
      root.path(), "durable_product_unknown_tuple");
  const auto& product =
      bundle->value().compiled_products.planning_collision_model;
  const FilesystemVerifiedRetainedProductArtifactSource source(
      root.path());
  RetainedProductArtifactQuery query{
      bundle->retained_bundle_id(), product.product_id,
      product.product_kind, core::ArtifactRoleId("urdf")};

  query.retained_bundle_id =
      core::RetainedBundleId(std::string(kZeroDigest));
  EXPECT_FALSE(source.get_verified(query).has_value());
  query.retained_bundle_id = bundle->retained_bundle_id();

  query.product_id = core::ProductId("unknown_product");
  EXPECT_FALSE(source.get_verified(query).has_value());
  query.product_id = product.product_id;

  query.product_kind = core::RetainedProductKind::kViewerModel;
  EXPECT_FALSE(source.get_verified(query).has_value());
  query.product_kind = product.product_kind;

  query.artifact_role_id = core::ArtifactRoleId("unknown_role");
  EXPECT_FALSE(source.get_verified(query).has_value());
}

TEST(FilesystemVerifiedRetainedProductArtifactSourceTest,
     MissingOrCorruptSelectedBytesFailClosed) {
  {
    ScopedTrustedRoot root;
    const auto bundle = materialize_durable_bundle(
        root.path(), "durable_product_missing_bytes");
    const auto& product =
        bundle->value().compiled_products.planning_collision_model;
    const auto& artifact = product.artifact_refs.front();
    const RetainedProductArtifactQuery query{
        bundle->retained_bundle_id(), product.product_id,
        product.product_kind, artifact.artifact_role_id};
    const FilesystemVerifiedRetainedProductArtifactSource source(
        root.path());
    ASSERT_TRUE(
        std::filesystem::remove(raw_path(root.path(), artifact)));
    EXPECT_THROW(
        static_cast<void>(source.get_verified(query)),
        RetainedBundleError);
  }

  {
    ScopedTrustedRoot root;
    const auto bundle = materialize_durable_bundle(
        root.path(), "durable_product_corrupt_bytes");
    const auto& product =
        bundle->value().compiled_products.planning_collision_model;
    const auto& artifact = product.artifact_refs.back();
    const RetainedProductArtifactQuery query{
        bundle->retained_bundle_id(), product.product_id,
        product.product_kind, artifact.artifact_role_id};
    const FilesystemVerifiedRetainedProductArtifactSource source(
        root.path());
    write_file(raw_path(root.path(), artifact), "corrupt");
    EXPECT_THROW(
        static_cast<void>(source.get_verified(query)),
        RetainedBundleError);
  }
}

TEST(FilesystemRetainedBundleRepositoryTest,
     CrossProcessSameBundleHasOneInsertionAndNoMutableAlias) {
  const auto bundle = make_sealed_bundle("durable_bundle_process_race");
  ScopedTrustedRoot root;
  {
    FilesystemRetainedBundleRepository initialize(root.path());
  }
  const ProcessRaceResult race = run_process_race(
      8U, [&](std::size_t) {
        FilesystemRetainedBundleRepository repository(root.path());
        return repository.put_if_absent(bundle);
      });
  EXPECT_EQ(race.inserted, 1U);
  EXPECT_EQ(race.idempotent, 7U);
  EXPECT_EQ(race.failed, 0U);

  FilesystemRetainedBundleRepository reopened(root.path());
  EXPECT_EQ(reopened.size(), 1U);
  EXPECT_NE(
      reopened.get_verified_envelope(
          bundle->retained_bundle_id()),
      nullptr);
}

TEST(FilesystemRetainedBundleRepositoryTest,
     ConcurrentDistinctBundlesRespectCountAndByteBounds) {
  const auto first = make_sealed_bundle("durable_bundle_distinct_a");
  const auto second = make_sealed_bundle("durable_bundle_distinct_b");
  const std::uint64_t total_bytes =
      static_cast<std::uint64_t>(first->canonical_bytes().size()) +
      static_cast<std::uint64_t>(second->canonical_bytes().size());
  const std::uint64_t maximum_bytes =
      std::max(first->canonical_bytes().size(),
               second->canonical_bytes().size());

  ScopedTrustedRoot root;
  FilesystemRetainedBundleRepository first_writer(
      root.path(), 2U, total_bytes, maximum_bytes);
  FilesystemRetainedBundleRepository second_writer(
      root.path(), 2U, total_bytes, maximum_bytes);
  std::barrier<> start(3);
  std::exception_ptr first_failure;
  std::exception_ptr second_failure;
  bool first_inserted = false;
  bool second_inserted = false;
  std::thread first_thread([&] {
    start.arrive_and_wait();
    try {
      first_inserted = first_writer.put_if_absent(first);
    } catch (...) {
      first_failure = std::current_exception();
    }
  });
  std::thread second_thread([&] {
    start.arrive_and_wait();
    try {
      second_inserted = second_writer.put_if_absent(second);
    } catch (...) {
      second_failure = std::current_exception();
    }
  });
  start.arrive_and_wait();
  first_thread.join();
  second_thread.join();
  const std::array failures{first_failure, second_failure};
  expect_no_thread_failures(failures);
  EXPECT_TRUE(first_inserted);
  EXPECT_TRUE(second_inserted);

  FilesystemRetainedBundleRepository reopened(
      root.path(), 2U, total_bytes, maximum_bytes);
  EXPECT_EQ(reopened.size(), 2U);
  EXPECT_EQ(reopened.retained_bytes(), total_bytes);

  ScopedTrustedRoot bounded_root;
  FilesystemRetainedBundleRepository count_bounded(
      bounded_root.path(), 1U, total_bytes, maximum_bytes);
  EXPECT_TRUE(count_bounded.put_if_absent(first));
  EXPECT_THROW(
      static_cast<void>(count_bounded.put_if_absent(second)),
      RetainedBundleError);

  ScopedTrustedRoot single_root;
  FilesystemRetainedBundleRepository single_bounded(
      single_root.path(), 1U, first->canonical_bytes().size(),
      first->canonical_bytes().size() - 1U);
  EXPECT_THROW(
      static_cast<void>(single_bounded.put_if_absent(first)),
      RetainedBundleError);
  EXPECT_EQ(single_bounded.size(), 0U);
}

TEST(FilesystemRetentionStoreTest,
     ReopenCleansSyntheticPrepublicationAndPostlinkResidue) {
  const auto bundle = make_sealed_bundle("durable_bundle_crash_residue");
  ScopedTrustedRoot root;
  StoredRawArtifact artifact{
      core::ContentDigest(std::string(kZeroDigest)),
      core::ContentAddressedStoreKey(
          "sha256/" + std::string(64U, '0')),
      0U,
      false,
  };
  {
    FilesystemRawArtifactStore raw_store(root.path());
    FilesystemRetainedBundleRepository bundle_repository(root.path());
    artifact = raw_store.put_if_absent("crash-window-artifact");
    EXPECT_TRUE(bundle_repository.put_if_absent(bundle));
  }

  const auto raw_directory =
      root.path() / "raw-artifacts" / "sha256";
  const auto bundle_directory =
      root.path() / "retained-bundles" / "sha256";
  const auto raw_postlink =
      raw_directory /
      ("." + artifact.content_digest.value().substr(7U) +
       ".blob.tmp.crash.postlink");
  const auto raw_prepublication =
      raw_directory /
      ("." + std::string(64U, '1') +
       ".blob.tmp.crash.prepublication");
  const auto bundle_postlink =
      bundle_directory /
      ("." + bundle->retained_bundle_id().value().substr(7U) +
       ".json.tmp.crash.postlink");
  const auto bundle_prepublication =
      bundle_directory /
      ("." + std::string(64U, '2') +
       ".json.tmp.crash.prepublication");
  std::filesystem::create_hard_link(
      raw_path(root.path(), artifact), raw_postlink);
  write_file(raw_prepublication, "partial");
  std::filesystem::create_hard_link(
      bundle_path(root.path(), bundle->retained_bundle_id()),
      bundle_postlink);
  write_file(bundle_prepublication, "partial");

  {
    FilesystemRawArtifactStore raw_reopened(root.path());
    FilesystemRetainedBundleRepository bundle_reopened(root.path());
    EXPECT_EQ(raw_reopened.size(), 1U);
    EXPECT_EQ(bundle_reopened.size(), 1U);
    EXPECT_FALSE(std::filesystem::exists(raw_postlink));
    EXPECT_FALSE(std::filesystem::exists(raw_prepublication));
    EXPECT_FALSE(std::filesystem::exists(bundle_postlink));
    EXPECT_FALSE(std::filesystem::exists(bundle_prepublication));
    EXPECT_NE(bundle_reopened.get_verified_envelope(
                  bundle->retained_bundle_id()),
              nullptr);
  }
}

TEST(FilesystemRetentionStoreTest,
     ReopenAndReadsDurablyAdoptSyntheticFinalOnlyCrashResidue) {
  const auto bundle =
      make_sealed_bundle("durable_bundle_final_only_residue");
  constexpr std::string_view kRawBytes =
      "synthetic-final-only-raw-artifact";
  const std::string raw_digest = sha256_digest(kRawBytes);
  const StoredRawArtifact artifact{
      core::ContentDigest(raw_digest),
      core::ContentAddressedStoreKey(
          "sha256/" + raw_digest.substr(7U)),
      static_cast<std::uint64_t>(kRawBytes.size()),
      false,
  };

  ScopedTrustedRoot root;
  FilesystemRawArtifactStore raw_already_open(root.path());
  FilesystemRetainedBundleRepository bundle_already_open(root.path());
  const auto raw_final = raw_path(root.path(), artifact);
  const auto bundle_final =
      bundle_path(root.path(), bundle->retained_bundle_id());
  const auto raw_temporary =
      raw_final.parent_path() /
      ("." + raw_digest.substr(7U) +
       ".blob.tmp.synthetic-final-only");
  const auto bundle_temporary =
      bundle_final.parent_path() /
      ("." + bundle->retained_bundle_id().value().substr(7U) +
       ".json.tmp.synthetic-final-only");

  write_file_and_fsync(raw_temporary, kRawBytes);
  std::filesystem::create_hard_link(raw_temporary, raw_final);
  ASSERT_TRUE(std::filesystem::remove(raw_temporary));
  write_file_and_fsync(
      bundle_temporary, bundle->canonical_bytes());
  std::filesystem::create_hard_link(
      bundle_temporary, bundle_final);
  ASSERT_TRUE(std::filesystem::remove(bundle_temporary));

  // This deliberately simulates kernel-visible residue after link +
  // temporary unlink and before the data-directory fsync. Fault injection
  // makes the adoption barrier observable: no reopen, read, or idempotent
  // insertion may acknowledge the final when that barrier fails.
  {
    ScopedDirectoryFsyncFailure fail_raw_directory_fsync(
        raw_final.parent_path());
    EXPECT_THROW(
        static_cast<void>(raw_already_open.get_verified(
            artifact.content_addressed_store_key,
            artifact.content_digest, artifact.byte_size)),
        std::runtime_error);
  }
  {
    ScopedDirectoryFsyncFailure fail_raw_directory_fsync(
        raw_final.parent_path());
    EXPECT_THROW(
        static_cast<void>(
            raw_already_open.put_if_absent(kRawBytes)),
        std::runtime_error);
  }
  {
    ScopedDirectoryFsyncFailure fail_raw_directory_fsync(
        raw_final.parent_path());
    EXPECT_THROW(
        static_cast<void>(
            FilesystemRawArtifactStore(root.path())),
        std::runtime_error);
  }
  {
    ScopedDirectoryFsyncFailure fail_bundle_directory_fsync(
        bundle_final.parent_path());
    EXPECT_THROW(
        static_cast<void>(
            bundle_already_open.get_verified_envelope(
                bundle->retained_bundle_id())),
        std::runtime_error);
  }
  {
    ScopedDirectoryFsyncFailure fail_bundle_directory_fsync(
        bundle_final.parent_path());
    EXPECT_THROW(
        static_cast<void>(
            bundle_already_open.put_if_absent(bundle)),
        std::runtime_error);
  }
  {
    ScopedDirectoryFsyncFailure fail_bundle_directory_fsync(
        bundle_final.parent_path());
    EXPECT_THROW(
        static_cast<void>(
            FilesystemRetainedBundleRepository(root.path())),
        std::runtime_error);
  }

  FilesystemRawArtifactStore raw_reopened(root.path());
  FilesystemRetainedBundleRepository bundle_reopened(root.path());
  EXPECT_EQ(raw_reopened.size(), 1U);
  EXPECT_EQ(
      raw_reopened.get_verified(
          artifact.content_addressed_store_key,
          artifact.content_digest, artifact.byte_size),
      kRawBytes);
  EXPECT_EQ(bundle_reopened.size(), 1U);
  const auto adopted =
      bundle_reopened.get_verified_envelope(
          bundle->retained_bundle_id());
  ASSERT_NE(adopted, nullptr);
  EXPECT_EQ(adopted->canonical_bytes(), bundle->canonical_bytes());
}

TEST(FilesystemRetentionStoreTest,
     NewPutFailsClosedWhenPostpublicationFsyncFailsAndReopenAdopts) {
  ScopedTrustedRoot root;
  constexpr std::string_view kRawBytes =
      "postpublication-fsync-raw-artifact";
  const std::string raw_digest = sha256_digest(kRawBytes);
  const StoredRawArtifact expected_artifact{
      core::ContentDigest(raw_digest),
      core::ContentAddressedStoreKey(
          "sha256/" + raw_digest.substr(7U)),
      static_cast<std::uint64_t>(kRawBytes.size()),
      false,
  };
  {
    FilesystemRawArtifactStore store(root.path());
    ScopedDirectoryFsyncFailure fail_postpublication_fsync(
        root.path() / "raw-artifacts" / "sha256", 2U);
    EXPECT_THROW(
        static_cast<void>(store.put_if_absent(kRawBytes)),
        std::runtime_error);
  }
  {
    FilesystemRawArtifactStore reopened(root.path());
    EXPECT_EQ(reopened.size(), 1U);
    EXPECT_EQ(
        reopened.get_verified(
            expected_artifact.content_addressed_store_key,
            expected_artifact.content_digest,
            expected_artifact.byte_size),
        kRawBytes);
    EXPECT_FALSE(reopened.put_if_absent(kRawBytes).inserted);
  }

  const auto bundle =
      make_sealed_bundle("postpublication_fsync_bundle");
  {
    FilesystemRetainedBundleRepository repository(root.path());
    ScopedDirectoryFsyncFailure fail_postpublication_fsync(
        root.path() / "retained-bundles" / "sha256", 2U);
    EXPECT_THROW(
        static_cast<void>(repository.put_if_absent(bundle)),
        std::runtime_error);
  }
  {
    FilesystemRetainedBundleRepository reopened(root.path());
    EXPECT_EQ(reopened.size(), 1U);
    const auto adopted =
        reopened.get_verified_envelope(
            bundle->retained_bundle_id());
    ASSERT_NE(adopted, nullptr);
    EXPECT_EQ(adopted->canonical_bytes(), bundle->canonical_bytes());
    EXPECT_FALSE(reopened.put_if_absent(bundle));
  }
}

TEST(FilesystemRetainedBundleRepositoryTest,
     CorruptionAndTighterReopenBoundsFailClosed) {
  const auto bundle = make_sealed_bundle("durable_bundle_corruption");
  {
    ScopedTrustedRoot root;
    {
      FilesystemRetainedBundleRepository repository(root.path());
      EXPECT_TRUE(repository.put_if_absent(bundle));
    }
    std::string corrupted(bundle->canonical_bytes());
    const auto cell = corrupted.find("durable_bundle_corruption");
    ASSERT_NE(cell, std::string::npos);
    corrupted[cell] = corrupted[cell] == 'd' ? 'e' : 'd';
    write_file(bundle_path(root.path(), bundle->retained_bundle_id()),
               corrupted);
    EXPECT_THROW(
        static_cast<void>(
            FilesystemRetainedBundleRepository(root.path())),
        RetainedBundleError);
  }

  {
    ScopedTrustedRoot root;
    {
      FilesystemRetainedBundleRepository repository(root.path());
      EXPECT_TRUE(repository.put_if_absent(bundle));
    }
    EXPECT_THROW(
        static_cast<void>(FilesystemRetainedBundleRepository(
            root.path(), 1U,
            bundle->canonical_bytes().size() - 1U,
            bundle->canonical_bytes().size() - 1U)),
        RetainedBundleError);
  }
}

TEST(FilesystemRetentionStoreTest,
     UnsupportedCurrentSubsetRejectsBeforeAnyDurableContentEffect) {
  const auto fixture = test::make_runtime_only_retention_fixture();
  ScopedTrustedRoot root;
  FilesystemBuildManifestRepository manifest_repository(
      root.path());
  FilesystemRawArtifactStore raw_store(root.path());
  FilesystemRetainedBundleRepository bundle_repository(root.path());

  EXPECT_THROW(
      static_cast<void>(
          detail::RetainedBundleMaterializer::
              materialize_store_and_reload_production(
                  fixture.qualified, manifest_repository, raw_store,
                  bundle_repository)),
      RetainedBundleError);
  EXPECT_EQ(manifest_repository.size(), 0U);
  EXPECT_EQ(raw_store.size(), 0U);
  EXPECT_EQ(raw_store.retained_bytes(), 0U);
  EXPECT_EQ(bundle_repository.size(), 0U);
  EXPECT_EQ(bundle_repository.retained_bytes(), 0U);
}

}  // namespace manyforge::deployment
