"""The Python surface is a transport adapter, never a Kernel implementation."""

import pytest

from manyforge_behavior.kernel_client import KernelClient


def test_contract_requires_the_base_headless_transport_calls() -> None:
    class IncompleteTransport:
        def execution_status(self):
            return {"ok": True}

    with pytest.raises(TypeError, match="execute"):
        KernelClient(IncompleteTransport())  # type: ignore[arg-type]


def test_contract_constructor_does_not_call_the_transport() -> None:
    class PoisonCallable:
        def __call__(self, *_args, **_kwargs):
            raise AssertionError("transport was called during construction")

    class Transport:
        execution_status = PoisonCallable()
        execute = PoisonCallable()
        operation = PoisonCallable()
        pause_operation = PoisonCallable()
        resume_operation = PoisonCallable()
        reset_operation = PoisonCallable()
        acknowledge_completed_operation = PoisonCallable()
        stop_operations = PoisonCallable()
        set_operation_runtime_scale = PoisonCallable()
        operation_runtime_scale = PoisonCallable()

    client = KernelClient(Transport())
    with pytest.raises(TypeError, match="plan_pick"):
        client.plan_pick({"sequence_id": "pick-1"})
