"""Focused Composer joint-position authority tests."""

from __future__ import annotations

import json
import threading
import time
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

pytest.importorskip("fastapi")

from fastapi import HTTPException
from manyforge_behavior.cycle_manager import InterventionSession
from manyforge_behavior.kernel_client import (
    ExecutionSnapshot,
    GripperCommandState,
    MockKernelClient,
)
from manyforge_composer.backend import routes_scene, routes_urdf
from manyforge_composer.backend.models import (
    ApplySessionRequest,
    CartesianJogRequest,
    InterventionExecuteRequest,
    JointJogRequest,
    SceneRobotResponse,
    UpdateTreeRequest,
    VisualSceneSnapshotResponse,
)
from manyforge_composer.backend.routes_direct import create_direct_router
from manyforge_composer.backend.routes_program import create_program_router
from manyforge_composer.backend.routes_runtime import _execution_profile
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.backend.viewer_data_plane import ViewerSnapshotBuild
from manyforge_composer.backend.visual_state import VisualFkModelDescriptor
from manyforge_composer.tests.node_environment_support import (
    node_environment_for_catalog,
)


_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(
    _REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml"
)


def _state(*, joint_names: list[str] | None = None) -> ComposerState:
    return ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(joint_names=joint_names or ["joint1", "joint2"]),
    )


def _deployment(
    *,
    adapter_id: str = "execution.ros2_control_follow_joint_trajectory",
    profile: str | None = "observed_robot",
    max_age_ms: int = 250,
    initial: list[float] | None = None,
) -> Any:
    config: dict[str, Any] = {"joint_state_max_age_ms": max_age_ms}
    if profile is not None:
        config["execution_state_authority_profile"] = profile
    if initial is not None:
        config["initial_joint_positions_rad"] = list(initial)
    return SimpleNamespace(
        adapters={
            "execution": SimpleNamespace(
                adapter_id=adapter_id,
                config=config,
            )
        },
        robot_config=SimpleNamespace(robot_id="robot", group_id="arm"),
        grippers={},
        capability_keys=lambda: None,
    )


def _observed_snapshot(
    positions: list[float],
    *,
    source: str = "observed_ros2",
    timestamp_ns: int = 123_000_000,
    age_ns: int = 10_000_000,
) -> ExecutionSnapshot:
    return ExecutionSnapshot(
        joint_positions_rad=list(positions),
        state_source=source,
        source_timestamp_ns=timestamp_ns,
        source_age_ns=age_ns,
    )


@pytest.mark.parametrize(
    ("adapter_id", "profile", "measurement_authoritative"),
    [
        ("execution.native", "observed_ros2", True),
        ("execution.native", "observed_robot", True),
        ("execution.ros2_control_follow_joint_trajectory", None, True),
        ("execution.ros2_control_follow_joint_trajectory", "real_robot", True),
        ("execution.ros2_control_follow_joint_trajectory", "commanded_sim", False),
        ("execution.ros2_control_follow_joint_trajectory", "authored_preview", False),
        ("execution.native", "native", False),
    ],
)
def test_authority_profile_comes_from_active_execution_adapter(
    adapter_id: str,
    profile: str | None,
    measurement_authoritative: bool,
) -> None:
    state = _state()
    state.deployment = _deployment(adapter_id=adapter_id, profile=profile)

    resolved = state.joint_state_authority_profile()

    assert resolved.measurement_authoritative is measurement_authoritative
    assert resolved.max_source_age_ns == 250_000_000


def test_artifact_read_transaction_is_reentrant_and_serializes_readers() -> None:
    state = _state()
    attempting = threading.Event()
    entered = threading.Event()

    def second_reader() -> None:
        attempting.set()
        with state.artifact_read_transaction():
            entered.set()

    with state.artifact_read_transaction():
        with state.artifact_read_transaction():
            thread = threading.Thread(target=second_reader)
            thread.start()
            assert attempting.wait(timeout=1.0) is True
            assert entered.is_set() is False
    thread.join(timeout=1.0)

    assert entered.is_set()
    assert thread.is_alive() is False


