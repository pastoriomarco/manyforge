#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <manyforge_core/filesystem_resolved_stop_base_plan_repository.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0U};
    for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("manyforge-stop-plan-repository-test-" +
               std::to_string(static_cast<long long>(::getpid())) + "-" +
               std::to_string(sequence.fetch_add(1U)));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
      if (error && error != std::errc::file_exists) {
        throw std::filesystem::filesystem_error("cannot create repository test directory", path_,
                                                error);
      }
    }
    throw std::runtime_error("repository test directory name exhausted");
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    throw std::runtime_error("cannot read repository test file");
  }
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    throw std::runtime_error("cannot write repository test file");
  }
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream.good()) {
    throw std::runtime_error("cannot finish repository test file");
  }
}

[[nodiscard]] std::string GoldenBytes() {
  std::string bytes = ReadFile(std::filesystem::path(__FILE__).parent_path() / "golden" /
                               "resolved_stop_base_plan.v1.json");
  if (bytes.ends_with('\n')) {
    bytes.pop_back();
  }
  return bytes;
}

[[nodiscard]] ResolvedStopBasePlanDTO GoldenPlan() {
  auto decoded = DecodeResolvedStopBasePlan(GoldenBytes());
  if (!decoded.ok()) {
    throw std::runtime_error("resolved Stop base-plan golden does not decode");
  }
  return decoded.value();
}

[[nodiscard]] ResolvedStopBasePlanDTO SecondPlan() {
  auto candidate = GoldenPlan();
  ++candidate.source_sequence;
  auto made = MakeResolvedStopBasePlan(std::move(candidate));
  if (!made.ok()) {
    throw std::runtime_error("second resolved Stop base-plan fixture is invalid");
  }
  return made.value();
}

[[nodiscard]] std::string Encoded(const ResolvedStopBasePlanDTO& plan) {
  auto encoded = EncodeResolvedStopBasePlan(plan);
  if (!encoded.ok()) {
    throw std::runtime_error("resolved Stop base-plan fixture does not encode");
  }
  return encoded.value();
}

[[nodiscard]] std::filesystem::path DataDirectory(const std::filesystem::path& root) {
  return root / "resolved-stop-base-plans" / "sha256";
}

[[nodiscard]] std::filesystem::path PlanPath(const std::filesystem::path& root,
                                             const ResolvedStopBasePlanId& id) {
  return DataDirectory(root) / (id.value().substr(7U) + ".json");
}

template <typename Repository>
concept HasCurrentAlias = requires(Repository& repository) { repository.Current(); };

template <typename Repository>
concept HasDelete =
    requires(Repository& repository, const ResolvedStopBasePlanId& id) { repository.Delete(id); };

static_assert(std::is_abstract_v<IResolvedStopBasePlanRepository>);
static_assert(!HasCurrentAlias<IResolvedStopBasePlanRepository>);
static_assert(!HasDelete<IResolvedStopBasePlanRepository>);

TEST(FilesystemResolvedStopBasePlanRepository,
     DerivesPublishesAndIndependentlyReloadsTheCompleteCanonicalPlan) {
  TemporaryDirectory root;
  auto supplied = GoldenPlan();
  supplied.resolved_stop_base_plan_id =
      ResolvedStopBasePlanId("caller-placeholder-is-not-authority");
  auto expected = MakeResolvedStopBasePlan(supplied);
  ASSERT_TRUE(expected.ok());
  const std::string expected_bytes = Encoded(expected.value());

  {
    FilesystemResolvedStopBasePlanRepository repository(root.path());
    const auto first = repository.PutIfAbsent(supplied);
    ASSERT_TRUE(first.ok()) << first.error().message;
    EXPECT_EQ(first.value(), expected.value().resolved_stop_base_plan_id);
    const auto stored_path = PlanPath(root.path(), first.value());
    EXPECT_EQ(ReadFile(stored_path), expected_bytes);
    struct stat stored_status {};
    ASSERT_EQ(::stat(stored_path.c_str(), &stored_status), 0);
    EXPECT_TRUE(S_ISREG(stored_status.st_mode));
    EXPECT_EQ(stored_status.st_mode & 0777, 0600);

    const auto loaded = repository.LoadVerified(first.value());
    ASSERT_TRUE(loaded.ok()) << loaded.error().message;
    EXPECT_EQ(loaded.value(), expected.value());

    const auto repeated = repository.PutIfAbsent(supplied);
    ASSERT_TRUE(repeated.ok()) << repeated.error().message;
    EXPECT_EQ(repeated.value(), first.value());
  }

  FilesystemResolvedStopBasePlanRepository reopened(root.path());
  const auto loaded = reopened.LoadVerified(expected.value().resolved_stop_base_plan_id);
  ASSERT_TRUE(loaded.ok()) << loaded.error().message;
  EXPECT_EQ(loaded.value(), expected.value());
  EXPECT_EQ(ReadFile(PlanPath(root.path(), loaded.value().resolved_stop_base_plan_id)),
            expected_bytes);
}

