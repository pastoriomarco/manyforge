#pragma once

#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace manyforge::roboplan_backend {

/// RoboPlan's detailed collision diagnostics are currently emitted through
/// std::cout by `Scene::hasCollisions(q, true)`. Redirecting a process-global
/// stream buffer is inherently process-global too, so every caller in this
/// library must use this shared guard. It serializes only the diagnostic
/// capture path; ordinary binary collision checks remain parallel.
///
/// This is an interim bridge. It prevents ManyForge-owned diagnostic captures
/// and echoes from racing each other, but it cannot prevent unrelated
/// third-party code from writing to std::cout during the capture window.
/// RoboPlan should eventually expose structured collision diagnostics directly
/// so this global stream-buffer redirection can be deleted.
inline std::mutex& collision_diagnostics_stdout_mutex() {
  static std::mutex mutex;
  return mutex;
}

template <typename Fn>
std::string capture_collision_diagnostics_stdout(Fn&& fn) {
  std::lock_guard<std::mutex> lock(collision_diagnostics_stdout_mutex());
  std::ostringstream capture;
  auto* previous = std::cout.rdbuf(capture.rdbuf());
  struct Restore {
    std::streambuf* previous;
    ~Restore() { std::cout.rdbuf(previous); }
  } restore{previous};
  std::forward<Fn>(fn)();
  return capture.str();
}

}  // namespace manyforge::roboplan_backend
