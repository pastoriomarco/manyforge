from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
from types import ModuleType

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]


def load_runner() -> ModuleType:
    path = REPO_ROOT / "scripts/qualification/run_i9_isaac_matrix.py"
    spec = importlib.util.spec_from_file_location("i9_isaac_matrix", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def runner() -> ModuleType:
    return load_runner()


def exact_environment(runner: ModuleType) -> dict[str, str]:
    return dict(runner.EXPECTED_DDS)


def row_for(runner: ModuleType, scenario) -> dict[str, object]:
    return {
        "schema_version": runner.ROW_SCHEMA,
        "scenario_id": scenario.scenario_id,
        "component_id": scenario.component_id,
        "world": {
            "name": scenario.world_name,
            "sha256": scenario.world_sha256,
            "uri": scenario.world_uri,
        },
        "checks": {key: True for key in scenario.required_checks},
        "details": {"terminal": {"state": "qualified"}},
        "elapsed_s": 1.25,
    }


def preflight_for(runner: ModuleType) -> dict[str, object]:
    return {"schema_version": runner.PREFLIGHT_SCHEMA}


def test_frozen_matrix_has_exact_five_non_aliasing_owner_rows(
    runner: ModuleType,
) -> None:
    assert tuple(row.scenario_id for row in runner.SCENARIOS) == (
        "isaac-lite6-pick-place",
        "isaac-lite6-trocar-foundationpose",
        "isaac-ur10e-light-scene",
        "isaac-ur10e-light-scene-assistant",
        "isaac-ur10e-light-scene-collision",
    )
    assert len({row.cell_id for row in runner.SCENARIOS}) == 5
    assert len({row.component_id for row in runner.SCENARIOS}) == 5
    lite6 = [row for row in runner.SCENARIOS if row.profile == "lite6"]
    assert all("UniversalRobots" not in row.world_uri for row in lite6)
    assert all(row.component_id.startswith("isaac_lite6_") for row in lite6)
    assert len({row.world_sha256 for row in runner.SCENARIOS}) == 3


def test_dds_environment_requires_exact_peers_and_rejects_stale_inputs(
    runner: ModuleType,
) -> None:
    evidence = runner.validate_dds_environment(exact_environment(runner))
    assert evidence["static_peers"] == ["192.168.1.132", "192.168.1.68"]

    crossed = exact_environment(runner)
    crossed["ROS_STATIC_PEERS"] = "192.168.1.132;192.168.1.136"
    with pytest.raises(runner.QualificationError, match="recorded topology"):
        runner.validate_dds_environment(crossed)

    stale = exact_environment(runner)
    stale["CYCLONEDDS_URI"] = "file:///tmp/stale.xml"
    with pytest.raises(runner.QualificationError, match="CYCLONEDDS_URI"):
        runner.validate_dds_environment(stale)


def test_world_manifest_is_exact_and_rejects_changed_content(
    runner: ModuleType, tmp_path: Path
) -> None:
    expected_paths: dict[Path, str] = {}
    for row in runner.SCENARIOS:
        relative = Path(row.world_uri.removeprefix("file:")).relative_to(
            Path(runner._ISAAC_ASSET_ROOT)  # noqa: SLF001
        )
        expected_paths[tmp_path / relative] = row.world_sha256
    for path in expected_paths:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"fixture")

    real_sha256 = runner._sha256_file  # noqa: SLF001
    runner._sha256_file = lambda path: expected_paths[path]  # noqa: SLF001
    try:
        manifest = runner.build_world_manifest(tmp_path)
    finally:
        runner._sha256_file = real_sha256  # noqa: SLF001
    assert len(manifest["worlds"]) == 3
    assert runner.validate_world_manifest(manifest) == manifest

    manifest["worlds"][0]["sha256"] = "0" * 64
    with pytest.raises(runner.QualificationError, match="exact I9 worlds"):
        runner.validate_world_manifest(manifest)


