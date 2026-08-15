from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
from types import ModuleType

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]


def load_module(relative: str, name: str) -> ModuleType:
    path = REPO_ROOT / relative
    qualification_root = str(path.parent)
    if qualification_root not in sys.path:
        sys.path.insert(0, qualification_root)
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def static_probe() -> ModuleType:
    return load_module(
        "scripts/qualification/run_i9_static_probes.py", "i9_static_probes"
    )


@pytest.fixture
def clock_probe() -> ModuleType:
    return load_module(
        "scripts/qualification/run_i9_clock_probe.py", "i9_clock_probe"
    )


def test_static_probe_collector_binds_exact_binary_and_test_selectors(
    static_probe: ModuleType, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    source = tmp_path / "source"
    source.mkdir()
    demo = source / (
        "manyforge_ros_bringup/tests/"
        "test_isaac_topic_based_control_launch.py"
    )
    demo.parent.mkdir(parents=True)
    demo.write_text("def test_demo(): pass\n", encoding="utf-8")
    roboplan = tmp_path / "test_retained_ros2_control_backend_factory"
    deployment = tmp_path / "test_verified_controller_lifecycle_expected_domain"
    roboplan.write_bytes(b"roboplan-binary")
    deployment.write_bytes(b"deployment-binary")
    commands: list[tuple[str, ...]] = []

    def run(command, **_kwargs):
        commands.append(tuple(command))
        return {"elapsed_s": 0.1, "output": b"passed\n", "returncode": 0}

    monkeypatch.setattr(static_probe, "_run_bounded", run)
    monkeypatch.setattr(
        static_probe,
        "_source_identity",
        lambda _root: {"commit": "a" * 40, "dirty": False, "tree": "b" * 40},
    )
    evidence = static_probe.build_evidence(
        source_root=source,
        roboplan_test=roboplan,
        deployment_test=deployment,
        environment={},
    )
    normalized, checks = static_probe.validate_static_probes(evidence)
    assert all(checks.values())
    assert len(normalized["probes"]) == 4
    assert any("--gtest_filter=" in " ".join(command) for command in commands)
    assert any("pytest" in command for command in commands)


def test_clock_probe_records_raw_midpoint_samples(
    clock_probe: ModuleType,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    ssh_config = tmp_path / "config"
    known_hosts = tmp_path / "known_hosts"
    hostname = tmp_path / "hostname"
    marker = tmp_path / "synchronized"
    for path in (ssh_config, known_hosts, marker):
        path.write_text("fixture\n", encoding="utf-8")
    hostname.write_text("tndlux-G16\n", encoding="utf-8")

    base = 1_800_000_000_000_000_000
    times: list[int] = []
    remotes: list[str] = []
    for index in range(clock_probe.CLOCK_SAMPLE_COUNT):
        sent = base + index * 10_000_000
        times.extend((sent, sent + 1_000_000))
        remotes.append(str(sent + 500_000))
    responses = iter(
        ["localhost.localdomain\nyes\nyes", *remotes]
    )
    time_values = iter(times)
    monkeypatch.setattr(clock_probe, "_run_ssh", lambda _command: next(responses))
    monkeypatch.setattr(clock_probe.time, "time_ns", lambda: next(time_values))
    evidence = clock_probe.build_evidence(
        ssh_config=ssh_config,
        known_hosts=known_hosts,
        target="orin",
        local_hostname_file=hostname,
        local_sync_marker=marker,
    )
    normalized, passed = clock_probe.validate_clock_evidence(evidence)
    assert passed is True
    assert normalized["sample_count"] == clock_probe.CLOCK_SAMPLE_COUNT
    assert normalized["maximum_abs_midpoint_offset_ns"] == 0
    assert normalized["maximum_round_trip_ns"] == 1_000_000
