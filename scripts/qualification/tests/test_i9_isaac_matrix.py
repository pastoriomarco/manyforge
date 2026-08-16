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
    details: dict[str, object] = {"terminal": {"state": "qualified"}}
    if scenario.scenario_id == "isaac-lite6-trocar-foundationpose":
        details["recovery"] = recovery_for(runner, scenario)
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
        "details": details,
        "elapsed_s": 1.25,
    }


def preflight_for(runner: ModuleType) -> dict[str, object]:
    return {"schema_version": runner.PREFLIGHT_SCHEMA}


def recovery_for(runner: ModuleType, scenario) -> dict[str, object]:
    return {
        "schema_version": runner.RECOVERY_EVIDENCE_SCHEMA,
        "component_id": scenario.component_id,
        "dds": {"loss_elapsed_s": 1.0, "rejoin_elapsed_s": 2.0},
        "exact_world_after_rejoin": True,
        "output_sample": {
            "bytes": 128,
            "returncode": 0,
            "seen": True,
            "sha256": "c" * 64,
        },
        "passed": True,
        "provider": {
            "epoch_after": "pose.epoch.2",
            "epoch_before": "pose.epoch.1",
            "loss_elapsed_s": 5.1,
            "recovery_elapsed_s": 2.5,
            "sequence_after": 1,
            "sequence_before": 7,
            "source_id": "foundationpose_trocar",
        },
        "simulation_stopped_after_rejoin": True,
        "simulation_stopped_during_outage": True,
    }


def matrix_evidence_for(runner: ModuleType) -> dict[str, object]:
    base_ns = 1_800_000_000_000_000_000
    samples = []
    for index in range(runner.CLOCK_SAMPLE_COUNT):
        sent = base_ns + index * 10_000_000
        received = sent + 1_000_000
        samples.append(
            {
                "local_send_ns": sent,
                "local_receive_ns": received,
                "remote_ns": sent + 500_000,
            }
        )
    probes = []
    for probe_id, (kind, selector) in runner.PROBE_CONTRACTS.items():
        probes.append(
            {
                "elapsed_s": 0.1,
                "kind": kind,
                "output_bytes": 100,
                "output_sha256": "d" * 64,
                "probe_id": probe_id,
                "returncode": 0,
                "selector": selector,
                "subject_sha256": "e" * 64,
            }
        )
    return {
        "schema_version": runner.MATRIX_EVIDENCE_SCHEMA,
        "clock": {
            "schema_version": runner.CLOCK_EVIDENCE_SCHEMA,
            "hosts": {
                "laptop": {
                    "address": "192.168.1.132",
                    "clock_synchronized": True,
                    "hostname": "tndlux-G16",
                    "synchronization_marker": True,
                },
                "orin": {
                    "address": "192.168.1.68",
                    "clock_synchronized": True,
                    "hostname": "localhost.localdomain",
                    "synchronization_marker": True,
                },
            },
            "samples": samples,
        },
        "static_probes": {
            "schema_version": runner.STATIC_PROBES_SCHEMA,
            "source": {
                "commit": "a" * 40,
                "dirty": False,
                "tree": "b" * 40,
            },
            "probes": probes,
        },
    }


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
    for row in runner.SCENARIOS:
        assert row.runtime_resource_ids == tuple(
            [
                f"{row.component_id}.arm.coordinate.{index:06d}"
                for index in range(6)
            ]
            + [f"{row.component_id}.gripper.coordinate.000000"]
        )
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

    mesh_uri = "examples/isaac_manipulator_scene_light/meshes/fixture.obj"
    mesh_bytes = b"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
    for root in (source, package):
        mesh = root / mesh_uri
        mesh.parent.mkdir(parents=True)
        mesh.write_bytes(mesh_bytes)
    program_path = package / "isaac_manipulator_scene_light.program.yaml"
    program_path.write_text(
        json.dumps(
            {
                "program_id": "isaac_manipulator_scene_light",
                "scene": [
                    {
                        "scene_resource_id": "fixture",
                        "shape": {
                            "type": "mesh",
                            "mesh_resource_uri": mesh_uri,
                        },
                    }
                ],
            }
        ),
        encoding="utf-8",
    )

    result = runner.validate_package(package, source)
    assert len(result["scenarios"]) == 5
    assert result["scene_mesh_assets"] == [
        {"sha256": runner._sha256_file(source / mesh_uri), "uri": mesh_uri}
    ]

    (package / mesh_uri).write_bytes(b"changed")
    with pytest.raises(runner.QualificationError, match="differs from source"):
        runner.validate_package(package, source)
    (package / mesh_uri).write_bytes(mesh_bytes)

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


