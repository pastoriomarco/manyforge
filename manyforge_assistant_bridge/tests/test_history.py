from __future__ import annotations

import asyncio
from types import SimpleNamespace

import pytest

from manyforge_assistant_bridge.history import (
    BridgeLedgerError,
    RequestLedger,
    SessionHistory,
)


def test_history_is_lru_bounded_and_never_fails_a_completed_exchange() -> None:
    async def scenario() -> None:
        history = SessionHistory(
            maximum_sessions=1,
            idle_ttl_seconds=60,
            character_budget=4_096,
        )
        await history.save(
            "first",
            [
                {"role": "user", "content": "a" * 3_000},
                {"role": "assistant", "content": "b" * 3_000},
            ],
        )
        assert await history.get("first") == []

        await history.save(
            "coherent",
            [
                {"role": "user", "content": "old" * 900},
                {"role": "assistant", "content": "old" * 900},
                {"role": "user", "content": "recent question"},
                {"role": "assistant", "content": "recent answer"},
            ],
        )
        assert await history.get("coherent") == [
            {"role": "user", "content": "recent question"},
            {"role": "assistant", "content": "recent answer"},
        ]

        await history.save(
            "second",
            [{"role": "assistant", "content": "c" * 5_000}],
        )
        assert await history.get("first") == []
        assert await history.get("second") == []

    asyncio.run(scenario())


def test_cancelled_request_is_not_evicted_until_its_runner_finishes() -> None:
    def request(request_id: str):
        return SimpleNamespace(
            request_id=request_id,
            request_owner_epoch="owner.1",
            source_label="test",
            correlation_id=None,
            conversation_id="conversation.1",
            assistant_mode="composer-assistant",
        )

    async def scenario() -> None:
        started = asyncio.Event()

        async def runner(_record):
            started.set()
            await asyncio.Future()

        ledger = RequestLedger(capacity=1)
        await ledger.submit(request("request.1"), "digest.1", runner)  # type: ignore[arg-type]
        await started.wait()
        await ledger.cancel("request.1")
        with pytest.raises(
            BridgeLedgerError,
            match="all bounded assistant request slots are active",
        ):
            await ledger.submit(request("request.2"), "digest.2", runner)  # type: ignore[arg-type]
        await ledger.close()

    asyncio.run(scenario())