TEST(FilesystemResolvedStopBasePlanRepository,
     RejectsMissingMalformedAndPathLikeIdentitiesWithoutPathTraversal) {
  TemporaryDirectory root;
  FilesystemResolvedStopBasePlanRepository repository(root.path());

  const auto malformed = repository.LoadVerified(ResolvedStopBasePlanId("../../outside"));
  ASSERT_FALSE(malformed.ok());
  EXPECT_EQ(malformed.error().code, "invalid_request");
  EXPECT_FALSE(std::filesystem::exists(root.path() / "outside"));

  const auto uppercase =
      repository.LoadVerified(ResolvedStopBasePlanId("sha256:" + std::string(64U, 'A')));
  ASSERT_FALSE(uppercase.ok());
  EXPECT_EQ(uppercase.error().code, "invalid_request");

  const ResolvedStopBasePlanId absent("sha256:" + std::string(64U, '0'));
  const auto missing = repository.LoadVerified(absent);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, "missing_reference");
  EXPECT_EQ(missing.error().details.at("reference_type"), "resolved_stop_base_plan");
  EXPECT_EQ(missing.error().details.at("reference_id"), absent.value());
}

TEST(FilesystemResolvedStopBasePlanRepository,
     EnforcesSingleCountAndAggregateBoundsAfterIdempotentReplay) {
  const auto first = GoldenPlan();
  const auto second = SecondPlan();
  const std::string first_bytes = Encoded(first);
  const std::string second_bytes = Encoded(second);

  {
    TemporaryDirectory root;
    FilesystemResolvedStopBasePlanRepository repository(
        root.path(), 2U, kDefaultResolvedStopBasePlanRepositoryBytes,
        static_cast<std::uint64_t>(first_bytes.size() - 1U));
    const auto result = repository.PutIfAbsent(first);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, "resolved_stop_base_plan_repository_capacity_exhausted");
  }

  {
    TemporaryDirectory root;
    FilesystemResolvedStopBasePlanRepository repository(
        root.path(), 1U, kDefaultResolvedStopBasePlanRepositoryBytes);
    ASSERT_TRUE(repository.PutIfAbsent(first).ok());
    EXPECT_TRUE(repository.PutIfAbsent(first).ok());
    const auto result = repository.PutIfAbsent(second);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, "resolved_stop_base_plan_repository_capacity_exhausted");
  }

  {
    TemporaryDirectory root;
    const auto maximum_single =
        static_cast<std::uint64_t>(std::max(first_bytes.size(), second_bytes.size()));
    const auto aggregate =
        static_cast<std::uint64_t>(first_bytes.size() + second_bytes.size() - 1U);
    FilesystemResolvedStopBasePlanRepository repository(root.path(), 2U, aggregate, maximum_single);
    ASSERT_TRUE(repository.PutIfAbsent(first).ok());
    const auto result = repository.PutIfAbsent(second);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, "resolved_stop_base_plan_repository_capacity_exhausted");
  }
}

TEST(FilesystemResolvedStopBasePlanRepository, ConcurrentInstancesConvergeOnOneImmutableValue) {
  TemporaryDirectory root;
  FilesystemResolvedStopBasePlanRepository first_repository(root.path());
  FilesystemResolvedStopBasePlanRepository second_repository(root.path());
  const auto plan = GoldenPlan();
  Result<ResolvedStopBasePlanId> first_result =
      Result<ResolvedStopBasePlanId>::Err(ErrorCode::kInternalError);
  Result<ResolvedStopBasePlanId> second_result =
      Result<ResolvedStopBasePlanId>::Err(ErrorCode::kInternalError);

  std::thread first_writer([&]() { first_result = first_repository.PutIfAbsent(plan); });
  std::thread second_writer([&]() { second_result = second_repository.PutIfAbsent(plan); });
  first_writer.join();
  second_writer.join();

  ASSERT_TRUE(first_result.ok()) << first_result.error().message;
  ASSERT_TRUE(second_result.ok()) << second_result.error().message;
  EXPECT_EQ(first_result.value(), second_result.value());
  EXPECT_EQ(ReadFile(PlanPath(root.path(), first_result.value())), Encoded(plan));
}

