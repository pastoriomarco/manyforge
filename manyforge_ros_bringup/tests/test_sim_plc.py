from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import random
import signal
import subprocess
import sys
import time

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:  # run from the source tree without a colcon build
    sys.path.insert(0, str(ROOT))

from manyforge_ros_bringup.sim_plc import (  # noqa: E402
    ConfigError,
    EXIT_CONFIG,
    EXIT_OCCUPIED,
    SimPlcConfig,
    SimPlcCore,
    endpoint_names,
    load_config,
    DuplicateWatch,
    UNKNOWN_OWNER,
    RclpyGraph,
    foreign_endpoints,
    foreign_owners,
    other_nodes,
    fully_qualified,
    parse_config,
    wait_for_occupied,
)


# --------------------------------------------------------------------------- config


def test_defaults_match_contract():
    assert parse_config(None) == SimPlcConfig(
        inputs=8, outputs=8, ready=True, rate_hz=10.0,
        ack_delay_s=0.1, mirror=True, mirror_delay_s=0.5,
    )
    assert parse_config({}) == SimPlcConfig()


def test_missing_file_means_defaults(tmp_path):
    assert load_config(tmp_path, "plc_io") == SimPlcConfig()


def test_valid_file_is_read(tmp_path):
    (tmp_path / "plc_io.yaml").write_text(
        "inputs: 2\noutputs: 0\nready: false\nrate_hz: 100\n"
        "ack_delay_s: 0\nmirror: false\nmirror_delay_s: 1.5\n"
    )
    assert load_config(tmp_path, "plc_io") == SimPlcConfig(
        inputs=2, outputs=0, ready=False, rate_hz=100.0,
        ack_delay_s=0.0, mirror=False, mirror_delay_s=1.5,
    )


def test_other_devices_file_is_not_read(tmp_path):
    (tmp_path / "other.yaml").write_text("inputs: 1\n")
    assert load_config(tmp_path, "plc_io").inputs == 8


@pytest.mark.parametrize(
    "text",
    [
        "bogus: 1\n",                     # unknown key
        "- 1\n",                          # not a mapping
        "inputs: 65\n", "inputs: -1\n",   # count range
        "outputs: 65\n",
        "inputs: 2.0\n", "inputs: true\n", "outputs: '8'\n",  # count type
        "ready: 1\n", "mirror: 'yes'\n",  # bool type
        "rate_hz: 0\n", "rate_hz: -1\n", "rate_hz: 100.5\n", "rate_hz: .nan\n",
        "rate_hz: true\n", "rate_hz: fast\n",
        "ack_delay_s: -0.1\n", "mirror_delay_s: -1\n", "ack_delay_s: .inf\n",
        "mirror_delay_s: false\n",
        "inputs: [\n",                    # invalid YAML
    ],
)
def test_invalid_config_is_rejected(tmp_path, text):
    (tmp_path / "plc_io.yaml").write_text(text)
    with pytest.raises(ConfigError, match="plc_io.yaml"):
        load_config(tmp_path, "plc_io")


@pytest.mark.parametrize("device", ["", "1plc", "plc/io", "plc-io", "../x"])
def test_invalid_device_is_rejected(tmp_path, device):
    with pytest.raises(ConfigError):
        load_config(tmp_path, device)


# --------------------------------------------------------------------------- naming


def test_endpoint_names_are_absolute_and_per_device():
    names = endpoint_names("plc_io", SimPlcConfig(inputs=2, outputs=3))
    assert names.ready == "/plc_io/ready"
    assert names.inputs == ("/plc_io/in_0", "/plc_io/in_1")
    assert names.outputs == ("/plc_io/out_0", "/plc_io/out_1", "/plc_io/out_2")


def test_zero_counts_leave_only_ready():
    names = endpoint_names("p", SimPlcConfig(inputs=0, outputs=0))
    assert (names.inputs, names.outputs) == ((), ())


# --------------------------------------------------------------------------- core


def test_write_stores_value_and_acks_after_delay():
    core = SimPlcCore(SimPlcConfig(ack_delay_s=0.3))
    decision = core.write(2, True, now=10.0)
    assert decision.response == "success" and decision.success
    assert decision.deadline == pytest.approx(10.3)
    assert core.inputs[2] is True
    core.schedule_ack(decision.deadline, 2, "tok")
    assert core.advance(10.29) == []
    assert core.advance(10.3) == ["tok"]
    assert core.advance(11.0) == []


