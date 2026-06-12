from __future__ import annotations

from pathlib import Path
import tarfile

import pytest

pytest.importorskip("fastapi")

from fastapi.testclient import TestClient

from manyforge_behavior.kernel_client import (
    MockKernelClient,
    SceneEditRequest,
    SceneObjectSnapshot,
    SceneShape,
)
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.verification import (
    expand_matrix_scenarios,
    ScenarioDefinition,
    ScenarioMatrixDefinition,
    ScenarioRunner,
)
from manyforge_composer.verification.runner import _git_rev_parse


_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
_SCENARIO_ROOT = _REPO_ROOT / "verification" / "scenarios"
_TEST_SECURITY = SecurityConfig(
    cors_origins=["http://localhost:3000"],
    api_token=None,
    allowed_fs_roots=[str(_REPO_ROOT), "/tmp", "/workspace"],
)


def test_basic_pick_and_place_scenario_runs_to_completion():
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=MockKernelClient(execute_duration_s=0.05),
        allowed_fs_roots=[str(_REPO_ROOT), "/tmp", "/workspace"],
    )
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        runner = ScenarioRunner(client, workspace_root=_REPO_ROOT)
        result = runner.run_from_path(
            _SCENARIO_ROOT / "basic_pick_and_place_complete.scenario.yaml"
        )

    assert result["success"] is True
    assert result["finalArtifacts"]["digest"]["state"] == "idle"
    assert any(
        assertion["condition"].get("event_type") == "execute_started"
        for assertion in result["assertions"]
    )

def test_runner_can_assert_visual_scene_consistency_without_manual_inspection():
    kernel = MockKernelClient()
    kernel.scene_edit(
        SceneEditRequest(
            operation="add",
            object_id="diag_box",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.4, 0.1, 0.2],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        )
    )
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        allowed_fs_roots=[str(_REPO_ROOT), "/tmp", "/workspace"],
    )

    state.update_visual_scene_objects_snapshot(
        [
            SceneObjectSnapshot(
                object_id="diag_box",
                shape=SceneShape(type="box", box_dimensions_m=[0.1, 0.1, 0.1]),
                pose_in_universe={
                    "position_m": [0.4, 0.1, 0.2],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                attached=False,
                frame_id="universe",
            )
        ]
    )
    app = create_app(state, security=_TEST_SECURITY)
    scenario = ScenarioDefinition(
        scenario_id="visual_scene_consistency",
        description="Machine-readable visual consistency check.",
        assertions=[
            {
                "endpoint": "scene_diagnostics",
                "path": "summary.viewerAvailable",
                "equals": True,
            },
            {
                "endpoint": "scene_diagnostics",
                "path": "summary.mismatched",
                "equals": 0,
            },
        ],
    )

    with TestClient(app) as client:
        runner = ScenarioRunner(client, workspace_root=_REPO_ROOT)
        result = runner.run(scenario)

    assert result["success"] is True
    assert result["finalArtifacts"]["sceneDiagnostics"]["summary"]["matched"] == 1


def test_runner_writes_metadata_and_matrix_summary(tmp_path):
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=MockKernelClient(execute_duration_s=0.05),
        allowed_fs_roots=[str(_REPO_ROOT), "/tmp", "/workspace"],
    )
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        runner = ScenarioRunner(client, workspace_root=_REPO_ROOT)
        definitions = [
            ScenarioDefinition(
                scenario_id="cycle_status_idle",
                assertions=[
                    {
                        "endpoint": "cycle_status",
                        "path": "state",
                        "equals": "idle",
                    },
                ],
            ),
            ScenarioDefinition(
                scenario_id="scene_diagnostics_unavailable_hmi",
                assertions=[
                    {
                        "endpoint": "scene_diagnostics",
                        "path": "summary.viewerAvailable",
                        "equals": False,
                    },
                ],
            ),
        ]
        matrix = ScenarioMatrixDefinition(
            matrix_id="test_matrix",
            description="Matrix summary smoke test.",
        )
        summary = runner.run_many(definitions, matrix_definition=matrix, output_dir=tmp_path)

    assert summary["success"] is True
    assert summary["metadata"]["gitCommit"] is not None
    assert summary["passedCount"] == 2
    assert (tmp_path / "test_matrix.summary.json").exists()
    result_payload = (tmp_path / "cycle_status_idle.result.json").read_text(encoding="utf-8")
    assert '"gitCommit"' in result_payload


