#include <manyforge_roboplan/retained_isaac_arm_guard_binding.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace manyforge::roboplan_backend {
namespace {

using GuardStatusMessage = diagnostic_msgs::msg::DiagnosticStatus;
using Gid = std::array<std::uint8_t, RMW_GID_STORAGE_SIZE>;

[[nodiscard]] std::optional<std::uint64_t> parse_unsigned(std::string_view text) {
  std::uint64_t value = 0U;
  if (text.empty()) return std::nullopt;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}

/// Compact JSON array of strings (the guard's `json.dumps(..., separators=
/// (",", ":"))` of its joint names). Only the escapes such names can need are
/// accepted; anything else is unparsable and therefore not trusted.
[[nodiscard]] std::optional<std::vector<std::string>>
parse_joint_names(std::string_view text) {
  std::vector<std::string> names;
  std::size_t index = 0U;
  const auto at_end = [&] { return index >= text.size(); };
  if (at_end() || text[index++] != '[') return std::nullopt;
  if (!at_end() && text[index] == ']') {
    ++index;
    return at_end() ? std::optional(names) : std::nullopt;
  }
  while (true) {
    if (at_end() || text[index++] != '"') return std::nullopt;
    std::string name;
    while (true) {
      if (at_end()) return std::nullopt;
      const char character = text[index++];
      if (character == '"') break;
      if (static_cast<unsigned char>(character) < 0x20U) return std::nullopt;
      if (character != '\\') {
        name.push_back(character);
        continue;
      }
      if (at_end()) return std::nullopt;
      switch (text[index++]) {
      case '"':
        name.push_back('"');
        break;
      case '\\':
        name.push_back('\\');
        break;
      case '/':
        name.push_back('/');
        break;
      default:
        return std::nullopt;
      }
    }
    names.push_back(std::move(name));
    if (at_end()) return std::nullopt;
    const char separator = text[index++];
    if (separator == ']') break;
    if (separator != ',') return std::nullopt;
  }
  return at_end() ? std::optional(names) : std::nullopt;
}

} // namespace

struct RetainedIsaacArmGuardBinding::Event final {
  struct JointStateReceived {
    core::TimePoint receipt_ns;
  };
  struct StatusReceived {
    ParsedStatus parsed;
    Gid publisher;
  };
  std::variant<JointStateReceived, StatusReceived> value;
};

