#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace manyforge::deployment::test {

class ScopedProcfdRoot final {
 public:
  explicit ScopedProcfdRoot(const std::filesystem::path& root)
      : descriptor_(::open(
            root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {
    if (descriptor_ < 0) {
      throw std::runtime_error("test could not open package-root descriptor");
    }
  }

  ~ScopedProcfdRoot() {
    if (descriptor_ >= 0) {
      (void)::close(descriptor_);
    }
  }

  ScopedProcfdRoot(const ScopedProcfdRoot&) = delete;
  ScopedProcfdRoot& operator=(const ScopedProcfdRoot&) = delete;
  ScopedProcfdRoot(ScopedProcfdRoot&&) = delete;
  ScopedProcfdRoot& operator=(ScopedProcfdRoot&&) = delete;

  [[nodiscard]] std::filesystem::path path() const {
    return std::filesystem::path("/proc/self/fd") /
           std::to_string(descriptor_);
  }

 private:
  int descriptor_{-1};
};

class ScopedPathReplacement final {
 public:
  explicit ScopedPathReplacement(const std::filesystem::path& root)
      : root_(root),
        backing_(root.parent_path() /
                 (root.filename().string() + "-descriptor-backing")) {
    std::error_code error;
    (void)std::filesystem::remove_all(backing_, error);
    std::filesystem::rename(root_, backing_);
    if (!std::filesystem::create_directory(root_)) {
      std::filesystem::rename(backing_, root_);
      throw std::runtime_error(
          "test could not create replacement package root");
    }
    active_ = true;
  }

  ~ScopedPathReplacement() {
    if (!active_) {
      return;
    }
    std::error_code error;
    (void)std::filesystem::remove_all(root_, error);
    error.clear();
    std::filesystem::rename(backing_, root_, error);
  }

  ScopedPathReplacement(const ScopedPathReplacement&) = delete;
  ScopedPathReplacement& operator=(const ScopedPathReplacement&) = delete;
  ScopedPathReplacement(ScopedPathReplacement&&) = delete;
  ScopedPathReplacement& operator=(ScopedPathReplacement&&) = delete;

  [[nodiscard]] const std::filesystem::path& backing() const noexcept {
    return backing_;
  }

 private:
  std::filesystem::path root_;
  std::filesystem::path backing_;
  bool active_{false};
};

}  // namespace manyforge::deployment::test
