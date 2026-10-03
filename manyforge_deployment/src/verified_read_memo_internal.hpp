#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "posix_durability_internal.hpp"

namespace manyforge::deployment::detail {

/// One immutable file read by a verification, with the exact identity
/// (device, inode, size, mtime, ctime) of the version that was read.
struct VerifiedFileRead final {
  std::filesystem::path path;
  std::uint64_t maximum_bytes;
  posix_durability::RegularFileIdentity identity;
};

/// Collects every file a verification reads on this thread. Recordings nest:
/// a finished inner recording also belongs to its enclosing one, so an outer
/// result depends on every file read by the verifications it reused.
class VerifiedFileReadRecording final {
public:
  VerifiedFileReadRecording() : parent_(current()) { current() = this; }
  ~VerifiedFileReadRecording() {
    current() = parent_;
    if (parent_ != nullptr) {
      parent_->reads_.insert(parent_->reads_.end(), reads_.begin(),
                             reads_.end());
    }
  }
  VerifiedFileReadRecording(const VerifiedFileReadRecording &) = delete;
  VerifiedFileReadRecording &
  operator=(const VerifiedFileReadRecording &) = delete;

  [[nodiscard]] const std::vector<VerifiedFileRead> &reads() const noexcept {
    return reads_;
  }

  /// Adds reads to the innermost active recording, if any.
  static void record(const std::vector<VerifiedFileRead> &reads) {
    if (auto *recording = current(); recording != nullptr) {
      recording->reads_.insert(recording->reads_.end(), reads.begin(),
                               reads.end());
    }
  }

private:
  [[nodiscard]] static VerifiedFileReadRecording *&current() {
    thread_local VerifiedFileReadRecording *recording = nullptr;
    return recording;
  }

  VerifiedFileReadRecording *parent_;
  std::vector<VerifiedFileRead> reads_;
};

/// Bounded process-wide reuse of one verification result.
///
/// A result is a pure function of content-addressed inputs named by its key
/// and of the bytes of the files it read. It is reused only while every one
/// of those files still has the identity that was verified: each reuse
/// re-opens (without following links) and re-stats every file, so a missing,
/// replaced or rewritten file is detected at the same point read as before and
/// the full verification runs again (failing closed exactly as before). A
/// result is remembered only when every file it read last changed longer ago
/// than any timestamp granularity, so a later in-place rewrite cannot keep an
/// identical identity. Empty results are never remembered.
template <typename Value> class VerifiedReadMemo final {
public:
  explicit VerifiedReadMemo(const std::size_t capacity) : capacity_(capacity) {}

  template <typename Verify>
  [[nodiscard]] Value get(const std::string &key,
                          const posix_durability::ErrorPolicy &errors,
                          const posix_durability::ReadMessages &messages,
                          Verify &&verify) {
    if (auto known = find(key, errors, messages); known.has_value()) {
      return std::move(*known);
    }
    const auto started = posix_durability::realtime_now_ns();
    VerifiedFileReadRecording recording;
    Value value = verify();
    if (static_cast<bool>(value) && !recording.reads().empty() &&
        std::all_of(recording.reads().begin(), recording.reads().end(),
                    [&](const VerifiedFileRead &read) {
                      return posix_durability::identity_settled_before(
                          read.identity, started);
                    })) {
      remember(key, value, recording.reads());
    }
    return value;
  }

private:
  struct Entry final {
    std::string key;
    Value value;
    std::vector<VerifiedFileRead> reads;
    std::uint64_t last_use;
  };

  [[nodiscard]] std::optional<Value>
  find(const std::string &key, const posix_durability::ErrorPolicy &errors,
       const posix_durability::ReadMessages &messages) {
    std::optional<Value> value;
    std::vector<VerifiedFileRead> reads;
    {
      std::lock_guard lock(mutex_);
      const auto entry =
          std::find_if(entries_.begin(), entries_.end(),
                       [&](const Entry &candidate) { return candidate.key == key; });
      if (entry == entries_.end()) {
        return std::nullopt;
      }
      entry->last_use = ++clock_;
      value = entry->value;
      reads = entry->reads;
    }
    for (const auto &read : reads) {
      try {
        if (posix_durability::regular_file_identity(
                read.path, read.maximum_bytes, true, errors, messages) !=
            std::optional{read.identity}) {
          return std::nullopt;
        }
      } catch (const std::exception &) {
        return std::nullopt;
      }
    }
    VerifiedFileReadRecording::record(reads);
    return value;
  }

  void remember(const std::string &key, const Value &value,
                const std::vector<VerifiedFileRead> &reads) {
    std::lock_guard lock(mutex_);
    std::erase_if(entries_,
                  [&](const Entry &entry) { return entry.key == key; });
    if (entries_.size() >= capacity_) {
      entries_.erase(std::min_element(
          entries_.begin(), entries_.end(),
          [](const Entry &left, const Entry &right) {
            return left.last_use < right.last_use;
          }));
    }
    entries_.push_back(Entry{key, value, reads, ++clock_});
  }

  const std::size_t capacity_;
  std::mutex mutex_;
  std::vector<Entry> entries_;
  std::uint64_t clock_{};
};

/// One key from its exact content-addressed components.
template <typename... Parts>
[[nodiscard]] std::string verified_read_key(const Parts &...parts) {
  std::string key;
  ((key.append(parts), key.push_back('\0')), ...);
  return key;
}

} // namespace manyforge::deployment::detail
