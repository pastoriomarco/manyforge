"""The one NRT JSON command frame carried in std_msgs/String.data."""
from __future__ import annotations

from dataclasses import dataclass
import json
import math

SCHEMA = "manyforge.isaac.command_frame.v1"
UINT64_MAX = (1 << 64) - 1
MAX_JOINTS = 16
MAX_KEYS = 4


def uint64(value: object, *, nonzero: bool = False) -> int:
    if type(value) is not int or not (int(nonzero) <= value <= UINT64_MAX):
        raise ValueError("expected lossless uint64")
    return value


def identity(value: object, bound: int) -> str:
    if type(value) is not str or not value or len(value.encode("utf-8")) > bound:
        raise ValueError("invalid bounded identity")
    return value


def _fields(value: object, names: set[str]) -> dict:
    if type(value) is not dict or set(value) != names:
        raise ValueError("unexpected frame fields")
    return value


@dataclass(frozen=True)
class Binding:
    active_control_configuration_generation: int
    provider_runtime_epoch: str


@dataclass(frozen=True)
class GoalKey:
    operation_id: str
    controller_goal_id: str
    controller_goal_generation: int
    prepared_execution_id: int
    binding: Binding


@dataclass(frozen=True)
class BoundaryAddress:
    control_binding_id: str
    control_route_id: str
    active_control_configuration_id: str
    provider_instance_id: str
    provider_runtime_unit_id: str
    command_target_id: str


def _validate_address(address: BoundaryAddress, config: GuardConfig) -> None:
    if type(address) is not BoundaryAddress:
        raise ValueError("invalid boundary address")
    for value in address.__dict__.values():
        identity(value, config.max_identity_bytes)


@dataclass(frozen=True)
class GuardConfig:
    owner_epoch: str
    occurrence: str
    binding: Binding
    address: BoundaryAddress
    joint_names: tuple[str, ...]
    max_encoded_bytes: int
    max_identity_bytes: int
    max_command_age_ns: int

    def __post_init__(self) -> None:
        for value in (self.max_encoded_bytes, self.max_identity_bytes, self.max_command_age_ns):
            uint64(value, nonzero=True)
        identity(self.owner_epoch, self.max_identity_bytes)
        identity(self.occurrence, self.max_identity_bytes)
        _validate_binding(self.binding, self)
        _validate_address(self.address, self)
        if (type(self.joint_names) is not tuple or
                not 1 <= len(self.joint_names) <= MAX_JOINTS or
                len(set(self.joint_names)) != len(self.joint_names)):
            raise ValueError("expected exact unique joint order")
        for name in self.joint_names:
            identity(name, self.max_identity_bytes)


@dataclass(frozen=True)
class CommandFrame:
    owner_epoch: str
    occurrence: str
    key: GoalKey
    joint_names: tuple[str, ...]
    positions_rad: tuple[float, ...]
    producer_sequence: int
    consumed_goal_generation: int
    consumed_scale_generation: int
    consumed_stop_generation: int
    final_command: bool
    address: BoundaryAddress


def _validate_binding(binding: Binding, config: GuardConfig) -> None:
    if type(binding) is not Binding:
        raise ValueError("invalid binding")
    uint64(binding.active_control_configuration_generation, nonzero=True)
    identity(binding.provider_runtime_epoch, config.max_identity_bytes)


def validate_key(key: GoalKey, config: GuardConfig) -> None:
    if type(key) is not GoalKey:
        raise ValueError("invalid full key")
    identity(key.operation_id, config.max_identity_bytes)
    identity(key.controller_goal_id, config.max_identity_bytes)
    uint64(key.controller_goal_generation, nonzero=True)
    uint64(key.prepared_execution_id, nonzero=True)
    _validate_binding(key.binding, config)
    if key.binding != config.binding:
        raise ValueError("foreign binding")


def _position(value: object) -> float:
    if type(value) not in (int, float):
        raise ValueError("invalid position")
    try:
        result = float(value)
    except OverflowError as error:
        raise ValueError("position overflow") from error
    if not math.isfinite(result) or (type(value) is int and int(result) != value):
        raise ValueError("nonfinite or lossy position")
    return result


