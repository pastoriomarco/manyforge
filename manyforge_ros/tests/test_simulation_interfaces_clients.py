"""In-container ROS service transport test for the selected Isaac clients."""

from __future__ import annotations

import threading
import time

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

from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerNodeInputs,
    OwnerTaskState,
    ProgramRunPortContext,
)
from manyforge_ros.program_simulation_port import (
    RosSimulationProgramTaskPort,
    SimulationProgramSiteConfig,
)
from manyforge_ros.scene_mirror_probe import SimulationSceneMirrorClient
from manyforge_ros.simulation_control import SimulationControlClient


_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_DIGEST_C = "sha256:" + "c" * 64


def test_control_and_scene_clients_share_the_typed_simulator_owner() -> None:
    rclpy.init()
    server: Node | None = None
    client_node: Node | None = None
    executor = SingleThreadedExecutor()
    worker: threading.Thread | None = None
    program_port: RosSimulationProgramTaskPort | None = None
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
        program_port = RosSimulationProgramTaskPort(
            SimulationProgramSiteConfig(
                service_prefix="auto",
                service_timeout_s=2.0,
            ),
        )
        program_port.bind(_program_context())
        identity = NodeAttemptIdentity(
            program_run_id="run.simulation.transport",
            node_instance_id="move_soup_can",
            node_attempt_id="run.simulation.transport/move_soup_can/1",
        )
        started = program_port.start(
            "simulation.set_entity_pose",
            identity,
            OwnerNodeInputs(
                parameters={
                    "scene_resource_id": "soup_can",
                    "pose": {
                        "frame_ref": {"kind": "cell_frame"},
                        "position_m": [0.25, -0.5, 0.75],
                        "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
                    },
                }
            ),
        )
        deadline = time.monotonic() + 2.0
        terminal = program_port.observe(started.task_id)
        while terminal.state is OwnerTaskState.ACTIVE and time.monotonic() < deadline:
            time.sleep(0.005)
            terminal = program_port.observe(started.task_id)
        assert terminal.state is OwnerTaskState.SUCCEEDED
        program_port.acknowledge(started.task_id, identity)

        mirror = SimulationSceneMirrorClient(
            client_node,
            service_timeout_s=2.0,
        )
        try:
            snapshot = mirror.capture(entity_names=["/World/soup_can"])
        finally:
            mirror.close()
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
        if program_port is not None:
            program_port.close()
        if client_node is not None:
            client_node.destroy_node()
        executor.shutdown()
        if worker is not None:
            worker.join(timeout=2.0)
        if server is not None:
            server.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


def _program_context() -> ProgramRunPortContext:
    return ProgramRunPortContext(
        program_run_id="run.simulation.transport",
        applied_record_digest="sha256:" + "9" * 64,
        scenario_id="isaac-cell",
        source_document_digest=_DIGEST_A,
        program_id="simulation_transport",
        program_revision=_DIGEST_B,
        program_binding_id=_DIGEST_C,
        retained_bundle_id=_DIGEST_A,
        graph_revision=_DIGEST_B,
        node_environment_digest=_DIGEST_C,
        program={
            "program_id": "simulation_transport",
            "program_revision": _DIGEST_B,
            "scene": [{"scene_resource_id": "soup_can"}],
        },
        simulation_scene={
            "cell_frame_id": "world",
            "scene_entities": [
                {"entity": "/World/soup_can", "scene_resource_id": "soup_can"}
            ],
        },
        program_binding={
            "program_binding_id": _DIGEST_C,
            "program_id": "simulation_transport",
            "program_revision": _DIGEST_B,
            "retained_bundle_id": _DIGEST_A,
            "graph_revision": _DIGEST_B,
            "target_role_bindings": [],
        },
    )


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
