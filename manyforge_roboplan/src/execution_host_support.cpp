#include <manyforge_roboplan/execution_host_support.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef __linux__
#include <malloc.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace manyforge::roboplan_backend {

// --- WallClock ---

core::TimePoint WallClock::now() const {
  auto tp = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             tp.time_since_epoch())
      .count();
}

// --- SimpleScheduler ---

namespace {

core::TimePoint steady_now_ns() {
  auto tp = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             tp.time_since_epoch())
      .count();
}

std::string trim_ascii(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string uppercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

std::optional<std::string> environment_value(
    const std::map<std::string, std::string>& environment,
    const std::string& key) {
  const auto it = environment.find(key);
  if (it == environment.end()) {
    return std::nullopt;
  }
  const auto value = trim_ascii(it->second);
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

int parse_bounded_integer(const std::string& text, const std::string& name,
                          int minimum, int maximum) {
  std::size_t consumed = 0;
  long parsed = 0;
  try {
    parsed = std::stol(text, &consumed, 10);
  } catch (const std::exception&) {
    throw std::invalid_argument(name + " must be an integer");
  }
  if (consumed != text.size() || parsed < minimum || parsed > maximum) {
    std::ostringstream message;
    message << name << " must be in [" << minimum << ", " << maximum << "]";
    throw std::invalid_argument(message.str());
  }
  return static_cast<int>(parsed);
}

std::vector<int> parse_cpu_affinity(const std::string& text,
                                    const std::string& name) {
#ifdef __linux__
  constexpr int kMaximumCpuIndex = CPU_SETSIZE - 1;
#else
  constexpr int kMaximumCpuIndex = 1023;
#endif
  std::set<int> cpus;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const auto comma = text.find(',', begin);
    const auto token = trim_ascii(text.substr(
        begin, comma == std::string::npos ? std::string::npos : comma - begin));
    if (token.empty()) {
      throw std::invalid_argument(name + " contains an empty CPU token");
    }
    const auto dash = token.find('-');
    if (dash == std::string::npos) {
      cpus.insert(parse_bounded_integer(token, name, 0,
                                        kMaximumCpuIndex));
    } else {
      if (token.find('-', dash + 1) != std::string::npos) {
        throw std::invalid_argument(name + " contains an invalid CPU range");
      }
      const auto first = parse_bounded_integer(
          trim_ascii(token.substr(0, dash)), name, 0, kMaximumCpuIndex);
      const auto last = parse_bounded_integer(
          trim_ascii(token.substr(dash + 1)), name, 0, kMaximumCpuIndex);
      if (last < first) {
        throw std::invalid_argument(name + " contains a descending CPU range");
      }
      for (int cpu = first; cpu <= last; ++cpu) {
        cpus.insert(cpu);
      }
    }
    if (comma == std::string::npos) {
      break;
    }
    begin = comma + 1;
  }
  if (cpus.empty()) {
    throw std::invalid_argument(name + " must select at least one CPU");
  }
  return {cpus.begin(), cpus.end()};
}

const char* os_scheduler_policy_name(int policy) {
  switch (policy) {
#ifdef __linux__
    case SCHED_OTHER:
      return "SCHED_OTHER";
    case SCHED_FIFO:
      return "SCHED_FIFO";
    case SCHED_RR:
      return "SCHED_RR";
#endif
    default:
      return "UNKNOWN";
  }
}

std::string format_cpu_affinity(const std::vector<int>& cpus) {
  std::ostringstream value;
  for (std::size_t i = 0; i < cpus.size(); ++i) {
    if (i != 0) {
      value << ',';
    }
    value << cpus[i];
  }
  return value.str();
}

bool cpu_affinities_overlap(const std::vector<int>& lhs,
                            const std::vector<int>& rhs) {
  return std::any_of(lhs.begin(), lhs.end(), [&](const int cpu) {
    return std::find(rhs.begin(), rhs.end(), cpu) != rhs.end();
  });
}

#ifdef __linux__
// OD-59: the bounded part of a priority worker's stack touched before it
// reports startup (its default stack is 8 MiB; the worker's frames use far
// less).
constexpr std::size_t kWorkerStackPrefaultBytes = 256U * 1024U;

[[gnu::noinline]] void prefault_worker_stack() {
  volatile unsigned char stack[kWorkerStackPrefaultBytes];
  const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  for (std::size_t offset = 0; offset < sizeof(stack); offset += page) stack[offset] = 0;
  stack[sizeof(stack) - 1U] = 0;
}
#endif

// OD-59 (strict profile): an empty string on success, else the reason.
std::string lock_process_memory() {
#ifndef __linux__
  return "process memory locking is supported only on Linux";
#else
  if (::mallopt(M_TRIM_THRESHOLD, -1) != 1)
    return "mallopt(M_TRIM_THRESHOLD, -1) failed";
  if (::mallopt(M_MMAP_MAX, 0) != 1) return "mallopt(M_MMAP_MAX, 0) failed";
  if (::mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) != 0)
    return std::string("mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) failed: ") +
           std::strerror(errno) + " (the strict profile needs an unlimited memlock limit)";
  return {};
#endif
}

