// Copyright 2026 Flexin Group SRL
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/io_source_ingress_sender.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/io_signal.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace manyforge::roboplan_backend {

/// Exact already-compiled endpoint and current observation/query owners used
/// by one Deployment-selected geometric-contact source. The endpoint owns its
/// contact-body list; this adapter never accepts another body selector list.
struct GeometricContactInputBinding final {
  core::InterruptSourceId source_id;
  core::IoSourceIncarnation incarnation;
  std::vector<core::IoSignalDescriptorDTO> signals;
  core::RuntimeMotionEndpointBindingDTO endpoint;
  core::SceneResourceId target_scene_resource_id;
  /// Current reported coordinate authorities needed by the selected endpoint.
  /// Integrated model-backed grippers have their own publication and are
  /// sampled alongside motion so unspecified joints are never reset neutral.
  std::vector<std::shared_ptr<const core::ObservedRobotStatePublication>>
      observed_states;
  std::shared_ptr<core::ICollisionWorldSnapshotPublisher> collision_publisher;
  std::shared_ptr<core::ICollisionQueryEngine> collision_engine;
  std::shared_ptr<core::IScheduler> scheduler;
  core::Duration sample_period_ns{};
  core::Duration query_timeout_ns{};
  std::optional<core::Duration> dynamic_body_max_age_cap_ns;
};

struct GeometricContactInputBaseline final {
  core::IoSourceIncarnation incarnation;
  std::uint64_t source_sequence{};
};

/// In-process boolean input adapter over the authoritative collision query
/// surface. Query preparation and scene-revision changes occur on the
/// Monitoring lane; the engine hot call consumes only immutable/preallocated
/// views. Missing or stale observations are published as unavailable.
class GeometricContactInputProvider final {
public:
  static core::Result<std::unique_ptr<GeometricContactInputProvider>> Create(
      std::shared_ptr<core::IIoSignalIngress> ingress, core::IClock &clock,
      core::IScheduler &scheduler, GeometricContactInputBinding binding,
      std::optional<std::uint64_t> prepared_driver_generation = std::nullopt);
  ~GeometricContactInputProvider();
  GeometricContactInputProvider(const GeometricContactInputProvider &) = delete;
  GeometricContactInputProvider &
  operator=(const GeometricContactInputProvider &) = delete;

  bool Pause(std::uint64_t driver_observation_generation);
  /// Publishes one immediate current acquisition before returning, then starts
  /// periodic observation. A conclusive result may therefore arm an inactive
  /// Probe without waiting for motion or a later timer tick.
  core::Result<GeometricContactInputBaseline>
  Resume(std::uint64_t driver_observation_generation);
  bool AbortResume(std::uint64_t driver_observation_generation);
  core::Result<void> RebindIngress(
      std::shared_ptr<core::IIoSignalIngress> ingress,
      GeometricContactInputBinding binding,
      std::uint64_t driver_observation_generation);
  void Shutdown();

private:
  struct State;
  explicit GeometricContactInputProvider(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

} // namespace manyforge::roboplan_backend
