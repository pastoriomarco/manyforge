"""Generation-scoped runtime-fault registry for recoverable behavior nodes."""

from __future__ import annotations

from collections.abc import Mapping
from copy import deepcopy
from dataclasses import dataclass, replace
import math
import threading
import time
import uuid
from typing import Any, Optional


_MAX_DETAIL_ITEMS = 64
_MAX_NESTED_DETAIL_ITEMS = 32
_MAX_DETAIL_DEPTH = 6
_MAX_DETAIL_NODES = 256
_MAX_DETAIL_STRING_CHARS = 1024
_MAX_DETAIL_TEXT_CHARS = 16_384
_MAX_DETAIL_KEY_CHARS = 128
_DETAILS_TRUNCATED_KEY = "_manyforge_details_truncated"
_MAX_FAULT_NAME_CHARS = 256
_MAX_FAULT_IDENTITY_CHARS = 128
_MAX_FAULT_MESSAGE_CHARS = 2048


def _bounded_text(value: Any, limit: int) -> str:
    return str(value)[:limit]


class _DetailSanitizer:
    """Build a bounded JSON-safe diagnostic value without calling repr()."""

    def __init__(self) -> None:
        self.remaining_nodes = _MAX_DETAIL_NODES
        self.remaining_text_chars = _MAX_DETAIL_TEXT_CHARS
        self.truncated = False

    def _text(self, value: str, *, limit: int = _MAX_DETAIL_STRING_CHARS) -> str:
        allowed = min(limit, self.remaining_text_chars)
        if len(value) > allowed:
            self.truncated = True
        result = value[:allowed]
        self.remaining_text_chars -= len(result)
        return result

    def _key(self, value: Any) -> str:
        if isinstance(value, str):
            text = value
        elif value is None or isinstance(value, (bool, int, float)):
            text = str(value)
        else:
            value_type = type(value)
            text = (
                f"<key:{value_type.__module__}.{value_type.__qualname__}>"
            )
        return self._text(text, limit=_MAX_DETAIL_KEY_CHARS)

    def sanitize(
        self,
        value: Any,
        *,
        depth: int = 0,
        ancestors: Optional[set[int]] = None,
    ) -> Any:
        if self.remaining_nodes <= 0:
            self.truncated = True
            return "<truncated>"
        self.remaining_nodes -= 1

        if value is None or isinstance(value, (bool, str)):
            return self._text(value) if isinstance(value, str) else value
        if isinstance(value, int):
            if value.bit_length() <= 256:
                return value
            self.truncated = True
            return "<integer_out_of_range>"
        if isinstance(value, float):
            if math.isfinite(value):
                return value
            self.truncated = True
            return f"<non_finite:{value}>"

        if depth >= _MAX_DETAIL_DEPTH:
            self.truncated = True
            return "<max_depth>"

        object_id = id(value)
        active_ancestors = ancestors if ancestors is not None else set()
        if object_id in active_ancestors:
            self.truncated = True
            return "<cycle>"

        if isinstance(value, Mapping):
            active_ancestors.add(object_id)
            result: dict[str, Any] = {}
            try:
                for index, (key, item) in enumerate(value.items()):
                    if index >= _MAX_NESTED_DETAIL_ITEMS:
                        self.truncated = True
                        break
                    if self.remaining_nodes <= 0 or self.remaining_text_chars <= 0:
                        self.truncated = True
                        break
                    result[self._key(key)] = self.sanitize(
                        item,
                        depth=depth + 1,
                        ancestors=active_ancestors,
                    )
            finally:
                active_ancestors.remove(object_id)
            return result

        if isinstance(value, (list, tuple)):
            active_ancestors.add(object_id)
            result_list: list[Any] = []
            try:
                for index, item in enumerate(value):
                    if index >= _MAX_NESTED_DETAIL_ITEMS:
                        self.truncated = True
                        break
                    if self.remaining_nodes <= 0 or self.remaining_text_chars <= 0:
                        self.truncated = True
                        break
                    result_list.append(
                        self.sanitize(
                            item,
                            depth=depth + 1,
                            ancestors=active_ancestors,
                        )
                    )
            finally:
                active_ancestors.remove(object_id)
            return result_list

        value_type = type(value)
        return self._text(
            f"<unsupported:{value_type.__module__}.{value_type.__qualname__}>"
        )


def _bounded_details(details: Optional[Mapping[str, Any]]) -> dict[str, Any]:
    if not details:
        return {}
    sanitizer = _DetailSanitizer()
    result: dict[str, Any] = {}
    for index, (key, value) in enumerate(details.items()):
        if index >= _MAX_DETAIL_ITEMS:
            sanitizer.truncated = True
            break
        if sanitizer.remaining_nodes <= 0 or sanitizer.remaining_text_chars <= 0:
            sanitizer.truncated = True
            break
        result[sanitizer._key(key)] = sanitizer.sanitize(value, depth=1)
    if sanitizer.truncated:
        if (
            _DETAILS_TRUNCATED_KEY not in result
            and len(result) >= _MAX_DETAIL_ITEMS
        ):
            result.pop(next(reversed(result)))
        result[_DETAILS_TRUNCATED_KEY] = True
    return result