def test_observed_motion_rejects_missing_or_stale_state_despite_cache_and_initial() -> None:
    state = _state()
    state.record_current_joint_positions([9.0, 9.0])
    state.deployment = _deployment(initial=[8.0, 8.0])

    snapshots = iter(
        [
            ExecutionSnapshot(joint_positions_rad=[]),
            _observed_snapshot([1.0, 2.0], age_ns=250_000_001),
            _observed_snapshot([1.0], age_ns=0),
        ]
    )
    state.kernel.get_execution_snapshot = lambda: next(snapshots)  # type: ignore[method-assign]

    assert state.get_current_joint_positions() is None
    assert state.get_current_joint_positions() is None
    assert state.get_current_joint_positions() is None


def test_observed_motion_accepts_exact_complete_fresh_snapshot() -> None:
    state = _state()
    state.record_current_joint_positions([9.0, 9.0])
    state.deployment = _deployment(initial=[8.0, 8.0])
    snapshot = _observed_snapshot([0.125, -0.375], age_ns=249_999_999)
    state.kernel.get_execution_snapshot = lambda: snapshot  # type: ignore[method-assign]

    assert state.get_current_joint_positions() == [0.125, -0.375]


def test_display_retains_positions_and_observed_metadata_atomically_when_stale() -> None:
    state = _state()
    state.deployment = _deployment()
    fresh = _observed_snapshot(
        [0.25, -0.5],
        timestamp_ns=456_000_000,
        age_ns=10_000_000,
    )

    accepted = state.resolve_joint_position_authority(
        fresh,
        joint_names=["joint1", "joint2"],
        for_display=True,
    )
    unrelated = ExecutionSnapshot(
        joint_positions_rad=[7.0, 7.0],
        state_source="commanded_sim_predicted",
        source_timestamp_ns=999_000_000,
        source_age_ns=1,
    )
    retained = state.resolve_joint_position_authority(
        unrelated,
        joint_names=["joint1", "joint2"],
        for_display=True,
    )

    assert accepted.positions_list() == [0.25, -0.5]
    assert retained.positions_list() == [0.25, -0.5]
    assert retained.state_source == "observed_ros2"
    assert retained.source_timestamp_ns == 456_000_000
    assert retained.source_age_ns is not None
    assert retained.source_age_ns >= 10_000_000
    assert retained.stale is True
    assert routes_scene._execution_authority_metadata(retained) == {
        "state_source": "observed_ros2",
        "source_timestamp_ns": 456_000_000,
        "source_age_ns": retained.source_age_ns,
    }


