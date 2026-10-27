"""Closed Draft/Saved Program workspace and descriptor-bound artifact tests."""

from __future__ import annotations

from copy import deepcopy
import json
from pathlib import Path
import shutil
from types import SimpleNamespace

from fastapi.testclient import TestClient
import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.executable_node_resolver import build_executable_node_resolver
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_composer.backend import program_artifacts
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.draft_session import DraftSessionError
from manyforge_composer.backend.execution_observation_stream import (
    ExecutionObservationSource,
)
from manyforge_composer.backend.program_artifacts import (
    ProgramArtifactError,
    load_program_artifact,
    save_program_artifact,
)
from manyforge_composer.backend import routes_draft
from manyforge_composer.backend.authoring_admission import LocalAuthoringCommit
from manyforge_composer.backend.authoring_forest import derive_candidate
from manyforge_composer.backend.authoring_workspace import AuthoringWorkspace
from manyforge_composer.backend.deployment_draft import (
    CompiledDeploymentDocument,
    DeploymentDraftSession,
)
from manyforge_composer.backend.routes_draft import _observed_scene_arguments
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.client import HeadlessClientError


_ROOT = Path(__file__).resolve().parents[2]
_SOURCE_PROGRAM = _ROOT / "examples" / "pick_and_place_minimal.program.yaml"


class _AuthoringPassThrough:
    """Keep route-unit tests focused; the real gate has dedicated tests."""

    receiver_epoch = "composer-snapshot.test"
    mutation_ready = True

    def execute(
        self,
        _envelope,
        *,
        effect,
        prepare_effect=None,
        validate_under_claim=None,
        **_kwargs,
    ):
        if prepare_effect is not None:
            prepare_effect()
        if validate_under_claim is not None:
            validate_under_claim({})
        return effect({})

    def execute_local_mutation(
        self,
        _envelope,
        *,
        session="program",
        draft_id=None,
        workspace,
        install=None,
        expected_identity=None,
        project=None,
        validate_under_claim=None,
        prepare_project_under_claim=None,
        **_kwargs,
    ):
        """Commit through the real composite owner; only the Kernel is stubbed.

        Every migrated route commits through the workspace owner, so the fence,
        the revision and the response projection come from production code. The
        admission protocol itself is covered by
        ``test_authoring_admission.py`` and ``test_authoring_local_mutation.py``.
        """

        if validate_under_claim is not None:
            validate_under_claim()
        if prepare_project_under_claim is not None:
            project = prepare_project_under_claim()
        if session == "whole-workspace":
            snapshot = workspace.replace(
                expected_identity=expected_identity,
                install=install,
                project=project,
            )
        elif session == "project":
            snapshot = workspace.commit_project(
                expected_identity=expected_identity,
                facts=project,
            )
        elif session == "deployment":
            snapshot = workspace.commit_deployment(
                draft_id=draft_id,
                expected_identity=expected_identity,
                install=install,
            )
        else:
            snapshot = workspace.commit_program(
                draft_id=draft_id,
                expected_identity=expected_identity,
                install=install,
            )
        return LocalAuthoringCommit(snapshot=snapshot, admission_generation=2)

    def execute_draft_edit(
        self, _envelope, *, workspace, session, draft_id, expected_identity,
        install, validate=None, **_kwargs,
    ):
        """A Draft-only edit through the real composite owner; no Kernel."""

        if validate is not None:
            validate()
        commit = (
            workspace.commit_deployment if session == "deployment"
            else workspace.commit_program
        )
        return LocalAuthoringCommit(
            snapshot=commit(
                draft_id=draft_id, expected_identity=expected_identity, install=install
            ),
            admission_generation=2,
        )


_FENCED_STATE: ComposerState | None = None


def _enable_authoring(state: ComposerState) -> ComposerState:
    global _FENCED_STATE
    state.authoring_admission = _AuthoringPassThrough()  # type: ignore[assignment]
    _FENCED_STATE = state
    return state


def _copy_program(tmp_path: Path, name: str = "fixture.program.yaml") -> Path:
    target = tmp_path / name
    shutil.copyfile(_SOURCE_PROGRAM, target)
    return target


def _client(
    tmp_path: Path,
    *,
    allowed_fs_roots: list[str] | None = None,
    allowed_program_write_roots: list[str] | None = None,
) -> TestClient:
    return TestClient(
        create_app(
            _enable_authoring(ComposerState()),
            security=SecurityConfig(
                allowed_fs_roots=allowed_fs_roots or [str(tmp_path)],
                allowed_program_write_roots=allowed_program_write_roots,
                cors_origins=["http://localhost:3000"],
            ),
        )
    )


def _fence(workspace: dict) -> dict:
    """Build the Program Draft fence plus the required workspace fence."""

    identity = workspace.get("authoringWorkspace")
    if identity is None:
        assert _FENCED_STATE is not None
        identity = _FENCED_STATE.authoring_workspace.identity().as_dict()
    return {
        "draftId": workspace["draft"]["draftId"],
        "expectedDraftRevision": workspace["draft"]["draftRevision"],
        "expectedAdmissionGeneration": 1,
        "requestOwnerEpoch": "test-browser.epoch.1",
        "requestId": "test-browser.request.1",
        "expectedWorkspaceEpoch": identity["workspaceEpoch"],
        "expectedWorkspaceRevision": identity["workspaceRevision"],
    }


def _motion_identity() -> dict:
    return {
        "owner_epoch": "owner-1",
        "graph_revision": "graph-1",
        "retained_bundle_id": "bundle-1",
    }


def _motion_targets_projection() -> dict:
    return {
        "ok": True,
        "identity": _motion_identity(),
        "targets": {
            "schema_version": "manyforge.execution_targets.v3",
            "components": [{
                "controllable_component_id": "robot",
                "availability": {"command_eligible": True, "reason": None},
                "targets": [{
                    "command_target_id": "arm",
                    "operation_capability_id": "execution.trajectory.execute",
                    "presentation": "motion",
                    "command": {
                        "target_role_id": "command_target",
                        "base_frame_role_id": "base",
                        "tcp_frame_role_id": "tcp",
                        "target_descriptor": {"frames": [
                            {"frame_role_id": "base", "compiled_frame_name": "robot.base",
                             "model_instance_id": "robot-model", "model_revision": "model-r1"},
                            {"frame_role_id": "tcp", "compiled_frame_name": "robot.tcp",
                             "model_instance_id": "robot-model", "model_revision": "model-r1"},
                        ]},
                    },
                }],
            }],
        },
    }


def _motion_control_projection(*, sequence: int = 1) -> dict:
    names = [
        "shoulder_pan_joint",
        "shoulder_lift_joint",
        "elbow_joint",
        "wrist_1_joint",
        "wrist_2_joint",
        "wrist_3_joint",
    ]
    return {
        "ok": True,
        "identity": _motion_identity(),
        "control_state": {
            "schema_version": "manyforge.execution_control_state.v6",
            "observed_targets": [{
                "controllable_component_id": "robot",
                "command_target_id": "arm",
                "operation_capability_id": "execution.trajectory.execute",
                "observed": {
                    "kind": "joint_positions",
                    "complete": True,
                    "current_tcp_pose": {
                        "position_m": {"x_m": 0.1, "y_m": 0.2, "z_m": 0.3},
                        "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
                },
                "positions_rad": [0.1, -1.1, 0.9, -1.4, -1.2, 0.3],
                "sequence": sequence,
                "source_epoch": "native-source",
                "source_joint_names": names,
                },
            }],
        },
    }


def _node_environment_projection() -> dict:
    """Return the smallest build-qualified node-environment projection."""
    environment = build_node_environment()
    manifest = environment.manifest
    dispositions = {
        item.row: item
        for item in build_executable_node_resolver(environment).dispositions
    }
    implementations = []
    for row in manifest.executable_registry:
        disposition = dispositions[row]
        implementations.append(
            {
                "adapter_build_qualified": disposition.adapter_build_qualified,
                "authoring_available": True,
                "binding_available": False,
                "engine_id": row.engine_id,
                "executable_package_id": row.executable_package_id,
                "execution_available": False,
                "implementation_id": row.implementation_id,
                "implementation_identity_resolved": True,
                "node_id": row.node_id,
                "owner_health_available": (
                    disposition.adapter_build_qualified
                    and disposition.owner_port_name is None
                ),
                "reason_codes": sorted(
                    {
                        "program_binding_unselected",
                        *(
                            [disposition.unavailable_reason_code]
                            if disposition.unavailable_reason_code is not None
                            else []
                        ),
                        *(
                            ["program_runtime_owner_unavailable"]
                            if disposition.owner_port_name is not None
                            or not disposition.adapter_build_qualified
                            else []
                        ),
                    }
                ),
                "restoration_gates": list(disposition.restoration_gates),
            }
        )
    return {
        "schema_version": "manyforge.node_environment_projection.v2",
        "retained_bundle_id": "bundle-1",
        "graph_revision": "graph-1",
        "catalog_id": manifest.catalog_id,
        "content_version": manifest.catalog_content_version,
        "catalog_digest": manifest.catalog_digest,
        "environment_digest": manifest.environment_digest,
        "package_environment": {
            "catalog_id": manifest.catalog_id,
            "content_version": manifest.catalog_content_version,
            "catalog_digest": manifest.catalog_digest,
            "environment_digest": manifest.environment_digest,
        },
        "catalog": environment.catalog.raw_data,
        "manifest": manifest.to_dict(),
        "availability": {
            "binding_context": "unselected",
            "execution_available": False,
            "implementations": implementations,
            "manager_runtime_converged": True,
            "portable_authoring_available": True,
            "reason_codes": [
                "program_binding_unselected",
                "program_runtime_owner_unavailable",
            ],
            "restoration_gates": ["I6", "N"],
        },
        "source": {
            "available": True,
            "freshness": "fresh",
            "known_gaps": [],
            "projection_generation": 1,
            "projection_owner_epoch": "headless.epoch.1",
            "receiver_age_ns": 0,
            "source_generation": 1,
            "source_owner_epoch": "manager.epoch.1",
            "source_owner_id": "component-manager.test",
            "source_content_digest": "sha256:" + "a" * 64,
        },
    }


