"""V13.7.5: Headless joins the runner's node request to the manual preview path."""

from __future__ import annotations

import copy
from typing import Any

from fastapi.testclient import TestClient
import pytest

from manyforge_headless_api.app import create_app
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.program_runner_client import ProgramRunnerClientError
from manyforge_headless_api.models import ProgramNodeCorrectionPreviewRequest
from manyforge_headless_api.service import TrustedAttribution
from manyforge_headless_api.tests.test_app import (
    FakeCore,
    FakeExecution,
    FakeManager,
    FakeTrustedAdapter,
    _service,
)


_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_DIGEST_C = "sha256:" + "c" * 64
_RUN = "program-run.7"


def _body(**changes: Any) -> dict[str, Any]:
    return {
        "schema_version": "manyforge.program_node_correction_preview_request.v1",
        "request_id": "node-preview.1",
        "program_run_id": _RUN,
        "expected_owner_epoch": "program-runner.epoch.1",
        "program_id": "lite6_program",
        "program_revision": _DIGEST_A,
        "program_binding_id": _DIGEST_A,
        "node_instance_id": "move_to_pick",
        "node_attempt_id": "attempt:1",
        "proposed_inputs": [{"path": "goal", "value": [{"joint_name": "j1", "position_rad": 0.1}]}],
        **changes,
    }


def _operation() -> dict[str, Any]:
    return {
        "request_context": {
            "request_id": "node-preview.1",
            "request_owner_epoch": "kernel.epoch.1",
            "source_label": "manyforge_behavior",
            "correlation_id": _RUN,
        },
        "program_id": "lite6_program",
        "program_revision": _DIGEST_A,
        "program_binding_id": _DIGEST_A,
        "program_run_id": _RUN,
        "expected_graph_revision": _DIGEST_C,
        "expected_retained_bundle_id": _DIGEST_B,
        "expected_scene_revision": {
            "scene_id": _DIGEST_A,
            "revision_id": 3,
            "content_hash": _DIGEST_B,
        },
        "participants": [
            {
                "target": {"component_id": "robot.1", "command_target_id": "arm.command"},
                "required_capability_id": "execution.trajectory.execute",
                "action_span": None,
                "payload": {
                    "kind": "motion",
                    "move_id": "preview-move:1",
                    "base_frame": {"component_id": "robot.1", "frame_id": "base"},
                    "tcp_endpoint": {"kind": "entry", "target": {"component_id": "robot.1", "command_target_id": "arm.command"}, "frame": {"component_id": "tool.1", "frame_id": "tcp"}},
                    "tolerances": {
                        "joint_position_tol_rad": 0.01,
                        "translation_tol_m": 0.001,
                        "rotation_tol_rad": 0.01,
                    },
                    "speed_profile": {
                        "velocity_scale": 0.5,
                        "acceleration_scale": 0.5,
                        "max_tcp_linear_speed_mps": None,
                    },
                    "timeouts": {"planning_timeout_s": 2.0, "execution_timeout_s": 10.0},
                    "cache_mode": "default",
                    "target_mode": "absolute",
                    "target_reference": None,
                    "offset_reference": None,
                    "motion": {
                        "kind": "joint_goal",
                        "joint_names": ["j1"],
                        "positions_rad": [0.1],
                    },
                    "contact_policy_id": None,
                    "planner": None,
                    "per_joint_dynamics_limits": {},
                    "motion_interrupt_scopes": [],
                    "runtime_object_bindings": [],
                },
            }
        ],
        "coordination_policy": "independent",
    }