@dataclass(frozen=True)
class RuntimePauseFault:
    fault_id: str
    generation: int
    node_name: str
    node_id: str
    domain: str
    reason_code: str
    message: str
    details: dict[str, Any]
    raised_at_ns: int
    retry_policy: str
    resume_owner: str
    state: str
    accepted_recovery_request_id: Optional[str] = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "fault_id": self.fault_id,
            "generation": self.generation,
            "node_name": self.node_name,
            "node_id": self.node_id,
            "domain": self.domain,
            "reason_code": self.reason_code,
            "message": self.message,
            "details": deepcopy(self.details),
            "raised_at_ns": self.raised_at_ns,
            "retry_policy": self.retry_policy,
            "resume_owner": self.resume_owner,
            "state": self.state,
            "accepted_recovery_request_id": self.accepted_recovery_request_id,
        }


@dataclass(frozen=True)
class RuntimeRecoveryBinding:
    """One active node execution that an accepted cycle action may retry.

    This covers the narrow stop-before-node-observation window: the kernel can
    publish a certified recovery action before the behavior node has ticked
    once more and raised a RuntimePauseFault. The binding is exact by execution
    id and is consumed while CycleManager owns the tree-tick lock.
    """

    node_name: str
    node_id: str
    domain: str
    execution_id: int


class RuntimePauseRegistry:
    """Owns exact runtime-fault identities and accepted resume grants.

    Nodes can raise and observe only their own fault handle. The cycle recovery
    coordinator requests resume by exact fault id after a versioned recovery
    action is accepted. There is no global boolean that can accidentally wake a
    different node or a newer fault generation.
    """

    def __init__(self) -> None:
        # Leaf lock: callers may hold CycleManager's transition/state lock while
        # taking this lock, but registry methods never call back into a node or
        # CycleManager. This preserves the global cycle -> registry lock order.
        self._lock = threading.Lock()
        self._next_generation = 0
        self._revision = 0
        self._faults: dict[str, RuntimePauseFault] = {}

    @staticmethod
    def _clone_fault(fault: RuntimePauseFault) -> RuntimePauseFault:
        # RuntimePauseFault is frozen, but its bounded diagnostic map is not.
        # Return a defensive copy so consumers cannot mutate registry authority.
        return replace(fault, details=deepcopy(fault.details))

    @property
    def revision(self) -> int:
        with self._lock:
            return self._revision

    def raise_fault(
        self,
        *,
        node_name: str,
        node_id: str,
        domain: str,
        reason_code: str,
        message: str,
        details: Optional[Mapping[str, Any]] = None,
        retry_policy: str = "pause_for_operator",
        resume_owner: str = "node",
    ) -> RuntimePauseFault:
        if resume_owner not in {"node", "cycle_manager"}:
            raise ValueError("resume_owner must be node or cycle_manager")
        bounded_details = _bounded_details(details)
        with self._lock:
            self._next_generation += 1
            generation = self._next_generation
            fault_id = f"runtime-fault-{generation}-{uuid.uuid4().hex[:12]}"
            fault = RuntimePauseFault(
                fault_id=fault_id,
                generation=generation,
                node_name=_bounded_text(node_name, _MAX_FAULT_NAME_CHARS),
                node_id=_bounded_text(node_id, _MAX_FAULT_IDENTITY_CHARS),
                domain=_bounded_text(domain, _MAX_FAULT_IDENTITY_CHARS),
                reason_code=_bounded_text(reason_code, _MAX_FAULT_IDENTITY_CHARS),
                message=_bounded_text(message, _MAX_FAULT_MESSAGE_CHARS),
                details=bounded_details,
                raised_at_ns=time.monotonic_ns(),
                retry_policy=_bounded_text(
                    retry_policy, _MAX_FAULT_IDENTITY_CHARS
                ),
                resume_owner=resume_owner,
                state="active",
            )
            self._faults[fault_id] = fault
            self._revision += 1
            return self._clone_fault(fault)

    def get(self, fault_id: str) -> Optional[RuntimePauseFault]:
        with self._lock:
            fault = self._faults.get(fault_id)
            return self._clone_fault(fault) if fault is not None else None

    def active_faults(self) -> tuple[RuntimePauseFault, ...]:
        _, faults = self.active_fault_snapshot()
        return faults

    def active_fault_snapshot(
        self,
    ) -> tuple[int, tuple[RuntimePauseFault, ...]]:
        """Return one revision-coherent snapshot of active runtime faults.

        The cycle tick loop uses the revision to reuse an immutable snapshot
        while no fault changes. Returning both values under the registry's leaf
        lock avoids a check-then-read window that could otherwise cache old
        faults under a newer revision.
        """
        with self._lock:
            return (
                self._revision,
                tuple(
                    self._clone_fault(fault)
                    for fault in self._faults.values()
                    if fault.state in {"active", "resume_requested"}
                ),
            )

    def request_resume(
        self,
        fault_ids: tuple[str, ...],
        *,
        recovery_request_id: str,
    ) -> bool:
        if not fault_ids or not recovery_request_id:
            return False
        with self._lock:
            selected: list[RuntimePauseFault] = []
            for fault_id in fault_ids:
                fault = self._faults.get(fault_id)
                if fault is None or fault.state != "active":
                    return False
                selected.append(fault)
            for fault in selected:
                self._faults[fault.fault_id] = replace(
                    fault,
                    state="resume_requested",
                    accepted_recovery_request_id=recovery_request_id,
                )
            self._revision += 1
            return True

    def consume_resume(
        self,
        fault_id: str,
        *,
        expected_generation: int,
    ) -> Optional[str]:
        with self._lock:
            fault = self._faults.get(fault_id)
            if (
                fault is None
                or fault.generation != expected_generation
                or fault.state != "resume_requested"
                or not fault.accepted_recovery_request_id
            ):
                return None
            request_id = fault.accepted_recovery_request_id
            del self._faults[fault_id]
            self._revision += 1
            return request_id

    def clear(self, fault_id: str, *, expected_generation: int) -> bool:
        with self._lock:
            fault = self._faults.get(fault_id)
            if fault is None or fault.generation != expected_generation:
                return False
            del self._faults[fault_id]
            self._revision += 1
            return True

    def clear_all(self) -> None:
        with self._lock:
            if self._faults:
                self._faults.clear()
                self._revision += 1