def _mock_applied_motion_binding(monkeypatch: pytest.MonkeyPatch) -> dict:
    binding = {
        "graph_revision": "graph-1", "retained_bundle_id": "bundle-1",
        "target_role_bindings": [{
            "target_role_id": "arm", "controllable_component_id": "robot",
            "command_target_id": "arm", "frame_role_bindings": [
                {"frame_role_id": "base", "compiled_frame_name": "robot.base",
                 "model_instance_id": "robot-model", "model_revision": "model-r1"},
                {"frame_role_id": "tcp", "compiled_frame_name": "robot.tcp",
                 "model_instance_id": "robot-model", "model_revision": "model-r1"},
            ],
        }],
    }
    monkeypatch.setattr(routes_draft, "_applied_context", lambda _state: (
        SimpleNamespace(binding_document=binding, program_revision=None), None,
    ))
    return binding


def test_observed_pose_joins_applied_address_and_preserves_base_frame(monkeypatch: pytest.MonkeyPatch) -> None:
    _mock_applied_motion_binding(monkeypatch)
    targets = _motion_targets_projection()
    other = deepcopy(targets["targets"]["components"][0])
    other["controllable_component_id"] = "other-robot"
    targets["targets"]["components"].insert(0, other)
    control = _motion_control_projection()
    other_observed = deepcopy(control["control_state"]["observed_targets"][0])
    other_observed["controllable_component_id"] = "other-robot"
    other_observed["observed"]["current_tcp_pose"]["position_m"]["x_m"] = 99
    control["control_state"]["observed_targets"].insert(0, other_observed)
    parameters = {
        "target_role": "arm",
        "base_frame_role": "base",
        "tcp_endpoint": {"target_role": "arm", "frame_role": "tcp"},
    }
    authoring = {"forest": [{"instance_id": "move", "node_id": "motion.move_to_pose", "parameters": parameters}]}
    state = SimpleNamespace(
        headless_client=SimpleNamespace(execution_targets=lambda: targets, execution_control_state=lambda: control),
        draft_session=SimpleNamespace(snapshot=lambda: {"authoring": authoring}),
    )
    args = {"instance_id": "move", "observed_kind": "tcp_pose"}
    resolved, _identity = routes_draft._observed_move_arguments(state, args)
    assert resolved["observed"]["current_tcp_pose"]["position_m"]["x_m"] == 0.1
    assert resolved["observed"]["frame_ref"] == {
        "kind": "target_frame", "target_role": "arm", "frame_role": "base",
    }


def test_observed_pose_endpoint_capture_requires_exact_current_tcp_model_frame(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    binding = _mock_applied_motion_binding(monkeypatch)
    binding["target_role_bindings"].append({
        "target_role_id": "tool",
        "controllable_component_id": "robot",
        "command_target_id": "gripper",
        "frame_role_bindings": [{
            "frame_role_id": "jaw_tcp",
            "compiled_frame_name": "robot.tcp",
            "model_instance_id": "robot-model",
            "model_revision": "model-r1",
        }],
    })
    targets = _motion_targets_projection()
    control = _motion_control_projection()
    authoring = {"forest": [{
        "instance_id": "move",
        "node_id": "motion.move_to_pose",
        "parameters": {
            "target_role": "arm",
            "base_frame_role": "base",
            "tcp_endpoint": {"target_role": "tool", "frame_role": "jaw_tcp"},
        },
    }]}
    state = SimpleNamespace(
        headless_client=SimpleNamespace(
            execution_targets=lambda: targets,
            execution_control_state=lambda: control,
        ),
        draft_session=SimpleNamespace(snapshot=lambda: {"authoring": authoring}),
    )

    resolved, _identity = routes_draft._observed_move_arguments(
        state, {"instance_id": "move", "observed_kind": "tcp_pose"},
    )
    assert resolved["observed"]["frame_ref"] == {
        "kind": "target_frame", "target_role": "arm", "frame_role": "base",
    }

    binding["target_role_bindings"][1]["frame_role_bindings"][0]["compiled_frame_name"] = (
        "robot.jaw_tcp"
    )
    with pytest.raises(DraftSessionError) as frame_mismatch:
        routes_draft._observed_move_arguments(
            state, {"instance_id": "move", "observed_kind": "tcp_pose"},
        )
    assert frame_mismatch.value.code == "observed_move_frame_mismatch"
    assert (
        "TCP endpoint target role 'tool' frame role 'jaw_tcp' resolves to 'robot.jaw_tcp', "
        "but the robot reports its current pose with TCP frame 'robot.tcp'; current-pose "
        "capture requires the selected endpoint to match the robot's current TCP"
    ) in str(frame_mismatch.value)

    binding["target_role_bindings"][1]["frame_role_bindings"][0]["compiled_frame_name"] = (
        "robot.tcp"
    )
    binding["target_role_bindings"][1]["frame_role_bindings"][0]["model_revision"] = "model-r2"
    with pytest.raises(DraftSessionError) as mismatch:
        routes_draft._observed_move_arguments(
            state, {"instance_id": "move", "observed_kind": "tcp_pose"},
        )
    assert mismatch.value.code == "observed_move_frame_mismatch"
    assert (
        "TCP endpoint target role 'tool' frame role 'jaw_tcp' and the robot's current TCP "
        "do not share the same retained model instance and revision"
    ) in str(mismatch.value)


def _set_parameter(field: str, value: object):
    def change(node: dict, _binding: dict, _targets: dict) -> None:
        if value is None:
            node["parameters"].pop(field)
        else:
            node["parameters"][field] = value
    return change


def _replace_configuration_and_lose_target(_node: dict, binding: dict, _targets: dict) -> None:
    # A stale configuration also hides the target; the configuration is named.
    binding["graph_revision"] = "replaced"
    binding["target_role_bindings"][0]["command_target_id"] = "gone"


def _lose_bound_target(_node: dict, binding: dict, _targets: dict) -> None:
    binding["target_role_bindings"][0]["command_target_id"] = "gone"


def _duplicate_bound_target(_node: dict, _binding: dict, targets: dict) -> None:
    component = targets["targets"]["components"][0]
    component["targets"].append(deepcopy(component["targets"][0]))


def _unbind_frame_role(_node: dict, binding: dict, _targets: dict) -> None:
    binding["target_role_bindings"][0]["frame_role_bindings"].pop(0)


def _drop_observed_base_frame(_node: dict, _binding: dict, targets: dict) -> None:
    command = targets["targets"]["components"][0]["targets"][0]["command"]
    command["target_descriptor"]["frames"].pop(0)


def _drop_owner_identity(_node: dict, _binding: dict, targets: dict) -> None:
    targets.pop("identity")


def _no_change(_node: dict, _binding: dict, _targets: dict) -> None:
    return None


def _invalid_tcp_endpoint(node: dict, _binding: dict, _targets: dict) -> None:
    node["parameters"]["tcp_endpoint"] = None


@pytest.mark.parametrize(
    ("observed_kind", "change", "applied", "code", "message"),
    [
        pytest.param("joint", _set_parameter("target_role", None), "bound",
                     "observed_move_role_missing", "has no target role", id="target-role-missing"),
        pytest.param("tcp_pose", _set_parameter("base_frame_role", None), "bound",
                     "observed_move_role_missing", "has no base frame role", id="base-role-missing"),
        pytest.param("tcp_pose", _set_parameter("tcp_endpoint", None), "bound",
                     "observed_move_role_missing",
                     "the selected move has no valid TCP endpoint; choose its target and frame "
                     "roles before capturing the current robot pose",
                     id="tcp-endpoint-missing"),
        pytest.param("tcp_pose", _invalid_tcp_endpoint, "bound",
                     "observed_move_role_missing",
                     "has no valid TCP endpoint",
                     id="endpoint-selector-invalid"),
        pytest.param("joint", _no_change, "unavailable", "observed_move_unavailable",
                     "could not be read: Applied owner disconnected", id="applied-unreadable"),
        pytest.param("joint", _no_change, "none", "observed_move_target_unbound",
                     "no Program with robot target bindings is Applied", id="nothing-applied"),
        pytest.param("joint", _drop_owner_identity, "bound", "observed_move_unavailable",
                     "publishes no configuration identity", id="owner-identity-missing"),
        pytest.param("tcp_pose", _replace_configuration_and_lose_target, "bound",
                     "observed_move_configuration_changed", "the Applied configuration changed",
                     id="configuration-changed-first"),
        pytest.param("joint", _set_parameter("target_role", "unbound"), "bound",
                     "observed_move_target_unbound",
                     "target role 'unbound' is not bound in the Applied Program",
                     id="target-role-unbound"),
        pytest.param("joint", _lose_bound_target, "bound", "observed_move_target_unavailable",
                     "bound robot target is unavailable in the current execution owner",
                     id="target-unavailable"),
        pytest.param("joint", _duplicate_bound_target, "bound", "observed_move_target_unavailable",
                     "more than one matching robot target", id="target-ambiguous"),
        pytest.param("tcp_pose", _unbind_frame_role, "bound", "observed_move_target_unbound",
                     "base frame role 'base' is not bound for target role 'arm'",
                     id="frame-role-unbound"),
        pytest.param("tcp_pose", _set_parameter(
                         "tcp_endpoint", {"target_role": "arm", "frame_role": "base"}
                     ), "bound",
                     "observed_move_frame_mismatch",
                     "TCP endpoint target role 'arm' frame role 'base' resolves to "
                     "'robot.base', but the robot reports its current pose with TCP frame "
                     "'robot.tcp'",
                     id="frame-mismatch"),
        pytest.param("tcp_pose", _drop_observed_base_frame, "bound", "observed_move_frame_mismatch",
                     "does not report a base frame", id="observed-frame-missing"),
    ],
)
def test_observed_move_names_each_refusal_cause_before_any_draft_change(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    observed_kind: str,
    change,
    applied: str,
    code: str,
    message: str,
) -> None:
    binding = _mock_applied_motion_binding(monkeypatch)
    if applied == "unavailable":
        monkeypatch.setattr(routes_draft, "_applied_context", lambda _state: (
            None, {"code": "headless_unavailable", "message": "Applied owner disconnected"},
        ))
    elif applied == "none":
        monkeypatch.setattr(routes_draft, "_applied_context", lambda _state: (
            SimpleNamespace(binding_document=None, program_revision=None), None,
        ))
    targets = _motion_targets_projection()

    class Headless:
        def execution_targets(self) -> dict:
            return targets

        def node_environment(self) -> dict:
            return _node_environment_projection()

        def execution_control_state(self) -> dict:
            return _motion_control_projection()

        def program_applied(self) -> dict:
            return {}

    program = Program(_SOURCE_PROGRAM, build_node_environment().catalog)
    state = _enable_authoring(ComposerState(program=program))
    state.headless_client = Headless()  # type: ignore[assignment]
    state.execution_observation_source = ExecutionObservationSource(
        state.headless_client  # type: ignore[arg-type]
    )
    workspace = state.draft_session.workspace_snapshot()
    session = state.draft_session
    # The resolver sees the selected move in the requested shape; the stored
    # Draft itself is never touched by this test.
    view = deepcopy(session.snapshot())
    node = view["authoring"]["forest"][0]["children"][0]
    assert node["instance_id"] == "move_to_pick"
    if observed_kind == "tcp_pose":
        node["node_id"] = "motion.move_to_pose"
    change(node, binding, targets)

    def snapshot() -> dict:
        return deepcopy(view)

    def prepare_mutation(**_kwargs):
        raise AssertionError("a refused observed move must not prepare a Draft change")

    monkeypatch.setattr(session, "snapshot", snapshot)
    monkeypatch.setattr(session, "prepare_mutation", prepare_mutation)
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(tmp_path)],
                cors_origins=["http://localhost:3000"],
            ),
        )
    )

    response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(workspace),
            "mutation": "program_draft_store_observed_move",
            "arguments": {"instance_id": "move_to_pick", "observed_kind": observed_kind},
        },
    )

    assert response.status_code == 409, response.text
    detail = response.json()["detail"]
    assert detail["code"] == code
    assert message in detail["message"]
    assert detail["effect"] == "none"
    monkeypatch.undo()
    after = state.draft_session.workspace_snapshot()
    assert after["draft"]["draftRevision"] == workspace["draft"]["draftRevision"]
    assert after["draft"]["authoring"] == workspace["draft"]["authoring"]