def _built(*, unavailable: dict[str, str] | None = None) -> dict[str, Any]:
    return {
        "schema_version": "manyforge.program_node_correction_preview_intent.v2",
        "request_id": "node-preview.1",
        "program_run_id": _RUN,
        "program_runner_owner_epoch": "program-runner.epoch.1",
        "run_revision": 4,
        "program_id": "lite6_program",
        "program_revision": _DIGEST_A,
        "program_binding_id": _DIGEST_A,
        "retained_bundle_id": _DIGEST_B,
        "graph_revision": _DIGEST_C,
        "node_instance_id": "move_to_pick",
        "node_attempt_id": "attempt:1",
        "node_id": "motion.move_to_joint_target",
        "proposed_paths": ["goal"],
        "parameters_digest": "sha256:" + "f" * 64,
        "target": None if unavailable is not None else {
            "component_id": "robot.1",
            "command_target_id": "arm.command",
        },
        "unavailable_reason": unavailable,
        "operation": None if unavailable is not None else _operation(),
    }


class _Runner:
    def __init__(self, response: dict[str, Any] | Exception) -> None:
        self.response = response
        self.calls: list[tuple[str, dict[str, Any]]] = []

    def preview_node_correction(self, program_run_id: str, body: dict[str, Any]):
        self.calls.append((program_run_id, copy.deepcopy(body)))
        if isinstance(self.response, Exception):
            raise self.response
        return copy.deepcopy(self.response)


def test_preview_sends_the_runner_intent_through_the_manual_preview_path() -> None:
    runner = _Runner(_built())
    execution = FakeExecution()
    app = create_app(
        service=_service(FakeCore(), FakeManager(), execution, program_runner=runner),
        trusted_principal_adapter=FakeTrustedAdapter(),
    )

    with TestClient(app) as client:
        response = client.post(
            f"/api/program/runs/{_RUN}/node-correction-previews", json=_body()
        )

    assert response.status_code == 200, response.text
    result = response.json()
    assert result["schema_version"] == "manyforge.program_node_correction_preview.v2"
    assert result["preview_check"] == "trajectory_planned_and_collision_validated"
    assert result["preview"] == {"ok": True, "command": "preview_motion", "preview_receipt": {}}
    assert result["node_attempt_id"] == "attempt:1"
    assert result["target"]["component_id"] == "robot.1"
    assert "operation" not in result
    assert runner.calls == [(_RUN, _body())]
    # Exactly one advisory native read, attributed like a manual preview.
    assert [kind for kind, _request in execution.requests] == ["preview_motion"]
    assert execution.requests[0][1].model_dump(mode="json") == _operation()
    assert execution.principals == [("preview_motion", "operator.7")]


def test_unavailable_preview_names_its_reason_and_skips_the_native_owner() -> None:
    reason = {
        "code": "goal_frame_motion_preview_unsupported",
        "message": "the native motion preview cannot resolve this goal frame",
    }
    execution = FakeExecution()
    service = _service(
        FakeCore(), FakeManager(), execution, program_runner=_Runner(_built(unavailable=reason))
    )

    result = service.program_node_correction_preview(
        _RUN,
        ProgramNodeCorrectionPreviewRequest.model_validate(_body()),
        TrustedAttribution(),
    )

    assert result["unavailable_reason"] == reason
    assert result["preview"] is None
    assert result["preview_check"] is None
    assert execution.requests == []


@pytest.mark.parametrize(
    "mutate",
    [
        lambda built: built.update(schema_version="manyforge.program_node_correction_preview_intent.v1"),
        lambda built: built.update(node_attempt_id="attempt:other"),
        lambda built: built.update(program_runner_owner_epoch="program-runner.epoch.2"),
        lambda built: built.update(operation=None),
        lambda built: built["operation"]["participants"][0]["target"].update(command_target_id="other"),
        lambda built: built["target"].update(target_role_id="old"),
        lambda built: built["operation"].update(program_run_id="program-run.8"),
        lambda built: built["operation"]["participants"][0].update(
            action_span={
                "action_source": "program_node",
                "action_span_id": "attempt:1",
                "node_instance_id": "move_to_pick",
                "node_attempt_id": "attempt:1",
            }
        ),
        lambda built: built.update(extra=True),
    ],
)
def test_inexact_runner_preview_is_refused_before_the_native_owner(mutate) -> None:
    built = _built()
    mutate(built)
    execution = FakeExecution()
    service = _service(FakeCore(), FakeManager(), execution, program_runner=_Runner(built))

    with pytest.raises(HeadlessApiError) as refused:
        service.program_node_correction_preview(
            _RUN,
            ProgramNodeCorrectionPreviewRequest.model_validate(_body()),
            TrustedAttribution(),
        )

    assert refused.value.code == "program_runner_response_invalid"
    assert execution.requests == []


