#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/planner_settings.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::deployment {

class ComponentSpecError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// The machine facts of this site (its site settings), resolved once and
/// passed to every spec: device addresses keyed by their portable reference.
/// Empty on a machine without site settings.
struct SiteFacts final {
  std::map<std::string, std::string, std::less<>> device_addresses;
  /// Whether this machine has site settings at all (an unresolved address
  /// then names an unknown reference rather than missing settings).
  bool configured{false};

  friend bool operator==(const SiteFacts &, const SiteFacts &) = default;
};

/// The top-level sections of a ComponentSpec, sorted. A replacement reason is
/// exactly one of these names.
inline constexpr std::string_view kComponentSpecAdapter = "adapter";
inline constexpr std::string_view kComponentSpecAddress = "address";
inline constexpr std::string_view kComponentSpecBindings = "bindings";
inline constexpr std::string_view kComponentSpecControllers = "controllers";
inline constexpr std::string_view kComponentSpecDescription = "description";
inline constexpr std::string_view kComponentSpecLaunch = "launch";
inline constexpr std::string_view kComponentSpecOwner = "owner";
inline constexpr std::string_view kComponentSpecProviders = "providers";
inline constexpr std::array<std::string_view, 8> kComponentSpecSections{
    kComponentSpecAdapter,     kComponentSpecAddress, kComponentSpecBindings,
    kComponentSpecControllers, kComponentSpecDescription,
    kComponentSpecLaunch,      kComponentSpecOwner,   kComponentSpecProviders};

/// One mount on a component (its description): the mounted child, its
/// models (custom TCPs only when it runs no process of its own) and whether
/// it runs none (its parent's launch then publishes its fixed links).
struct ComponentSpecMount final {
  core::ControllableComponentId child;
  std::vector<core::ModelInstanceDTO> child_models;
  bool child_runs_no_process{};

  friend bool operator==(const ComponentSpecMount &, const ComponentSpecMount &) = default;
};

/// What a component's launched description is built from: its own model
/// instances in model-role order (custom TCPs, no placement) and the mounts
/// on it. The spec's typed description facts; the Deployment compiler builds
/// the controller profile's `description` from the same facts.
struct ComponentDescriptionFacts final {
  std::vector<core::ModelInstanceDTO> models;
  std::vector<ComponentSpecMount> mounts;

  friend bool operator==(const ComponentDescriptionFacts &,
                         const ComponentDescriptionFacts &) = default;
};

/// The description facts of component `id` in a cell graph: `typed` and its
/// canonical value `graph` (whose execution-host rows say which mounted
/// children run no process of their own). Throws ComponentSpecError when a
/// model instance is absent.
[[nodiscard]] ComponentDescriptionFacts
component_description_facts(const core::CellGraphDTO &typed, const CanonicalValue &graph,
                            const core::ControllableComponentId &id);

/// What one component is launched and constructed with (build spec plan,
/// tier 1). A change to any of it restarts the component; nothing else does.
/// The execution host builds a component from this value only (FP-2): the
/// typed members below are decoded once from the sections, so every fact the
/// host reads is in the fingerprint.
///
/// - description: its cell, its component row (type, revision, model roles,
///   runtime units; no placement, no default TCP), its own model instances
///   (asset, revision, custom TCPs; no placement), the mounts on it (pose,
///   interfaces, primary; no collision exclusions) with each mounted child's
///   model, whether the child runs no process of its own (and then its custom
///   TCPs); a machine owner also its arm member's description (CONTRACTS §16).
/// - controllers: its control domains, desired control configurations and
///   compiled domain products.
/// - launch: its runtime-unit rows and products, runtime-contract
///   requirements and its rows of the execution host's configuration
///   (controller profile, machine row, motion limits, fault Stop profiles,
///   gripper and I/O signal rows, initial joint positions); a component whose
///   motion kernel the host builds also the host scalars (execution tick,
///   collision monitor period and budgets, collision workers): they are bound
///   at kernel construction; a component with I/O signals on one ROS host
///   also whether that host is simulated (its simulated PLC); a machine
///   owner also its arm member's sections other than description and owner
///   (CONTRACTS §16: the owner's process is launched with its arm, so the
///   host rebuilds the owner whenever it rebuilds that arm).
/// - adapter: its adapter configuration rows (lifecycle timeout, streams,
///   I/O devices, pose-source, AI-model and sensor settings) and, on a
///   motion component, its own `measured_joints` stream override
///   (construction_stream_overrides).
/// - bindings: its control bindings.
/// - providers: the capability providers its component, bindings and domains
///   cite (a cell-global provider only when one of its bindings cites it).
/// - address: each device address reference it uses with the resolved site
///   address (null when the site names none); null without a reference.
/// - owner: a machine member's owner's spec fingerprint; null otherwise.
struct ComponentSpec final {
  core::ControllableComponentId controllable_component_id;
  CanonicalValue description{nullptr};
  CanonicalValue controllers{nullptr};
  CanonicalValue launch{nullptr};
  CanonicalValue adapter{nullptr};
  CanonicalValue bindings{nullptr};
  CanonicalValue providers{nullptr};
  CanonicalValue address{nullptr};
  CanonicalValue owner{nullptr};
  /// The control domains it owns, sorted (also inside `controllers`).
  std::vector<core::ControlDomainId> control_domains{};