@pytest.mark.parametrize(
    ("node_id", "observed_kind", "message"),
    [
        ("motion.move_to_pose", "joint", "Move to joint target node"),
        ("motion.move_to_joint_target", "tcp_pose", "Move to pose node"),
        ("scene.remove_object", "joint", "Move to joint target node"),
    ],
)
def test_observed_move_refuses_a_node_that_cannot_hold_the_capture(
    node_id: str, observed_kind: str, message: str,
) -> None:
    authoring = {"forest": [{"instance_id": "move", "node_id": node_id,
                             "parameters": {"target_role": "arm"}}]}
    state = SimpleNamespace(
        headless_client=SimpleNamespace(
            execution_targets=_motion_targets_projection,
            execution_control_state=_motion_control_projection,
        ),
        draft_session=SimpleNamespace(snapshot=lambda: {"authoring": authoring}),
    )
    with pytest.raises(DraftSessionError, match=message) as refused:
        routes_draft._observed_move_arguments(
            state, {"instance_id": "move", "observed_kind": observed_kind},
        )
    assert refused.value.code == "observed_move_node_unsupported"
    assert refused.value.status_code == 409


@pytest.mark.parametrize("failing_read", ["execution_targets", "execution_control_state"])
def test_observed_move_names_a_failed_robot_pose_read_before_any_draft_change(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, failing_read: str,
) -> None:
    class Headless:
        def execution_targets(self) -> dict:
            if failing_read == "execution_targets":
                raise HeadlessClientError(
                    "headless_unavailable", "execution owner disconnected", 503,
                )
            return _motion_targets_projection()

        def execution_control_state(self) -> dict:
            if failing_read == "execution_control_state":
                raise HeadlessClientError(
                    "headless_unavailable", "execution owner disconnected", 502,
                )
            return _motion_control_projection()

        def node_environment(self) -> dict:
            return _node_environment_projection()

    program = Program(_SOURCE_PROGRAM, build_node_environment().catalog)
    state = _enable_authoring(ComposerState(program=program))
    state.headless_client = Headless()  # type: ignore[assignment]
    state.execution_observation_source = ExecutionObservationSource(
        state.headless_client  # type: ignore[arg-type]
    )
    workspace = state.draft_session.workspace_snapshot()

    def prepare_mutation(**_kwargs):
        raise AssertionError("a failed pose read must not prepare a Draft change")

    monkeypatch.setattr(state.draft_session, "prepare_mutation", prepare_mutation)
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(tmp_path)],
                cors_origins=["http://localhost:3000"],
            ),
        )
    )

    response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(workspace),
            "mutation": "program_draft_store_observed_move",
            "arguments": {"instance_id": "move_to_pick", "observed_kind": "joint"},
        },
    )

    # The code, not the status, is what tells the client nothing was prepared.
    assert response.status_code == 503, response.text
    detail = response.json()["detail"]
    assert detail["code"] == "observed_move_unavailable"
    assert "could not be read: execution owner disconnected" in detail["message"]
    assert detail["context"] == {"headless_code": "headless_unavailable"}
    assert detail["effect"] == "none"
    monkeypatch.undo()
    after = state.draft_session.workspace_snapshot()
    assert after["draft"]["draftRevision"] == workspace["draft"]["draftRevision"]
    assert after["draft"]["authoring"] == workspace["draft"]["authoring"]


def test_observed_move_reads_one_exact_source_snapshot_under_the_claim(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    _mock_applied_motion_binding(monkeypatch)
    class Headless:
        def __init__(self) -> None:
            self.reads = 0

        def execution_targets(self) -> dict:
            return _motion_targets_projection()

        def node_environment(self) -> dict:
            return _node_environment_projection()

        def execution_control_state(self) -> dict:
            self.reads += 1
            return _motion_control_projection(sequence=self.reads)

        def program_applied(self) -> dict:
            return {}

    program = Program(_SOURCE_PROGRAM, build_node_environment().catalog)
    state = _enable_authoring(ComposerState(program=program))
    state.headless_client = Headless()  # type: ignore[assignment]
    state.execution_observation_source = ExecutionObservationSource(
        state.headless_client  # type: ignore[arg-type]
    )
    workspace = state.draft_session.workspace_snapshot()
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(tmp_path)],
                cors_origins=["http://localhost:3000"],
            ),
        )
    )

    response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(workspace),
            "mutation": "program_draft_store_observed_move",
            "arguments": {
                "instance_id": "move_to_pick",
                "observed_kind": "joint",
            },
        },
    )

    assert response.status_code == 200, response.text
    assert state.headless_client.reads == 1
    goal = response.json()["workspace"]["draft"]["authoring"]["forest"][0][
        "children"
    ][0]["parameters"]["goal"]
    assert [row["position_rad"] for row in goal] == [
        0.1,
        -1.1,
        0.9,
        -1.4,
        -1.2,
        0.3,
    ]


def test_observed_move_is_one_revision_fenced_draft_mutation(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    _mock_applied_motion_binding(monkeypatch)
    class Headless:
        def execution_targets(self) -> dict:
            return _motion_targets_projection()

        def node_environment(self) -> dict:
            return _node_environment_projection()

        def execution_control_state(self) -> dict:
            return _motion_control_projection()

        def program_applied(self) -> dict:
            return {}

    program = Program(_SOURCE_PROGRAM, build_node_environment().catalog)
    state = _enable_authoring(ComposerState(program=program))
    state.headless_client = Headless()  # type: ignore[assignment]
    state.execution_observation_source = ExecutionObservationSource(
        state.headless_client  # type: ignore[arg-type]
    )
    workspace = state.draft_session.workspace_snapshot()
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(tmp_path)],
                cors_origins=["http://localhost:3000"],
            ),
        )
    )

    response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(workspace),
            "mutation": "program_draft_store_observed_move",
            "arguments": {
                "instance_id": "move_to_pick",
                "observed_kind": "joint",
            },
        },
    )

    assert response.status_code == 200, response.text
    body = response.json()
    assert body["workspace"]["draft"]["draftRevision"] == (
        workspace["draft"]["draftRevision"] + 1
    )
    goal = body["workspace"]["draft"]["authoring"]["forest"][0]["children"][0][
        "parameters"
    ]["goal"]
    assert [row["position_rad"] for row in goal] == [
        0.1,
        -1.1,
        0.9,
        -1.4,
        -1.2,
        0.3,
    ]