RetainedIsaacArmGuardBinding::ParsedStatus
RetainedIsaacArmGuardBinding::ParseStatus(const GuardStatusMessage &message) {
  ParsedStatus parsed;
  std::optional<std::string_view> state;
  std::optional<std::string_view> reason;
  std::optional<std::string_view> seconds;
  std::optional<std::string_view> nanoseconds;
  std::optional<std::string_view> joint_names;
  for (const auto &value : message.values) {
    std::optional<std::string_view> *slot = nullptr;
    if (value.key == "state") slot = &state;
    else if (value.key == "reason") slot = &reason;
    else if (value.key == "authorized_after.sec") slot = &seconds;
    else if (value.key == "authorized_after.nanosec") slot = &nanoseconds;
    else if (value.key == "joint_names") slot = &joint_names;
    if (slot == nullptr) continue;
    if (slot->has_value()) return {};  // duplicated decision value: malformed
    *slot = value.value;
  }
  if (joint_names) parsed.joint_names = parse_joint_names(*joint_names);
  if (!state || seconds.has_value() != nanoseconds.has_value()) return parsed;
  IsaacGuardStatus status;
  if (*state == "closed") status.state = IsaacGuardState::kClosed;
  else if (*state == "open") status.state = IsaacGuardState::kOpen;
  else if (*state == "tripped") status.state = IsaacGuardState::kTripped;
  else return parsed;
  if (reason) status.reason = std::string(*reason);
  if (seconds) {
    const auto sec = parse_unsigned(*seconds);
    const auto nsec = parse_unsigned(*nanoseconds);
    constexpr auto kMaxSeconds =
        static_cast<std::uint64_t>(std::numeric_limits<core::TimePoint>::max() / 1'000'000'000);
    if (!sec || !nsec || *nsec >= 1'000'000'000U || *sec >= kMaxSeconds) return parsed;
    status.authorized_after_ns =
        static_cast<core::TimePoint>(*sec) * 1'000'000'000 + static_cast<core::TimePoint>(*nsec);
  }
  parsed.status = status;
  return parsed;
}

std::shared_ptr<RetainedIsaacArmGuardBinding> RetainedIsaacArmGuardBinding::Create(
    RetainedIsaacArmGuardBindingConfig config, rclcpp::Node::SharedPtr node,
    std::shared_ptr<IRetainedIsaacArmHost> host, core::IClock &clock) {
  if (!node || !host || config.joint_commands_topic.empty() ||
      config.joint_commands_topic.front() != '/' || config.joint_states_topic.empty() ||
      config.joint_state_max_age_ns <= 0 || config.step_period_ns <= 0) {
    throw std::invalid_argument(
        "Isaac guard binding requires the arm command topic, the joint-state topic, "
        "its maximum age and the step period");
  }
  const auto command_topic = config.joint_commands_topic;
  const auto joint_states_topic = config.joint_states_topic;
  std::shared_ptr<RetainedIsaacArmGuardBinding> binding(
      new RetainedIsaacArmGuardBinding(std::move(config), std::move(host), clock));
  const std::weak_ptr<RetainedIsaacArmGuardBinding> weak = binding;
  // The guard subscribes with reliable, volatile, depth 1.
  binding->authorization_ = node->create_publisher<builtin_interfaces::msg::Time>(
      command_topic + "/authorization", rclcpp::QoS(1).reliable());
  binding->guard_status_ = node->create_subscription<GuardStatusMessage>(
      command_topic + "/guard_status", rclcpp::QoS(1).reliable().transient_local(),
      [weak](const GuardStatusMessage::ConstSharedPtr message, const rclcpp::MessageInfo &info) {
        const auto self = weak.lock();
        if (!self) return;
        Gid publisher{};
        std::memcpy(publisher.data(), info.get_rmw_message_info().publisher_gid.data,
                    publisher.size());
        self->Enqueue(Event{Event::StatusReceived{ParseStatus(*message), publisher}});
      });
  binding->joint_states_ = node->create_subscription<sensor_msgs::msg::JointState>(
      joint_states_topic, rclcpp::SensorDataQoS(),
      [weak](const sensor_msgs::msg::JointState::ConstSharedPtr) {
        if (const auto self = weak.lock()) {
          self->Enqueue(Event{Event::JointStateReceived{self->clock_.now()}});
        }
      });
  binding->worker_ = std::jthread([raw = binding.get()](std::stop_token stop) { raw->Run(stop); });
  return binding;
}

RetainedIsaacArmGuardBinding::RetainedIsaacArmGuardBinding(
    RetainedIsaacArmGuardBindingConfig config, std::shared_ptr<IRetainedIsaacArmHost> host,
    core::IClock &clock)
    : config_(std::move(config)),
      host_(std::move(host)),
      clock_(clock),
      guard_(IsaacArmGuardConfig{config_.joint_state_max_age_ns}) {}

RetainedIsaacArmGuardBinding::~RetainedIsaacArmGuardBinding() {
  worker_.request_stop();
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void RetainedIsaacArmGuardBinding::Enqueue(Event event) {
  {
    std::lock_guard lock(mutex_);
    events_.push_back(std::move(event));
  }
  wake_.notify_all();
}

void RetainedIsaacArmGuardBinding::PublishDerivedLocked() {
  status_.motion_admitted = guard_.MotionAdmitted() && !status_.joint_names_mismatch;
  status_.tripped = guard_.Tripped();
  status_.activation_allowed = guard_.JtcActivationAllowed();
  status_.pending_authorization_ns = guard_.PendingAuthorization();
}

void RetainedIsaacArmGuardBinding::Run(const std::stop_token stop) {
  const auto period = std::chrono::nanoseconds(config_.step_period_ns);
  auto next_step = std::chrono::steady_clock::now();
  while (!stop.stop_requested()) {
    std::unique_lock lock(mutex_);
    wake_.wait_until(lock, stop, next_step, [this] { return !events_.empty(); });
    if (stop.stop_requested()) return;
    // Observations in arrival order; each one's effects before the next.
    while (!events_.empty()) {
      auto event = std::move(events_.front());
      events_.pop_front();
      IsaacArmGuardActions actions;
      if (const auto *joint = std::get_if<Event::JointStateReceived>(&event.value)) {
        actions = guard_.OnJointStateReceived(joint->receipt_ns);
      } else {
        auto &received = std::get<Event::StatusReceived>(event.value);
        ++status_.statuses_received;
        if (guard_publisher_.has_value() && *guard_publisher_ != received.publisher) {
          // A restarted or rematched guard: nothing it reports is trusted
          // before the reconnect handling, which requires a fresh
          // authorization (a restarted guard starts closed).
          ++status_.guard_reconnects;
          actions = guard_.OnGuardReconnected();
        }
        guard_publisher_ = received.publisher;
        const auto expected = host_->ControllerJointNames();
        if (!received.parsed.joint_names || *received.parsed.joint_names != expected) {
          ++status_.statuses_rejected;
          status_.joint_names_mismatch = true;
          status_.last_diagnostic =
              received.parsed.joint_names
                  ? "isaac_guard_joint_names_differ_from_controller_order"
                  : "isaac_guard_joint_names_missing_or_unparsable";
        } else if (!received.parsed.status) {
          ++status_.statuses_rejected;
          status_.last_diagnostic = "isaac_guard_status_values_malformed";
        } else {
          status_.joint_names_mismatch = false;
          const auto decided = guard_.OnGuardStatus(*received.parsed.status);
          actions.deactivate_jtc = actions.deactivate_jtc || decided.deactivate_jtc;
          if (decided.fault_reason) actions.fault_reason = decided.fault_reason;
        }
      }
      if (actions.fault_reason) trip_reason_ = actions.fault_reason;
      PublishDerivedLocked();
      lock.unlock();
      Apply(actions, stop);
      lock.lock();
    }
    if (std::chrono::steady_clock::now() < next_step) continue;
    next_step = std::chrono::steady_clock::now() + period;
    lock.unlock();
    const auto facts = host_->ReadFacts();
    lock.lock();
    const auto actions = guard_.Step(clock_.now(), facts);
    if (actions.fault_reason) trip_reason_ = actions.fault_reason;
    PublishDerivedLocked();
    lock.unlock();
    Apply(actions, stop);
  }
}

void RetainedIsaacArmGuardBinding::Apply(const IsaacArmGuardActions &actions,
                                         const std::stop_token stop) {
  if (actions.feedback_stale.has_value()) {
    std::lock_guard lock(mutex_);
    status_.feedback_stale = *actions.feedback_stale;
    if (*actions.feedback_stale) status_.last_diagnostic = "isaac_joint_states_stale";
  }
  if (actions.authorize_after_ns) {
    builtin_interfaces::msg::Time stamp;
    stamp.sec = static_cast<std::int32_t>(*actions.authorize_after_ns / 1'000'000'000);
    stamp.nanosec = static_cast<std::uint32_t>(*actions.authorize_after_ns % 1'000'000'000);
    authorization_->publish(stamp);
    std::lock_guard lock(mutex_);
    status_.last_authorization_sent_ns = actions.authorize_after_ns;
  }
  if (actions.fault_reason) {
    // Item 44 first, so the motion ends InterventionRequired rather than as a
    // controller-binding loss; then the containment (Q41).
    host_->ReportFault(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInternalError)),
        "Isaac command guard tripped: " + *actions.fault_reason,
        {{"reason", "isaac_command_guard_tripped"},
         {"reason_code", "isaac_command_guard_tripped"},
         {"guard_reason", *actions.fault_reason}}});
  }
  if (actions.deactivate_jtc) host_->DeactivateForFault(stop);
  // `activate_jtc` (Reset after a trip) is the CCLC Enable that follows the
  // operator's Recover; nothing is switched on from here.
}

RetainedRos2ControlArmGuardHooks RetainedIsaacArmGuardBinding::hooks() {
  // The handle owns the hooks; each keeps this binding alive.
  const auto self = shared_from_this();
  RetainedRos2ControlArmGuardHooks hooks;
  hooks.motion_admitted = [this, self] {
    std::lock_guard lock(mutex_);
    return guard_.MotionAdmitted() && !status_.joint_names_mismatch;
  };
  hooks.before_activation = [this, self]() -> core::Result<void> {
    std::lock_guard lock(mutex_);
    const auto refuse = [](std::string reason, std::string message) {
      return core::Result<void>::Err(core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kExecutionRejected)), std::move(message),
          {{"reason", std::move(reason)}}});
    };
    if (!guard_.JtcActivationAllowed()) {
      return refuse("isaac_joint_states_not_received",
                    "The arm controller is activated only after real Isaac joint states");
    }
    if (guard_.Tripped()) {
      return refuse("isaac_command_guard_tripped",
                    "The tripped Isaac command guard needs the operator's Recover first");
    }
    return core::Result<void>::Ok();
  };
  hooks.on_recover = [this, self] {
    std::lock_guard lock(mutex_);
    // Q41: the Reset sequence; the CCLC Enable that follows reactivates the
    // controller on a new binding epoch, then the hold is authorized.
    (void)guard_.OnResetRequested();
    trip_reason_.reset();
    PublishDerivedLocked();
  };
  hooks.fault = [this, self]() -> std::optional<std::string> {
    std::lock_guard lock(mutex_);
    if (!guard_.Tripped()) return std::nullopt;
    return trip_reason_.value_or("isaac_command_guard_tripped");
  };
  return hooks;
}

