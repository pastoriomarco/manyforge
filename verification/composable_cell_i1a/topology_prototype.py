"""Bounded I1a process-placement and failure-isolation evidence.

This is not a launcher, process manager, or supported topology.  It makes the
selected semantic restart boundaries executable without choosing a final
container packaging or introducing a process per controllable component.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
import hashlib
import json
from pathlib import Path
import select
import subprocess
import sys
from typing import TextIO


class RuntimeRole(str, Enum):
    COMPONENT_MANAGER = "component_manager"
    COMPILER_WORKER = "compiler_worker"
    EXECUTION_HOST = "execution_host"
    PROGRAM_RUNNER = "program_runner"
    HEADLESS_STATE_API = "headless_state_alarm_api"
    CONTROL_DOMAIN = "control_domain"


@dataclass(frozen=True, slots=True)
class RuntimePlacement:
    runtime_unit_id: str
    role: RuntimeRole
    process_boundary: str
    managed_by_component_manager: bool
    bounded_lifetime: bool = False


@dataclass(frozen=True, slots=True)
class ControlDomainPlacement:
    control_domain_id: str
    runtime_unit_id: str
    controllable_component_ids: tuple[str, ...]

    def __post_init__(self) -> None:
        if not self.controllable_component_ids:
            raise ValueError("a control domain must contain at least one component")
        if len(set(self.controllable_component_ids)) != len(
            self.controllable_component_ids
        ):
            raise ValueError("control-domain components must be unique")


@dataclass(frozen=True, slots=True)
class TopologyCandidate:
    placements: tuple[RuntimePlacement, ...]
    control_domains: tuple[ControlDomainPlacement, ...]

    def validate(self) -> None:
        unit_ids = tuple(item.runtime_unit_id for item in self.placements)
        if len(set(unit_ids)) != len(unit_ids):
            raise ValueError("runtime-unit identities must be unique")
        by_role: dict[RuntimeRole, list[RuntimePlacement]] = {}
        for item in self.placements:
            by_role.setdefault(item.role, []).append(item)
        for role in (
            RuntimeRole.COMPONENT_MANAGER,
            RuntimeRole.COMPILER_WORKER,
            RuntimeRole.EXECUTION_HOST,
            RuntimeRole.PROGRAM_RUNNER,
            RuntimeRole.HEADLESS_STATE_API,
        ):
            if len(by_role.get(role, ())) != 1:
                raise ValueError(f"topology requires exactly one {role.value}")

        manager = by_role[RuntimeRole.COMPONENT_MANAGER][0]
        if manager.managed_by_component_manager:
            raise ValueError("component manager cannot manage itself")
        compiler = by_role[RuntimeRole.COMPILER_WORKER][0]
        if not compiler.bounded_lifetime:
            raise ValueError("compiler worker must have bounded lifetime")

        independently_restartable = (
            by_role[RuntimeRole.EXECUTION_HOST][0],
            by_role[RuntimeRole.PROGRAM_RUNNER][0],
            by_role[RuntimeRole.HEADLESS_STATE_API][0],
        )
        boundaries = tuple(item.process_boundary for item in independently_restartable)
        if len(set(boundaries)) != len(boundaries):
            raise ValueError(
                "execution host, program runner, and headless state/API "
                "must have independent process boundaries"
            )

        placement_ids = set(unit_ids)
        domain_ids: set[str] = set()
        component_ids: set[str] = set()
        for domain in self.control_domains:
            if domain.control_domain_id in domain_ids:
                raise ValueError("control-domain identities must be unique")
            domain_ids.add(domain.control_domain_id)
            if domain.runtime_unit_id not in placement_ids:
                raise ValueError("control domain references unknown runtime unit")
            placement = next(
                item
                for item in self.placements
                if item.runtime_unit_id == domain.runtime_unit_id
            )
            if placement.role is not RuntimeRole.CONTROL_DOMAIN:
                raise ValueError("control domain must use a control-domain runtime")
            overlap = component_ids.intersection(domain.controllable_component_ids)
            if overlap:
                raise ValueError("component cannot belong to two control domains")
            component_ids.update(domain.controllable_component_ids)


def selected_topology_candidate() -> TopologyCandidate:
    """Return the bounded topology selected for I1a failure testing."""

    candidate = TopologyCandidate(
        placements=(
            RuntimePlacement(
                "component-manager",
                RuntimeRole.COMPONENT_MANAGER,
                "process.manager",
                managed_by_component_manager=False,
            ),
            RuntimePlacement(
                "compiler-worker",
                RuntimeRole.COMPILER_WORKER,
                "process.compiler",
                managed_by_component_manager=True,
                bounded_lifetime=True,
            ),
            RuntimePlacement(
                "execution-host",
                RuntimeRole.EXECUTION_HOST,
                "process.execution",
                managed_by_component_manager=True,
            ),
            RuntimePlacement(
                "program-runner",
                RuntimeRole.PROGRAM_RUNNER,
                "process.program",
                managed_by_component_manager=True,
            ),
            RuntimePlacement(
                "headless-state-api",
                RuntimeRole.HEADLESS_STATE_API,
                "process.headless",
                managed_by_component_manager=True,
            ),
            RuntimePlacement(
                "control-domain-a",
                RuntimeRole.CONTROL_DOMAIN,
                "process.control-a",
                managed_by_component_manager=True,
            ),
        ),
        control_domains=(
            ControlDomainPlacement(
                control_domain_id="domain-a",
                runtime_unit_id="control-domain-a",
                controllable_component_ids=(
                    "component.arm-a",
                    "component.tool-a",
                ),
            ),
        ),
    )
    candidate.validate()
    return candidate


@dataclass(frozen=True, slots=True)
class ProcessProbeMeasurement:
    started_roles: tuple[RuntimeRole, ...]
    live_roles: tuple[RuntimeRole, ...]
    process_count: int
    aggregate_rss_kib: int


_IDLE_WORKER = (
    "import sys\n"
    "sys.stdout.write('ready\\n')\n"
    "sys.stdout.flush()\n"
    "sys.stdin.read()\n"
)


class ProcessIsolationProbe:
    """Launch disposable sibling processes and observe failure independence."""

    def __init__(self) -> None:
        self._processes: dict[RuntimeRole, subprocess.Popen[str]] = {}

    def __enter__(self) -> ProcessIsolationProbe:
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()

    def start(self, role: RuntimeRole) -> int:
        if role in self._processes:
            raise ValueError(f"{role.value} already started")
        process = subprocess.Popen(
            (sys.executable, "-c", _IDLE_WORKER),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        assert process.stdout is not None
        ready, _, _ = select.select((process.stdout,), (), (), 5.0)
        if not ready or process.stdout.readline().strip() != "ready":
            process.kill()
            process.wait(timeout=5)
            raise RuntimeError(f"{role.value} did not become ready")
        self._processes[role] = process
        return process.pid

    def terminate(self, role: RuntimeRole) -> None:
        process = self._processes[role]
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)

    def alive(self, role: RuntimeRole) -> bool:
        return self._processes[role].poll() is None

    def run_bounded_compiler(self, value: object) -> tuple[str, int]:
        payload = json.dumps(value, sort_keys=True, separators=(",", ":"))
        code = (
            "import hashlib,sys\n"
            "payload=sys.stdin.read().encode()\n"
            "sys.stdout.write(hashlib.sha256(payload).hexdigest())\n"
        )
        completed = subprocess.run(
            (sys.executable, "-c", code),
            input=payload,
            text=True,
            capture_output=True,
            check=True,
            timeout=5,
        )
        expected = hashlib.sha256(payload.encode()).hexdigest()
        if completed.stdout != expected:
            raise RuntimeError("bounded compiler result mismatch")
        return completed.stdout, completed.returncode

    def measurement(self) -> ProcessProbeMeasurement:
        live = tuple(
            role for role in RuntimeRole if role in self._processes and self.alive(role)
        )
        rss = sum(
            _read_rss_kib(process.pid)
            for process in self._processes.values()
            if process.poll() is None
        )
        return ProcessProbeMeasurement(
            started_roles=tuple(
                role for role in RuntimeRole if role in self._processes
            ),
            live_roles=live,
            process_count=len(live),
            aggregate_rss_kib=rss,
        )

    def close(self) -> None:
        for process in self._processes.values():
            if process.poll() is None:
                if process.stdin is not None:
                    process.stdin.close()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2)


def _read_rss_kib(process_id: int) -> int:
    status = Path(f"/proc/{process_id}/status").read_text(encoding="utf-8")
    for line in status.splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    raise RuntimeError(f"VmRSS missing for process {process_id}")
