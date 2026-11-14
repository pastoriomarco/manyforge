"""Application-start identity `A` and request keys as Composer uses them.

`A` is the 64-bit value the Manager mints once per application start and hands
to every child as ``--application-start-id <16 hex>``. Inside the process it is
an ``int``; on every wire it is exactly 16 lowercase hex characters, never zero.
A request key is 16 random bytes in memory and 32 hex characters on the wire.
"""

from __future__ import annotations

import enum
import re
import secrets
from typing import Annotated

from pydantic import AfterValidator

_APPLICATION_START_ID = re.compile(r"[0-9a-f]{16}")
_REQUEST_KEY = re.compile(r"[0-9a-fA-F]{32}")


class ApplicationRole(enum.Enum):
    """The application roles Composer names (one enumeration for all processes)."""

    COMPOSER = "composer"
    AUTHORING_OWNER = "authoring_owner"
    COMPOSER_SNAPSHOT = "composer_snapshot"


def parse_application_start_id(text: str) -> int:
    """Parse the 16-hex wire form; raise ``ValueError`` for any other text."""

    if not isinstance(text, str) or _APPLICATION_START_ID.fullmatch(text) is None:
        raise ValueError("application start id must be 16 lowercase hex characters")
    value = int(text, 16)
    if value == 0:
        raise ValueError("application start id must not be zero")
    return value


def format_application_start_id(value: int) -> str:
    return f"{value:016x}"


def parse_request_key(text: str) -> bytes:
    """Parse the 32-hex wire form (either case); raise ``ValueError`` otherwise."""

    if not isinstance(text, str) or _REQUEST_KEY.fullmatch(text) is None:
        raise ValueError("request key must be 32 hex characters")
    return bytes.fromhex(text)


def new_request_key() -> bytes:
    """A fresh random request key for one Composer request to Headless."""

    return secrets.token_bytes(16)


def _checked_application_start_id(text: str) -> str:
    parse_application_start_id(text)
    return text


def _normalized_request_key(text: str) -> str:
    return parse_request_key(text).hex()


# Field types of Composer-local request envelopes (16 hex / 32 hex text).
ApplicationStartIdText = Annotated[str, AfterValidator(_checked_application_start_id)]
RequestKeyText = Annotated[str, AfterValidator(_normalized_request_key)]


__all__ = [
    "ApplicationRole",
    "ApplicationStartIdText",
    "RequestKeyText",
    "format_application_start_id",
    "new_request_key",
    "parse_application_start_id",
    "parse_request_key",
]
