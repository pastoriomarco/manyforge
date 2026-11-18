"""Application-start identity, Headless roles and request keys.

``A`` (the application-start identity) is the 64-bit value the Manager mints at
its start and hands to every application process at launch; inside Headless it
is an ``int``, on every wire 16 lowercase hexadecimal characters.

A request key is the 16-byte identity of one request, carried unchanged to the
Kernel ledgers and records. Byte 0 holds the class (bits 7-6) and the part
(bits 5-0); bytes 1-7 the origin; bytes 8-15 the sequence (both big-endian).
"""

from __future__ import annotations

import enum
import itertools
import re
from typing import Any


_APPLICATION_START_ID = re.compile(r"[0-9a-f]{16}")
_REQUEST_KEY = re.compile(r"[0-9A-Fa-f]{32}")

MAXIMUM_ORIGIN = (1 << 56) - 1
MAXIMUM_SEQUENCE = (1 << 64) - 1
MAXIMUM_PART = 63


def parse_application_start_id(text: str) -> int:
    """Parse the one text form of ``A``; raise ``ValueError`` otherwise."""

    if not isinstance(text, str) or _APPLICATION_START_ID.fullmatch(text) is None:
        raise ValueError("application_start_id must be 16 lowercase hex characters")
    value = int(text, 16)
    if value == 0:
        raise ValueError("application_start_id must not be zero")
    return value


def format_application_start_id(value: int) -> str:
    return f"{value:016x}"


class ApplicationRole(enum.Enum):
    """The roles of the one application role enumeration Headless uses."""

    MANAGER = "manager"
    LOGICAL_SCENE = "logical_scene"
    SCENE_RUNTIME = "scene_runtime"
    EXECUTION_HOST = "execution_host"
    KERNEL = "kernel"
    PROGRAM_RUNNER = "program_runner"
    HEADLESS = "headless"
    HEADLESS_PROJECTION = "headless_projection"
    HEADLESS_ALARM = "headless_alarm"


class RequestKeyClass(enum.IntEnum):
    WEBSOCKET = 0
    INTERNAL = 1
    PROGRAM_DERIVED = 2
    HTTP_CLIENT = 3


# Internal minters (class 1 origin).
MINTER_HEADLESS = 5


class RequestKeyInvalid(ValueError):
    """A client request key that is not exactly 32 hexadecimal characters."""


def _key(key_class: int, part: int, origin: int, sequence: int) -> bytes:
    if not 0 <= part <= MAXIMUM_PART:
        raise ValueError("request key part is outside 0-63")
    if not 0 <= origin <= MAXIMUM_ORIGIN:
        raise ValueError("request key origin is outside 56 bits")
    if not 0 <= sequence <= MAXIMUM_SEQUENCE:
        raise ValueError("request key sequence is outside 64 bits")
    return (
        bytes(((key_class << 6) | part,))
        + origin.to_bytes(7, "big")
        + sequence.to_bytes(8, "big")
    )


def websocket_request_key(session_number: int, client_sequence: int) -> bytes:
    """Class 0: the control session number and its client sequence."""

    return _key(RequestKeyClass.WEBSOCKET, 0, session_number, client_sequence)


def http_request_key(text: str) -> bytes:
    """Class 3: the client's 16 bytes with byte 0 overwritten (class 3, part 0).

    Upper or lower case is accepted; everything emitted is lower case.
    """

    if not isinstance(text, str) or _REQUEST_KEY.fullmatch(text) is None:
        raise RequestKeyInvalid("request_key must be exactly 32 hex characters")
    client = bytes.fromhex(text)
    return bytes(((RequestKeyClass.HTTP_CLIENT << 6),)) + client[1:]


def with_part(key: bytes, part: int) -> bytes:
    """The same key for a further Kernel action the request causes."""

    if not 0 <= part <= MAXIMUM_PART:
        raise ValueError("request key part is outside 0-63")
    return bytes(((key[0] & 0xC0) | part,)) + key[1:]