def validate_frame(frame: CommandFrame, config: GuardConfig) -> None:
    if type(frame) is not CommandFrame:
        raise ValueError("invalid frame")
    if frame.owner_epoch != config.owner_epoch or frame.occurrence != config.occurrence:
        raise ValueError("foreign owner or occurrence")
    validate_key(frame.key, config)
    _validate_address(frame.address, config)
    if frame.address != config.address:
        raise ValueError("foreign boundary address")
    if frame.joint_names != config.joint_names or type(frame.joint_names) is not tuple:
        raise ValueError("unexpected joint order")
    if type(frame.positions_rad) is not tuple or len(frame.positions_rad) != len(config.joint_names):
        raise ValueError("incomplete command")
    for position in frame.positions_rad:
        _position(position)
    uint64(frame.producer_sequence, nonzero=True)
    uint64(frame.consumed_goal_generation, nonzero=True)
    uint64(frame.consumed_scale_generation)
    uint64(frame.consumed_stop_generation)
    if frame.consumed_goal_generation != frame.key.controller_goal_generation:
        raise ValueError("uncorrelated consumed goal")
    if type(frame.final_command) is not bool:
        raise ValueError("invalid final marker")


def _unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate frame field")
        result[key] = value
    return result


def _invalid_constant(value: str) -> None:
    raise ValueError(f"nonfinite JSON constant: {value}")


def decode_frame(payload: str, config: GuardConfig) -> CommandFrame:
    if type(payload) is not str or len(payload) > config.max_encoded_bytes:
        raise ValueError("invalid or overlong payload")
    if len(payload.encode("utf-8")) > config.max_encoded_bytes:
        raise ValueError("overlong encoded payload")
    try:
        raw = json.loads(payload, object_pairs_hook=_unique_object, parse_constant=_invalid_constant)
    except (RecursionError, json.JSONDecodeError) as error:
        raise ValueError("malformed frame") from error
    raw = _fields(raw, {
        "schema", "owner_epoch", "occurrence", "key", "joint_names", "positions_rad",
        "producer_sequence", "consumed_goal_generation", "consumed_scale_generation",
        "consumed_stop_generation", "final_command", "address",
    })
    if raw["schema"] != SCHEMA:
        raise ValueError("unknown frame schema")
    address = _fields(raw["address"], {
        "control_binding_id", "control_route_id", "active_control_configuration_id",
        "provider_instance_id", "provider_runtime_unit_id", "command_target_id",
    })
    key = _fields(raw["key"], {
        "operation_id", "controller_goal_id", "controller_goal_generation",
        "prepared_execution_id", "binding",
    })
    binding = _fields(key["binding"], {
        "active_control_configuration_generation", "provider_runtime_epoch",
    })
    if type(raw["joint_names"]) is not list or type(raw["positions_rad"]) is not list:
        raise ValueError("expected command arrays")
    if (len(raw["joint_names"]) != len(config.joint_names) or
            len(raw["positions_rad"]) != len(config.joint_names)):
        raise ValueError("incomplete or oversized command")
    frame = CommandFrame(
        raw["owner_epoch"], raw["occurrence"], GoalKey(
            key["operation_id"], key["controller_goal_id"],
            key["controller_goal_generation"], key["prepared_execution_id"], Binding(**binding)),
        tuple(raw["joint_names"]), tuple(_position(p) for p in raw["positions_rad"]),
        raw["producer_sequence"], raw["consumed_goal_generation"],
        raw["consumed_scale_generation"], raw["consumed_stop_generation"], raw["final_command"],
        BoundaryAddress(**address),
    )
    validate_frame(frame, config)
    return frame


def encode_frame(frame: CommandFrame, config: GuardConfig) -> str:
    validate_frame(frame, config)
    from dataclasses import asdict
    raw = {"schema": SCHEMA, **asdict(frame)}
    payload = json.dumps(raw, sort_keys=True, separators=(",", ":"), ensure_ascii=False, allow_nan=False)
    if len(payload.encode("utf-8")) > config.max_encoded_bytes:
        raise ValueError("overlong encoded payload")
    return payload
