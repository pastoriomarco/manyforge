"""Adversarial vectors for the shared strict identity loader."""

from __future__ import annotations

from pathlib import Path

import pytest

import manyforge_behavior.canonical_artifact as canonical_artifact
from manyforge_behavior.canonical_artifact import (
    MAX_SAFE_INTEGER,
    MAX_SCALAR_BYTES,
    StrictArtifactError,
    canonical_json_bytes,
    load_strict_json_yaml,
)


def _load(tmp_path: Path, text: str):
    path = tmp_path / "artifact.yaml"
    path.write_text(text, encoding="utf-8")
    return load_strict_json_yaml(path)


def test_exact_json_mapping_uses_the_bounded_fast_path(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    path = tmp_path / "artifact.json"
    path.write_text('{"nested":{"enabled":true},"value":1.25}', encoding="utf-8")

    def unexpected_yaml_scan(*args: object, **kwargs: object) -> object:
        del args, kwargs
        raise AssertionError("exact JSON must not enter the YAML scanner")

    monkeypatch.setattr(canonical_artifact.yaml, "scan", unexpected_yaml_scan)

    assert load_strict_json_yaml(path) == {
        "nested": {"enabled": True},
        "value": 1.25,
    }


def test_unambiguous_plain_scalars_remain_strings(tmp_path: Path) -> None:
    loaded = _load(
        tmp_path,
        "\n".join(
            (
                "word_value: hello",
                "identifier_value: on_call",
                "version_like: 1.2.3",
                "path_like: package/module",
                "",
            )
        ),
    )

    assert loaded == {
        "word_value": "hello",
        "identifier_value": "on_call",
        "version_like": "1.2.3",
        "path_like": "package/module",
    }


def test_only_exact_json_plain_scalars_resolve(tmp_path: Path) -> None:
    loaded = _load(
        tmp_path,
        "\n".join(
            (
                "null_value: null",
                "true_value: true",
                "false_value: false",
                "integer_value: -12",
                "decimal_value: 1.25",
                "exponent_value: 1e2",
                'quoted_true: "true"',
                'quoted_number: "1.25"',
                'quoted_yaml_bool: "yes"',
                'quoted_yaml_int: "0x10"',
                'quoted_timestamp: "2026-07-30"',
                'quoted_empty: ""',
                "",
            )
        ),
    )

    assert loaded == {
        "null_value": None,
        "true_value": True,
        "false_value": False,
        "integer_value": -12,
        "decimal_value": 1.25,
        "exponent_value": 100.0,
        "quoted_true": "true",
        "quoted_number": "1.25",
        "quoted_yaml_bool": "yes",
        "quoted_yaml_int": "0x10",
        "quoted_timestamp": "2026-07-30",
        "quoted_empty": "",
    }


@pytest.mark.parametrize(
    ("text", "message"),
    [
        ("value: ~\n", "null spelling"),
        ("value:\n", "empty plain"),
        ("value: .nan\n", "non-finite"),
        ("value: .NaN\n", "non-finite"),
        ("value: +.INF\n", "non-finite"),
        ("value: -.inf\n", "non-finite"),
        ("value: 1e309\n", "overflows"),
        ("value: 1e-4000\n", "underflows"),
        ("value: yes\n", "YAML-only implicit"),
        ("value: ON\n", "YAML-only implicit"),
        ("value: Null\n", "YAML-only implicit"),
        ("value: 012\n", "YAML-only implicit"),
        ("value: 0o12\n", "YAML-only implicit"),
        ("value: 0x10\n", "YAML-only implicit"),
        ("value: 0X10\n", "YAML-only implicit"),
        ("value: 0b10\n", "YAML-only implicit"),
        ("value: .5\n", "YAML-only implicit"),
        ("value: 2026-07-30\n", "YAML-only implicit"),
        ("%YAML 1.2\n---\nvalue: true\n", "directives"),
        ("value: !!str true\n", "explicit YAML tags"),
    ],
)
def test_yaml_only_or_unrepresentable_scalars_reject(
    tmp_path: Path,
    text: str,
    message: str,
) -> None:
    with pytest.raises(StrictArtifactError, match=message):
        _load(tmp_path, text)


def test_i_json_safe_integral_range_applies_after_binary64_conversion(
    tmp_path: Path,
) -> None:
    loaded = _load(
        tmp_path,
        f"positive: {MAX_SAFE_INTEGER}\nnegative: {-MAX_SAFE_INTEGER}\n",
    )
    assert loaded == {
        "positive": MAX_SAFE_INTEGER,
        "negative": -MAX_SAFE_INTEGER,
    }

    for spelling in (
        str(MAX_SAFE_INTEGER + 1),
        f"{MAX_SAFE_INTEGER + 1}.0",
        "9.007199254740992e15",
    ):
        with pytest.raises(StrictArtifactError, match="safe range"):
            _load(tmp_path, f"value: {spelling}\n")


def test_programmatic_canonicalization_enforces_safe_integral_range() -> None:
    with pytest.raises(StrictArtifactError, match="safe range"):
        canonical_json_bytes({"value": MAX_SAFE_INTEGER + 1})
    with pytest.raises(StrictArtifactError, match="safe range"):
        canonical_json_bytes({"value": float(MAX_SAFE_INTEGER + 1)})


def test_unicode_must_already_be_nfc(tmp_path: Path) -> None:
    with pytest.raises(StrictArtifactError, match="Unicode NFC"):
        _load(tmp_path, 'value: "e\u0301"\n')
    with pytest.raises(StrictArtifactError, match="Unicode NFC"):
        _load(tmp_path, '"e\u0301": value\n')


@pytest.mark.parametrize(
    "text",
    [
        "value: &shared text\nother: *shared\n",
        "value: [*missing]\n",
    ],
)
def test_aliases_and_anchors_reject(tmp_path: Path, text: str) -> None:
    with pytest.raises(StrictArtifactError, match="aliases|anchors"):
        _load(tmp_path, text)


def test_merge_key_rejects_even_under_json_scalar_resolution(
    tmp_path: Path,
) -> None:
    with pytest.raises(StrictArtifactError, match="merge keys"):
        _load(tmp_path, "value:\n  <<: inherited\n")


def test_quoted_merge_spelling_is_an_ordinary_json_string_key(
    tmp_path: Path,
) -> None:
    assert _load(tmp_path, 'value:\n  "<<": inherited\n') == {
        "value": {"<<": "inherited"}
    }


def test_scalar_limit_is_utf8_bytes_and_precedes_numeric_resolution(
    tmp_path: Path,
) -> None:
    oversized = "a" * (MAX_SCALAR_BYTES + 1)
    with pytest.raises(StrictArtifactError, match="scalar exceeds"):
        _load(tmp_path, f'value: "{oversized}"\n')


def test_collection_depth_limit_allows_64_and_rejects_65(
    tmp_path: Path,
) -> None:
    # The document root mapping is the first collection level.
    at_limit = ("[" * 63) + "0" + ("]" * 63)
    assert _load(tmp_path, f"value: {at_limit}\n")

    over_limit = ("[" * 64) + "0" + ("]" * 64)
    with pytest.raises(StrictArtifactError, match="collection nesting"):
        _load(tmp_path, f"value: {over_limit}\n")
