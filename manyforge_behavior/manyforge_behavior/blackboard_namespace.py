"""Shared helpers for bundle-based blackboard namespace I/O."""

from __future__ import annotations

from dataclasses import dataclass
import time
from typing import Any

import py_trees


@dataclass(frozen=True)
class RuntimeFieldRef:
    """Resolved runtime binding for a declared namespace field."""

    namespace_prefix: str
    field_name: str

    @property
    def path(self) -> str:
        return f"{self.namespace_prefix}/value/{self.field_name}"


def make_runtime_field_ref(namespace_prefix: str, field_name: str) -> RuntimeFieldRef:
    return RuntimeFieldRef(namespace_prefix=namespace_prefix, field_name=field_name)


def write_namespace_bundle(
    blackboard: py_trees.blackboard.Client,
    prefix: str,
    *,
    value: Any,
    status: str,
    timestamp_ns: int | None = None,
    **meta: Any,
) -> dict[str, Any]:
    bundle = {
        "meta": {
            "status": status,
            "timestamp_ns": int(timestamp_ns if timestamp_ns is not None else time.monotonic_ns()),
            **meta,
        },
        "value": value,
    }
    blackboard.set(prefix, bundle)
    return bundle


def register_blackboard_binding_read(
    blackboard: py_trees.blackboard.Client,
    binding: Any,
) -> None:
    if isinstance(binding, RuntimeFieldRef):
        blackboard.register_key(
            key=binding.namespace_prefix,
            access=py_trees.common.Access.READ,
        )
    elif isinstance(binding, str) and binding.strip():
        blackboard.register_key(key=binding.strip(), access=py_trees.common.Access.READ)


def read_namespace_bundle(
    blackboard: py_trees.blackboard.Client,
    prefix: str,
) -> dict[str, Any]:
    bundle = read_blackboard_path(blackboard, prefix)
    if (
        not isinstance(bundle, dict)
        or "meta" not in bundle
        or "value" not in bundle
        or not isinstance(bundle["meta"], dict)
    ):
        raise KeyError(f"Blackboard namespace '{prefix}' is not a valid namespace bundle")
    return bundle


def read_namespace_value(
    blackboard: py_trees.blackboard.Client,
    prefix: str,
) -> Any:
    return read_namespace_bundle(blackboard, prefix)["value"]


def read_namespace_meta(
    blackboard: py_trees.blackboard.Client,
    prefix: str,
) -> dict[str, Any]:
    return read_namespace_bundle(blackboard, prefix)["meta"]


def read_namespace_field(
    blackboard: py_trees.blackboard.Client,
    prefix: str,
    field_name: str,
) -> Any:
    return read_blackboard_path(blackboard, f"{prefix}/value/{field_name}")


def read_runtime_field_ref(
    blackboard: py_trees.blackboard.Client,
    field_ref: RuntimeFieldRef,
) -> Any:
    return read_namespace_field(
        blackboard,
        field_ref.namespace_prefix,
        field_ref.field_name,
    )


def read_blackboard_binding_value(
    blackboard: py_trees.blackboard.Client,
    binding: Any,
) -> Any:
    if isinstance(binding, RuntimeFieldRef):
        return read_runtime_field_ref(blackboard, binding)
    if isinstance(binding, str) and binding.strip():
        return read_blackboard_path(blackboard, binding.strip())
    raise KeyError(f"Unsupported blackboard binding: {binding!r}")


def read_blackboard_path(
    blackboard: py_trees.blackboard.Client,
    path: str,
) -> Any:
    normalized = path.strip()
    if not normalized:
        raise KeyError("Blackboard path must not be empty")

    direct_root = _bundle_root_from_path(normalized)
    if direct_root is not None:
        root_key, relative_parts = direct_root
        blackboard.register_key(key=root_key, access=py_trees.common.Access.READ)
        if blackboard.exists(root_key):
            return _traverse_blackboard_value(blackboard.get(root_key), relative_parts)

    parts = normalized.split("/")
    for prefix_len in range(len(parts), 0, -1):
        key = "/".join(parts[:prefix_len])
        blackboard.register_key(key=key, access=py_trees.common.Access.READ)
        if not blackboard.exists(key):
            continue
        current = blackboard.get(key)
        return _traverse_blackboard_value(current, parts[prefix_len:])
    raise KeyError(f"Missing blackboard key '{normalized}'")


def _bundle_root_from_path(path: str) -> tuple[str, list[str]] | None:
    for separator, lead in (("/value/", ["value"]), ("/meta/", ["meta"])):
        if separator in path:
            prefix, remainder = path.split(separator, 1)
            if prefix and remainder:
                return prefix, [*lead, *remainder.split("/")]
    for suffix, lead in (("/value", ["value"]), ("/meta", ["meta"])):
        if path.endswith(suffix):
            prefix = path[: -len(suffix)]
            if prefix:
                return prefix, lead
    return None


def _traverse_blackboard_value(value: Any, path_parts: list[str]) -> Any:
    current = value
    for segment in path_parts:
        if isinstance(current, dict):
            if segment not in current:
                raise KeyError(f"Missing blackboard path segment '{segment}'")
            current = current[segment]
            continue
        if isinstance(current, list):
            try:
                index = int(segment)
            except ValueError as exc:
                raise KeyError(
                    f"List-valued blackboard path segment '{segment}' is not an integer index"
                ) from exc
            if index < 0 or index >= len(current):
                raise KeyError(f"Blackboard list index '{index}' out of range")
            current = current[index]
            continue
        raise KeyError(
            f"Cannot traverse blackboard path segment '{segment}' through "
            f"{type(current).__name__}"
        )
    return current
