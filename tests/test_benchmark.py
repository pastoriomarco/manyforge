"""Disposition of the former singleton native-Kernel benchmark corpus.

The three historical cases at commit
``08222188293df8e12f90dc72e204c21f22f92ea1`` remain the acceptance source:

* ``test_benchmark_single_plan`` returns at I6/N using the C++
  resource-addressed request and the selected native planning/execution route;
* ``test_benchmark_cached_vs_uncached`` returns at I6/N with cache identity
  bound to the complete component/target/capability/artifact tuple; and
* ``test_benchmark_five_moves`` returns at I6/N for sequential native
  execution, then participates in the I8/P5 native regression proving that
  manager/compiler NRT work does not disturb execution or Stop hot paths.

The old Python ``ExecuteRequest(robot_id, group_id, ...)``, ad-hoc
``NativeKernelClient`` import, polling/reset loop, and conditional skip are not
restored.  This focused test keeps collection truthful until the native
benchmark is rebuilt on the new bindings.
"""

from __future__ import annotations

import sys


_DIRECT_ENTRYPOINT_UNAVAILABLE_MESSAGE = (
    "composable_cell_runtime_unavailable: native benchmark execution returns "
    "at I6/N and participates in I8/P5 regression qualification."
)


# Historically this file was also an executable pytest runner.  Keep that old
# command shape explicit, but fail before pytest or any ManyForge product
# module can be imported or a former binding can be reached.
if __name__ == "__main__":
    print(_DIRECT_ENTRYPOINT_UNAVAILABLE_MESSAGE, file=sys.stderr)
    raise SystemExit(78)

import pytest

from manyforge_behavior import kernel_client
from manyforge_behavior.kernel_client import (
    KERNEL_CLIENT_RESTORATION_GATES,
    KERNEL_CLIENT_UNAVAILABLE_REASON_CODE,
    KernelClient,
    KernelClientBindingUnavailableError,
)


class _PoisonBinding:
    def __getattribute__(self, name: str) -> object:
        raise AssertionError(f"benchmark gate accessed former binding: {name}")


def test_native_benchmark_fails_before_binding_or_robot_access() -> None:
    with pytest.raises(KernelClientBindingUnavailableError) as raised:
        KernelClient(_PoisonBinding())

    message = str(raised.value)
    assert KERNEL_CLIENT_UNAVAILABLE_REASON_CODE in message
    assert "I6/N" in message
    assert "I8/P5" in message


def test_native_benchmark_uses_no_singleton_python_request_contract() -> None:
    assert KERNEL_CLIENT_RESTORATION_GATES == ("I6", "N", "I8", "P5")
    assert not hasattr(kernel_client, "ExecuteRequest")