def format_request_key(key: bytes) -> str:
    if len(key) != 16:
        raise ValueError("a request key is 16 bytes")
    return key.hex()


class HeadlessKeyMinter:
    """Class 1 keys of minter 5 (Headless), from 1 per start.

    ``itertools.count`` advances atomically under the interpreter lock, so
    the minter needs no lock of its own.
    """

    def __init__(self) -> None:
        self._sequence = itertools.count(1)

    def mint(self) -> bytes:
        return _key(RequestKeyClass.INTERNAL, 0, MINTER_HEADLESS, next(self._sequence))


class StartIdentityMismatch(Exception):
    """``A`` of a request is missing or is not this start's (contract 1.6).

    ``code`` is ``start_identity_required`` (missing) or
    ``stale_application_start_id`` (any other value than this start's).
    """

    def __init__(self, code: str) -> None:
        super().__init__(code)
        self.code = code


class RequestAdmission:
    """The one Headless check of ``A`` and the one place request keys form.

    Only Headless checks ``A``, once per request, before dispatch. Holds no
    lock: ``A`` is fixed for the process and the minter needs none.
    """

    def __init__(self, application_start_id: str) -> None:
        parse_application_start_id(application_start_id)
        self._application_start_id = application_start_id
        self._minter = HeadlessKeyMinter()

    @property
    def application_start_id(self) -> str:
        return self._application_start_id

    def start_identity(self, value: object) -> None:
        """Raise ``StartIdentityMismatch`` unless ``value`` is this start's ``A``."""

        if value is None:
            raise StartIdentityMismatch("start_identity_required")
        if value != self._application_start_id:
            raise StartIdentityMismatch("stale_application_start_id")

    def mint_key(self) -> str:
        """A fresh class 1 Headless key (minter 5), as 32 lowercase hex."""

        return format_request_key(self._minter.mint())

    def http_key(self, raw: object, *, stop: bool) -> str:
        """Form the key of one HTTP request from the client's 32-hex key.

        A Stop without a key, or with an invalid one, acts as a fresh Stop
        with a Headless key; any other effectful request is refused with
        ``request_key_required`` or ``request_key_invalid``.
        """

        if raw is None:
            if stop:
                return self.mint_key()
            raise RequestKeyRequired()
        try:
            return format_request_key(http_request_key(raw))  # type: ignore[arg-type]
        except RequestKeyInvalid:
            if stop:
                return self.mint_key()
            raise


class RequestKeyRequired(ValueError):
    """An effectful HTTP request other than a Stop carries no request key."""


def display_prefix(application_start_id: str) -> str:
    """The short start prefix a person reads: the first 4 hex characters."""

    return application_start_id[:4]


def display_operation(application_start_id: str, operation_id: int) -> str:
    return f"{display_prefix(application_start_id)}/op{operation_id}"


def display_stop(application_start_id: str, stop_event_id: int) -> str:
    return f"{display_prefix(application_start_id)}/stop{stop_event_id}"


def display_run(application_start_id: str, program_run_id: int) -> str:
    return f"{display_prefix(application_start_id)}/run{program_run_id}"


def display_request_key(application_start_id: str, key: bytes) -> str:
    """``3f9a/s4.118`` for a WebSocket key, ``3f9a/k1a2b3c4d`` otherwise.

    The ``k`` form shows the first 8 hex characters after byte 0 (the
    client's bytes for an HTTP key; the origin for class 1 and 2) (1.16.2).
    """

    prefix = display_prefix(application_start_id)
    if key[0] >> 6 == RequestKeyClass.WEBSOCKET:
        origin = int.from_bytes(key[1:8], "big")
        sequence = int.from_bytes(key[8:16], "big")
        return f"{prefix}/s{origin}.{sequence}"
    return f"{prefix}/k{key[1:5].hex()}"


