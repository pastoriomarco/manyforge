"""In-container ROS service transport test for the selected Isaac clients."""

from __future__ import annotations

import threading

from geometry_msgs.msg import Vector3
import rclpy
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node
from simulation_interfaces.msg import (
    Bounds,
    EntityCategory,
    EntityState,
    SimulationState,
    SimulatorFeatures,
)
from simulation_interfaces.srv import (
    GetEntities,
    GetEntityBounds,
    GetEntityInfo,
    GetEntityState,
    GetSimulationState,
    GetSimulatorFeatures,
    ResetSimulation,
    SetEntityState,
    SetSimulationState,
    StepSimulation,
)

from manyforge_ros.scene_mirror_probe import SimulationSceneMirrorClient
from manyforge_ros.simulation_control import SimulationControlClient


def test_control_and_scene_clients_share_the_typed_simulator_owner() -> None:
    rclpy.init()
    server: Node | None = None
    client_node: Node | None = None
    executor = SingleThreadedExecutor()
    worker: threading.Thread | None = None
    state = {
        "simulation_state": int(SimulationState.STATE_STOPPED),
        "entity_state": EntityState(),
        "steps": 0,
    }
    state["entity_state"].header.frame_id = "world"
    state["entity_state"].pose.orientation.w = 1.0
    try:
        server = Node("manyforge_fake_selected_simulator_owner")
        _install_services(server, state)
        executor.add_node(server)
        worker = threading.Thread(target=executor.spin, daemon=True)
        worker.start()
        client_node = Node("manyforge_selected_simulator_client_test")

        control = SimulationControlClient(client_node, service_timeout_s=2.0)
        assert control.reset().success
        assert control.get_simulation_state().state == "stopped"
        assert control.set_simulation_state(state="playing").success
        assert control.get_simulation_state().state == "playing"
        assert control.set_simulation_state(state="stopped").success
        assert control.step(steps=2).success
        assert control.set_entity_state(
            entity="/World/soup_can",
            pose={
                "frame_id": "world",
                "position_m": [0.25, -0.5, 0.75],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
        ).success

        snapshot = SimulationSceneMirrorClient(
            client_node,
            service_timeout_s=2.0,
        ).capture(entity_names=["/World/soup_can"])
        assert snapshot.errors == ()
        assert len(snapshot.entities) == 1
        entity = snapshot.entities[0]
        assert entity.name == "/World/soup_can"
        assert entity.category_name == "CATEGORY_OBJECT"
        assert entity.pose is not None
        assert entity.pose.position_xyz == (0.25, -0.5, 0.75)
        assert entity.bounds is not None
        assert entity.bounds.type_name == "TYPE_BOX"
        assert state["steps"] == 2
    finally:
        if client_node is not None:
            client_node.destroy_node()
        executor.shutdown()
        if worker is not None:
            worker.join(timeout=2.0)
        if server is not None:
            server.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


def _install_services(node: Node, state: dict[str, object]) -> None:
    def ok(response):
        response.result.result = response.result.RESULT_OK
        return response

    def reset(_request, response):
        state["simulation_state"] = int(SimulationState.STATE_STOPPED)
        return ok(response)

    def get_state(_request, response):
        response.state.state = state["simulation_state"]
        return ok(response)

    def set_state(request, response):
        state["simulation_state"] = int(request.state.state)
        return ok(response)

    def step(request, response):
        state["steps"] = int(request.steps)
        return ok(response)

    def set_entity(request, response):
        assert request.entity == "/World/soup_can"
        state["entity_state"] = request.state
        return ok(response)

    def features(_request, response):
        response.features.features = [
            int(SimulatorFeatures.ENTITY_INFO_GETTING),
            int(SimulatorFeatures.ENTITY_STATE_GETTING),
            int(SimulatorFeatures.ENTITY_BOUNDS),
        ]
        response.features.spawn_formats = ["usd"]
        response.features.custom_info = "typed fake owner"
        return response

    def entities(_request, response):
        response.entities = ["/World/soup_can"]
        return ok(response)

    def info(_request, response):
        response.info.category.category = int(EntityCategory.CATEGORY_OBJECT)
        response.info.description = "retained soup can"
        response.info.tags = ["pickable"]
        return ok(response)

    def entity_state(_request, response):
        response.state = state["entity_state"]
        return ok(response)

    def bounds(_request, response):
        response.bounds.type = int(Bounds.TYPE_BOX)
        response.bounds.points = [
            Vector3(x=-0.05, y=-0.05, z=-0.05),
            Vector3(x=0.05, y=0.05, z=0.05),
        ]
        return ok(response)

    for service_type, name, callback in (
        (ResetSimulation, "/reset_simulation", reset),
        (GetSimulationState, "/get_simulation_state", get_state),
        (SetSimulationState, "/set_simulation_state", set_state),
        (StepSimulation, "/step_simulation", step),
        (SetEntityState, "/set_entity_state", set_entity),
        (GetSimulatorFeatures, "/get_simulator_features", features),
        (GetEntities, "/get_entities", entities),
        (GetEntityInfo, "/get_entity_info", info),
        (GetEntityState, "/get_entity_state", entity_state),
        (GetEntityBounds, "/get_entity_bounds", bounds),
    ):
        node.create_service(service_type, name, callback)