std::string apply_worker_os_config(const SchedulerWorkerOsConfig& config) {
  if (!config.requested()) {
    return {};
  }
#ifndef __linux__
  return "native scheduler OS profiles are supported only on Linux";
#else
  if (!config.cpu_affinity.empty()) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    for (const int cpu : config.cpu_affinity) {
      if (cpu < 0 || cpu >= CPU_SETSIZE) {
        return "requested CPU exceeds CPU_SETSIZE";
      }
      CPU_SET(cpu, &cpu_set);
    }
    const int affinity_error =
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
    if (affinity_error != 0) {
      return std::string("pthread_setaffinity_np failed: ") +
             std::strerror(affinity_error);
    }
  }
  if (config.policy.has_value()) {
    sched_param parameters{};
    parameters.sched_priority = config.priority;
    const int scheduler_error =
        pthread_setschedparam(pthread_self(), *config.policy, &parameters);
    if (scheduler_error != 0) {
      return std::string("pthread_setschedparam failed: ") +
             std::strerror(scheduler_error);
    }
  }
  return {};
#endif
}

// Typed containment reason for a throwing callback on a Stop/safety-path
// lane; empty for the lanes that only log and continue.
std::string_view safety_path_fault_reason(core::SchedulerPriority priority) {
  switch (priority) {
    case core::SchedulerPriority::ControllerCommand:
      return "scheduler_controller_command_callback_exception";
    case core::SchedulerPriority::RuntimeSafety:
      return "scheduler_runtime_safety_callback_exception";
    case core::SchedulerPriority::StopControl:
      return "scheduler_stop_control_callback_exception";
    case core::SchedulerPriority::Lifecycle:
    case core::SchedulerPriority::Monitoring:
    case core::SchedulerPriority::Background:
      return {};
  }
  return "scheduler_unknown_lane_callback_exception";
}

// Written for the first throw per lane only, so a repeating throw cannot grow
// the log. A contained lane's later throws are counted by the Kernel internal
// fault (`later_count` in the runtime-fault status).
void log_scheduler_callback_exception(core::SchedulerPriority priority,
                                      const char* exception_text,
                                      bool contained) {
  std::cerr << "manyforge_scheduler_callback_exception"
            << " priority=" << core::scheduler_priority_name(priority)
            << " contained=" << (contained ? "true" : "false")
            << " exception=" << exception_text
            << " later_throws=not_logged" << std::endl;
}

}  // namespace

SimpleSchedulerConfig SimpleSchedulerConfig::from_environment() {
  static constexpr const char* kKeys[] = {
      "MANYFORGE_RT_RESOURCE_PROFILE",
      "MANYFORGE_RT_SCHED_POLICY",
      "MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY",
      "MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY",
      "MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY",
      "MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY",
      "MANYFORGE_RT_STOP_CONTROL_PRIORITY",
      "MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY",
  };
  std::map<std::string, std::string> environment;
  for (const char* key : kKeys) {
    if (const char* value = std::getenv(key); value != nullptr) {
      environment.emplace(key, value);
    }
  }
  return from_environment(environment);
}

