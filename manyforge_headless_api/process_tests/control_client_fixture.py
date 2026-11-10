"""One bounded read-only v2 control client used by the headless process tests.

This is test transport, not an execution owner. It answers server heartbeats
and credits consumed state while the caller performs other in-scope checks.
"""

from contextlib import contextmanager
import json
from queue import Empty, Queue
from threading import Event, Thread

from websockets.sync.client import connect


@contextmanager
def composer_snapshot_stream(composer_url: str, *, client_id: str):
    protocol = "manyforge.execution_control.v2"
    url = composer_url.replace("http://", "ws://", 1).replace("https://", "wss://", 1)
    with connect(
        url.rstrip("/") + "/ws/control",
        open_timeout=2,
        close_timeout=1,
        max_size=16 * 1024 * 1024,
        proxy=None,
    ) as websocket:
        websocket.send(
            json.dumps(
                {
                    "schema_version": protocol,
                    "message_kind": "client_hello",
                    "supported_versions": [protocol],
                    "client_incarnation": client_id,
                    "source_label": "native_qualification",
                }
            )
        )
        hello = json.loads(websocket.recv(timeout=2))
        if hello.get("schema_version") != protocol or hello.get("message_kind") != "server_hello":
            raise RuntimeError("Composer did not accept the v2 session")
        session = hello["control_session_id"]
        stopped = Event()
        pending = Queue(maxsize=1)

        def publish(value):
            try:
                pending.get_nowait()
            except Empty:
                pass
            pending.put_nowait(value)

        def receive():
            shell = None
            client_sequence = 0
            try:
                while not stopped.is_set():
                    try:
                        message = json.loads(websocket.recv(timeout=0.25))
                    except TimeoutError:
                        continue
                    if (
                        message.get("schema_version") != protocol
                        or message.get("control_session_id") != session
                    ):
                        raise RuntimeError("control message crossed protocol/session")
                    kind = message["message_kind"]
                    if kind == "heartbeat_challenge":
                        client_sequence += 1
                        websocket.send(
                            json.dumps(
                                {
                                    "schema_version": protocol,
                                    "message_kind": "heartbeat_echo",
                                    "control_session_id": session,
                                    "client_sequence": str(client_sequence),
                                    "challenge_sequence": message["challenge_sequence"],
                                }
                            )
                        )
                    elif kind in {"snapshot", "delta"}:
                        update = message.get("shell")
                        if not isinstance(update, dict):
                            raise RuntimeError("state has no shell")
                        if kind == "snapshot":
                            if update.get("complete") is not True:
                                raise RuntimeError("snapshot shell is incomplete")
                            shell = dict(update)
                        else:
                            if shell is None or update.get("complete") is not False:
                                raise RuntimeError("delta has no initial snapshot")
                            shell.update(
                                {key: value for key, value in update.items() if key != "complete"}
                            )
                        if "state_sequence" in message:
                            websocket.send(
                                json.dumps(
                                    {
                                        "schema_version": protocol,
                                        "message_kind": "state_applied",
                                        "control_session_id": session,
                                        "state_sequence": message["state_sequence"],
                                    }
                                )
                            )
                        publish(dict(shell))
                    elif kind == "resync_required":
                        raise RuntimeError("qualification owner/session was replaced")
            except Exception as error:
                if not stopped.is_set():
                    publish(error)

        receiver = Thread(target=receive, name="qualification-control-reader", daemon=True)
        receiver.start()

        def next_snapshot():
            try:
                value = pending.get(timeout=8)
            except Empty as error:
                raise RuntimeError("Composer did not publish current state") from error
            if isinstance(value, Exception):
                raise RuntimeError("Composer control stream failed") from value
            return value

        try:
            yield next_snapshot
        finally:
            stopped.set()
            websocket.close()
            receiver.join(timeout=2)
            if receiver.is_alive():
                raise RuntimeError("qualification control reader did not stop")
