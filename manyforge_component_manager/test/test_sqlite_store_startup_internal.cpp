#include "sqlite_store_startup_internal.hpp"

#include "manyforge_component_manager/active_bundle_publication_store.hpp"
#include "manyforge_component_manager/manager_apply_journal.hpp"
#include "manyforge_component_manager/manager_bootstrap_journal.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::component_manager::sqlite_startup_internal {
namespace {

using namespace std::chrono_literals;

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() /
         "manyforge-manager-lifecycle-gate-test-XXXXXX")
            .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto* created = mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

void ExpectNoSqliteSidecars(const std::filesystem::path& path) {
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-wal"));
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-shm"));
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-journal"));
}

struct LifecycleObservation final {
  LifecycleObservation(
      const ManagerSqliteStoreLifecycleEvent attempt,
      const ManagerSqliteStoreLifecycleEvent acquired)
      : expected_attempt(attempt),
        expected_acquired(acquired),
        release_signal(release_after_acquired.get_future().share()) {}

  ManagerSqliteStoreLifecycleEvent expected_attempt;
  ManagerSqliteStoreLifecycleEvent expected_acquired;
  std::promise<void> attempted;
  std::promise<void> acquired;
  std::promise<void> release_after_acquired;
  std::shared_future<void> release_signal;
  std::atomic<bool> attempt_notified{false};
  std::atomic<bool> acquired_notified{false};
};

void ObserveLifecycleEvent(
    const ManagerSqliteStoreLifecycleEvent event,
    void* const context) noexcept {
  auto& observation = *static_cast<LifecycleObservation*>(context);
  try {
    if (event == observation.expected_attempt &&
        !observation.attempt_notified.exchange(true)) {
      observation.attempted.set_value();
      return;
    }
    if (event == observation.expected_acquired &&
        !observation.acquired_notified.exchange(true)) {
      observation.acquired.set_value();
      observation.release_signal.wait();
    }
  } catch (...) {
  }
}

class ScopedLifecycleObserver final {
 public:
  explicit ScopedLifecycleObserver(LifecycleObservation& observation) {
    SetManagerSqliteStoreLifecycleEventObserverForTesting(
        &ObserveLifecycleEvent, &observation);
  }

  ~ScopedLifecycleObserver() {
    ClearManagerSqliteStoreLifecycleEventObserverForTesting();
  }

  ScopedLifecycleObserver(const ScopedLifecycleObserver&) = delete;
  ScopedLifecycleObserver& operator=(const ScopedLifecycleObserver&) = delete;
};

template <typename Action>
void ExpectActionAcquiresLifecycleGate(
    const ManagerSqliteStoreLifecycleEvent expected_attempt,
    const ManagerSqliteStoreLifecycleEvent expected_acquired,
    Action action) {
  ManagerSqliteStoreStartupLock held(ManagerSqliteStoreStartupMutex());
  LifecycleObservation observation(expected_attempt, expected_acquired);
  auto attempted = observation.attempted.get_future();
  auto acquired = observation.acquired.get_future();
  const ScopedLifecycleObserver observer(observation);
  auto completion = std::async(
      std::launch::async,
      [action = std::move(action)]() mutable { action(); });

  const auto attempt_status = attempted.wait_for(2s);
  if (attempt_status != std::future_status::ready) {
    held.unlock();
    observation.release_after_acquired.set_value();
    ADD_FAILURE() << "production action did not attempt the lifecycle gate";
    EXPECT_NO_THROW(completion.get());
    return;
  }

  held.unlock();
  const auto acquired_status = acquired.wait_for(2s);
  if (acquired_status != std::future_status::ready) {
    observation.release_after_acquired.set_value();
    ADD_FAILURE() << "production action did not acquire the lifecycle gate";
    EXPECT_NO_THROW(completion.get());
    return;
  }

  auto& mutex = ManagerSqliteStoreStartupMutex();
  const bool acquired_by_test = mutex.try_lock();
  if (acquired_by_test) {
    mutex.unlock();
  }
  EXPECT_FALSE(acquired_by_test)
      << "production action reported acquisition without holding the gate";

  observation.release_after_acquired.set_value();
  EXPECT_NO_THROW(completion.get());
}

template <typename Factory, typename Verify>
void ExpectStoreCloseAndDestructionUseLifecycleGate(
    const std::filesystem::path& database,
    const Factory& make_store,
    const Verify& verify) {
  ExpectActionAcquiresLifecycleGate(
      ManagerSqliteStoreLifecycleEvent::kStartupAttempt,
      ManagerSqliteStoreLifecycleEvent::kStartupAcquired,
      [&make_store] {
        auto store = make_store();
        static_cast<void>(store);
      });
  ExpectNoSqliteSidecars(database);

  auto explicit_store = make_store();
  verify(*explicit_store);
  ExpectActionAcquiresLifecycleGate(
      ManagerSqliteStoreLifecycleEvent::kCloseAttempt,
      ManagerSqliteStoreLifecycleEvent::kCloseAcquired,
      [store = explicit_store.get()] { store->Close(); });
  ExpectNoSqliteSidecars(database);
  {
    auto reopened = make_store();
    verify(*reopened);
  }

  auto destructor_store = make_store();
  verify(*destructor_store);
  ExpectActionAcquiresLifecycleGate(
      ManagerSqliteStoreLifecycleEvent::kCloseAttempt,
      ManagerSqliteStoreLifecycleEvent::kCloseAcquired,
      [store = std::move(destructor_store)]() mutable { store.reset(); });
  ExpectNoSqliteSidecars(database);
  {
    auto reopened = make_store();
    verify(*reopened);
  }

  auto recursively_closed = make_store();
  auto outer = TryAcquireManagerSqliteStoreStartupLock(50ms);
  ASSERT_TRUE(outer.owns_lock());
  EXPECT_NO_THROW(recursively_closed->Close());
  outer.unlock();
  ExpectNoSqliteSidecars(database);
  auto final_reopen = make_store();
  verify(*final_reopen);
}

