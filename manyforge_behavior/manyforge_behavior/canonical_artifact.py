"""Strict JSON-model YAML and RFC 8785 identity helpers.

This module is intentionally small and runtime-owned.  Node-catalog and
node-environment identity must not depend on YAML presentation, permissive
loader behaviour, or a private specifications checkout.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
import unicodedata
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any, Mapping

import rfc8785
import yaml
from yaml.constructor import ConstructorError
from yaml.events import AliasEvent, MappingStartEvent, SequenceStartEvent
from yaml.tokens import DirectiveToken


SHA256_DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
MAX_DOCUMENT_BYTES = 16 * 1024 * 1024
MAX_DOCUMENT_DEPTH = 64
MAX_DOCUMENT_NODES = 250_000
MAX_SCALAR_BYTES = 1024 * 1024
MAX_SAFE_INTEGER = 9_007_199_254_740_991
CONTENT_IDENTITY_PREFIX = b"manyforge.content-identity.v1"
_JSON_NUMBER_RE = re.compile(
    r"^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?$"
)
_NONFINITE_YAML_RE = re.compile(r"^[+-]?\.(?:inf|nan)$", re.IGNORECASE)
_YAML_PREFIXED_INTEGER_RE = re.compile(
    r"^[+-]?0(?:[oO][0-7_]+|[xX][0-9a-fA-F_]+|[bB][01_]+)$"
)


def _yaml_implicit_non_string_tag(value: str) -> str | None:
    """Return a YAML tag that would reinterpret a plain scalar as non-string."""

    resolvers = yaml.SafeLoader.yaml_implicit_resolvers
    for tag, pattern in (
        *resolvers.get(value[0], ()),
        *resolvers.get(None, ()),
    ):
        if tag != "tag:yaml.org,2002:str" and pattern.match(value):
            return tag
    return None


class StrictArtifactError(ValueError):
    """A serialized artifact is not a strict bounded JSON-model document."""


class _StrictJsonLoader(yaml.BaseLoader):
    """Reject YAML features that have no unambiguous JSON-model meaning."""

    def __init__(self, stream: str) -> None:
        super().__init__(stream)
        self._collection_depth = 0
        self._composed_nodes = 0

    def compose_node(self, parent: yaml.Node | None, index: int | None) -> yaml.Node:
        event = self.peek_event()
        is_collection = isinstance(event, (MappingStartEvent, SequenceStartEvent))
        if is_collection and self._collection_depth >= MAX_DOCUMENT_DEPTH:
            event = self.peek_event()
            raise ConstructorError(
                None,
                None,
                f"collection nesting exceeds {MAX_DOCUMENT_DEPTH}",
                event.start_mark,
            )
        self._composed_nodes += 1
        if self._composed_nodes > MAX_DOCUMENT_NODES:
            event = self.peek_event()
            raise ConstructorError(
                None,
                None,
                f"document contains more than {MAX_DOCUMENT_NODES} nodes",
                event.start_mark,
            )
        if isinstance(event, AliasEvent):
            raise ConstructorError(
                None,
                None,
                "YAML aliases are forbidden",
                event.start_mark,
            )
        if getattr(event, "anchor", None) is not None:
            raise ConstructorError(
                None,
                None,
                "YAML anchors are forbidden",
                event.start_mark,
            )
        if getattr(event, "tag", None) is not None:
            raise ConstructorError(
                None,
                None,
                "explicit YAML tags are forbidden",
                event.start_mark,
            )
        if is_collection:
            self._collection_depth += 1
        try:
            return super().compose_node(parent, index)
        finally:
            if is_collection:
                self._collection_depth -= 1

    def construct_mapping(
        self,
        node: yaml.MappingNode,
        deep: bool = False,
    ) -> dict[str, Any]:
        if not isinstance(node, yaml.MappingNode):
            raise ConstructorError(
                None,
                None,
                f"expected a mapping node, got {node.id}",
                node.start_mark,
            )

        result: dict[str, Any] = {}
        for key_node, value_node in node.value:
            if (
                key_node.tag == "tag:yaml.org,2002:merge"
                or (key_node.value == "<<" and key_node.style is None)
            ):
                raise ConstructorError(
                    "while constructing a mapping",
                    node.start_mark,
                    "YAML merge keys are forbidden",
                    key_node.start_mark,
                )
            key = self.construct_object(key_node, deep=deep)
            if not isinstance(key, str):
                raise ConstructorError(
                    "while constructing a mapping",
                    node.start_mark,
                    "mapping keys must be strings",
                    key_node.start_mark,
                )
            if key == "<<" and key_node.style is None:
                raise ConstructorError(
                    "while constructing a mapping",
                    node.start_mark,
                    "YAML merge keys are forbidden",
                    key_node.start_mark,
                )
            if key in result:
                raise ConstructorError(
                    "while constructing a mapping",
                    node.start_mark,
                    f"duplicate mapping key {key!r}",
                    key_node.start_mark,
                )
            result[key] = self.construct_object(value_node, deep=deep)
        return result

    def construct_scalar(self, node: yaml.ScalarNode) -> Any:
        value = super().construct_scalar(node)
        if not isinstance(value, str):
            raise ConstructorError(
                None,
                None,
                "scalar constructor produced a non-string source value",
                node.start_mark,
            )
        try:
            encoded = value.encode("utf-8")
        except UnicodeEncodeError as exc:
            raise ConstructorError(
                None,
                None,
                "scalar is not valid Unicode",
                node.start_mark,
            ) from exc
        if len(encoded) > MAX_SCALAR_BYTES:
            raise ConstructorError(
                None,
                None,
                f"scalar exceeds {MAX_SCALAR_BYTES} UTF-8 bytes",
                node.start_mark,
            )
        if node.style is not None:
            return value
        if value == "":
            raise ConstructorError(
                None,
                None,
                "empty plain scalars are forbidden; use explicit null",
                node.start_mark,
            )
        if value == "~":
            raise ConstructorError(
                None,
                None,
                "YAML-only null spelling '~' is forbidden",
                node.start_mark,
            )
        if _NONFINITE_YAML_RE.fullmatch(value) is not None:
            raise ConstructorError(
                None,
                None,
                "YAML non-finite number spellings are forbidden",
                node.start_mark,
            )
        if value == "null":
            return None
        if value == "true":
            return True
        if value == "false":
            return False
        if _JSON_NUMBER_RE.fullmatch(value) is None:
            if (
                _YAML_PREFIXED_INTEGER_RE.fullmatch(value) is not None
                or _yaml_implicit_non_string_tag(value) is not None
            ):
                raise ConstructorError(
                    None,
                    None,
                    "YAML-only implicit scalar spellings are forbidden; "
                    "quote the value when a string is intended",
                    node.start_mark,
                )
            return value
        try:
            mathematical = Decimal(value)
            if "." not in value and "e" not in value.lower():
                numeric: int | float = int(value)
            else:
                numeric = float(value)
        except (InvalidOperation, OverflowError, ValueError) as exc:
            raise ConstructorError(
                None,
                None,
                "invalid JSON number",
                node.start_mark,
            ) from exc
        if isinstance(numeric, int):
            if abs(numeric) > MAX_SAFE_INTEGER:
                raise ConstructorError(
                    None,
                    None,
                    "integral JSON number exceeds the I-JSON safe range",
                    node.start_mark,
                )
            return numeric
        if not math.isfinite(numeric):
            raise ConstructorError(
                None,
                None,
                "JSON number overflows the binary64 range",
                node.start_mark,
            )
        if numeric == 0.0 and mathematical != 0:
            raise ConstructorError(
                None,
                None,
                "JSON number underflows the binary64 range",
                node.start_mark,
            )
        if numeric.is_integer() and abs(numeric) > MAX_SAFE_INTEGER:
            raise ConstructorError(
                None,
                None,
                "integral binary64 value exceeds the I-JSON safe range",
                node.start_mark,
            )
        return numeric


def _utf8_scalar_bytes(value: str, label: str) -> bytes:
    if unicodedata.normalize("NFC", value) != value:
        raise StrictArtifactError(f"{label}: strings must already be Unicode NFC")
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise StrictArtifactError(f"{label}: string is not valid Unicode") from exc
    if len(encoded) > MAX_SCALAR_BYTES:
        raise StrictArtifactError(
            f"{label}: scalar exceeds {MAX_SCALAR_BYTES} UTF-8 bytes"
        )
    return encoded


def _validate_json_model(
    value: Any,
    label: str,
    *,
    collection_depth: int = 0,
    budget: list[int] | None = None,
) -> None:
    if budget is None:
        budget = [MAX_DOCUMENT_NODES]
    budget[0] -= 1
    if budget[0] < 0:
        raise StrictArtifactError(
            f"{label}: document contains more than {MAX_DOCUMENT_NODES} values"
        )
    if value is None or isinstance(value, (bool, int, str)):
        if isinstance(value, int) and not isinstance(value, bool):
            if abs(value) > MAX_SAFE_INTEGER:
                raise StrictArtifactError(
                    f"{label}: integral value exceeds the I-JSON safe range"
                )
        if isinstance(value, str):
            _utf8_scalar_bytes(value, label)
        return
    if isinstance(value, float):
        if not math.isfinite(value):
            raise StrictArtifactError(f"{label}: non-finite numbers are forbidden")
        if value.is_integer() and abs(value) > MAX_SAFE_INTEGER:
            raise StrictArtifactError(
                f"{label}: integral binary64 value exceeds the I-JSON safe range"
            )
        return
    if isinstance(value, list):
        if collection_depth >= MAX_DOCUMENT_DEPTH:
            raise StrictArtifactError(
                f"{label}: collection nesting exceeds {MAX_DOCUMENT_DEPTH}"
            )
        for index, item in enumerate(value):
            _validate_json_model(
                item,
                f"{label}[{index}]",
                collection_depth=collection_depth + 1,
                budget=budget,
            )
        return
    if isinstance(value, dict):
        if collection_depth >= MAX_DOCUMENT_DEPTH:
            raise StrictArtifactError(
                f"{label}: collection nesting exceeds {MAX_DOCUMENT_DEPTH}"
            )
        for key, item in value.items():
            if not isinstance(key, str):
                raise StrictArtifactError(f"{label}: mapping keys must be strings")
            budget[0] -= 1
            if budget[0] < 0:
                raise StrictArtifactError(
                    f"{label}: document contains more than "
                    f"{MAX_DOCUMENT_NODES} values"
                )
            _utf8_scalar_bytes(key, f"{label} key")
            _validate_json_model(
                item,
                f"{label}.{key}",
                collection_depth=collection_depth + 1,
                budget=budget,
            )
        return
    raise StrictArtifactError(
        f"{label}: {type(value).__name__} is not a JSON data-model value"
    )


def _json_object_without_duplicates(
    pairs: list[tuple[str, Any]],
) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate mapping key {key!r}")
        result[key] = value
    return result


def _json_binary64(value: str) -> float:
    mathematical = Decimal(value)
    numeric = float(value)
    if not math.isfinite(numeric):
        raise ValueError("JSON number overflows the binary64 range")
    if numeric == 0.0 and mathematical != 0:
        raise ValueError("JSON number underflows the binary64 range")
    return numeric


def _reject_json_constant(token: str) -> None:
    raise ValueError(f"non-finite JSON number {token}")


def _load_exact_json_mapping(text: str, label: str) -> dict[str, Any] | None:
    """Use the linear-time JSON parser when a mapping is exact JSON.

    A JSON parse failure returns ``None`` so the existing strict YAML parser
    remains authoritative for YAML flow mappings and diagnostics. Successful
    JSON values pass the same bounded JSON-model validation as YAML values.
    """

    if not text.lstrip().startswith("{"):
        return None
    try:
        value = json.loads(
            text,
            object_pairs_hook=_json_object_without_duplicates,
            parse_float=_json_binary64,
            parse_constant=_reject_json_constant,
        )
    except (json.JSONDecodeError, InvalidOperation, OverflowError, ValueError):
        return None
    if not isinstance(value, dict):
        return None
    _validate_json_model(value, label)
    return value


def load_strict_json_yaml_bytes(raw: bytes, *, source: str) -> dict[str, Any]:
    """Parse one already-captured bounded UTF-8 mapping."""

    if len(raw) > MAX_DOCUMENT_BYTES:
        raise StrictArtifactError(
            f"{source}: document exceeds {MAX_DOCUMENT_BYTES} bytes"
        )
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise StrictArtifactError(f"{source}: document is not UTF-8") from exc
    exact_json = _load_exact_json_mapping(text, source)
    if exact_json is not None:
        return exact_json
    try:
        if any(
            isinstance(token, DirectiveToken)
            for token in yaml.scan(text, Loader=_StrictJsonLoader)
        ):
            raise StrictArtifactError(
                f"{artifact_path}: YAML directives are forbidden"
            )
        value = yaml.load(text, Loader=_StrictJsonLoader)
    except StrictArtifactError:
        raise
    except yaml.YAMLError as exc:
        raise StrictArtifactError(f"{source}: invalid strict YAML: {exc}") from exc
    if not isinstance(value, dict):
        raise StrictArtifactError(f"{source}: root must be a mapping")
    _validate_json_model(value, source)
    return value


def load_strict_json_yaml(path: str | Path) -> dict[str, Any]:
    """Load one bounded UTF-8 mapping with no YAML-only semantics."""

    artifact_path = Path(path)
    try:
        raw = artifact_path.read_bytes()
    except OSError as exc:
        raise StrictArtifactError(f"cannot read {artifact_path}: {exc}") from exc
    return load_strict_json_yaml_bytes(raw, source=str(artifact_path))


def canonical_json_bytes(value: Any) -> bytes:
    """Return exact RFC 8785 canonical JSON bytes."""

    _validate_json_model(value, "$")
    try:
        return rfc8785.dumps(value)
    except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as exc:
        raise StrictArtifactError(f"value is not RFC 8785 canonicalizable: {exc}") from exc


def raw_sha256_digest(content: bytes) -> str:
    return f"sha256:{hashlib.sha256(content).hexdigest()}"


def require_sha256_digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or SHA256_DIGEST_RE.fullmatch(value) is None:
        raise StrictArtifactError(f"{label} must be sha256:<64 lowercase hex>")
    return value


def compute_omitted_field_digest(
    document: Mapping[str, Any],
    *,
    own_field: str,
) -> str:
    """Hash RFC 8785 bytes after omitting exactly one top-level field."""

    if own_field not in document:
        raise StrictArtifactError(f"missing digest field {own_field!r}")
    payload = dict(document)
    payload.pop(own_field)
    return raw_sha256_digest(canonical_json_bytes(payload))


def verify_omitted_field_digest(
    document: Mapping[str, Any],
    *,
    own_field: str,
    label: str,
) -> str:
    declared = require_sha256_digest(document.get(own_field), f"{label}.{own_field}")
    computed = compute_omitted_field_digest(document, own_field=own_field)
    if declared != computed:
        raise StrictArtifactError(
            f"{label}.{own_field}: digest mismatch; "
            f"declared {declared}, computed {computed}"
        )
    return declared


def compute_content_identity(
    document: Mapping[str, Any],
    *,
    own_field: str,
    identity_domain: str,
    api_version: str,
) -> str:
    """Compute the standard domain-separated ManyForge content identity."""

    for value, label in (
        (identity_domain, "identity_domain"),
        (api_version, "api_version"),
        (own_field, "own_field"),
    ):
        if (
            not isinstance(value, str)
            or not value
            or any(ord(character) < 0x21 or ord(character) > 0x7E for character in value)
        ):
            raise StrictArtifactError(
                f"{label} must be non-empty printable ASCII"
            )
    if document.get("api_version") != api_version:
        raise StrictArtifactError(
            "document api_version does not match content-identity API tag"
        )
    require_sha256_digest(document.get(own_field), own_field)
    payload = dict(document)
    payload.pop(own_field)
    preimage = b"\0".join(
        (
            CONTENT_IDENTITY_PREFIX,
            identity_domain.encode("ascii"),
            api_version.encode("ascii"),
            canonical_json_bytes(payload),
        )
    )
    return raw_sha256_digest(preimage)


def verify_content_identity(
    document: Mapping[str, Any],
    *,
    own_field: str,
    identity_domain: str,
    api_version: str,
    label: str,
) -> str:
    declared = require_sha256_digest(document.get(own_field), f"{label}.{own_field}")
    computed = compute_content_identity(
        document,
        own_field=own_field,
        identity_domain=identity_domain,
        api_version=api_version,
    )
    if declared != computed:
        raise StrictArtifactError(
            f"{label}.{own_field}: content identity mismatch; "
            f"declared {declared}, computed {computed}"
        )
    return declared