SimpleSchedulerConfig SimpleSchedulerConfig::from_environment(
    const std::map<std::string, std::string>& environment) {
  SimpleSchedulerConfig config;
  const auto profile = environment_value(environment,
                                         "MANYFORGE_RT_RESOURCE_PROFILE");
  if (!profile || uppercase_ascii(*profile) == "OFF") {
    return config;
  }
  if (uppercase_ascii(*profile) != "STRICT") {
    throw std::invalid_argument(
        "MANYFORGE_RT_RESOURCE_PROFILE must be 'off' or 'strict'");
  }

#ifndef __linux__
  throw std::runtime_error(
      "the strict ManyForge RT resource profile requires Linux");
#else
  const auto requested_policy = uppercase_ascii(
      environment_value(environment, "MANYFORGE_RT_SCHED_POLICY")
          .value_or("FIFO"));
  int policy = 0;
  if (requested_policy == "FIFO" || requested_policy == "SCHED_FIFO") {
    policy = SCHED_FIFO;
  } else if (requested_policy == "RR" ||
             requested_policy == "SCHED_RR") {
    policy = SCHED_RR;
  } else {
    throw std::invalid_argument(
        "MANYFORGE_RT_SCHED_POLICY must be FIFO or RR");
  }

  const auto configure_role = [&](core::SchedulerPriority role,
                                  const std::string& priority_key,
                                  int default_priority,
                                  const std::string& affinity_key) {
    auto& worker = config.workers[core::scheduler_priority_index(role)];
    worker.policy = policy;
    worker.priority = parse_bounded_integer(
        environment_value(environment, priority_key)
            .value_or(std::to_string(default_priority)),
        priority_key, 1, 99);
    const auto affinity = environment_value(environment, affinity_key);
    if (!affinity) {
      throw std::invalid_argument(affinity_key +
                                  " is required by the strict RT profile");
    }
    worker.cpu_affinity = parse_cpu_affinity(*affinity, affinity_key);
    worker.required = true;
    worker.prefault_stack = true;
  };
  config.lock_memory = true;

  // OD-58: StopControl is the highest-priority lane and has its own CPUs
  // (no default share with ControllerCommand).
  configure_role(core::SchedulerPriority::ControllerCommand,
                 "MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY", 45,
                 "MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY");
  configure_role(core::SchedulerPriority::RuntimeSafety,
                 "MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY", 40,
                 "MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY");
  configure_role(core::SchedulerPriority::StopControl,
                 "MANYFORGE_RT_STOP_CONTROL_PRIORITY", 48,
                 "MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY");

  const auto& controller =
      config.workers[core::scheduler_priority_index(
          core::SchedulerPriority::ControllerCommand)];
  const auto& safety =
      config.workers[core::scheduler_priority_index(
          core::SchedulerPriority::RuntimeSafety)];
  const auto& stop =
      config.workers[core::scheduler_priority_index(
          core::SchedulerPriority::StopControl)];
  if (!(stop.priority > controller.priority &&
        controller.priority > safety.priority)) {
    throw std::invalid_argument(
        "strict native scheduler priorities must satisfy "
        "stop_control > controller_command > runtime_safety");
  }
  if (cpu_affinities_overlap(controller.cpu_affinity,
                             safety.cpu_affinity) ||
      cpu_affinities_overlap(safety.cpu_affinity, stop.cpu_affinity)) {
    throw std::invalid_argument(
        "strict native scheduler RuntimeSafety CPUs must be disjoint from "
        "ControllerCommand and StopControl CPUs");
  }
  return config;
#endif
}

SimpleScheduler::SimpleScheduler()
    : SimpleScheduler(SimpleSchedulerConfig::from_environment()) {}