def test_runner_refusal_and_path_mismatch_keep_their_codes() -> None:
    runner = _Runner(
        ProgramRunnerClientError(
            "program_node_preview_input_invalid", "selects captured binding meaning", 400
        )
    )
    execution = FakeExecution()
    app = create_app(
        service=_service(FakeCore(), FakeManager(), execution, program_runner=runner)
    )

    with TestClient(app) as client:
        refused = client.post(
            f"/api/program/runs/{_RUN}/node-correction-previews", json=_body()
        )
        mismatch = client.post(
            "/api/program/runs/program-run.8/node-correction-previews", json=_body()
        )
        foreign_field = client.post(
            f"/api/program/runs/{_RUN}/node-correction-previews",
            json={**_body(), "program_binding": {"target_role_bindings": []}},
        )
        duplicate = client.post(
            f"/api/program/runs/{_RUN}/node-correction-previews",
            json=_body(
                proposed_inputs=[
                    {"path": "goal", "value": []},
                    {"path": "goal", "value": []},
                ]
            ),
        )

    assert refused.status_code == 400
    assert refused.json()["detail"]["code"] == "program_node_preview_input_invalid"
    assert mismatch.status_code == 400
    assert foreign_field.status_code == 422
    assert duplicate.status_code == 422
    assert len(runner.calls) == 1
    assert execution.requests == []


class _BlockingRunner:
    """Holds the first preview of ``attempt:1`` until released."""

    def __init__(self) -> None:
        import threading

        self.entered = threading.Event()
        self.release = threading.Event()
        self.calls: list[str] = []

    def preview_node_correction(self, program_run_id: str, body: dict[str, Any]):
        attempt = body["node_attempt_id"]
        self.calls.append(attempt)
        if attempt == "attempt:1" and not self.release.is_set():
            self.entered.set()
            assert self.release.wait(timeout=5)
        built = _built()
        built["node_attempt_id"] = attempt
        built["request_id"] = body["request_id"]
        built["operation"]["request_context"]["request_id"] = body["request_id"]
        return built


def test_one_preview_per_node_attempt_is_in_flight_and_duplicates_are_refused() -> None:
    from concurrent.futures import ThreadPoolExecutor

    runner = _BlockingRunner()
    execution = FakeExecution()
    app = create_app(
        service=_service(FakeCore(), FakeManager(), execution, program_runner=runner)
    )
    path = f"/api/program/runs/{_RUN}/node-correction-previews"

    with TestClient(app) as client, ThreadPoolExecutor(max_workers=1) as pool:
        first = pool.submit(client.post, path, json=_body())
        assert runner.entered.wait(timeout=5)
        duplicate = client.post(path, json=_body(request_id="node-preview.2"))
        other_attempt = client.post(path, json=_body(node_attempt_id="attempt:2"))
        runner.release.set()
        completed = first.result(timeout=5)
        after = client.post(path, json=_body(request_id="node-preview.3"))

    assert duplicate.status_code == 409
    assert duplicate.json()["detail"]["code"] == "program_node_preview_busy"
    # The refusal is pre-effect: no runner build and no native preview.
    assert runner.calls == ["attempt:1", "attempt:2", "attempt:1"]
    assert other_attempt.status_code == 200, other_attempt.text
    assert other_attempt.json()["node_attempt_id"] == "attempt:2"
    assert completed.status_code == 200, completed.text
    # The bound is released when the preview finishes, also after a refusal.
    assert after.status_code == 200, after.text
    assert [kind for kind, _request in execution.requests] == ["preview_motion"] * 3
