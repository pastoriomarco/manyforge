"""Request keys: the 16-byte identity of one request.

A request key is carried unchanged from its sender to the Kernel ledgers and
records. Byte 0 holds the class (bits 7-6) and the part (bits 5-0), bytes 1-7
the origin (56 bits, big-endian) and bytes 8-15 the sequence (64 bits,
big-endian). In memory a key is ``bytes`` of length 16; on every wire it is
exactly 32 lowercase hexadecimal characters.

Classes:

* ``WEBSOCKET`` (0): origin is the control session number, sequence the
  client sequence. Minted by Headless.
* ``INTERNAL`` (1): origin is the minter (:class:`Minter`), sequence the
  minter's own counter from 1 per start. The key is stored on the in-memory
  object that owns the action, so a resend reuses it.
* ``PROGRAM`` (2): origin is the run number, sequence is the attempt number
  (48 bits) times 2**16 plus the phase code (:class:`Phase`). Deterministic:
  re-deriving gives the same key, no stored state.
* ``HTTP_CLIENT`` (3): the client's 16 random bytes with byte 0 overwritten by
  Headless.

Part numbers are fixed per command kind (:class:`Part`): part 0 is the request
itself, parts 1-63 the further Kernel actions it causes.
"""

from __future__ import annotations

import enum
import itertools
import re
from typing import Final

REQUEST_KEY_BYTES: Final = 16
_ORIGIN_LIMIT: Final = 1 << 56
_SEQUENCE_LIMIT: Final = 1 << 64
_ATTEMPT_LIMIT: Final = 1 << 48
_PHASE_LIMIT: Final = 1 << 16
_PART_LIMIT: Final = 1 << 6
_REQUEST_KEY_TEXT: Final = re.compile(r"[0-9a-f]{32}")


class KeyClass(enum.IntEnum):
    WEBSOCKET = 0
    INTERNAL = 1
    PROGRAM = 2
    HTTP_CLIENT = 3


class Minter(enum.IntEnum):
    """Origins of class 1 keys."""

    MANAGER = 1
    COMPOSER = 2
    KERNEL = 3
    RUNNER = 4
    HEADLESS = 5


class Part(enum.IntEnum):
    """Part numbers, fixed per command kind.

    The same number can serve several command kinds: a part is read together
    with the key of the request that caused it.
    """

    REQUEST = 0
    # Program cycle Reset.
    RESET_FORCED_END = 1
    RESET_CLAIM = 2
    # Cycle recovery action: the Reset it nests; that Reset's Forced Program
    # End and claim take parts 1 and 2 of the same key.
    NESTED_RESET = 3
    # Run (Program start).
    START_RACE_FORCED_END = 1
    # Stop (level 1, selection).
    SELECTION_CONTAINMENT_STOP = 1
    # Follow-ups on an operation, keyed on the key that started it.
    OPERATION_STOP = 8
    OPERATION_ACKNOWLEDGE_COMPLETED = 9
    OPERATION_RESET = 10
    HELD_PREPARATION_RESET = 11


class Phase(enum.IntEnum):
    """Phase codes of class 2 keys: one action per (run, attempt, phase).

    Values are never reused; 0 is invalid.
    """

    PROGRAM_NODE = 1
    MOVE = 2
    PICK_PLACE_SEQUENCE = 3
    PICK_PLACE_APPROACH_MOVE = 4
    PICK_PLACE_DESCEND_MOVE = 5
    PICK_PLACE_TOOL_MOVE = 6
    PICK_PLACE_RETREAT_MOVE = 7
    PICK_PLACE_APPROACH = 8
    PICK_PLACE_DESCEND = 9
    PICK_PLACE_TOOL = 10
    PICK_PLACE_RETREAT = 11
    PROGRAM_CONTROL = 12
    PROGRAM_IO_WATCH = 13
    # 14 is retired (scene node commits take class 1 keys per admission generation).
    PROGRAM_CLOSE = 15
    PNEUMATIC_CLOSE = 16
    AUTOMATIC_RETRY = 17
    ASSISTANT_TASK = 18
    # 256-511: model attempts, MODEL_ATTEMPT_BASE + the model call's ordinal
    # within its node attempt (model_attempt_phase).
    MODEL_ATTEMPT_BASE = 256


MODEL_ATTEMPT_ORDINALS: Final = 256


def model_attempt_phase(ordinal: int) -> int:
    """The phase code of a node attempt's ``ordinal``-th model call."""

    if not 1 <= ordinal < MODEL_ATTEMPT_ORDINALS:
        raise ValueError("model attempt ordinal must be 1 to 255")
    return int(Phase.MODEL_ATTEMPT_BASE) + ordinal