TEST(
    ManagerSqliteStoreStartupLock,
    AcquiresImmediatelyAndRecursivelyOnOneThread) {
  auto outer = TryAcquireManagerSqliteStoreStartupLock(0ms);
  ASSERT_TRUE(outer.owns_lock());
  auto inner = TryAcquireManagerSqliteStoreStartupLock(0ms);
  ASSERT_TRUE(inner.owns_lock());

  inner.unlock();
  outer.unlock();
  auto after_balanced_release =
      TryAcquireManagerSqliteStoreStartupLock(0ms);
  EXPECT_TRUE(after_balanced_release.owns_lock());
}

TEST(
    ManagerSqliteStoreStartupLock,
    ContendedDeadlineReturnsWithoutOwnership) {
  constexpr auto kTimeout = 50ms;
  auto held = TryAcquireManagerSqliteStoreStartupLock(kTimeout);
  ASSERT_TRUE(held.owns_lock());
  std::promise<void> start_promise;
  const auto start = start_promise.get_future().share();

  auto waiter = std::async(
      std::launch::async,
      [start] {
        start.get();
        const auto started = std::chrono::steady_clock::now();
        auto attempted =
            TryAcquireManagerSqliteStoreStartupLock(kTimeout);
        return std::pair{
            attempted.owns_lock(),
            std::chrono::steady_clock::now() - started};
      });
  start_promise.set_value();

  const auto [owns_lock, elapsed] = waiter.get();
  EXPECT_FALSE(owns_lock);
  EXPECT_GE(elapsed, kTimeout);
}

TEST(
    ManagerSqliteStoreStartupLock,
    ContendedWaitAcquiresAfterReleaseBeforeDeadline) {
  auto held = TryAcquireManagerSqliteStoreStartupLock(50ms);
  ASSERT_TRUE(held.owns_lock());
  std::promise<void> start_promise;
  const auto start = start_promise.get_future().share();
  std::promise<bool> contention_promise;
  auto contention = contention_promise.get_future();

  auto waiter = std::async(
      std::launch::async,
      [start, &contention_promise] {
        start.get();
        auto& mutex = ManagerSqliteStoreStartupMutex();
        const bool acquired_directly = mutex.try_lock();
        if (acquired_directly) {
          mutex.unlock();
        }
        contention_promise.set_value(!acquired_directly);
        auto acquired =
            TryAcquireManagerSqliteStoreStartupLock(2s);
        return acquired.owns_lock();
      });
  start_promise.set_value();
  EXPECT_TRUE(contention.get());
  std::this_thread::sleep_for(25ms);
  held.unlock();

  EXPECT_TRUE(waiter.get());
}

TEST(
    ManagerSqliteStoreStartupLock,
    ExceptionUnwindReleasesOwnedLevel) {
  EXPECT_THROW(
      [] {
        auto lock = TryAcquireManagerSqliteStoreStartupLock(50ms);
        if (!lock.owns_lock()) {
          throw std::logic_error("startup lock unexpectedly unavailable");
        }
        throw std::runtime_error("exercise RAII unwind");
      }(),
      std::runtime_error);

  auto after_unwind = TryAcquireManagerSqliteStoreStartupLock(0ms);
  EXPECT_TRUE(after_unwind.owns_lock());
}

TEST(
    ManagerSqliteStoreStartupLock,
    BootstrapCloseAndDestructionUseLifecycleGate) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "bootstrap.sqlite3";
  const auto make_store = [&] {
    return std::make_unique<SqliteManagerBootstrapJournal>(
        database, core::CellId("cell-a"));
  };
  ExpectStoreCloseAndDestructionUseLifecycleGate(
      database,
      make_store,
      [](SqliteManagerBootstrapJournal& store) {
        EXPECT_EQ(
            store.VerifyIntegrity().phase,
            ManagerBootstrapJournalPhase::kEmpty);
      });
}

TEST(
    ManagerSqliteStoreStartupLock,
    PublicationCloseAndDestructionUseLifecycleGate) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  const auto make_store = [&] {
    return std::make_unique<SqliteActiveBundlePublicationStore>(
        database, core::CellId("cell-a"));
  };
  ExpectStoreCloseAndDestructionUseLifecycleGate(
      database,
      make_store,
      [](SqliteActiveBundlePublicationStore& store) {
        EXPECT_EQ(store.VerifyIntegrity().receipt_count, 0);
      });
}

TEST(
    ManagerSqliteStoreStartupLock,
    ApplyCloseAndDestructionUseLifecycleGate) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "apply.sqlite3";
  const auto make_store = [&] {
    return std::make_unique<SqliteManagerApplyJournal>(
        database, core::CellId("cell-a"));
  };
  ExpectStoreCloseAndDestructionUseLifecycleGate(
      database,
      make_store,
      [](SqliteManagerApplyJournal& store) {
        EXPECT_EQ(store.VerifyIntegrity().apply_intent_count, 0);
      });
}

}  // namespace
}  // namespace manyforge::component_manager::sqlite_startup_internal
