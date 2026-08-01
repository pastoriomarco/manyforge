"""Portable cycle vocabulary and the I6/N + I8/P5 runtime cutover gate."""

from pathlib import Path
import re

import pytest

from manyforge_behavior import cycle_manager
from manyforge_behavior.cycle_manager import (
    CYCLE_MANAGER_RESTORATION_GATES,
    ROS2_CONTROL_BACKEND_FAULT_CLASSES,
    CycleManager,
    CycleManagerUnavailableError,
    RecoveryTrigger,
    canonical_recovery_trigger_values,
    preferred_structured_error_code,
)


_CANONICAL_RECOVERY_TRIGGERS = {
    "planning_failed",
    "validation_failed",
    "execute_failed",
    "execute_timeout",
    "scene_mismatch",
    "precondition_failed",
    "postcondition_failed",
}


def test_python_cycle_module_exports_no_singleton_lifecycle_enum() -> None:
    assert not hasattr(cycle_manager, "CycleState")


def test_recovery_trigger_vocabulary_is_closed_and_has_no_legacy_aliases() -> None:
    assert canonical_recovery_trigger_values() == _CANONICAL_RECOVERY_TRIGGERS
    assert {trigger.value for trigger in RecoveryTrigger} == (
        _CANONICAL_RECOVERY_TRIGGERS
    )
    with pytest.raises(ValueError):
        RecoveryTrigger("timeout")
    with pytest.raises(ValueError):
        RecoveryTrigger("node_failure")
    with pytest.raises(ValueError):
        RecoveryTrigger("made_up_reason")


def test_error_projection_promotes_only_known_reason_from_generic_code() -> None:
    known = "controller_cancel_timeout"
    assert preferred_structured_error_code(
        "execution_aborted",
        {"reason_code": known},
    ) == known
    assert preferred_structured_error_code(
        "runtime_collision",
        {"reason_code": known},
    ) == "runtime_collision"
    assert preferred_structured_error_code(
        "execution_aborted",
        {"reason_code": "untrusted_free_form"},
    ) == "execution_aborted"


def test_fault_taxonomy_matches_the_cpp_emitter() -> None:
    workspace_root = Path(__file__).parent.parent.parent
    candidates = (
        workspace_root
        / "manyforge_ros2_control_execution"
        / "include"
        / "manyforge_ros2_control_execution"
        / "ros2_control_capability.hpp",
        workspace_root
        / "manyforge_roboplan"
        / "include"
        / "manyforge_roboplan"
        / "ros2_control_capability.hpp",
    )
    header = next((candidate for candidate in candidates if candidate.is_file()), None)
    assert header is not None
    match = re.search(
        r"namespace ros2_control_fault_reason \{(.*?)\}\s*//\s*namespace"
        r"\s+ros2_control_fault_reason",
        header.read_text(encoding="utf-8"),
        flags=re.DOTALL,
    )
    assert match is not None
    assert set(re.findall(r'"([a-z0-9_]+)"', match.group(1))) == set(
        ROS2_CONTROL_BACKEND_FAULT_CLASSES
    )


def test_fault_aliases_and_severities_are_bounded() -> None:
    aliases = [
        fault.external_alias for fault in ROS2_CONTROL_BACKEND_FAULT_CLASSES.values()
    ]
    assert len(aliases) == len(set(aliases))
    for fault in ROS2_CONTROL_BACKEND_FAULT_CLASSES.values():
        assert re.fullmatch(r"[A-Z][A-Z0-9_]*", fault.external_alias)
        assert len(fault.external_alias) <= 64
        assert fault.severity in {"INFO", "WARN", "ERROR", "CRITICAL"}


def test_cycle_manager_fails_before_runner_or_kernel_access() -> None:
    class PoisonCollaborator:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"cycle collaborator was accessed: {name}")

    poison = PoisonCollaborator()
    with pytest.raises(
        CycleManagerUnavailableError,
        match="composable_cell_cycle_manager_unavailable",
    ):
        CycleManager(poison, poison, event_sink=poison)

    assert CYCLE_MANAGER_RESTORATION_GATES == ("I6", "N", "I8", "P5")
