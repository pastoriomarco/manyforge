#pragma once

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/ports/icomponent_adapter_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_roboplan/component_process.hpp>
#include <manyforge_roboplan/launch_contribution.hpp>
#include <manyforge_roboplan/tool_context_hook.hpp>
#include <manyforge_core/types/io_signal.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <map>
#include <memory>
#include <rclcpp/node.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::roboplan_backend {

class RetainedRos2ControlRuntimeHandle;
struct Ros2ControlLaneHooks;

/// CONTRACTS §6 I/O update sink (the host's I/O ingress): one acquisition of
/// a component's I/O signals, stamped at receipt on the host clock.
using ComponentIoUpdateSink = std::function<void(
    const std::string &component_id, const std::vector<core::IoSignalUpdateDTO> &updates,
    std::int64_t received_at_ns)>;

/// CONTRACTS §6, §12, §16: what the execution host hands an adapter family to
/// make the adapter of one component (an owner, a member or an owner-less
/// component alike). `row` is the model's `lanes.<lane>` row the Deployment's
/// lane selected (scalar keys typed; an owner's is its machine model's row);
/// `lane_row` is the whole row, for an adapter's non-scalar keys. An I/O
/// member's row is its model's lane row plus its declared signals:
/// `{adapter, bank, inputs: [name], outputs: [name]}`.
struct ComponentAdapterContext final {
  core::ControllableComponentId component_id;
  /// The component's lane; empty for a sensor (it has none).
  std::string lane;
  ModelLaneRow row;
  content_identity::CanonicalValue lane_row;
  /// CONTRACTS §16: the member slot (`arm`, `controller_io`, `tool_io`, ...);
  /// empty for an owner and for an owner-less component.
  std::string role;
  /// CONTRACTS §16: the owner's handle (`ComponentAdapters::owner_handle` of
  /// the owner's factory call) for a member; null for an owner and for an
  /// owner-less component. The host never looks inside.
  std::shared_ptr<void> owner;
  /// The absolute ROS namespace of the component's driver.
  std::string ros_namespace;
  /// The device address from the site settings; empty when none.
  std::string address;
  rclcpp::Node::SharedPtr node;
  std::shared_ptr<core::IClock> clock;
  /// The component's ros2_control connection when it runs on a ros2_control
  /// lane (Mock, Isaac, Real); null otherwise.
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> ros2_control;
  /// The ros2_control lane adapter of that connection, which a vendor
  /// adapter composes; null otherwise.
  std::shared_ptr<core::IComponentAdapter> lane_adapter;
  /// The lane's latest fresh measured joints in axis order (joint state
  /// broadcaster); empty without a ros2_control connection.
  std::function<std::optional<std::vector<double>>()> measured_joints;
  /// CONTRACTS §6 driver-restart entry: restarts the component's driver
  /// process (its controller launch) on the same command; for a driver that
  /// doesn't reconnect by itself (e.g. after a controller reboot).
  std::function<core::Result<void>(std::stop_token)> restart_driver;
  /// CONTRACTS §6 I/O update sink: where the family's I/O components hand
  /// their stream samples.
  ComponentIoUpdateSink io_updates;
  /// Whether this call makes a machine's owner (CONTRACTS §16): `role` is
  /// empty and its members get `ComponentAdapters::owner_handle`.
  bool machine_owner{};
  /// The component's compiled lifecycle command time
  /// (`lifecycle_command_timeout_ns`): the deadline of each of its prepare,
  /// enable, disable and recover actions, counted from the action's start.
  std::chrono::nanoseconds lifecycle_command_timeout{};
  /// The cell's id (instance value of a component that reports to the cell,
  /// e.g. a sensor bridge).
  std::string cell_id;
  /// CONTRACTS §6 component process: the host's launcher of the component's
  /// own process (`component_process.hpp`); a family that runs one starts it
  /// here, never by itself.
  ComponentProcessLauncher start_process;
};

/// What one sample of a display joint source is good for, by the family's
/// check.
enum class DisplayJointSampleUse : std::uint8_t {
  /// Not a pose (e.g. a vendor's e-stop signature): the display keeps the
  /// last valid pose.
  kIgnore,
  /// The display only.
  kDisplay,
  /// Also the `measured_joints` stream's liveness and the planning previews'
  /// start, while the joint state broadcaster is off (e.g. the re-arm after
  /// a TCP offset send).
  kLeads,
};

