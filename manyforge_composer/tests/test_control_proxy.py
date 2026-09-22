from __future__ import annotations

import asyncio
import json
from pathlib import Path
import threading
from typing import Any

from fastapi import WebSocketDisconnect

from manyforge_composer.backend import headless_ws
from manyforge_composer.backend.headless_ws import (
    HeadlessStateSource,
    serve_headless_state,
)
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.control_protocol import ExecutionControlDeltaDTO


class _Client:
    control_websocket_url = "ws://headless.test/ws/control"

    def __getattr__(self, name: str) -> Any:
        raise AssertionError(f"control proxy must not poll {name}")


class _Admission:
    receiver_epoch = "composer.epoch.1"


def _source() -> HeadlessStateSource:
    return HeadlessStateSource(
        _Client(),  # type: ignore[arg-type]
        admission=_Admission(),  # type: ignore[arg-type]
        workspace=ComposerState().authoring_workspace,
    )


def _owner_snapshot() -> str:
    return json.dumps(
        {
            "schema_version": "manyforge.execution_control.v2",
            "message_kind": "snapshot",
            "control_session_id": "control-session.1",
            "server_sequence": "1",
            "received_client_sequence": None,
            "control": {"semantic_owner_epoch": "runner.1"},
            "shell": {"component_overview": {"readiness": "ready"}},
            "complete": True,
        }
    )


def test_source_only_enriches_owner_frames_and_performs_no_http_reads() -> None:
    source = _source()
    first = json.loads(source.enrich(_owner_snapshot()))
    second_value = json.loads(_owner_snapshot())
    second_value["message_kind"] = "delta"
    second_payload = json.dumps(second_value)

    assert first["control"] == {"semantic_owner_epoch": "runner.1"}
    assert first["shell"]["schema_version"] == (
        "manyforge.composer_headless_state.v2"
    )
    assert first["shell"]["receiver_epoch"] == "composer.epoch.1"
    assert first["shell"]["snapshot_sequence"] == 1
    assert first["shell"]["authoring_workspace"]["program_draft_revision"] == 0
    assert first["shell"]["authoring_workspace"]["workspace_revision"] == 1
    assert first["shell"]["complete"] is True
    assert source.enrich(second_payload) == second_payload


def test_source_forwards_receipts_without_reencoding_them() -> None:
    source = _source()
    receipt = '{ "schema_version":"manyforge.execution_control.v2", "message_kind":"command_receipt" }'

    assert source.enrich(receipt) == receipt


def test_actual_local_delta_matches_the_public_dto_without_an_owner_claim() -> None:
    source = _source()
    template = json.loads(_owner_snapshot())
    template["state_sequence"] = "7"
    template["received_client_sequence"] = "11"
    local = json.loads(source.local_delta(json.dumps(template)))
    assert ExecutionControlDeltaDTO.model_validate(local).model_dump(mode="json") == local
    assert "state_sequence" not in local
    assert "received_client_sequence" not in local
    assert local["control"] is None
    assert local["shell"]["complete"] is False
    fixture = json.loads((Path(__file__).parents[2] / "manyforge_headless_api/tests/fixtures/control-v2-local-delta.json").read_text())
    # Only runtime identities/revisions vary. The real producer must have
    # exactly the same wire shape consumed by Python and the browser.
    expected = fixture["local_delta"]
    assert set(local) == set(expected)
    assert set(local["shell"]) == set(expected["shell"])
    for key in ("schema_version", "message_kind", "event_class", "control"):
        assert local[key] == expected[key]


class _Upstream:
    def __init__(self, *, wait_for_browser: bool) -> None:
        self.wait_for_browser = wait_for_browser
        self.browser_message = asyncio.Event()
        self.sent: list[str] = []
        self.yielded = False
        self.never = asyncio.Event()

    async def __aenter__(self) -> "_Upstream":
        return self

    async def __aexit__(self, *_args: Any) -> None:
        return None

    async def send(self, message: str) -> None:
        self.sent.append(message)
        self.browser_message.set()

    def __aiter__(self) -> "_Upstream":
        return self

    async def __anext__(self) -> str:
        if self.yielded:
            await self.never.wait()
            raise StopAsyncIteration
        if self.wait_for_browser:
            await self.browser_message.wait()
        self.yielded = True
        return _owner_snapshot()


