"""Tests for the fail-closed real-Lite6 product-lane smoke reducer.

Historical test blob ``4f5eaf4f570d01bbac798ded94bcb0fc735ab94d``
maps advancing/closed admission to I5/H0 plus I10/R, unexpected admission to
I8 plus I10/R, component convergence/identity to I4/I5 plus I10/R, and
endpoint failure handling to the future I10/R thin client.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import urllib.error

import pytest

_SCRIPT = (
    Path(__file__).resolve().parents[2]
    / 'scripts'
    / 'xarm'
    / 'validate-lite6-product-lane-smoke.py'
)
_SPEC = importlib.util.spec_from_file_location(
    'validate_lite6_product_lane_smoke',
    _SCRIPT,
)
assert _SPEC is not None and _SPEC.loader is not None
smoke = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(smoke)


def lifecycle(*, can_execute: bool = False) -> dict[str, object]:
    return {
        'configured': True,
        'fresh': True,
        'canExecute': can_execute,
        'userMessage': 'observed E-stop',
    }


def runtime(
    *,
    evaluation_ns: int,
    can_execute: bool = False,
    cell_id: str = 'lite6-a',
) -> dict[str, object]:
    roles = (
        'cell_supervisor',
        'transport_owner',
        'lifecycle_provider',
        'execution_backend',
        'control_authority',
    )
    return {
        'canExecute': can_execute,
        'cellSupervision': {
            'cellId': cell_id,
            'dependencyGraphHealthy': True,
            'dispatchAdmitted': True,
            'evaluatedAtSteadyTimeNs': evaluation_ns,
            'components': [
                {
                    'role': role,
                    'componentEpoch': f'{role}-epoch',
                    'healthy': True,
                }
                for role in roles
            ],
        },
    }


class FakeClock:
    def __init__(self) -> None:
        self.now = 1.0

    def __call__(self) -> float:
        return self.now

    def sleep(self, duration_s: float) -> None:
        self.now += duration_s


def test_wait_requires_advancing_supervision_and_closed_motion_admission():
    samples = iter(
        (
            (lifecycle(), runtime(evaluation_ns=10)),
            (lifecycle(), runtime(evaluation_ns=10)),
            (lifecycle(), runtime(evaluation_ns=11)),
        )
    )
    clock = FakeClock()

    evaluation_ns = smoke.wait_for_observation_only_smoke(
        fetch_status=lambda: next(samples),
        expected_cell_id='lite6-a',
        timeout_s=1.0,
        clock=clock,
        sleep=clock.sleep,
    )

    assert evaluation_ns == 11


def test_sample_fails_closed_on_unexpected_runtime_admission():
    runtime_status = runtime(evaluation_ns=10)
    runtime_status['canExecute'] = True

    with pytest.raises(
        smoke.SmokeValidationError,
        match='runtime unexpectedly admitted execution',
    ):
        smoke.validate_observation_only_sample(
            lifecycle(),
            runtime_status,
            expected_cell_id='lite6-a',
        )


@pytest.mark.parametrize(
    'mutate',
    (
        lambda status: status['cellSupervision'].__setitem__(
            'dependencyGraphHealthy',
            False,
        ),
        lambda status: status['cellSupervision'].__setitem__(
            'dispatchAdmitted',
            False,
        ),
        lambda status: status['cellSupervision']['components'][0].__setitem__(
            'healthy',
            False,
        ),
    ),
)
def test_sample_waits_for_supervision_convergence(mutate):
    runtime_status = runtime(evaluation_ns=10)
    mutate(runtime_status)

    assert smoke.validate_observation_only_sample(
        lifecycle(),
        runtime_status,
        expected_cell_id='lite6-a',
    ) is None


def test_sample_waits_for_missing_role_and_rejects_duplicate_epoch():
    missing = runtime(evaluation_ns=10)
    missing['cellSupervision']['components'].pop()
    assert smoke.validate_observation_only_sample(
        lifecycle(),
        missing,
        expected_cell_id='lite6-a',
    ) is None

    duplicate = runtime(evaluation_ns=10)
    components = duplicate['cellSupervision']['components']
    components[1]['componentEpoch'] = components[0]['componentEpoch']
    with pytest.raises(smoke.SmokeValidationError, match='epochs are not unique'):
        smoke.validate_observation_only_sample(
            lifecycle(),
            duplicate,
            expected_cell_id='lite6-a',
        )


def test_wait_times_out_on_endpoint_failure_without_false_pass():
    clock = FakeClock()

    with pytest.raises(TimeoutError, match='HTTP Error 500'):
        smoke.wait_for_observation_only_smoke(
            fetch_status=lambda: (_ for _ in ()).throw(
                urllib.error.HTTPError(
                    'http://127.0.0.1:9000/api/runtime/status',
                    500,
                    'Internal Server Error',
                    {},
                    None,
                )
            ),
            expected_cell_id='lite6-a',
            timeout_s=0.3,
            clock=clock,
            sleep=clock.sleep,
        )


def test_direct_smoke_client_is_gated_before_argument_access(capsys):
    class PoisonArguments:
        def __iter__(self):
            raise AssertionError('unavailable client inspected arguments')

    assert smoke.main(PoisonArguments()) == 78
    assert (
        smoke.LITE6_PRODUCT_SMOKE_UNAVAILABLE_REASON_CODE
        in capsys.readouterr().err
    )