TEST(FilesystemResolvedStopBasePlanRepository, ConcurrentProcessesConvergeOnOneImmutableValue) {
  TemporaryDirectory root;
  { FilesystemResolvedStopBasePlanRepository initialize(root.path()); }
  int start_pipe[2]{};
  ASSERT_EQ(::pipe(start_pipe), 0);
  std::vector<pid_t> children;
  for (std::size_t index = 0U; index < 2U; ++index) {
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
      (void)::close(start_pipe[1]);
      char start = '\0';
      if (::read(start_pipe[0], &start, 1U) != 1) {
        ::_exit(20);
      }
      (void)::close(start_pipe[0]);
      try {
        FilesystemResolvedStopBasePlanRepository repository(root.path());
        const auto result = repository.PutIfAbsent(GoldenPlan());
        ::_exit(result.ok() ? 0 : 21);
      } catch (...) {
        ::_exit(22);
      }
    }
    children.push_back(child);
  }
  (void)::close(start_pipe[0]);
  ASSERT_EQ(::write(start_pipe[1], "xx", 2U), 2);
  (void)::close(start_pipe[1]);

  for (const pid_t child : children) {
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
  }
  const auto plan = GoldenPlan();
  FilesystemResolvedStopBasePlanRepository reopened(root.path());
  const auto loaded = reopened.LoadVerified(plan.resolved_stop_base_plan_id);
  ASSERT_TRUE(loaded.ok()) << loaded.error().message;
  EXPECT_EQ(loaded.value(), plan);
  EXPECT_EQ(ReadFile(PlanPath(root.path(), plan.resolved_stop_base_plan_id)), Encoded(plan));
}

TEST(FilesystemResolvedStopBasePlanRepository,
     InheritedInstanceFailsWhileFreshChildInstanceCanRead) {
  TemporaryDirectory root;
  FilesystemResolvedStopBasePlanRepository repository(root.path());
  const auto plan = GoldenPlan();
  ASSERT_TRUE(repository.PutIfAbsent(plan).ok());

  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    const auto inherited = repository.LoadVerified(plan.resolved_stop_base_plan_id);
    if (inherited.ok() ||
        inherited.error().code != "resolved_stop_base_plan_repository_inherited_process") {
      ::_exit(10);
    }
    try {
      FilesystemResolvedStopBasePlanRepository fresh(root.path());
      const auto loaded = fresh.LoadVerified(plan.resolved_stop_base_plan_id);
      ::_exit(loaded.ok() && loaded.value() == plan ? 0 : 11);
    } catch (...) {
      ::_exit(12);
    }
  }
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(FilesystemResolvedStopBasePlanRepository,
     CorruptionAndFilenameIdentityMismatchFailClosedOnReadAndReopen) {
  TemporaryDirectory root;
  const auto plan = GoldenPlan();
  {
    FilesystemResolvedStopBasePlanRepository repository(root.path());
    ASSERT_TRUE(repository.PutIfAbsent(plan).ok());
    WriteFile(PlanPath(root.path(), plan.resolved_stop_base_plan_id), GoldenBytes() + "\n");
    const auto loaded = repository.LoadVerified(plan.resolved_stop_base_plan_id);
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, "resolved_stop_base_plan_repository_integrity_failure");
  }
  EXPECT_THROW(static_cast<void>(FilesystemResolvedStopBasePlanRepository(root.path())),
               std::exception);

  TemporaryDirectory mismatched_root;
  { FilesystemResolvedStopBasePlanRepository initialize(mismatched_root.path()); }
  const auto second = SecondPlan();
  WriteFile(PlanPath(mismatched_root.path(), plan.resolved_stop_base_plan_id), Encoded(second));
  EXPECT_THROW(static_cast<void>(FilesystemResolvedStopBasePlanRepository(mismatched_root.path())),
               std::exception);
}

TEST(FilesystemResolvedStopBasePlanRepository, RejectsSymlinkedAndUnknownRepositoryEntries) {
  {
    TemporaryDirectory root;
    const auto target = root.path() / "target";
    ASSERT_TRUE(std::filesystem::create_directory(target));
    const auto linked_root = root.path() / "linked-root";
    std::filesystem::create_directory_symlink(target, linked_root);
    ASSERT_TRUE(std::filesystem::is_symlink(linked_root));
    EXPECT_THROW(static_cast<void>(FilesystemResolvedStopBasePlanRepository(linked_root)),
                 std::exception);
  }

  {
    TemporaryDirectory root;
    ASSERT_TRUE(std::filesystem::create_directory(root.path() / "resolved-stop-base-plans"));
    const auto target = root.path() / "target";
    ASSERT_TRUE(std::filesystem::create_directory(target));
    const auto linked_data = root.path() / "resolved-stop-base-plans" / "sha256";
    std::filesystem::create_directory_symlink(target, linked_data);
    ASSERT_TRUE(std::filesystem::is_symlink(linked_data));
    EXPECT_THROW(static_cast<void>(FilesystemResolvedStopBasePlanRepository(root.path())),
                 std::exception);
  }

  {
    TemporaryDirectory root;
    { FilesystemResolvedStopBasePlanRepository initialize(root.path()); }
    WriteFile(DataDirectory(root.path()) / "unexpected", "unexpected");
    EXPECT_THROW(static_cast<void>(FilesystemResolvedStopBasePlanRepository(root.path())),
                 std::exception);
  }

  {
    TemporaryDirectory root;
    { FilesystemResolvedStopBasePlanRepository initialize(root.path()); }
    const auto fifo = DataDirectory(root.path()) / "unexpected-fifo";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    EXPECT_THROW(static_cast<void>(FilesystemResolvedStopBasePlanRepository(root.path())),
                 std::exception);
  }
}

