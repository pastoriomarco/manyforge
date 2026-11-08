#!/usr/bin/env python3
"""Verify the applied target RT resource profile from Linux /proc state.

This is qualification-only NRT tooling. It inspects the already-running
Composer or controller_manager process and compares actual thread scheduler,
priority, affinity, and memory-lock state with the strict profile inherited by
that process. Requests alone are never accepted as evidence.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
from typing import Any, Mapping


EVIDENCE_PREFIX = "MANYFORGE_QUALIFICATION_EVIDENCE "
SCHEMA_VERSION = "manyforge.realtime_resource_profile.v1"
MAXIMUM_CPU_INDEX = 1023


@dataclass(frozen=True)
class ThreadObservation:
    tid: int
    name: str
    policy: str
    priority: int
    affinity: tuple[int, ...]


@dataclass(frozen=True)
class ProcessObservation:
    pid: int
    command_line: str
    locked_memory_bytes: int
    main_policy: str
    main_priority: int
    main_affinity: tuple[int, ...]
    threads: tuple[ThreadObservation, ...]


def _scheduler_name(policy: int) -> str:
    names = {
        getattr(os, "SCHED_OTHER", object()): "SCHED_OTHER",
        getattr(os, "SCHED_FIFO", object()): "SCHED_FIFO",
        getattr(os, "SCHED_RR", object()): "SCHED_RR",
        getattr(os, "SCHED_BATCH", object()): "SCHED_BATCH",
        getattr(os, "SCHED_IDLE", object()): "SCHED_IDLE",
    }
    return str(names.get(policy, f"policy_{policy}"))


def _parse_cpu_set(value: str, *, name: str) -> tuple[int, ...]:
    cpus: set[int] = set()
    for raw_token in value.split(","):
        token = raw_token.strip()
        if not token:
            raise ValueError(f"{name} contains an empty CPU token")
        if "-" not in token:
            cpu = int(token)
            if cpu < 0 or cpu > MAXIMUM_CPU_INDEX:
                raise ValueError(
                    f"{name} CPU must be in [0, {MAXIMUM_CPU_INDEX}]"
                )
            cpus.add(cpu)
            continue
        pieces = token.split("-")
        if len(pieces) != 2:
            raise ValueError(f"{name} contains an invalid CPU range: {token}")
        first, last = (int(piece) for piece in pieces)
        if first < 0 or last < first:
            raise ValueError(f"{name} contains a descending CPU range: {token}")
        if last > MAXIMUM_CPU_INDEX:
            raise ValueError(
                f"{name} CPU must be in [0, {MAXIMUM_CPU_INDEX}]"
            )
        cpus.update(range(first, last + 1))
    if not cpus:
        raise ValueError(f"{name} must select at least one CPU")
    return tuple(sorted(cpus))


def _read_environ(path: Path) -> dict[str, str]:
    raw = path.read_bytes()
    result: dict[str, str] = {}
    for item in raw.split(b"\0"):
        if not item or b"=" not in item:
            continue
        key, value = item.split(b"=", 1)
        result[key.decode("utf-8", errors="replace")] = value.decode(
            "utf-8", errors="replace"
        )
    return result


def _locked_memory_bytes(status_path: Path) -> int:
    for line in status_path.read_text(encoding="utf-8").splitlines():
        if line.startswith("VmLck:"):
            fields = line.split()
            if len(fields) >= 2:
                return int(fields[1]) * 1024
    raise RuntimeError(f"VmLck is absent from {status_path}")


def _read_optional_text(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return None


def host_resource_envelope() -> dict[str, Any]:
    """Record host-wide facts that per-thread affinity cannot establish.

    A successful strict process profile proves that Linux accepted the named
    thread requests. It does not imply a PREEMPT_RT kernel, exclusive cpusets,
    IRQ isolation, or full-tickless CPUs. Keep those facts in the same artifact
    so simulation evidence cannot be promoted into a hard-RT deployment claim.
    """
    uname = os.uname()
    kernel_realtime = _read_optional_text(Path("/sys/kernel/realtime"))
    isolated_cpus = _read_optional_text(
        Path("/sys/devices/system/cpu/isolated")
    )
    nohz_full_cpus = _read_optional_text(
        Path("/sys/devices/system/cpu/nohz_full")
    )
    return {
        "kernel_release": uname.release,
        "kernel_version": uname.version,
        "kernel_realtime_flag": kernel_realtime,
        "preempt_rt_reported": (
            kernel_realtime == "1" or "PREEMPT_RT" in uname.version
        ),
        "kernel_command_line": _read_optional_text(Path("/proc/cmdline")),
        "online_cpus": _read_optional_text(
            Path("/sys/devices/system/cpu/online")
        ),
        "isolated_cpus": isolated_cpus,
        "nohz_full_cpus": nohz_full_cpus,
        "exclusive_rt_cpu_isolation_reported": bool(
            isolated_cpus or nohz_full_cpus
        ),
        "cgroup_effective_cpus": _read_optional_text(
            Path("/sys/fs/cgroup/cpuset.cpus.effective")
        ),
    }


def _observe_thread(pid: int, tid: int, *, proc_root: Path) -> ThreadObservation:
    task_root = proc_root / str(pid) / "task" / str(tid)
    name = (task_root / "comm").read_text(encoding="utf-8").strip()
    policy = _scheduler_name(os.sched_getscheduler(tid))
    priority = int(os.sched_getparam(tid).sched_priority)
    affinity = tuple(sorted(os.sched_getaffinity(tid)))
    return ThreadObservation(tid, name, policy, priority, affinity)


def observe_process(pid: int, *, proc_root: Path = Path("/proc")) -> ProcessObservation:
    process_root = proc_root / str(pid)
    command_line = (
        (process_root / "cmdline")
        .read_bytes()
        .replace(b"\0", b" ")
        .decode("utf-8", errors="replace")
        .strip()
    )
    thread_ids = sorted(
        int(path.name)
        for path in (process_root / "task").iterdir()
        if path.name.isdigit()
    )
    threads = tuple(
        _observe_thread(pid, tid, proc_root=proc_root) for tid in thread_ids
    )
    return ProcessObservation(
        pid=pid,
        command_line=command_line,
        locked_memory_bytes=_locked_memory_bytes(process_root / "status"),
        main_policy=_scheduler_name(os.sched_getscheduler(pid)),
        main_priority=int(os.sched_getparam(pid).sched_priority),
        main_affinity=tuple(sorted(os.sched_getaffinity(pid))),
        threads=threads,
    )


def _ancestor_pids(pid: int, *, proc_root: Path) -> set[int]:
    ancestors: set[int] = set()
    current = pid
    while current > 1:
        status_path = proc_root / str(current) / "status"
        try:
            lines = status_path.read_text(encoding="utf-8").splitlines()
        except OSError:
            break
        parent = 0
        for line in lines:
            if line.startswith("PPid:"):
                parent = int(line.split()[1])
                break
        if parent <= 0 or parent in ancestors:
            break
        ancestors.add(parent)
        current = parent
    return ancestors


def find_process(
    command_line_token: str,
    *,
    proc_root: Path = Path("/proc"),
    caller_pid: int | None = None,
) -> int:
    matches: list[int] = []
    caller = os.getpid() if caller_pid is None else caller_pid
    excluded = {caller, *_ancestor_pids(caller, proc_root=proc_root)}
    for path in proc_root.iterdir():
        if not path.name.isdigit():
            continue
        if int(path.name) in excluded:
            continue
        try:
            command_line = (
                (path / "cmdline")
                .read_bytes()
                .replace(b"\0", b" ")
                .decode("utf-8", errors="replace")
            )
        except OSError:
            continue
        if command_line_token in command_line:
            matches.append(int(path.name))
    if len(matches) != 1:
        raise RuntimeError(
            f"expected one process containing {command_line_token!r}; "
            f"found {matches}"
        )
    return matches[0]


def _required_int(environment: Mapping[str, str], name: str) -> int:
    try:
        value = int(environment[name])
    except KeyError as exc:
        raise ValueError(f"target process is missing {name}") from exc
    if value < 1 or value > 99:
        raise ValueError(f"{name} must be in [1, 99]")
    return value


def _required_cpu_set(
    environment: Mapping[str, str],
    name: str,
) -> tuple[int, ...]:
    try:
        value = environment[name]
    except KeyError as exc:
        raise ValueError(f"target process is missing {name}") from exc
    return _parse_cpu_set(value, name=name)


def _expected_policy(environment: Mapping[str, str]) -> str:
    value = environment.get("MANYFORGE_RT_SCHED_POLICY", "FIFO").strip().upper()
    if value in {"FIFO", "SCHED_FIFO"}:
        return "SCHED_FIFO"
    if value in {"RR", "SCHED_RR"}:
        return "SCHED_RR"
    raise ValueError("MANYFORGE_RT_SCHED_POLICY must be FIFO or RR")


def evaluate_composer(
    observation: ProcessObservation,
    environment: Mapping[str, str],
) -> tuple[list[str], dict[str, Any]]:
    expected_nrt = _required_cpu_set(environment, "MANYFORGE_NRT_CPU_AFFINITY")
    policy = _expected_policy(environment)
    role_specs = {
        "controller_command": (
            "mf_sched_ctrl",
            _required_int(environment, "MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY"),
            _required_cpu_set(
                environment, "MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY"
            ),
        ),
        "runtime_safety": (
            "mf_sched_safe",
            _required_int(environment, "MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY"),
            _required_cpu_set(
                environment, "MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY"
            ),
        ),
        "stop_control": (
            "mf_sched_stop",
            _required_int(environment, "MANYFORGE_RT_STOP_CONTROL_PRIORITY"),
            _required_cpu_set(
                environment, "MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY"
            ),
        ),
    }
    failures: list[str] = []
    role_evidence: dict[str, Any] = {}
    named_rt_threads: set[int] = set()
    for role, (thread_name, priority, affinity) in role_specs.items():
        matches = [thread for thread in observation.threads if thread.name == thread_name]
        if len(matches) != 1:
            failures.append(
                f"{role}: expected one {thread_name} thread, found {len(matches)}"
            )
            role_evidence[role] = {"expected_thread_name": thread_name, "actual": []}
            continue
        thread = matches[0]
        named_rt_threads.add(thread.tid)
        accepted = (
            thread.policy == policy
            and thread.priority == priority
            and thread.affinity == affinity
        )
        if not accepted:
            failures.append(
                f"{role}: expected {policy}/{priority}/{affinity}, got "
                f"{thread.policy}/{thread.priority}/{thread.affinity}"
            )
        role_evidence[role] = {
            "expected": {
                "thread_name": thread_name,
                "policy": policy,
                "priority": priority,
                "affinity": list(affinity),
            },
            "actual": asdict(thread),
            "accepted": accepted,
        }

    # OD-59: the strict profile locks the execution host's memory.
    memory_locked = observation.locked_memory_bytes > 0
    if not memory_locked:
        failures.append("Composer strict profile requires locked memory but VmLck is zero")
    if observation.main_policy != "SCHED_OTHER" or observation.main_priority != 0:
        failures.append("Composer main/Python thread must remain SCHED_OTHER/0")
    if observation.main_affinity != expected_nrt:
        failures.append(
            f"Composer main affinity expected {expected_nrt}, got "
            f"{observation.main_affinity}"
        )
    unexpected_rt = [
        thread
        for thread in observation.threads
        if thread.tid not in named_rt_threads and thread.policy != "SCHED_OTHER"
    ]
    if unexpected_rt:
        failures.append(
            "unowned Composer RT threads: "
            + ", ".join(f"{item.name}:{item.tid}" for item in unexpected_rt)
        )
    nrt_outside_reservation = [
        thread
        for thread in observation.threads
        if thread.tid not in named_rt_threads and thread.affinity != expected_nrt
    ]
    if nrt_outside_reservation:
        failures.append(
            "Composer NRT threads escaped the NRT CPU set: "
            + ", ".join(
                f"{item.name}:{item.tid}:{item.affinity}"
                for item in nrt_outside_reservation
            )
        )
    return failures, {
        "roles": role_evidence,
        "expected_nrt_affinity": list(expected_nrt),
        "unexpected_rt_threads": [asdict(item) for item in unexpected_rt],
        "nrt_threads_outside_reservation": [
            asdict(item) for item in nrt_outside_reservation
        ],
        "process_wide_memory_lock_requested": True,
        "memory_lock_accepted": memory_locked,
    }


def evaluate_controller_manager(
    observation: ProcessObservation,
    environment: Mapping[str, str],
) -> tuple[list[str], dict[str, Any]]:
    policy = _expected_policy(environment)
    priority = _required_int(
        environment, "MANYFORGE_CONTROLLER_MANAGER_THREAD_PRIORITY"
    )
    affinity = _required_cpu_set(
        environment, "MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY"
    )
    expected_nrt = _required_cpu_set(environment, "MANYFORGE_NRT_CPU_AFFINITY")
    matching_threads = [
        thread
        for thread in observation.threads
        if thread.policy == policy
        and thread.priority == priority
        and thread.affinity == affinity
    ]
    failures: list[str] = []
    if len(matching_threads) != 1:
        failures.append(
            "controller_manager: expected exactly one qualified update thread; "
            f"found {len(matching_threads)}"
        )
    update_thread_ids = {thread.tid for thread in matching_threads}
    if observation.main_policy != "SCHED_OTHER" or observation.main_priority != 0:
        failures.append("controller_manager main thread must remain SCHED_OTHER/0")
    if observation.main_affinity != expected_nrt:
        failures.append(
            f"controller_manager main affinity expected {expected_nrt}, got "
            f"{observation.main_affinity}"
        )
    unexpected_rt_threads = [
        thread
        for thread in observation.threads
        if thread.tid not in update_thread_ids and thread.policy != "SCHED_OTHER"
    ]
    if unexpected_rt_threads:
        failures.append(
            "controller_manager contains unowned RT threads: "
            + ", ".join(
                f"{item.name}:{item.tid}" for item in unexpected_rt_threads
            )
        )
    nrt_outside_reservation = [
        thread
        for thread in observation.threads
        if thread.tid not in update_thread_ids and thread.affinity != expected_nrt
    ]
    if nrt_outside_reservation:
        failures.append(
            "controller_manager NRT threads escaped the NRT CPU set: "
            + ", ".join(
                f"{item.name}:{item.tid}:{item.affinity}"
                for item in nrt_outside_reservation
            )
        )
    higher_rt_threads = [
        thread
        for thread in observation.threads
        if thread.policy in {"SCHED_FIFO", "SCHED_RR"}
        and thread.priority > priority
    ]
    if higher_rt_threads:
        failures.append("controller_manager contains an unowned higher-priority RT thread")
    # OD-59: the strict profile (its one source of truth) sets the stock
    # `lock_memory: true` on every lane's controller manager, which upstream
    # only warns about when it fails.
    lock_requested = True
    lock_accepted = observation.locked_memory_bytes > 0
    if not lock_accepted:
        failures.append("controller_manager requested memory locking but VmLck is zero")
    return failures, {
        "expected": {
            "policy": policy,
            "priority": priority,
            "affinity": list(affinity),
            "memory_lock_requested": lock_requested,
        },
        "matching_update_threads": [asdict(item) for item in matching_threads],
        "higher_priority_rt_threads": [asdict(item) for item in higher_rt_threads],
        "expected_nrt_affinity": list(expected_nrt),
        "unexpected_rt_threads": [
            asdict(item) for item in unexpected_rt_threads
        ],
        "nrt_threads_outside_reservation": [
            asdict(item) for item in nrt_outside_reservation
        ],
        "memory_lock_accepted": lock_accepted,
    }


def build_evidence(
    target: str,
    observation: ProcessObservation,
    environment: Mapping[str, str],
) -> dict[str, Any]:
    if environment.get("MANYFORGE_RT_RESOURCE_PROFILE") != "strict":
        raise ValueError("target process does not declare the strict RT profile")
    if target == "composer":
        failures, contract = evaluate_composer(observation, environment)
    elif target == "controller-manager":
        failures, contract = evaluate_controller_manager(observation, environment)
    else:
        raise ValueError(f"unknown target: {target}")
    return {
        "schema_version": SCHEMA_VERSION,
        "target": target,
        "passed": not failures,
        "failed_checks": failures,
        "contract": contract,
        "host_resource_envelope": host_resource_envelope(),
        "process": {
            "pid": observation.pid,
            "command_line": observation.command_line,
            "locked_memory_bytes": observation.locked_memory_bytes,
            "main_policy": observation.main_policy,
            "main_priority": observation.main_priority,
            "main_affinity": list(observation.main_affinity),
            "thread_count": len(observation.threads),
            "threads": [asdict(thread) for thread in observation.threads],
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--target",
        required=True,
        choices=("composer", "controller-manager"),
    )
    selector = parser.add_mutually_exclusive_group(required=True)
    selector.add_argument("--pid", type=int)
    selector.add_argument("--command-line-token")
    parser.add_argument("--json-only", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    try:
        pid = args.pid or find_process(args.command_line_token)
        process_root = Path("/proc") / str(pid)
        environment = _read_environ(process_root / "environ")
        evidence = build_evidence(
            args.target,
            observe_process(pid),
            environment,
        )
    except Exception as exc:  # noqa: BLE001 - malformed evidence fails closed.
        evidence = {
            "schema_version": SCHEMA_VERSION,
            "target": args.target,
            "passed": False,
            "failed_checks": [f"probe_failed:{type(exc).__name__}:{exc}"],
        }

    encoded = json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
    if args.json_only:
        print(encoded)
    else:
        print(f"{EVIDENCE_PREFIX}{encoded}")
    return 0 if evidence["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
