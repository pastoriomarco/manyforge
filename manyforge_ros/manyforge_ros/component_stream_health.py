"""CP9 component stream health: Python twin and adapter-side sample sink.

``StreamDeclaration`` and ``StreamLiveness`` mirror
``manyforge_core/component_stream_health.hpp`` (names, validation and the
liveness rule) and need no ROS. ``ComponentStreamSink`` forwards samples and
fault events of one stream from an adapter process to the execution host,
which records them into the stream the host-side adapter declared.

Transport: ``diagnostic_msgs/msg/DiagnosticStatus`` on
``/manyforge/component_stream_samples`` (reliable, depth 64);
``hardware_id`` = component id, ``name`` = stream id, ``level`` OK = sample,
ERROR = fault event, ``message`` = fault code, values ``stamp_ns`` (decimal,
``"0"`` = arrival only) and, for faults, ``detail``.
"""

from __future__ import annotations

import re
import time
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any, Final, Literal

StreamSeverity = Literal["fault", "blocking", "warning"]
StreamRecovery = Literal["ack", "auto"]
StreamTimestampSource = Literal["message_stamp", "arrival"]
StreamLivenessState = Literal["live", "stale", "absent"]

STREAM_SEVERITIES: Final = ("fault", "blocking", "warning")
STREAM_RECOVERIES: Final = ("ack", "auto")
STREAM_TIMESTAMP_SOURCES: Final = ("message_stamp", "arrival")

COMPONENT_STREAM_STALE_CODE: Final = "component_stream_stale"
COMPONENT_STREAM_ABSENT_CODE: Final = "component_stream_absent"
COMPONENT_STREAM_ACKNOWLEDGEMENT_REQUIRED_CODE: Final = (
    "component_stream_acknowledgement_required"
)

MAX_STREAM_ID_BYTES: Final = 64
MAX_STREAM_FAULT_CODE_BYTES: Final = 64
MAX_STREAM_FAULT_DETAIL_BYTES: Final = 256

COMPONENT_STREAM_SAMPLES_TOPIC: Final = "/manyforge/component_stream_samples"
COMPONENT_STREAM_SAMPLES_DEPTH: Final = 64
DEFAULT_HEARTBEAT_PERIOD_NS: Final = 50_000_000

LEVEL_OK: Final = 0
LEVEL_ERROR: Final = 2

_MAX_INT64: Final = 2**63 - 1
_STREAM_ID: Final = re.compile(r"[a-z0-9_./-]{1,64}")
_FAULT_CODE: Final = re.compile(r"[a-z0-9_.]{1,64}")


def valid_stream_id(value: object) -> bool:
    """1..64 bytes of ``[a-z0-9_./-]`` (C++ ``ValidStreamId``)."""

    return type(value) is str and _STREAM_ID.fullmatch(value) is not None


def valid_stream_fault_code(value: object) -> bool:
    """1..64 bytes of ``[a-z0-9_.]`` (C++ ``ValidStreamFaultCode``)."""

    return type(value) is str and _FAULT_CODE.fullmatch(value) is not None


def _require_int(value: object, label: str, *, minimum: int) -> int:
    if type(value) is not int or not minimum <= value <= _MAX_INT64:
        raise ValueError(f"{label} must be an int64 >= {minimum}")
    return value


def _require_member(value: object, allowed: tuple[str, ...], label: str) -> None:
    if value not in allowed or type(value) is not str:
        raise ValueError(f"{label} must be one of {allowed}")


@dataclass(frozen=True)
class StreamDeclaration:
    """One declared stream with its adapter-kind defaults."""

    stream_id: str
    budget_ns: int
    severity: StreamSeverity = "fault"
    recovery: StreamRecovery = "ack"
    timestamp_source: StreamTimestampSource = "message_stamp"

    def __post_init__(self) -> None:
        if not valid_stream_id(self.stream_id):
            raise ValueError("stream_id must be 1..64 bytes of [a-z0-9_./-]")
        _require_int(self.budget_ns, "budget_ns", minimum=1)
        _require_member(self.severity, STREAM_SEVERITIES, "severity")
        _require_member(self.recovery, STREAM_RECOVERIES, "recovery")
        _require_member(
            self.timestamp_source, STREAM_TIMESTAMP_SOURCES, "timestamp_source"
        )


