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

from pathlib import Path
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

from manyforge_behavior import kernel_client
from manyforge_behavior.kernel_client import KernelClient


class _Transport:
    def execution_status(self):
        return {"ok": True, "status": {}}

    def execute(self, body):
        return {"ok": True, "accepted": {"request": body}}

    def operation(self, operation_id):
        return {"ok": True, "operation": {"operation_id": operation_id}}


def test_native_benchmark_client_is_now_only_a_headless_transport() -> None:
    client = KernelClient(_Transport())
    assert client.execute({"component_role_id": "robot"})["accepted"] == {
        "request": {"component_role_id": "robot"}
    }


def test_native_benchmark_uses_no_singleton_python_request_contract() -> None:
    assert not hasattr(kernel_client, "ExecuteRequest")
    source = Path(kernel_client.__file__).read_text(encoding="utf-8")
    assert "robot_id" not in source
    assert "group_id" not in source
