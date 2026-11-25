"""The core's blocking Gateway calls release the GIL while they wait.

A fake owner in a child process takes the request and withholds its reply
until a second Python thread of this process, woken by the owner, runs and
releases it. That thread can only run while the call waits without the GIL,
so the owner's reply arriving proves it. If the GIL were held, the owner
gives up after a fail-safe limit and answers with a refusal the test
reports, so a broken build fails instead of hanging.
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

import pytest

import manyforge_headless_py as core

_OWNER = r"""
import os, select, socket, sys
listener = socket.socket(fileno=int(sys.argv[1]))
release_fd, received_fd = int(sys.argv[2]), int(sys.argv[3])
reply, fallback = sys.argv[4].encode(), sys.argv[5].encode()
connection, _ = listener.accept()
connection.recv(1 << 20)
os.write(received_fd, b"r")
readable, _, _ = select.select([release_fd], [], [], float(sys.argv[6]))
connection.send(reply if readable else fallback)
connection.close()
"""

# Only reached when the waiting call holds the GIL (the test then fails).
_FAIL_SAFE_S = 30.0


def _canonical(value: dict[str, Any]) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


@pytest.fixture
def run_root() -> Iterator[Path]:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(os.path.realpath(directory))
        root.chmod(0o700)
        yield root


def _withheld_call(
    root: Path,
    leaf: str,
    reply: dict[str, Any],
    fallback: dict[str, Any],
    call: Callable[[core.Gateway], dict[str, Any]],
) -> dict[str, Any]:
    """Runs `call` against an owner that answers only after another thread
    of this process ran; returns the reply."""
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(root / leaf))
    (root / leaf).chmod(0o600)
    listener.listen(1)
    release_read, release_write = os.pipe()
    received_read, received_write = os.pipe()
    owner = subprocess.Popen(
        [
            sys.executable, "-I", "-c", _OWNER,
            str(listener.fileno()), str(release_read), str(received_write),
            _canonical(reply), _canonical(fallback), str(_FAIL_SAFE_S),
        ],
        pass_fds=(listener.fileno(), release_read, received_write),
    )
    listener.close()
    os.close(release_read)
    os.close(received_write)

    def other_thread() -> None:
        os.read(received_read, 1)  # the owner holds the request
        try:
            os.write(release_write, b"g")
        except OSError:
            pass  # the owner already gave up

    helper = threading.Thread(target=other_thread)
    helper.start()
    try:
        return call(core.Gateway(str(root), str(root)))
    except BaseException:
        owner.kill()  # a call that never reached the owner
        raise
    finally:
        owner.wait()
        helper.join()
        os.close(received_read)
        os.close(release_write)


_HELD = {"code": "gil_held", "details": {}, "message": "the GIL was held", "producer": "test"}


def test_host_call_releases_the_gil(run_root: Path) -> None:
    reply = {
        "api_version": "manyforge.execution_command_response.v6",
        "command": "identity",
        "ok": True,
    }
    fallback = {
        "api_version": "manyforge.execution_command_response.v6",
        "error": _HELD,
        "ok": False,
    }
    result = _withheld_call(
        run_root, "execution-command.sock", reply, fallback, lambda g: g.host("identity"))
    assert result == reply


def test_manager_call_releases_the_gil(run_root: Path) -> None:
    reply = {"api_version": "manyforge.manager_command_response.v3", "ok": True}
    fallback = {"api_version": "manyforge.manager_command_response.v3", "error": _HELD, "ok": False}
    result = _withheld_call(
        run_root, "manager-command.sock", reply, fallback,
        lambda g: g.manager("apply_status", {}))
    assert result == reply


def test_scene_commit_releases_the_gil(run_root: Path) -> None:
    reply = {"api_version": "manyforge.manager_command_response.v3", "ok": True}
    fallback = {"api_version": "manyforge.manager_command_response.v3", "error": _HELD, "ok": False}
    result = _withheld_call(
        run_root, "manager-command.sock", reply, fallback,
        lambda g: g.scene_commit({"operation": {"kind": "attach_object"}}))
    assert result == reply
