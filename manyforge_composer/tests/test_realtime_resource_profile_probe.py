from __future__ import annotations

import importlib.util
from pathlib import Path
from typing import Any


_REPO_ROOT = Path(__file__).resolve().parents[2]
_PROBE = (
    _REPO_ROOT
    / "manyforge_core"
    / "tools"
    / "realtime_resource_profile_probe.py"
)


def _load_probe() -> Any:
    spec = importlib.util.spec_from_file_location(
        "manyforge_realtime_resource_profile_probe_test",
        _PROBE,
    )
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    # Dataclasses resolve the module through sys.modules while decorating.
    import sys

    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _environment() -> dict[str, str]:
    return {
        "MANYFORGE_RT_RESOURCE_PROFILE": "strict",
        "MANYFORGE_NRT_CPU_AFFINITY": "0-1",
        "MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY": "4",
        "MANYFORGE_CONTROLLER_MANAGER_THREAD_PRIORITY": "50",
        "MANYFORGE_RT_SCHED_POLICY": "FIFO",
        "MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY": "45",
        "MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY": "2",
        "MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY": "40",
        "MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY": "3",
        "MANYFORGE_RT_STOP_CONTROL_PRIORITY": "48",
        "MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY": "5",
    }


def test_composer_probe_accepts_only_named_rt_threads_and_nrt_confinement():
    probe = _load_probe()
    observation = probe.ProcessObservation(
        pid=1,
        command_line="python -m manyforge_composer",
        locked_memory_bytes=64 * 1024 * 1024,
        main_policy="SCHED_OTHER",
        main_priority=0,
        main_affinity=(0, 1),
        threads=(
            probe.ThreadObservation(1, "python", "SCHED_OTHER", 0, (0, 1)),
            probe.ThreadObservation(2, "mf_sched_ctrl", "SCHED_FIFO", 45, (2,)),
            probe.ThreadObservation(3, "mf_sched_safe", "SCHED_FIFO", 40, (3,)),
            probe.ThreadObservation(4, "mf_sched_stop", "SCHED_FIFO", 48, (5,)),
            probe.ThreadObservation(5, "worker", "SCHED_OTHER", 0, (0, 1)),
        ),
    )

    evidence = probe.build_evidence("composer", observation, _environment())

    assert evidence["passed"] is True
    assert evidence["contract"]["roles"]["runtime_safety"]["accepted"] is True


def test_composer_probe_rejects_python_thread_on_rt_policy_or_reserved_cpu():
    probe = _load_probe()
    observation = probe.ProcessObservation(
        pid=1,
        command_line="python -m manyforge_composer",
        locked_memory_bytes=0,
        main_policy="SCHED_OTHER",
        main_priority=0,
        main_affinity=(0, 1),
        threads=(
            probe.ThreadObservation(1, "python", "SCHED_OTHER", 0, (0, 1)),
            probe.ThreadObservation(2, "mf_sched_ctrl", "SCHED_FIFO", 45, (2,)),
            probe.ThreadObservation(3, "mf_sched_safe", "SCHED_FIFO", 40, (3,)),
            probe.ThreadObservation(4, "mf_sched_stop", "SCHED_FIFO", 48, (5,)),
            probe.ThreadObservation(5, "python", "SCHED_FIFO", 30, (2,)),
        ),
    )

    evidence = probe.build_evidence("composer", observation, _environment())

    assert evidence["passed"] is False
    assert any("unowned Composer RT threads" in item for item in evidence["failed_checks"])
    assert any("escaped the NRT CPU set" in item for item in evidence["failed_checks"])
    # OD-59: the strict profile's execution host runs with locked memory.
    assert any("VmLck is zero" in item for item in evidence["failed_checks"])


def test_controller_probe_requires_one_highest_update_thread_and_memory_lock():
    probe = _load_probe()
    observation = probe.ProcessObservation(
        pid=99,
        command_line="/opt/ros/jazzy/lib/controller_manager/ros2_control_node",
        locked_memory_bytes=32 * 1024 * 1024,
        main_policy="SCHED_OTHER",
        main_priority=0,
        main_affinity=(0, 1),
        threads=(
            probe.ThreadObservation(
                99, "ros2_control_no", "SCHED_OTHER", 0, (0, 1)
            ),
            probe.ThreadObservation(108, "ros2_control_no", "SCHED_FIFO", 50, (4,)),
            probe.ThreadObservation(109, "dds", "SCHED_OTHER", 0, (0, 1)),
        ),
    )

    evidence = probe.build_evidence(
        "controller-manager", observation, _environment()
    )

    assert evidence["passed"] is True
    assert len(evidence["contract"]["matching_update_threads"]) == 1


def test_controller_probe_rejects_unowned_rt_or_nrt_cpu_escape():
    probe = _load_probe()
    observation = probe.ProcessObservation(
        pid=99,
        command_line="/opt/ros/jazzy/lib/controller_manager/ros2_control_node",
        locked_memory_bytes=32 * 1024 * 1024,
        main_policy="SCHED_OTHER",
        main_priority=0,
        main_affinity=(0, 1),
        threads=(
            probe.ThreadObservation(99, "ros2_control_no", "SCHED_OTHER", 0, (0, 1)),
            probe.ThreadObservation(108, "ros2_control_no", "SCHED_FIFO", 50, (4,)),
            probe.ThreadObservation(109, "dds", "SCHED_FIFO", 20, (0, 1)),
            probe.ThreadObservation(110, "worker", "SCHED_OTHER", 0, (4,)),
        ),
    )

    evidence = probe.build_evidence(
        "controller-manager", observation, _environment()
    )

    assert evidence["passed"] is False
    assert any("unowned RT threads" in item for item in evidence["failed_checks"])
    assert any("escaped the NRT CPU set" in item for item in evidence["failed_checks"])


def test_controller_probe_rejects_unlocked_or_lower_priority_runtime():
    probe = _load_probe()
    observation = probe.ProcessObservation(
        pid=99,
        command_line="ros2_control_node",
        locked_memory_bytes=0,
        main_policy="SCHED_OTHER",
        main_priority=0,
        main_affinity=(4,),
        threads=(
            probe.ThreadObservation(99, "ros2_control_no", "SCHED_OTHER", 0, (4,)),
            probe.ThreadObservation(108, "ros2_control_no", "SCHED_FIFO", 40, (4,)),
        ),
    )

    evidence = probe.build_evidence(
        "controller-manager", observation, _environment()
    )

    assert evidence["passed"] is False
    assert any("expected exactly one" in item for item in evidence["failed_checks"])
    assert any("VmLck is zero" in item for item in evidence["failed_checks"])


def test_process_lookup_excludes_probe_and_shell_ancestor_tokens(tmp_path):
    probe = _load_probe()

    def add_process(pid: int, parent: int, command: str) -> None:
        root = tmp_path / str(pid)
        root.mkdir()
        (root / "status").write_text(
            f"Name:\ttest\nPPid:\t{parent}\n",
            encoding="utf-8",
        )
        (root / "cmdline").write_bytes(command.encode() + b"\0")

    add_process(10, 1, "bash -lc probe --command-line-token target")
    add_process(11, 10, "python probe --command-line-token target")
    add_process(20, 1, "actual target process")

    assert probe.find_process("target", proc_root=tmp_path, caller_pid=11) == 20