RetainedIsaacArmGuardBindingStatus RetainedIsaacArmGuardBinding::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

namespace {

class HandleArmHost final : public IRetainedIsaacArmHost {
public:
  explicit HandleArmHost(std::weak_ptr<RetainedRos2ControlRuntimeHandle> handle)
      : handle_(std::move(handle)) {}

  IsaacArmGuardFacts ReadFacts() override {
    IsaacArmGuardFacts facts;
    const auto handle = handle_.lock();
    if (!handle) return facts;
    const auto port = handle->current_port();
    const auto participant = handle->participant();
    if (!port) return facts;
    facts.readiness = port->ReadReadiness();
    facts.sample = port->ReadSample();
    facts.goal_executing = participant && participant->GoalInFlight();
    return facts;
  }

  std::vector<std::string> ControllerJointNames() override {
    const auto handle = handle_.lock();
    return handle ? handle->controller_joint_names() : std::vector<std::string>{};
  }

  void ReportFault(core::ErrorDTO error) override {
    const auto handle = handle_.lock();
    if (!handle) return;
    if (const auto participant = handle->participant()) {
      participant->ReportCriticalFault(std::move(error));
    }
  }

  void DeactivateForFault(const std::stop_token cancellation) override {
    if (const auto handle = handle_.lock()) {
      (void)handle->DeactivateArmControllerForFault(cancellation);
    }
  }

private:
  std::weak_ptr<RetainedRos2ControlRuntimeHandle> handle_;
};

} // namespace

std::shared_ptr<IRetainedIsaacArmHost>
make_retained_isaac_arm_host(std::weak_ptr<RetainedRos2ControlRuntimeHandle> handle) {
  return std::make_shared<HandleArmHost>(std::move(handle));
}

} // namespace manyforge::roboplan_backend