class _Browser:
    def __init__(self, command: str, *, block_send: bool = False) -> None:
        self.command = command
        self.block_send = block_send
        self.accepted = False
        self.closed = False
        self.received = False
        self.send_started = asyncio.Event()
        self.release_send = asyncio.Event()
        self.sent: list[dict[str, Any]] = []
        self.never = asyncio.Event()

    async def accept(self) -> None:
        self.accepted = True

    async def receive_text(self) -> str:
        if not self.received:
            if self.block_send:
                await self.send_started.wait()
            self.received = True
            return self.command
        await self.never.wait()
        raise WebSocketDisconnect()

    async def send_text(self, message: str) -> None:
        self.send_started.set()
        if self.block_send:
            await self.release_send.wait()
        self.sent.append(json.loads(message))
        raise WebSocketDisconnect()

    async def close(self, *_args: Any, **_kwargs: Any) -> None:
        self.closed = True


def test_proxy_forwards_both_directions_and_releases_demand(
    monkeypatch,
) -> None:
    async def exercise() -> None:
        upstream = _Upstream(wait_for_browser=True)
        connection_options: dict[str, Any] = {}

        def connect(*_args: Any, **kwargs: Any) -> _Upstream:
            connection_options.update(kwargs)
            return upstream

        monkeypatch.setattr(
            headless_ws,
            "connect_websocket",
            connect,
        )
        source = _source()
        browser = _Browser('{"message_kind":"client_hello"}')
        await serve_headless_state(browser, source)  # type: ignore[arg-type]

        assert browser.accepted and browser.closed
        assert upstream.sent == ['{"message_kind":"client_hello"}']
        assert browser.sent[0]["message_kind"] == "snapshot"
        assert browser.sent[0]["shell"]["receiver_epoch"] == "composer.epoch.1"
        assert connection_options["compression"] is None
        assert not source._local_subscriptions  # noqa: SLF001

    asyncio.run(exercise())


def test_slow_browser_output_cannot_block_command_ingress(monkeypatch) -> None:
    async def exercise() -> None:
        upstream = _Upstream(wait_for_browser=False)
        monkeypatch.setattr(
            headless_ws,
            "connect_websocket",
            lambda *_args, **_kwargs: upstream,
        )
        source = _source()
        browser = _Browser("stop-command", block_send=True)
        task = asyncio.create_task(
            serve_headless_state(browser, source)  # type: ignore[arg-type]
        )
        await asyncio.wait_for(browser.send_started.wait(), 0.2)
        await asyncio.wait_for(upstream.browser_message.wait(), 0.2)
        assert upstream.sent == ["stop-command"]
        browser.release_send.set()
        await asyncio.wait_for(task, 0.5)

    asyncio.run(exercise())


def test_local_draft_wake_emits_one_resequenced_latest_shell(
    monkeypatch,
) -> None:
    class _TwoFrameBrowser(_Browser):
        async def send_text(self, message: str) -> None:
            self.sent.append(json.loads(message))
            if len(self.sent) == 2:
                raise WebSocketDisconnect()

    async def exercise() -> None:
        upstream = _Upstream(wait_for_browser=False)
        monkeypatch.setattr(
            headless_ws,
            "connect_websocket",
            lambda *_args, **_kwargs: upstream,
        )
        source = _source()
        browser = _TwoFrameBrowser('{"message_kind":"client_hello"}')
        task = asyncio.create_task(
            serve_headless_state(browser, source)  # type: ignore[arg-type]
        )
        while not browser.sent:
            await asyncio.sleep(0)
        source.notify_local_state_change()
        await asyncio.wait_for(task, 0.5)

        identity = source._workspace.identity()  # noqa: SLF001
        assert [frame["server_sequence"] for frame in browser.sent] == ["1", "2"]
        assert browser.sent[1]["message_kind"] == "delta"
        assert browser.sent[1]["event_class"] == "cold"
        assert browser.sent[1]["control"] is None
        assert browser.sent[1]["shell"]["complete"] is False
        assert browser.sent[1]["shell"]["snapshot_sequence"] == 2
        assert browser.sent[1]["shell"] == {
            "complete": False,
            "authoring_workspace": {
                "workspace_epoch": identity.workspace_epoch,
                "workspace_revision": identity.workspace_revision,
                "program_draft_id": identity.program_draft_id,
                "program_draft_revision": identity.program_draft_revision,
                "deployment_draft_id": identity.deployment_draft_id,
                "deployment_draft_revision": identity.deployment_draft_revision,
            },
            "receiver_epoch": "composer.epoch.1",
            "schema_version": "manyforge.composer_headless_state.v2",
            "snapshot_sequence": 2,
        }
        assert not source._local_subscriptions  # noqa: SLF001

    asyncio.run(exercise())