SimpleScheduler::SimpleScheduler(SimpleSchedulerConfig config) {
  for (std::size_t i = 0; i < workers_.size(); ++i) {
    workers_[i].owner = this;
    workers_[i].priority = core::scheduler_priority_at(i);
    workers_[i].os_config = std::move(config.workers[i]);
    workers_[i].thread_name = [&]() -> const char* {
      switch (core::scheduler_priority_at(i)) {
        case core::SchedulerPriority::ControllerCommand:
          return "mf_sched_ctrl";
        case core::SchedulerPriority::RuntimeSafety:
          return "mf_sched_safe";
        case core::SchedulerPriority::StopControl:
          return "mf_sched_stop";
        case core::SchedulerPriority::Lifecycle:
          return "mf_sched_life";
        case core::SchedulerPriority::Monitoring:
          return "mf_sched_mon";
        case core::SchedulerPriority::Background:
          return "mf_sched_bg";
      }
      return "mf_sched";
    }();
  }
  // A strict profile is an admission contract, not a best-effort hint. Lock
  // the process memory, then start every requested worker now so a refused
  // lock, policy or affinity aborts kernel construction before any
  // controller or safety callback can be admitted.
  if (config.lock_memory) {
    const auto error = lock_process_memory();
    std::cerr << "manyforge_scheduler_memory_lock accepted="
              << (error.empty() ? "true" : "false");
    if (!error.empty()) std::cerr << " error=\"" << error << '"';
    std::cerr << std::endl;
    if (!error.empty())
      throw std::runtime_error("native scheduler strict profile rejected its memory lock: " +
                               error);
  }
  for (auto& worker : workers_) {
    if (worker.os_config.required) {
      std::lock_guard<std::mutex> lock(worker.mutex);
      worker.ensure_started_locked(&worker);
    }
  }
}

SimpleScheduler::~SimpleScheduler() {
  for (auto& worker : workers_) {
    if (worker.thread.joinable()) {
      worker.thread.request_stop();
      worker.cv.notify_all();
    }
  }
}

void SimpleScheduler::Worker::ensure_started_locked(Worker* self) {
  if (thread.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> startup_lock(startup_mutex);
    startup_complete = false;
    startup_error.clear();
  }
  thread = std::jthread([self](std::stop_token st) { self->worker_loop(st); });
  std::string error;
  {
    std::unique_lock<std::mutex> startup_lock(startup_mutex);
    startup_cv.wait(startup_lock, [this]() { return startup_complete; });
    error = startup_error;
  }
  if (!error.empty()) {
    // On strict startup failure the worker returns before touching its task
    // mutex, so joining while the caller retains that mutex is safe.
    thread.join();
    throw std::runtime_error("native scheduler role " +
                             std::string(core::scheduler_priority_name(priority)) +
                             " rejected its OS resource profile: " + error);
  }
}

SimpleScheduler::Worker& SimpleScheduler::worker_for(
    core::SchedulerPriority priority) {
  return workers_[core::scheduler_priority_index(priority)];
}

core::ScheduleHandle SimpleScheduler::schedule(
    core::TimePoint deadline, std::function<void()> callback,
    core::SchedulerPriority priority) {
  auto handle = next_handle_.fetch_add(1);
  Worker& w = worker_for(priority);
  std::unique_lock<std::mutex> registry_lock(registry_mutex_);
  if (admission_closed_) throw std::runtime_error("scheduler admission is closed");
  handle_to_priority_[handle] = priority;
  try {
    std::lock_guard<std::mutex> lock(w.mutex);
    w.ensure_started_locked(&w);
    ++w.queue_generation;
    w.tasks.emplace(handle, ScheduledTask{deadline, std::move(callback)});
  } catch (...) {
    // A strict worker can reject construction before accepting a callback, and
    // allocation can fail while inserting the task. Neither path may leave a
    // phantom handle that cancel()/diagnostics still consider live.
    handle_to_priority_.erase(handle);
    throw;
  }
  registry_lock.unlock();
  w.cv.notify_all();
  return handle;
}

