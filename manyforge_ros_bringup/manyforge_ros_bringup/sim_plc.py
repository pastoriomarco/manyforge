"""ManyForge simulated PLC: one rclpy node per simulated I/O device.

Shape of a ROXSIE-served PLC:
- status ``/<device>/ready`` (std_msgs/Bool), published at ``rate_hz``;
- PLC inputs ``/<device>/in_<i>`` (std_srvs/SetBool services); the response is
  the acknowledgement, sent ``ack_delay_s`` after the request;
- PLC outputs ``/<device>/out_<i>`` (std_msgs/Bool), published at ``rate_hz``.

With ``mirror`` on, ``out_i`` takes the value of ``in_i`` ``mirror_delay_s``
after an accepted write. ``ready`` is never mirrored and has no service.

Bank mode (``mode: bank``): the PLC side of one I/O bank (``ros_io_bank``,
``manyforge_io_interfaces``). Each cycle, at ``rate_hz`` (default 200), the
command block reads the latest ``/<device>/io_bank/command`` and applies it
when its ``seq`` differs from ``applied_seq``: each value 0 or 1 sets its
output (or is refused), any other value leaves it unchanged; the values
first, then ``refused`` and the result, then ``applied_seq``; the first
command read after a start or restart is adopted, not applied (the start-up
guard against replaying the last command). Then the state block publishes
``/<device>/io_bank/state`` with ``cycle`` + 1 and the real output values.
With ``mirror`` on, input ``i`` follows output ``i`` (a loop-back wiring).
Test-only ROS parameters: ``bank.response`` (``accept``, ``no_answer``),
``bank.refused`` (the output indexes it refuses, e.g. ``"2,5"``),
``bank.device_code`` (the code of a command with a refusal),
``bank.apply_delay_cycles`` (a late effect), ``bank.publishing`` (stream
stops), ``bank.frozen`` (samples repeat with a frozen ``cycle``) and
``bank.restart`` (setting it true restarts the PLC: ``cycle``, outputs and
``applied_seq`` reset).

Configuration: optional ``<config_dir>/<device>.yaml`` read once at start.
Test-only ROS parameters (``ready``, ``outputs.out_<i>.publishing``,
``inputs.in_<i>.response``, ``inputs.in_<i>.ack_delay_s``) change behaviour at
runtime. Exit codes: 0 on SIGINT/SIGTERM, 2 configuration error, 3 occupied
endpoint.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import heapq
import itertools
import math
from pathlib import Path
import re
import signal
import sys
import threading
import time
import uuid
from typing import Any, Callable, Iterable, Optional, Protocol

EXIT_OK = 0
EXIT_CONFIG = 2
EXIT_OCCUPIED = 3

MAX_COUNT = 64
MAX_RATE_HZ = 100.0
MODES = ("signals", "bank")
BANK_SIZE = 64
MAX_BANK_RATE_HZ = 1000.0
DEFAULT_BANK_RATE_HZ = 200.0
BANK_RESPONSES = ("accept", "no_answer")
# IoBankCommand.values.
OUTPUT_OFF = 0
OUTPUT_ON = 1
OUTPUT_KEEP = 2
# IoBankState.applied_result values.
RESULT_UNSET = 0
RESULT_ACCEPTED = 1
RESULT_REFUSED = 2
DISCOVERY_S = 1.0
DUPLICATE_CHECK_PERIOD_S = 1.0
UNKNOWN_GRACE_S = 3.0
UNKNOWN_OWNER = "<unknown node>"
# Fast DDS (Lyrical) keeps a SIGKILLed participant's endpoints for its 20 s
# lease (measured 19.9-20.0 s); 25 s covers it plus the check period.
SUCCESSOR_WINDOW_S = 25.0
TICK_PERIOD_S = 0.01
RESPONSES = ("success", "refuse", "no_response", "absent")
_DEVICE_RE = re.compile(r"[A-Za-z][A-Za-z0-9_]*\Z")


class ConfigError(ValueError):
    """Invalid simulated PLC configuration or device id."""


@dataclass(frozen=True)
class SimPlcConfig:
    mode: str = "signals"
    inputs: int = 8
    outputs: int = 8
    ready: bool = True
    rate_hz: float = 10.0
    ack_delay_s: float = 0.1
    mirror: bool = True
    mirror_delay_s: float = 0.5


_FIELDS = {
    "mode": "mode",
    "inputs": "count",
    "outputs": "count",
    "ready": "bool",
    "rate_hz": "rate",
    "ack_delay_s": "delay",
    "mirror": "bool",
    "mirror_delay_s": "delay",
}


def _number(key: str, value: Any) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ConfigError(f"'{key}' must be a number, got {type(value).__name__}")
    result = float(value)
    if not math.isfinite(result):
        raise ConfigError(f"'{key}' must be finite")
    return result


def parse_config(data: Any) -> SimPlcConfig:
    """Strictly validate a parsed YAML document; ``None`` (empty file) means defaults."""
    if data is None:
        return SimPlcConfig()
    if not isinstance(data, dict):
        raise ConfigError("top level must be a mapping")
    values: dict[str, Any] = {}
    bank = data.get("mode") == "bank"
    # A bank has 64 inputs and 64 outputs and answers in its state.
    allowed = ("mode", "rate_hz", "mirror") if bank else tuple(_FIELDS)
    for key, value in data.items():
        kind = _FIELDS.get(key) if isinstance(key, str) and key in allowed else None
        if kind is None:
            raise ConfigError(f"unknown key '{key}' (allowed: {', '.join(allowed)})")
        if kind == "mode":
            if value not in MODES:
                raise ConfigError(f"'{key}' must be one of {', '.join(MODES)}, got {value!r}")
            values[key] = value
        elif kind == "bool":
            if not isinstance(value, bool):
                raise ConfigError(f"'{key}' must be a boolean, got {type(value).__name__}")
            values[key] = value
        elif kind == "count":
            if isinstance(value, bool) or not isinstance(value, int):
                raise ConfigError(f"'{key}' must be an integer, got {type(value).__name__}")
            if not 0 <= value <= MAX_COUNT:
                raise ConfigError(f"'{key}' must be in 0..{MAX_COUNT}, got {value}")
            values[key] = value
        elif kind == "rate":
            rate = _number(key, value)
            limit = MAX_BANK_RATE_HZ if bank else MAX_RATE_HZ
            if not 0.0 < rate <= limit:
                raise ConfigError(f"'{key}' must be > 0 and <= {limit:g}, got {value}")
            values[key] = rate
        else:
            delay = _number(key, value)
            if delay < 0.0:
                raise ConfigError(f"'{key}' must be >= 0, got {value}")
            values[key] = delay
    if bank:
        values.setdefault("rate_hz", DEFAULT_BANK_RATE_HZ)
        values.update(inputs=BANK_SIZE, outputs=BANK_SIZE)
    return SimPlcConfig(**values)


def config_path(config_dir: Path, device: str) -> Path:
    return Path(config_dir) / f"{device}.yaml"


def load_config(config_dir: Path, device: str) -> SimPlcConfig:
    """Read ``<config_dir>/<device>.yaml``; a missing file means defaults."""
    import yaml

    validate_device(device)
    path = config_path(config_dir, device)
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return SimPlcConfig()
    except OSError as error:
        raise ConfigError(f"{path}: cannot read: {error}") from error
    try:
        data = yaml.safe_load(text)
    except yaml.YAMLError as error:
        raise ConfigError(f"{path}: invalid YAML: {error}") from error
    try:
        return parse_config(data)
    except ConfigError as error:
        raise ConfigError(f"{path}: {error}") from error


def validate_device(device: str) -> str:
    if not isinstance(device, str) or not _DEVICE_RE.match(device):
        raise ConfigError(f"device id '{device}' must match [A-Za-z][A-Za-z0-9_]*")
    return device


@dataclass(frozen=True)
class Endpoints:
    # The status topic (bank mode: the bank's state topic).
    ready: str
    inputs: tuple[str, ...]
    outputs: tuple[str, ...]


def bank_topics(device: str) -> tuple[str, str]:
    """``(state, command)`` topics of a bank (``/<device>/io_bank/...``)."""
    validate_device(device)
    return f"/{device}/io_bank/state", f"/{device}/io_bank/command"


def endpoint_names(device: str, config: SimPlcConfig) -> Endpoints:
    validate_device(device)
    if config.mode == "bank":
        # The only endpoint it serves: its state publisher.
        return Endpoints(ready=bank_topics(device)[0], inputs=(), outputs=())
    return Endpoints(
        ready=f"/{device}/ready",
        inputs=tuple(f"/{device}/in_{i}" for i in range(config.inputs)),
        outputs=tuple(f"/{device}/out_{i}" for i in range(config.outputs)),
    )


@dataclass(frozen=True)
class WriteDecision:
    """What the service does with one SetBool request."""

    response: str  # one of RESPONSES except "absent"
    deadline: float  # monotonic time at which to answer (ignored for no_response)
    success: bool
    message: str


class SimPlcCore:
    """ROS-free PLC state. Not thread-safe; the node serializes access."""

    def __init__(self, config: SimPlcConfig) -> None:
        self.config = config
        self.ready = config.ready
        self.inputs = [False] * config.inputs
        self.outputs = [False] * config.outputs
        self.publishing = [True] * config.outputs
        self.responses = ["success"] * config.inputs
        self.ack_delays = [config.ack_delay_s] * config.inputs
        self._mirrors: list[tuple[float, int, int, bool]] = []
        self._acks: list[tuple[float, int, int, Any]] = []
        self._seq = itertools.count()

    def write(self, index: int, value: bool, now: float) -> WriteDecision:
        """Handle a SetBool on ``in_<index>`` received at ``now``."""
        mode = self.responses[index]
        deadline = now + self.ack_delays[index]
        if mode == "refuse":
            return WriteDecision("refuse", deadline, False, f"in_{index} refused (test injection)")
        if mode != "success":  # no_response; "absent" has no service to call
            return WriteDecision("no_response", deadline, False, "")
        self.inputs[index] = bool(value)
        if self.config.mirror and index < len(self.outputs):
            heapq.heappush(
                self._mirrors,
                (now + self.config.mirror_delay_s, next(self._seq), index, bool(value)),
            )
        return WriteDecision("success", deadline, True, f"in_{index} set to {bool(value)}")

    def schedule_ack(self, deadline: float, index: int, token: Any) -> None:
        heapq.heappush(self._acks, (deadline, next(self._seq), index, token))

    def drop_acks(self, index: int) -> list[Any]:
        """Forget pending acknowledgements of one input (its service went absent)."""
        dropped = [entry[3] for entry in self._acks if entry[2] == index]
        self._acks = [entry for entry in self._acks if entry[2] != index]
        heapq.heapify(self._acks)
        return dropped

    def advance(self, now: float) -> list[Any]:
        """Apply due mirror events (in arrival order); return due ack tokens."""
        while self._mirrors and self._mirrors[0][0] <= now:
            _deadline, _seq, index, value = heapq.heappop(self._mirrors)
            self.outputs[index] = value
        due = []
        while self._acks and self._acks[0][0] <= now:
            due.append(heapq.heappop(self._acks)[3])
        return due

    def samples(self) -> tuple[bool, list[tuple[int, bool]]]:
        """``ready`` and the (index, value) of every output that is publishing."""
        return self.ready, [
            (i, value) for i, value in enumerate(self.outputs) if self.publishing[i]
        ]


@dataclass(frozen=True)
class BankCommand:
    seq: int = 0
    # One value per output: OUTPUT_OFF/ON set it, anything else keeps it
    # (a data block before any command holds zeros).
    values: tuple[int, ...] = (OUTPUT_OFF,) * BANK_SIZE


def bank_command(seq: int, **outputs: bool) -> BankCommand:
    """A command setting ``Q<i>=value`` outputs, KEEP for the others (tests)."""
    values = [OUTPUT_KEEP] * BANK_SIZE
    for name, value in outputs.items():
        values[int(name[1:])] = OUTPUT_ON if value else OUTPUT_OFF
    return BankCommand(seq, tuple(values))


def parse_refused(text: str) -> frozenset[int]:
    """``bank.refused``: comma-separated output indexes 0..63 (empty: none)."""
    indexes = set()
    for part in (piece.strip() for piece in text.split(",")):
        if not part:
            continue
        if not part.isdigit() or int(part) >= BANK_SIZE:
            raise ConfigError(f"bank.refused: {part!r} is not an output index 0..{BANK_SIZE - 1}")
        indexes.add(int(part))
    return frozenset(indexes)


@dataclass(frozen=True)
class BankState:
    """One ``IoBankState`` sample."""

    cycle: int
    applied_seq: int
    applied_result: int
    device_code: int
    inputs: tuple[bool, ...]
    outputs: tuple[bool, ...]
    refused: tuple[bool, ...]


class BankCore:
    """ROS-free PLC side of one bank, one ``step`` per PLC cycle. Not
    thread-safe; the node serializes access."""

    def __init__(self, mirror: bool = True) -> None:
        self.mirror = mirror
        self.response = "accept"
        self.refuse: frozenset[int] = frozenset()
        self.device_code = 2
        self.apply_delay_cycles = 0
        self.frozen = False
        # The command data block: the latest command received (zeros before any).
        self.command = BankCommand()
        self.inputs = [False] * BANK_SIZE
        self.restart()

    def restart(self) -> None:
        """A PLC (re)start: non-retain data back to zero; OB100's guard set.
        The command data block still holds the last command received."""
        self.cycle = 0
        self.outputs = [False] * BANK_SIZE
        self.applied_seq = 0
        self.applied_result = RESULT_UNSET
        self.applied_code = 0
        self.refused = [False] * BANK_SIZE
        self.first_command = True
        self._seen_seq: Optional[int] = None
        self._seen_at = 0

    def receive(self, command: BankCommand) -> None:
        """The latest command (the topic keeps one)."""
        self.command = command

    def step(self) -> BankState:
        """One PLC cycle: the command block, then the state block."""
        if not self.frozen:
            self._apply_command()
            self.cycle = (self.cycle + 1) % (1 << 32)
            if self.mirror:
                self.inputs = list(self.outputs)
        return BankState(self.cycle, self.applied_seq, self.applied_result,
                         self.applied_code, tuple(self.inputs), tuple(self.outputs),
                         tuple(self.refused))

    def _apply_command(self) -> None:
        command = self.command
        if self.first_command:
            # Start-up guard: the first command read is adopted, not applied.
            self.applied_seq = command.seq
            self.first_command = False
            return
        if command.seq == self.applied_seq or self.response == "no_answer":
            return
        # A late effect: the command takes effect some cycles after it arrived.
        if self._seen_seq != command.seq:
            self._seen_seq, self._seen_at = command.seq, self.cycle
        if self.cycle - self._seen_at < self.apply_delay_cycles:
            return
        refused = [False] * BANK_SIZE
        for index, value in enumerate(command.values):
            if value not in (OUTPUT_OFF, OUTPUT_ON):
                continue  # KEEP (or any other value): unchanged
            if index in self.refuse:
                refused[index] = True
            else:
                self.outputs[index] = value == OUTPUT_ON  # the values first,
        self.refused = refused  # then refused and the result,
        if any(refused):
            self.applied_result, self.applied_code = RESULT_REFUSED, self.device_code
        else:
            self.applied_result, self.applied_code = RESULT_ACCEPTED, 0
        self.applied_seq = command.seq  # then the sequence


GUID_PREFIX_LEN = 12  # RTPS GUID = 12-byte participant prefix + 4-byte entity id


class Graph(Protocol):
    """Servers/publishers of an endpoint as ``(owner node FQN, (gid, ...))``.

    A service entry carries its request-reader and response-writer gids. rmw
    pairs them by node FQN and service name, so with two same-FQN servers an
    entry can mix our writer with the other's reader (seen on Lyrical/Fast DDS).
    Ownership is therefore decided per gid, never per entry.
    """

    def service_owners(self, service_name: str) -> list[tuple[str, Any]]: ...

    def publisher_owners(self, topic_name: str) -> list[tuple[str, Any]]: ...


def fully_qualified(namespace: str, name: str) -> str:
    return f"{namespace.rstrip('/')}/{name}"


class RclpyGraph:
    """Graph view of an rclpy node (``get_servers_info_by_service`` and
    ``get_publishers_info_by_topic``; both exist in Lyrical rclpy)."""

    def __init__(self, node: Any) -> None:
        self._node = node

    def service_owners(self, service_name: str) -> list[tuple[str, Any]]:
        return [
            (_owner(info), tuple(tuple(gid) for gid in info.endpoint_gids))
            for info in self._node.get_servers_info_by_service(service_name)
        ]

    def publisher_owners(self, topic_name: str) -> list[tuple[str, Any]]:
        return [
            (_owner(info), (tuple(info.endpoint_gid),))
            for info in self._node.get_publishers_info_by_topic(topic_name)
        ]


def _owner(info: Any) -> str:
    # An endpoint is often discovered before its participant's node list
    # (ros_discovery_info); until then rmw reports _NODE_NAME_UNKNOWN_.
    if info.node_name == "_NODE_NAME_UNKNOWN_":
        return UNKNOWN_OWNER
    return fully_qualified(info.node_namespace, info.node_name)


def foreign_owners(
    graph: Graph, endpoints: Endpoints, own_prefix: Optional[tuple] = None,
) -> dict[str, list[str]]:
    """``{endpoint: [owner, ...]}`` for every server/publisher that is not one
    of this instance's own endpoints (identified by endpoint gid).

    Clients and subscriptions never count: ManyForge's own may already exist.
    Owners may be another node, this instance's own FQN (a stale predecessor
    or a live duplicate), or ``UNKNOWN_OWNER`` (node list not yet known).

    A gid is ours only if it carries our participant's RTPS GUID prefix
    (``own_prefix``, from ``participant_prefix``): the process has one
    participant and one node, so that prefix is this instance. Nothing that is
    merely discovered is ever marked as ours. Per gid, because rmw can pair a
    same-FQN twin's request reader with our response writer.
    """
    def own(gid: Any) -> bool:
        return own_prefix is not None and tuple(gid[:GUID_PREFIX_LEN]) == own_prefix

    found: dict[str, list[str]] = {}
    queries = [(name, graph.service_owners) for name in endpoints.inputs]
    queries += [(name, graph.publisher_owners) for name in (endpoints.ready, *endpoints.outputs)]
    for name, owners in queries:
        others = [
            owner for owner, gids in owners(name)
            if not all(own(gid) for gid in gids)
        ]
        if others:
            found[name] = others
    return found


def other_nodes(found: dict[str, list[str]], own_name: str) -> dict[str, list[str]]:
    """Endpoints with an owner that is a known node with a different FQN."""
    return {
        name: owners for name, owners in found.items()
        if any(owner not in (own_name, UNKNOWN_OWNER) for owner in owners)
    }


class DuplicateWatch:
    """Running-check decision on an injected clock. Per endpoint:

    - a known node with another FQN fails at once;
    - same-FQN endpoints that are not ours are allowed only while
      ``now - started_at < window_s``: a stale predecessor (e.g. SIGKILLed)
      disappears when its DDS lease expires; a live duplicate does not;
    - endpoints owned only by still-unattributed nodes fail after ``grace_s``.
    """

    def __init__(self, own_name: str, started_at: float,
                 window_s: float = SUCCESSOR_WINDOW_S,
                 grace_s: float = UNKNOWN_GRACE_S) -> None:
        self.own_name, self.started_at = own_name, started_at
        self.window_s, self.grace_s = window_s, grace_s
        self._unknown_since: dict[str, float] = {}

    def update(self, found: dict[str, list[str]], now: float) -> dict[str, list[str]]:
        failed = other_nodes(found, self.own_name)
        unknown = {name for name, owners in found.items() if UNKNOWN_OWNER in owners}
        self._unknown_since = {name: self._unknown_since.get(name, now) for name in unknown}
        for name, owners in found.items():
            if name in failed:
                continue
            if self.own_name in owners and now - self.started_at >= self.window_s:
                failed[name] = owners
            elif name in unknown and now - self._unknown_since[name] >= self.grace_s:
                failed[name] = owners
        return failed


def foreign_endpoints(graph: Graph, endpoints: Endpoints, own_name: str) -> list[str]:
    """Endpoints the preflight refuses: served/published by another node."""
    return list(other_nodes(foreign_owners(graph, endpoints), own_name))


def describe(found: dict[str, list[str]]) -> str:
    return ", ".join(f"{name} (by {', '.join(owners)})" for name, owners in found.items())


def wait_for_occupied(
    graph: Graph,
    endpoints: Endpoints,
    own_name: str,
    spin_once: Callable[[float], None],
    duration_s: float = DISCOVERY_S,
    clock: Callable[[], float] = time.monotonic,
) -> dict[str, list[str]]:
    """Bounded discovery: return as soon as another node's endpoint is seen.

    Same-FQN endpoints (stale predecessor or live duplicate) and still
    unattributed ones are left to the running check (``DuplicateWatch``).
    """
    until = clock() + duration_s
    while True:
        busy = other_nodes(foreign_owners(graph, endpoints), own_name)
        if busy or clock() >= until:
            return busy
        spin_once(0.1)


def _make_node(
    device: str, config: SimPlcConfig, context: Any = None,
    on_duplicate: Optional[Callable[[list[str]], None]] = None,
):
    from rcl_interfaces.msg import ParameterDescriptor, SetParametersResult
    from rclpy.callback_groups import MutuallyExclusiveCallbackGroup, ReentrantCallbackGroup
    from rclpy.node import Node
    from rclpy.task import Future
    from std_msgs.msg import Bool
    from std_srvs.srv import SetBool

    class SimPlcNode(Node):
        def __init__(self) -> None:
            super().__init__(f"sim_plc_{device}", namespace="/manyforge", context=context)
            self.device = device
            self.endpoints = endpoint_names(device, config)
            self.core = SimPlcCore(config)
            self.bank = BankCore(config.mirror) if config.mode == "bank" else None
            self._lock = threading.Lock()
            self._input_services: list[Any] = [None] * config.inputs
            self._parked: list[Any] = []  # futures that are never answered
            self._service_group = ReentrantCallbackGroup()
            self._own_prefix: Optional[tuple] = None

        def participant_prefix(self) -> tuple:
            """Our participant's RTPS GUID prefix, read from one of our own entities.

            rclpy exposes no gid for its own publishers/services, so a probe
            publisher goes on a fresh, unique topic: the only publisher there is
            ours, whatever else is being discovered at the same time.
            """
            probe_topic = f"/manyforge/_sim_plc_probe_{uuid.uuid4().hex}"
            probe = self.create_publisher(Bool, probe_topic, 1)
            try:
                for _attempt in range(100):
                    infos = self.get_publishers_info_by_topic(probe_topic)
                    if infos:
                        break
                    time.sleep(0.001)
                if len(infos) != 1:
                    raise RuntimeError(f"cannot identify own probe publisher ({len(infos)})")
                gid = tuple(infos[0].endpoint_gid)
            finally:
                self.destroy_publisher(probe)
            if len(gid) < GUID_PREFIX_LEN + 4:
                raise RuntimeError(
                    f"endpoint gid of {len(gid)} bytes is not an RTPS GUID; "
                    "own-endpoint identity needs an RTPS rmw (Fast DDS)"
                )
            return gid[:GUID_PREFIX_LEN]

        def check_occupied(self, duration_s: float = DISCOVERY_S) -> dict[str, list[str]]:
            import rclpy

            return wait_for_occupied(
                RclpyGraph(self),
                self.endpoints,
                self.get_fully_qualified_name(),
                lambda timeout: rclpy.spin_once(self, timeout_sec=timeout),
                duration_s,
            )

        def start(self) -> None:
            """Declare parameters and create every endpoint."""
            if self.bank is not None:
                self._start_bank()
            else:
                self._start_signals()
            self.duplicates: dict[str, list[str]] = {}
            window = self.declare_parameter(
                "successor_window_s", SUCCESSOR_WINDOW_S,
                ParameterDescriptor(description="test only; read once at start"),
            ).value
            if isinstance(window, bool) or not isinstance(window, (int, float)) \
                    or not math.isfinite(window) or window < 0:
                raise ConfigError("successor_window_s must be a finite number >= 0")
            self._watch = DuplicateWatch(
                self.get_fully_qualified_name(), time.monotonic(), float(window)
            )
            self.create_timer(
                DUPLICATE_CHECK_PERIOD_S, self._check_duplicates,
                callback_group=MutuallyExclusiveCallbackGroup(),
            )

        def _start_bank(self) -> None:
            from manyforge_io_interfaces.msg import IoBankCommand, IoBankState

            self._bank_state_type = IoBankState
            bank = self.bank
            for name, default in (
                ("bank.response", bank.response), ("bank.refused", ""),
                ("bank.device_code", bank.device_code),
                ("bank.apply_delay_cycles", bank.apply_delay_cycles),
                ("bank.publishing", True), ("bank.frozen", False), ("bank.restart", False),
            ):
                value = self.declare_parameter(name, default).value
                problem = _parameter_problem(name, value)
                if problem:
                    raise ConfigError(problem)
                self._apply_bank_parameter(name, value)
            self.add_on_set_parameters_callback(self._validate_parameters)
            self.add_post_set_parameters_callback(self._apply_parameters)
            self._own_prefix = self.participant_prefix()
            state_topic, command_topic = bank_topics(self.device)
            # As a PLC bridge: reliable, the latest sample / command only.
            self._state_pub = self.create_publisher(IoBankState, state_topic, 1)
            self.create_subscription(IoBankCommand, command_topic, self._on_command, 1)
            self.create_timer(
                1.0 / config.rate_hz, self._bank_cycle,
                callback_group=MutuallyExclusiveCallbackGroup(),
            )

        def _on_command(self, message) -> None:
            with self._lock:
                self.bank.receive(BankCommand(message.seq, tuple(int(value) for value in message.values)))

        def _bank_cycle(self) -> None:
            with self._lock:
                state = self.bank.step()
                publishing = self._bank_publishing
            if not publishing:
                return
            message = self._bank_state_type()
            message.cycle = state.cycle
            message.applied_seq = state.applied_seq
            message.applied_result = state.applied_result
            message.device_code = state.device_code
            message.inputs = list(state.inputs)
            message.outputs = list(state.outputs)
            message.refused = list(state.refused)
            self._state_pub.publish(message)

        def _apply_bank_parameter(self, name: str, value: Any) -> None:
            bank = self.bank
            if name == "bank.response":
                bank.response = value
            elif name == "bank.refused":
                bank.refuse = parse_refused(value)
            elif name == "bank.device_code":
                bank.device_code = int(value)
            elif name == "bank.apply_delay_cycles":
                bank.apply_delay_cycles = int(value)
            elif name == "bank.publishing":
                self._bank_publishing = bool(value)
            elif name == "bank.frozen":
                bank.frozen = bool(value)
            elif name == "bank.restart" and value:
                bank.restart()

        def _start_signals(self) -> None:
            core = self.core
            core.ready = bool(self.declare_parameter(
                "ready", config.ready, ParameterDescriptor(description="PLC status (test)")
            ).value)
            for i in range(config.outputs):
                core.publishing[i] = bool(
                    self.declare_parameter(f"outputs.out_{i}.publishing", True).value
                )
            for i in range(config.inputs):
                response = self.declare_parameter(f"inputs.in_{i}.response", "success").value
                delay = self.declare_parameter(
                    f"inputs.in_{i}.ack_delay_s", float(config.ack_delay_s)
                ).value
                problem = _parameter_problem(f"inputs.in_{i}.response", response) or \
                    _parameter_problem(f"inputs.in_{i}.ack_delay_s", delay)
                if problem:
                    raise ConfigError(problem)
                core.responses[i] = response
                core.ack_delays[i] = float(delay)
            self.add_on_set_parameters_callback(self._validate_parameters)
            self.add_post_set_parameters_callback(self._apply_parameters)

            self._own_prefix = self.participant_prefix()
            self._ready_pub = self.create_publisher(Bool, self.endpoints.ready, 10)
            self._out_pubs = [
                self.create_publisher(Bool, name, 10) for name in self.endpoints.outputs
            ]
            for i in range(config.inputs):
                if core.responses[i] != "absent":
                    self._create_input_service(i)
            self.create_timer(
                1.0 / config.rate_hz, self._publish,
                callback_group=MutuallyExclusiveCallbackGroup(),
            )
            self.create_timer(
                TICK_PERIOD_S, self._tick, callback_group=MutuallyExclusiveCallbackGroup()
            )

        def _check_duplicates(self) -> None:
            """Running check: covers late discovery and a peer that appears later."""
            if self.duplicates:
                return
            found = foreign_owners(RclpyGraph(self), self.endpoints, self._own_prefix)
            busy = self._watch.update(found, time.monotonic())
            if busy:
                self.duplicates = busy
                self.get_logger().error(
                    f"another node also serves/publishes {describe(busy)}; stopping (exit 3)"
                )
                if on_duplicate is not None:
                    on_duplicate(busy)

        def _create_input_service(self, index: int) -> None:
            # rclpy awaits the callback only if it is itself a coroutine function.
            async def callback(request, response):
                return await self._on_write(index, request, response)

            self._input_services[index] = self.create_service(
                SetBool, self.endpoints.inputs[index], callback,
                callback_group=self._service_group,
            )

        async def _on_write(self, index: int, request, response):
            future = Future(executor=self.executor)
            with self._lock:
                decision = self.core.write(index, request.data, time.monotonic())
                if decision.response == "no_response":
                    self._parked.append(future)
                else:
                    response.success = decision.success
                    response.message = decision.message
                    self.core.schedule_ack(decision.deadline, index, future)
            await future
            return response

        def _tick(self) -> None:
            with self._lock:
                due = self.core.advance(time.monotonic())
            for future in due:
                future.set_result(True)

        def _publish(self) -> None:
            with self._lock:
                ready, outputs = self.core.samples()
            self._ready_pub.publish(Bool(data=ready))
            for index, value in outputs:
                self._out_pubs[index].publish(Bool(data=value))

        def _validate_parameters(self, parameters) -> Any:
            for parameter in parameters:
                problem = _parameter_problem(parameter.name, parameter.value)
                if problem:
                    return SetParametersResult(successful=False, reason=problem)
            return SetParametersResult(successful=True)

        def _apply_parameters(self, parameters) -> None:
            for parameter in parameters:
                name, value = parameter.name, parameter.value
                with self._lock:
                    if name.startswith("bank."):
                        self._apply_bank_parameter(name, value)
                    elif name == "ready":
                        self.core.ready = bool(value)
                    elif name.startswith("outputs.out_") and name.endswith(".publishing"):
                        self.core.publishing[_index(name)] = bool(value)
                    elif name.startswith("inputs.in_") and name.endswith(".ack_delay_s"):
                        self.core.ack_delays[_index(name)] = float(value)
                    elif name.startswith("inputs.in_") and name.endswith(".response"):
                        self._set_response(_index(name), value)

        def _set_response(self, index: int, value: str) -> None:
            self.core.responses[index] = value
            service = self._input_services[index]
            if value == "absent" and service is not None:
                # Requests in flight on a vanished server are never answered.
                self._parked.extend(self.core.drop_acks(index))
                self._input_services[index] = None
                self.destroy_service(service)
            elif value != "absent" and service is None:
                self._create_input_service(index)

    return SimPlcNode()


def _index(name: str) -> int:
    return int(name.split(".")[1].split("_")[1])


def _parameter_problem(name: str, value: Any) -> Optional[str]:
    if name == "bank.response":
        return None if value in BANK_RESPONSES else \
            f"{name} must be one of {', '.join(BANK_RESPONSES)}"
    if name == "bank.refused":
        if not isinstance(value, str):
            return f"{name} must be text, e.g. '2,5'"
        try:
            parse_refused(value)
        except ConfigError as error:
            return str(error)
        return None
    if name in ("bank.device_code", "bank.apply_delay_cycles"):
        limit = 65535 if name == "bank.device_code" else 1_000_000
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= limit:
            return f"{name} must be an integer in 0..{limit}"
        return None
    if name in ("bank.publishing", "bank.frozen", "bank.restart"):
        return None if isinstance(value, bool) else f"{name} must be a boolean"
    if name.endswith(".response") and value not in RESPONSES:
        return f"{name} must be one of {', '.join(RESPONSES)}"
    if name.endswith(".ack_delay_s"):
        if isinstance(value, bool) or not isinstance(value, (int, float)) \
                or not math.isfinite(value) or value < 0:
            return f"{name} must be a finite number >= 0"
    return None


def parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog="sim_plc", description=__doc__.splitlines()[0])
    parser.add_argument("--device", required=True, help="I/O device id, e.g. plc_io")
    parser.add_argument("--config-dir", required=True, type=Path)
    return parser.parse_args(list(argv))


def start_spinning(executor) -> threading.Thread:
    """Run ``executor.spin()`` on an ordinary (non-daemon) thread (OD-259).

    A daemon thread could be ended by interpreter finalization inside rclpy's
    wait and abort the process; ``stop_spinning`` always joins this one.
    """
    spinner = threading.Thread(target=executor.spin, name="sim_plc_spin")
    spinner.start()
    return spinner


def stop_spinning(executor, spinner: Optional[threading.Thread]) -> None:
    """Shut the executor down and join its spin thread, without a time limit.

    ``executor.shutdown()`` wakes ``spin()`` and makes it return (OD-259).
    """
    executor.shutdown()
    if spinner is not None:
        spinner.join()


def main(argv: Optional[list[str]] = None) -> int:
    import rclpy
    from rclpy.executors import MultiThreadedExecutor
    from rclpy.signals import SignalHandlerOptions
    from rclpy.utilities import remove_ros_args

    raw = sys.argv if argv is None else argv
    args = parse_args(remove_ros_args(args=raw)[1:])
    try:
        config = load_config(args.config_dir, args.device)
    except ConfigError as error:
        print(f"sim_plc: configuration error: {error}", file=sys.stderr, flush=True)
        return EXIT_CONFIG

    # Orderly shutdown: SIGINT/SIGTERM only set a flag. The executor is then
    # stopped and the node destroyed while the context is still valid; letting
    # rclpy's own handler shut the context down under a running
    # MultiThreadedExecutor segfaulted intermittently (rclpy, Lyrical).
    stop = threading.Event()
    previous = {
        signum: signal.signal(signum, lambda _signum, _frame: stop.set())
        for signum in (signal.SIGINT, signal.SIGTERM)
    }
    rclpy.init(args=raw, signal_handler_options=SignalHandlerOptions.NO)
    node = None
    executor = None
    spinner = None
    try:
        node = _make_node(args.device, config, on_duplicate=lambda _busy: stop.set())
        busy = node.check_occupied()
        if stop.is_set():
            return EXIT_OK
        if busy:
            print(
                f"sim_plc: refusing to start; endpoint(s) already served/published by "
                f"another node: {describe(busy)}",
                file=sys.stderr, flush=True,
            )
            return EXIT_OCCUPIED
        try:
            node.start()
        except ConfigError as error:
            print(f"sim_plc: parameter error: {error}", file=sys.stderr, flush=True)
            return EXIT_CONFIG
        if config.mode == "bank":
            node.get_logger().info(
                f"simulated PLC '{args.device}' up: I/O bank of {BANK_SIZE} inputs and "
                f"{BANK_SIZE} outputs, rate={config.rate_hz:g} Hz"
            )
        else:
            node.get_logger().info(
                f"simulated PLC '{args.device}' up: {config.inputs} inputs, "
                f"{config.outputs} outputs, ready={node.core.ready}, rate={config.rate_hz:g} Hz"
            )
        executor = MultiThreadedExecutor()
        executor.add_node(node)
        # spin() runs in a worker thread; the main thread only waits for the
        # stop flag, so signal handling never depends on the wait set waking.
        # (A main-thread spin_once(timeout) loop hung intermittently in rclpy's
        # wait set after SIGINT.) executor.shutdown() wakes and ends spin().
        spinner = start_spinning(executor)
        while not stop.wait(0.2):
            if not spinner.is_alive():
                raise RuntimeError("simulated PLC executor stopped unexpectedly")
        if node.duplicates:
            print(
                f"sim_plc: stopping; endpoint(s) also served/published by another "
                f"node: {describe(node.duplicates)}",
                file=sys.stderr, flush=True,
            )
            return EXIT_OCCUPIED
        return EXIT_OK
    finally:
        if executor is not None:
            stop_spinning(executor, spinner)
        if node is not None:
            node.destroy_node()
        rclpy.try_shutdown()
        for signum, handler in previous.items():
            signal.signal(signum, handler)


if __name__ == "__main__":
    raise SystemExit(main())
