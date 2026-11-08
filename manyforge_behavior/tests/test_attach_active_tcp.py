"""The Scene adapter resolves Active TCP before attempting an attach."""

from manyforge_behavior.orchestration_ports import OwnerNodeInputs
from manyforge_headless_api.native import HeadlessApiError

from .test_production_ports import (
    _SceneTransport, _authored_scene, _fixed_scene_object, _identity,
    _scene_context, _scene_port,
)
from .v3_fixtures import frame, target


def _setup():
    transport = _SceneTransport()
    transport.objects["graspable"] = _fixed_scene_object("graspable")
    port = _scene_port(transport)
    port.prepare_initial_scene(_scene_context(), [_authored_scene()[0]])
    transport.submissions.clear()
    return port, transport


def _inputs():
    return OwnerNodeInputs(parameters={
        "scene_resource_id": "graspable", "target": target("robot.1", "arm.command"),
        "carrier_frame": {"kind": "active_tcp"},
        "touch_bodies": [{"component_id": "robot.1", "collision_body_id": "tool_collision"}],
    })


def test_active_tcp_resolves_holder_frame_before_scene_commit():
    port, transport = _setup()
    calls = []
    port._resolve_attach_active_tcp = lambda binding, target_ref: (
        calls.append((binding, dict(target_ref))) or frame("robot.1", "tcp"))
    result = port.invoke("scene.attach_carried_object", _identity("attach"), _inputs())
    assert result.succeeded
    assert calls == [(_scene_context().program_binding_id, target("robot.1", "arm.command"))]
    assert transport.submissions[0]["operation"]["frame_role_id"] == "tcp"


def test_active_tcp_wrong_holder_fails_before_scene_commit():
    port, transport = _setup()
    def refuse(_binding, _target_ref):
        raise HeadlessApiError("attach_active_tcp_not_holder", "wrong holder")
    port._resolve_attach_active_tcp = refuse
    result = port.invoke("scene.attach_carried_object", _identity("attach"), _inputs())
    assert not result.succeeded
    assert result.reason_code == "attach_active_tcp_not_holder"
    assert transport.submissions == []


def test_program_requires_exactly_one_carrier_selector():
    import pytest
    from manyforge_behavior.program import Program, ProgramValidationError
    from .test_grasp_lost_response import _attach, _catalog, _program
    from .test_program_cutover import _identified

    catalog = _catalog()
    document = _program().to_dict()
    attach = _attach(document)
    carrier_frame_ref = attach["parameters"].pop("carrier_frame_ref")
    attach["parameters"]["carrier_frame"] = {"kind": "active_tcp"}
    Program.from_dict(_identified(document), catalog)
    attach["parameters"]["carrier_frame_ref"] = carrier_frame_ref
    with pytest.raises(ProgramValidationError, match="exactly one carrier"):
        Program.from_dict(_identified(document), catalog)
    del attach["parameters"]["carrier_frame"]
    del attach["parameters"]["carrier_frame_ref"]
    with pytest.raises(ProgramValidationError, match="exactly one carrier"):
        Program.from_dict(_identified(document), catalog)
