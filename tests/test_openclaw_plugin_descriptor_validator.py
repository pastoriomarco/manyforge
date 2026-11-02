"""Bounded owner-neutral OpenClaw descriptor validation."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
from types import ModuleType

import pytest


_SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "scripts"
    / "generate-manyforge-openclaw-plugin.py"
)


def _load_validator() -> ModuleType:
    name = "manyforge_openclaw_descriptor_validator_test_subject"
    spec = importlib.util.spec_from_file_location(name, _SCRIPT)
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def _descriptor(schema=None):
    return {
        "name": "status_read",
        "description": "Read status.",
        "inputSchema": schema if schema is not None else {"type": "object"},
    }


def test_descriptor_validator_returns_a_bounded_deep_copy() -> None:
    module = _load_validator()
    source = [_descriptor({"type": "object", "properties": {}})]

    normalized = module.validate_tool_descriptors(source)

    assert normalized[0] == source[0]
    assert normalized[0] is not source[0]
    assert normalized[0]["inputSchema"] is not source[0]["inputSchema"]


def test_descriptor_validator_rejects_deep_recursive_or_nonfinite_schemas() -> None:
    module = _load_validator()
    deep: dict[str, object] = {}
    cursor = deep
    for _ in range(module.MAX_SCHEMA_DEPTH + 1):
        child: dict[str, object] = {}
        cursor["child"] = child
        cursor = child
    with pytest.raises(ValueError, match="level bound"):
        module.validate_tool_descriptors([_descriptor(deep)])

    recursive: dict[str, object] = {}
    recursive["self"] = recursive
    with pytest.raises(ValueError, match="recursive container"):
        module.validate_tool_descriptors([_descriptor(recursive)])

    with pytest.raises(ValueError, match="finite"):
        module.validate_tool_descriptors(
            [_descriptor({"type": "number", "default": float("nan")})]
        )


def test_descriptor_validator_rejects_oversized_schema_collections() -> None:
    module = _load_validator()

    with pytest.raises(ValueError, match="4096-item bound"):
        module.validate_tool_descriptors(
            [_descriptor({"enum": [None] * (module.MAX_SCHEMA_COLLECTION_ITEMS + 1)})]
        )
