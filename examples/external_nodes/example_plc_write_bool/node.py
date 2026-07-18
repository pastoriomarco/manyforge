"""Contract example for a nonblocking external PLC node."""

from __future__ import annotations

import time

import py_trees


class ExamplePlcWriteBool(py_trees.behaviour.Behaviour):
    """Poll a provider-owned async operation without blocking the tree tick."""

    def __init__(self, name: str, **params):
        super().__init__(name=name)
        self._plc_client_id = str(params.get("plc_client_id", "default"))
        self._tag = str(params["tag"])
        self._value = bool(params["value"])
        self._timeout_ns = int(float(params.get("timeout_s", 1.0)) * 1e9)
        self._client = None
        self._operation = None
        self._started_ns = 0

    def setup(self, **kwargs) -> None:
        blackboard = self.attach_blackboard_client(name=f"{self.name}-resources")
        blackboard.register_key(
            key="runtime_resources",
            access=py_trees.common.Access.READ,
        )
        self._client = blackboard.runtime_resources.get_plc_client(
            self._plc_client_id
        )

    def initialise(self) -> None:
        self._operation = None
        self._started_ns = time.monotonic_ns()
        self.feedback_message = ""

    def update(self) -> py_trees.common.Status:
        if self._operation is None:
            # The provider contract requires this call to return immediately.
            self._operation = self._client.write_bool_async(
                tag=self._tag,
                value=self._value,
            )
            return py_trees.common.Status.RUNNING

        if time.monotonic_ns() - self._started_ns >= self._timeout_ns:
            self._cancel_operation()
            self.feedback_message = "plc_write_timeout"
            return py_trees.common.Status.FAILURE

        if not self._operation.done():
            return py_trees.common.Status.RUNNING

        result = self._operation.result()
        if bool(getattr(result, "ok", result)):
            return py_trees.common.Status.SUCCESS
        self.feedback_message = str(getattr(result, "error", "plc_write_failed"))
        return py_trees.common.Status.FAILURE

    def terminate(self, new_status: py_trees.common.Status) -> None:
        if new_status is py_trees.common.Status.INVALID:
            self._cancel_operation()

    def _cancel_operation(self) -> None:
        if self._operation is not None and not self._operation.done():
            self._operation.cancel()