TEST(FilesystemResolvedStopBasePlanRepository, RejectsSymlinkedCommittedEntriesAndRepositoryLock) {
  const auto plan = GoldenPlan();
  {
    TemporaryDirectory root;
    { FilesystemResolvedStopBasePlanRepository initialize(root.path()); }
    const auto target = root.path() / "outside-plan.json";
    WriteFile(target, GoldenBytes());
    const auto linked_entry = PlanPath(root.path(), plan.resolved_stop_base_plan_id);
    std::filesystem::create_symlink(target, linked_entry);
    ASSERT_TRUE(std::filesystem::is_symlink(linked_entry));
    EXPECT_THROW(static_cast<void>(FilesystemResolvedStopBasePlanRepository(root.path())),
                 std::exception);
  }

  {
    TemporaryDirectory root;
    const auto repository_directory = root.path() / "resolved-stop-base-plans";
    ASSERT_TRUE(std::filesystem::create_directory(repository_directory));
    ASSERT_TRUE(std::filesystem::create_directory(repository_directory / "sha256"));
    const auto target = root.path() / "outside-lock";
    WriteFile(target, "lock");
    const auto linked_lock = repository_directory / ".repository.lock";
    std::filesystem::create_symlink(target, linked_lock);
    ASSERT_TRUE(std::filesystem::is_symlink(linked_lock));
    EXPECT_THROW(static_cast<void>(FilesystemResolvedStopBasePlanRepository(root.path())),
                 std::exception);
  }
}

TEST(FilesystemResolvedStopBasePlanRepository,
     CleansBoundedStaleTemporaryEntriesAndAdoptsFinalOnlyResidue) {
  TemporaryDirectory root;
  const auto plan = GoldenPlan();
  {
    FilesystemResolvedStopBasePlanRepository repository(root.path());
    ASSERT_TRUE(repository.PutIfAbsent(plan).ok());
  }
  const auto stale = DataDirectory(root.path()) /
                     ("." + plan.resolved_stop_base_plan_id.value().substr(7U) + ".json.tmp.123.1");
  WriteFile(stale, "stale");
  ASSERT_TRUE(std::filesystem::exists(stale));

  FilesystemResolvedStopBasePlanRepository reopened(root.path());
  EXPECT_FALSE(std::filesystem::exists(stale));
  const auto loaded = reopened.LoadVerified(plan.resolved_stop_base_plan_id);
  ASSERT_TRUE(loaded.ok()) << loaded.error().message;
  EXPECT_EQ(loaded.value(), plan);
}

}  // namespace

TEST(FilesystemResolvedStopBasePlanRepository, ReopensTypedPneumaticPlanAndRefusesForeignContent) {
  TemporaryDirectory root;
  auto candidate = GoldenPlan();
  candidate.gripper_actuation_kind = GripperActuationKind::kPneumatic;
  candidate.escalation_stages.erase(candidate.escalation_stages.begin() + 1,
                                    candidate.escalation_stages.end());
  candidate.containment_granularity = candidate.escalation_stages.back().containment_granularity;
  const auto made = MakeResolvedStopBasePlan(candidate);
  ASSERT_TRUE(made.ok()) << made.error().message;
  {
    FilesystemResolvedStopBasePlanRepository repository(root.path());
    const auto stored = repository.PutIfAbsent(candidate);
    ASSERT_TRUE(stored.ok()) << stored.error().message;
    EXPECT_EQ(stored.value(), made.value().resolved_stop_base_plan_id);
    EXPECT_EQ(ReadFile(PlanPath(root.path(), stored.value())), Encoded(made.value()));
  }
  FilesystemResolvedStopBasePlanRepository reopened(root.path());
  const auto retained = reopened.LoadVerified(made.value().resolved_stop_base_plan_id);
  ASSERT_TRUE(retained.ok()) << retained.error().message;
  EXPECT_EQ(retained.value(), made.value());
  WriteFile(PlanPath(root.path(), made.value().resolved_stop_base_plan_id), GoldenBytes());
  EXPECT_FALSE(reopened.LoadVerified(made.value().resolved_stop_base_plan_id).ok());
}

}  // namespace manyforge::core
