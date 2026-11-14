from __future__ import annotations

import pytest

from manyforge_behavior.request_keys import (
    InternalKeyMinter,
    KeyClass,
    Minter,
    Part,
    Phase,
    format_request_key,
    key_class,
    key_origin,
    key_part,
    key_sequence,
    parse_request_key,
    program_key,
    request_key,
    with_part,
)


def test_layout_is_class_part_origin_sequence_big_endian() -> None:
    key = request_key(KeyClass.HTTP_CLIENT, 0x01020304050607, 0x08090A0B0C0D0E0F, part=5)
    assert key == bytes([0xC5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15])
    assert key_class(key) is KeyClass.HTTP_CLIENT
    assert key_part(key) == 5
    assert key_origin(key) == 0x01020304050607
    assert key_sequence(key) == 0x08090A0B0C0D0E0F


def test_program_key_rederivation_gives_the_same_key() -> None:
    first = program_key(3, 7, Phase.PROGRAM_NODE)
    assert first == program_key(3, 7, Phase.PROGRAM_NODE)
    assert key_class(first) is KeyClass.PROGRAM
    assert key_origin(first) == 3
    assert key_sequence(first) == 7 * 2**16 + int(Phase.PROGRAM_NODE)
    assert program_key(3, 7, Phase.PROGRAM_CLOSE) != first
    assert program_key(3, 8, Phase.PROGRAM_NODE) != first
    assert program_key(4, 7, Phase.PROGRAM_NODE) != first


def test_follow_up_parts_keep_the_starting_key() -> None:
    start = program_key(2, 1, Phase.PROGRAM_NODE)
    stop = with_part(start, Part.OPERATION_STOP)
    assert key_part(stop) == 8 and stop[1:] == start[1:]
    assert key_class(stop) is KeyClass.PROGRAM
    assert with_part(stop, Part.REQUEST) == start


def test_internal_minter_counts_from_one_per_start() -> None:
    minter = InternalKeyMinter(Minter.RUNNER)
    first, second = minter.next_key(), minter.next_key()
    assert key_class(first) is KeyClass.INTERNAL
    assert key_origin(first) == Minter.RUNNER
    assert (key_sequence(first), key_sequence(second)) == (1, 2)


def test_wire_form_is_32_lowercase_hex() -> None:
    key = program_key(9, 2, Phase.MOVE)
    text = format_request_key(key)
    assert len(text) == 32 and text == text.lower()
    assert parse_request_key(text) == key
    # This key's hex has no letters, so upper case is shown with an "A".
    for invalid in (text[:-1] + "A", text[:-1], text + "0", "g" * 32, None):
        with pytest.raises(ValueError):
            parse_request_key(invalid)  # type: ignore[arg-type]


@pytest.mark.parametrize(
    ("run", "attempt", "part"),
    [(0, 1, 0), (1, 0, 0), (1, 2**48, 0), (2**56, 1, 0), (1, 1, 64)],
)
def test_out_of_range_fields_are_refused(run: int, attempt: int, part: int) -> None:
    with pytest.raises(ValueError):
        program_key(run, attempt, Phase.PROGRAM_NODE, part)


def test_nested_reset_part_is_three_and_its_effects_take_reset_parts():
    from manyforge_behavior.request_keys import Part, key_part, with_part

    action = bytes.fromhex("c0" + "11" * 15)
    nested = with_part(action, Part.NESTED_RESET)
    assert Part.NESTED_RESET == 3 and key_part(nested) == 3
    assert with_part(nested, Part.RESET_FORCED_END) == with_part(action, 1)
    assert with_part(nested, Part.RESET_CLAIM) == with_part(action, 2)


def test_model_attempt_keys_are_class_2_per_ordinal_and_rederive():
    import pytest

    from manyforge_behavior.request_keys import (
        KeyClass,
        Phase,
        key_class,
        key_sequence,
        model_attempt_phase,
        program_key,
    )

    first = program_key(3, 7, model_attempt_phase(1))
    assert key_class(first) is KeyClass.PROGRAM
    assert key_sequence(first) == 7 * 2**16 + int(Phase.MODEL_ATTEMPT_BASE) + 1
    assert first == program_key(3, 7, model_attempt_phase(1))
    assert first != program_key(3, 7, model_attempt_phase(2))
    for ordinal in (0, 256):
        with pytest.raises(ValueError):
            model_attempt_phase(ordinal)