  // -- Typed facts, decoded once from the sections above --------------------
  /// Its cell (description).
  core::CellId cell_id{std::string()};
  /// Its own model instances in model-role order, without placement
  /// (description).
  std::vector<core::ModelInstanceDTO> models{};
  /// The mounts on it (description).
  std::vector<ComponentSpecMount> mounts{};
  /// The execution host that runs it and its rows of that host's
  /// configuration (launch, adapter): only its own rows; the kernel scalars
  /// only on a motion component; never the planner settings (CellContext).
  std::optional<core::RuntimeUnitId> execution_host{};
  ros2_control_execution_runtime_contract::Configuration host_configuration{};
  /// Its own stream budget overrides it is constructed with
  /// (construction_stream_overrides, adapter).
  std::vector<core::StreamParameterOverrideDTO> stream_overrides{};
  /// Its compiled lifecycle command time (adapter), else the default.
  std::int64_t lifecycle_command_timeout_ns{};
  /// The control domains it provides (a generic I/O provider's; controllers).
  std::vector<core::ControlDomainDTO> provider_domains{};
  /// Its I/O device row and I/O signal rows (adapter, launch).
  std::optional<RetainedIoProviderDeviceConfiguration> io_device{};
  std::vector<RetainedIoSignalConfiguration> io_signals{};
  /// Whether the one ROS host its I/O signals run on is simulated (no
  /// component it hosts has measured state authority); absent without I/O
  /// signals or when they span hosts (launch).
  std::optional<bool> io_host_simulated{};
  /// Each device address reference it uses, with the resolved site address
  /// (absent when the site names none), and whether the site has settings
  /// (address).
  std::map<std::string, std::optional<std::string>, std::less<>> device_addresses{};
  bool site_configured{false};

  /// One section by its name (one of kComponentSpecSections).
  [[nodiscard]] const CanonicalValue &section(std::string_view name) const;
  /// The canonical value the fingerprint hashes.
  [[nodiscard]] CanonicalValue value() const;
};

/// The facts of one cell refreshed on every Apply without restarting anything
/// (build spec plan, tier 2).
struct CellContext final {
  /// Retained policy records and the per-stream budget overrides.
  CanonicalValue policies;
  /// Moving-takeover preparation and planning budgets.
  CanonicalValue takeover_budgets;
  /// Planning/viewer models, symbol maps, every model occurrence, root poses
  /// and the attachment rows (with their collision exclusions).
  CanonicalValue scene;
  CanonicalValue pose_sources;
  CanonicalValue default_tcps;
  /// Global interrupt rules.
  CanonicalValue rules;
  /// Planner settings per execution host: seed, collision-check step,
  /// collision clearance, the fresh-plan pre-dispatch check, search,
  /// optimize, n, node budget, Cartesian timing, motion timing.
  CanonicalValue planner;
  /// Assistant projection, external resources and the site overlay ref.
  CanonicalValue assistant;

  [[nodiscard]] CanonicalValue value() const;
};

/// One execution host's planner settings, read from the cell context (the
/// Deployment's defaults where it names none). A retained component takes
/// them at Apply; a new one is built with them.
struct PlannerSettings final {
  std::uint32_t rng_seed{};
  double collision_check_step_rad{core::kDefaultCollisionCheckStepRad};
  double collision_clearance_m{core::kDefaultCollisionClearanceM};
  bool collision_check_fresh_plans{core::kDefaultCollisionCheckFreshPlans};
  core::PlannerSearchDTO search{core::kDefaultPlannerSearch};
  core::PlannerOptimizeDTO optimize{core::kDefaultPlannerOptimize};
  std::uint32_t n{core::kDefaultPlannerBestOfN};
  std::uint32_t max_nodes{core::kDefaultPlannerMaxNodes};
  core::CartesianSpeedModeDTO cartesian_timing{core::kDefaultCartesianTiming};
  core::MotionTimingDTO motion_timing{core::kDefaultMotionTiming};

  friend bool operator==(const PlannerSettings &, const PlannerSettings &) = default;
};

/// The planner settings of `runtime_unit_id` in `context`. Throws
/// ComponentSpecError when the context has no such execution host.
[[nodiscard]] PlannerSettings planner_settings(const CellContext &context,
                                               const core::RuntimeUnitId &runtime_unit_id);

/// The spec of one component of a sealed, verified bundle. Pure and
/// deterministic. Throws ComponentSpecError when the bundle has no such
/// component.
[[nodiscard]] ComponentSpec
component_spec(const RetainedBundle &bundle, const SiteFacts &site_facts,
               const core::ControllableComponentId &controllable_component_id);

/// A component's own `policies.component_streams` rows it is constructed
/// with, which its spec `adapter` section holds (so an edit replaces it):
/// a motion component's `measured_joints` row, its in-motion staleness
/// budget. Every other override is re-read on retain and stays CellContext
/// only. Canonical array, empty when there is none.
[[nodiscard]] CanonicalValue
construction_stream_overrides(const RetainedBundle &bundle,
                              const core::ControllableComponentId &id);

/// The cell context of a sealed, verified bundle. Pure and deterministic.
[[nodiscard]] CellContext cell_context(const RetainedBundle &bundle);

/// Whether a field of the execution host's configuration has a tier (spec
/// launch, spec adapter or CellContext planner). Every field must; a test
/// over every example checks it, so a new field forces the choice.
[[nodiscard]] bool execution_host_field_has_tier(std::string_view name) noexcept;

/// RFC 8785 canonical JSON digest of the spec: the component's fingerprint.
[[nodiscard]] core::ContentDigest
component_spec_fingerprint(const ComponentSpec &spec);

} // namespace manyforge::deployment
