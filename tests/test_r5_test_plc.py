from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest


_PATH = Path(__file__).resolve().parents[1] / "scripts/qualification/r5_test_plc.py"
_SPEC = importlib.util.spec_from_file_location("r5_test_plc", _PATH)
assert _SPEC and _SPEC.loader
plc = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(plc)


@pytest.mark.parametrize("delay", ["nan", "inf", "-0.1"])
def test_rejects_invalid_response_delay(delay):
    with pytest.raises(argparse.ArgumentTypeError):
        plc.response_delay(delay)


def test_publisher_is_independent_of_delayed_service():
    publisher, service = plc.peer_commands(2.0)
    assert publisher == ["ros2", "topic", "pub", "-r", "10", "/plc_io/ready", "std_msgs/msg/Bool", "{data: true}"]
    assert service[-2:] == ["_serve", "2.0"]
    assert plc.response_delay("0") == 0


class Child:
    pid = 100
    returncode = None

    def __init__(self, exited=False, slow=False):
        self.exited, self.slow = exited, slow
        self.actions = []

    def poll(self):
        return 9 if self.exited else None

    def terminate(self):
        self.actions.append("terminate")

    def kill(self):
        self.actions.append("kill")

    def wait(self, timeout=None):
        self.actions.append(("wait", timeout))
        if self.slow and timeout is not None:
            raise subprocess.TimeoutExpired("owned child", timeout)


@pytest.mark.parametrize("interrupt", [False, True])
def test_both_owned_children_are_cleaned_up_on_failure_or_interrupt(monkeypatch, tmp_path, interrupt):
    children = [Child(), Child(exited=not interrupt)]
    spawned = []

    def spawn(command, **kwargs):
        assert kwargs["start_new_session"] is True
        spawned.append(command)
        return children[len(spawned) - 1]

    def sleep(_delay):
        raise KeyboardInterrupt

    monkeypatch.setattr(plc.subprocess, "Popen", spawn)
    monkeypatch.setattr(plc.time, "sleep", sleep)
    if interrupt:
        assert plc.run_peers(0.1, tmp_path / "logs") == 130
    else:
        with pytest.raises(RuntimeError, match="exited"):
            plc.run_peers(0.1, tmp_path / "logs")
    assert len(spawned) == 2
    assert children[0].actions == ["terminate", ("wait", 5)]
    assert children[1].actions == ([("wait", 5)] if not interrupt else ["terminate", ("wait", 5)])


def test_partial_spawn_failure_cleans_up_first_child(monkeypatch, tmp_path):
    child = Child(slow=True)
    calls = []

    def spawn(*args, **kwargs):
        calls.append(args)
        if len(calls) == 2:
            raise OSError("service spawn failed")
        return child

    monkeypatch.setattr(plc.subprocess, "Popen", spawn)
    with pytest.raises(OSError, match="service spawn failed"):
        plc.run_peers(0.1, tmp_path / "logs")
    assert child.actions == ["terminate", ("wait", 5), "kill", ("wait", None)]


def _preflight_ros(monkeypatch, actions, *, publishers, servers, clients):
    node = SimpleNamespace(
        # The graph lists the service name whenever any client or server exists.
        get_service_names_and_types=lambda: (
            [("/plc_io/start", ["std_srvs/srv/SetBool"])] if servers or clients else []
        ),
        count_services=lambda name: servers if name == "/plc_io/start" else 0,
        count_clients=lambda name: clients if name == "/plc_io/start" else 0,
        count_publishers=lambda topic: publishers if topic == "/plc_io/ready" else 0,
        destroy_node=lambda: actions.append("destroy"),
    )
    ros = SimpleNamespace(
        init=lambda: actions.append("init"),
        create_node=lambda name: node,
        spin_once=lambda *args, **kwargs: None,
        shutdown=lambda: actions.append("shutdown"),
    )
    monkeypatch.setitem(sys.modules, "rclpy", ros)


@pytest.mark.parametrize("publishers,servers", [(1, 0), (0, 1)])
def test_duplicate_endpoint_refusal_releases_only_preflight_node(monkeypatch, publishers, servers):
    actions = []
    _preflight_ros(monkeypatch, actions, publishers=publishers, servers=servers, clients=1)
    with pytest.raises(RuntimeError, match="already have"):
        plc.check_endpoints_unused()
    assert actions == ["init", "destroy", "shutdown"]


def test_consumer_client_and_subscription_do_not_occupy_the_endpoints(monkeypatch):
    # A loaded Scenario's plc_io client/subscription is the consumer, not a peer.
    actions = []
    _preflight_ros(monkeypatch, actions, publishers=0, servers=0, clients=1)
    plc.check_endpoints_unused()
    assert actions == ["init", "destroy", "shutdown"]
