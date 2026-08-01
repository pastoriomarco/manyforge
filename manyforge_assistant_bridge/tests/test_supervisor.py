"""Assistant-supervisor cutover and pure reduction tests.

The effectful historical cohorts remain pinned in source blob
8c0ecee5ad79b553063c0f171740a3ccf617e8d7: HTTP probes at lines 77-142,
Composer-v0 reduction/publication at 164-256, and polling/shutdown at 264-304.
H0 acceptance must recover endpoint failure, model-identity mismatch, optional
proxy selection, readiness transitions, bounded polling, and shutdown truth.
"""

from __future__ import annotations

import inspect

import pytest

from manyforge_assistant_bridge import supervisor


def test_effectful_supervisor_entrypoint_is_fenced() -> None:
    assert supervisor.main() == 78
    source = inspect.getsource(supervisor)
    for forbidden in (
        "urllib.request",
        "urlopen",
        "probe_vllm",
        "probe_bridge",
        "build_status_payload",
        "while _RUNNING",
    ):
        assert forbidden not in source


def test_status_reducer_preserves_component_failures() -> None:
    config = supervisor.SupervisorConfig(
        start_vllm_proxy=False,
        assistant_provider="direct",
    )

    payload = supervisor.reduce_status_payload(
        config,
        vllm_result=(True, "ready"),
        proxy_result=(True, "ready"),
        bridge_result=(False, "bridge unavailable"),
    )

    assert payload.available is False
    assert "Assistant bridge" in payload.message
    assert not hasattr(payload, "can_execute")


@pytest.mark.parametrize(
    ("field", "value", "message"),
    [
        ("start_vllm_proxy", "false", "start_vllm_proxy must be a boolean"),
        ("model_profile", "x" * 257, "model_profile exceeds"),
        ("assistant_provider", "x" * 257, "assistant_provider exceeds"),
    ],
)
def test_supervisor_config_rejects_coercible_or_unbounded_values(
    field: str, value, message: str
) -> None:
    values = {
        "start_vllm_proxy": False,
        "model_profile": "model",
        "assistant_provider": "direct",
    }
    values[field] = value
    with pytest.raises(ValueError, match=message):
        supervisor.SupervisorConfig(**values)


@pytest.mark.parametrize(
    "probe_result",
    [
        ("false", "unavailable"),
        (True,),
        [True, "ready"],
        (True, "x" * 2049),
    ],
)
def test_status_reducer_rejects_malformed_probe_facts(probe_result) -> None:
    with pytest.raises(ValueError):
        supervisor.reduce_status_payload(
            supervisor.SupervisorConfig(),
            vllm_result=probe_result,
            proxy_result=(True, "ready"),
            bridge_result=(True, "ready"),
        )


def test_old_status_transport_is_fenced_before_network() -> None:
    with pytest.raises(
        RuntimeError,
        match="composable_cell_assistant_supervisor_unavailable",
    ):
        supervisor.post_status(
            supervisor.SupervisorConfig(),
            supervisor.AssistantStackObservation(
                available=False,
                message="unavailable",
                components=(),
                assistant_provider="unconfigured",
                model_profile="unconfigured",
            ),
        )


def test_historical_effectful_supervisor_cohorts_remain_assigned_to_h0() -> None:
    assert supervisor.SUPERVISOR_RESTORATION_GATE == "H0"
    source = inspect.getsource(supervisor)
    assert "8c0ecee5ad79b553063c0f171740a3ccf617e8d7" in source
    assert "lines 77-142" in source
    assert "164-230" in source
    assert "232-256" in source
    assert "264-304" in source