def test_git_commit_metadata_can_come_from_environment(
    tmp_path, monkeypatch: pytest.MonkeyPatch
):
    monkeypatch.setenv(
        "MANYFORGE_GIT_COMMIT",
        "0123456789abcdef0123456789abcdef01234567",
    )

    assert _git_rev_parse(tmp_path) == "0123456789abcdef0123456789abcdef01234567"


def test_expand_matrix_scenarios_repeats_and_preserves_tags(tmp_path):
    scenario_path = tmp_path / "sample.scenario.yaml"
    scenario_path.write_text(
        """
scenario_id: sample
description: Sample scenario.
assertions:
  - endpoint: cycle_status
    path: state
    equals: idle
""".strip(),
        encoding="utf-8",
    )
    matrix_path = tmp_path / "matrix.yaml"
    matrix_path.write_text(
        f"""
matrix_id: repeat_matrix
scenarios:
  - path: {scenario_path.name}
    repeat: 3
    tags: [soak, deterministic]
""".strip(),
        encoding="utf-8",
    )
    matrix = ScenarioMatrixDefinition(
        matrix_id="repeat_matrix",
        scenarios=[{"path": scenario_path.name, "repeat": 3, "tags": ["soak", "deterministic"]}],
        source_path=matrix_path,
    )

    definitions = expand_matrix_scenarios(matrix, workspace_root=tmp_path)

    assert [definition.scenario_id for definition in definitions] == [
        "sample__repeat_01",
        "sample__repeat_02",
        "sample__repeat_03",
    ]
    assert all(definition.tags == ["soak", "deterministic"] for definition in definitions)


def test_runner_can_assert_scene_snapshot_and_archive_matrix_output(tmp_path):
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=MockKernelClient(execute_duration_s=0.05),
        allowed_fs_roots=[str(_REPO_ROOT), "/tmp", "/workspace"],
    )

    class SnapshotBroker:
        def has_clients(self):
            return True

        def render_sync(self, width=640, height=480, format="jpeg"):
            return b"\xff\xd8\xffdemo"

    state.snapshot_broker = SnapshotBroker()
    app = create_app(state, security=_TEST_SECURITY)
    scenario = ScenarioDefinition(
        scenario_id="scene_snapshot_available",
        assertions=[
            {"endpoint": "scene_snapshot", "path": "statusCode", "equals": 200},
            {"endpoint": "scene_snapshot", "path": "contentType", "contains": "image/"},
            {"endpoint": "scene_snapshot", "path": "contentLength", "greater_equal": 1},
        ],
    )
    matrix = ScenarioMatrixDefinition(matrix_id="snapshot_matrix", description="Snapshot archive test.")
    archive_path = tmp_path.parent / "snapshot_matrix.tar.gz"

    with TestClient(app) as client:
        runner = ScenarioRunner(client, workspace_root=_REPO_ROOT)
        summary = runner.run_many(
            [scenario],
            matrix_definition=matrix,
            output_dir=tmp_path,
            archive_path=archive_path,
        )

    assert summary["success"] is True
    assert archive_path.exists()
    with tarfile.open(archive_path, "r:gz") as tar:
        names = tar.getnames()
    assert any(name.endswith("snapshot_matrix.summary.json") for name in names)
    assert any(name.endswith("scene_snapshot_available.sceneSnapshot.json") for name in names)
