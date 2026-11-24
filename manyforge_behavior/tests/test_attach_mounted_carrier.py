"""OD-372: an arm target attaches to a frame of a component mounted on it."""

from dataclasses import replace

from manyforge_behavior.orchestration_ports import OwnerNodeInputs

from .test_production_ports import (
    _DIGEST_E, _SceneTransport, _authored_scene, _fixed_scene_object, _identity,
    _scene_context, _scene_port,
)
from .v3_fixtures import frame, target

_ARM = target("robot.1", "arm.command")


def _body(component_id: str, body_id: str) -> dict[str, str]:
    return {"component_id": component_id, "collision_body_id": body_id}


def _mounted_tool_context():
    """The scene context plus a tool's frames and bodies as plain Program
    references (the Program Binding holds them so; Apply checked the mount)."""

    value = _scene_context()
    binding = dict(value.program_binding)
    binding["component_bindings"] = [*binding["component_bindings"], {
        "component_id": "tool.1",
        "model_bindings": [{
            "model_role_id": "model", "model_instance_id": "tool.1.model",
            "model_revision": _DIGEST_E,
        }],
    }]
    binding["frame_bindings"] = [*binding["frame_bindings"], *(
        {"reference": frame("tool.1", name), "model_instance_id": "tool.1.model",
         "model_revision": _DIGEST_E}
        for name in ("holder_rod", "tcp")
    )]
    binding["collision_body_bindings"] = [*binding["collision_body_bindings"], {
        "reference": _body("tool.1", "holder_rod"),
        "model_instance_id": "tool.1.model", "model_revision": _DIGEST_E,
    }]
    return replace(value, program_binding=binding)


def _setup():
    transport = _SceneTransport()
    transport.objects["graspable"] = _fixed_scene_object("graspable")
    port = _scene_port(transport)
    port.prepare_initial_scene(_mounted_tool_context(), [_authored_scene()[0]])
    transport.submissions.clear()
    return port, transport


def _attach(port, **parameters):
    return port.invoke("scene.attach_carried_object", _identity("attach"), OwnerNodeInputs(
        parameters={"scene_resource_id": "graspable", "target": _ARM, **parameters}))


def test_arm_target_attaches_to_a_mounted_tool_frame():
    port, transport = _setup()
    result = _attach(port, carrier_frame_ref=frame("tool.1", "holder_rod"),
                     touch_bodies=[_body("tool.1", "holder_rod")])
    assert result.succeeded, result.message
    operation = transport.submissions[0]["operation"]
    assert operation["controllable_component_id"] == "robot.1"
    assert operation["command_target_id"] == "arm.command"
    assert operation["carrier_component_id"] == "tool.1"
    assert operation["frame_role_id"] == "holder_rod"
    assert operation["touch_body_roles"] == ["holder_rod"]


def test_own_frame_sends_no_carrier_component():
    port, transport = _setup()
    result = _attach(port, carrier_frame_ref=frame("robot.1", "tcp"),
                     touch_bodies=[_body("robot.1", "tool_collision")])
    assert result.succeeded, result.message
    assert "carrier_component_id" not in transport.submissions[0]["operation"]


def test_frame_absent_from_the_binding_is_refused():
    # A component's frame the Program Binding does not hold (a frame of a
    # component not mounted on the target never passes Apply validation).
    port, transport = _setup()
    result = _attach(port, carrier_frame_ref=frame("other.1", "tcp"),
                     touch_bodies=[_body("other.1", "tool_collision")])
    assert not result.succeeded
    assert "no exact carrier frame" in result.message
    assert transport.submissions == []


def test_touch_bodies_belong_to_the_carrier_frame_component():
    port, transport = _setup()
    for frame_ref, bodies in (
        (frame("tool.1", "holder_rod"), [_body("robot.1", "tool_collision")]),
        (frame("robot.1", "tcp"), [_body("tool.1", "holder_rod")]),
    ):
        result = _attach(port, carrier_frame_ref=frame_ref, touch_bodies=bodies)
        assert not result.succeeded
        assert "touch body belongs to another component" in result.message
    assert transport.submissions == []


def test_active_tcp_on_the_arm_selects_the_mounted_tool_tcp():
    port, transport = _setup()
    port._resolve_attach_active_tcp = lambda _binding, _target: frame("tool.1", "tcp")
    result = _attach(port, carrier_frame={"kind": "active_tcp"},
                     touch_bodies=[_body("tool.1", "holder_rod")])
    assert result.succeeded, result.message
    operation = transport.submissions[0]["operation"]
    assert operation["controllable_component_id"] == "robot.1"
    assert operation["carrier_component_id"] == "tool.1"
    assert operation["frame_role_id"] == "tcp"