def test_per_input_ack_delay_override():
    core = SimPlcCore(SimPlcConfig(ack_delay_s=0.1))
    core.ack_delays[1] = 0.8
    assert core.write(1, True, 0.0).deadline == pytest.approx(0.8)
    assert core.write(0, True, 0.0).deadline == pytest.approx(0.1)


def test_mirror_follows_after_delay():
    core = SimPlcCore(SimPlcConfig(mirror_delay_s=0.5))
    core.write(0, True, now=1.0)
    core.advance(1.49)
    assert core.outputs[0] is False
    core.advance(1.5)
    assert core.outputs == [True] + [False] * 7


def test_mirror_applies_writes_in_order():
    core = SimPlcCore(SimPlcConfig(mirror_delay_s=0.5))
    core.write(0, True, now=0.0)
    core.write(0, False, now=0.2)
    core.advance(0.6)
    assert core.outputs[0] is True
    core.advance(0.7)
    assert core.outputs[0] is False


def test_mirror_off_keeps_outputs():
    core = SimPlcCore(SimPlcConfig(mirror=False))
    core.write(0, True, 0.0)
    core.advance(100.0)
    assert core.outputs[0] is False and core.inputs[0] is True


def test_input_without_matching_output_does_not_mirror():
    core = SimPlcCore(SimPlcConfig(inputs=3, outputs=1, mirror_delay_s=0.0))
    core.write(2, True, 0.0)
    core.advance(1.0)
    assert core.outputs == [False]


def test_refuse_neither_stores_nor_mirrors():
    core = SimPlcCore(SimPlcConfig(mirror_delay_s=0.0))
    core.responses[0] = "refuse"
    decision = core.write(0, True, 0.0)
    assert decision.response == "refuse" and not decision.success and decision.message
    core.advance(1.0)
    assert core.inputs[0] is False and core.outputs[0] is False


def test_no_response_neither_stores_nor_mirrors():
    core = SimPlcCore(SimPlcConfig(mirror_delay_s=0.0))
    core.responses[0] = "no_response"
    assert core.write(0, True, 0.0).response == "no_response"
    core.advance(1.0)
    assert core.inputs[0] is False and core.outputs[0] is False


def test_absent_drops_pending_acks_of_that_input_only():
    core = SimPlcCore(SimPlcConfig())
    core.schedule_ack(1.0, 0, "a")
    core.schedule_ack(1.0, 1, "b")
    assert core.drop_acks(0) == ["a"]
    assert core.advance(2.0) == ["b"]


def test_publishing_off_omits_output_sample():
    core = SimPlcCore(SimPlcConfig(outputs=3))
    core.publishing[1] = False
    ready, outputs = core.samples()
    assert ready is True
    assert [index for index, _ in outputs] == [0, 2]


def test_ready_is_not_mirrored_and_not_writable():
    core = SimPlcCore(SimPlcConfig(inputs=1, outputs=1, ready=False, mirror_delay_s=0.0))
    for value in (True, False, True):
        core.write(0, value, 0.0)
    core.advance(1.0)
    assert core.samples()[0] is False
    assert core.outputs == [True]


# --------------------------------------------------------------------------- preflight


OWN = "/manyforge/sim_plc_p"
REAL_PLC = "/roxsie/plc_bridge"


class FakeGraph:
    """Endpoint name -> [(owner FQN, gid), ...] (servers / publishers only).

    A bare owner string gets a unique gid. Clients and subscriptions are on the
    graph too but are never owners.
    """

    _gids = iter(range(10**6))

    def __init__(self, services=None, publishers=None, clients=(), subscriptions=()):
        self.services = {k: [self._entry(o) for o in v] for k, v in (services or {}).items()}
        self.publishers = {k: [self._entry(o) for o in v] for k, v in (publishers or {}).items()}
        self.clients, self.subscriptions = set(clients), set(subscriptions)

    @classmethod
    def _entry(cls, owner):
        if isinstance(owner, tuple):
            return owner
        return (owner, (f"gid{next(cls._gids)}",))

    def service_owners(self, name):
        return self.services.get(name, [])

    def publisher_owners(self, name):
        return self.publishers.get(name, [])


def test_preflight_refuses_other_node_server_or_publisher():
    names = endpoint_names("p", SimPlcConfig(inputs=2, outputs=2))
    graph = FakeGraph(services={"/p/in_1": [REAL_PLC]},
                      publishers={"/p/ready": [REAL_PLC], "/p/out_0": ["/other"]})
    assert foreign_endpoints(graph, names, OWN) == ["/p/in_1", "/p/ready", "/p/out_0"]


