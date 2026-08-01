"""Cutover tests for the historical qualification process launcher.

The three original safety cases are pinned at blob
30410567b74434c990c072c7a40a9a1be907befa. They return with the future
headless scenario client and remain named in RESTORATION_ASSIGNMENTS.
"""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
from typing import Any

import pytest


_REPO_ROOT = Path(__file__).resolve().parents[2]
_HISTORICAL_CASES = {
    "test_local_process_launcher_cleans_up_on_forced_health_timeout",
    "test_local_process_launcher_refuses_port_already_serving_a_composer",
    "test_local_process_launcher_rejects_foreign_readiness_after_child_death",
}


def _load_run_scenarios_module() -> Any:
    module_path = _REPO_ROOT / "scripts" / "run-scenarios.py"
    spec = importlib.util.spec_from_file_location(
        "manyforge_run_scenarios_test",
        module_path,
    )
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _load_scenario_rows_module() -> Any:
    module_path = _REPO_ROOT / "scripts" / "lib" / "scenarios_loader.py"
    spec = importlib.util.spec_from_file_location(
        "manyforge_scenario_rows_test",
        module_path,
    )
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_managed_composer_process_is_a_pre_effect_typed_gate() -> None:
    run_scenarios = _load_run_scenarios_module()

    with pytest.raises(
        run_scenarios.ScenarioExecutionUnavailableError,
        match="composable_cell_runtime_unavailable",
    ):
        run_scenarios.ManagedComposerProcess(
            workspace_root=_REPO_ROOT,
            catalog_path="must-not-be-read.yaml",
            host="127.0.0.1",
            port=19000,
            output_dir="must-not-be-created",
        )

    source = (_REPO_ROOT / "scripts" / "run-scenarios.py").read_text(
        encoding="utf-8"
    )
    assert "subprocess" not in source
    assert "httpx" not in source
    assert "Popen" not in source
    assert "/api/health" not in source


def test_unavailable_payload_preserves_all_process_safety_assignments(
    capsys: pytest.CaptureFixture[str],
) -> None:
    run_scenarios = _load_run_scenarios_module()

    assert run_scenarios.main(["--scenario", "must-not-be-read.yaml"]) == 78
    payload = json.loads(capsys.readouterr().err)
    mapped = {
        assignment["historical_symbols_or_cases"]
        for assignment in payload["assignments"]
    }

    assert _HISTORICAL_CASES <= mapped
    assert payload["historical_source"].endswith(
        "08222188293df8e12f90dc72e204c21f22f92ea1"
    )


def test_scenario_row_adapter_gates_before_legacy_catalog_or_path_work(
    capsys: pytest.CaptureFixture[str],
) -> None:
    scenario_rows = _load_scenario_rows_module()

    assert scenario_rows.main(["must-not-be-read.yaml"]) == 78
    payload = json.loads(capsys.readouterr().err)
    assert payload == {
        "schema_version": "manyforge.qualification_unavailable.v1",
        "reason_code": "composable_cell_scenario_discovery_unavailable",
        "restoration_gate": "I6/N",
        "historical_source_blob": "916b265d221ee13b47f046e3a7f8a8d553de8403",
        "exit_code": 78,
        "removed_contract": "legacy_pipe_delimited_deployment_v0_scenario_rows",
    }

    source = (_REPO_ROOT / "scripts" / "lib" / "scenarios_loader.py").read_text(
        encoding="utf-8"
    )
    for forbidden in (
        "os.environ",
        "sys.path.insert",
        "build_node_environment",
        "discover_scenarios",
        "Path(",
    ):
        assert forbidden not in source


def test_scenario_shell_library_contains_only_typed_boundaries() -> None:
    source = (_REPO_ROOT / "scripts" / "lib" / "scenarios.sh").read_text(
        encoding="utf-8"
    )
    assert "f6045793d753354bbe0fecf8fb153aee235b9d9e" in source
    assert "I6/N" in source
    for symbol in (
        "_load_scenarios",
        "scenario_canonical",
        "scenario_lookup",
        "scenario_field",
        "scenarios_for",
        "scenario_describe",
        "scenarios_print_table",
    ):
        assert f"{symbol}()" in source
    for forbidden in (
        "python3",
        "awk -F",
        "_SCENARIO_TABLE",
        "_SCENARIO_ALIASES",
        "mapfile",
    ):
        assert forbidden not in source
