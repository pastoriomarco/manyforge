#!/usr/bin/env python3
"""Effect-free viewer-fixture configuration plus the I9/I cutover gate.

The full historical implementation is preserved exactly at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` (blob
``0403c905d15a668031a199c0f62dc960d7496e84``):

* lines 33-64 supplied the value-only configuration and URL transformations
  retained here with explicit bounds; the old default layer vocabulary at
  lines 22-29 is left to the future observer contract;
* lines 66-136 minted ``manyforge.viewer.control.v1`` leases and owned the
  WebSocket receive/renew loop; and
* lines 138-186 parsed the old CLI and started that network client.

The latter two responsibilities return only after I9/I defines a qualified
observer-owned viewer contract.  They are intentionally absent rather than
kept as dormant viewer-v1 code.  These URL helpers prepare values only: using
them does not prove that a route, protocol, renderer, or live runtime exists.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
import sys
from typing import Final, NoReturn
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit


RUNTIME_UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
VIEWER_FIXTURE_RESTORATION_GATES: Final = ("I9", "I")

MAX_FIXTURE_URL_CHARS: Final = 4_096
MAX_FIXTURE_DURATION_S: Final = 3_600.0
MAX_REQUESTED_RATE_HZ: Final = 120.0
MAX_LAYER_COUNT: Final = 64
MAX_LAYER_NAME_CHARS: Final = 128
MAX_QUERY_FIELDS: Final = 256


class ViewerFixtureUnavailableError(RuntimeError):
    """Raised before a viewer socket, lease, or runtime is touched."""


def _raise_viewer_fixture_unavailable() -> NoReturn:
    raise ViewerFixtureUnavailableError(
        f"{RUNTIME_UNAVAILABLE_REASON_CODE}: the observer-owned viewer "
        "qualification fixture returns at I9/I"
    )


def _finite_number(value: object, *, field_name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TypeError(f"{field_name} must be a finite number")
    parsed = float(value)
    if not math.isfinite(parsed):
        raise ValueError(f"{field_name} must be finite")
    return parsed


@dataclass(frozen=True, slots=True)
class FixtureConfig:
    """Bounded, transport-neutral inputs for a future I9/I fixture client."""

    url: str
    mode: str
    duration_s: float
    renew_interval_s: float
    requested_rate_hz: float
    layers: tuple[str, ...]
    qualification_send_delay_ms: float = 250.0
    token: str | None = None

    def __post_init__(self) -> None:
        if not isinstance(self.url, str):
            raise TypeError("url must be a string")
        if not self.url or len(self.url) > MAX_FIXTURE_URL_CHARS:
            raise ValueError(
                f"url must contain 1..{MAX_FIXTURE_URL_CHARS} characters"
            )
        parsed_url = urlsplit(self.url)
        if parsed_url.scheme not in {"ws", "wss"} or not parsed_url.netloc:
            raise ValueError("url must be an absolute ws:// or wss:// URL")
        if self.mode not in {"normal", "slow"}:
            raise ValueError("mode must be 'normal' or 'slow'")

        duration_s = _finite_number(self.duration_s, field_name="duration_s")
        if not 0.0 < duration_s <= MAX_FIXTURE_DURATION_S:
            raise ValueError(
                f"duration_s must be in (0, {MAX_FIXTURE_DURATION_S:g}]"
            )
        renew_s = _finite_number(
            self.renew_interval_s,
            field_name="renew_interval_s",
        )
        if not 0.0 < renew_s < 15.0:
            raise ValueError("renew_interval_s must be in (0, 15)")
        rate_hz = _finite_number(
            self.requested_rate_hz,
            field_name="requested_rate_hz",
        )
        if not 0.0 < rate_hz <= MAX_REQUESTED_RATE_HZ:
            raise ValueError(
                f"requested_rate_hz must be in (0, {MAX_REQUESTED_RATE_HZ:g}]"
            )
        delay_ms = _finite_number(
            self.qualification_send_delay_ms,
            field_name="qualification_send_delay_ms",
        )
        if not 0.0 < delay_ms < 2_000.0:
            raise ValueError("qualification_send_delay_ms must be in (0, 2000)")

        if isinstance(self.layers, (str, bytes)):
            raise TypeError("layers must be a sequence of layer names")
        normalized_layers: list[str] = []
        for layer in self.layers:
            if not isinstance(layer, str):
                raise TypeError("layers must contain only strings")
            normalized = layer.strip()
            if not normalized or len(normalized) > MAX_LAYER_NAME_CHARS:
                raise ValueError(
                    "each layer must contain "
                    f"1..{MAX_LAYER_NAME_CHARS} non-whitespace characters"
                )
            if normalized not in normalized_layers:
                normalized_layers.append(normalized)
        if not normalized_layers:
            raise ValueError("at least one layer is required")
        if len(normalized_layers) > MAX_LAYER_COUNT:
            raise ValueError(f"layers exceeds {MAX_LAYER_COUNT} entries")
        object.__setattr__(self, "layers", tuple(normalized_layers))

        if self.token is not None:
            if not isinstance(self.token, str):
                raise TypeError("token must be a string or None")
            if len(self.token) > MAX_FIXTURE_URL_CHARS:
                raise ValueError(
                    f"token must not exceed {MAX_FIXTURE_URL_CHARS} characters"
                )


def _authenticated_url(url: str, token: str | None) -> str:
    """Add or replace a bearer query value without dropping other pairs."""

    if not isinstance(url, str) or not url or len(url) > MAX_FIXTURE_URL_CHARS:
        raise ValueError(f"url must contain 1..{MAX_FIXTURE_URL_CHARS} characters")
    if token is not None and (
        not isinstance(token, str) or len(token) > MAX_FIXTURE_URL_CHARS
    ):
        raise ValueError(
            f"token must be a string of at most {MAX_FIXTURE_URL_CHARS} characters"
        )
    parsed = urlsplit(url)
    query = [
        (name, value)
        for name, value in parse_qsl(
            parsed.query,
            keep_blank_values=True,
            max_num_fields=MAX_QUERY_FIELDS,
        )
        if name != "token"
    ]
    if token:
        query.append(("token", token))
    return urlunsplit(
        (parsed.scheme, parsed.netloc, parsed.path, urlencode(query), parsed.fragment)
    )


def _fixture_url(config: FixtureConfig) -> str:
    """Project bounded fixture configuration into a URL without I/O."""

    url = _authenticated_url(config.url, config.token)
    if config.mode != "slow":
        return url
    parsed = urlsplit(url)
    query = [
        (name, value)
        for name, value in parse_qsl(
            parsed.query,
            keep_blank_values=True,
            max_num_fields=MAX_QUERY_FIELDS,
        )
        if name != "qualification_send_delay_ms"
    ]
    query.append(
        (
            "qualification_send_delay_ms",
            f"{config.qualification_send_delay_ms:g}",
        )
    )
    return urlunsplit(
        (parsed.scheme, parsed.netloc, parsed.path, urlencode(query), parsed.fragment)
    )


async def run_fixture(
    *_args: object,
    **_kwargs: object,
) -> NoReturn:
    """Fail before constructing the deleted viewer-v1 network client."""

    _raise_viewer_fixture_unavailable()


def require_viewer_fixture(
    *_args: object,
    **_kwargs: object,
) -> NoReturn:
    """Synchronous typed gate for callers that would start the fixture."""

    _raise_viewer_fixture_unavailable()


def main(_argv: list[str] | None = None) -> int:
    print(
        f"{RUNTIME_UNAVAILABLE_REASON_CODE}: historical viewer fixture "
        "execution returns at I9/I",
        file=sys.stderr,
    )
    return 78


if __name__ == "__main__":
    raise SystemExit(main())


__all__ = [
    "FixtureConfig",
    "RUNTIME_UNAVAILABLE_REASON_CODE",
    "VIEWER_FIXTURE_RESTORATION_GATES",
    "ViewerFixtureUnavailableError",
    "require_viewer_fixture",
    "run_fixture",
]