def test_clients_and_subscriptions_do_not_occupy():
    names = endpoint_names("p", SimPlcConfig(inputs=2, outputs=2))
    graph = FakeGraph(clients={"/p/in_0"}, subscriptions={"/p/ready", "/p/out_0"})
    assert foreign_endpoints(graph, names, OWN) == []


def test_preflight_defers_same_fqn_and_unknown_owners():
    names = endpoint_names("p", SimPlcConfig(inputs=1, outputs=1))
    graph = FakeGraph(services={"/p/in_0": [OWN]}, publishers={"/p/ready": [UNKNOWN_OWNER]})
    assert foreign_endpoints(graph, names, OWN) == []


OURS, THEIRS = (1,) * 12, (2,) * 12  # participant GUID prefixes


def _gid(prefix, entity):
    return prefix + (0, 0, entity, 3)


def test_own_endpoints_are_identified_by_participant_prefix_not_name():
    names = endpoint_names("p", SimPlcConfig(inputs=1, outputs=1))
    graph = FakeGraph(
        services={"/p/in_0": [(OWN, (_gid(OURS, 1), _gid(OURS, 2)))]},
        publishers={"/p/ready": [(OWN, (_gid(OURS, 3),)), (OWN, (_gid(THEIRS, 3),))]},
    )
    assert foreign_owners(graph, names, OURS) == {"/p/ready": [OWN]}
    assert foreign_owners(graph, names, None) == {"/p/in_0": [OWN], "/p/ready": [OWN, OWN]}


def test_mispaired_same_fqn_service_entry_is_still_foreign():
    # rmw pairs reader/writer by node FQN + service: the twin's entry can carry
    # the twin's request reader with OUR response writer, and vice versa.
    names = endpoint_names("p", SimPlcConfig(inputs=1, outputs=0))
    graph = FakeGraph(services={"/p/in_0": [
        (OWN, (_gid(OURS, 21), _gid(THEIRS, 22))),
        (OWN, (_gid(THEIRS, 21), _gid(OURS, 22))),
    ]})
    assert foreign_owners(graph, names, OURS) == {"/p/in_0": [OWN, OWN]}


def test_same_fqn_twin_is_never_own_whenever_discovered():
    # Regression (sim-plc-03): identity comes only from our own participant
    # prefix, never from what was discovered around endpoint creation.
    names = endpoint_names("p", SimPlcConfig(inputs=0, outputs=1))
    graph = FakeGraph(publishers={"/p/out_0": [(OWN, (_gid(OURS, 5),))]})
    assert foreign_owners(graph, names, OURS) == {}
    graph.publishers["/p/out_0"].append((OWN, (_gid(THEIRS, 5),)))
    assert foreign_owners(graph, names, OURS) == {"/p/out_0": [OWN]}


def test_other_nodes_ignores_same_fqn_and_unknown():
    found = {"/p/in_0": [OWN], "/p/ready": [UNKNOWN_OWNER], "/p/out_0": [OWN, REAL_PLC]}
    assert other_nodes(found, OWN) == {"/p/out_0": [OWN, REAL_PLC]}


def test_fully_qualified_names():
    assert fully_qualified("/", "n") == "/n"
    assert fully_qualified("/manyforge", "sim_plc_p") == OWN


def test_watch_other_node_fails_at_once():
    watch = DuplicateWatch(OWN, started_at=0.0, window_s=25.0, grace_s=3.0)
    assert watch.update({"/p/in_0": [REAL_PLC]}, now=0.5) == {"/p/in_0": [REAL_PLC]}


def test_watch_same_fqn_allowed_only_within_successor_window():
    watch = DuplicateWatch(OWN, started_at=100.0, window_s=25.0, grace_s=3.0)
    dup = {"/p/in_0": [OWN]}
    assert watch.update(dup, 101.0) == {}
    assert watch.update(dup, 124.9) == {}
    assert watch.update(dup, 125.0) == dup  # a live duplicate outlives any lease


def test_watch_stale_predecessor_disappearing_in_window_is_fine():
    watch = DuplicateWatch(OWN, started_at=0.0, window_s=25.0, grace_s=3.0)
    assert watch.update({"/p/in_0": [OWN]}, 1.0) == {}
    assert watch.update({}, 20.0) == {}  # lease expired
    assert watch.update({}, 60.0) == {}