PICK_PLACE_MOVE_PHASES: Final = {
    "approach": Phase.PICK_PLACE_APPROACH_MOVE,
    "descend": Phase.PICK_PLACE_DESCEND_MOVE,
    "tool": Phase.PICK_PLACE_TOOL_MOVE,
    "retreat": Phase.PICK_PLACE_RETREAT_MOVE,
}
PICK_PLACE_PHASES: Final = {
    "approach": Phase.PICK_PLACE_APPROACH,
    "descend": Phase.PICK_PLACE_DESCEND,
    "tool": Phase.PICK_PLACE_TOOL,
    "retreat": Phase.PICK_PLACE_RETREAT,
}


def request_key(key_class: KeyClass, origin: int, sequence: int, part: int = 0) -> bytes:
    """Compose a request key from its fields."""

    if not 0 <= part < _PART_LIMIT:
        raise ValueError("request key part must be 0-63")
    if not 0 <= origin < _ORIGIN_LIMIT:
        raise ValueError("request key origin must fit 56 bits")
    if not 0 <= sequence < _SEQUENCE_LIMIT:
        raise ValueError("request key sequence must fit 64 bits")
    return (
        bytes(((int(key_class) << 6) | part,))
        + origin.to_bytes(7, "big")
        + sequence.to_bytes(8, "big")
    )


def program_key(
    run_number: int, attempt_number: int, phase: Phase | int, part: int = 0
) -> bytes:
    """The class 2 key of one action named by (run, attempt, phase)."""

    if run_number < 1:
        raise ValueError("run number must be positive")
    if not 1 <= attempt_number < _ATTEMPT_LIMIT:
        raise ValueError("attempt number must be 1 to 2**48-1")
    if not 1 <= int(phase) < _PHASE_LIMIT:
        raise ValueError("phase code must be 1 to 2**16-1")
    return request_key(
        KeyClass.PROGRAM, run_number, attempt_number * _PHASE_LIMIT + int(phase), part
    )


def with_part(key: bytes, part: int) -> bytes:
    """The same key with another part number."""

    if len(key) != REQUEST_KEY_BYTES:
        raise ValueError("request key must be 16 bytes")
    if not 0 <= part < _PART_LIMIT:
        raise ValueError("request key part must be 0-63")
    return bytes(((key[0] & 0xC0) | part,)) + key[1:]


def key_class(key: bytes) -> KeyClass:
    return KeyClass(key[0] >> 6)


def key_part(key: bytes) -> int:
    return key[0] & 0x3F


def key_origin(key: bytes) -> int:
    return int.from_bytes(key[1:8], "big")


def key_sequence(key: bytes) -> int:
    return int.from_bytes(key[8:16], "big")


def format_request_key(key: bytes) -> str:
    """The wire form: exactly 32 lowercase hexadecimal characters."""

    if len(key) != REQUEST_KEY_BYTES:
        raise ValueError("request key must be 16 bytes")
    return key.hex()


def parse_request_key(text: str) -> bytes:
    """Parse the wire form; ManyForge emits and accepts lower case only."""

    if not isinstance(text, str) or _REQUEST_KEY_TEXT.fullmatch(text) is None:
        raise ValueError("request key must be exactly 32 lowercase hex characters")
    return bytes.fromhex(text)


class InternalKeyMinter:
    """Class 1 keys of one minter: its own counter, from 1 per start.

    ``next(itertools.count())`` is atomic in CPython, so minting needs no lock
    and may run under the caller's existing state lock.
    """

    __slots__ = ("_minter", "_counter")

    def __init__(self, minter: Minter) -> None:
        self._minter = Minter(minter)
        self._counter = itertools.count(1)

    def next_key(self) -> bytes:
        return request_key(KeyClass.INTERNAL, int(self._minter), next(self._counter))


RUNNER_KEYS: Final = InternalKeyMinter(Minter.RUNNER)
"""The runner process's class 1 minter (the behaviour layer runs in it)."""


__all__ = [
    "InternalKeyMinter",
    "MODEL_ATTEMPT_ORDINALS",
    "model_attempt_phase",
    "KeyClass",
    "Minter",
    "PICK_PLACE_MOVE_PHASES",
    "PICK_PLACE_PHASES",
    "Part",
    "Phase",
    "REQUEST_KEY_BYTES",
    "RUNNER_KEYS",
    "format_request_key",
    "key_class",
    "key_origin",
    "key_part",
    "key_sequence",
    "parse_request_key",
    "program_key",
    "request_key",
    "with_part",
]