class StreamLiveness:
    """Pure twin of the C++ liveness rule for one stream.

    A sample advances when its stamp is strictly greater than the last
    advancing stamp (``message_stamp``), always for ``arrival`` or when it
    carries no stamp. ``live`` while ``now - last_advance_arrival <= budget``,
    ``stale`` after that, ``absent`` before the first advancing sample.
    Arrival and ``now`` use one host clock; stamps are only compared with
    each other.
    """

    def __init__(
        self,
        budget_ns: int,
        timestamp_source: StreamTimestampSource = "message_stamp",
    ) -> None:
        self._budget_ns = _require_int(budget_ns, "budget_ns", minimum=1)
        _require_member(timestamp_source, STREAM_TIMESTAMP_SOURCES, "timestamp_source")
        self._by_stamp = timestamp_source == "message_stamp"
        self._last_stamp_ns: int | None = None
        self._last_advance_arrival_ns: int | None = None

    def record(self, stamp_ns: int | None, arrival_ns: int) -> bool:
        if self._by_stamp and stamp_ns is not None:
            if self._last_stamp_ns is not None and stamp_ns <= self._last_stamp_ns:
                return False
            self._last_stamp_ns = stamp_ns
        self._last_advance_arrival_ns = arrival_ns
        return True

    def age_ns(self, now_ns: int) -> int | None:
        if self._last_advance_arrival_ns is None:
            return None
        return now_ns - self._last_advance_arrival_ns

    def state(self, now_ns: int) -> StreamLivenessState:
        age = self.age_ns(now_ns)
        if age is None:
            return "absent"
        return "live" if age <= self._budget_ns else "stale"


def _truncate_utf8(text: str, limit: int) -> str:
    return text.encode("utf-8")[:limit].decode("utf-8", errors="ignore")


def _ros_transport() -> tuple[Any, Any, Any]:
    """(DiagnosticStatus, KeyValue, QoS profile); imported only for the sink."""

    from diagnostic_msgs.msg import DiagnosticStatus, KeyValue
    from rclpy.qos import QoSProfile, ReliabilityPolicy

    qos = QoSProfile(
        depth=COMPONENT_STREAM_SAMPLES_DEPTH,
        reliability=ReliabilityPolicy.RELIABLE,
    )
    return DiagnosticStatus, KeyValue, qos


class ComponentStreamSink:
    """Forwards one stream's samples and fault events to the execution host.

    ``record`` is cheap: it publishes an advancing sample at most every
    ``heartbeat_period_ns`` (the first one at once; a throttled one, as the
    newest advancing stamp, on the next call after the period). Budgets are
    declared only by the host-side adapter of the component.
    """

    def __init__(
        self,
        node: Any,
        controllable_component_id: str,
        stream_id: str,
        *,
        heartbeat_period_ns: int = DEFAULT_HEARTBEAT_PERIOD_NS,
        clock_ns: Callable[[], int] = time.monotonic_ns,
    ) -> None:
        if type(controllable_component_id) is not str or not controllable_component_id:
            raise ValueError("controllable_component_id must be a non-empty str")
        if not valid_stream_id(stream_id):
            raise ValueError("stream_id must be 1..64 bytes of [a-z0-9_./-]")
        self._component = controllable_component_id
        self._stream_id = stream_id
        self._period_ns = _require_int(
            heartbeat_period_ns, "heartbeat_period_ns", minimum=0
        )
        self._clock_ns = clock_ns
        self._status_type, self._key_value_type, qos = _ros_transport()
        self._publisher = node.create_publisher(
            self._status_type, COMPONENT_STREAM_SAMPLES_TOPIC, qos
        )
        self._last_stamp_ns: int | None = None
        self._pending_stamp_ns: int | None = None
        self._last_publish_ns: int | None = None

    def record(self, stamp_ns: int | None = None) -> None:
        """One sample; ``None`` or ``0`` = arrival only (no usable stamp).

        A zero stamp is what the wire and the host already treat as arrival
        only (simulator entity-state reads carry no stamp).
        """

        if stamp_ns is None or (type(stamp_ns) is int and stamp_ns == 0):
            self._pending_stamp_ns = 0
        else:
            _require_int(stamp_ns, "stamp_ns", minimum=0)
            if self._last_stamp_ns is None or stamp_ns > self._last_stamp_ns:
                self._last_stamp_ns = stamp_ns
                self._pending_stamp_ns = stamp_ns
        if self._pending_stamp_ns is None:
            return
        now_ns = self._clock_ns()
        if (
            self._last_publish_ns is not None
            and now_ns - self._last_publish_ns < self._period_ns
        ):
            return
        self._last_publish_ns = now_ns
        self._publish(LEVEL_OK, "", self._pending_stamp_ns, None)
        self._pending_stamp_ns = None

    def report_fault(self, code: str, detail: str = "") -> None:
        """Immediate bad news with a typed code and a bounded detail."""

        if not valid_stream_fault_code(code):
            raise ValueError("fault code must be 1..64 bytes of [a-z0-9_.]")
        if type(detail) is not str:
            raise ValueError("fault detail must be a str")
        self._publish(
            LEVEL_ERROR, code, 0, _truncate_utf8(detail, MAX_STREAM_FAULT_DETAIL_BYTES)
        )

    def _publish(self, level: int, code: str, stamp_ns: int, detail: str | None) -> None:
        message = self._status_type()
        message.level = bytes([level])
        message.name = self._stream_id
        message.message = code
        message.hardware_id = self._component
        values = [self._key_value_type(key="stamp_ns", value=str(stamp_ns))]
        if detail is not None:
            values.append(self._key_value_type(key="detail", value=detail))
        message.values = values
        self._publisher.publish(message)