def test_watch_same_fqn_appearing_after_window_fails():
    watch = DuplicateWatch(OWN, started_at=0.0, window_s=25.0, grace_s=3.0)
    assert watch.update({}, 10.0) == {}
    assert watch.update({"/p/ready": [OWN]}, 30.0) == {"/p/ready": [OWN]}


def test_watch_unknown_owner_gets_grace_then_fails():
    watch = DuplicateWatch(OWN, started_at=0.0, window_s=25.0, grace_s=3.0)
    unknown = {"/p/out_0": [UNKNOWN_OWNER]}
    assert watch.update(unknown, 10.0) == {}
    assert watch.update(unknown, 12.9) == {}
    assert watch.update(unknown, 13.0) == unknown


def test_watch_unknown_resolving_clears_and_restarts_grace():
    watch = DuplicateWatch(OWN, started_at=0.0, window_s=25.0, grace_s=3.0)
    assert watch.update({"/p/in_0": [UNKNOWN_OWNER]}, 0.0) == {}
    assert watch.update({}, 1.0) == {}
    assert watch.update({"/p/in_0": [UNKNOWN_OWNER]}, 2.0) == {}
    assert watch.update({"/p/in_0": [UNKNOWN_OWNER]}, 4.9) == {}


def test_rclpy_graph_maps_unknown_placeholder_names_and_gids():
    from types import SimpleNamespace as Info

    class Node:
        def get_servers_info_by_service(self, name):
            return [Info(node_name="_NODE_NAME_UNKNOWN_", node_namespace="_NODE_NAMESPACE_UNKNOWN_",
                         endpoint_gids=[[1, 2], [3]]),
                    Info(node_name="sim_plc_p", node_namespace="/manyforge", endpoint_gids=[[4]])]

        def get_publishers_info_by_topic(self, name):
            return [Info(node_name="plc_bridge", node_namespace="/roxsie", endpoint_gid=[5, 6])]

    graph = RclpyGraph(Node())
    assert graph.service_owners("/p/in_0") == [(UNKNOWN_OWNER, ((1, 2), (3,))), (OWN, ((4,),))]
    assert graph.publisher_owners("/p/ready") == [(REAL_PLC, ((5, 6),))]


def test_discovery_is_bounded_and_returns_early():
    names = endpoint_names("p", SimPlcConfig(inputs=1, outputs=0))
    clock = [0.0]
    spins = []

    def spin(timeout):
        spins.append(timeout)
        clock[0] += timeout

    assert wait_for_occupied(FakeGraph(), names, OWN, spin, 1.0, lambda: clock[0]) == {}
    assert 9 <= len(spins) <= 11
    graph = FakeGraph()

    def spin_then_appear(timeout):
        clock[0] += timeout
        graph.services["/p/in_0"] = [(REAL_PLC, ("g",))]

    clock[0] = 0.0
    assert wait_for_occupied(graph, names, OWN, spin_then_appear, 1.0, lambda: clock[0]) == {
        "/p/in_0": [REAL_PLC]
    }
    assert clock[0] == pytest.approx(0.1)


def test_preflight_waits_whole_window_for_unattributed_owner():
    names = endpoint_names("p", SimPlcConfig(inputs=1, outputs=0))
    clock = [0.0]

    def spin(timeout):
        clock[0] += timeout

    graph = FakeGraph(services={"/p/in_0": [UNKNOWN_OWNER]})
    assert wait_for_occupied(graph, names, OWN, spin, 1.0, lambda: clock[0]) == {}
    assert clock[0] >= 1.0


# --------------------------------------------------------------------------- launch


