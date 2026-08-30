"""The Python surface is a transport adapter, never a Kernel implementation."""

import pytest

from manyforge_behavior.kernel_client import KernelClient


def test_contract_requires_the_base_headless_transport_calls() -> None:
    class IncompleteTransport:
        def identity(self):
            return {"ok": True}

    with pytest.raises(TypeError, match="submit"):
        KernelClient(IncompleteTransport())  # type: ignore[arg-type]


def test_contract_constructor_does_not_call_the_transport() -> None:
    class PoisonCallable:
        def __call__(self, *_args, **_kwargs):
            raise AssertionError("transport was called during construction")

    class Transport:
        identity = PoisonCallable()
        submit = PoisonCallable()
        read = PoisonCallable()
        pause = PoisonCallable()
        resume = PoisonCallable()
        reset = PoisonCallable()
        acknowledge_completed = PoisonCallable()
        stop = PoisonCallable()
        set_runtime_scale = PoisonCallable()
        get_runtime_scale = PoisonCallable()

    client = KernelClient(Transport())
    with pytest.raises(TypeError, match="plan_pick"):
        client.plan_pick({"sequence_id": "pick-1"})
