#!/usr/bin/env python3
"""Attach a bounded normal or intentionally slow viewer qualification client.

This fixture creates viewer demand only. It never reports renderer diagnostics,
drives motion, or mutates scene state. A real browser remains the qualification
authority for rendering and frame-time evidence.
"""
from __future__ import annotations

import argparse
import asyncio
from dataclasses import dataclass
import json
import time
from typing import Any, AsyncContextManager, Callable
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit
import uuid

import websockets


_DEFAULT_LAYERS = (
    "canonical",
    "measured_robot",
    "measured_objects",
    "measured_tcp",
    "candidates",
    "diagnostics",
)


@dataclass(frozen=True)
class FixtureConfig:
    url: str
    mode: str
    duration_s: float
    renew_interval_s: float
    requested_rate_hz: float
    layers: tuple[str, ...]
    qualification_send_delay_ms: float = 250.0
    token: str | None = None


def _authenticated_url(url: str, token: str | None) -> str:
    parsed = urlsplit(url)
    query = dict(parse_qsl(parsed.query, keep_blank_values=True))
    if token:
        query["token"] = token
    return urlunsplit(
        (parsed.scheme, parsed.netloc, parsed.path, urlencode(query), parsed.fragment)
    )


def _fixture_url(config: FixtureConfig) -> str:
    url = _authenticated_url(config.url, config.token)
    if config.mode != "slow":
        return url
    parsed = urlsplit(url)
    query = dict(parse_qsl(parsed.query, keep_blank_values=True))
    query["qualification_send_delay_ms"] = f"{config.qualification_send_delay_ms:g}"
    return urlunsplit(
        (parsed.scheme, parsed.netloc, parsed.path, urlencode(query), parsed.fragment)
    )


def _lease_payload(config: FixtureConfig, request_index: int) -> str:
    return json.dumps(
        {
            "type": "lease",
            "schema_version": "manyforge.viewer.control.v1",
            "request_id": f"qualification-{request_index}-{uuid.uuid4().hex}",
            "visibility": "focused",
            "requested_rate_hz": config.requested_rate_hz,
            "layers": list(config.layers),
        },
        separators=(",", ":"),
        sort_keys=True,
    )


async def _run_connected(
    websocket: Any,
    config: FixtureConfig,
    *,
    monotonic: Callable[[], float] = time.monotonic,
) -> dict[str, int | float | str]:
    deadline = monotonic() + config.duration_s
    next_renewal = monotonic()
    leases_sent = 0
    messages_received = 0
    bytes_received = 0

    while monotonic() < deadline:
        now = monotonic()
        if now >= next_renewal:
            await websocket.send(_lease_payload(config, leases_sent))
            leases_sent += 1
            next_renewal = now + config.renew_interval_s

        wait_s = min(0.1, max(0.0, deadline - monotonic()))
        try:
            message = await asyncio.wait_for(websocket.recv(), timeout=wait_s)
        except asyncio.TimeoutError:
            continue
        messages_received += 1
        bytes_received += len(message.encode("utf-8") if isinstance(message, str) else message)

    return {
        "mode": config.mode,
        "duration_s": config.duration_s,
        "leases_sent": leases_sent,
        "messages_received": messages_received,
        "bytes_received": bytes_received,
    }


async def run_fixture(
    config: FixtureConfig,
    *,
    connect: Callable[..., AsyncContextManager[Any]] = websockets.connect,
) -> dict[str, int | float | str]:
    # The client always consumes the protocol normally. In slow mode the gated
    # query parameter asks the server to delay only this client's writer, so the
    # real capacity-one fanout replacement policy is exercised deterministically.
    # The local queue remains bounded in both modes as a second containment layer.
    fixture_url = _fixture_url(config)
    connect_options: dict[str, Any] = {
        "max_queue": 1,
        "max_size": 16 * 1024 * 1024,
        "close_timeout": 1.0,
        "ping_interval": 10.0,
        "ping_timeout": 10.0,
    }
    async with connect(fixture_url, **connect_options) as websocket:
        return await _run_connected(websocket, config)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--url",
        default="ws://127.0.0.1:9000/ws/scene/viewer",
    )
    parser.add_argument("--mode", choices=("normal", "slow"), required=True)
    parser.add_argument("--duration-s", type=float, default=110.0)
    parser.add_argument("--renew-interval-s", type=float, default=5.0)
    parser.add_argument("--requested-rate-hz", type=float, default=15.0)
    parser.add_argument("--qualification-send-delay-ms", type=float, default=250.0)
    parser.add_argument("--layer", action="append", dest="layers")
    parser.add_argument("--token")
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    if args.duration_s <= 0.0:
        raise SystemExit("--duration-s must be positive")
    if args.renew_interval_s <= 0.0 or args.renew_interval_s >= 15.0:
        raise SystemExit("--renew-interval-s must be in (0, 15)")
    if args.requested_rate_hz <= 0.0:
        raise SystemExit("--requested-rate-hz must be positive")
    if not 0.0 < args.qualification_send_delay_ms < 2000.0:
        raise SystemExit("--qualification-send-delay-ms must be in (0, 2000)")
    layers = tuple(dict.fromkeys(args.layers or _DEFAULT_LAYERS))
    if not layers:
        raise SystemExit("at least one --layer is required")
    config = FixtureConfig(
        url=args.url,
        mode=args.mode,
        duration_s=args.duration_s,
        renew_interval_s=args.renew_interval_s,
        requested_rate_hz=args.requested_rate_hz,
        layers=layers,
        qualification_send_delay_ms=args.qualification_send_delay_ms,
        token=args.token,
    )
    try:
        result = asyncio.run(run_fixture(config))
    except KeyboardInterrupt:
        return 130
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