def test_observed_scene_save_preserves_program_metadata_and_source_identity() -> None:
    class Headless:
        def scene(self) -> dict:
            return {
                "logical_scene": {
                    "access": {
                        "active_projection": {
                            "retained_bundle_id": "bundle",
                            "graph_revision": "graph",
                            "scene_revision": {
                                "scene_id": "cell",
                                "revision_id": 3,
                                "content_hash": "sha256:" + "1" * 64,
                            },
                        }
                    },
                    "snapshot": {
                        "transaction_sequence": 7,
                        "objects": [{
                            "scene_resource_id": "table",
                            "placement": {
                                "kind": "cell_fixed",
                                "cell_id": "cell",
                                "pose_in_cell": {
                                    "position_m": {"x": 1.0, "y": 2.0, "z": 3.0},
                                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                                },
                            },
                            "collision_shapes": [{
                                "type": "box",
                                "dimensions_m": {"x": 0.5, "y": 0.6, "z": 0.7},
                            }],
                            "provider_evidence": None,
                            "support_contacts": [],
                        }],
                    },
                }
            }

    catalog = build_node_environment().catalog
    document = Program(_SOURCE_PROGRAM, catalog).to_dict()
    document["scene"][0]["resource_roles"] = ["support_surface"]
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    state = ComposerState(program=Program.from_dict(document, catalog))
    state.headless_client = Headless()  # type: ignore[assignment]

    arguments, identity = _observed_scene_arguments(
        state, {"scene_resource_id": "table"}
    )

    resource = arguments["scene_resource"]
    assert resource["resource_roles"] == ["support_surface"]
    assert resource["shape"] == {
        "type": "box",
        "box_dimensions_m": [0.5, 0.6, 0.7],
    }
    assert resource["pose"]["position_m"] == [1.0, 2.0, 3.0]
    assert identity["transaction_sequence"] == 7


def _carried_scene() -> dict:
    return {"logical_scene": {
        "access": {"active_projection": {"retained_bundle_id": "bundle"}},
        "snapshot": {"transaction_sequence": 7, "objects": [{
            "scene_resource_id": "part",
            "placement": {
                "kind": "carried",
                "carried_object_attachment_id": "attachment.1",
                "carried_object_attachment_revision": "1",
            },
            "collision_shapes": [{"type": "box", "dimensions_m": {"x": 0.1, "y": 0.1, "z": 0.1}}],
            "provider_evidence": None,
            "support_contacts": [],
        }]},
    }}


@pytest.mark.parametrize(
    ("scene", "status", "code"),
    [
        # The live case: a carried object is not eligible.
        pytest.param(_carried_scene, 409, "observed_scene_object_ineligible", id="carried"),
        pytest.param(None, 503, "observed_scene_unavailable", id="scene-read-failed"),
    ],
)
def test_observed_scene_refusal_is_marked_no_effect_and_logged_by_code(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
    scene,
    status: int,
    code: str,
) -> None:
    class Headless:
        def scene(self) -> dict:
            if scene is None:
                raise HeadlessClientError(
                    "headless_unavailable", "scene owner disconnected", 503,
                )
            return scene()

        def node_environment(self) -> dict:
            return _node_environment_projection()

    program = Program(_SOURCE_PROGRAM, build_node_environment().catalog)
    state = _enable_authoring(ComposerState(program=program))
    state.headless_client = Headless()  # type: ignore[assignment]
    state.execution_observation_source = ExecutionObservationSource(
        state.headless_client  # type: ignore[arg-type]
    )
    workspace = state.draft_session.workspace_snapshot()

    def prepare_mutation(**_kwargs):
        raise AssertionError("a refused copy must not prepare a Draft change")

    monkeypatch.setattr(state.draft_session, "prepare_mutation", prepare_mutation)
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(tmp_path)],
                cors_origins=["http://localhost:3000"],
            ),
        )
    )

    with caplog.at_level("INFO", logger="manyforge_composer.backend.app"):
        response = client.post(
            "/api/program/draft/mutations",
            json={
                **_fence(workspace),
                "mutation": "program_draft_store_observed_scene_resource",
                "arguments": {"scene_resource_id": "part"},
            },
        )

    assert response.status_code == status, response.text
    detail = response.json()["detail"]
    assert detail["code"] == code
    # Refused before any Draft change: the client must not lock the editor.
    assert detail["effect"] == "none"
    logged = [
        record for record in caplog.records
        if getattr(record, "path", None) == "/api/program/draft/mutations"
    ]
    assert [record.status_code for record in logged] == [status]
    # Only the code reaches the request log, never the message.
    assert logged[0].error_code == code
    assert detail["message"] not in logged[0].getMessage()
    monkeypatch.undo()
    after = state.draft_session.workspace_snapshot()
    assert after["draft"]["draftRevision"] == workspace["draft"]["draftRevision"]


def _assert_program_workspace_contract(workspace: dict) -> None:
    """The browser parses these replies as a Program workspace whose
    ``authoringWorkspace`` is exactly its Draft-matched identity."""

    identity = workspace["authoringWorkspace"]
    assert set(identity) == {
        "workspaceEpoch", "workspaceRevision", "programDraftId",
        "programDraftRevision", "deploymentDraftId", "deploymentDraftRevision",
    }
    assert identity["programDraftId"] == workspace["draft"]["draftId"]
    assert identity["programDraftRevision"] == workspace["draft"]["draftRevision"]


def test_open_mutate_save_and_save_as_preserve_exact_workspace_facts(
    tmp_path: Path,
) -> None:
    source = _copy_program(tmp_path)
    client = _client(tmp_path)
    empty = client.get("/api/program/workspace").json()

    opened_response = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(source), "confirmDiscard": False},
    )
    assert opened_response.status_code == 200
    opened = opened_response.json()
    _assert_program_workspace_contract(opened)
    assert opened["draft"]["draftId"] != empty["draft"]["draftId"]
    assert opened["saved"]["path"] == str(source.resolve())
    assert opened["saved"]["matchesDraft"] is True
    assert opened["applied"]["available"] is False

    mutated_response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "tree_draft_update_node_params",
            "arguments": {
                "instance_id": "move_to_pick",
                "expected_node_id": "motion.move_to_joint_target",
                "parameters": {"planning_timeout_s": 3.0},
            },
        },
    )
    assert mutated_response.status_code == 200
    mutated = mutated_response.json()["workspace"]
    assert mutated["hasUnsavedChanges"] is True
    assert mutated["saved"]["matchesDraft"] is False

    saved_response = client.post("/api/program/save", json=_fence(mutated))
    assert saved_response.status_code == 200
    saved = saved_response.json()
    _assert_program_workspace_contract(saved)
    assert saved["saved"]["matchesDraft"] is True
    assert Program(source, build_node_environment().catalog).to_dict()["tree"][
        "children"
    ][0]["parameters"]["planning_timeout_s"] == 3.0

    save_as_response = client.post(
        "/api/program/save-as",
        json={
            **_fence(saved),
            "path": str(tmp_path / "copy"),
            "confirmOverwrite": False,
        },
    )
    assert save_as_response.status_code == 200
    save_as = save_as_response.json()
    expected_copy = tmp_path / "copy.program.yaml"
    assert save_as["saved"]["path"] == str(expected_copy.resolve())
    assert expected_copy.is_file()


def test_node_policy_mutation_is_closed_draft_only_and_revision_fenced(
    tmp_path: Path,
) -> None:
    source = _copy_program(tmp_path)
    original_bytes = source.read_bytes()
    client = _client(tmp_path)
    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(source)},
    ).json()
    arguments = {
        "instance_id": "move_to_pick",
        "recovery_policy": {"allow_intervention": True, "max_retries": 1},
        "operator_stop_policy": "hold",
    }

    response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "program_draft_update_node_policies",
            "arguments": arguments,
        },
    )
    assert response.status_code == 200
    body = response.json()
    assert set(body) == {
        "mutation",
        "details",
        "workspace",
        "authoringWorkspace",
    }
    assert body["mutation"] == "program_draft_update_node_policies"
    assert body["details"] == {"updated_instance_id": "move_to_pick"}
    draft = body["workspace"]["draft"]
    assert draft["draftRevision"] == opened["draft"]["draftRevision"] + 1
    assert source.read_bytes() == original_bytes

    stale = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence({"draft": draft}),
            "mutation": "program_draft_update_node_policies",
            "arguments": {
                **arguments,
                "expected_program_revision": "sha256:" + "f" * 64,
            },
        },
    )
    assert stale.status_code == 400
    assert stale.json()["detail"]["code"] == "draft_arguments_invalid"
    assert client.get("/api/program/workspace").json()["draft"] == draft


def test_program_settings_mutation_is_validated_and_draft_only(
    tmp_path: Path,
) -> None:
    source = _copy_program(tmp_path)
    original_bytes = source.read_bytes()
    client = _client(tmp_path)
    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(source)},
    ).json()

    response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "program_draft_update_settings",
            "arguments": {
                "name": "Portable pick",
                "description": "Edited only in the Composer Draft.",
                "program_profile": "composable-cell-v1",
                "compatibility": {
                    "lanes": ["manyforge-only", "real-robot"],
                    "assistant": "optional",
                    "requires": ["scene.mutable"],
                    "tags": ["pick"],
                },
                "execution_groups": {},
            },
        },
    )

    assert response.status_code == 200, response.text
    workspace = response.json()["workspace"]
    assert workspace["draft"]["program"]["name"] == "Portable pick"
    assert workspace["draft"]["program"]["compatibility"]["lanes"] == [
        "manyforge-only",
        "real-robot",
    ]
    assert workspace["hasUnsavedChanges"] is True
    assert source.read_bytes() == original_bytes


@pytest.mark.parametrize("invalid", ["removed_policy_field", "instance_id"])
def test_node_policy_rejections_are_atomic_and_do_not_read_headless(
    tmp_path: Path,
    invalid: str,
) -> None:
    source = _copy_program(tmp_path)
    original_bytes = source.read_bytes()

    class FailOnHeadlessCall:
        def __init__(self) -> None:
            self.calls: list[str] = []

        def __getattr__(self, name: str) -> object:
            self.calls.append(name)
            raise AssertionError(f"unexpected headless call: {name}")

    headless = FailOnHeadlessCall()
    program = Program(source, build_node_environment().catalog)
    state = _enable_authoring(ComposerState(
        program=program,
        program_source_path=str(source),
    ))
    before = state.draft_session.workspace_snapshot()
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(tmp_path)],
                cors_origins=["http://localhost:3000"],
            ),
        )
    )
    arguments = {
        "instance_id": "move_to_pick",
        "recovery_policy": {"allow_intervention": True, "max_retries": 1},
        "operator_stop_policy": "hold",
    }
    if invalid == "removed_policy_field":
        arguments["recovery_policy"]["allowed_skills"] = []
    if invalid == "instance_id":
        arguments["instance_id"] = "a" + ("b" * 128)

    response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(before),
            "mutation": "program_draft_update_node_policies",
            "arguments": arguments,
        },
    )

    assert response.status_code == (400 if invalid == "removed_policy_field" else 409)
    assert response.json()["detail"]["code"] == "draft_arguments_invalid"
    assert headless.calls == []
    assert state.draft_session.workspace_snapshot() == before
    assert source.read_bytes() == original_bytes


