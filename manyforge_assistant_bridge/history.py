"""Bounded conversation and request-state storage for the direct bridge."""

from __future__ import annotations

import asyncio
from collections import OrderedDict
from copy import deepcopy
from dataclasses import dataclass, field
import json
import time
from typing import Any, Awaitable, Callable, Literal

from .contracts import ProviderRequest, provider_status


class BridgeLedgerError(RuntimeError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


class SessionHistory:
    """Per-conversation transcript with one lock, LRU, TTL, and byte proxy."""

    def __init__(
        self,
        *,
        maximum_sessions: int = 50,
        idle_ttl_seconds: float = 1_800.0,
        character_budget: int = 196_608,
    ) -> None:
        if maximum_sessions < 1 or not 60 <= idle_ttl_seconds <= 86_400:
            raise ValueError("conversation history bounds are invalid")
        if not 4_096 <= character_budget <= 4 * 1024 * 1024:
            raise ValueError("conversation history character budget is invalid")
        self._maximum_sessions = maximum_sessions
        self._idle_ttl_seconds = idle_ttl_seconds
        self._character_budget = character_budget
        self._values: OrderedDict[str, tuple[float, list[dict[str, Any]]]] = (
            OrderedDict()
        )
        self._lock = asyncio.Lock()

    async def get(self, conversation_id: str) -> list[dict[str, Any]]:
        async with self._lock:
            self._evict_idle()
            entry = self._values.pop(conversation_id, None)
            if entry is None:
                return []
            messages = entry[1]
            self._values[conversation_id] = (time.monotonic(), messages)
            return deepcopy(messages)

    async def save(
        self, conversation_id: str, messages: list[dict[str, Any]]
    ) -> None:
        bounded = self._truncate(messages)
        async with self._lock:
            self._evict_idle()
            self._values.pop(conversation_id, None)
            self._values[conversation_id] = (time.monotonic(), bounded)
            while len(self._values) > self._maximum_sessions:
                self._values.popitem(last=False)

    async def clear(self, conversation_id: str) -> bool:
        async with self._lock:
            return self._values.pop(conversation_id, None) is not None

    def _evict_idle(self) -> None:
        cutoff = time.monotonic() - self._idle_ttl_seconds
        stale = [key for key, (used, _messages) in self._values.items() if used < cutoff]
        for key in stale:
            self._values.pop(key, None)

    def _truncate(self, messages: list[dict[str, Any]]) -> list[dict[str, Any]]:
        body = [deepcopy(row) for row in messages if row.get("role") != "system"]
        sizes = [
            len(
                json.dumps(
                    row,
                    allow_nan=False,
                    ensure_ascii=False,
                    separators=(",", ":"),
                )
            )
            for row in body
        ]
        total = sum(sizes)
        first = 0
        while len(body) - first > 1 and total > self._character_budget:
            total -= sizes[first]
            first += 1
        if total > self._character_budget:
            # History is conversational convenience, never semantic state.
            # If even the newest message exceeds the configured budget, omit
            # it rather than converting an already completed Draft callback
            # into a failed provider request.
            return []
        bounded = body[first:]
        # Never retain an orphan assistant/tool suffix. OpenAI-compatible
        # providers require tool results to follow their matching assistant
        # call, and beginning at an arbitrary row can make otherwise valid
        # history unparseable. A user message is the only safe exchange
        # boundary in this bridge-owned history.
        while bounded and bounded[0].get("role") != "user":
            bounded.pop(0)
        return bounded


@dataclass(slots=True)
class RequestRecord:
    request: ProviderRequest
    request_digest: str
    state: Literal["running", "completed", "failed", "cancelled"] = "running"
    response: dict[str, Any] | None = None
    error: dict[str, str] | None = None
    cancel_event: asyncio.Event = field(default_factory=asyncio.Event)
    task: asyncio.Task[None] | None = None
    runner_started: bool = False
    updated_at: float = field(default_factory=time.monotonic)

    def status(self) -> dict[str, Any]:
        return provider_status(
            request=self.request,
            state=self.state,
            response=deepcopy(self.response),
            error=deepcopy(self.error),
        )


class RequestLedger:
    """Own in-flight and recent terminal records for outcome reconciliation."""

    def __init__(self, *, capacity: int = 256) -> None:
        if not 1 <= capacity <= 4_096:
            raise ValueError("request ledger capacity is invalid")
        self._capacity = capacity
        self._records: OrderedDict[str, RequestRecord] = OrderedDict()
        self._lock = asyncio.Lock()

    async def submit(
        self,
        request: ProviderRequest,
        request_digest: str,
        runner: Callable[[RequestRecord], Awaitable[dict[str, Any]]],
    ) -> dict[str, Any]:
        async with self._lock:
            existing = self._records.get(request.request_id)
            if existing is not None:
                if existing.request_digest != request_digest:
                    raise BridgeLedgerError(
                        "assistant_request_id_conflict",
                        "requestId was already used for different content",
                    )
                existing.updated_at = time.monotonic()
                self._records.move_to_end(request.request_id)
                return existing.status()
            self._make_capacity()
            record = RequestRecord(request=request, request_digest=request_digest)
            self._records[request.request_id] = record
            record.task = asyncio.create_task(
                self._run(record, runner),
                name=f"manyforge-assistant-{request.request_id}",
            )
            return record.status()

    async def _run(
        self,
        record: RequestRecord,
        runner: Callable[[RequestRecord], Awaitable[dict[str, Any]]],
    ) -> None:
        try:
            record.runner_started = True
            response = await runner(record)
            if record.cancel_event.is_set():
                state = "cancelled"
                response = None
            else:
                state = "completed"
            error = None
        except asyncio.CancelledError:
            state = "cancelled"
            response = None
            error = None
        except Exception as exc:  # the typed route reports this through status
            state = "failed"
            response = None
            error = {
                "code": getattr(exc, "code", "assistant_request_failed"),
                "message": str(exc),
            }
        async with self._lock:
            record.state = state
            record.response = response
            record.error = error
            record.updated_at = time.monotonic()
            record.task = None
            self._records.move_to_end(record.request.request_id)

    async def status(self, request_id: str) -> dict[str, Any]:
        async with self._lock:
            record = self._records.get(request_id)
            if record is None:
                raise BridgeLedgerError(
                    "assistant_request_not_found", "assistant request is unknown"
                )
            record.updated_at = time.monotonic()
            self._records.move_to_end(request_id)
            return record.status()

    async def cancel(self, request_id: str) -> dict[str, Any]:
        async with self._lock:
            record = self._records.get(request_id)
            if record is None:
                raise BridgeLedgerError(
                    "assistant_request_not_found", "assistant request is unknown"
                )
            if record.state == "running":
                record.cancel_event.set()
                record.state = "cancelled"
                if record.task is not None and record.runner_started:
                    record.task.cancel()
            record.updated_at = time.monotonic()
            self._records.move_to_end(request_id)
            return record.status()

    async def close(self) -> None:
        async with self._lock:
            tasks = [record.task for record in self._records.values() if record.task]
            for record in self._records.values():
                record.cancel_event.set()
                if record.task is not None and record.runner_started:
                    record.task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)

    def _make_capacity(self) -> None:
        while len(self._records) >= self._capacity:
            terminal = next(
                (
                    key
                    for key, value in self._records.items()
                    if value.state != "running" and value.task is None
                ),
                None,
            )
            if terminal is None:
                raise BridgeLedgerError(
                    "assistant_request_capacity_exhausted",
                    "all bounded assistant request slots are active",
                )
            self._records.pop(terminal)


__all__ = [
    "BridgeLedgerError",
    "RequestLedger",
    "RequestRecord",
    "SessionHistory",
]
