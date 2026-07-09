#!/usr/bin/env python3
"""Emit RT-6 realtime architecture qualification evidence.

The probe is intentionally observational. It records the source-level
architecture contract and the OS realtime properties visible to this process;
deployment launchers are responsible for requesting SCHED_FIFO/RR, affinity, or
mlock. If a request is declared through environment variables, this probe marks
whether the running process actually received it.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
from typing import Any


EVIDENCE_PREFIX = "MANYFORGE_QUALIFICATION_EVIDENCE "
SCHEMA_VERSION = "manyforge.rt6_qualification.v1"


def _scheduler_name(policy: int | None) -> str:
    if policy is None:
        return "unknown"
    names = {
        getattr(os, "SCHED_OTHER", object()): "SCHED_OTHER",
        getattr(os, "SCHED_FIFO", object()): "SCHED_FIFO",
        getattr(os, "SCHED_RR", object()): "SCHED_RR",
        getattr(os, "SCHED_BATCH", object()): "SCHED_BATCH",
        getattr(os, "SCHED_IDLE", object()): "SCHED_IDLE",
    }
    return str(names.get(policy, f"policy_{policy}"))


def _current_scheduler_policy() -> tuple[int | None, str | None]:
    try:
        return os.sched_getscheduler(0), None
    except (AttributeError, OSError) as exc:
        return None, str(exc)


def _parse_cpu_set(value: str | None) -> list[int] | None:
    if value is None or not value.strip():
        return None
    cpus: set[int] = set()
    for part in value.split(","):
        token = part.strip()
        if not token:
            continue
        if "-" in token:
            start_text, end_text = token.split("-", 1)
            start = int(start_text)
            end = int(end_text)
            if end < start:
                raise ValueError(f"invalid descending CPU range: {token}")
            cpus.update(range(start, end + 1))
        else:
            cpus.add(int(token))
    return sorted(cpus)


def _current_affinity() -> tuple[list[int] | None, str | None]:
    try:
        return sorted(os.sched_getaffinity(0)), None
    except (AttributeError, OSError) as exc:
        return None, str(exc)


def _locked_memory_bytes() -> tuple[int | None, str | None]:
    status = Path("/proc/self/status")
    try:
        for line in status.read_text(encoding="utf-8").splitlines():
            if line.startswith("VmLck:"):
                fields = line.split()
                if len(fields) >= 2:
                    return int(fields[1]) * 1024, None
    except OSError as exc:
        return None, str(exc)
    return None, "VmLck not present"


def _requested_policy(env: dict[str, str]) -> str | None:
    value = env.get("MANYFORGE_RT6_REQUESTED_SCHED_POLICY")
    if value:
        return value.strip().upper()
    return None


def _requested_affinity(env: dict[str, str]) -> list[int] | None:
    return _parse_cpu_set(env.get("MANYFORGE_RT6_REQUESTED_CPU_AFFINITY"))


def _requested_mlock(env: dict[str, str]) -> bool:
    return env.get("MANYFORGE_RT6_REQUESTED_MLOCK", "").strip().lower() in {
        "1",
        "true",
        "yes",
        "on",
    }


def build_evidence(env: dict[str, str] | None = None) -> dict[str, Any]:
    env = dict(os.environ if env is None else env)
    policy_value, policy_error = _current_scheduler_policy()
    policy_name = _scheduler_name(policy_value)
    requested_policy = _requested_policy(env)
    affinity, affinity_error = _current_affinity()
    affinity_parse_error = None
    try:
        requested_affinity = _requested_affinity(env)
    except ValueError as exc:
        requested_affinity = None
        affinity_parse_error = str(exc)
    locked_bytes, locked_error = _locked_memory_bytes()
    requested_mlock = _requested_mlock(env)

    policy_accepted = (
        None if requested_policy is None else policy_name == requested_policy
    )
    if affinity_parse_error is not None:
        affinity_accepted = False
    elif requested_affinity is None:
        affinity_accepted = None
    elif affinity is None:
        affinity_accepted = False
    else:
        affinity_accepted = set(affinity) == set(requested_affinity)
    mlock_accepted = (
        None if not requested_mlock
        else False if locked_bytes is None
        else locked_bytes > 0
    )

    failed_checks = [
        name
        for name, accepted in {
            "scheduler_policy": policy_accepted,
            "cpu_affinity": affinity_accepted,
            "memory_locking": mlock_accepted,
        }.items()
        if accepted is False
    ]

    return {
        "schema_version": SCHEMA_VERSION,
        "passed": not failed_checks,
        "failed_checks": failed_checks,
        "source_level_architecture": {
            "scheduler_roles": [
                "controller_command",
                "runtime_safety",
                "stop_control",
                "lifecycle",
                "monitoring",
                "background",
            ],
            "highest_runtime_role": "controller_command",
            "runtime_collision_role": "runtime_safety",
            "stop_control_role": "stop_control",
            "python_routes_realtime_authority": False,
        },
        "collision_resources": {
            "resource_policy": "role_owned_collision_resources",
            "validation_runtime_concurrency_mode": (
                "independent_validation_resources_or_fail_closed_defer"
            ),
            "shared_single_context_runtime_validation": "fail_closed_deferred",
            "runtime_snapshot_coherence": (
                "world_policy_paired_bundle_observed_state_may_lead_with_budget"
            ),
        },
        "os_realtime": {
            "scheduler_policy": {
                "current": policy_name,
                "current_raw": policy_value,
                "requested": requested_policy,
                "accepted": policy_accepted,
                "error": policy_error,
            },
            "cpu_affinity": {
                "current": affinity,
                "requested": requested_affinity,
                "accepted": affinity_accepted,
                "error": affinity_parse_error or affinity_error,
            },
            "memory_locking": {
                "locked_bytes": locked_bytes,
                "requested": requested_mlock,
                "accepted": mlock_accepted,
                "error": locked_error,
            },
        },
        "environment_dependent_gates": {
            "native_loaded_stability_gate": "not_run_by_probe",
            "isaac_ros_loaded_gate": "not_run_by_probe",
            "strict_tsan": "not_run_by_probe",
            "strict_asan": "not_run_by_probe",
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Emit RT-6 realtime qualification evidence.",
    )
    parser.add_argument(
        "--json-only",
        action="store_true",
        help="Print raw JSON instead of a qualification-evidence line.",
    )
    args = parser.parse_args()
    payload = build_evidence()
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    if args.json_only:
        print(encoded)
    else:
        print(f"{EVIDENCE_PREFIX}{encoded}")
    return 0 if payload["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