def test_examples_open_read_only_and_save_as_is_confined_to_program_workspace(
    tmp_path: Path,
) -> None:
    workspace_root = tmp_path / "program-workspace"
    outside_root = tmp_path / "outside"
    workspace_root.mkdir()
    outside_root.mkdir()
    client = _client(
        tmp_path,
        allowed_fs_roots=[str(_ROOT / "examples"), str(workspace_root)],
        allowed_program_write_roots=[str(workspace_root)],
    )

    empty = client.get("/api/program/workspace").json()
    opened_response = client.post(
        "/api/program/open-draft",
        json={
            **_fence(empty),
            "path": str(_SOURCE_PROGRAM),
            "confirmDiscard": False,
        },
    )
    assert opened_response.status_code == 200
    opened = opened_response.json()
    assert opened["saved"]["path"] == str(_SOURCE_PROGRAM.resolve())

    before_failed_save = client.get("/api/program/workspace").json()
    denied_save = client.post("/api/program/save", json=_fence(before_failed_save))
    assert denied_save.status_code == 403
    assert denied_save.json()["detail"]["code"] == "program_artifact_path_invalid"
    assert client.get("/api/program/workspace").json() == before_failed_save

    for path in (_SOURCE_PROGRAM, outside_root / "outside"):
        denied_save_as = client.post(
            "/api/program/save-as",
            json={
                **_fence(before_failed_save),
                "path": str(path),
                "confirmOverwrite": False,
            },
        )
        assert denied_save_as.status_code == 403
        assert (
            denied_save_as.json()["detail"]["code"]
            == "program_artifact_path_invalid"
        )
        assert client.get("/api/program/workspace").json() == before_failed_save

    saved = client.post(
        "/api/program/save-as",
        json={
            **_fence(before_failed_save),
            "path": str(workspace_root / "saved"),
            "confirmOverwrite": False,
        },
    )
    assert saved.status_code == 200
    assert saved.json()["saved"]["path"] == str(
        (workspace_root / "saved.program.yaml").resolve()
    )


@pytest.mark.parametrize("route", ["save", "save-as"])
@pytest.mark.parametrize(
    ("admission", "code"),
    [
        ({"authoring_allowed": False, "admission_generation": 1}, "authoring_not_ready"),
        ({"authoring_allowed": True, "admission_generation": 2}, "stale_generation"),
    ],
)
def test_refused_program_save_writes_no_artifact(
    tmp_path: Path, route: str, admission: dict, code: str
) -> None:
    """Admission precedes the artifact write, so its refusal has no effect."""

    from manyforge_composer.backend.authoring_admission import (
        AuthoringAdmissionCoordinator,
    )

    class Refusing:
        def authoring_admission(self) -> dict:
            return dict(admission)

        def acquire_authoring_transaction(self, _request):
            raise AssertionError("a refused Save takes no claim")

    source = _copy_program(tmp_path)
    state = _enable_authoring(ComposerState(allowed_fs_roots=[str(tmp_path)]))
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(tmp_path)],
                cors_origins=["http://localhost:3000"],
            ),
        )
    )
    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft", json={**_fence(empty), "path": str(source)}
    ).json()
    edited = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "tree_draft_update_node_params",
            "arguments": {
                "instance_id": "move_to_pick",
                "expected_node_id": "motion.move_to_joint_target",
                "parameters": {"planning_timeout_s": 3.0},
            },
        },
    ).json()["workspace"]
    before = client.get("/api/program/workspace").json()
    source_bytes = source.read_bytes()
    state.authoring_admission = AuthoringAdmissionCoordinator(Refusing())  # type: ignore[arg-type]

    target = tmp_path / "saved"
    refused = client.post(
        f"/api/program/{route}",
        json=(
            _fence(edited)
            if route == "save"
            else {**_fence(edited), "path": str(target), "confirmOverwrite": False}
        ),
    )

    assert refused.status_code == 409, refused.text
    assert refused.json()["detail"]["code"] == code
    assert refused.json()["detail"]["effect"] == "none"
    assert source.read_bytes() == source_bytes
    assert not (tmp_path / "saved.program.yaml").exists()
    assert client.get("/api/program/workspace").json() == before


@pytest.mark.parametrize("stale_at", ["before_write", "after_write"])
def test_save_draft_refusal_is_marked_only_before_the_write(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, stale_at: str
) -> None:
    """A Draft refusal before the write has no effect; a failed adoption after
    the write (the race) is never marked as no-effect."""

    source = _copy_program(tmp_path)
    state = _enable_authoring(ComposerState(allowed_fs_roots=[str(tmp_path)]))
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(tmp_path)],
                cors_origins=["http://localhost:3000"],
            ),
        )
    )
    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft", json={**_fence(empty), "path": str(source)}
    ).json()

    def stale(*_args, **_kwargs):
        raise DraftSessionError(
            "composer_draft_revision_stale", "Draft changed", status_code=409
        )

    monkeypatch.setattr(
        state.draft_session,
        "require_current" if stale_at == "before_write" else "install_prepared_save",
        stale,
    )
    target = tmp_path / "saved.program.yaml"
    refused = client.post(
        "/api/program/save-as",
        json={**_fence(opened), "path": str(tmp_path / "saved"), "confirmOverwrite": False},
    )

    detail = refused.json()["detail"]
    if stale_at == "before_write":
        assert refused.status_code == 409, refused.text
        assert detail["code"] == "composer_draft_revision_stale"
        assert detail["effect"] == "none"
        assert not target.exists()
    else:
        # Written, then the workspace owner contained the failed adoption: the
        # definite "written but not adopted" outcome names the written file and
        # never carries the no-effect marker.
        assert refused.status_code == 409, refused.text
        assert detail["code"] == "program_saved_baseline_not_adopted"
        assert detail["causeCode"] == "composer_workspace_install_failed"
        assert detail["path"] == str(target)
        assert str(target) in detail["message"]
        assert "effect" not in detail
        assert target.exists()


def test_explicit_invalid_program_write_roots_fail_closed_without_draft_change(
    tmp_path: Path,
) -> None:
    source = _copy_program(tmp_path)
    client = _client(
        tmp_path,
        allowed_program_write_roots=["invalid\x00root"],
    )
    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(source)},
    ).json()
    before = client.get("/api/program/workspace").json()

    rejected = client.post("/api/program/save", json=_fence(before))

    assert rejected.status_code == 503
    assert rejected.json()["detail"]["code"] == "program_artifact_roots_unavailable"
    assert client.get("/api/program/workspace").json() == before


def test_create_app_security_fallback_overrides_preseeded_write_roots(
    tmp_path: Path,
) -> None:
    read_root = tmp_path / "read-root"
    preseeded_write_root = tmp_path / "preseeded-write-root"
    read_root.mkdir()
    preseeded_write_root.mkdir()
    source = _copy_program(read_root)
    state = _enable_authoring(ComposerState(
        allowed_fs_roots=[str(preseeded_write_root)],
        allowed_program_write_roots=[str(preseeded_write_root)],
    ))
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(read_root)],
                allowed_program_write_roots=None,
                cors_origins=["http://localhost:3000"],
            ),
        )
    )

    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(source)},
    ).json()
    before = client.get("/api/program/workspace").json()

    rejected = client.post(
        "/api/program/save-as",
        json={
            **_fence(before),
            "path": str(preseeded_write_root / "bypass"),
            "confirmOverwrite": False,
        },
    )

    assert state.allowed_program_write_roots is None
    assert rejected.status_code == 403
    assert rejected.json()["detail"]["code"] == "program_artifact_path_invalid"
    assert client.get("/api/program/workspace").json() == before
    assert not (preseeded_write_root / "bypass.program.yaml").exists()


def test_dirty_open_confirmation_and_rotated_identity_are_fail_closed(
    tmp_path: Path,
) -> None:
    first = _copy_program(tmp_path, "first.program.yaml")
    second = _copy_program(tmp_path, "second.program.yaml")
    client = _client(tmp_path)
    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(first)},
    ).json()
    dirty = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "tree_draft_update_node_params",
            "arguments": {
                "instance_id": "move_to_pick",
                "expected_node_id": "motion.move_to_joint_target",
                "parameters": {"planning_timeout_s": 3.0},
            },
        },
    ).json()["workspace"]
    before = deepcopy(client.get("/api/program/workspace").json())

    rejected = client.post(
        "/api/program/open-draft",
        json={**_fence(dirty), "path": str(second), "confirmDiscard": False},
    )
    assert rejected.status_code == 409
    assert rejected.json()["detail"]["code"] == (
        "composer_draft_discard_confirmation_required"
    )
    assert client.get("/api/program/workspace").json() == before

    replaced = client.post(
        "/api/program/open-draft",
        json={**_fence(dirty), "path": str(second), "confirmDiscard": True},
    ).json()
    assert replaced["draft"]["draftId"] != dirty["draft"]["draftId"]
    stale = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(dirty),
            "mutation": "tree_draft_delete_node",
            "arguments": {"instance_id": "remove_table"},
        },
    )
    assert stale.status_code == 409
    assert stale.json()["detail"]["code"] == "composer_draft_id_stale"