def test_reducer_derives_all_checks_from_structured_evidence(
    runner: ModuleType,
) -> None:
    rows = [row_for(runner, scenario) for scenario in runner.SCENARIOS]
    evidence = matrix_evidence_for(runner)
    result = runner.reduce_matrix(
        rows, evidence, preflight=preflight_for(runner)
    )
    assert result["passed"] is True
    assert result["matrix_checks"] == {
        check: True for check in sorted(runner.MATRIX_CHECKS)
    }
    assert [row["scenario_id"] for row in result["rows"]] == [
        scenario.scenario_id for scenario in runner.SCENARIOS
    ]

    with pytest.raises(runner.QualificationError, match="exactly five"):
        runner.reduce_matrix(
            rows[:-1], evidence, preflight=preflight_for(runner)
        )

    crossed = [dict(row) for row in rows]
    crossed[1] = dict(crossed[0])
    with pytest.raises(runner.QualificationError, match="duplicate"):
        runner.reduce_matrix(
            crossed, evidence, preflight=preflight_for(runner)
        )

    failed = matrix_evidence_for(runner)
    failed_probe = next(
        probe
        for probe in failed["static_probes"]["probes"]
        if probe["probe_id"] == "two_same_type_instances_isolated"
    )
    failed_probe["returncode"] = 1
    result = runner.reduce_matrix(
        rows, failed, preflight=preflight_for(runner)
    )
    assert result["passed"] is False
    assert result["failures"]["matrix_checks"] == [
        "no_cross_drive",
        "two_same_type_instances_isolated",
    ]


def test_reducer_surfaces_failed_row_checks_without_accepting_pass_field(
    runner: ModuleType,
) -> None:
    rows = [row_for(runner, scenario) for scenario in runner.SCENARIOS]
    rows[4]["checks"]["collision_stop_terminal"] = False
    result = runner.reduce_matrix(
        rows,
        matrix_evidence_for(runner),
        preflight=preflight_for(runner),
    )
    assert result["passed"] is False
    assert result["failures"]["rows"] == {
        "isaac-ur10e-light-scene-collision": ["collision_stop_terminal"]
    }


def test_clock_and_recovery_evidence_are_recomputed_fail_closed(
    runner: ModuleType,
) -> None:
    rows = [row_for(runner, scenario) for scenario in runner.SCENARIOS]
    evidence = matrix_evidence_for(runner)
    evidence["clock"]["samples"][0]["remote_ns"] += (
        runner.CLOCK_MAX_ABS_OFFSET_NS + 1
    )
    result = runner.reduce_matrix(
        rows, evidence, preflight=preflight_for(runner)
    )
    assert result["matrix_checks"]["clock_synchronized"] is False

    recovery = rows[1]["details"]["recovery"]
    recovery["provider"]["epoch_after"] = recovery["provider"]["epoch_before"]
    with pytest.raises(runner.QualificationError, match="pass field"):
        runner.reduce_matrix(
            rows,
            matrix_evidence_for(runner),
            preflight=preflight_for(runner),
        )


def test_static_probe_evidence_requires_exact_selectors_and_clean_source(
    runner: ModuleType,
) -> None:
    rows = [row_for(runner, scenario) for scenario in runner.SCENARIOS]
    evidence = matrix_evidence_for(runner)
    evidence["static_probes"]["probes"][0]["selector"] = "weak.proxy"
    with pytest.raises(runner.QualificationError, match="another test"):
        runner.reduce_matrix(
            rows, evidence, preflight=preflight_for(runner)
        )

    dirty = matrix_evidence_for(runner)
    dirty["static_probes"]["source"]["dirty"] = True
    result = runner.reduce_matrix(
        rows, dirty, preflight=preflight_for(runner)
    )
    assert result["passed"] is False
    assert set(result["failures"]["matrix_checks"]) >= set(
        runner.PROBE_CONTRACTS
    )
