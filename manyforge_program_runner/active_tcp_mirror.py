"""Sequence confirmed Active TCP values from results and the event stream."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from copy import deepcopy
from threading import Lock
from typing import Any


class ActiveTcpWireError(ValueError):
    """An Active TCP reading or event is malformed."""


def _reading(row: Any) -> tuple[str, str, int, int, dict[str, str] | None]:
    if not isinstance(row, Mapping):
        raise ActiveTcpWireError("Active TCP reading is not an object")
    if not {"component_id", "owner_epoch", "registration_generation", "sequence", "entry"} <= set(row):
        raise ActiveTcpWireError("Active TCP reading is incomplete")
    robot, owner = row.get("component_id"), row.get("owner_epoch")
    generation, sequence, entry = (
        row.get("registration_generation"), row.get("sequence"), row.get("entry")
    )
    if (
        type(robot) is not str or not robot
        or type(owner) is not str or not owner
        or type(generation) is not int or generation < 1
        or type(sequence) is not int or sequence < 0
    ):
        raise ActiveTcpWireError("Active TCP reading identity is invalid")
    if entry is not None:
        fields = {"component_id", "target_id", "frame_role_id"}
        if (
            not isinstance(entry, Mapping) or set(entry) != fields
            or any(type(entry[field]) is not str or not entry[field] for field in fields)
        ):
            raise ActiveTcpWireError("Active TCP entry is invalid")
        entry = dict(entry)
    return robot, owner, generation, sequence, entry


def decode_active_tcp_rows(rows: Any) -> tuple[tuple[str, str, int, int, dict[str, str] | None], ...]:
    """Decode the rows shared by baseline, read, reset, submit and read replies."""

    if not isinstance(rows, list):
        raise ActiveTcpWireError("Active TCP readings are not an array")
    return tuple(_reading(row) for row in rows)


class ActiveTcpMirror:
    """Keep the latest confirmed value per robot.

    The owner decodes the host's baseline, events and operation receipts, and
    supplies their registration generation and per-robot sequence here. The
    existing owner fence must set the current Kernel owner epoch; a received
    record does not itself change that authority. ``on_change`` updates the
    stores for the robot's bound target roles. It must not re-enter this mirror.
    """

    def __init__(
        self, *, on_change: Callable[[str, Mapping[str, Any] | None], None]
    ) -> None:
        self._on_change = on_change
        self._lock = Lock()
        self._owner_epoch: str | None = None
        self._values: dict[str, tuple[int | None, int | None, dict[str, Any] | None]] = {}
        self._pending: set[str] = set()
        self._unavailable: set[str] = set()

    def set_owner_if_absent(self, owner_epoch: str) -> None:
        """Initialize from a captured owner without replacing a stream fence."""

        with self._lock:
            if self._owner_epoch is None:
                self._owner_epoch = owner_epoch

    def set_owner(self, owner_epoch: str) -> None:
        """Install the epoch established by the existing Kernel owner fence.

        On an owner replacement the previous robot values become unavailable
        until the new owner's baseline or a confirmed result supplies them.
        """

        with self._lock:
            self._set_owner_locked(owner_epoch)

    def _set_owner_locked(self, owner_epoch: str) -> None:
        if owner_epoch == self._owner_epoch:
            return
        self._owner_epoch = owner_epoch
        self._values = {robot_id: (None, None, None) for robot_id in self._values}
        self._pending = set(self._values)
        self._unavailable = set(self._values)
        for robot_id in sorted(self._pending):
            self._deliver_locked(robot_id)

    def stream_lost(self) -> None:
        """Make keys unavailable until a baseline or result confirms them."""

        with self._lock:
            self._unavailable.update(self._values)
            for robot_id in self._values:
                self._on_change(robot_id, None)

    def apply(
        self, robot_id: str, owner_epoch: str, registration_generation: int,
        sequence: int, entry: Mapping[str, Any] | None,
    ) -> bool:
        """Apply a confirmed value and write its stores before returning.

        True means this exact value is current; False means a stale owner,
        registration, sequence, conflicting equal receipt, or incomplete store
        delivery. Result callers use ``apply_result_row`` to distinguish a
        stale owner from a same-owner delivery failure. The callback runs
        under the mirror lock.
        """

        with self._lock:
            return self._apply_locked(
                robot_id, owner_epoch, registration_generation, sequence, entry
            )

    def _apply_locked(
        self, robot_id: str, owner_epoch: str, registration_generation: int,
        sequence: int, entry: Mapping[str, Any] | None,
    ) -> bool:
        if owner_epoch != self._owner_epoch:
            return False
        previous = self._values.get(robot_id)
        previous_stamp = None if previous is None or previous[0] is None else (previous[0], previous[1])
        stamp = (registration_generation, sequence)
        if previous_stamp is not None and stamp < previous_stamp:
            return False
        if previous_stamp is None or stamp > previous_stamp:
            value = None if entry is None else deepcopy(dict(entry))
            # Retain the sequence before fanout: a partial store update must
            # not allow an older event to overwrite an already updated store.
            self._values[robot_id] = (registration_generation, sequence, value)
            self._pending.add(robot_id)
            self._deliver_locked(robot_id)
        elif stamp == previous_stamp and self._values[robot_id][2] == entry:
            if robot_id in self._unavailable:
                self._pending.add(robot_id)
            if robot_id in self._pending:
                self._deliver_locked(robot_id)
        return (
            robot_id not in self._pending and robot_id not in self._unavailable
            and self._values[robot_id][2] == entry
        )

    def _deliver_locked(self, robot_id: str) -> None:
        if robot_id not in self._pending:
            return
        self._on_change(robot_id, deepcopy(self._values[robot_id][2]))
        self._pending.remove(robot_id)
        self._unavailable.discard(robot_id)

    def apply_baseline(
        self, owner_epoch: str,
        values: Mapping[str, tuple[int, int, Mapping[str, Any] | None]],
    ) -> None:
        """Apply the decoded baseline with each robot's own sequence."""

        with self._lock:
            for robot_id, (generation, sequence, entry) in values.items():
                self._apply_locked(robot_id, owner_epoch, generation, sequence, entry)

    def apply_rows(self, rows: Any) -> dict[str, bool]:
        """Apply a fully decoded host row list under the existing owner fence.

        The returned value for each component says whether its exact reading
        is current and delivered. A result caller must check its own robot.
        """

        readings = decode_active_tcp_rows(rows)
        with self._lock:
            return {
                robot: self._apply_locked(robot, owner, generation, sequence, entry)
                for robot, owner, generation, sequence, entry in readings
            }

    def apply_reset_rows(
        self, rows: Any, required: set[str], *, ordered_owner: bool = False,
    ) -> None:
        """Apply and certify each currently bound robot under the owner fence."""

        readings = decode_active_tcp_rows(rows)
        by_robot = {}
        for reading in readings:
            if reading[0] in by_robot:
                raise ActiveTcpWireError("Active TCP reset contains duplicate robot")
            by_robot[reading[0]] = reading
        if required - by_robot.keys():
            raise ActiveTcpWireError("Active TCP reset omitted a bound robot")
        owners = {reading[1] for reading in readings}
        if len(owners) > 1:
            raise ActiveTcpWireError("Active TCP reset names multiple owners")
        with self._lock:
            if ordered_owner and owners:
                self._set_owner_locked(next(iter(owners)))
            for robot in required:
                _, owner, generation, sequence, entry = by_robot[robot]
                if owner != self._owner_epoch:
                    raise ActiveTcpWireError("Active TCP reset names a stale owner")
                applied = self._apply_locked(robot, owner, generation, sequence, entry)
                current = self._values.get(robot)
                if not applied and (
                    robot in self._pending or robot in self._unavailable
                    or current is None or current[0] is None or current[1] is None
                    or (current[0], current[1]) <= (generation, sequence)
                ):
                    raise ActiveTcpWireError("Active TCP reset could not be delivered")

    def apply_event(self, owner_epoch: str, event: Mapping[str, Any]) -> bool | None:
        """Apply an Active TCP journal event; other event kinds are ignored."""

        if event.get("record_kind") != "active_tcp_changed":
            return None
        payload = event.get("payload")
        if not isinstance(payload, Mapping) or payload.get("api_version") != "manyforge.active_tcp_change.v1":
            raise ActiveTcpWireError("Active TCP event payload is invalid")
        robot, owner, generation, sequence, entry = _reading(payload)
        if owner != owner_epoch:
            raise ActiveTcpWireError("Active TCP event names another owner")
        return self.apply(robot, owner, generation, sequence, entry)

    def current(self) -> dict[str, tuple[int | None, int | None, dict[str, Any] | None]]:
        """Return a detached diagnostic snapshot."""

        with self._lock:
            return {
                robot: (generation, sequence, None if robot in self._unavailable else deepcopy(entry))
                for robot, (generation, sequence, entry) in self._values.items()
            }

    @property
    def owner_epoch(self) -> str | None:
        """The epoch most recently installed by the external owner fence."""

        with self._lock:
            return self._owner_epoch

    def apply_result_row(
        self, row: Any, *, write_current: Callable[[str, Mapping[str, Any] | None], None] | None = None
    ) -> bool | None:
        """Deliver a result, accept a newer value, or ignore a stale owner.

        None means the owner is stale; False means a conflict or incomplete
        delivery within the current owner. The owner check is under the lock.
        """

        robot, owner, generation, sequence, entry = _reading(row)
        with self._lock:
            if owner != self._owner_epoch:
                return None
            accepted = self._apply_locked(robot, owner, generation, sequence, entry)
            current = self._values.get(robot)
            if not accepted:
                if owner != self._owner_epoch or robot in self._pending or robot in self._unavailable:
                    return False
                if current is None or current[0] is None or current[1] is None:
                    return False
                if (current[0], current[1]) <= (generation, sequence):
                    return False
            if write_current is not None:
                write_current(robot, deepcopy(current[2]))
            return True

    def seed(
        self, write: Callable[[str, Mapping[str, Any] | None], None], *,
        owner_epoch: str | None = None,
        register: Callable[[], None] | None = None,
    ) -> bool:
        """Register and seed under the same lock that serializes updates."""

        with self._lock:
            if register is not None:
                register()
            if owner_epoch is not None and owner_epoch != self._owner_epoch:
                return False
            for robot_id, (_generation, _sequence, entry) in self._values.items():
                write(robot_id, None if robot_id in self._unavailable else deepcopy(entry))
            return True
