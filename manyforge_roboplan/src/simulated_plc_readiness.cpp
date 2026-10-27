#include "simulated_plc_readiness.hpp"
#include "io_provider_lifecycle_product.hpp"

#include <atomic>
#include <cctype>
#include <map>
#include <stdexcept>

#include <rclcpp/executors/single_threaded_executor.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_srvs/srv/set_bool.hpp>

namespace manyforge::roboplan_backend::host_detail {

SimulatedPlcReadiness
WaitForSimulatedPlcEndpoints(const std::shared_ptr<rclcpp::Context> &context,
                             const SimulatedPlcPlan &plan,
                             const std::chrono::milliseconds timeout,
                             const std::function<bool()> &abandon) {
  SimulatedPlcReadiness result;
  if (!context || !context->is_valid()) {
    result.missing_services = plan.served_services;
    result.missing_topic_samples = plan.subscribed_topics;
    return result;
  }
  static std::atomic<unsigned> probes{0U};
  std::string name = "manyforge_sim_plc_probe_";
  for (const char c : plan.device_id)
    name.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
  name += "_" + std::to_string(probes.fetch_add(1U));
  auto node = std::make_shared<rclcpp::Node>(
      name, "/manyforge",
      rclcpp::NodeOptions()
          .context(context)
          .start_parameter_services(false)
          .start_parameter_event_publisher(false)
          .use_global_arguments(false));
  rclcpp::ExecutorOptions options;
  options.context = context;
  rclcpp::executors::SingleThreadedExecutor executor(options);
  executor.add_node(node);

  std::map<std::string, rclcpp::Client<std_srvs::srv::SetBool>::SharedPtr>
      clients;
  for (const auto &service : plan.served_services)
    clients.emplace(service, node->create_client<std_srvs::srv::SetBool>(service));
  // Best effort matches any publisher reliability; only arrival matters.
  const auto qos = rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile();
  std::map<std::string, std::shared_ptr<std::atomic<bool>>> sampled;
  std::vector<rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr> subscriptions;
  for (const auto &topic : plan.subscribed_topics) {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    sampled.emplace(topic, flag);
    subscriptions.push_back(node->create_subscription<std_msgs::msg::Bool>(
        topic, qos, [flag](const std_msgs::msg::Bool &) { flag->store(true); }));
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto complete = [&] {
    for (const auto &[service, client] : clients)
      if (!client->service_is_ready())
        return false;
    for (const auto &[topic, flag] : sampled)
      if (!flag->load())
        return false;
    return true;
  };
  while (!complete() && std::chrono::steady_clock::now() < deadline &&
         context->is_valid() && !(abandon && abandon()))
    executor.spin_once(std::chrono::milliseconds(20));
  for (const auto &[service, client] : clients)
    if (!client->service_is_ready())
      result.missing_services.push_back(service);
  for (const auto &[topic, flag] : sampled)
    if (!flag->load())
      result.missing_topic_samples.push_back(topic);
  executor.remove_node(node);
  return result;
}

ControllerLaunchSupervisor::Ensured StartSimulatedPlcCandidate(
    ControllerLaunchSupervisor &supervisor, const SimulatedPlcPlan &plan,
    const std::filesystem::path &artifacts_root, const std::string &context,
    const IoProviderDriverGroup &drivers, const bool borrowed_drivers,
    const std::shared_ptr<ControllerLaunchProcess> &installed_process,
    const SimulatedPlcWait &wait) {
  const auto ensured = supervisor.ensure(simulated_plc_launch_command(plan),
                                         artifacts_root, context);
  const auto &process = ensured.process;
  // Until ownership transfers to the caller, every exceptional exit (refusal,
  // wait/ContextProvider/probe throw) discards a fresh start; kKeep is a no-op.
  struct Unowned final {
    ControllerLaunchSupervisor &supervisor;
    const ControllerLaunchSupervisor::Ensured *ensured;
    ~Unowned() {
      if (ensured) static_cast<void>(supervisor.discard_candidate(*ensured));
    }
  } unowned{supervisor, &ensured};
  const auto refuse = [&](const std::string &reason) {
    throw std::runtime_error("simulated_plc_not_ready: device=" +
                             plan.device_id + reason +
                             " config_dir=" + plan.config_dir.string());
  };
  if (borrowed_drivers &&
      (ensured.decision != ControllerLaunchDecision::kKeep ||
       process != installed_process))
    refuse(" installed process is no longer running; the retained device "
           "cannot change its simulated PLC");
  const auto readiness = wait ? wait(process) : SimulatedPlcReadiness{};
  if (const auto exited = process->exited()) {
    refuse(" process exited: " + *exited);
  } else if (!readiness.ready()) {
    std::string missing = " missing:";
    for (const auto &service : readiness.missing_services)
      missing += " service=" + service;
    for (const auto &topic : readiness.missing_topic_samples)
      missing += " topic=" + topic;
    refuse(missing);
  }
  if (!borrowed_drivers)
    for (const auto &driver : drivers)
      if (driver)
        driver->SetSupervisedProcessFaultSource(
            [source = ControllerLaunchSupervisor::fault_source(process)]()
                -> std::optional<core::ErrorDTO> {
              auto reason = source();
              if (!reason)
                return std::nullopt;
              return core::ErrorDTO{"simulated_plc_exited", std::move(*reason),
                                    {}};
            });
  unowned.ensured = nullptr;
  return ensured;
}

} // namespace manyforge::roboplan_backend::host_detail