def test_routes_reject_unsupported_mutation_malformed_body_and_unavailable_load(
    tmp_path: Path,
) -> None:
    client = _client(tmp_path)
    workspace = client.get("/api/program/workspace").json()
    unsupported = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(workspace),
            "mutation": "runtime_apply",
            "arguments": {},
        },
    )
    assert unsupported.status_code == 404
    assert unsupported.json()["detail"]["code"] == (
        "composer_authoring_mutation_unsupported"
    )
    malformed = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(workspace),
            "mutation": "tree_draft_insert_node",
            "arguments": {},
            "unexpected": True,
        },
    )
    assert malformed.status_code == 422
    invalid_save_as = client.post(
        "/api/program/save-as",
        json={**_fence(workspace), "path": "/", "confirmOverwrite": False},
    )
    assert invalid_save_as.status_code == 400
    assert invalid_save_as.json()["detail"]["code"] == (
        "program_artifact_path_invalid"
    )


def test_scene_and_viewer_edits_share_one_revision_fenced_program_draft(
    tmp_path: Path,
) -> None:
    source = _copy_program(tmp_path)
    client = _client(tmp_path)
    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(source)},
    ).json()

    scene_response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "scene_draft_add_object",
            "arguments": {
                "scene_resource": {
                    "scene_resource_id": "workpiece",
                    "shape": {"type": "sphere", "sphere_radius_m": 0.03},
                    "pose": {
                        "frame_ref": {"kind": "cell_frame"},
                        "position_m": [0.4, 0.0, 0.5],
                        "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
                    },
                }
            },
        },
    )
    assert scene_response.status_code == 200
    scene_workspace = scene_response.json()["workspace"]
    assert [
        row["scene_resource_id"]
        for row in scene_workspace["draft"]["program"]["scene"]
    ] == ["table", "workpiece"]

    stale = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "viewer_draft_replace",
            "arguments": {"viewer": {"frame_role_visibility": {"tcp": False}}},
        },
    )
    assert stale.status_code == 409
    assert stale.json()["detail"]["code"] == "composer_draft_revision_stale"

    viewer_response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(scene_workspace),
            "mutation": "viewer_draft_replace",
            "arguments": {
                "viewer": {
                    "frame_role_visibility": {"tcp": False},
                    "frame_gizmo_size_m": 0.15,
                    "object_axes_gizmo_size_m": 0.1,
                }
            },
        },
    )
    assert viewer_response.status_code == 200
    viewer_workspace = viewer_response.json()["workspace"]
    assert viewer_workspace["draft"]["program"]["viewer"] == {
        "frame_role_visibility": {"tcp": False},
        "frame_gizmo_size_m": 0.15,
        "object_axes_gizmo_size_m": 0.1,
    }
    assert viewer_workspace["applied"]["available"] is False


def test_artifact_roots_symlinks_overwrite_confirmation_and_atomic_failure(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    allowed = tmp_path / "allowed"
    outside = tmp_path / "outside"
    allowed.mkdir()
    outside.mkdir()
    outside_program = _copy_program(outside, "outside.program.yaml")
    symlink = allowed / "linked.program.yaml"
    symlink.symlink_to(outside_program)
    catalog = build_node_environment().catalog

    with pytest.raises(ProgramArtifactError) as escaped:
        load_program_artifact(
            str(symlink),
            allowed_roots=[str(allowed)],
            catalog=catalog,
        )
    assert escaped.value.status_code == 403

    program = Program(outside_program, catalog)
    existing = allowed / "existing.program.yaml"
    existing.write_text("sentinel", encoding="utf-8")
    with pytest.raises(ProgramArtifactError) as confirmation:
        save_program_artifact(
            program,
            str(existing),
            allowed_roots=[str(allowed)],
            overwrite=False,
        )
    assert confirmation.value.code == (
        "program_artifact_overwrite_confirmation_required"
    )
    assert existing.read_text(encoding="utf-8") == "sentinel"

    def fail_replace(*_args: object, **_kwargs: object) -> None:
        raise OSError("injected replace failure")

    monkeypatch.setattr(program_artifacts.os, "replace", fail_replace)
    with pytest.raises(ProgramArtifactError) as failed:
        save_program_artifact(
            program,
            str(existing),
            allowed_roots=[str(allowed)],
            overwrite=True,
        )
    assert failed.value.code == "program_artifact_write_failed"
    assert existing.read_text(encoding="utf-8") == "sentinel"
    assert list(allowed.glob(".*.tmp-*")) == []


# --- V13.5 formula checks and path-keyed value sources --------------------


def _expression_program(tmp_path: Path) -> Path:
    import json

    document = Program(_SOURCE_PROGRAM, build_node_environment().catalog).to_dict()

    def parameter(parameter_id: str, value_type: str, default) -> dict:
        return {
            "parameter_id": parameter_id,
            "type": value_type,
            "default": default,
        }

    def key(name: str, value_type: str, **extra) -> dict:
        return {
            "key": name,
            "type": value_type,
            "access": "read_write",
            "persistence": "run",
            **extra,
        }

    document["parameters"] = sorted(
        [
            *document["parameters"],
            parameter("grasp_offset", "float64", 0.01),
            parameter("tube_length", "float64", 0.2),
        ],
        key=lambda row: row["parameter_id"],
    )
    document["blackboard"] = {
        "namespaces": [],
        "keys": [
            key("approach_z", "float64", access="read",
                expression="key.grasp_z + 0.05"),
            key("count", "int64"),
            key("grasp_z", "float64", access="read",
                expression="-param.tube_length / 2 + param.grasp_offset"),
            key("index", "int64", initial=3),
            key("names", "string_list"),
            key("place", "stored_pose"),
        ],
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    target = tmp_path / "formulas.program.yaml"
    target.write_text(json.dumps(document), encoding="utf-8")
    return target


def _check(client: TestClient, expression: str, destination: str = "float64", **extra):
    return client.post(
        "/api/program/draft/expression-check",
        json={"expression": expression, "destination_type": destination, **extra},
    )


def test_expression_check_reports_type_preview_and_names(tmp_path: Path) -> None:
    client = _client(tmp_path)
    missing = _check(client, "1.0")
    assert missing.status_code == 404
    assert missing.json()["detail"]["code"] == "composer_draft_unavailable"

    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(_expression_program(tmp_path))},
    )
    assert opened.status_code == 200, opened.json()

    reply = _check(client, "key.approach_z + key.index * 0.5")
    assert reply.status_code == 200
    body = reply.json()
    assert body["schema_version"] == "manyforge.expression_check.v1"
    assert body["result_type"] == "float64"
    assert body["diagnostics"] == []
    # Preview from authoring values: -0.2 / 2 + 0.01 + 0.05 + 3 * 0.5.
    assert body["preview"]["value"] == pytest.approx(1.46)
    assert body["preview"]["unavailable_reason"] is None
    names = {row["name"]: row["type"] for row in body["names"]}
    assert names["param.tube_length"] == "float64"
    assert names["key.place"] == "stored_pose"
    assert names["key.approach_z"] == "float64"
    assert "key.names" not in names  # string_list cannot be read

    unavailable = _check(client, "key.count + 1", "int64").json()
    assert unavailable["result_type"] == "int64"
    assert unavailable["preview"] == {
        "value": None,
        "unavailable_reason": "key.count has no authoring value",
    }

    errors = _check(client, "key.names + 1").json()
    assert errors["result_type"] is None
    assert errors["diagnostics"][0] == {
        "code": "unknown_name",
        "message": "key.names has type string_list, which expressions cannot read",
        "position": 0,
        "length": 9,
        "name": "key.names",
    }
    assert errors["preview"]["value"] is None

    syntax = _check(client, "(1 +", "int64").json()
    assert syntax["diagnostics"][0]["position"] == 4

    mismatch = _check(client, "key.index > 1", "float64").json()
    assert mismatch["diagnostics"][0]["code"] == "destination_type_mismatch"

    invalid = client.post(
        "/api/program/draft/expression-check",
        json={"expression": "1", "destination_type": "pose"},
    )
    assert invalid.status_code == 422


def test_expression_check_excludes_the_edited_computed_key_and_dependents(
    tmp_path: Path,
) -> None:
    client = _client(tmp_path)
    empty = client.get("/api/program/workspace").json()
    client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(_expression_program(tmp_path))},
    )
    body = _check(
        client, "key.approach_z + 1", exclude_computed_key="grasp_z"
    ).json()
    # approach_z reads grasp_z, so it would close a cycle.
    assert body["diagnostics"][0]["code"] == "unknown_name"
    assert "depends on it" in body["diagnostics"][0]["message"]
    names = {row["name"] for row in body["names"]}
    assert {"key.grasp_z", "key.approach_z"}.isdisjoint(names)
    assert "key.index" in names

    refused = _check(client, "1.0", exclude_computed_key="index")
    assert refused.status_code == 422
    assert refused.json()["detail"]["code"] == "expression_check_exclusion_invalid"


def test_merged_literal_replaces_sources_on_the_parameter_but_full_edits_keep_both(
    tmp_path: Path,
) -> None:
    client = _client(tmp_path)
    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(_expression_program(tmp_path))},
    ).json()
    opened_node = None
    draft = client.get("/api/program/draft").json()
    for candidate in draft["authoring"]["forest"][0]["children"]:
        if candidate["instance_id"] == "move_to_pick":
            opened_node = candidate
    assert opened_node is not None
    sources = {
        **opened_node.get("value_sources", {}),
        "goal[0].position_rad": {
            "kind": "expression",
            "expression": "key.grasp_z * 2",
        },
    }
    full = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "tree_draft_update_node_params",
            "arguments": {
                "instance_id": "move_to_pick",
                "expected_node_id": "motion.move_to_joint_target",
                "merge": False,
                "parameters": opened_node["parameters"],
                "value_sources": dict(sorted(sources.items())),
            },
        },
    )
    assert full.status_code == 200, full.json()
    workspace = full.json()["workspace"]
    draft = client.get("/api/program/draft").json()
    assert draft["candidate_available"] is True, draft["diagnostics"]
    node = draft["authoring"]["forest"][0]["children"][0]
    assert "goal[0].position_rad" in node["value_sources"]
    assert node["parameters"]["goal"] == opened_node["parameters"]["goal"]

    merged = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(workspace),
            "mutation": "tree_draft_update_node_params",
            "arguments": {
                "instance_id": "move_to_pick",
                "expected_node_id": "motion.move_to_joint_target",
                "parameters": {"goal": opened_node["parameters"]["goal"]},
            },
        },
    )
    assert merged.status_code == 200, merged.json()
    node = client.get("/api/program/draft").json()["authoring"]["forest"][0][
        "children"
    ][0]
    assert "goal[0].position_rad" not in node["value_sources"]
    assert "velocity_scale" in node["value_sources"]


