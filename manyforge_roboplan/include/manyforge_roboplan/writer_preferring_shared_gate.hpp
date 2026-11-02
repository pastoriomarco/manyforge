#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace manyforge::roboplan_backend {

/// A shared/exclusive gate whose exclusive acquisition is bounded: once a
/// writer claims the gate, new shared acquisitions wait, so the writer waits
/// only for the shared holders already in flight at its claim. A continuous
/// stream of overlapping readers therefore cannot starve a writer (which a
/// reader-preferring `std::shared_mutex` permits). Writers are serialized.
/// Satisfies SharedLockable for `std::shared_lock` and `std::unique_lock`.
class WriterPreferringSharedGate final {
 public:
  void lock_shared() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return !writer_claimed_; });
    ++readers_;
  }

  [[nodiscard]] bool try_lock_shared() {
    std::lock_guard lock(mutex_);
    if (writer_claimed_) return false;
    ++readers_;
    return true;
  }

  void unlock_shared() {
    std::lock_guard lock(mutex_);
    if (--readers_ == 0U) changed_.notify_all();
  }

  void lock() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return !writer_claimed_; });
    writer_claimed_ = true;
    changed_.wait(lock, [this] { return readers_ == 0U; });
  }

  void unlock() {
    {
      std::lock_guard lock(mutex_);
      writer_claimed_ = false;
    }
    changed_.notify_all();
  }

  /// Whether a writer currently waits for, or holds, exclusive access.
  [[nodiscard]] bool writer_claimed() const {
    std::lock_guard lock(mutex_);
    return writer_claimed_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t readers_{};
  bool writer_claimed_{};
};

}  // namespace manyforge::roboplan_backend