class RuntimePauseMixin:
    """Small node helper for one generation-scoped recoverable fault."""

    _runtime_pause_registry: RuntimePauseRegistry
    _runtime_pause_fault: Optional[RuntimePauseFault]
    _runtime_pause_node_id: str
    _runtime_pause_domain: str

    def _configure_runtime_pause(
        self,
        *,
        registry: Optional[RuntimePauseRegistry],
        node_id: str,
        domain: str,
    ) -> None:
        self._runtime_pause_registry = registry or RuntimePauseRegistry()
        self._runtime_pause_fault = None
        self._runtime_pause_node_id = node_id
        self._runtime_pause_domain = domain

    def _raise_runtime_pause(
        self,
        *,
        reason_code: str,
        message: str,
        details: Optional[Mapping[str, Any]] = None,
        retry_policy: str = "pause_for_operator",
    ) -> RuntimePauseFault:
        if self._runtime_pause_fault is not None:
            current = self._runtime_pause_registry.get(
                self._runtime_pause_fault.fault_id
            )
            if current is not None:
                self._runtime_pause_fault = current
                return current
        fault = self._runtime_pause_registry.raise_fault(
            node_name=str(getattr(self, "name", self._runtime_pause_node_id)),
            node_id=self._runtime_pause_node_id,
            domain=self._runtime_pause_domain,
            reason_code=reason_code,
            message=message,
            details=details,
            retry_policy=retry_policy,
        )
        self._runtime_pause_fault = fault
        return fault

    def _consume_runtime_resume(self) -> Optional[str]:
        fault = self._runtime_pause_fault
        if fault is None:
            return None
        request_id = self._runtime_pause_registry.consume_resume(
            fault.fault_id,
            expected_generation=fault.generation,
        )
        if request_id is not None:
            self._runtime_pause_fault = None
        return request_id

    def _runtime_pause_active(self) -> bool:
        fault = self._runtime_pause_fault
        if fault is None:
            return False
        current = self._runtime_pause_registry.get(fault.fault_id)
        if current is None:
            self._runtime_pause_fault = None
            return False
        self._runtime_pause_fault = current
        return current.state == "active"

    def _clear_runtime_pause(self) -> None:
        fault = self._runtime_pause_fault
        if fault is not None:
            self._runtime_pause_registry.clear(
                fault.fault_id,
                expected_generation=fault.generation,
            )
            self._runtime_pause_fault = None

    def runtime_recovery_binding(self) -> Optional[RuntimeRecoveryBinding]:
        """Return an exact pre-fault recovery binding, if this node owns one."""
        return None

    def accept_runtime_recovery(
        self,
        *,
        recovery_request_id: str,
        expected_execution_id: int,
    ) -> bool:
        """Consume a cycle recovery grant for an exact active execution."""
        _ = (recovery_request_id, expected_execution_id)
        return False