def test_package_preflight_binds_catalog_program_pose_and_site(
    runner: ModuleType, tmp_path: Path
) -> None:
    package = tmp_path / "package"
    source = tmp_path / "source"
    package.mkdir()
    config = source / "manyforge_ros_bringup/config"
    config.mkdir(parents=True)
    catalog_rows = []
    site_rows = []
    for row in runner.SCENARIOS:
        catalog_rows.append(
            {
                "scenario_id": row.scenario_id,
                "deployment_artifact_id": row.deployment_artifact_id,
            }
        )
        site_rows.append({"scenario_id": row.scenario_id})
        (package / row.admission_filename).write_text("{}", encoding="utf-8")
        (package / row.program_filename).write_text(
            "program_id: " + row.program_filename.removesuffix(".program.yaml") + "\n",
            encoding="utf-8",
        )
        (config / row.pose_observer_filename).write_text(
            json.dumps({"cell_id": row.cell_id}), encoding="utf-8"
        )
    (package / "maintained.scenario-catalog.yaml").write_text(
        json.dumps(
            {
                "scenario_catalog_revision": "sha256:" + "1" * 64,
                "scenarios": catalog_rows,
            }
        ),
        encoding="utf-8",
    )
    (config / "isaac_program_simulation_site.json").write_text(
        json.dumps({"scenarios": site_rows}), encoding="utf-8"
    )

    result = runner.validate_package(package, source)
    assert len(result["scenarios"]) == 5

    catalog_rows[1]["deployment_artifact_id"] = catalog_rows[0][
        "deployment_artifact_id"
    ]
    (package / "maintained.scenario-catalog.yaml").write_text(
        json.dumps(
            {
                "scenario_catalog_revision": "sha256:" + "1" * 64,
                "scenarios": catalog_rows,
            }
        ),
        encoding="utf-8",
    )
    with pytest.raises(runner.QualificationError, match="another deployment"):
        runner.validate_package(package, source)


def test_row_validation_rejects_world_component_and_obligation_substitution(
    runner: ModuleType,
) -> None:
    scenario = runner.SCENARIOS[0]
    row = row_for(runner, scenario)
    assert runner.validate_row(row) == row

    crossed = row_for(runner, scenario)
    crossed["component_id"] = runner.SCENARIOS[1].component_id
    with pytest.raises(runner.QualificationError, match="another component"):
        runner.validate_row(crossed)

    crossed = row_for(runner, scenario)
    crossed["world"] = row_for(runner, runner.SCENARIOS[2])["world"]
    with pytest.raises(runner.QualificationError, match="another world"):
        runner.validate_row(crossed)

    missing = row_for(runner, scenario)
    missing["checks"].pop(next(iter(scenario.required_checks)))
    with pytest.raises(runner.QualificationError, match="frozen set"):
        runner.validate_row(missing)


def test_reducer_requires_all_rows_and_all_matrix_checks(runner: ModuleType) -> None:
    rows = [row_for(runner, scenario) for scenario in runner.SCENARIOS]
    checks = {key: True for key in runner.MATRIX_CHECKS}
    result = runner.reduce_matrix(
        rows, checks, preflight=preflight_for(runner)
    )
    assert result["passed"] is True
    assert [row["scenario_id"] for row in result["rows"]] == [
        scenario.scenario_id for scenario in runner.SCENARIOS
    ]

    with pytest.raises(runner.QualificationError, match="exactly five"):
        runner.reduce_matrix(rows[:-1], checks, preflight=preflight_for(runner))

    crossed = [dict(row) for row in rows]
    crossed[1] = dict(crossed[0])
    with pytest.raises(runner.QualificationError, match="duplicate"):
        runner.reduce_matrix(crossed, checks, preflight=preflight_for(runner))

    checks["no_cross_drive"] = False
    result = runner.reduce_matrix(
        rows, checks, preflight=preflight_for(runner)
    )
    assert result["passed"] is False
    assert result["failures"]["matrix_checks"] == ["no_cross_drive"]


def test_reducer_surfaces_failed_row_checks_without_accepting_pass_field(
    runner: ModuleType,
) -> None:
    rows = [row_for(runner, scenario) for scenario in runner.SCENARIOS]
    rows[4]["checks"]["collision_stop_terminal"] = False
    result = runner.reduce_matrix(
        rows,
        {key: True for key in runner.MATRIX_CHECKS},
        preflight=preflight_for(runner),
    )
    assert result["passed"] is False
    assert result["failures"]["rows"] == {
        "isaac-ur10e-light-scene-collision": ["collision_stop_terminal"]
    }