def test_workspace_reports_the_names_each_draft_formula_reads(tmp_path: Path) -> None:
    client = _client(tmp_path)
    empty = client.get("/api/program/workspace").json()
    assert empty["draft"]["formulaReferences"] == {"computedKeys": {}, "nodes": {}}
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(_expression_program(tmp_path))},
    ).json()
    computed = {
        "approach_z": ["key.grasp_z"],
        "grasp_z": ["param.grasp_offset", "param.tube_length"],
    }
    assert opened["draft"]["formulaReferences"] == {
        "computedKeys": computed,
        "nodes": {},
    }

    node = client.get("/api/program/draft").json()["authoring"]["forest"][0][
        "children"
    ][0]
    assert node["instance_id"] == "move_to_pick"
    sources = {
        **node.get("value_sources", {}),
        "goal[0].position_rad": {
            "kind": "expression",
            "expression": "key.grasp_z * 2 + param.tube_length + key.grasp_z",
        },
        # Does not check (string_list cannot be read): reported as reading nothing.
        "goal[1].position_rad": {"kind": "expression", "expression": "key.names + 1"},
    }
    edited = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "tree_draft_update_node_params",
            "arguments": {
                "instance_id": "move_to_pick",
                "expected_node_id": "motion.move_to_joint_target",
                "merge": False,
                "parameters": node["parameters"],
                "value_sources": dict(sorted(sources.items())),
            },
        },
    )
    assert edited.status_code == 200, edited.json()
    draft = edited.json()["workspace"]["draft"]
    # The Draft has no candidate, yet its formulas that check are reported.
    assert draft["candidateAvailable"] is False
    expected = {
        "computedKeys": computed,
        "nodes": {
            "move_to_pick": {
                "goal[0].position_rad": ["key.grasp_z", "param.tube_length"],
            },
        },
    }
    assert draft["formulaReferences"] == expected
    snapshot = client.get("/api/authoring/workspace").json()
    assert snapshot["program"]["formulaReferences"] == expected


def _reset_apply_context(**changes) -> dict:
    return {
        "schemaVersion": "manyforge.composer_reset_apply_context.v1",
        "correlationId": "reset-apply.1",
        "document": "program",
        "programRunId": "program-run.7",
        "programRunnerOwnerEpoch": "program-runner.epoch.1",
        "cycleGeneration": 7,
        "draftId": "draft-1",
        "draftRevision": 3,
        "draftContentRevision": "sha256:" + "a" * 64,
        "appliedRevision": "sha256:" + "b" * 64,
        **changes,
    }


