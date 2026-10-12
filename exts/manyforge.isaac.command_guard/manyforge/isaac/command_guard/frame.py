"""Immutable projection of the standard sensor_msgs/JointState command wire."""
from __future__ import annotations

from dataclasses import dataclass
import math
from typing import Any


def stamp_ns(stamp: Any) -> int:
    """Read the standard ROS Time representation without float conversion."""
    seconds, nanoseconds = stamp.sec, stamp.nanosec
    if (type(seconds) is not int or not -(2**31) <= seconds < 2**31 or
            type(nanoseconds) is not int or not 0 <= nanoseconds < 1_000_000_000):
        raise ValueError("invalid ROS Time")
    return seconds * 1_000_000_000 + nanoseconds


@dataclass(frozen=True)
class JointCommand:
    stamp_ns: int
    names: tuple[str, ...]
    positions: tuple[float, ...]
    velocities: tuple[float, ...] = ()
    efforts: tuple[float, ...] = ()

    @classmethod
    def from_joint_state(cls, message: Any) -> JointCommand:
        # Decode/copy in the receiving callback, before guard admission. No JSON,
        # generated message, goal key, source sequence or remote-clock subtraction.
        return cls(stamp_ns(message.header.stamp), tuple(message.name),
                   tuple(message.position), tuple(message.velocity), tuple(message.effort))


def validate_command(command: JointCommand, joint_names: tuple[str, ...]) -> None:
    if not isinstance(command, JointCommand) or type(command.stamp_ns) is not int:
        raise ValueError("invalid JointState command")
    if (type(command.names) is not tuple or len(command.names) != len(joint_names) or
            any(type(name) is not str for name in command.names) or
            len(set(command.names)) != len(command.names) or
            set(command.names) != set(joint_names)):
        raise ValueError("command joints do not match configured writer")
    for values, required in ((command.positions, True), (command.velocities, False),
                             (command.efforts, False)):
        if (type(values) is not tuple or (required and len(values) != len(command.names)) or
                (values and len(values) != len(command.names)) or
                any(type(value) not in (int, float) or not math.isfinite(value) for value in values)):
            raise ValueError("invalid JointState arrays")


def project_joint_state(message: Any, joint_names: tuple[str, ...]) -> JointCommand | None:
    """Select the arm from a shared stream; disjoint gripper packets are ignored.

    Positions are the only SDK effect. Optional arrays are checked but never
    translated into velocity or effort effects by the receiving writer.
    """
    names = tuple(message.name)
    if not set(joint_names).intersection(names):
        return None
    if (any(type(name) is not str or not name for name in names) or
            len(set(names)) != len(names) or not set(joint_names).issubset(names)):
        raise ValueError("partial or duplicate configured arm joints")
    positions, velocities, efforts = (tuple(message.position), tuple(message.velocity),
                                     tuple(message.effort))
    for values, required in ((positions, True), (velocities, False), (efforts, False)):
        if ((required and len(values) != len(names)) or
                (values and len(values) != len(names)) or
                any(type(value) not in (int, float) or not math.isfinite(value) for value in values)):
            raise ValueError("invalid shared JointState arrays")
    indices = tuple(names.index(name) for name in joint_names)
    command = JointCommand(stamp_ns(message.header.stamp), joint_names,
                           tuple(positions[index] for index in indices))
    validate_command(command, joint_names)
    return command