void SimpleScheduler::close_admission_and_discard() {
  std::array<std::map<core::ScheduleHandle, ScheduledTask>, core::kSchedulerPriorityCount>
      discarded;
  {
    std::lock_guard<std::mutex> registry_lock(registry_mutex_);
    admission_closed_ = true;
    for (std::size_t i = 0; i < workers_.size(); ++i) {
      auto& worker = workers_[i];
      std::lock_guard<std::mutex> worker_lock(worker.mutex);
      discarded[i].swap(worker.tasks);
      ++worker.queue_generation;
    }
    handle_to_priority_.clear();
  }
  for (auto& worker : workers_) worker.cv.notify_all();
  // Captured owners are released here, outside all scheduler locks.
}

void SimpleScheduler::cancel(core::ScheduleHandle handle) {
  static_cast<void>(cancel_before_dequeue(handle));
}

bool SimpleScheduler::cancel_before_dequeue(core::ScheduleHandle handle) {
  core::SchedulerPriority priority;
  {
    std::lock_guard<std::mutex> registry_lock(registry_mutex_);
    auto it = handle_to_priority_.find(handle);
    if (it == handle_to_priority_.end()) {
      return false;  // already fired or never scheduled
    }
    priority = it->second;
    handle_to_priority_.erase(it);
  }
  Worker& w = worker_for(priority);
  decltype(w.tasks)::node_type removed;
  {
    std::lock_guard<std::mutex> lock(w.mutex);
    ++w.queue_generation;
    // Zero when the worker already dequeued it (the registry entry is erased
    // only after the dequeue), so a running callback is never reported removed.
    removed = w.tasks.extract(handle);
  }
  w.cv.notify_all();
  return !removed.empty();
}

void SimpleScheduler::set_safety_path_fault_sink(
    std::function<void(std::string_view)> sink) {
  std::lock_guard<std::mutex> lock(safety_path_fault_sink_mutex_);
  safety_path_fault_sink_ = std::move(sink);
}

void SimpleScheduler::contain_safety_path_fault(
    std::string_view reason_code) noexcept {
  std::lock_guard<std::mutex> lock(safety_path_fault_sink_mutex_);
  if (!safety_path_fault_sink_) {
    std::terminate();
  }
  safety_path_fault_sink_(reason_code);
}

void SimpleScheduler::Worker::drain() {
  std::unique_lock<std::mutex> lock(mutex);
  if (thread.joinable() && std::this_thread::get_id() == thread.get_id()) {
    return;
  }
  drain_cv.wait(lock, [this]() { return callbacks_in_flight == 0; });
}

bool SimpleScheduler::Worker::drain_for(std::chrono::nanoseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex);
  if (thread.joinable() && std::this_thread::get_id() == thread.get_id()) {
    return true;
  }
  return drain_cv.wait_for(
      lock, timeout, [this]() { return callbacks_in_flight == 0; });
}

void SimpleScheduler::drain(core::SchedulerPriority priority) {
  worker_for(priority).drain();
}

bool SimpleScheduler::drain_for(core::SchedulerPriority priority,
                                std::chrono::nanoseconds timeout) {
  return worker_for(priority).drain_for(timeout);
}

void SimpleScheduler::drain_all() {
  for (auto& worker : workers_) {
    worker.drain();
  }
}

bool SimpleScheduler::drain_all_for(std::chrono::nanoseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  const auto maximum = std::chrono::steady_clock::time_point::max() - start;
  const auto bounded = std::max(std::chrono::nanoseconds::zero(), timeout);
  const auto deadline = bounded >= maximum
                            ? std::chrono::steady_clock::time_point::max()
                            : start + bounded;
  bool drained = true;
  for (auto& worker : workers_) {
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = now < deadline
        ? std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now)
        : std::chrono::nanoseconds::zero();
    drained = worker.drain_for(remaining) && drained;
  }
  return drained;
}