def test_reset_apply_context_is_bounded_logged_once_and_grants_nothing(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    client = _client(tmp_path)
    before = client.get("/api/program/workspace").json()
    with caplog.at_level("INFO", logger="manyforge_composer.reset_apply"):
        recorded = client.post(
            "/api/authoring/reset-apply-contexts", json=_reset_apply_context()
        )
    assert recorded.status_code == 200, recorded.text
    assert recorded.json() == {"correlationId": "reset-apply.1", "recorded": True}
    [record] = [row for row in caplog.records if row.msg == "reset_apply_context"]
    assert record.reset_apply_correlation_id == "reset-apply.1"
    assert (record.document, record.program_run_id, record.cycle_generation) == (
        "program", "program-run.7", 7,
    )
    assert record.draft_revision == 3
    # Metadata only: no Draft, workspace or runtime change.
    assert client.get("/api/program/workspace").json() == before

    for invalid in (
        _reset_apply_context(document="scene"),
        _reset_apply_context(cycleGeneration=-1),
        _reset_apply_context(draftId=""),
        {**_reset_apply_context(), "authority": "reset"},
    ):
        assert client.post(
            "/api/authoring/reset-apply-contexts", json=invalid
        ).status_code == 422


@pytest.mark.parametrize("correlation", [None, "reset-apply.1"])
def test_program_apply_preserves_a_shared_reset_apply_correlation(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, correlation: str | None
) -> None:
    program = Program(_copy_program(tmp_path), build_node_environment().catalog)
    state = _enable_authoring(ComposerState(program=program))
    submitted: list[dict] = []

    class Admission(_AuthoringPassThrough):
        def execute_external_submission(
            self, _envelope, *, validate_under_claim, submit, **_kwargs
        ):
            validate_under_claim({})
            return submit({})

    state.authoring_admission = Admission()  # type: ignore[assignment]
    closure = {"closure": "fixture"}
    monkeypatch.setattr(state, "published_scenario", lambda: {"published_scenario": {}})
    monkeypatch.setattr(state, "applied_activation_for_publication", lambda _p: {
        "deployment": {"components": []},
        "program_role_assignment": {"component_roles": [], "target_roles": []},
        "source_document_digest": "sha256:" + "b" * 64,
        "scenario_id": "cell",
        "scenario_project_path": "/projects/cell.scenario",
        "project_path": str(tmp_path),
        "model_source_closure": closure,
    })
    monkeypatch.setattr(
        routes_draft, "capture_deployment_model_sources", lambda *_a, **_k: closure
    )
    monkeypatch.setattr(routes_draft, "_active_catalog", lambda s: s.node_catalog)
    monkeypatch.setattr(routes_draft, "_workspace", lambda _s: {})
    monkeypatch.setattr(state, "_manager_current", lambda _s: ("bundle", "graph", "epoch"))

    def execute(intent, claim, *, apply, prepared=None, prepare_only=False):
        if prepare_only:
            return {"confirmation_required": False, "prepared_activation": {}}
        submitted.append(intent)
        return {"status": "accepted", "apply_id": "apply-1", "apply_generation": 4}

    monkeypatch.setattr(state, "execute_authored_deployment_candidate", execute)
    client = TestClient(create_app(state, security=SecurityConfig(
        allowed_fs_roots=[str(tmp_path)], cors_origins=["http://localhost:3000"],
    )))
    # Only the Manager read the activation needs; the app routes no headless.
    # The Applied Program is unreadable here, which never proves a no-op.
    def program_applied():
        raise HeadlessClientError("headless_http_timeout", "timed out")

    state.headless_client = SimpleNamespace(  # type: ignore[assignment]
        manager_status=lambda: {}, program_applied=program_applied,
    )
    body = _fence({"draft": {
        "draftId": state.draft_session.draft_id,
        "draftRevision": state.draft_session.revision,
    }})
    if correlation is not None:
        body["correlationId"] = correlation
    response = client.post("/api/program/apply", json=body)

    assert response.status_code == 202, response.text
    assert submitted[0]["scene_initialization"] == "preserve"
    assert "typed_parameter_overrides" not in submitted[0]
    # The Manager record carries the shared reference with its own request ID.
    assert submitted[0]["correlation_id"] == (correlation or body["requestId"])
    assert submitted[0]["manager_request_id"].startswith("program-activation-")




def test_program_apply_refuses_a_no_op_before_any_effect(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    program = Program(_copy_program(tmp_path), build_node_environment().catalog)
    state = _enable_authoring(ComposerState(program=program))

    class Admission(_AuthoringPassThrough):
        def execute_external_submission(self, *_args, **_kwargs):
            pytest.fail("a no-op Apply must not reach admission")

    state.authoring_admission = Admission()  # type: ignore[assignment]

    def prepare(*_args, **_kwargs):
        pytest.fail("a no-op Apply must not prepare an activation")

    monkeypatch.setattr(state, "execute_authored_deployment_candidate", prepare)
    monkeypatch.setattr(routes_draft, "_active_catalog", lambda s: s.node_catalog)
    # The Applied Program already is exactly the Draft.
    monkeypatch.setattr(routes_draft, "_applied_context", lambda _s: (
        SimpleNamespace(program_revision=program.program_revision), None,
    ))
    client = TestClient(create_app(state, security=SecurityConfig(
        allowed_fs_roots=[str(tmp_path)], cors_origins=["http://localhost:3000"],
    )))
    state.headless_client = SimpleNamespace(  # type: ignore[assignment]
        manager_status=lambda: {}
    )
    body = _fence({"draft": {
        "draftId": state.draft_session.draft_id,
        "draftRevision": state.draft_session.revision,
    }})
    response = client.post("/api/program/apply", json=body)

    assert response.status_code == 409, response.text
    assert response.json()["detail"]["code"] == "program_apply_nothing_to_apply"
    assert response.json()["detail"]["message"].startswith("Nothing to apply")


def test_program_revert_refuses_saved_equal_to_applied_before_any_effect(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    source = _copy_program(tmp_path)
    program = Program(source, build_node_environment().catalog)
    state = _enable_authoring(
        ComposerState(program=program, program_source_path=str(source))
    )

    class Admission(_AuthoringPassThrough):
        def execute_external_submission(self, *_args, **_kwargs):
            pytest.fail("a no-op Revert must not reach admission")

    state.authoring_admission = Admission()  # type: ignore[assignment]

    def submit(*_args, **_kwargs):
        pytest.fail("a no-op Revert must not submit a Manager activation")

    monkeypatch.setattr(state, "execute_authored_deployment_candidate", submit)
    monkeypatch.setattr(routes_draft, "_active_catalog", lambda s: s.node_catalog)
    # Saved is the loaded file and the Applied Program already is exactly it.
    monkeypatch.setattr(routes_draft, "_applied_context", lambda _s: (
        SimpleNamespace(program_revision=program.program_revision), None,
    ))
    client = TestClient(create_app(state, security=SecurityConfig(
        allowed_fs_roots=[str(tmp_path)], cors_origins=["http://localhost:3000"],
    )))
    state.headless_client = SimpleNamespace(  # type: ignore[assignment]
        manager_status=lambda: pytest.fail("a no-op Revert must not read the Manager")
    )
    before = state.draft_session.retained_state()
    body = _fence({"draft": {
        "draftId": state.draft_session.draft_id,
        "draftRevision": state.draft_session.revision,
    }})
    response = client.post("/api/program/revert", json=body)

    assert response.status_code == 409, response.text
    assert response.json()["detail"]["code"] == "program_revert_nothing_to_revert"
    assert response.json()["detail"]["message"].startswith("Nothing to revert")
    assert state.draft_session.retained_state() == before


def _deployment(name: str, revision: str | None = None) -> dict:
    document = {"deployment_artifact_id": "cell", "name": name}
    if revision is not None:
        document["deployment_revision"] = revision
    return document


def _compile_as_is(source: str) -> CompiledDeploymentDocument:
    document = json.loads(source)
    return CompiledDeploymentDocument(
        document=document,
        canonical_json=source,
        deployment_artifact_id=document["deployment_artifact_id"],
        deployment_revision=document.get("deployment_revision"),
        graph_revision=None,
    )


def _program_revert_scenario(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    *,
    saved_deployment: dict,
    draft_deployment: dict,
    applied_deployment: dict,
) -> tuple[TestClient, ComposerState, list[str]]:
    """Saved Program differs from Applied, both Drafts edited, Manager recorded."""

    source = _copy_program(tmp_path)
    program = Program(source, build_node_environment().catalog)
    state = _enable_authoring(
        ComposerState(program=program, program_source_path=str(source))
    )
    deployment = DeploymentDraftSession(compiler=_compile_as_is)
    retained = deployment.retained_state()
    retained.update(
        draft_revision=1, document=draft_deployment, saved_document=saved_deployment
    )
    deployment.restore_retained_state(retained)
    state.deployment_draft_session = deployment
    state.authoring_workspace = AuthoringWorkspace(
        program_session=state.draft_session, deployment_session=deployment
    )
    edited = state.draft_session.retained_state()
    edited["authoring"]["description"] = "Edited only in the Draft."
    edited["draft_revision"] += 1
    candidate, edited["diagnostics"] = derive_candidate(
        edited["authoring"], state.node_catalog
    )
    edited["program"] = candidate.to_dict() if candidate is not None else None
    state.authoring_workspace.commit_program(
        draft_id=edited["draft_id"],
        install=lambda: state.draft_session.install_prepared_replacement(
            state.draft_session.prepare_load_replacement(edited, state.node_catalog),
            expected_draft_id=edited["draft_id"],
            expected_revision=edited["draft_revision"] - 1,
        ),
    )
    manager_calls: list[str] = []

    class Admission(_AuthoringPassThrough):
        def execute_external_submission(
            self, _envelope, *, validate_under_claim, submit,
            commit_before_submit=None, **_kwargs,
        ):
            validate_under_claim({})
            if commit_before_submit is not None:
                commit_before_submit()
            return submit({})

    state.authoring_admission = Admission()  # type: ignore[assignment]
    closure = {"closure": "fixture"}
    monkeypatch.setattr(state, "published_scenario", lambda: {"published_scenario": {}})
    monkeypatch.setattr(state, "applied_activation_for_publication", lambda _p: {
        "deployment": deepcopy(applied_deployment),
        "program_role_assignment": {"component_roles": [], "target_roles": []},
        "source_document_digest": "sha256:" + "b" * 64,
        "scenario_id": "cell",
        "scenario_project_path": "/projects/cell.scenario",
        "project_path": str(tmp_path),
        "model_source_closure": closure,
    })
    monkeypatch.setattr(
        routes_draft, "capture_deployment_model_sources", lambda *_a, **_k: closure
    )
    monkeypatch.setattr(routes_draft, "_active_catalog", lambda s: s.node_catalog)
    monkeypatch.setattr(routes_draft, "_workspace", lambda _s: {})
    # The Applied Program is another revision, so the Saved one is reverted to.
    monkeypatch.setattr(routes_draft, "_applied_context", lambda _s: (
        SimpleNamespace(program_revision="sha256:" + "c" * 64), None,
    ))
    monkeypatch.setattr(state, "_manager_current", lambda _s: ("bundle", "graph", "epoch"))

    def execute(intent, claim, *, apply, prepared=None, prepare_only=False):
        manager_calls.append("prepare_only" if prepare_only else "apply")
        if prepare_only:
            return {"confirmation_required": False, "prepared_activation": {}}
        return {"status": "accepted", "apply_id": "apply-1", "apply_generation": 4}

    monkeypatch.setattr(state, "execute_authored_deployment_candidate", execute)
    client = TestClient(create_app(state, security=SecurityConfig(
        allowed_fs_roots=[str(tmp_path)], cors_origins=["http://localhost:3000"],
    )))
    # Only the Manager read the activation needs; the app routes no headless.
    state.headless_client = SimpleNamespace(  # type: ignore[assignment]
        manager_status=lambda: manager_calls.append("manager_status") or {},
    )
    return client, state, manager_calls


@pytest.mark.parametrize(
    ("saved", "applied"),
    [
        # Revisions decide when both carry one.
        (_deployment("Cell", "sha256:" + "1" * 64), _deployment("Cell", "sha256:" + "2" * 64)),
        # Without revision fields the documents decide.
        (_deployment("Cell"), _deployment("Other cell")),
    ],
)
def test_program_revert_refuses_a_saved_deployment_other_than_applied_before_any_effect(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, saved: dict, applied: dict
) -> None:
    client, state, manager_calls = _program_revert_scenario(
        tmp_path, monkeypatch,
        saved_deployment=saved, draft_deployment=saved, applied_deployment=applied,
    )
    program_before = state.draft_session.retained_state()
    deployment_before = state.deployment_draft_session.retained_state()
    revision_before = state.authoring_workspace.identity().workspace_revision
    body = _fence({"draft": {
        "draftId": state.draft_session.draft_id,
        "draftRevision": state.draft_session.revision,
    }})
    response = client.post("/api/program/revert", json=body)

    assert response.status_code == 409, response.text
    assert response.json()["detail"]["code"] == "scenario_revert_requires_deployment"
    assert response.json()["detail"]["effect"] == "none"
    # Refused before the Manager preview: no Manager call at all.
    assert manager_calls == []
    assert state.draft_session.retained_state() == program_before
    assert state.deployment_draft_session.retained_state() == deployment_before
    assert state.authoring_workspace.identity().workspace_revision == revision_before


def test_program_revert_restores_both_drafts_to_saved_in_one_workspace_revision(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    saved = _deployment("Cell", "sha256:" + "1" * 64)
    client, state, manager_calls = _program_revert_scenario(
        tmp_path, monkeypatch,
        saved_deployment=saved,
        draft_deployment=_deployment("Edited cell", "sha256:" + "3" * 64),
        applied_deployment=saved,
    )
    saved_description = state.draft_session.retained_state()["saved_document"][
        "description"
    ]
    revision_before = state.authoring_workspace.identity().workspace_revision
    body = _fence({"draft": {
        "draftId": state.draft_session.draft_id,
        "draftRevision": state.draft_session.revision,
    }})
    response = client.post("/api/program/revert", json=body)

    assert response.status_code == 202, response.text
    assert manager_calls == ["manager_status", "prepare_only", "apply"]
    # The Deployment Draft that left Saved returns to it together with the
    # Program restore, in one workspace revision.
    assert state.deployment_draft_session.retained_state()["document"] == saved
    assert state.draft_session.retained_state()["authoring"]["description"] == (
        saved_description
    )
    assert state.authoring_workspace.identity().workspace_revision == revision_before + 1


def test_program_revert_partial_contained_install_keeps_the_claim(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from manyforge_composer.backend.authoring_admission import (
        AuthoringAdmissionCoordinator,
    )
    from manyforge_composer.tests.test_authoring_local_mutation import _Headless

    saved = _deployment("Cell", "sha256:" + "1" * 64)
    client, state, manager_calls = _program_revert_scenario(
        tmp_path, monkeypatch,
        saved_deployment=saved,
        draft_deployment=_deployment("Edited cell", "sha256:" + "3" * 64),
        applied_deployment=saved,
    )
    headless = _Headless(generation=1)
    coordinator = AuthoringAdmissionCoordinator(headless)  # type: ignore[arg-type]
    coordinator.recover()
    state.authoring_admission = coordinator

    def fail_after_program_restore(_identity):
        raise RuntimeError("Deployment restore failed")

    # The Program restore installs first; the Deployment restore then fails.
    monkeypatch.setattr(
        routes_draft, "saved_deployment_restore", lambda _s: fail_after_program_restore
    )
    body = _fence({"draft": {
        "draftId": state.draft_session.draft_id,
        "draftRevision": state.draft_session.revision,
    }})
    response = client.post("/api/program/revert", json=body)

    assert response.status_code == 503, response.text
    assert response.json()["detail"]["code"] == "composer_workspace_install_failed"
    assert "effect" not in response.json()["detail"]
    assert state.authoring_workspace.failed_closed is not None
    assert "apply" not in manager_calls
    # Never settled as proven no effect: the claim is retained, admission blocked.
    assert headless.settlements == []
    assert coordinator.local_claim_pending is True
    assert coordinator.mutation_ready is False