def test_two_browsers_coalesce_wakes_and_publish_the_newest_identity(
    monkeypatch,
) -> None:
    """Two clients share one owner: each wake collapses and carries the newest identity."""

    class _StopAfterDeltas(_Browser):
        def __init__(self) -> None:
            super().__init__('{"message_kind":"client_hello"}')
            self.deltas = 0
            self.ready = asyncio.Event()
            self.first_delta = asyncio.Event()

        async def send_text(self, message: str) -> None:
            self.sent.append(json.loads(message))
            self.ready.set()
            if json.loads(message).get("message_kind") == "delta":
                self.deltas += 1
                if self.deltas == 1:
                    self.first_delta.set()
                if self.deltas == 2:
                    raise WebSocketDisconnect()

    async def exercise() -> None:
        def _connect(*_args, **_kwargs) -> _Upstream:
            # Each browser owns its own owner-side session; one shared upstream
            # would deliver the snapshot to only one of them.
            return _Upstream(wait_for_browser=False)

        monkeypatch.setattr(headless_ws, "connect_websocket", _connect)
        source = _source()
        first = _StopAfterDeltas()
        second = _StopAfterDeltas()
        tasks = [
            asyncio.create_task(serve_headless_state(first, source)),  # type: ignore[arg-type]
            asyncio.create_task(serve_headless_state(second, source)),  # type: ignore[arg-type]
        ]

        await asyncio.wait_for(
            asyncio.gather(first.ready.wait(), second.ready.wait()),
            0.5,
        )

        # Three commits raise three wakes before either browser is served. Each
        # subscription holds one pending event, so one delta is published per
        # browser per wake round instead of three.
        source.notify_local_state_change()
        source.notify_local_state_change()
        source.notify_local_state_change()
        await asyncio.wait_for(
            asyncio.gather(first.first_delta.wait(), second.first_delta.wait()),
            0.5,
        )
        source.notify_local_state_change()
        source.notify_local_state_change()
        source.notify_local_state_change()

        await asyncio.wait_for(asyncio.gather(*tasks), 0.5)

        deltas = [
            frame
            for frame in first.sent + second.sent
            if frame.get("message_kind") == "delta"
        ]
        assert len(deltas) == 4
        identities = [
            (
                frame["shell"]["authoring_workspace"]["workspace_epoch"],
                frame["shell"]["authoring_workspace"]["workspace_revision"],
            )
            for frame in deltas
        ]
        # Every delta names the shared workspace epoch, and no later frame
        # regresses the revision.
        assert all(epoch == identities[0][0] for epoch, _revision in identities)
        revisions = [revision for _epoch, revision in identities]
        assert revisions == sorted(revisions)
        assert not source._local_subscriptions  # noqa: SLF001

    asyncio.run(exercise())


def test_state_enrichment_cannot_block_command_ingress(monkeypatch) -> None:
    class _GatedBrowser(_Browser):
        def __init__(self) -> None:
            super().__init__("stop-command")
            self.command_ready = asyncio.Event()

        async def receive_text(self) -> str:
            await self.command_ready.wait()
            return await super().receive_text()

    async def exercise() -> None:
        upstream = _Upstream(wait_for_browser=False)
        monkeypatch.setattr(
            headless_ws,
            "connect_websocket",
            lambda *_args, **_kwargs: upstream,
        )
        source = _source()
        enrich_started = threading.Event()
        release_enrich = threading.Event()
        original_enrich = source.enrich

        def slow_enrich(payload: str) -> str:
            enrich_started.set()
            release_enrich.wait(timeout=1.0)
            return original_enrich(payload)

        monkeypatch.setattr(source, "enrich", slow_enrich)
        browser = _GatedBrowser()
        task = asyncio.create_task(
            serve_headless_state(browser, source)  # type: ignore[arg-type]
        )
        try:
            assert await asyncio.to_thread(enrich_started.wait, 0.2)
            browser.command_ready.set()
            await asyncio.wait_for(upstream.browser_message.wait(), 0.2)
            assert upstream.sent == ["stop-command"]
        finally:
            release_enrich.set()
        await asyncio.wait_for(task, 0.5)

    asyncio.run(exercise())


def test_oversized_browser_message_is_rejected_before_upstream_send(
    monkeypatch,
) -> None:
    async def exercise() -> None:
        upstream = _Upstream(wait_for_browser=True)
        monkeypatch.setattr(
            headless_ws,
            "connect_websocket",
            lambda *_args, **_kwargs: upstream,
        )
        source = _source()
        browser = _Browser("x" * (64 * 1024 + 1))
        await asyncio.wait_for(
            serve_headless_state(browser, source),  # type: ignore[arg-type]
            0.5,
        )
        assert upstream.sent == []
        assert browser.closed

    asyncio.run(exercise())