def test_visual_hmi_projects_stale_observed_pose_and_matching_provenance(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    state = _state(joint_names=["joint1", "joint2"])
    state.deployment = _deployment()
    state.program = SimpleNamespace(
        robot=SimpleNamespace(
            base_frame="base_link",
            tcp_frame="tool0",
            base_pose={},
        ),
        scene=[],
    )
    snapshot = _observed_snapshot(
        [0.4, -0.6],
        timestamp_ns=777_000_000,
        age_ns=300_000_000,
    )
    state.kernel.get_execution_snapshot = lambda: snapshot  # type: ignore[method-assign]
    monkeypatch.setattr(
        routes_urdf,
        "active_urdf_model_key",
        lambda _state: "authority-test-model",
    )
    monkeypatch.setattr(
        routes_urdf,
        "visual_link_names_for_fk",
        lambda _state: ("base_link", "tool0"),
    )
    monkeypatch.setattr(routes_scene, "_load_model_frame_names", lambda _state: [])
    monkeypatch.setattr(
        routes_scene,
        "visual_fk_model_descriptor",
        lambda _state: VisualFkModelDescriptor(
            model_key="authority-test-model",
            canonical_urdf_path="/tmp/authority-test.urdf",
            authority_metadata={},
        ),
    )
    worker_payloads: list[dict[str, Any]] = []

    def build_without_fk(
        payload: dict[str, Any],
        *,
        input_signature: tuple[Any, ...],
        sample_duration_ms: float,
        sample_stage_durations_ms: dict[str, float] | None = None,
    ) -> ViewerSnapshotBuild:
        del sample_duration_ms
        worker_payloads.append(payload)
        return ViewerSnapshotBuild(
            snapshot=VisualSceneSnapshotResponse(
                sequence_number=1,
                timestamp_ns=time.monotonic_ns(),
                stale=bool(payload["stale"]),
                robot=SceneRobotResponse.model_validate(payload["robot"]),
            ),
            input_signature=input_signature,
            stage_durations_ms=dict(sample_stage_durations_ms or {}),
            object_revision=int(payload["object_revision"]),
        )

    state.build_visual_state_in_worker = build_without_fk  # type: ignore[method-assign]

    built = routes_scene._build_visual_state_snapshot_for_publisher(state)

    assert worker_payloads[0]["visual_joint_positions"] == [0.4, -0.6]
    assert worker_payloads[0]["stale"] is True
    assert built.snapshot.stale is True
    assert built.snapshot.robot is not None
    assert built.snapshot.robot.joint_positions == {
        "joint1": 0.4,
        "joint2": -0.6,
    }
    assert built.snapshot.robot.state_source == "observed_ros2"
    assert built.snapshot.robot.source_timestamp_ns == 777_000_000
    assert built.snapshot.robot.source_age_ns == 300_000_000


def test_authored_lane_preserves_snapshot_cache_and_initial_fallbacks() -> None:
    state = _state()
    state.deployment = _deployment(
        adapter_id="execution.native",
        profile="native",
        initial=[0.3, -0.2],
    )

    initial = state.resolve_joint_position_authority(
        ExecutionSnapshot(joint_positions_rad=[]),
        joint_names=["joint1", "joint2"],
        for_display=False,
    )
    snapshot = state.resolve_joint_position_authority(
        ExecutionSnapshot(
            joint_positions_rad=[0.1, 0.2],
            state_source="authored_preview",
            source_timestamp_ns=123,
            source_age_ns=0,
        ),
        joint_names=["joint1", "joint2"],
        for_display=False,
    )
    cached = state.resolve_joint_position_authority(
        ExecutionSnapshot(joint_positions_rad=[]),
        joint_names=["joint1", "joint2"],
        for_display=False,
    )

    assert initial.positions_list() == [0.3, -0.2]
    assert initial.provenance == "deployment_initial"
    assert snapshot.positions_list() == [0.1, 0.2]
    assert snapshot.state_source == "authored_preview"
    assert cached.positions_list() == [0.1, 0.2]
    assert cached.provenance == "backend_cache"
    assert cached.state_source is None


def test_runtime_cutover_preserves_matching_backend_cache_only_when_requested() -> None:
    state = _state()
    state.deployment = _deployment(
        adapter_id="execution.native",
        profile="native",
        initial=[0.3, -0.2],
    )
    state.record_current_joint_positions([0.7, -0.6])

    state._commit_joint_state_runtime_cutover(  # noqa: SLF001
        ["joint1", "joint2"],
        preserve_backend_state=True,
    )
    preserved = state.resolve_joint_position_authority(
        ExecutionSnapshot(joint_positions_rad=[]),
        joint_names=["joint1", "joint2"],
        for_display=False,
    )

    state._commit_joint_state_runtime_cutover(  # noqa: SLF001
        ["joint1", "joint2"],
        preserve_backend_state=False,
    )
    replacement = state.resolve_joint_position_authority(
        ExecutionSnapshot(joint_positions_rad=[]),
        joint_names=["joint1", "joint2"],
        for_display=False,
    )

    assert preserved.positions_list() == [0.7, -0.6]
    assert preserved.provenance == "backend_cache"
    assert replacement.positions_list() == [0.3, -0.2]
    assert replacement.provenance == "deployment_initial"


def test_runtime_cutover_clears_retained_observed_pose() -> None:
    state = _state()
    state.deployment = _deployment()
    accepted = state.resolve_joint_position_authority(
        _observed_snapshot([0.4, -0.1]),
        joint_names=["joint1", "joint2"],
        for_display=True,
    )
    assert accepted.positions_list() == [0.4, -0.1]

    state._commit_joint_state_runtime_cutover(  # noqa: SLF001
        ["joint1", "joint2"],
        preserve_backend_state=False,
    )
    after_cutover = state.resolve_joint_position_authority(
        ExecutionSnapshot(joint_positions_rad=[]),
        joint_names=["joint1", "joint2"],
        for_display=True,
    )

    assert after_cutover.positions_list() is None
    assert after_cutover.provenance == "observed_snapshot_unavailable"


def test_failed_runtime_cutover_preserves_position_caches() -> None:
    state = _state()
    state.deployment = _deployment(
        adapter_id="execution.native",
        profile="native",
        initial=[0.3, -0.2],
    )
    state.record_current_joint_positions([0.7, -0.6])

    def reject_candidate(*_args: Any, **_kwargs: Any) -> None:
        raise RuntimeError("candidate rejected")

    state._rebuild_runtime_locked = reject_candidate  # type: ignore[method-assign]

    with pytest.raises(RuntimeError, match="candidate rejected"):
        state.apply_models(
            SimpleNamespace(viewer=None),  # type: ignore[arg-type]
            state.deployment,
        )

    retained = state.resolve_joint_position_authority(
        ExecutionSnapshot(joint_positions_rad=[]),
        joint_names=["joint1", "joint2"],
        for_display=False,
    )
    assert retained.positions_list() == [0.7, -0.6]
    assert retained.provenance == "backend_cache"


@pytest.mark.parametrize(
    ("adapter_id", "profile", "expected_profile", "expected_authority"),
    [
        (
            "execution.ros2_control_follow_joint_trajectory",
            "authored_preview",
            "composer_only",
            "backend_authored",
        ),
        (
            "execution.ros2_control_follow_joint_trajectory",
            "commanded_sim",
            "commanded_sim",
            "backend_authored",
        ),
        (
            "execution.ros2_control_follow_joint_trajectory",
            "observed_ros2",
            "observed_ros2",
            "controller_observed",
        ),
        (
            "execution.ros2_control_follow_joint_trajectory",
            "observed_robot",
            "real_robot",
            "controller_observed",
        ),
        (
            "execution.native",
            "native",
            "composer_only",
            "backend_authored",
        ),
    ],
)
def test_runtime_status_uses_the_same_position_authority_classification(
    adapter_id: str,
    profile: str,
    expected_profile: str,
    expected_authority: str,
) -> None:
    state = _state()
    state.deployment = _deployment(adapter_id=adapter_id, profile=profile)

    runtime_profile, _label, authority, _config = _execution_profile(state)

    assert runtime_profile == expected_profile
    assert authority == expected_authority
    assert (
        authority == "controller_observed"
    ) is state.joint_state_authority_profile().measurement_authoritative


@pytest.mark.parametrize("operation", ["apply", "load"])
def test_artifact_mutation_rechecks_cycle_state_after_waiting_for_lock(
    operation: str,
) -> None:
    state = _state()
    state.cycle_manager = SimpleNamespace(  # type: ignore[assignment]
        state=SimpleNamespace(value="idle")
    )
    called: list[str] = []
    if operation == "apply":
        state._apply_models_locked = (  # type: ignore[method-assign]
            lambda *_args, **_kwargs: called.append("apply")
        )

        def mutate() -> None:
            state.apply_models(None, None)  # type: ignore[arg-type]

    else:
        state._load_program_locked = (  # type: ignore[method-assign]
            lambda *_args, **_kwargs: called.append("load")
        )

        def mutate() -> None:
            state.load_program("/unused.program.yaml")

    attempting = threading.Event()
    errors: list[BaseException] = []

    def worker() -> None:
        attempting.set()
        try:
            mutate()
        except BaseException as exc:  # noqa: BLE001 - captured for thread assertion
            errors.append(exc)

    with state.artifact_read_transaction():
        thread = threading.Thread(target=worker)
        thread.start()
        assert attempting.wait(timeout=1.0)
        state.cycle_manager.state.value = "direct_control"
    thread.join(timeout=1.0)

    assert thread.is_alive() is False
    assert called == []
    assert len(errors) == 1
    assert str(errors[0]) == (
        "artifact_mutation_requires_idle_runtime: cycle_state=direct_control"
    )


@pytest.mark.parametrize("operation", ["apply", "load"])
def test_initial_artifact_mutation_without_manager_remains_allowed(
    operation: str,
) -> None:
    state = _state()
    called: list[str] = []
    if operation == "apply":
        state._apply_models_locked = (  # type: ignore[method-assign]
            lambda *_args, **_kwargs: called.append("apply")
        )
        state.apply_models(None, None)  # type: ignore[arg-type]
    else:
        state._load_program_locked = (  # type: ignore[method-assign]
            lambda *_args, **_kwargs: called.append("load")
        )
        state.load_program("/unused.program.yaml")

    assert called == [operation]


def test_internal_runtime_rebuild_uses_the_same_idle_guard() -> None:
    state = _state()
    state.cycle_manager = SimpleNamespace(  # type: ignore[assignment]
        state=SimpleNamespace(value="stopped")
    )
    called: list[str] = []
    state._rebuild_runtime_locked = (  # type: ignore[method-assign]
        lambda *_args, **_kwargs: called.append("rebuild")
    )

    with pytest.raises(
        RuntimeError,
        match=r"^artifact_mutation_requires_idle_runtime: cycle_state=stopped$",
    ):
        state._rebuild_runtime(None, None)  # type: ignore[arg-type]  # noqa: SLF001

    assert called == []


def test_artifact_mutation_rejects_executing_gripper_while_cycle_is_idle() -> None:
    state = _state()
    deployment = _deployment()
    deployment.grippers = {"tool": SimpleNamespace()}
    state.deployment = deployment
    state.cycle_manager = SimpleNamespace(  # type: ignore[assignment]
        state=SimpleNamespace(value="idle")
    )
    state.kernel.get_gripper_status = (  # type: ignore[method-assign,union-attr]
        lambda _gripper_id: SimpleNamespace(state=GripperCommandState.EXECUTING)
    )
    called: list[str] = []
    state._apply_models_locked = (  # type: ignore[method-assign]
        lambda *_args, **_kwargs: called.append("apply")
    )

    with pytest.raises(
        RuntimeError,
        match=r"^artifact_mutation_requires_idle_runtime: gripper_executing=tool$",
    ):
        state.apply_models(None, None)  # type: ignore[arg-type]

    assert called == []


def test_artifact_mutation_fails_closed_when_gripper_status_is_unavailable() -> None:
    state = _state()
    deployment = _deployment()
    deployment.grippers = {"tool": SimpleNamespace()}
    state.deployment = deployment
    state.cycle_manager = SimpleNamespace(  # type: ignore[assignment]
        state=SimpleNamespace(value="idle")
    )

    def unavailable(_gripper_id: str) -> Any:
        raise RuntimeError("status transport unavailable")

    state.kernel.get_gripper_status = unavailable  # type: ignore[method-assign,union-attr]

    with pytest.raises(
        RuntimeError,
        match=(
            r"^artifact_mutation_runtime_state_unavailable: "
            r"gripper_status=tool$"
        ),
    ):
        state.apply_models(None, None)  # type: ignore[arg-type]


def test_run_admission_blocks_cutover_without_serializing_run_execution() -> None:
    state = _state()
    state.cycle_manager = SimpleNamespace(  # type: ignore[assignment]
        state=SimpleNamespace(value="idle")
    )
    called: list[str] = []
    state._apply_models_locked = (  # type: ignore[method-assign]
        lambda *_args, **_kwargs: called.append("apply")
    )
    attempting = threading.Event()
    finished = threading.Event()
    errors: list[BaseException] = []

    def mutate() -> None:
        attempting.set()
        try:
            state.apply_models(None, None)  # type: ignore[arg-type]
        except BaseException as exc:  # noqa: BLE001 - captured for thread assertion
            errors.append(exc)
        finally:
            finished.set()

    with state.runtime_admission_guard():
        # Before CycleManager.run() publishes RUNNING, the in-flight admission
        # token alone must prevent a cutover without holding the artifact lock.
        thread = threading.Thread(target=mutate)
        thread.start()
        assert attempting.wait(timeout=1.0)
        assert finished.wait(timeout=1.0)
    thread.join(timeout=1.0)

    assert thread.is_alive() is False
    assert called == []
    assert len(errors) == 1
    assert str(errors[0]) == (
        "artifact_mutation_requires_idle_runtime: run_admission_in_progress"
    )


def test_run_admission_may_activate_its_own_staged_cutover() -> None:
    state = _state()
    state.cycle_manager = SimpleNamespace(  # type: ignore[assignment]
        state=SimpleNamespace(value="idle")
    )
    called: list[str] = []
    state._apply_models_locked = (  # type: ignore[method-assign]
        lambda *_args, **_kwargs: called.append("apply")
    )

    with state.runtime_admission_guard():
        state.apply_models(None, None)  # type: ignore[arg-type]

    assert called == ["apply"]


def test_stopped_runtime_override_promotion_requires_reset_to_idle() -> None:
    state = _state()
    state.cycle_manager = SimpleNamespace(  # type: ignore[assignment]
        state=SimpleNamespace(value="stopped")
    )

    with pytest.raises(
        RuntimeError,
        match="Reset a stopped execution before promotion",
    ):
        state.promote_runtime_overrides_to_loaded_program(include_staged=False)


@pytest.mark.parametrize(
    "operation",
    ["activate", "remove", "clear", "revalidate", "promote", "apply"],
)
def test_runtime_override_mutations_share_the_artifact_transaction(
    operation: str,
) -> None:
    state = _state()
    entered = threading.Event()
    attempting = threading.Event()
    errors: list[BaseException] = []
    private_names = {
        "activate": "_activate_staged_runtime_overrides_locked",
        "remove": "_remove_runtime_override_locked",
        "clear": "_clear_runtime_overrides_locked",
        "revalidate": "_revalidate_stale_overrides_locked",
        "promote": "_promote_runtime_overrides_to_loaded_program_locked",
        "apply": "_apply_runtime_override_locked",
    }
    setattr(
        state,
        private_names[operation],
        lambda *_args, **_kwargs: entered.set(),
    )

    def mutate() -> None:
        attempting.set()
        try:
            if operation == "activate":
                state.activate_staged_runtime_overrides()
            elif operation == "remove":
                state.remove_runtime_override("move")
            elif operation == "clear":
                state.clear_runtime_overrides()
            elif operation == "revalidate":
                state.revalidate_stale_overrides()
            elif operation == "promote":
                state.promote_runtime_overrides_to_loaded_program(
                    include_staged=False
                )
            else:
                state.apply_runtime_override(
                    node_name="move",
                    params={"planning_timeout_s": 1.0},
                    authority_source="human",
                    approval_mode="human_approved",
                )
        except BaseException as exc:  # noqa: BLE001 - captured for thread assertion
            errors.append(exc)

    with state.artifact_read_transaction():
        thread = threading.Thread(target=mutate)
        thread.start()
        assert attempting.wait(timeout=1.0)
        assert entered.wait(timeout=0.05) is False
    thread.join(timeout=1.0)

    assert thread.is_alive() is False
    assert errors == []
    assert entered.is_set()


def test_program_mutation_routes_enter_artifact_transaction() -> None:
    state = _state()
    entries: list[str] = []
    original_transaction = state.artifact_read_transaction

    @contextmanager
    def tracked_transaction():
        entries.append("entered")
        with original_transaction():
            yield

    state.artifact_read_transaction = tracked_transaction  # type: ignore[method-assign]
    router = create_program_router(state)
    endpoints = {
        route.path: route.endpoint
        for route in router.routes
        if route.path in {"/program/tree", "/session/apply"}
    }

    with pytest.raises(HTTPException) as tree_error:
        endpoints["/program/tree"](
            UpdateTreeRequest(
                tree={
                    "id": "timer",
                    "name": "timer",
                    "params": {"duration_s": 0.01},
                    "children": [],
                }
            )
        )
    with pytest.raises(HTTPException) as session_error:
        endpoints["/session/apply"](ApplySessionRequest())

    assert tree_error.value.status_code == 404
    assert session_error.value.status_code == 404
    assert entries == ["entered", "entered"]


class _DirectCycleManager:
    def __init__(self, snapshot: ExecutionSnapshot) -> None:
        self._snapshot = snapshot
        self.state = SimpleNamespace(value="idle")
        self.submitted_tree: dict[str, Any] | None = None
        self.on_execute: Any = None

    def get_execution_snapshot(self) -> ExecutionSnapshot:
        return self._snapshot

    def execute_intervention(self, tree: dict[str, Any], **kwargs: Any):
        self.submitted_tree = tree
        self.state.value = "direct_control"
        if self.on_execute is not None:
            self.on_execute()
        now_ns = time.monotonic_ns()
        return InterventionSession(
            session_id="session",
            parent_run_id=kwargs.get("parent_run_id"),
            entry_state="idle",
            lease_timeout_ms=int(kwargs["lease_timeout_ms"]),
            lease_expires_at_ns=now_ns + int(kwargs["lease_timeout_ms"]) * 1_000_000,
            resume_target=str(kwargs["resume_target"]),
            session_kind=str(kwargs["session_kind"]),
            authority_source=str(kwargs["authority_source"]),
            principal_id=str(kwargs["principal_id"]),
            grant_type=str(kwargs["grant_type"]),
            trigger_reason=str(kwargs["trigger_reason"]),
            trigger_node_instance=kwargs["trigger_node_instance"],
            approval_mode=str(kwargs["approval_mode"]),
            artifact_origin=str(kwargs["artifact_origin"]),
        )


def _direct_endpoint(state: ComposerState, path: str):
    router = create_direct_router(state)
    return next(route.endpoint for route in router.routes if route.path == path)


def _joint_jog_endpoint(state: ComposerState):
    return _direct_endpoint(state, "/joint-jog")


def test_all_motion_capable_direct_routes_enter_artifact_transaction() -> None:
    state = _state()
    entries: list[str] = []
    original_transaction = state.artifact_read_transaction

    @contextmanager
    def tracked_transaction():
        entries.append("entered")
        with original_transaction():
            yield

    state.artifact_read_transaction = tracked_transaction  # type: ignore[method-assign]
    http_request = SimpleNamespace(state=SimpleNamespace(principal_id=None))
    requests = {
        "/execute": InterventionExecuteRequest(
            tree={
                "id": "timer",
                "name": "timer",
                "params": {"duration_s": 0.01},
                "children": [],
            },
            principalId="operator",
        ),
        "/joint-jog": JointJogRequest(
            jointNames=["joint1"],
            jointPositionsRad=[0.1],
        ),
        "/cartesian-jog": CartesianJogRequest(
            positionM=[0.0, 0.0, 0.0],
            orientationXyzw=[0.0, 0.0, 0.0, 1.0],
        ),
    }

    for path, request in requests.items():
        response = _direct_endpoint(state, path)(request, http_request)
        assert response.status_code == 409

    assert entries == ["entered", "entered", "entered"]


@pytest.mark.parametrize(
    ("profile", "expects_optimistic_target"),
    [("observed_robot", False), ("commanded_sim", True)],
)
def test_direct_joint_jog_respects_deployment_position_authority(
    profile: str,
    expects_optimistic_target: bool,
) -> None:
    state = _state()
    state.deployment = _deployment(profile=profile)
    source = "observed_ros2" if profile == "observed_robot" else "commanded_sim_predicted"
    snapshot = _observed_snapshot([0.0, 0.1], source=source)
    state.cycle_manager = _DirectCycleManager(snapshot)  # type: ignore[assignment]
    state.motion_arm_admission_block_reason = lambda: None  # type: ignore[method-assign]
    writes: list[list[float]] = []
    state.record_current_joint_positions = (  # type: ignore[method-assign]
        lambda positions: writes.append(list(positions))
    )
    transaction_depth = [0]
    accepted_under_transaction: list[tuple[int, str]] = []
    original_transaction = state.artifact_read_transaction

    @contextmanager
    def tracked_transaction():
        with original_transaction():
            transaction_depth[0] += 1
            try:
                yield
            finally:
                transaction_depth[0] -= 1

    state.artifact_read_transaction = tracked_transaction  # type: ignore[method-assign]
    state.cycle_manager.on_execute = lambda: accepted_under_transaction.append(  # type: ignore[union-attr]
        (transaction_depth[0], state.cycle_manager.state.value)  # type: ignore[union-attr]
    )

    response = _joint_jog_endpoint(state)(
        JointJogRequest(
            jointNames=["joint2"],
            jointPositionsRad=[0.2],
            principalId="operator",
        ),
        SimpleNamespace(state=SimpleNamespace(principal_id=None)),
    )
    body = json.loads(response.body)

    assert response.status_code == 200, body
    assert state.cycle_manager.submitted_tree is not None
    assert state.cycle_manager.submitted_tree["params"]["joint_goal_positions_rad"] == [
        0.0,
        0.2,
    ]
    assert accepted_under_transaction == [(1, "direct_control")]
    assert ([0.0, 0.2] in writes) is expects_optimistic_target