/// CONTRACTS §6 display joint source: a vendor driver's own joint topic,
/// which the arm's live view follows in every mode (also while the joint
/// state broadcaster is off), and the family's check of one sample (in the
/// arm's joint order).
struct DisplayJointSource final {
  std::string topic;
  std::function<DisplayJointSampleUse(std::int64_t now_ns, const std::vector<double> &positions)>
      use;
};

/// What one factory call returns: the component's adapter; for an owner, the
/// opaque handle the host passes to its members' factory calls (CONTRACTS
/// §16); the tool-context hook (CONTRACTS §6: TCP offset and payload, e.g.
/// xArm `ApplyToolContext`) and the display joint source of an arm, which the
/// host sets on its ros2_control connection.
struct ComponentAdapters final {
  std::shared_ptr<core::IComponentAdapter> component;
  std::shared_ptr<void> owner_handle;
  std::optional<ToolContextHook> tool_context;
  std::optional<DisplayJointSource> display_joints;
};

/// One adapter family as its library exports it (CONTRACTS §6 "What a family
/// exports"): the family id (the model row's `adapter`), the factory of one
/// component's adapter (called once per component: owners before their
/// members) and, for a family that runs on a ros2_control lane,
/// its launch contribution (CONTRACTS §12; null otherwise). A lane family
/// with its own ros2_control execution configuration (Mock, Isaac) also
/// exports `lane` (`ros2_control_lane_family.hpp`); without it a ros2_control
/// component runs the generic configuration of a driver that reports its own
/// measured state (a vendor's Real driver). Each throws
/// `std::invalid_argument` for adapter data it can't use. A vendor family
/// also exports `controllers`: its controllers template for a model row (the
/// vendor base plus ManyForge's additions, command interfaces as
/// `[__command_interfaces__]`), which a lane row naming `vendor: <family>`
/// runs on its lane's hardware (Mock and Isaac rows of a vendor's model).
struct ComponentAdapterFamily final {
  std::string_view family;
  ComponentAdapters (*make)(const ComponentAdapterContext &context);
  LaunchContribution (*launch_contribution)(const ModelLaneRow &row, bool arm);
  const Ros2ControlLaneHooks *lane{};
  std::string (*controllers)(const ModelLaneRow &row){};
};

/// The symbol every adapter library exports.
inline constexpr std::string_view kComponentAdapterFamilySymbol =
    "manyforge_component_adapter_family";
using ComponentAdapterFamilyEntry = const ComponentAdapterFamily *(*)();

/// Adapters register through the ament resource index (CONTRACTS §6): one
/// resource per family under this type, named after the family, whose content
/// is the adapter library's file name under the package prefix's `lib/`.
inline constexpr std::string_view kComponentAdapterResourceType =
    "manyforge_component_adapters";

/// The adapter families of this installation. The core keeps no list of
/// vendors or lanes; the host loads whatever the index registers.
class ComponentAdapterRegistry final {
 public:
  /// Loads every family the ament resource index registers. A library that
  /// can't be loaded, or whose exported family id differs from its resource
  /// name, refuses with `component_adapter_load_failed`.
  [[nodiscard]] static core::Result<ComponentAdapterRegistry> LoadInstalled();

  /// A registry of the given families (tests and in-process families).
  explicit ComponentAdapterRegistry(std::vector<ComponentAdapterFamily> families);

  ComponentAdapterRegistry(ComponentAdapterRegistry &&) noexcept;
  ComponentAdapterRegistry &operator=(ComponentAdapterRegistry &&) noexcept;
  ~ComponentAdapterRegistry();

  /// The adapters of one component. `component_adapter_unknown` when no
  /// family is registered under `context.row.adapter`;
  /// `component_adapter_invalid` when the family refuses the context.
  [[nodiscard]] core::Result<ComponentAdapters> Make(const ComponentAdapterContext &context) const;

  /// The family's launch contribution for one model row; a row naming
  /// `vendor` gets that family's `controllers` first
  /// (`ModelLaneRow::vendor_controllers_yaml`). Throws
  /// `ControllerLaunchArtifactError` when no installed family contributes
  /// one, or the family refuses the row.
  [[nodiscard]] LaunchContribution LaunchContributionOf(const ModelLaneRow &row, bool arm) const;

  /// The family registered under `adapter`, or null.
  [[nodiscard]] const ComponentAdapterFamily *Find(std::string_view adapter) const;

  [[nodiscard]] std::vector<std::string> families() const;

 private:
  struct Library;
  std::map<std::string, ComponentAdapterFamily, std::less<>> families_;
  /// Loaded libraries stay open for the registry's lifetime.
  std::vector<std::shared_ptr<Library>> libraries_;
};

} // namespace manyforge::roboplan_backend
