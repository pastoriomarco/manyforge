#!/usr/bin/env python3
"""Validate observation-only startup of the real-Lite6 product lane."""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from typing import Any, Callable, Mapping
import urllib.error
import urllib.request

REQUIRED_COMPONENT_ROLES = frozenset(
    {
        'cell_supervisor',
        'transport_owner',
        'lifecycle_provider',
        'execution_backend',
        'control_authority',
    }
)


class SmokeValidationError(RuntimeError):
    """A fail-closed startup invariant was violated."""


def _mapping(value: object) -> Mapping[str, Any] | None:
    return value if isinstance(value, Mapping) else None


def validate_observation_only_sample(
    lifecycle: Mapping[str, Any],
    runtime: Mapping[str, Any],
    *,
    expected_cell_id: str,
) -> int | None:
    """
    Validate one sample and return its supervision progress.

    ``None`` means startup health has not converged yet. Unexpected motion
    admission and malformed/replaced identity remain terminal immediately.
    """
    if lifecycle.get('canExecute') is True:
        raise SmokeValidationError(
            'observation-only lifecycle unexpectedly admitted execution'
        )
    if runtime.get('canExecute') is True:
        raise SmokeValidationError(
            'observation-only runtime unexpectedly admitted execution'
        )
    if lifecycle.get('configured') is not True or lifecycle.get('fresh') is not True:
        return None
    supervision = _mapping(runtime.get('cellSupervision'))
    if supervision is None:
        return None
    if supervision.get('cellId') != expected_cell_id:
        raise SmokeValidationError('real-robot supervision cell identity mismatch')
    if supervision.get('dependencyGraphHealthy') is not True:
        return None
    if supervision.get('dispatchAdmitted') is not True:
        return None

    raw_components = supervision.get('components')
    if not isinstance(raw_components, list):
        return None
    components_by_role: dict[str, Mapping[str, Any]] = {}
    for raw_component in raw_components:
        component = _mapping(raw_component)
        if component is None:
            continue
        role = component.get('role')
        if role not in REQUIRED_COMPONENT_ROLES:
            continue
        if role in components_by_role:
            raise SmokeValidationError(
                f'real-robot supervision role is duplicated: {role}'
            )
        components_by_role[role] = component
    missing = sorted(REQUIRED_COMPONENT_ROLES - components_by_role.keys())
    if missing:
        return None

    epochs: list[str] = []
    for role in sorted(REQUIRED_COMPONENT_ROLES):
        component = components_by_role[role]
        epoch = component.get('componentEpoch')
        if not isinstance(epoch, str) or not epoch:
            raise SmokeValidationError(f'real-robot component epoch is missing: {role}')
        if component.get('healthy') is not True:
            return None
        epochs.append(epoch)
    if len(set(epochs)) != len(epochs):
        raise SmokeValidationError('real-robot component epochs are not unique')

    evaluation_ns = supervision.get('evaluatedAtSteadyTimeNs')
    if (
        isinstance(evaluation_ns, bool)
        or not isinstance(evaluation_ns, int)
        or evaluation_ns <= 0
    ):
        raise SmokeValidationError('real-robot supervision progress is invalid')
    return evaluation_ns


def wait_for_observation_only_smoke(
    *,
    fetch_status: Callable[[], tuple[Mapping[str, Any], Mapping[str, Any]]],
    expected_cell_id: str,
    timeout_s: float,
    clock: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> int:
    """Wait for two advancing, fail-closed startup observations."""
    if (
        isinstance(timeout_s, bool)
        or not isinstance(timeout_s, (int, float))
        or not math.isfinite(timeout_s)
        or not 0 < timeout_s <= 120
    ):
        raise ValueError('timeout_s must be finite and in (0, 120]')
    if not expected_cell_id:
        raise ValueError('expected_cell_id must be non-empty')

    deadline = clock() + float(timeout_s)
    previous_evaluation_ns: int | None = None
    lifecycle: Mapping[str, Any] = {}
    last_error = ''
    while True:
        try:
            lifecycle, runtime = fetch_status()
            last_error = ''
            evaluation_ns = validate_observation_only_sample(
                lifecycle,
                runtime,
                expected_cell_id=expected_cell_id,
            )
            if evaluation_ns is not None:
                if (
                    previous_evaluation_ns is not None
                    and evaluation_ns < previous_evaluation_ns
                ):
                    raise SmokeValidationError(
                        'real-robot supervision progress regressed'
                    )
                if (
                    previous_evaluation_ns is not None
                    and evaluation_ns > previous_evaluation_ns
                ):
                    return evaluation_ns
                previous_evaluation_ns = evaluation_ns
        except (
            json.JSONDecodeError,
            OSError,
            urllib.error.URLError,
        ) as error:
            last_error = f'{type(error).__name__}: {error}'
            lifecycle = {}

        if clock() >= deadline:
            raise TimeoutError(
                'real-robot startup readiness timed out: '
                f'configured={lifecycle.get("configured")!r}, '
                f'fresh={lifecycle.get("fresh")!r}, '
                f'lifecycle_message={lifecycle.get("userMessage")!r}, '
                f'runtime_error={last_error!r}'
            )
        sleep(0.1)


def _load_json(url: str, *, timeout_s: float) -> Mapping[str, Any]:
    with urllib.request.urlopen(url, timeout=timeout_s) as response:
        payload = json.load(response)
    if not isinstance(payload, Mapping):
        raise ValueError(f'status response is not an object: {url}')
    return payload


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--composer-url', required=True)
    parser.add_argument('--expected-cell-id', required=True)
    parser.add_argument('--timeout-s', required=True, type=float)
    args = parser.parse_args(argv)

    request_timeout_s = min(5.0, max(0.1, args.timeout_s))
    base_url = args.composer_url.rstrip('/')

    def fetch_status() -> tuple[Mapping[str, Any], Mapping[str, Any]]:
        return (
            _load_json(
                f'{base_url}/api/robot-lifecycle/status',
                timeout_s=request_timeout_s,
            ),
            _load_json(
                f'{base_url}/api/runtime/status',
                timeout_s=request_timeout_s,
            ),
        )

    try:
        wait_for_observation_only_smoke(
            fetch_status=fetch_status,
            expected_cell_id=args.expected_cell_id,
            timeout_s=args.timeout_s,
        )
    except (SmokeValidationError, TimeoutError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1
    print(
        'real-robot startup supervision is advancing and ' 'motion admission is closed'
    )
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