void SimpleScheduler::Worker::worker_loop(std::stop_token st) {
#ifdef __linux__
  if (thread_name != nullptr) {
    (void)pthread_setname_np(pthread_self(), thread_name);
  }
#endif
  const auto os_error = apply_worker_os_config(os_config);
  const bool startup_failed = !os_error.empty() && os_config.required;
#ifdef __linux__
  if (!startup_failed && os_config.prefault_stack) prefault_worker_stack();
#endif
  if (os_config.requested()) {
    std::cerr << "manyforge_scheduler_os_profile"
              << " role=" << core::scheduler_priority_name(priority)
              << " policy="
              << (os_config.policy
                      ? os_scheduler_policy_name(*os_config.policy)
                      : "INHERIT")
              << " priority=" << os_config.priority
              << " affinity=" << format_cpu_affinity(os_config.cpu_affinity)
              << " required=" << (os_config.required ? "true" : "false")
              << " accepted=" << (os_error.empty() ? "true" : "false");
    if (!os_error.empty()) {
      std::cerr << " error=\"" << os_error << '"';
    }
    std::cerr << std::endl;
  }
  {
    std::lock_guard<std::mutex> startup_lock(startup_mutex);
    startup_error = startup_failed ? os_error : std::string{};
    startup_complete = true;
  }
  startup_cv.notify_all();
  if (startup_failed) {
    return;
  }
  while (!st.stop_requested()) {
    core::ScheduleHandle fired_handle = 0;
    std::function<void()> callback;

    {
      std::unique_lock<std::mutex> lock(mutex);
      if (!cv.wait(lock, st, [this]() { return !tasks.empty(); })) {
        return;
      }

      const auto next_it = std::min_element(
          tasks.begin(), tasks.end(),
          [](const auto& lhs, const auto& rhs) {
            return lhs.second.deadline < rhs.second.deadline;
          });

      const auto now = steady_now_ns();
      if (next_it->second.deadline > now) {
        const auto observed_generation = queue_generation;
        (void)cv.wait_for(
            lock,
            st,
            std::chrono::nanoseconds(next_it->second.deadline - now),
            [this, observed_generation]() {
              return queue_generation != observed_generation;
            });
        continue;
      }

      callback = std::move(next_it->second.callback);
      fired_handle = next_it->first;
      tasks.erase(next_it);
      ++callbacks_in_flight;
    }

    if (owner != nullptr && fired_handle != 0) {
      std::lock_guard<std::mutex> registry_lock(owner->registry_mutex_);
      owner->handle_to_priority_.erase(fired_handle);
    }

    const auto finish_callback = [this]() {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (callbacks_in_flight > 0) {
          --callbacks_in_flight;
        }
      }
      drain_cv.notify_all();
    };

    if (callback) {
      // A throw on a Stop/safety-path lane is contained by the host (motion
      // admission closed, Full Stop attempted); the lane keeps running.
      // finish_callback() runs before the containment on purpose: draining
      // while holding the Kernel lock must not wait on this worker. So
      // "drained" means the callback returned, not that this worker is idle;
      // nothing may rely on drain() to order after the containment.
      try {
        callback();
      } catch (const std::exception& e) {
        finish_callback();
        const auto fault = safety_path_fault_reason(priority);
        if (callback_exceptions++ == 0U)
          log_scheduler_callback_exception(priority, e.what(), !fault.empty());
        if (!fault.empty()) {
          owner->contain_safety_path_fault(fault);
        }
        continue;
      } catch (...) {
        finish_callback();
        const auto fault = safety_path_fault_reason(priority);
        if (callback_exceptions++ == 0U)
          log_scheduler_callback_exception(priority,
                                           "unknown_non_std_exception",
                                           !fault.empty());
        if (!fault.empty()) {
          owner->contain_safety_path_fault(fault);
        }
        continue;
      }
    }

    finish_callback();
  }
}

}  // namespace manyforge::roboplan_backend
