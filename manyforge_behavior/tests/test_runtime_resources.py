"""Focused disposition tests for the removed v0 runtime-resource owner.

The historical factory lookup, partial-open rollback, duplicate-handle close,
alias fallback, and post-close rejection cases at commit
``08222188293df8e12f90dc72e204c21f22f92ea1`` map to I5/H0 implementation
availability and I6/N resource-addressed native binding.  Exact lifecycle and
partial-failure behavior is requalified at the component Manager/Kernel owner;
the deployment-owned Python registry never returns.
"""

from __future__ import annotations

from collections.abc import Callable

import pytest

from manyforge_behavior.runtime_resources import (
    RUNTIME_RESOURCE_BINDING_RESTORATION_GATES,
    RUNTIME_RESOURCE_BINDING_UNAVAILABLE_REASON_CODE,
    RuntimeResourceBindingUnavailableError,
    RuntimeResourceFactoryRegistry,
    RuntimeResourceRegistry,
    build_runtime_resource_registry,
)


class _PoisonInput:
    def __getattribute__(self, name: str) -> object:
        raise AssertionError(f"runtime-resource gate accessed former input: {name}")

    def __iter__(self):
        raise AssertionError("runtime-resource gate iterated former input")


@pytest.mark.parametrize(
    "entrypoint",
    [
        pytest.param(
            lambda poison: RuntimeResourceFactoryRegistry(poison),
            id="factory-registry",
        ),
        pytest.param(
            lambda poison: RuntimeResourceRegistry(poison),
            id="handle-registry",
        ),
        pytest.param(
            lambda poison: build_runtime_resource_registry(poison, poison),
            id="mapping-builder",
        ),
    ],
)
def test_runtime_resource_entrypoints_gate_before_input_or_effects(
    entrypoint: Callable[[object], object],
) -> None:
    with pytest.raises(RuntimeResourceBindingUnavailableError) as raised:
        entrypoint(_PoisonInput())

    message = str(raised.value)
    assert RUNTIME_RESOURCE_BINDING_UNAVAILABLE_REASON_CODE in message
    assert "H0 provides read-only implementation availability" in message
    assert "I6/N" in message


def test_runtime_resource_gate_names_exact_restoration_order() -> None:
    assert RUNTIME_RESOURCE_BINDING_RESTORATION_GATES == (
        "I6",
        "N",
    )
