"""
External fail-closed supervisor for one real-robot container domain.

The supervisor lives outside Composer and the transport owner.  It never sends
robot commands and never restarts or re-enables a component.  Its only process
control authority is bounded dependency-ordered teardown after an unexpected
exit, plus an explicit graceful shutdown request from the launcher.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, UTC
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
from typing import Callable, Mapping, Protocol
import urllib.error
import urllib.request
import uuid

HEARTBEAT_SCHEMA = 'manyforge.cell_supervisor.heartbeat.v1'
STATE_SCHEMA = 'manyforge.cell_supervisor.state.v1'
POLICY_SCHEMA = 'manyforge.cell_supervisor.policy.v1'
_MAX_HEALTH_RESPONSE_BYTES = 256 * 1024


class ProcessControl(Protocol):
    """Replaceable deployment process-control port."""

    def running_many(self, names: tuple[str, ...]) -> Mapping[str, bool]: ...

    def stop(self, name: str, grace_seconds: int) -> bool: ...

    def force_remove(self, name: str) -> bool: ...


@dataclass(frozen=True, slots=True)
class DomainHealthObservation:
    """Receiver-local result of the reduced component-health channel."""

    status: str
    reason_codes: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if self.status not in {'starting', 'healthy', 'lost'}:
            raise ValueError('domain health status is invalid')


class DomainHealthProbe(Protocol):
    """Replaceable NRT health-evidence input for the process supervisor."""

    def bind_supervisor_epoch(self, component_epoch: str) -> None: ...

    def observe(self) -> DomainHealthObservation: ...


class ComposerSupervisionProbe:
    """
    Strictly consume Composer's already-reduced supervision snapshot.

    HTTP responsiveness alone is not health.  Only a progressing snapshot
    under one pinned Composer authority epoch renews receiver-local freshness.
    """

    def __init__(
        self,
        *,
        url: str,
        expected_cell_id: str,
        startup_grace_ns: int,
        maximum_progress_age_ns: int,
        maximum_unhealthy_age_ns: int,
        request_timeout_s: float,
        clock_ns: Callable[[], int] = time.monotonic_ns,
        urlopen: Callable[..., object] = urllib.request.urlopen,
    ) -> None:
        if not url.startswith(('http://', 'https://')):
            raise ValueError('Composer supervision URL is invalid')
        if not expected_cell_id:
            raise ValueError('expected cell id is required')
        if (
            startup_grace_ns <= 0
            or maximum_progress_age_ns <= 0
            or maximum_unhealthy_age_ns <= 0
        ):
            raise ValueError('supervision timing budgets must be positive')
        if request_timeout_s <= 0:
            raise ValueError('supervision request timeout must be positive')
        self._url = url
        self._expected_cell_id = expected_cell_id
        self._startup_grace_ns = startup_grace_ns
        self._maximum_progress_age_ns = maximum_progress_age_ns
        self._maximum_unhealthy_age_ns = maximum_unhealthy_age_ns
        self._request_timeout_s = request_timeout_s
        self._clock_ns = clock_ns
        self._urlopen = urlopen
        self._started_at_ns = clock_ns()
        self._expected_supervisor_epoch = ''
        self._composer_epoch = ''
        self._last_source_progress_ns = 0
        self._last_progress_received_ns = 0
        self._healthy_observed = False
        self._unhealthy_started_ns = 0

    def bind_supervisor_epoch(self, component_epoch: str) -> None:
        if not component_epoch:
            raise ValueError('supervisor component epoch is required')
        if (
            self._expected_supervisor_epoch
            and self._expected_supervisor_epoch != component_epoch
        ):
            raise RuntimeError('supervisor component epoch is already bound')
        self._expected_supervisor_epoch = component_epoch

    @staticmethod
    def _reason_codes(value: object) -> tuple[str, ...]:
        if not isinstance(value, list):
            return ()
        return tuple(
            str(item)[:256]
            for item in value[:16]
            if isinstance(item, str) and item
        )

    def _starting_or_lost(
        self,
        *,
        now_ns: int,
        reason: str,
    ) -> DomainHealthObservation:
        if (
            not self._healthy_observed
            and now_ns - self._started_at_ns <= self._startup_grace_ns
        ):
            return DomainHealthObservation(
                status='starting',
                reason_codes=(reason,),
            )
        if (
            self._healthy_observed
            and self._last_progress_received_ns > 0
            and now_ns - self._last_progress_received_ns
            <= self._maximum_progress_age_ns
        ):
            return DomainHealthObservation(status='healthy')
        return DomainHealthObservation(
            status='lost',
            reason_codes=(reason,),
        )

    def _load_payload(self) -> Mapping[str, object]:
        request = urllib.request.Request(
            self._url,
            headers={'Accept': 'application/json'},
            method='GET',
        )
        with self._urlopen(
            request,
            timeout=self._request_timeout_s,
        ) as response:
            payload = response.read(_MAX_HEALTH_RESPONSE_BYTES + 1)
        if len(payload) > _MAX_HEALTH_RESPONSE_BYTES:
            raise ValueError('composer_supervision_response_oversized')
        value = json.loads(payload)
        if not isinstance(value, Mapping):
            raise ValueError('composer_supervision_response_malformed')
        supervision = value.get('cellSupervision')
        if not isinstance(supervision, Mapping):
            raise ValueError('composer_supervision_snapshot_unavailable')
        return supervision

    def observe(self) -> DomainHealthObservation:
        now_ns = self._clock_ns()
        try:
            snapshot = self._load_payload()
        except (
            OSError,
            ValueError,
            json.JSONDecodeError,
            urllib.error.URLError,
        ) as exc:
            return self._starting_or_lost(
                now_ns=now_ns,
                reason=(
                    str(exc)[:256]
                    or 'composer_supervision_request_failed'
                ),
            )

        if snapshot.get('cellId') != self._expected_cell_id:
            return DomainHealthObservation(
                status='lost',
                reason_codes=('composer_supervision_cell_mismatch',),
            )
        components = snapshot.get('components')
        supervisor_epoch = ''
        if isinstance(components, list):
            for component in components:
                if (
                    isinstance(component, Mapping)
                    and component.get('role') == 'cell_supervisor'
                ):
                    supervisor_epoch = str(
                        component.get('componentEpoch') or ''
                    )
                    break
        if (
            not self._expected_supervisor_epoch
            or supervisor_epoch != self._expected_supervisor_epoch
        ):
            return DomainHealthObservation(
                status='lost',
                reason_codes=('composer_supervisor_epoch_mismatch',),
            )
        composer_epoch = snapshot.get('authorityComponentEpoch')
        source_progress_ns = snapshot.get('evaluatedAtSteadyTimeNs')
        if (
            not isinstance(composer_epoch, str)
            or not composer_epoch
            or isinstance(source_progress_ns, bool)
            or not isinstance(source_progress_ns, int)
            or source_progress_ns <= 0
        ):
            return DomainHealthObservation(
                status='lost',
                reason_codes=('composer_supervision_identity_malformed',),
            )
        if self._composer_epoch and composer_epoch != self._composer_epoch:
            return DomainHealthObservation(
                status='lost',
                reason_codes=('composer_component_epoch_changed',),
            )
        if not self._composer_epoch:
            self._composer_epoch = composer_epoch
        if source_progress_ns < self._last_source_progress_ns:
            return DomainHealthObservation(
                status='lost',
                reason_codes=('composer_supervision_progress_regressed',),
            )
        if source_progress_ns > self._last_source_progress_ns:
            self._last_source_progress_ns = source_progress_ns
            self._last_progress_received_ns = now_ns

        admitted = snapshot.get('dispatchAdmitted')
        if admitted is not True:
            reasons = self._reason_codes(snapshot.get('reasonCodes'))
            if not self._healthy_observed:
                return self._starting_or_lost(
                    now_ns=now_ns,
                    reason=(
                        reasons[0]
                        if reasons
                        else 'composer_supervision_not_admitted'
                    ),
                )
            if self._unhealthy_started_ns <= 0:
                self._unhealthy_started_ns = now_ns
            if (
                now_ns - self._unhealthy_started_ns
                <= self._maximum_unhealthy_age_ns
            ):
                # Composer's local reducer has already fenced dispatch. Keep
                # process teardown as a slower, persistent-loss boundary so a
                # single scheduler/DDS freshness miss cannot destroy a healthy
                # owner domain. Identity, epoch, progress-regression, and
                # process-exit failures remain terminal without this grace.
                return DomainHealthObservation(
                    status='starting',
                    reason_codes=(
                        reasons
                        or ('composer_supervision_not_admitted',)
                    ),
                )
            return DomainHealthObservation(
                status='lost',
                reason_codes=(
                    reasons
                    or ('composer_supervision_not_admitted',)
                ),
            )
        if (
            self._last_progress_received_ns <= 0
            or now_ns - self._last_progress_received_ns
            > self._maximum_progress_age_ns
        ):
            return DomainHealthObservation(
                status='lost',
                reason_codes=('composer_supervision_progress_stale',),
            )
        self._unhealthy_started_ns = 0
        self._healthy_observed = True
        return DomainHealthObservation(status='healthy')


@dataclass(frozen=True, slots=True)
class ContainerDomain:
    composer: str
    transport_owner: str

    @property
    def teardown_order(self) -> tuple[str, ...]:
        return (self.composer, self.transport_owner)


@dataclass(frozen=True, slots=True)
class SupervisorDecision:
    desired_state: str
    healthy: bool
    terminal: bool
    reason_codes: tuple[str, ...]
    requested_containment: str
    observed_running: tuple[tuple[str, bool], ...]


class DockerCliProcessControl:
    """Docker CLI adapter; the domain state machine remains runtime-neutral."""

    @staticmethod
    def _run(
        arguments: list[str],
        *,
        timeout_s: float = 30.0,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            arguments,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout_s,
        )

    def running_many(self, names: tuple[str, ...]) -> Mapping[str, bool]:
        observed = {name: False for name in names}
        if not names:
            return observed
        try:
            result = self._run(
                [
                    'docker',
                    'inspect',
                    '--format',
                    '{{json .Name}} {{json .State.Running}}',
                    *names,
                ],
                timeout_s=1.0,
            )
        except (OSError, subprocess.SubprocessError):
            return observed
        for line in result.stdout.splitlines():
            try:
                raw_name, raw_running = line.split(' ', 1)
                parsed_name = str(json.loads(raw_name)).lstrip('/')
                parsed_running = json.loads(raw_running)
            except (ValueError, json.JSONDecodeError):
                continue
            if parsed_name in observed and isinstance(parsed_running, bool):
                observed[parsed_name] = parsed_running
        return observed

    def stop(self, name: str, grace_seconds: int) -> bool:
        try:
            result = self._run(
                ['docker', 'stop', '--time', str(grace_seconds), name]
            )
        except (OSError, subprocess.SubprocessError):
            return False
        return result.returncode == 0

    def force_remove(self, name: str) -> bool:
        try:
            result = self._run(['docker', 'rm', '--force', name])
        except (OSError, subprocess.SubprocessError):
            return False
        return result.returncode == 0


class CellSupervisor:
    """Deterministic one-way process-domain supervisor."""

    def __init__(
        self,
        *,
        domain: ContainerDomain,
        process_control: ProcessControl,
    ) -> None:
        self.domain = domain
        self.process_control = process_control
        self._armed = False
        self._terminal = False
        self._terminal_reason_codes: tuple[str, ...] = ()
        self._unclean_terminations: set[str] = set()

    @property
    def unclean_terminations(self) -> tuple[str, ...]:
        return tuple(sorted(self._unclean_terminations))

    def _observe(self) -> tuple[tuple[str, bool], ...]:
        names = self.domain.teardown_order
        observed = self.process_control.running_many(names)
        return tuple(
            (name, observed.get(name, False))
            for name in names
        )

    def _teardown(self) -> None:
        for name in self.domain.teardown_order:
            if not self.process_control.running_many((name,)).get(
                name,
                False,
            ):
                continue
            if self.process_control.stop(
                name,
                10 if name == self.domain.composer else 20,
            ):
                continue
            self._unclean_terminations.add(name)
            self.process_control.force_remove(name)

    def evaluate(
        self,
        *,
        arm_requested: bool,
        shutdown_requested: bool,
        domain_health: DomainHealthObservation | None = None,
    ) -> SupervisorDecision:
        if self._terminal:
            observed = self._observe()
            return SupervisorDecision(
                desired_state='restart_required',
                healthy=False,
                terminal=True,
                reason_codes=self._terminal_reason_codes,
                requested_containment='manual_restart_required',
                observed_running=observed,
            )

        observed = self._observe()
        observed_map = dict(observed)
        if shutdown_requested:
            self._teardown()
            return SupervisorDecision(
                desired_state='stopped',
                healthy=False,
                terminal=True,
                reason_codes=('operator_shutdown_completed',),
                requested_containment='none',
                observed_running=self._observe(),
            )

        if not self._armed:
            if arm_requested:
                missing = tuple(
                    name for name, running in observed if not running
                )
                if missing:
                    self._terminal = True
                    self._terminal_reason_codes = tuple(
                        f'component_not_running_at_arm:{name}'
                        for name in missing
                    )
                    self._teardown()
                    return SupervisorDecision(
                        desired_state='restart_required',
                        healthy=False,
                        terminal=True,
                        reason_codes=self._terminal_reason_codes,
                        requested_containment=(
                            'fence_and_teardown_restart_domain'
                        ),
                        observed_running=self._observe(),
                    )
                self._armed = True
            else:
                return SupervisorDecision(
                    desired_state='starting',
                    healthy=False,
                    terminal=False,
                    reason_codes=('supervisor_not_armed',),
                    requested_containment='dispatch_fenced',
                    observed_running=observed,
                )

        missing = tuple(
            name for name, running in observed_map.items() if not running
        )
        if missing:
            self._terminal = True
            self._terminal_reason_codes = tuple(
                f'supervised_component_exited:{name}' for name in missing
            )
            self._teardown()
            return SupervisorDecision(
                desired_state='restart_required',
                healthy=False,
                terminal=True,
                reason_codes=self._terminal_reason_codes,
                requested_containment='fence_and_teardown_restart_domain',
                observed_running=self._observe(),
            )
        if domain_health is not None and domain_health.status == 'lost':
            self._terminal = True
            self._terminal_reason_codes = (
                domain_health.reason_codes
                or ('supervised_domain_health_lost',)
            )
            self._teardown()
            return SupervisorDecision(
                desired_state='restart_required',
                healthy=False,
                terminal=True,
                reason_codes=self._terminal_reason_codes,
                requested_containment='fence_and_teardown_restart_domain',
                observed_running=self._observe(),
            )
        return SupervisorDecision(
            desired_state='running',
            # This heartbeat represents the external supervisor itself.  A
            # responsive supervisor awaiting its first reduced domain sample
            # is healthy; only a terminal domain-health decision makes this
            # component lost.  That avoids a circular startup dependency in
            # which Composer cannot reduce a healthy graph until the
            # supervisor first reports healthy.
            healthy=True,
            terminal=False,
            reason_codes=(
                ()
                if domain_health is None
                or domain_health.status == 'healthy'
                else domain_health.reason_codes
                or ('supervised_domain_health_starting',)
            ),
            requested_containment=(
                'none'
                if domain_health is None
                or domain_health.status == 'healthy'
                else 'dispatch_fenced'
            ),
            observed_running=observed,
        )


def _atomic_json(path: Path, payload: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=f'.{path.name}.',
        dir=path.parent,
        text=True,
    )
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8') as handle:
            json.dump(payload, handle, sort_keys=True, separators=(',', ':'))
            handle.write('\n')
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


def _heartbeat_payload(
    *,
    decision: SupervisorDecision,
    cell_id: str,
    robot_id: str,
    component_epoch: str,
    sequence: int,
    critical_progress_sequence: int,
    now_ns: int,
    unclean_terminations: tuple[str, ...],
) -> dict[str, object]:
    health_state = (
        'healthy'
        if decision.healthy
        else 'starting'
        if decision.desired_state == 'starting'
        else 'lost'
    )
    return {
        'schema': HEARTBEAT_SCHEMA,
        'componentRole': 'cell_supervisor',
        'componentId': f'{cell_id}:cell-supervisor',
        'robotId': robot_id,
        'cellId': cell_id,
        'componentEpoch': component_epoch,
        'sequence': sequence,
        'healthState': health_state,
        'criticalProgressSequence': critical_progress_sequence,
        'dependencyEpochs': {},
        'runtimeGeneration': '',
        'bindingGeneration': '',
        'configurationGeneration': '',
        'desiredState': decision.desired_state,
        'reasonCodes': list(decision.reason_codes),
        'requestedContainment': decision.requested_containment,
        'observedContainers': dict(decision.observed_running),
        'uncleanTerminations': list(unclean_terminations),
        'observedAtSteadyTimeNs': now_ns,
        'observedAtUtc': datetime.now(UTC).isoformat(),
    }


def run_supervisor(
    *,
    runtime_root: Path,
    control_root: Path | None = None,
    cell_id: str,
    robot_id: str,
    domain: ContainerDomain,
    process_control: ProcessControl,
    period_seconds: float,
    domain_health_probe: DomainHealthProbe | None = None,
    clock_ns: Callable[[], int] = time.monotonic_ns,
    sleep: Callable[[float], None] = time.sleep,
) -> int:
    effective_control_root = control_root or runtime_root
    effective_control_root.mkdir(parents=True, exist_ok=True)
    component_epoch = f'cell-supervisor-{uuid.uuid4()}'
    if domain_health_probe is not None:
        domain_health_probe.bind_supervisor_epoch(component_epoch)
    supervisor = CellSupervisor(
        domain=domain,
        process_control=process_control,
    )
    heartbeat_path = runtime_root / 'cell-supervisor-heartbeat.json'
    state_path = runtime_root / 'cell-supervisor-state.json'
    arm_path = effective_control_root / 'cell-supervisor-arm'
    shutdown_path = effective_control_root / 'cell-supervisor-shutdown'
    stop_signal = False

    def request_stop(_signum: int, _frame: object) -> None:
        nonlocal stop_signal
        stop_signal = True

    previous_sigterm = signal.signal(signal.SIGTERM, request_stop)
    previous_sigint = signal.signal(signal.SIGINT, request_stop)
    try:
        sequence = 0
        progress_sequence = 0
        while True:
            sequence += 1
            try:
                domain_health = (
                    domain_health_probe.observe()
                    if domain_health_probe is not None
                    else None
                )
            except Exception as exc:
                # A failed health adapter is itself loss of the only reduced
                # health channel.  Keep process teardown available instead of
                # letting the external supervisor crash and orphan its domain.
                domain_health = DomainHealthObservation(
                    status='lost',
                    reason_codes=(
                        (
                            'domain_health_probe_failed:'
                            f'{str(exc) or type(exc).__name__}'
                        )[:256],
                    ),
                )
            decision = supervisor.evaluate(
                arm_requested=arm_path.exists(),
                shutdown_requested=shutdown_path.exists() or stop_signal,
                domain_health=domain_health,
            )
            progress_sequence += 1
            now_ns = clock_ns()
            heartbeat = _heartbeat_payload(
                decision=decision,
                cell_id=cell_id,
                robot_id=robot_id,
                component_epoch=component_epoch,
                sequence=sequence,
                critical_progress_sequence=progress_sequence,
                now_ns=now_ns,
                unclean_terminations=supervisor.unclean_terminations,
            )
            _atomic_json(heartbeat_path, heartbeat)
            if decision.terminal:
                _atomic_json(
                    state_path,
                    {
                        'schema': STATE_SCHEMA,
                        'componentEpoch': component_epoch,
                        'terminalState': decision.desired_state,
                        'reasonCodes': list(decision.reason_codes),
                        'requestedContainment': (
                            decision.requested_containment
                        ),
                        'uncleanTerminations': list(
                            supervisor.unclean_terminations
                        ),
                        'terminalAtSteadyTimeNs': now_ns,
                        'terminalAtUtc': datetime.now(UTC).isoformat(),
                    },
                )
                return 0 if decision.desired_state == 'stopped' else 3
            sleep(period_seconds)
    finally:
        signal.signal(signal.SIGTERM, previous_sigterm)
        signal.signal(signal.SIGINT, previous_sigint)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-root', type=Path, required=True)
    parser.add_argument(
        '--control-root',
        type=Path,
        help=(
            'Host-only arm/shutdown directory; defaults to runtime-root '
            'for standalone use.'
        ),
    )
    parser.add_argument('--cell-id', required=True)
    parser.add_argument('--robot-id', required=True)
    parser.add_argument('--composer-container', required=True)
    parser.add_argument('--owner-container', required=True)
    parser.add_argument('--composer-health-url', required=True)
    parser.add_argument(
        '--health-startup-grace-ms',
        type=int,
        default=600_000,
    )
    parser.add_argument(
        '--health-progress-max-age-ms',
        type=int,
        default=3_000,
    )
    parser.add_argument(
        '--health-unhealthy-grace-ms',
        type=int,
        default=2_000,
    )
    parser.add_argument(
        '--health-request-timeout-ms',
        type=int,
        default=200,
    )
    parser.add_argument('--period-ms', type=int, default=250)
    args = parser.parse_args()
    if args.period_ms < 50 or args.period_ms > 1000:
        parser.error('--period-ms must be in 50..1000')
    if args.health_startup_grace_ms < 1_000:
        parser.error('--health-startup-grace-ms must be at least 1000')
    if args.health_progress_max_age_ms < args.period_ms * 4:
        parser.error(
            '--health-progress-max-age-ms must cover at least four periods'
        )
    if (
        args.health_unhealthy_grace_ms < args.period_ms * 2
        or args.health_unhealthy_grace_ms > 60_000
    ):
        parser.error(
            '--health-unhealthy-grace-ms must cover at least two periods '
            'and be at most 60000'
        )
    if (
        args.health_request_timeout_ms < 10
        or args.health_request_timeout_ms >= args.period_ms
    ):
        parser.error(
            '--health-request-timeout-ms must be in 10..period-ms-1'
        )
    runtime_root = args.runtime_root.resolve()
    control_root = (
        args.control_root.resolve()
        if args.control_root is not None
        else None
    )
    health_probe = ComposerSupervisionProbe(
        url=args.composer_health_url,
        expected_cell_id=args.cell_id,
        startup_grace_ns=args.health_startup_grace_ms * 1_000_000,
        maximum_progress_age_ns=(
            args.health_progress_max_age_ms * 1_000_000
        ),
        maximum_unhealthy_age_ns=(
            args.health_unhealthy_grace_ms * 1_000_000
        ),
        request_timeout_s=args.health_request_timeout_ms / 1000.0,
    )
    _atomic_json(
        runtime_root / 'cell-supervisor-policy.json',
        {
            'schema': POLICY_SCHEMA,
            'cellId': args.cell_id,
            'robotId': args.robot_id,
            'composerContainer': args.composer_container,
            'transportOwnerContainer': args.owner_container,
            'composerHealthUrl': args.composer_health_url,
            'periodMs': args.period_ms,
            'healthStartupGraceMs': args.health_startup_grace_ms,
            'healthProgressMaximumAgeMs': (
                args.health_progress_max_age_ms
            ),
            'healthUnhealthyGraceMs': (
                args.health_unhealthy_grace_ms
            ),
            'healthRequestTimeoutMs': args.health_request_timeout_ms,
            'automaticRestart': False,
        },
    )
    return run_supervisor(
        runtime_root=runtime_root,
        control_root=control_root,
        cell_id=args.cell_id,
        robot_id=args.robot_id,
        domain=ContainerDomain(
            composer=args.composer_container,
            transport_owner=args.owner_container,
        ),
        process_control=DockerCliProcessControl(),
        period_seconds=args.period_ms / 1000.0,
        domain_health_probe=health_probe,
    )


if __name__ == '__main__':
    raise SystemExit(main())
