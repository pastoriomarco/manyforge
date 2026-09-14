"""One real managed-owner shutdown race; private qualification state only."""

from __future__ import annotations

import json
from pathlib import Path
import socket
import socketserver
import sqlite3
import struct
import threading
import time

from manyforge_behavior.canonical_artifact import canonical_json_bytes
from manyforge_headless_api.execution import (
    EXECUTION_COMMAND_VERSION,
    ExecutionCommandClient,
)


def check_pending_scene_read_shutdown(
    stack, state_root: Path, workbench, *, apply_change=None
):
    """Withhold one native Scene reply, without replacing any owner response."""
    manager = next(process for name, process in stack.processes if name == "Manager")
    assert manager.poll() is None
    children = {
        int(value)
        for value in Path(f"/proc/{manager.pid}/task/{manager.pid}/children")
        .read_text()
        .split()
    }
    assert children, "Manager has no native child to qualify"
    native = ExecutionCommandClient(state_root / "execution-run")
    owner_epoch = native.identity()["identity"]["owner_epoch"]
    socket_path = state_root / "manager-run/manager-command.sock"
    forwarded_path = socket_path.with_name("qualification-manager.sock")
    assert socket_path.is_socket() and not forwarded_path.exists()
    socket_mode = socket_path.stat().st_mode & 0o777
    captured = threading.Event()
    cancelled = threading.Event()
    armed = threading.Event()
    if apply_change is None:
        armed.set()
    capture_lock = threading.Lock()
    outcome = {}

    class Handler(socketserver.BaseRequestHandler):
        def handle(self):
            self.request.settimeout(8)
            packet = self.request.recv(16 * 1024 * 1024)
            if not packet:
                return
            pid, _, _ = struct.unpack(
                "3i", self.request.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
            )
            with capture_lock:
                hold = (
                    pid in children
                    and armed.is_set()
                    and json.loads(packet).get("command") == "scene_owner_read"
                    and not captured.is_set()
                )
                if hold:
                    outcome["child_pid"] = pid
                    outcome["captured_at"] = time.monotonic()
                    captured.set()
            if hold:
                try:
                    outcome["peer_closed"] = self.request.recv(1) == b""
                    outcome["cancelled_at"] = time.monotonic()
                except OSError as error:
                    outcome["error"] = str(error)
                finally:
                    cancelled.set()
                return
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as upstream:
                    upstream.settimeout(5)
                    upstream.connect(str(forwarded_path))
                    upstream.sendall(packet)
                    reply = upstream.recv(16 * 1024 * 1024)
                    if reply:
                        self.request.sendall(reply)
            except OSError:
                # Ordinary peers can disappear during the requested shutdown.
                # The held read and durable close are asserted separately.
                return

    class Server(socketserver.ThreadingUnixStreamServer):
        socket_type = socket.SOCK_SEQPACKET
        daemon_threads = True

    socket_path.rename(forwarded_path)
    server = None
    worker = None
    try:
        server = Server(str(socket_path), Handler)
        socket_path.chmod(socket_mode)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        if apply_change is not None:
            # Use the actual durable Apply fence as the trigger. Holding a read
            # before compilation would only test its ordinary RPC timeout.
            journal_path = state_root / "manager/manager-apply-journal.sqlite3"

            def fences():
                with sqlite3.connect(
                    f"file:{journal_path}?mode=ro", uri=True
                ) as journal:
                    return set(
                        journal.execute(
                            "SELECT apply_id,apply_generation "
                            "FROM manager_apply_fence_acceptances"
                        ).fetchall()
                    )

            previous_fences = fences()

            def new_fence():
                added = fences() - previous_fences
                assert len(added) <= 1, added
                return next(iter(added), None)

            apply_change()
            outcome["apply_id"], outcome["apply_generation"] = workbench._wait_until(
                "new durable Apply fence",
                new_fence,
                stack.processes,
            )
            armed.set()
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
            client.settimeout(3)
            client.connect(str(state_root / "execution-run/execution-command.sock"))
            client.sendall(
                canonical_json_bytes(
                    {"api_version": EXECUTION_COMMAND_VERSION, "command": "identity"}
                )
            )
            if not captured.wait(3):
                raise AssertionError(
                    (
                        "native identity did not reach the held Scene read",
                        client.recv(65536),
                    )
                )
            started = time.monotonic()
            if apply_change is None:
                stack._shutdown_manager()
                manager.wait(timeout=3)
            assert cancelled.wait(2), "native Scene read was not cancelled"
        assert outcome.get("peer_closed") is True, outcome
        elapsed = outcome["cancelled_at"] - (
            started if apply_change is None else outcome["captured_at"]
        )
        assert 0 <= elapsed < 2, (elapsed, outcome)
        assert manager.poll() == (
            0 if apply_change is None else None
        ), manager.returncode

        def owner_closed():
            with sqlite3.connect(
                f"file:{state_root / 'execution-state/operation-journal.sqlite3'}?mode=ro",
                uri=True,
            ) as journal:
                closed = journal.execute(
                    "SELECT active,closed_at_ns,close_sequence FROM owner_epochs "
                    "WHERE owner_epoch=?",
                    (owner_epoch,),
                ).fetchone()
            return (
                closed
                if (
                    closed and closed[0] == 0 and all(x is not None for x in closed[1:])
                )
                else None
            )

        if apply_change is None:
            assert owner_closed(), "shutdown did not close the owner epoch"
        else:
            workbench._wait_until(
                "Apply predecessor closure", owner_closed, stack.processes
            )
        result = {
            "passed": True,
            "pending_scene_read_cancelled_s": elapsed,
            "owner_epoch_closed": owner_epoch,
            "motion_issued": False,
            "apply_generation": outcome.get("apply_generation"),
            "apply_id": outcome.get("apply_id"),
        }
    finally:
        if server is not None:
            server.shutdown()
            server.server_close()
            socket_path.unlink(missing_ok=True)
        if worker is not None:
            worker.join(timeout=2)
        forwarded_path.rename(socket_path)

    # Reuse the actual Manager command and durable state, not a new bootstrap
    # package or synthetic successor. Keep cleanup bound to the new process.
    if apply_change is None:
        stack.processes.remove(("Manager", manager))
        runner = next(p for name, p in stack.processes if name == "Program runner")
        stack._terminate(runner)
        stack.processes.remove(("Program runner", runner))
        stack._spawn("Program runner", runner.args)

    def runner_ready():
        with workbench.HeadlessHttpClient(
            f"http://127.0.0.1:{stack.runner_port}", timeout_seconds=0.5
        ) as client:
            return client.request("GET", "/api/program/health").get("started")

    if apply_change is None:
        workbench._wait_until(
            "replacement Runner service", runner_ready, stack.processes
        )
        stack._spawn("Manager", manager.args)

    def successor_ready():
        identity = native.identity()["identity"]
        if identity["owner_epoch"] == owner_epoch:
            return None
        with workbench.HeadlessHttpClient(
            f"http://127.0.0.1:{stack.headless_port}", timeout_seconds=0.5
        ) as client:
            status = client.manager_status()
            diff = client.manager_diff()
            admission = client.authoring_admission()
        if not workbench._manager_projection_ready(status, diff):
            return None
        if admission.get("authoring_allowed") is not True:
            return None
        return identity

    successor = workbench._wait_until(
        "replacement owner and restored admission",
        successor_ready,
        stack.processes,
    )
    result["successor_owner_epoch"] = successor["owner_epoch"]
    result["successor_authoring_allowed"] = True
    result["program_runner_replaced"] = apply_change is None
    return result
