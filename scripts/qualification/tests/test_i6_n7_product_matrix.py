from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
from types import ModuleType

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]


def _load_matrix(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> ModuleType:
    package = tmp_path / "package"
    installed = tmp_path / "installed"
    package.mkdir()
    installed.mkdir()
    monkeypatch.setenv("MANYFORGE_ROOT", str(REPO_ROOT))
    monkeypatch.setenv("N7_MATRIX_PACKAGE", str(package))
    monkeypatch.setenv("N7_MATRIX_INSTALLED", str(installed))
    path = REPO_ROOT / "scripts/qualification/run_i6_n7_product_matrix.py"
    spec = importlib.util.spec_from_file_location("i6_n7_product_matrix", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_process_commands_cover_the_complete_typed_runtime_boundary(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    matrix = _load_matrix(tmp_path, monkeypatch)
    layout = matrix.CellInstanceLayout.open_or_create(
        cell_id="cell.fixture",
        cell_root=tmp_path / "cell",
        install_root=matrix.PACKAGE,
    )
    commands = matrix._process_commands(  # noqa: SLF001
        scenario_id="lite6_pick_and_place",
        cell_id="cell.fixture",
        admission_filename="admission.json",
        model_config=tmp_path / "models.json",
        api_port=9010,
        runner_port=9011,
        composer_port=9000,
        layout=layout,
    )

    def value(arguments: list[str], option: str) -> str:
        return arguments[arguments.index(option) + 1]

    manager = commands["manager"]
    for removed in ("--package-root", "--runtime-state-root", "--publication-root"):
        assert removed not in manager
    for required in (
        "--install-root",
        "--manager-state-root",
        "--manager-run-root",
        "--execution-state-root",
        "--execution-run-root",
        "--program-cycle-journal",
        "--authoring-transaction-journal",
        "--projection-root",
        "--program-runner-liveness-socket",
    ):
        assert manager.count(required) == 1

    for required in (
        "--execution-run-root",
        "--cycle-journal-path",
        "--manager-liveness-socket",
        "--cell-id",
    ):
        assert commands["program"].count(required) == 1
    assert value(manager, "--execution-run-root") == value(
        commands["program"], "--execution-run-root"
    )
    assert value(manager, "--program-cycle-journal") == value(
        commands["program"], "--cycle-journal-path"
    )
    assert value(manager, "--program-runner-liveness-socket") == value(
        commands["program"], "--manager-liveness-socket"
    )

    headless = commands["headless"]
    for removed in (
        "--publication-root",
        "--package-root",
        "--manager-state-root",
        "--execution-state-root",
    ):
        assert removed not in headless
    for required in (
        "--projection-root",
        "--install-root",
        "--artifact-root",
        "--program-selection-state-root",
        "--manager-run-root",
        "--execution-run-root",
    ):
        assert headless.count(required) == 1
    assert value(manager, "--manager-run-root") == value(
        headless, "--manager-run-root"
    )
    assert value(manager, "--execution-run-root") == value(
        headless, "--execution-run-root"
    )
    assert commands["composer"].count("--authoring-state-root") == 1
    assert value(manager, "--authoring-transaction-journal") == str(
        Path(value(commands["composer"], "--authoring-state-root"))
        / "composer-authoring-transactions.sqlite3"
    )


def test_authoring_apply_uses_one_exact_current_browser_barrier(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    matrix = _load_matrix(tmp_path, monkeypatch)
    workspace = {
        "schemaVersion": "manyforge.composer_program_workspace.v1",
        "draft": {"draftId": "draft.1", "draftRevision": 7},
    }
    snapshot = {
        "schema_version": "manyforge.composer_headless_state.v2",
        "complete": True,
        "receiver_epoch": "receiver.1",
        "snapshot_sequence": 9,
        "authoring_admission": {
            "authoring_allowed": True,
            "admission_generation": 4,
        },
        "program_cycle": {"state": "idle_after_reset"},
        "draft_identity": {"draft_id": "draft.1", "draft_revision": 7},
    }
    body = matrix._authoring_apply_body(  # noqa: SLF001
        workspace=workspace,
        snapshot=snapshot,
        client_id="browser.1",
        scenario_id="lite6_pick_and_place",
        scenario_catalog_revision="sha256:" + "a" * 64,
    )
    assert body == {
        "draftId": "draft.1",
        "expectedDraftRevision": 7,
        "clientId": "browser.1",
        "snapshotReceiverEpoch": "receiver.1",
        "snapshotSequence": 9,
        "expectedAdmissionGeneration": 4,
        "requestOwnerEpoch": "n7-product-matrix.authoring",
        "requestId": "n7.apply.lite6_pick_and_place",
        "scenarioCatalogRevision": "sha256:" + "a" * 64,
        "scenarioId": "lite6_pick_and_place",
    }

    snapshot["draft_identity"] = {
        "draft_id": "draft.1",
        "draft_revision": 8,
    }
    with pytest.raises(RuntimeError, match="Apply-ready snapshot"):
        matrix._authoring_apply_body(  # noqa: SLF001
            workspace=workspace,
            snapshot=snapshot,
            client_id="browser.1",
            scenario_id="lite6_pick_and_place",
            scenario_catalog_revision="sha256:" + "a" * 64,
        )