# The members a reply names its id by, in the order the display picks one
# (CONTRACT CHOICE: one display per reply, the most specific id).
# A Stop reply names its Stop by the event id of its accepted record.
_DISPLAYED_IDS = (
    ("event_id", display_stop),
    ("operation_id", display_operation),
    ("program_run_id", display_run),
)
# Nested members of host replies that hold the accepted or read record; a
# group Stop's acceptance holds its Stop event in ``accepted.stop``.
_DISPLAYED_CONTAINERS = (
    ("accepted",),
    ("operation",),
    ("run",),
    ("capture",),
    ("accepted", "stop"),
    # A command row answered at acceptance (OD-214) names its request key.
    ("command",),
)


def _display(application_start_id: str, value: dict) -> str | None:
    for name, form in _DISPLAYED_IDS:
        number = value.get(name)
        if type(number) is int:
            return form(application_start_id, number)
    key = value.get("request_key")
    if isinstance(key, str) and len(key) == 32:
        return display_request_key(application_start_id, bytes.fromhex(key))
    return None


def causal_display(application_start_id: str, request_key: str | None) -> str | None:
    """The display of a cycle status's ``causal_request_key`` (1.16.2)."""

    if request_key is None:
        return None
    return display_request_key(application_start_id, bytes.fromhex(request_key))


def identified(reply: dict, application_start_id: str) -> dict:
    """Add ``application_start_id`` and ``display`` to a reply naming an id.

    Every Headless reply that carries an operation, Stop or run number or a
    request key also carries ``A`` and the display form (contract 1.5,
    BS 2.9). The id is read from the reply's top level, else from its
    accepted or read record; a reply naming none is returned unchanged.
    """

    shown = _display(application_start_id, reply)
    if shown is None:
        for path in _DISPLAYED_CONTAINERS:
            nested: Any = reply
            for name in path:
                nested = nested.get(name) if isinstance(nested, dict) else None
            if isinstance(nested, dict):
                shown = _display(application_start_id, nested)
                if shown is not None:
                    break
    if shown is None:
        return reply
    return {**reply, "application_start_id": application_start_id, "display": shown}


_DISPLAY_FORM = re.compile(r"(?:([0-9a-f]{16})|([0-9a-f]{4}))/(op|stop|run)([1-9][0-9]{0,15})")
_DISPLAY_KINDS = {"operation_id": "op", "stop_event_id": "stop", "program_run_id": "run"}


def parse_id_filter(name: str, text: str) -> tuple[int, str | None, str | None]:
    """Parse one History id filter: ``7``, ``3f9a/op7`` or ``<16 hex>/op7``.

    Returns the number, the full ``A`` of a copy form and the 4-hex prefix
    of a display form (BS 2.9: "the History filter accepts the display form
    or the full form"). Raises ``ValueError`` for any other text.
    """

    if text.isascii() and text.isdigit() and not text.startswith("0"):
        number, full, prefix = int(text), None, None
    else:
        match = _DISPLAY_FORM.fullmatch(text)
        if match is None or _DISPLAY_KINDS.get(name) != match.group(3):
            raise ValueError(f"{name} must be a number or its display or copy form")
        full, prefix = match.group(1), match.group(2)
        number = int(match.group(4))
    if not 1 <= number <= 9_007_199_254_740_991:
        raise ValueError(f"{name} is outside the id range")
    return number, full, prefix


__all__ = [
    "ApplicationRole",
    "RequestAdmission",
    "RequestKeyRequired",
    "StartIdentityMismatch",
    "HeadlessKeyMinter",
    "MINTER_HEADLESS",
    "RequestKeyClass",
    "RequestKeyInvalid",
    "display_operation",
    "display_prefix",
    "display_request_key",
    "display_run",
    "display_stop",
    "format_application_start_id",
    "format_request_key",
    "http_request_key",
    "causal_display",
    "identified",
    "parse_id_filter",
    "parse_application_start_id",
    "websocket_request_key",
    "with_part",
]