def _launch_module():
    spec = importlib.util.spec_from_file_location("sim_plc_launch", ROOT / "launch/sim_plc.launch.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_launch_node_contract():
    kwargs = _launch_module().node_kwargs("plc_io", "/cfg")
    assert kwargs["executable"] == "sim_plc"
    assert kwargs["name"] == "sim_plc_plc_io"
    assert kwargs["namespace"] == "/manyforge"
    assert kwargs["arguments"] == ["--device", "plc_io", "--config-dir", "/cfg"]
    assert kwargs["ros_arguments"] == ["--disable-external-lib-logs"]
    assert kwargs["additional_env"] == {"ROS_AUTOMATIC_DISCOVERY_RANGE": "LOCALHOST"}


def test_launch_rejects_bad_device():
    with pytest.raises(ValueError):
        _launch_module().node_kwargs("plc/io", "/cfg")


def test_setup_registers_console_script():
    assert "sim_plc = manyforge_ros_bringup.sim_plc:main" in (ROOT / "setup.py").read_text()


# --------------------------------------------------------------------------- real rclpy

DOMAIN = str(random.randint(120, 220))


def _env():
    env = dict(os.environ)
    env.update(
        ROS_DOMAIN_ID=DOMAIN,
        ROS_AUTOMATIC_DISCOVERY_RANGE="LOCALHOST",
        PYTHONPATH=f"{ROOT}{os.pathsep}{env.get('PYTHONPATH', '')}",
        PYTHONUNBUFFERED="1",
        PYTHONFAULTHANDLER="1",
    )
    return env


@pytest.fixture(scope="module")
def ros():
    rclpy = pytest.importorskip("rclpy")
    os.environ["ROS_DOMAIN_ID"] = DOMAIN
    os.environ["ROS_AUTOMATIC_DISCOVERY_RANGE"] = "LOCALHOST"
    rclpy.init()
    node = rclpy.create_node(f"sim_plc_test_{os.getpid()}")
    yield rclpy, node
    node.destroy_node()
    rclpy.shutdown()


def _start(tmp_path, device, config_text=None, window_s=None):
    if config_text is not None:
        (tmp_path / f"{device}.yaml").write_text(config_text)
    extra = [] if window_s is None else ["--ros-args", "-p", f"successor_window_s:={window_s}"]
    return subprocess.Popen(
        [sys.executable, "-m", "manyforge_ros_bringup.sim_plc",
         "--device", device, "--config-dir", str(tmp_path), *extra],
        env=_env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )


def _stop(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
    try:
        return proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        raise


REFUSAL_WINDOW_S = 6.0


def _exit_within(proc, window, rclpy=None, node=None):
    until = time.monotonic() + window
    while proc.poll() is None and time.monotonic() < until:
        if node is not None:
            rclpy.spin_once(node, timeout_sec=0.05)
        else:
            time.sleep(0.05)
    code = proc.poll()
    _stop(proc)
    return code, proc.stdout.read()


def _exit_within_or_alive(proc, window, rclpy, node):
    """Spin for ``window`` seconds; (exit code or None if still alive, None)."""
    until = time.monotonic() + window
    while proc.poll() is None and time.monotonic() < until:
        rclpy.spin_once(node, timeout_sec=0.05)
    return proc.poll(), None


def _stop_ok(proc):
    code = _stop(proc)
    assert code == 0, proc.stdout.read()


def _wait(rclpy, node, predicate, timeout=8.0):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if predicate():
            return True
        rclpy.spin_once(node, timeout_sec=0.05)
    return predicate()


def _call(rclpy, node, client, value, timeout=5.0):
    from std_srvs.srv import SetBool

    started = time.monotonic()
    future = client.call_async(SetBool.Request(data=value))
    rclpy.spin_until_future_complete(node, future, timeout_sec=timeout)
    return future.result() if future.done() else None, time.monotonic() - started


def _set_param(rclpy, node, device, name, value):
    from rcl_interfaces.msg import Parameter, ParameterType, ParameterValue
    from rcl_interfaces.srv import SetParameters

    client = node.create_client(SetParameters, f"/manyforge/sim_plc_{device}/set_parameters")
    assert client.wait_for_service(timeout_sec=5)
    if isinstance(value, bool):
        pv = ParameterValue(type=ParameterType.PARAMETER_BOOL, bool_value=value)
    elif isinstance(value, float):
        pv = ParameterValue(type=ParameterType.PARAMETER_DOUBLE, double_value=value)
    else:
        pv = ParameterValue(type=ParameterType.PARAMETER_STRING, string_value=value)
    future = client.call_async(SetParameters.Request(parameters=[Parameter(name=name, value=pv)]))
    rclpy.spin_until_future_complete(node, future, timeout_sec=5)
    node.destroy_client(client)
    return future.result().results[0].successful


class Latest:
    def __init__(self, node, topic):
        from std_msgs.msg import Bool

        self.values, self.stamps = [], []
        self.sub = node.create_subscription(Bool, topic, self._on, 10)

    def _on(self, msg):
        self.values.append(msg.data)
        self.stamps.append(time.monotonic())


def test_ros_ack_delay_mirror_and_ready(ros, tmp_path):
    rclpy, node = ros
    from std_srvs.srv import SetBool

    device = "simplc_a"
    proc = _start(
        tmp_path, device, "inputs: 2\noutputs: 2\nack_delay_s: 0.4\nmirror_delay_s: 0.6\n"
    )
    try:
        ready = Latest(node, f"/{device}/ready")
        out0 = Latest(node, f"/{device}/out_0")
        client = node.create_client(SetBool, f"/{device}/in_0")
        assert client.wait_for_service(timeout_sec=8), proc
        assert _wait(rclpy, node, lambda: len(out0.values) >= 3 and ready.values)
        assert out0.values[-1] is False and ready.values[-1] is True
        # Publishing at about 10 Hz.
        start = len(out0.values)
        _wait(rclpy, node, lambda: False, timeout=1.0)
        assert 6 <= len(out0.values) - start <= 14

        sent = time.monotonic()
        during = len(out0.values)
        response, elapsed = _call(rclpy, node, client, True)
        assert response is not None and response.success and response.message
        assert 0.35 <= elapsed < 1.0
        # A pending acknowledgement never stalls publishing.
        assert len(out0.values) - during >= 2
        # Mirror: still False just before mirror_delay_s, True afterwards.
        mark = len(out0.values)
        assert _wait(rclpy, node, lambda: True in out0.values[mark:], timeout=3)
        first_true = out0.stamps[mark + out0.values[mark:].index(True)]
        assert first_true - sent >= 0.55
        assert ready.values[-1] is True
        ready_count = len(ready.values)
        assert _wait(rclpy, node, lambda: len(ready.values) > ready_count + 3)
        assert set(ready.values[ready_count:]) == {True}
    finally:
        _stop_ok(proc)


def test_ros_failure_injection_parameters(ros, tmp_path):
    rclpy, node = ros
    from std_srvs.srv import SetBool

    device = "simplc_b"
    proc = _start(tmp_path, device, "inputs: 1\noutputs: 1\nack_delay_s: 0.0\n")
    try:
        client = node.create_client(SetBool, f"/{device}/in_0")
        assert client.wait_for_service(timeout_sec=8)

        assert _set_param(rclpy, node, device, "inputs.in_0.response", "refuse")
        response, _ = _call(rclpy, node, client, True)
        assert response is not None and response.success is False

        assert _set_param(rclpy, node, device, "inputs.in_0.response", "no_response")
        response, _ = _call(rclpy, node, client, True, timeout=1.5)
        assert response is None

        assert not _set_param(rclpy, node, device, "inputs.in_0.response", "bogus")
        assert not _set_param(rclpy, node, device, "inputs.in_0.ack_delay_s", -1.0)

        assert _set_param(rclpy, node, device, "inputs.in_0.response", "absent")
        assert _wait(rclpy, node, lambda: node.count_services(f"/{device}/in_0") == 0)
        assert _set_param(rclpy, node, device, "inputs.in_0.response", "success")
        assert client.wait_for_service(timeout_sec=5)

        assert _set_param(rclpy, node, device, "inputs.in_0.ack_delay_s", 0.8)
        response, elapsed = _call(rclpy, node, client, True)
        assert response is not None and response.success and elapsed >= 0.75

        out0 = Latest(node, f"/{device}/out_0")
        ready = Latest(node, f"/{device}/ready")
        assert _wait(rclpy, node, lambda: len(out0.values) >= 2)
        assert _set_param(rclpy, node, device, "outputs.out_0.publishing", False)
        assert _set_param(rclpy, node, device, "ready", False)
        time.sleep(0.3)
        _wait(rclpy, node, lambda: False, timeout=0.3)
        mark = len(out0.values)
        assert _wait(rclpy, node, lambda: len(ready.values) > 5 and ready.values[-1] is False)
        _wait(rclpy, node, lambda: False, timeout=0.6)
        assert len(out0.values) == mark
    finally:
        _stop_ok(proc)


def test_ros_occupied_server_refuses_with_3(ros, tmp_path):
    rclpy, node = ros
    from std_srvs.srv import SetBool

    device = "simplc_c"
    server = node.create_service(SetBool, f"/{device}/in_3", lambda req, resp: resp)
    try:
        _wait(rclpy, node, lambda: False, timeout=0.3)
        proc = _start(tmp_path, device)
        # Refused by the 1 s preflight, or, after late discovery, by the
        # running duplicate check; either way within a bounded window.
        code, output = _exit_within(proc, REFUSAL_WINDOW_S)
        assert code == EXIT_OCCUPIED, output
        assert f"/{device}/in_3" in output
    finally:
        node.destroy_service(server)


@pytest.mark.parametrize("kind", ["server", "publisher"])
def test_ros_peer_appearing_after_start_exits_3(ros, tmp_path, kind):
    rclpy, node = ros
    from std_msgs.msg import Bool
    from std_srvs.srv import SetBool

    device = f"simplc_late_{kind}"
    sub = Latest(node, f"/{device}/ready")
    proc = _start(tmp_path, device)
    peer = None
    try:
        assert _wait(rclpy, node, lambda: sub.values), "node never came up"
        if kind == "server":
            peer = node.create_service(SetBool, f"/{device}/in_2", lambda req, resp: resp)
            name = f"/{device}/in_2"
        else:
            peer = node.create_publisher(Bool, f"/{device}/out_1", 10)
            name = f"/{device}/out_1"
        code, output = _exit_within(proc, REFUSAL_WINDOW_S, rclpy, node)
        assert code == EXIT_OCCUPIED, output
        assert name in output
    finally:
        if peer is not None:
            (node.destroy_service if kind == "server" else node.destroy_publisher)(peer)


def test_ros_client_only_proceeds_and_sigint_exits_0(ros, tmp_path):
    rclpy, node = ros
    from std_srvs.srv import SetBool

    device = "simplc_d"
    client = node.create_client(SetBool, f"/{device}/in_0")
    sub = Latest(node, f"/{device}/ready")
    proc = _start(tmp_path, device)
    try:
        assert client.wait_for_service(timeout_sec=8)
        assert _wait(rclpy, node, lambda: sub.values)
        assert proc.poll() is None
    finally:
        _stop_ok(proc)
    _wait(rclpy, node, lambda: node.count_services(f"/{device}/in_0") == 0)
    assert node.count_services(f"/{device}/in_0") == 0


def test_ros_config_error_exits_2(tmp_path):
    proc = _start(tmp_path, "simplc_e", "inputs: 99\n")
    assert proc.wait(timeout=10) == EXIT_CONFIG
    assert "inputs" in proc.stdout.read()


def test_ros_sigterm_exits_0(ros, tmp_path):
    rclpy, node = ros
    device = "simplc_f"
    sub = Latest(node, f"/{device}/ready")
    proc = _start(tmp_path, device)
    try:
        assert _wait(rclpy, node, lambda: sub.values)
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        code = proc.wait(timeout=10)
    assert code == 0, proc.stdout.read()


def _same_named(rclpy, device):
    """A live in-process node with the sim_plc's own FQN serving in_0 / publishing ready."""
    from std_msgs.msg import Bool
    from std_srvs.srv import SetBool

    twin = rclpy.create_node(f"sim_plc_{device}", namespace="/manyforge")
    twin.create_service(SetBool, f"/{device}/in_0", lambda req, resp: resp)
    twin.create_publisher(Bool, f"/{device}/ready", 10)
    return twin


def test_ros_live_same_fqn_duplicate_is_refused(ros, tmp_path):
    """Same FQN alone is not identity: a LIVE same-named node outlives the window."""
    rclpy, node = ros
    device = "simplc_twin"
    twin = _same_named(rclpy, device)
    try:
        proc = _start(tmp_path, device, "inputs: 1\noutputs: 1\n", window_s=2.0)
        started = time.monotonic()
        code, output = _exit_within(proc, 1.0 + 2.0 + 3.0, rclpy, node)
        assert code == EXIT_OCCUPIED, output
        assert time.monotonic() - started >= 2.0  # not before the successor window
        # rmw may list the twin's in_0 server twice (reader/writer mis-pairing).
        assert f"/{device}/in_0 (by /manyforge/sim_plc_{device}" in output
    finally:
        twin.destroy_node()


def test_ros_stale_predecessor_vanishing_within_window_serves(ros, tmp_path):
    """Same-named endpoints that disappear inside the window (lease expiry)."""
    rclpy, node = ros
    from std_srvs.srv import SetBool

    device = "simplc_succ"
    twin = _same_named(rclpy, device)
    proc = _start(tmp_path, device, "inputs: 1\noutputs: 1\nack_delay_s: 0.0\n", window_s=4.0)
    try:
        _exit_within_or_alive(proc, 2.0, rclpy, node)
        twin.destroy_node()  # the predecessor's endpoints expire
        twin = None
        code, _ = _exit_within_or_alive(proc, 4.0, rclpy, node)  # past the window
        assert code is None, proc.stdout.read()
        client = node.create_client(SetBool, f"/{device}/in_0")
        response, _ = _call(rclpy, node, client, True)
        assert response is not None and response.success
    finally:
        if twin is not None:
            twin.destroy_node()
        _stop_ok(proc)


def test_ros_two_live_default_instances_one_exits_3(ros, tmp_path):
    rclpy, node = ros
    device = "simplc_two"
    first = _start(tmp_path, device, window_s=2.0)
    sub = Latest(node, f"/{device}/ready")
    try:
        assert _wait(rclpy, node, lambda: sub.values)
        second = _start(tmp_path, device, window_s=2.0)
        try:
            until = time.monotonic() + 1.0 + 2.0 + 3.0
            while time.monotonic() < until and first.poll() is None and second.poll() is None:
                rclpy.spin_once(node, timeout_sec=0.05)
            codes = {first.poll(), second.poll()}
            assert EXIT_OCCUPIED in codes, codes
        finally:
            _stop(second)
    finally:
        _stop(first)


def test_ros_sigkill_then_immediate_replacement_serves(ros, tmp_path):
    """Real reproduction: the killed instance's endpoints linger; the new one serves."""
    rclpy, node = ros
    from std_srvs.srv import SetBool

    device = "simplc_kill"
    config = "inputs: 1\noutputs: 1\nack_delay_s: 0.0\n"
    client = node.create_client(SetBool, f"/{device}/in_0")
    first = _start(tmp_path, device, config)
    try:
        assert client.wait_for_service(timeout_sec=8)
    finally:
        first.kill()
        first.wait(timeout=5)
    lingering = node.count_services(f"/{device}/in_0")
    replacement = _start(tmp_path, device, config, window_s=3.0)
    try:
        code, _ = _exit_within_or_alive(replacement, 1.0 + 3.0 + 1.5, rclpy, node)
        assert code is None, replacement.stdout.read()
        response, _ = _call(rclpy, node, client, True)
        assert response is not None and response.success
    finally:
        _stop_ok(replacement)
    # Evidence that the scenario was real: the killed server was still on the graph.
    print(f"lingering servers right after SIGKILL: {lingering}")


def test_ros_same_fqn_peer_discovered_during_creation_is_not_own():
    """Regression (sim-plc-03 review): a live same-FQN peer discovered while our
    endpoints are created must never be taken as ours."""
    import rclpy
    from rclpy.context import Context
    from std_msgs.msg import Bool

    from manyforge_ros_bringup.sim_plc import _make_node

    os.environ["ROS_DOMAIN_ID"] = DOMAIN
    ours, theirs = Context(), Context()  # two participants, like two processes
    rclpy.init(context=ours)
    rclpy.init(context=theirs)
    device = "simplc_race"
    node = _make_node(device, SimPlcConfig(inputs=1, outputs=1), context=ours)
    twin = rclpy.create_node(f"sim_plc_{device}", namespace="/manyforge", context=theirs)
    try:
        original = node.create_publisher

        def racing_create_publisher(msg_type, topic, qos, **kwargs):
            publisher = original(msg_type, topic, qos, **kwargs)
            if topic in (f"/{device}/ready", f"/{device}/out_0"):
                # The twin's publisher appears on our graph mid-creation.
                twin.create_publisher(Bool, topic, 10)
                until = time.monotonic() + 5
                def attributed_twice():
                    infos = node.get_publishers_info_by_topic(topic)
                    return len(infos) == 2 and all(
                        i.node_name == f"sim_plc_{device}" for i in infos
                    )
                while not attributed_twice() and time.monotonic() < until:
                    time.sleep(0.01)
                assert attributed_twice()
            return publisher

        node.create_publisher = racing_create_publisher
        node.start()
        found = foreign_owners(RclpyGraph(node), node.endpoints, node._own_prefix)
        twin_name = f"/manyforge/sim_plc_{device}"
        assert found.get(f"/{device}/ready") == [twin_name]
        assert found.get(f"/{device}/out_0") == [twin_name]
        watch = DuplicateWatch(node.get_fully_qualified_name(), started_at=0.0, window_s=1.0)
        assert set(watch.update(found, now=1.0)) >= {f"/{device}/ready", f"/{device}/out_0"}
    finally:
        twin.destroy_node()
        node.destroy_node()
        rclpy.shutdown(context=theirs)
        rclpy.shutdown(context=ours)
