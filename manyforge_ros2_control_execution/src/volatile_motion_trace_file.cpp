#include <manyforge_ros2_control_execution/volatile_motion_trace.hpp>

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>

namespace manyforge::ros2_control_execution {
namespace {
using V = content_identity::CanonicalValue;
constexpr std::uint64_t kStoreBytes = 64U * 1024U * 1024U;
constexpr std::size_t kStoreEntries = 128U;
constexpr auto kTemporary = ".snapshot.tmp";
class Fd final {
public:
  explicit Fd(int value) : value_(value) {}
  ~Fd() { if (value_ >= 0) ::close(value_); }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  int get() const { return value_; }
private:
  int value_;
};
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
int open_directory(const std::filesystem::path &path) {
  require(path.is_absolute() && path.lexically_normal() == path,
          "trace directory must be absolute and normalized");
  int current = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  require(current >= 0, "cannot open trace directory root");
  try {
    const auto relative = path.relative_path();
    for (auto it = relative.begin(); it != relative.end(); ++it) {
      const auto &part = *it;
      require(part != "." && part != "..", "unsafe trace directory component");
      int child = ::openat(current, part.c_str(),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (child < 0 && errno == ENOENT && std::next(it) == relative.end()) {
        require(::mkdirat(current, part.c_str(), 0700) == 0 || errno == EEXIST,
                "cannot create trace directory");
        child = ::openat(current, part.c_str(),
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      }
      require(child >= 0, "trace directory is unavailable or symlinked");
      ::close(current);
      current = child;
    }
    struct stat status {};
    require(::fstat(current, &status) == 0 && status.st_uid == ::geteuid() &&
                (status.st_mode & 0777) == 0700,
            "trace directory must be private and owned by this user");
    return current;
  } catch (...) {
    ::close(current);
    throw;
  }
}
bool trace_name(const std::string &name) {
  return name.size() == 75U && name.starts_with("trace-") &&
         name.ends_with(".json") &&
         std::all_of(name.begin() + 6, name.begin() + 70, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
struct Stored {
  std::string name;
  std::uint64_t bytes;
  timespec modified;
};
} // namespace

core::Result<V> VolatileMotionTrace::SnapshotFile(
    const core::RequestOwnerEpoch &owner, std::size_t allowance,
    const std::filesystem::path &directory) const {
  const auto error = [](const char *code, const char *message) {
    return core::Result<V>::Err({code, message, {}});
  };
  auto snapshot = Snapshot(owner, allowance);
  if (!snapshot.ok()) return snapshot;
  try {
    const auto bytes = content_identity::canonical_json(snapshot.value());
    const auto digest = content_identity::sha256_digest(bytes);
    const auto filename = "trace-" + digest.substr(7U) + ".json";
    Fd root(open_directory(directory));
    if (::flock(root.get(), LOCK_EX | LOCK_NB) != 0)
      return error("motion_trace_store_busy", "another trace reader or writer is active");
    const auto close_directory = [](DIR *value) { if (value) ::closedir(value); };
    const auto duplicate = ::dup(root.get());
    auto *listing = ::fdopendir(duplicate);
    if (!listing && duplicate >= 0) ::close(duplicate);
    std::unique_ptr<DIR, decltype(close_directory)> entries(listing, close_directory);
    require(entries != nullptr, "cannot inventory trace store");
    std::vector<Stored> retained;
    std::uint64_t total = 0U;
    bool already_stored = false;
    for (;;) {
      errno = 0;
      const auto *entry = ::readdir(entries.get());
      require(entry != nullptr || errno == 0, "cannot finish trace store inventory");
      if (!entry) break;
      const std::string name(entry->d_name);
      if (name == "." || name == "..") continue;
      require(retained.size() < kStoreEntries && (trace_name(name) || name == kTemporary),
              "trace store inventory is excessive or contains unknown files");
      struct stat status {};
      require(::fstatat(root.get(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0 &&
                  S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
                  status.st_nlink == 1 && (status.st_mode & 0777) == 0600 &&
                  status.st_size >= 0 &&
                  static_cast<std::uint64_t>(status.st_size) <= kMaximumTraceBytes,
              "trace store contains an unsafe or oversized file");
      const auto charge = std::max(static_cast<std::uint64_t>(status.st_size),
          static_cast<std::uint64_t>(status.st_blocks) * 512U);
      total += charge;
      if (total > kStoreBytes)
        return error("motion_trace_store_full", "trace store already exceeds its aggregate cap");
      already_stored = already_stored || name == filename;
      retained.push_back({name, charge, status.st_mtim});
    }
    entries.reset();
    // The exclusive directory lock proves this exact temporary is from an
    // interrupted writer. It counts towards the cap before removal.
    for (auto it = retained.begin(); it != retained.end(); ++it)
      if (it->name == kTemporary) {
        require(::unlinkat(root.get(), kTemporary, 0) == 0, "cannot remove stale trace temporary");
        total -= it->bytes;
        retained.erase(it);
        break;
      }
    std::sort(retained.begin(), retained.end(), [](const Stored &a, const Stored &b) {
      return std::tie(a.modified.tv_sec, a.modified.tv_nsec, a.name) <
             std::tie(b.modified.tv_sec, b.modified.tv_nsec, b.name);
    });
    if (!already_stored) {
      // Reserve an allocation block for the one in-progress file as well as
      // its encoded bytes; readers hold a shared lock through their file read.
      struct stat root_status {};
      require(::fstat(root.get(), &root_status) == 0 && root_status.st_blksize > 0,
              "cannot determine trace store allocation size");
      const auto block = static_cast<std::uint64_t>(root_status.st_blksize);
      const auto reserved = (bytes.size() + block - 1U) / block * block;
      while (!retained.empty() &&
             (total + reserved > kStoreBytes || retained.size() + 1U >= kStoreEntries)) {
        require(::unlinkat(root.get(), retained.front().name.c_str(), 0) == 0,
                "cannot evict the oldest trace snapshot");
        total -= retained.front().bytes;
        retained.erase(retained.begin());
      }
      if (total + reserved > kStoreBytes)
        return error("motion_trace_store_full", "trace store cannot fit this bounded snapshot");
      Fd temporary(::openat(root.get(), kTemporary,
          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
      require(temporary.get() >= 0, "cannot create trace snapshot temporary");
      bool renamed = false;
      try {
        std::size_t offset = 0U;
        while (offset < bytes.size()) {
          const auto written = ::write(temporary.get(), bytes.data() + offset, bytes.size() - offset);
          if (written < 0 && errno == EINTR) continue;
          require(written > 0, "cannot write trace snapshot");
          offset += static_cast<std::size_t>(written);
        }
        struct stat status {};
        require(::fstat(temporary.get(), &status) == 0 &&
                    total + std::max(static_cast<std::uint64_t>(status.st_size),
                      static_cast<std::uint64_t>(status.st_blocks) * 512U) <= kStoreBytes,
                "trace snapshot allocation exceeds aggregate cap");
        require(::fsync(temporary.get()) == 0 &&
                    ::renameat(root.get(), kTemporary, root.get(), filename.c_str()) == 0,
                "cannot publish trace snapshot");
        renamed = true;
        require(::fsync(root.get()) == 0, "cannot persist trace store directory");
      } catch (...) {
        if (!renamed) ::unlinkat(root.get(), kTemporary, 0);
        throw;
      }
    }
    const auto *coverage = snapshot.value().find_object_member("coverage");
    bool partial = false;
    for (const auto *key : {"truncated", "evicted_goals", "omitted_captures", "dropped_samples",
                           "omitted_samples", "evicted_scale_events", "omitted_scale_events",
                           "capture_failures", "association_missing"}) {
      const auto *value = coverage->find_object_member(key);
      const auto encoded = content_identity::canonical_json(*value);
      partial = partial || (encoded != "false" && encoded != "0");
    }
    return core::Result<V>::Ok(V(V::Object{
        {"schema_version", V("manyforge.motion_trace_file.v1")},
        {"file", V(filename)}, {"bytes", V(static_cast<double>(bytes.size()))},
        {"sha256", V(digest)}, {"request_owner_epoch", V(owner.value())},
        {"status", V(partial ? "partial" : "complete")}}));
  } catch (const std::exception &failure) {
    return core::Result<V>::Err({"motion_trace_store_unavailable", failure.what(), {}});
  }
}
} // namespace manyforge::ros2_control_execution
