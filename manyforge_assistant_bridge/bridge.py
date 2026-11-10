"""Bounded assistant-provider bridge.

The bridge owns only model mediation, bounded conversation/request records,
callback routing, and audit. Catalog meaning comes from the independently read
headless projection; Draft mutation remains in Composer and runtime mutation is
never executed here.
"""

from __future__ import annotations

import argparse
import asyncio
import difflib
from contextlib import asynccontextmanager
from copy import deepcopy
from dataclasses import dataclass, replace
import hashlib
import json
import os
from pathlib import Path
import re
import time
from typing import Any, Final, Mapping, Sequence
from urllib.parse import quote

from fastapi import FastAPI, HTTPException, Path as ApiPath, Request
from fastapi.responses import JSONResponse
import httpx
from jsonschema import Draft202012Validator
from jsonschema.exceptions import SchemaError
import uvicorn

from manyforge_headless_api.client import HeadlessHttpClient
from manyforge_headless_api.read_client import (
    HeadlessReadClient,
    HeadlessReadInputError,
)

from .catalog import (
    AssistantCatalogError,
    _narrow_schema,
    normalize_field_policy,
    ResolvedAssistantCatalog,
    canonical_json,
    resolve_catalog,
    verify_active_mode_projection,
)
from .contracts import (
    PROVIDER_RESPONSE_VERSION,
    ProviderRequest,
)
from .node_catalog import (
    FieldError,
    NodeCallChecker,
    catalog_rows,
    collect_errors,
    enclosing_shape,
    failure_detail,
    json_shape,
    failure_text,
    kind_shapes,
    normalize_tree_arguments,
    placeholder_errors,
    placement_errors,
    value_source_ids,
    PLACEMENT_FIELDS,
    VALUE_SOURCE_SHAPE,
)
from .constrained import RegenerationFailed, adopt, regenerate_call, regenerates
from .prompt import render_system_prompt
from .recovery import StuckLoopTracker
from .history import (
    BridgeLedgerError,
    RequestLedger,
    RequestRecord,
    SessionHistory,
    compact_exchange,
)


MAXIMUM_PROVIDER_BODY_BYTES: Final = 4 * 1024 * 1024
MAXIMUM_UPSTREAM_RESPONSE_BYTES: Final = 8 * 1024 * 1024
MAXIMUM_CALLBACK_RESPONSE_BYTES: Final = 8 * 1024 * 1024
MAXIMUM_MODEL_TEXT_BYTES: Final = 65_536
MAXIMUM_TOOL_CALLS_PER_TURN: Final = 64
MAXIMUM_SAFE_INTEGER: Final = 9_007_199_254_740_991
_REQUEST_ID_PATTERN: Final = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]*$")
_COMPOSER_TOOL_EFFECTS: Final = frozenset(
    {"composer_draft_mutating"}
)
_COMPOSER_READ_TOOLS: Final = frozenset({"program_read", "program_validate", "scene_inspect"})
# Typed failures caused by a tool call's arguments (returned to the model).
_ARGUMENT_FAILURE_CODES: Final = frozenset(
    {
        "assistant_tool_arguments_invalid",
        "assistant_skill_body_unavailable",
        # Composer's refusal of the call's arguments (its own schema check).
        "assistant_callback_arguments_invalid",
        # A read whose result exceeds the model-visible bound: a narrower
        # read (fewer entries, no parameters) fits.
        "assistant_owner_response_oversized",
    }
)
_HEADLESS_SIMPLE_READS: Final = {
    "alarms_read": ("alarms", {"limit": 32}),
    "deployment_capabilities_read": ("node_environment", {}),
    "status_read": ("runtime", {}),
}


class BridgeRunError(RuntimeError):
    def __init__(
        self,
        code: str,
        message: str,
        *,
        audit_detail: dict[str, Any] | None = None,
        detail: dict[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.audit_detail = deepcopy(audit_detail)
        # Typed fields shown to the model with the failure (field path,
        # expected shape); the message is only their presentation.
        self.detail = deepcopy(detail) if detail is not None else {"code": code}


def _prim_path_matches(path: str, root: str) -> bool:
    normalized = root.rstrip("/") or "/"
    return path == normalized or (
        normalized == "/" and path.startswith("/")
    ) or path.startswith(normalized + "/")


def _simulation_entity_path(source: dict[str, Any]) -> str | None:
    payload = source.get("payload")
    if type(payload) is not dict or payload.get("kind") not in {
        "simulation_entity_pose",
        "simulation_entity_pose_with_reference.v1",
    }:
        return None
    descriptor = payload.get("source")
    if type(descriptor) is dict and type(descriptor.get("entity")) is str:
        return descriptor["entity"]
    source_error = payload.get("source_error")
    if type(source_error) is dict and type(source_error.get("source_key")) is str:
        return source_error["source_key"]
    raise BridgeRunError(
        "assistant_owner_response_invalid",
        "scene runtime contains a malformed simulation-entity observation",
    )


def _inspect_isaac_scene_report(
    scene: dict[str, Any],
    arguments: dict[str, Any],
) -> dict[str, Any]:
    """Build a read-only presentation over Headless-owned scene truth."""

    if scene.get("schema_version") != "manyforge.scene_projection.v1":
        raise BridgeRunError(
            "assistant_owner_response_invalid",
            "Headless returned an unsupported scene projection",
        )
    projected = deepcopy(scene)
    runtime_section = projected.get("scene_runtime")
    logical_section = projected.get("logical_scene")
    if type(runtime_section) is not dict or type(logical_section) is not dict:
        raise BridgeRunError(
            "assistant_owner_response_invalid",
            "Headless scene projection is missing its owner sections",
        )

    include = tuple(arguments.get("include_subtrees", ()))
    exclude = tuple(arguments.get("exclude_subtrees", ()))
    probes = tuple(arguments.get("probe_prims", ()))
    source_count: int | None = None
    selected_count: int | None = None
    selected_entity_count: int | None = None
    runtime_payload = runtime_section.get("payload")
    if runtime_payload is not None:
        if (
            type(runtime_payload) is not dict
            or type(runtime_payload.get("state")) is not dict
        ):
            raise BridgeRunError(
                "assistant_owner_response_invalid",
                "Headless scene-runtime payload is malformed",
            )
        sources = runtime_payload["state"].get("pose_sources")
        if type(sources) is not list or any(
            type(row) is not dict for row in sources
        ):
            raise BridgeRunError(
                "assistant_owner_response_invalid",
                "Headless scene-runtime pose sources are malformed",
            )
        source_count = len(sources)
        filtered: list[dict[str, Any]] = []
        selected_entity_count = 0
        for source in sources:
            entity = _simulation_entity_path(source)
            if entity is None:
                filtered.append(source)
                continue
            admitted = (
                not include
                or any(_prim_path_matches(entity, root) for root in include)
                or entity in probes
            ) and not any(_prim_path_matches(entity, root) for root in exclude)
            if admitted:
                filtered.append(source)
                selected_entity_count += 1
        runtime_payload["state"]["pose_sources"] = filtered
        selected_count = len(filtered)

    requested_export = arguments.get("mesh_export_dir")
    return {
        "schema_version": "manyforge.assistant.isaac_scene_inspection.v1",
        "source_kind": "headless_scene_projection",
        "projection_generation": scene.get("projection_generation"),
        "presentation": {
            "exclude_subtrees": list(exclude),
            "include_subtrees": list(include),
            "mesh_export": {
                "performed": False,
                "requested_directory": requested_export,
                "reason": (
                    "Headless inspection is read-only and performs no simulator "
                    "or filesystem RPC"
                ),
            },
            "mesh_source_preference": arguments.get(
                "mesh_source_preference", "auto"
            ),
            "output_profile": arguments.get(
                "output_profile", "llm_scene_authoring"
            ),
            "pose_source_count": source_count,
            "probe_prims": list(probes),
            "selected_pose_source_count": selected_count,
            "selected_simulation_entity_count": selected_entity_count,
        },
        "scene": present_scene(projected),
        "simulation_pose_sources": (
            deepcopy(runtime_payload["state"].get("pose_sources") or [])
            if type(runtime_payload) is dict and type(runtime_payload.get("state")) is dict
            else []
        ),
    }


def _shape_in_tool_form(shapes: Any) -> dict[str, Any] | None:
    if type(shapes) is not list or len(shapes) != 1 or type(shapes[0]) is not dict:
        return None
    shape = shapes[0]
    kind = shape.get("type")
    dims = shape.get("dimensions_m")
    if kind == "box" and type(dims) is dict:
        return {"type": "box", "box_dimensions_m": [dims.get("x"), dims.get("y"), dims.get("z")]}
    if kind == "sphere" and "radius_m" in shape:
        return {"type": "sphere", "sphere_radius_m": shape["radius_m"]}
    if kind == "cylinder" and "radius_m" in shape:
        return {"type": "cylinder", "cylinder_radius_m": shape["radius_m"],
                "cylinder_height_m": shape.get("height_m")}
    return None


def present_scene(scene: dict[str, Any]) -> dict[str, Any]:
    """The Applied scene (for inspect_isaac_scene) in the shape the scene tools take.

    Each logical-scene object becomes a scene resource
    `{scene_resource_id, shape, pose: {frame_ref, position_m, orientation}}`;
    an object not fixed in the cell (carried) or with another shape is listed
    by id with its placement kind. Projection bookkeeping is dropped.
    """

    logical = scene.get("logical_scene") if type(scene.get("logical_scene")) is dict else {}
    snapshot = ((logical.get("payload") or {}).get("snapshot") or {}) if type(
        logical.get("payload")) is dict else {}
    resources: list[dict[str, Any]] = []
    other: list[dict[str, Any]] = []
    for row in snapshot.get("objects") or []:
        if type(row) is not dict:
            continue
        placement = row.get("placement") if type(row.get("placement")) is dict else {}
        pose = placement.get("pose_in_cell") if type(placement.get("pose_in_cell")) is dict else {}
        position = pose.get("position_m")
        shape = _shape_in_tool_form(row.get("collision_shapes"))
        if placement.get("kind") == "cell_fixed" and type(position) is dict and shape:
            resources.append({
                "scene_resource_id": row.get("scene_resource_id"),
                "shape": shape,
                "pose": {"frame_ref": {"kind": "cell_frame"},
                         "position_m": [position.get("x"), position.get("y"), position.get("z")],
                         "orientation": {"xyzw": pose.get("orientation_xyzw")}},
            })
        else:
            other.append({"scene_resource_id": row.get("scene_resource_id"),
                          "placement_kind": placement.get("kind")})
    return {
        "note": "The Applied scene (what runs). The Draft scene is in scene_inspect; "
        "edit it with the scene_draft tools in this same shape.",
        "scene_resources": resources,
        "other_objects": other,
        "carried_object_attachments": deepcopy(snapshot.get("carried_object_attachments") or []),
    }


@dataclass(frozen=True, slots=True)
class BridgeConfig:
    headless_api_url: str
    composer_base_url: str
    audit_log_path: Path
    upstream_timeout_seconds: float = 3_600.0
    callback_timeout_seconds: float = 10.0
    maximum_wall_seconds: float = 180.0
    maximum_turns: int = 16
    maximum_output_tokens: int = 65_536
    request_capacity: int = 256
    history_maximum_sessions: int = 50
    history_idle_ttl_seconds: float = 1_800.0
    history_character_budget: int = 196_608
    audit_maximum_bytes: int = 10 * 1024 * 1024
    audit_keep_files: int = 3

    def __post_init__(self) -> None:
        for name, value in (
            ("headless_api_url", self.headless_api_url),
            ("composer_base_url", self.composer_base_url),
        ):
            if not value or value != value.strip() or len(value) > 4_096:
                raise ValueError(f"{name} must be one bounded URL")
        for name, value, maximum in (
            ("upstream_timeout_seconds", self.upstream_timeout_seconds, 3_600.0),
            ("callback_timeout_seconds", self.callback_timeout_seconds, 60.0),
            ("maximum_wall_seconds", self.maximum_wall_seconds, 1_800.0),
        ):
            if not 0 < value <= maximum:
                raise ValueError(f"{name} is outside its supported bound")
        if not 1 <= self.maximum_turns <= 64:
            raise ValueError("maximum_turns must be between 1 and 64")
        if not 1 <= self.maximum_output_tokens <= 65_536:
            raise ValueError("maximum_output_tokens is outside its bound")
        if not 1 * 1024 * 1024 <= self.audit_maximum_bytes <= 1024 * 1024 * 1024:
            raise ValueError("audit_maximum_bytes is outside its bound")
        if not 1 <= self.audit_keep_files <= 16:
            raise ValueError("audit_keep_files must be between 1 and 16")


class _BodyLimitMiddleware:
    def __init__(self, app, maximum_bytes: int) -> None:
        self.app = app
        self.maximum_bytes = maximum_bytes

    async def __call__(self, scope, receive, send) -> None:
        if scope["type"] != "http" or not scope.get("path", "").startswith(
            "/v1/manyforge/assistant"
        ):
            await self.app(scope, receive, send)
            return
        raw_length = dict(scope.get("headers", ())).get(b"content-length")
        if raw_length is not None:
            try:
                if int(raw_length) > self.maximum_bytes:
                    await self._reject(scope, receive, send)
                    return
            except ValueError:
                await self._reject(scope, receive, send)
                return
        body = bytearray()
        disconnected = False
        while True:
            message = await receive()
            if message["type"] == "http.disconnect":
                disconnected = True
                break
            if message["type"] != "http.request":
                continue
            body.extend(message.get("body", b""))
            if len(body) > self.maximum_bytes:
                await self._reject(scope, receive, send)
                return
            if not message.get("more_body", False):
                break
        if (
            scope.get("method") == "POST"
            and scope.get("path") == "/v1/manyforge/assistant"
        ):
            try:
                json.loads(
                    bytes(body),
                    parse_constant=_reject_nonfinite_json,
                    object_pairs_hook=_object_without_duplicates,
                )
            except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
                await self._reject_invalid_json(scope, send)
                return
        delivered = False

        async def replay():
            nonlocal delivered
            if not delivered:
                delivered = True
                if disconnected:
                    return {"type": "http.disconnect"}
                return {"type": "http.request", "body": bytes(body), "more_body": False}
            return await receive()

        await self.app(scope, replay, send)

    async def _reject(self, scope, receive, send) -> None:
        del receive

        async def disconnected():
            return {"type": "http.disconnect"}

        response = JSONResponse(
            status_code=413,
            content={
                "detail": {
                    "code": "assistant_request_oversized",
                    "message": "assistant request exceeds its byte bound",
                }
            },
        )
        await response(scope, disconnected, send)

    @staticmethod
    async def _reject_invalid_json(scope, send) -> None:
        async def disconnected():
            return {"type": "http.disconnect"}

        response = JSONResponse(
            status_code=400,
            content={
                "detail": {
                    "code": "assistant_request_invalid_json",
                    "message": "assistant request must be strict duplicate-free JSON",
                }
            },
        )
        await response(scope, disconnected, send)


class AuditWriter:
    """Bounded rotating JSONL audit that never stores prompt/response bodies."""

    def __init__(self, path: Path, *, maximum_bytes: int, keep_files: int) -> None:
        self._path = path.expanduser()
        self._maximum_bytes = maximum_bytes
        self._keep_files = keep_files
        self._lock = asyncio.Lock()

    async def write(self, record: dict[str, Any]) -> None:
        try:
            payload = (
                json.dumps(
                    record,
                    allow_nan=False,
                    ensure_ascii=False,
                    separators=(",", ":"),
                    sort_keys=True,
                )
                + "\n"
            ).encode("utf-8")
        except (TypeError, ValueError, UnicodeError) as error:
            raise BridgeRunError(
                "assistant_audit_invalid", "audit record is not strict JSON"
            ) from error
        if len(payload) > 256 * 1024:
            raise BridgeRunError("assistant_audit_invalid", "audit record exceeds its bound")
        async with self._lock:
            await asyncio.to_thread(self._write_sync, payload)

    def _write_sync(self, payload: bytes) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        if self._path.exists() and self._path.stat().st_size + len(payload) > self._maximum_bytes:
            for index in range(self._keep_files, 0, -1):
                source = self._path if index == 1 else self._path.with_suffix(
                    self._path.suffix + f".{index - 1}"
                )
                target = self._path.with_suffix(self._path.suffix + f".{index}")
                if source.exists():
                    if index == self._keep_files and target.exists():
                        target.unlink()
                    source.replace(target)
        descriptor = os.open(
            self._path,
            os.O_APPEND | os.O_CREAT | os.O_WRONLY | os.O_CLOEXEC,
            0o600,
        )
        try:
            remaining = memoryview(payload)
            while remaining:
                written = os.write(descriptor, remaining)
                if written <= 0:
                    raise OSError("audit append made no forward progress")
                remaining = remaining[written:]
            os.fsync(descriptor)
        finally:
            os.close(descriptor)


@dataclass(frozen=True, slots=True)
class _VerifiedRequest:
    active_projection: dict[str, Any]
    catalog: ResolvedAssistantCatalog
    node_catalog: dict[str, Any]
    field_policy: dict[str, Any]


@dataclass(frozen=True, slots=True)
class _VerifiedSource:
    """The last verified mode projection + node environment (OD-93).

    Keyed by the Headless ETags, which are content digests: Headless answers
    304 while the content is unchanged, so verification and catalog
    resolution run again only when the content changes.
    """

    mode: str
    mode_etag: str | None
    environment_etag: str | None
    active: dict[str, Any]
    node_environment: dict[str, Any]
    catalog_key: bytes | None = None
    catalog: ResolvedAssistantCatalog | None = None
    catalog_wire: dict[str, Any] | None = None


@dataclass(frozen=True, slots=True)
class _ToolCall:
    position: int
    call_id: str
    tool_id: str
    arguments: dict[str, Any]
    raw_arguments_hash: str
    descriptor: dict[str, Any]
    # Object/array arguments the model server handed over as text that does
    # not parse (typed failures, reported before schema validation).
    decode_errors: tuple[FieldError, ...] = ()
    # EXPERIMENTAL (E1): the arguments come from one constrained regeneration.
    regenerated: bool = False
    regenerated_function: dict[str, Any] | None = None


def _projection_content(projection: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in projection.items() if key != "source"}


def _composer_callback_request_id(
    request: ProviderRequest,
    *,
    turn: int,
    call: _ToolCall,
) -> str:
    """Return one stable Composer idempotency key per exact model tool call."""

    material = canonical_json(
        {
            "provider_request_id": request.request_id,
            "request_owner_epoch": request.request_owner_epoch,
            "turn": turn,
            "upstream_tool_call_id": call.call_id,
            "tool_position": call.position,
        }
    )
    return "assistant.callback." + hashlib.sha256(material).hexdigest()


class BridgeService:
    def __init__(
        self,
        config: BridgeConfig,
        *,
        headless_client: HeadlessHttpClient | None = None,
        upstream_client: httpx.AsyncClient | None = None,
        composer_client: httpx.AsyncClient | None = None,
    ) -> None:
        self.config = config
        self._headless = headless_client or HeadlessHttpClient(
            config.headless_api_url,
            timeout_seconds=min(config.callback_timeout_seconds, 5.0),
        )
        self._owns_headless = headless_client is None
        self._reads = HeadlessReadClient(self._headless)
        # The endpoint is chosen per request from the active Deployment's AI
        # model component (projection ``model_endpoint``), never fixed here.
        self._upstream = upstream_client or httpx.AsyncClient(
            timeout=config.upstream_timeout_seconds,
        )
        self._owns_upstream = upstream_client is None
        self._composer = composer_client or httpx.AsyncClient(
            base_url=config.composer_base_url.rstrip("/"),
            timeout=config.callback_timeout_seconds,
        )
        self._owns_composer = composer_client is None
        self._history = SessionHistory(
            maximum_sessions=config.history_maximum_sessions,
            idle_ttl_seconds=config.history_idle_ttl_seconds,
            character_budget=config.history_character_budget,
        )
        self.ledger = RequestLedger(capacity=config.request_capacity)
        self._audit = AuditWriter(
            config.audit_log_path,
            maximum_bytes=config.audit_maximum_bytes,
            keep_files=config.audit_keep_files,
        )
        self._validators: dict[str, Draft202012Validator] = {}
        # One slot, read and replaced only on the event loop (no lock needed);
        # nothing awaits while it is held.
        self._verified_source: _VerifiedSource | None = None

    async def close(self) -> None:
        await self.ledger.close()
        if self._owns_upstream:
            await self._upstream.aclose()
        if self._owns_composer:
            await self._composer.aclose()
        if self._owns_headless:
            self._headless.close()

    async def submit(self, request: ProviderRequest) -> dict[str, Any]:
        request_digest = hashlib.sha256(
            canonical_json(request.model_dump(by_alias=True))
        ).hexdigest()
        return await self.ledger.submit(request, request_digest, self._run)

    async def _verified_source_for(self, mode: str) -> _VerifiedSource:
        """Return the verified active projection pair, re-verifying on change."""

        cached = self._verified_source
        if cached is not None and cached.mode != mode:
            cached = None
        (active_raw, mode_etag), (node_environment, environment_etag) = (
            await asyncio.gather(
                asyncio.to_thread(
                    self._headless.assistant_mode_if_changed,
                    mode,
                    cached.mode_etag if cached is not None else None,
                ),
                asyncio.to_thread(
                    self._headless.node_environment_if_changed,
                    cached.environment_etag if cached is not None else None,
                ),
            )
        )
        if cached is not None and active_raw is None and node_environment is None:
            return cached
        # A 304 only answers a tag this slot sent, so ``cached`` is set here.
        if active_raw is None:
            active_raw = cached.active
        if node_environment is None:
            node_environment = cached.node_environment
        active = verify_active_mode_projection(active_raw, expected_mode=mode)
        self._join_node_environment(active, node_environment)
        verified = _VerifiedSource(
            mode=mode,
            mode_etag=mode_etag,
            environment_etag=environment_etag,
            active=active,
            node_environment=node_environment,
        )
        self._verified_source = verified
        return verified

    async def _verify_request(self, request: ProviderRequest) -> _VerifiedRequest:
        try:
            source = await self._verified_source_for(request.assistant_mode)
            active = source.active
            # ``source`` is the observation envelope (ages, generations and
            # the Manager section digest move on every Manager tick); the
            # projection content outside it is what must match.
            if _projection_content(request.mode_projection) != _projection_content(
                active
            ):
                raise BridgeRunError(
                    "assistant_projection_stale",
                    "forwarded mode projection is not the active headless projection",
                )
            catalog_key = canonical_json(
                {
                    "field_policy": request.field_policy,
                    "requested_tools": list(request.requested_tools),
                }
            )
            if source.catalog_key != catalog_key:
                catalog = resolve_catalog(
                    active,
                    requested_tools=request.requested_tools,
                    field_policy=request.field_policy,
                )
                # Still a verified identity: a newer slot it replaces only
                # costs the next read a full verify.
                source = self._verified_source = replace(
                    source,
                    catalog_key=catalog_key,
                    catalog=catalog,
                    catalog_wire=catalog.to_wire(),
                )
            if (
                request.resolved_catalog.model_dump(by_alias=True)
                != source.catalog_wire
            ):
                raise BridgeRunError(
                    "assistant_catalog_mismatch",
                    "forwarded resolved catalog is not the exact active narrowing",
                )
            node_catalog = source.node_environment.get("catalog")
            if type(node_catalog) is not dict:
                raise BridgeRunError(
                    "assistant_projection_unavailable",
                    "active node environment omits the node catalog",
                )
            return _VerifiedRequest(
                active_projection=active,
                catalog=source.catalog,
                node_catalog=node_catalog,
                field_policy=normalize_field_policy(request.field_policy),
            )
        except AssistantCatalogError as error:
            raise BridgeRunError(error.code, str(error)) from error
        except BridgeRunError:
            raise
        except Exception as error:
            code = getattr(error, "code", "assistant_projection_unavailable")
            raise BridgeRunError(code, str(error)) from error

    @staticmethod
    def _join_node_environment(
        projection: dict[str, Any], node_environment: dict[str, Any]
    ) -> None:
        source = node_environment.get("source")
        if (
            node_environment.get("schema_version")
            != "manyforge.node_environment_projection.v2"
            or type(source) is not dict
            or source.get("available") is not True
            or source.get("freshness") != "fresh"
            or source.get("known_gaps") != []
        ):
            raise BridgeRunError(
                "assistant_projection_unavailable",
                "active node environment is not fresh and complete",
            )
        for field in ("retained_bundle_id", "graph_revision"):
            if node_environment.get(field) != projection[field]:
                raise BridgeRunError(
                    "assistant_projection_stale",
                    "assistant mode and node environment select different bundles",
                )
        package_environment = node_environment.get("package_environment")
        if type(package_environment) is not dict:
            raise BridgeRunError(
                "assistant_projection_unavailable",
                "active node environment omits package provenance",
            )
        for field in (
            "catalog_digest",
            "catalog_id",
            "content_version",
            "environment_digest",
        ):
            if (
                package_environment.get(field)
                != projection["node_environment"][field]
            ):
                raise BridgeRunError(
                    "assistant_projection_stale",
                    "assistant mode and node environment identities disagree",
                )

    async def _run(self, record: RequestRecord) -> dict[str, Any]:
        request = record.request
        started = time.monotonic()
        verified: _VerifiedRequest | None = None
        try:
            if record.cancel_event.is_set():
                raise asyncio.CancelledError
            verified = await self._verify_request(request)
            response = await asyncio.wait_for(
                self._agent_loop(record, verified),
                timeout=self.config.maximum_wall_seconds,
            )
            await self._audit_summary(
                request,
                verified,
                response=response,
                duration_ms=(time.monotonic() - started) * 1_000,
            )
            return response
        except asyncio.CancelledError:
            await self._audit_failure(
                request,
                verified,
                BridgeRunError("cancelled", "assistant request was cancelled"),
                duration_ms=(time.monotonic() - started) * 1_000,
            )
            raise
        except TimeoutError as error:
            typed = BridgeRunError(
                "assistant_timeout", "assistant request exceeded its wall-time bound"
            )
            await self._audit_failure(
                request,
                verified,
                typed,
                duration_ms=(time.monotonic() - started) * 1_000,
            )
            raise typed from error
        except BridgeRunError as error:
            await self._audit_failure(
                request,
                verified,
                error,
                duration_ms=(time.monotonic() - started) * 1_000,
            )
            raise
        except Exception as error:
            typed = BridgeRunError("assistant_request_failed", str(error))
            await self._audit_failure(
                request,
                verified,
                typed,
                duration_ms=(time.monotonic() - started) * 1_000,
            )
            raise typed from error

    async def _agent_loop(
        self, record: RequestRecord, verified: _VerifiedRequest
    ) -> dict[str, Any]:
        request = record.request
        history_key = hashlib.sha256(
            (request.conversation_id + "\0" + verified.catalog.catalog_hash).encode(
                "utf-8"
            )
        ).hexdigest()
        messages = await self._history.get(history_key)
        source_model = await asyncio.to_thread(self._reads.invoke, "source_model", {})
        messages.insert(
            0,
            {"role": "system", "content": self._system_prompt(request, verified, source_model)},
        )
        exchange_start = len(messages)
        messages.append({"role": "user", "content": request.message})
        # With compact_history the saved history carries each completed
        # request as request, answer and edit summary; the running request
        # stays complete.
        compact = verified.active_projection["compact_history"] is True

        async def save_history(answer: str) -> None:
            saved = messages[1:]
            if compact:
                saved = messages[1:exchange_start] + compact_exchange(
                    messages[exchange_start], answer, completed
                )
            await self._history.save(history_key, saved)

        tool_by_id = {row["tool_id"]: row for row in verified.catalog.tools}
        openai_tools = [
            {
                "type": "function",
                "function": {
                    "name": row["tool_id"],
                    "description": row["description"],
                    "parameters": row["input_schema"],
                },
            }
            for row in verified.catalog.tools
        ]
        completed: list[dict[str, Any]] = []
        draft_mutated = False
        draft_revision = request.composer_draft.expected_draft_revision
        admission_generation = (
            request.composer_draft.expected_admission_generation
        )
        # The composite workspace fence advances on every committed edit, so the
        # loop tracks it exactly like the Draft revision and admission
        # generation instead of reusing the first turn's identity.
        workspace_revision = request.composer_draft.expected_workspace_revision
        # Draft diagnostics open at request start; each edit result splits the
        # open diagnostics against them.
        # The selector facts plus the Draft's declared scene resources.
        selector_facts = {
            **(request.composer_draft.selector_facts or {}),
            "scene_resources": [
                row.get("scene_resource_id")
                for row in ((request.composer_draft.authoring or {}).get("scene") or [])
                if isinstance(row, dict)
            ],
        }
        start_reference = [
            _reference_row(row, selector_facts)
            for row in request.composer_draft.reference_diagnostics
            if type(row) is dict
        ]
        diagnostics_state = _DiagnosticsBaseline(
            start=frozenset(
                _diagnostic_key(row)
                for row in [*(request.composer_draft.diagnostics or []), *start_reference]
                if type(row) is dict
            ),
            reference=start_reference,
            selector_facts=selector_facts,
        )
        shapes_shown: set[str] = set()
        recovery = StuckLoopTracker(
            enabled=verified.active_projection["stuck_loop_recovery"] is True
        )

        for turn in range(1, self.config.maximum_turns + 1):
            if record.cancel_event.is_set():
                raise asyncio.CancelledError
            if turn > 1:
                # The first projection was read immediately before this loop.
                # Re-read before later model turns so a bundle/mode change
                # cannot keep an old catalog alive inside one conversation.
                verified = await self._verify_request(request)
                tool_by_id = {row["tool_id"]: row for row in verified.catalog.tools}
                openai_tools = [
                    {
                        "type": "function",
                        "function": {
                            "name": row["tool_id"],
                            "description": row["description"],
                            "parameters": row["input_schema"],
                        },
                    }
                    for row in verified.catalog.tools
                ]
            request_contract = verified.active_projection["model_endpoint"][
                "request_contract"
            ]
            if request_contract["max_output_tokens"] > self.config.maximum_output_tokens:
                raise BridgeRunError(
                    "assistant_provider_configuration_invalid",
                    "retained model output limit exceeds the bridge safety cap",
                )
            if request_contract["default_timeout_s"] > self.config.upstream_timeout_seconds:
                raise BridgeRunError(
                    "assistant_provider_configuration_invalid",
                    "retained model timeout exceeds the bridge transport cap",
                )
            upstream = await self._call_upstream(
                endpoint=verified.active_projection["model_endpoint"],
                messages=messages,
                tools=openai_tools,
                maximum_output_tokens=request_contract["max_output_tokens"],
                timeout_seconds=request_contract["default_timeout_s"],
            )
            message_text, assistant_message, calls = self._parse_upstream(
                upstream, tool_by_id
            )
            messages.append(assistant_message)
            if not calls:
                response = self._response(
                    request=request,
                    catalog=verified.catalog,
                    message=message_text or "The model returned no textual response.",
                    tool_calls=completed,
                    draft_mutated=draft_mutated,
                    needs_clarification=False,
                    exit_reason="ok",
                    turn_count=turn,
                )
                await save_history(response["message"])
                return response

            for call in calls:
                if record.cancel_event.is_set():
                    raise asyncio.CancelledError
                regeneration_failure: str | None = None
                if (verified.active_projection["constrained_regeneration"] is True
                        and regenerates(tool_by_id.get(call.tool_id))):
                    # EXPERIMENTAL (E1): one constrained regeneration of an
                    # invalid Draft edit; the new call is validated below like
                    # any, so the validator enforces what the grammar skipped.
                    try:
                        self._validate_arguments(call, verified)
                    except BridgeRunError as error:
                        if error.code != "assistant_tool_arguments_invalid":
                            raise
                        try:
                            candidate = await regenerate_call(
                                self, call, error.detail, verified=verified,
                                messages=messages, tools=openai_tools,
                                tool_by_id=tool_by_id, request_contract=request_contract,
                            )
                            self._validate_arguments(candidate, verified)
                        except (RegenerationFailed, BridgeRunError) as failure:
                            if isinstance(failure, BridgeRunError) and (
                                    failure.code != "assistant_tool_arguments_invalid"):
                                raise
                            # The original call and its error stand, as with E1 off.
                            regeneration_failure = getattr(failure, "reason", "invalid")
                        else:
                            adopt(messages, candidate)
                            call = candidate
                try:
                    self._validate_arguments(call, verified)
                except BridgeRunError as error:
                    if error.code != "assistant_tool_arguments_invalid":
                        raise
                    shapes = error.detail.get("shape_of")
                    if isinstance(shapes, dict):
                        # Each shape is shown once per request.
                        fresh = {k: v for k, v in shapes.items() if k not in shapes_shown}
                        shapes_shown.update(fresh)
                        if fresh:
                            error.detail["shape_of"] = fresh
                        else:
                            error.detail.pop("shape_of")
                    result = {
                        "success": False,
                        "mutated": False,
                        "draftMutated": False,
                        "result": {
                            "kind": "validation_error",
                            "message": str(error),
                            "detail": deepcopy(error.detail),
                        },
                    }
                else:
                    try:
                        result = await self._execute_callback(
                            request=request,
                            verified=verified,
                            call=call,
                            expected_draft_revision=draft_revision,
                            expected_workspace_revision=workspace_revision,
                            expected_admission_generation=admission_generation,
                            turn=turn,
                        )
                    except BridgeRunError as error:
                        # A failure caused by the call's arguments is a tool
                        # result the model can correct; only infrastructure
                        # failures end the request.
                        if error.code not in _ARGUMENT_FAILURE_CODES:
                            raise
                        result = {
                            "success": False,
                            "mutated": False,
                            "draftMutated": False,
                            "result": {
                                "kind": "validation_error",
                                "message": str(error),
                                "detail": deepcopy(error.detail),
                            },
                        }
                if regeneration_failure and type(result.get("result")) is dict:
                    result = {**result, "result": {
                        **result["result"], "regenerationFailed": regeneration_failure}}
                if call.regenerated and type(result.get("result")) is dict:
                    result = {**result, "result": {**result["result"], "regenerated": True}}
                next_admission_generation = result.get(
                    "nextAdmissionGeneration"
                )
                if type(next_admission_generation) is int:
                    admission_generation = next_admission_generation
                next_workspace_revision = result.get("nextWorkspaceRevision")
                if (
                    type(next_workspace_revision) is int
                    and next_workspace_revision >= workspace_revision
                ):
                    workspace_revision = next_workspace_revision
                mutated = result.get("draftMutated") is True
                draft_mutated = draft_mutated or mutated
                result_payload = result.get("result")
                if mutated and type(result_payload) is dict:
                    next_revision = result_payload.get("draftRevision")
                    if type(next_revision) is int and next_revision > draft_revision:
                        draft_revision = next_revision
                status = "completed" if result.get("success") is True else "failed"
                stuck = (recovery.record(call.tool_id, result) if status == "failed"
                         else recovery.record_success(call.tool_id, call.arguments)
                         if mutated else None)
                if stuck is not None:
                    result = {**result, "recovery": stuck}
                completed_call = {
                    "toolId": call.tool_id,
                    "status": status,
                    "effect": call.descriptor["effect"],
                    "arguments": deepcopy(call.arguments),
                    "result": deepcopy(result_payload),
                    "draftMutated": mutated,
                }
                completed.append(completed_call)
                await self._audit_tool_call(request, verified, call, completed_call, turn)
                tool_message = json.dumps(
                    _model_result(call, result, diagnostics_state),
                    allow_nan=False,
                    ensure_ascii=False,
                    separators=(",", ":"),
                )
                messages.append(
                    {"role": "tool", "tool_call_id": call.call_id, "content": tool_message}
                )
                if stuck is not None and stuck["clarificationRequired"]:
                    response = self._response(
                        request=request,
                        catalog=verified.catalog,
                        message=recovery.clarification_message(stuck),
                        tool_calls=completed,
                        draft_mutated=draft_mutated,
                        needs_clarification=True,
                        exit_reason="needs_clarification",
                        turn_count=turn,
                    )
                    await save_history(response["message"])
                    return response

        response = self._response(
            request=request,
            catalog=verified.catalog,
            message="The assistant reached its bounded turn limit before completing.",
            tool_calls=completed,
            draft_mutated=draft_mutated,
            needs_clarification=True,
            exit_reason="max_turns_exceeded",
            turn_count=self.config.maximum_turns,
        )
        await save_history(response["message"])
        return response

    def _system_prompt(
        self,
        request: ProviderRequest,
        verified: _VerifiedRequest,
        source_model: dict[str, Any] | None,
    ) -> str:
        draft = request.composer_draft
        return render_system_prompt(
            mode=request.assistant_mode,
            program_loaded=draft.program_loaded,
            authoring=draft.authoring,
            diagnostics=draft.diagnostics or [],
            source_model=source_model,
            selector_facts=draft.selector_facts,
            nodes=verified.catalog.nodes,
            catalog=verified.node_catalog,
            skills=verified.catalog.skills,
        )

    async def _call_upstream(
        self,
        *,
        endpoint: dict[str, Any],
        messages: list[dict[str, Any]],
        tools: list[dict[str, Any]],
        maximum_output_tokens: int,
        timeout_seconds: float,
        response_format: dict[str, Any] | None = None,
        thinking: bool = True,
    ) -> dict[str, Any]:
        headers: dict[str, str] = {}
        token_env = endpoint["api_key_env"]
        if token_env is not None:
            token = os.environ.get(token_env)
            if not token:
                raise BridgeRunError(
                    "ai_model_token_unavailable",
                    f"environment variable {token_env} named by the AI model component is not set",
                )
            headers["Authorization"] = f"Bearer {token}"
        body: dict[str, Any] = {
            "model": endpoint["model_id"],
            "messages": messages,
            "max_tokens": maximum_output_tokens,
        }
        # OD-91: only the profile's sampling fields; none keeps the model's
        # or server's defaults.
        body.update(endpoint["request_contract"].get("sampling", {}))
        reasoning = endpoint["request_contract"].get("reasoning")
        if reasoning is not None:
            # EXPERIMENTAL (pending maintainer decision): the common
            # OpenAI-compatible chat-template switch for reasoning (thinking).
            # `thinking=False` (the E1 regeneration) turns it off for that call.
            body["chat_template_kwargs"] = {"enable_thinking": reasoning == "on" and thinking}
        if tools:
            body["tools"] = tools
            # `auto` is the qualified default. Forced tool_choice remains an
            # endpoint capability because some parsers corrupt forced calls.
            body["tool_choice"] = "auto"
        if response_format is not None:
            # E1: the reply is constrained text, not a tool call.
            body["response_format"] = response_format
            body["tool_choice"] = "none"
        try:
            async with self._upstream.stream(
                "POST",
                endpoint["base_url"].rstrip("/") + "/chat/completions",
                json=body,
                headers=headers,
                timeout=timeout_seconds,
            ) as response:
                response.raise_for_status()
                chunks: list[bytes] = []
                size = 0
                async for chunk in response.aiter_bytes():
                    size += len(chunk)
                    if size > MAXIMUM_UPSTREAM_RESPONSE_BYTES:
                        raise BridgeRunError(
                            "upstream_response_oversized",
                            "upstream response exceeds its byte bound",
                        )
                    chunks.append(chunk)
        except httpx.TimeoutException as error:
            raise BridgeRunError("upstream_timeout", "upstream model request timed out") from error
        except httpx.HTTPStatusError as error:
            raise BridgeRunError(
                "upstream_http_error",
                f"upstream model returned HTTP {error.response.status_code}",
            ) from error
        except httpx.HTTPError as error:
            raise BridgeRunError("upstream_unavailable", str(error)) from error
        try:
            value = json.loads(
                b"".join(chunks),
                parse_constant=_reject_nonfinite_json,
                object_pairs_hook=_object_without_duplicates,
            )
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
            raise BridgeRunError("upstream_response_invalid", "upstream response is invalid JSON") from error
        if type(value) is not dict:
            raise BridgeRunError("upstream_response_invalid", "upstream response must be an object")
        return value

    def _parse_upstream(
        self,
        payload: dict[str, Any],
        tool_by_id: dict[str, dict[str, Any]],
    ) -> tuple[str, dict[str, Any], list[_ToolCall]]:
        choices = payload.get("choices")
        if type(choices) is not list or len(choices) != 1 or type(choices[0]) is not dict:
            raise BridgeRunError("upstream_response_invalid", "upstream response must contain one choice")
        raw_message = choices[0].get("message")
        if type(raw_message) is not dict:
            raise BridgeRunError("upstream_response_invalid", "upstream choice lacks a message")
        content = _flatten_text(raw_message.get("content"))
        if len(content.encode("utf-8")) > MAXIMUM_MODEL_TEXT_BYTES:
            raise BridgeRunError(
                "upstream_response_oversized",
                "upstream message text exceeds its byte bound",
            )
        raw_calls = raw_message.get("tool_calls", [])
        if raw_calls is None:
            raw_calls = []
        if type(raw_calls) is not list or len(raw_calls) > MAXIMUM_TOOL_CALLS_PER_TURN:
            raise BridgeRunError("upstream_response_invalid", "upstream tool calls exceed their bound")
        calls: list[_ToolCall] = []
        sanitized_calls: list[dict[str, Any]] = []
        seen_call_ids: set[str] = set()
        for index, raw in enumerate(raw_calls):
            if type(raw) is not dict or raw.get("type") != "function":
                raise BridgeRunError("upstream_response_invalid", "upstream tool call shape is invalid")
            function = raw.get("function")
            call_id = raw.get("id")
            if (
                type(function) is not dict
                or type(call_id) is not str
                or not call_id
                or call_id != call_id.strip()
                or len(call_id.encode("utf-8")) > 256
                or call_id in seen_call_ids
            ):
                raise BridgeRunError("upstream_response_invalid", "upstream tool call is incomplete")
            seen_call_ids.add(call_id)
            name = function.get("name")
            raw_arguments = function.get("arguments")
            if type(name) is not str or type(raw_arguments) is not str:
                raise BridgeRunError("upstream_response_invalid", "upstream function call is malformed")
            descriptor = tool_by_id.get(name)
            argument_hash = hashlib.sha256(raw_arguments.encode("utf-8")).hexdigest()
            unknown_tool: FieldError | None = None
            if descriptor is None:
                # Never executed: the call gets a typed error naming the
                # closest listed tool, so the model can correct it.
                unknown_tool = FieldError(
                    "tool", "one of the listed tools", "unknown_tool",
                    f"unknown tool {name[:128]}; {_tool_suggestion(name, list(tool_by_id))}",
                )
                descriptor = {
                    "argument_types": {},
                    "effect": "read_only",
                    "input_schema": {"type": "object"},
                    "input_schema_hash": "",
                    "tool_id": name[:256],
                }
            if len(raw_arguments.encode("utf-8")) > MAXIMUM_PROVIDER_BODY_BYTES:
                raise BridgeRunError("upstream_response_invalid", "tool arguments exceed their bound")
            try:
                arguments = json.loads(
                    raw_arguments,
                    parse_constant=_reject_nonfinite_json,
                    object_pairs_hook=_object_without_duplicates,
                )
            except (json.JSONDecodeError, ValueError):
                arguments = {"_malformedArguments": raw_arguments[:4_096]}
            if type(arguments) is not dict:
                arguments = {"_nonObjectArguments": arguments}
            decode_errors = (
                (unknown_tool,)
                if unknown_tool is not None
                else _decode_typed_arguments(arguments, descriptor["input_schema"])
            )
            canonical_arguments = json.dumps(
                arguments,
                allow_nan=False,
                ensure_ascii=False,
                separators=(",", ":"),
                sort_keys=True,
            )
            sanitized_calls.append(
                {
                    "id": call_id,
                    "type": "function",
                    "function": {"name": name, "arguments": canonical_arguments},
                }
            )
            calls.append(
                _ToolCall(
                    position=index,
                    call_id=call_id,
                    tool_id=name,
                    arguments=arguments,
                    raw_arguments_hash=argument_hash,
                    descriptor=descriptor,
                    decode_errors=decode_errors,
                )
            )
        assistant_message: dict[str, Any] = {"role": "assistant", "content": content}
        if sanitized_calls:
            assistant_message["tool_calls"] = sanitized_calls
        return content, assistant_message, calls

    def _validator(self, schema: dict[str, Any]) -> Draft202012Validator:
        digest = hashlib.sha256(canonical_json(schema)).hexdigest()
        validator = self._validators.get(digest)
        if validator is None:
            try:
                Draft202012Validator.check_schema(schema)
            except SchemaError as error:
                raise BridgeRunError(
                    "assistant_schema_invalid", "qualified tool schema is unsupported"
                ) from error
            if len(self._validators) >= 4_096:
                self._validators.clear()
            validator = Draft202012Validator(schema)
            self._validators[digest] = validator
        return validator

    def _validate_arguments(self, call: _ToolCall, verified: _VerifiedRequest) -> None:
        """Validate the tool shape, then each node against its own schema.

        Every error of the closest alternative is reported (bounded), each
        with its field path and the expected shape of that field.
        """

        errors: list[FieldError] = list(call.decode_errors)
        if not errors:
            normalize_tree_arguments(call.tool_id, call.arguments)
            errors = placement_errors(call.tool_id, call.arguments)
            placeholders = placeholder_errors(call.descriptor["input_schema"], call.arguments)
            if placeholders:
                errors = placeholders
            schema_errors = [] if placeholders else collect_errors(
                self._validator(call.descriptor["input_schema"]), call.arguments
            )
            if errors:
                # The placement rule replaces the opaque root alternative.
                schema_errors = [
                    row for row in schema_errors
                    if row.path not in ("$", "index", *PLACEMENT_FIELDS)
                ]
            errors += schema_errors
        shape_of: dict[str, str] = {}
        if not errors:
            errors = self._typed_argument_errors(call, verified, shape_of)
        if not errors:
            errors = NodeCallChecker(
                verified.catalog.nodes, verified.node_catalog, self._validator
            ).check(call.tool_id, call.arguments)
        if errors:
            detail = failure_detail(errors)
            if shape_of:
                detail["shape_of"] = shape_of
            raise BridgeRunError(
                "assistant_tool_arguments_invalid",
                failure_text(errors),
                detail=detail,
            )

    def _typed_argument_errors(
        self, call: _ToolCall, verified: _VerifiedRequest, shape_of: dict[str, str]
    ) -> list[FieldError]:
        """Validate each named-type argument against its full type schema.

        `shape_of` receives the compact shape of the nearest enclosing object
        of each error (keyed by its field name, or the type name at the top).
        """

        types = verified.active_projection["types"]
        allowed = verified.field_policy.get("allowedOrientationFields")
        errors: list[FieldError] = []
        for name, type_name in call.descriptor["argument_types"].items():
            if name not in call.arguments:
                continue
            schema = types[type_name.removesuffix("[]")]
            if allowed is not None:
                # The request's orientation field policy narrows the type.
                schema = _narrow_schema(schema, set(allowed))
            validator = self._validator(schema)
            value = call.arguments[name]
            items = (
                [(f"{name}[{index}]", item) for index, item in enumerate(value)]
                if type_name.endswith("[]") and type(value) is list
                else [(name, value)]
            )
            for prefix, item in items:
                found = collect_errors(validator, item, prefix=prefix)
                errors.extend(found)
                for error in found:
                    key, shape = enclosing_shape(schema, type_name.removesuffix("[]"),
                                                 error.path.removeprefix(prefix))
                    shape_of.setdefault(key, shape)
        return errors[:8]

    async def _execute_callback(
        self,
        *,
        request: ProviderRequest,
        verified: _VerifiedRequest,
        call: _ToolCall,
        expected_draft_revision: int,
        expected_workspace_revision: int,
        expected_admission_generation: int,
        turn: int,
    ) -> dict[str, Any]:
        effect = call.descriptor["effect"]
        if effect == "read_only" and call.tool_id not in _COMPOSER_READ_TOOLS:
            return await self._execute_headless_read(request, verified, call)
        if effect not in _COMPOSER_TOOL_EFFECTS and call.tool_id not in _COMPOSER_READ_TOOLS:
            raise BridgeRunError(
                "assistant_tool_effect_rejected", "tool effect is not callback-safe"
            )
        context = request.composer_draft
        projection = verified.active_projection
        callback_request_id = _composer_callback_request_id(
            request,
            turn=turn,
            call=call,
        )
        body = {
            "assistantMode": request.assistant_mode,
            "retainedBundleId": projection["retained_bundle_id"],
            "graphRevision": projection["graph_revision"],
            "nodeEnvironmentDigest": context.node_environment_digest,
            "nodeEnvironmentProjectionDigest": context.node_environment_projection_digest,
            "sourceModelProjectionDigest": context.source_model_projection_digest,
            "assistantProjectionDigest": projection["assistant_projection_digest"],
            "assistantModeProjectionDigest": projection["source"]["source_content_digest"],
            "catalogHash": verified.catalog.catalog_hash,
            "inputSchemaHash": call.descriptor["input_schema_hash"],
            "requestedTools": list(request.requested_tools),
            "fieldPolicy": deepcopy(request.field_policy),
            "draftId": context.draft_id,
            "expectedDraftRevision": expected_draft_revision,
            "expectedWorkspaceEpoch": context.expected_workspace_epoch,
            "expectedWorkspaceRevision": expected_workspace_revision,
            "expectedAdmissionGeneration": expected_admission_generation,
            "requestId": callback_request_id,
            "requestOwnerEpoch": request.request_owner_epoch,
            "sourceLabel": request.source_label,
            "correlationId": request.correlation_id,
            "arguments": deepcopy(call.arguments),
        }
        try:
            async with self._composer.stream(
                "POST",
                f"/api/assistant/bridge/tools/{quote(call.tool_id, safe='')}",
                json=body,
            ) as response:
                chunks: list[bytes] = []
                size = 0
                async for chunk in response.aiter_bytes():
                    size += len(chunk)
                    if size > MAXIMUM_CALLBACK_RESPONSE_BYTES:
                        raise BridgeRunError(
                            "assistant_callback_oversized",
                            "Composer callback response exceeds its byte bound",
                        )
                    chunks.append(chunk)
                response_status = response.status_code
        except httpx.TimeoutException as error:
            raise BridgeRunError("assistant_callback_timeout", "Composer callback timed out") from error
        except BridgeRunError:
            raise
        except httpx.HTTPError as error:
            raise BridgeRunError("assistant_callback_unavailable", str(error)) from error
        try:
            result = json.loads(
                b"".join(chunks),
                parse_constant=_reject_nonfinite_json,
                object_pairs_hook=_object_without_duplicates,
            )
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
            raise BridgeRunError("assistant_callback_invalid", "Composer callback returned invalid JSON") from error
        if response_status >= 400:
            detail = result.get("detail") if type(result) is dict else None
            code = detail.get("code") if type(detail) is dict else None
            message = detail.get("message") if type(detail) is dict else None
            raise BridgeRunError(
                code if type(code) is str else "assistant_callback_rejected",
                message if type(message) is str else "Composer callback rejected the request",
            )
        if type(result) is not dict:
            raise BridgeRunError("assistant_callback_invalid", "Composer callback result must be an object")
        expected_fields = {
            "success",
            "mutated",
            "draftMutated",
            "requestId",
            "requestOwnerEpoch",
            "sourceLabel",
            "correlationId",
            "nextAdmissionGeneration",
            "nextWorkspaceRevision",
            "result",
        }
        if set(result) != expected_fields or any(
            type(result[field]) is not bool
            for field in ("success", "mutated", "draftMutated")
        ):
            raise BridgeRunError(
                "assistant_callback_invalid",
                "Composer callback returned an unsupported result shape",
            )
        for field, expected in (
            ("requestId", callback_request_id),
            ("requestOwnerEpoch", request.request_owner_epoch),
            ("sourceLabel", request.source_label),
            ("correlationId", request.correlation_id),
        ):
            if result[field] != expected:
                raise BridgeRunError(
                    "assistant_callback_invalid",
                    f"Composer callback changed {field}",
                )
        if type(result["result"]) is not dict:
            raise BridgeRunError(
                "assistant_callback_invalid",
                "Composer callback result detail must be an object",
            )
        # A Draft callback takes no Kernel claim, so it echoes the admission
        # generation it was sent, committed or not. It is not a fence: a
        # concurrent Kernel update neither refuses the edit nor shows here.
        # The workspace revision below is what proves a committed edit.
        next_generation = result["nextAdmissionGeneration"]
        if (
            type(next_generation) is not int
            or next_generation != expected_admission_generation
        ):
            raise BridgeRunError(
                "assistant_callback_invalid",
                "Composer callback returned an invalid admission generation",
            )
        if result["success"] is False and (
            result["mutated"] is not False or result["draftMutated"] is not False
        ):
            raise BridgeRunError(
                "assistant_callback_invalid",
                "failed Composer callback claimed a mutation",
            )
        if result["mutated"] != result["draftMutated"]:
            raise BridgeRunError(
                "assistant_callback_invalid",
                "Composer callback mutation flags disagree",
            )
        # A committed edit advances the composite workspace exactly once; a
        # validated no-op callback and a read leave it unchanged.
        next_workspace_revision = result["nextWorkspaceRevision"]
        expected_next_workspace_revision = expected_workspace_revision + (
            1 if result["mutated"] else 0
        )
        if (
            type(next_workspace_revision) is not int
            or not 1 <= next_workspace_revision <= MAXIMUM_SAFE_INTEGER
            or next_workspace_revision != expected_next_workspace_revision
        ):
            raise BridgeRunError(
                "assistant_callback_invalid",
                "Composer callback returned an invalid workspace revision",
            )
        return result

    async def _execute_headless_read(
        self,
        request: ProviderRequest,
        verified: _VerifiedRequest,
        call: _ToolCall,
    ) -> dict[str, Any]:
        del request
        try:
            if call.tool_id in _HEADLESS_SIMPLE_READS:
                operation, arguments = _HEADLESS_SIMPLE_READS[call.tool_id]
                value = await asyncio.to_thread(self._reads.invoke, operation, arguments)
            elif call.tool_id == "history_read":
                limit = call.arguments.get("limit", 32)
                if type(limit) is not int or not 1 <= limit <= 32:
                    raise BridgeRunError(
                        "assistant_tool_arguments_invalid",
                        "history_read.limit must be between 1 and 32",
                    )
                read_args: dict[str, Any] = {"limit": limit}
                cursor = call.arguments.get("cursor")
                if cursor is not None:
                    if type(cursor) is not str or re.fullmatch(r"before:[1-9][0-9]*", cursor) is None:
                        raise BridgeRunError(
                            "assistant_tool_arguments_invalid",
                            "history_read.cursor must be a bridge-issued before:<sequence> cursor",
                        )
                    read_args["before_sequence"] = int(cursor.removeprefix("before:"))
                page = await asyncio.to_thread(self._reads.invoke, "history", read_args)
                next_sequence = page.get("next_before_sequence")
                value = {
                    "schemaVersion": "manyforge.assistant.history_read_result.v1",
                    "page": page,
                    "cursor": (
                        f"before:{next_sequence}" if type(next_sequence) is int else None
                    ),
                }
            elif call.tool_id == "catalog_read":
                environment = await asyncio.to_thread(
                    self._reads.invoke, "node_environment", {}
                )
                value = self._catalog_read(
                    environment,
                    call.arguments,
                    verified.catalog.nodes,
                    verified.active_projection["types"],
                    verified.active_projection["tools"],
                )
            elif call.tool_id == "skills_read":
                selected = set(call.arguments.get("skill_ids", []))
                if call.arguments.get("include_bodies") is True:
                    raise BridgeRunError(
                        "assistant_skill_body_unavailable",
                        "active headless projection does not publish skill bodies",
                    )
                rows = [
                    row
                    for row in verified.catalog.skills
                    if not selected or row["skill_id"] in selected
                ]
                if selected - {row["skill_id"] for row in rows}:
                    raise BridgeRunError(
                        "assistant_tool_arguments_invalid",
                        "skills_read selected an unknown skill",
                    )
                value = {
                    "schemaVersion": "manyforge.assistant.skills_read_result.v1",
                    "skills": deepcopy(rows),
                }
            elif call.tool_id == "inspect_isaac_scene":
                scene = await asyncio.to_thread(
                    self._reads.invoke, "scene", {}
                )
                value = _inspect_isaac_scene_report(scene, call.arguments)
            else:
                raise BridgeRunError(
                    "assistant_tool_owner_unavailable",
                    "read-only tool has no current semantic owner route",
                )
        except HeadlessReadInputError as error:
            raise BridgeRunError("assistant_tool_arguments_invalid", str(error)) from error
        result = {
            "success": True,
            "mutated": False,
            "draftMutated": False,
            "result": {"kind": "headless_read", "details": value},
        }
        if len(canonical_json(result)) > MAXIMUM_CALLBACK_RESPONSE_BYTES:
            raise BridgeRunError(
                "assistant_owner_response_oversized",
                "the result exceeds its model-visible byte bound; read less at once",
            )
        return result

    @staticmethod
    def _catalog_read(
        environment: dict[str, Any],
        arguments: dict[str, Any],
        nodes: list[dict[str, Any]] | tuple = (),
        types: dict[str, Any] | None = None,
        tools: list[dict[str, Any]] | tuple = (),
    ) -> dict[str, Any]:
        catalog = environment.get("catalog")
        if type(catalog) is not dict or type(catalog.get("entries")) is not list:
            raise BridgeRunError("assistant_owner_response_invalid", "node catalog is unavailable")
        selected = set(arguments.get("entry_ids", []))
        include_parameters = arguments.get("include_parameters", False) is True
        type_names = list(arguments.get("types", []))
        described_types: dict[str, Any] = {}
        # Kinds without a schema_types entry (scalars, condition grammars):
        # their generated shape, from the node schemas the validator uses.
        kinds = kind_shapes(catalog, nodes)

        def describe(name: str) -> bool:
            if types is not None and name in types:
                described_types[name] = {
                    "shape": json_shape(types[name], types[name], depth=4)
                }
                if isinstance(types[name].get("description"), str):
                    described_types[name]["description"] = types[name]["description"]
            elif name in catalog.get("schema_types", {}):
                described_types[name] = deepcopy(catalog["schema_types"][name])
            elif name in kinds:
                shapes = kinds[name]
                described_types[name] = (
                    {"shape": shapes[0]} if len(shapes) == 1 else {"shapes": shapes}
                )
            else:
                return False
            return True

        for name in type_names:
            if not describe(name):
                raise BridgeRunError(
                    "assistant_tool_arguments_invalid",
                    f"catalog_read: unknown type {name}",
                    detail={"code": "assistant_tool_arguments_invalid", "path": "types",
                            "expected": "one of: " + ", ".join(sorted(
                                [*(types or {}), *catalog.get("schema_types", {}), *kinds]))},
                )
        only_types = bool(type_names) and not (
            selected or "category" in arguments or "kind" in arguments
        )
        rows, schema_types = ([], {}) if only_types else catalog_rows(
            catalog,
            entry_ids=selected,
            category=arguments.get("category"),
            kind=arguments.get("kind"),
            include_parameters=include_parameters,
        )
        entries = {
            row.get("id"): row for row in catalog["entries"] if isinstance(row, dict)
        }
        tool_rows = {row.get("tool_id"): row for row in tools if isinstance(row, dict)}
        # A type or tool name in entry_ids is answered (node ids, type names and
        # tool ids never collide), with a note naming the right argument.
        notes: list[str] = []
        for name in sorted(selected - {row.get("id") for row in rows} - set(entries)):
            if describe(name):
                notes.append(f"{name} is a type; next time use types: [{name}]")
            elif name in tool_rows:
                typed = tool_rows[name].get("argument_types") or {}
                named = sorted({kind.removesuffix("[]") for kind in typed.values()})
                for type_name in named:
                    describe(type_name)
                schema = tool_rows[name].get("input_schema") or {}
                notes.append(
                    f"{name} is a tool; its arguments: " + (", ".join(
                        f"{argument} (type {kind})" for argument, kind in typed.items())
                        + f"; their types are under types (next time use types: [{', '.join(named)}])"
                        if typed else str(json_shape(schema, schema))))
            else:
                continue
            selected.discard(name)
        unknown = sorted(selected - {row.get("id") for row in rows})
        if unknown:

            def unknown_message(name: str) -> str:
                entry = entries.get(name)
                if entry is not None:
                    # A real entry the kind or category filter left out.
                    for field in ("kind", "category"):
                        wanted = arguments.get(field)
                        if wanted is not None and entry.get(field) != wanted:
                            return (f"{name} is a {entry.get(field)}, excluded by "
                                    f"{field}: {wanted}")
                return f"not a node id: {name}; node ids are listed in the system prompt"

            messages = [unknown_message(name) for name in unknown]
            raise BridgeRunError(
                "assistant_tool_arguments_invalid",
                "; ".join(messages),
                detail={
                    "code": "assistant_tool_arguments_invalid",
                    "path": "entry_ids",
                    "expected": "node ids from the system prompt's node list",
                    "errors": [{"path": "entry_ids", "reason": "unknown_entry",
                                "expected": "node ids from the system prompt's node list",
                                "message": message} for message in messages],
                },
            )
        result = {
            "schemaVersion": "manyforge.assistant.catalog_read_result.v2",
            "retainedBundleId": environment["retained_bundle_id"],
            "environmentDigest": environment["environment_digest"],
            "entries": rows,
        }
        if described_types:
            result["types"] = described_types
        if notes:
            result["note"] = "; ".join(notes)
        schemas = {row["node_id"]: row["node_schema"] for row in nodes}
        for row in rows:
            schema = schemas.get(row.get("id"))
            if schema is not None:
                # The node object as the tree tools take it; value_sources is
                # named once instead of per parameter.
                properties = dict(schema.get("properties", {}))
                if "value_sources" in properties:
                    properties["value_sources"] = {
                        "enum": ["{<value_source_parameter>: value source}"]
                    }
                row["insert_as"] = json_shape(
                    {**schema, "properties": properties}, schema, depth=2
                )
        if include_parameters:
            # Value types in the catalog's own notation: required_when and
            # allowed_when name the fields a kind needs or permits.
            result["schemaTypes"] = schema_types
            sources = {row["node_id"]: value_source_ids(row["node_schema"]) for row in nodes}
            for row in rows:
                if row.get("id") in sources:
                    row["value_source_parameters"] = sources[row["id"]]
            result["valueSources"] = (
                "Optional node field value_sources binds a parameter to a Program "
                "parameter or blackboard key instead of a literal: {<parameter>: "
                + VALUE_SOURCE_SHAPE
                + "}; only the entry's value_source_parameters accept one."
            )
        return result

    @staticmethod
    def _response(
        *,
        request: ProviderRequest,
        catalog: ResolvedAssistantCatalog,
        message: str,
        tool_calls: list[dict[str, Any]],
        draft_mutated: bool,
        needs_clarification: bool,
        exit_reason: str,
        turn_count: int,
    ) -> dict[str, Any]:
        return {
            "schemaVersion": PROVIDER_RESPONSE_VERSION,
            "requestId": request.request_id,
            "requestOwnerEpoch": request.request_owner_epoch,
            "sourceLabel": request.source_label,
            "correlationId": request.correlation_id,
            "conversationId": request.conversation_id,
            "assistantMode": request.assistant_mode,
            "catalogHash": catalog.catalog_hash,
            "message": message,
            "toolCalls": tool_calls,
            "warnings": [],
            "requiresReview": True,
            "mutated": False,
            "draftMutated": draft_mutated,
            "needsClarification": needs_clarification,
            "exitReason": exit_reason,
            "turnCount": turn_count,
        }

    async def _audit_tool_call(
        self,
        request: ProviderRequest,
        verified: _VerifiedRequest,
        call: _ToolCall,
        completed: dict[str, Any],
        turn: int,
    ) -> None:
        await self._audit.write(
            {
                "kind": "tool_call",
                "timestampNs": time.time_ns(),
                "requestId": request.request_id,
                "requestOwnerEpoch": request.request_owner_epoch,
                "sourceLabel": request.source_label,
                "correlationId": request.correlation_id,
                "assistantMode": request.assistant_mode,
                "catalogHash": verified.catalog.catalog_hash,
                "tool": call.tool_id,
                "effect": call.descriptor["effect"],
                "inputSchemaHash": call.descriptor["input_schema_hash"],
                "argumentsHash": call.raw_arguments_hash,
                "status": completed["status"],
                "draftMutated": completed["draftMutated"],
                "turnIndex": turn,
                **({"regenerated": True} if call.regenerated else {}),
            }
        )

    async def _audit_summary(
        self,
        request: ProviderRequest,
        verified: _VerifiedRequest,
        *,
        response: dict[str, Any],
        duration_ms: float,
    ) -> None:
        projection = verified.active_projection
        await self._audit.write(
            {
                "kind": "request",
                "timestampNs": time.time_ns(),
                "requestId": request.request_id,
                "requestOwnerEpoch": request.request_owner_epoch,
                "sourceLabel": request.source_label,
                "correlationId": request.correlation_id,
                "assistantMode": request.assistant_mode,
                "assistantComponentId": projection["assistant_component_id"],
                "model": projection["model_endpoint"]["model_id"],
                "retainedBundleId": projection["retained_bundle_id"],
                "graphRevision": projection["graph_revision"],
                "environmentDigest": projection["node_environment"]["environment_digest"],
                "assistantProjectionDigest": projection["assistant_projection_digest"],
                "assistantModeProjectionDigest": projection["source"]["source_content_digest"],
                "catalogHash": verified.catalog.catalog_hash,
                "requestedTools": list(request.requested_tools),
                "resolvedTools": [row["tool_id"] for row in verified.catalog.tools],
                "fieldPolicy": deepcopy(request.field_policy),
                "toolCalls": [row["toolId"] for row in response["toolCalls"]],
                "turnCount": response["turnCount"],
                "durationMs": round(duration_ms, 3),
                "exitReason": response["exitReason"],
            }
        )

    async def _audit_failure(
        self,
        request: ProviderRequest,
        verified: _VerifiedRequest | None,
        error: BridgeRunError,
        *,
        duration_ms: float,
    ) -> None:
        forwarded_source = request.mode_projection.get("source")
        record: dict[str, Any] = {
            "kind": "request",
            "timestampNs": time.time_ns(),
            "requestId": request.request_id,
            "requestOwnerEpoch": request.request_owner_epoch,
            "sourceLabel": request.source_label,
            "correlationId": request.correlation_id,
            "assistantMode": request.assistant_mode,
            "forwardedCatalogHash": request.resolved_catalog.catalog_hash,
            "forwardedAssistantModeProjectionDigest": (
                forwarded_source.get("source_content_digest")
                if type(forwarded_source) is dict
                else None
            ),
            "durationMs": round(duration_ms, 3),
            "exitReason": error.code,
        }
        if verified is not None:
            projection = verified.active_projection
            record.update(
                {
                    "assistantComponentId": projection["assistant_component_id"],
                    "model": projection["model_endpoint"]["model_id"],
                    "retainedBundleId": projection["retained_bundle_id"],
                    "graphRevision": projection["graph_revision"],
                    "environmentDigest": projection["node_environment"][
                        "environment_digest"
                    ],
                    "assistantProjectionDigest": projection[
                        "assistant_projection_digest"
                    ],
                    "assistantModeProjectionDigest": projection["source"][
                        "source_content_digest"
                    ],
                    "catalogHash": verified.catalog.catalog_hash,
                }
            )
        if error.audit_detail is not None:
            record["detail"] = deepcopy(error.audit_detail)
        await self._audit.write(record)


def _tool_suggestion(name: str, tool_ids: list[str]) -> str:
    """The listed tools of the unknown name's family (its prefix before the
    verb, e.g. tree_draft_*), else the closest name by edit distance."""

    parts = name.split("_")
    for length in range(len(parts) - 1, 0, -1):
        prefix = "_".join(parts[:length]) + "_"
        family = sorted(tool for tool in tool_ids if tool.startswith(prefix))
        if family:
            return f"listed {prefix}* tools: " + ", ".join(family)
    closest = difflib.get_close_matches(name, tool_ids, n=1, cutoff=0.0)
    return f"closest: {closest[0]}" if closest else "see the tool list"


def _diagnostic_key(row: dict[str, Any]) -> tuple[str, ...]:
    key = (str(row.get("code")), str(row.get("path")), str(row.get("message")))
    return (*key, "reference") if row.get("reference") is True else key


@dataclass(slots=True)
class _DiagnosticsBaseline:
    """The Draft diagnostics open at request start, and the latest reference rows."""

    start: frozenset[tuple[str, ...]]
    reference: list[dict[str, Any]]
    selector_facts: Mapping[str, Any] | None = None


_MAXIMUM_ALTERNATIVES: Final = 10


def _names(values: Sequence[str]) -> str:
    shown = list(dict.fromkeys(values))
    text = ", ".join(shown[:_MAXIMUM_ALTERNATIVES])
    extra = len(shown) - _MAXIMUM_ALTERNATIVES
    return text + (f" +{extra} more" if extra > 0 else "")


def _valid_alternatives(code: str, payload: Mapping[str, Any], facts: Mapping[str, Any] | None) -> str | None:
    """The valid ids a reference diagnostic's wrong id could have been."""

    if not isinstance(facts, Mapping):
        return None
    component = payload.get("component_id")

    def rows(kind: str) -> list[Mapping[str, Any]]:
        return [row for row in facts.get(kind) or [] if isinstance(row, Mapping)]

    def of(row: Mapping[str, Any], key: str) -> Mapping[str, Any]:
        value = row.get(key)
        return value if isinstance(value, Mapping) else {}

    if code == "scene_resource_missing":
        declared = [str(name) for name in facts.get("scene_resources") or []]
        return ((f"declared scene resources: {_names(declared)}; " if declared else "")
                + "declare it with scene_draft_add_object (a declared resource exists at run start)")
    if "capability_id" in payload:
        capability = payload.get("capability_id")
        providers = [
            f"{of(row, 'target').get('component_id')}/{of(row, 'target').get('command_target_id')}"
            for row in rows("targets")
            if any(isinstance(entry, Mapping) and entry.get("capability_id") == capability
                   and entry.get("available") is True for entry in row.get("capabilities") or [])
        ]
        return (f"{capability} is provided by: {_names(providers)}" if providers
                else f"no command target provides {capability}")
    if code == "component_reference_missing":
        text = "components: " + _names([str(row.get("component_id")) for row in rows("components")])
        # The wrong id may be a member's name: say whose and how to address it.
        members = [
            (kind, field, key, row)
            for kind, field, key in (("signals", "signal_name", "reference"),
                                     ("targets", "command_target_id", "target"),
                                     ("frames", "frame_id", "reference"),
                                     ("bodies", "collision_body_id", "reference"))
            for row in rows(kind)
            if of(row, key).get(field) == component
        ]
        if members:
            kind, field, key, row = members[0]
            owner = of(row, key).get("component_id")
            what = {"signals": "signal", "targets": "command target", "frames": "frame",
                    "bodies": "collision body"}[kind]
            direction = f" ({row.get('direction')})" if kind == "signals" and row.get("direction") else ""
            text = (f"{component} is a {what} of {owner}{direction}: use component_id {owner}, "
                    f"{field} {component}; {text}")
        if "frame" in str(component) or "world" in str(component):
            text += "; for the cell frame use frame_ref kind cell_frame"
        return text
    if "frame_id" in payload:
        frames = [str(of(row, "reference").get("frame_id")) for row in rows("frames")
                  if of(row, "reference").get("component_id") == component]
        return f"frames of {component}: {_names(frames)}" if frames else None
    if "signal_name" in payload:
        signals = [f"{of(row, 'reference').get('signal_name')} ({row.get('direction')})"
                   for row in rows("signals") if of(row, "reference").get("component_id") == component]
        return f"signals of {component}: {_names(signals)}" if signals else None
    if "collision_body_id" in payload:
        bodies = [str(of(row, "reference").get("collision_body_id")) for row in rows("bodies")
                  if of(row, "reference").get("component_id") == component]
        return f"collision bodies of {component}: {_names(bodies)}" if bodies else None
    if "command_target_id" in payload:
        targets = [str(of(row, "target").get("command_target_id")) for row in rows("targets")
                   if of(row, "target").get("component_id") == component]
        return f"{component} has command targets: {_names(targets)}" if targets else None
    return None


def _reference_row(
    row: dict[str, Any], selector_facts: Mapping[str, Any] | None = None
) -> dict[str, Any]:
    """A typed reference diagnostic in the authoring diagnostics' shape; the
    message names the valid alternatives from the candidate's selector facts."""

    payload = row.get("payload") or {}
    facts = ", ".join(f"{key}={value}" for key, value in payload.items())
    message = f"{row.get('code')} ({facts})" if facts else str(row.get("code"))
    alternatives = _valid_alternatives(str(row.get("code")), payload, selector_facts)
    if alternatives:
        message += f"; {alternatives}"
    view = {
        "code": row.get("code"),
        "path": (row.get("location") or {}).get("path"),
        "message": message,
        "reference": True,
    }
    if isinstance(row.get("node"), Mapping):
        view["node"] = row["node"]
    return view


_ONE_CHILD: Final = re.compile(r"(?P<id>[a-z][a-z0-9_.-]*) accepts at most 1 children$")


def _with_capacity_hint(row: Any) -> Any:
    """A one-child node refusing more children names the sequence pattern."""

    match = _ONE_CHILD.search(str(row.get("message", ""))) if isinstance(row, dict) else None
    if match is None:
        return row
    return {**row, "message": row["message"] + (
        f"; to run several steps under {match['id']}, put them in a sequence and make "
        "that its child")}


def details_placed(payload: Mapping[str, Any]) -> str | None:
    """The node an insert or replace just placed, from its result details."""

    details = payload.get("details") if isinstance(payload.get("details"), Mapping) else {}
    placed = details.get("inserted_instance_id")
    return placed if isinstance(placed, str) else None


def _diagnostic_text(row: Mapping[str, Any]) -> str:
    """`id (node_id): field: message [raw path]` when the path names a node."""

    node = row.get("node")
    if isinstance(node, Mapping) and node.get("instance_id"):
        where = f"{node.get('instance_id')} ({node.get('node_id')})"
        if node.get("path"):
            where += f": {node.get('path')}"
        return f"{where}: {row.get('message')} [{row.get('path')}]"
    return f"{row.get('path')}: {row.get('message')}"


def _find_subtree(forest: Any, instance_id: str) -> dict[str, Any] | None:
    stack = list(forest) if type(forest) is list else []
    while stack:
        node = stack.pop()
        if type(node) is not dict:
            continue
        if node.get("instance_id") == instance_id:
            return node
        stack.extend(node.get("children") or [])
    return None


def _capabilities_view(environment: dict[str, Any]) -> dict[str, Any]:
    """What a model needs from the node environment to pick nodes.

    Per node: available or not, with the typed reasons; the capabilities the
    cell is known to offer (required by an available node) and those reported
    missing. Catalog, manifest, digests and implementation internals stay in
    the audit.
    """

    availability = environment.get("availability") or {}
    rows: dict[str, list[dict[str, Any]]] = {}
    for row in availability.get("implementations") or []:
        rows.setdefault(row["node_id"], []).append(row)
    required = {
        entry.get("id"): {item["capability_id"] for item in entry.get("required_capabilities") or []}
        for entry in (environment.get("catalog") or {}).get("entries") or []
    }
    available: list[str] = []
    unavailable: dict[str, list[str]] = {}
    offered: set[str] = set()
    missing: set[str] = set()
    for node_id, implementations in sorted(rows.items()):
        if any(row["execution_available"] for row in implementations):
            available.append(node_id)
            offered |= required.get(node_id, set())
            continue
        reasons: list[str] = []
        for row in implementations:
            detailed = {reason.get("code"): reason.get("details") or {}
                        for reason in row.get("reasons") or []}
            for code in row.get("reason_codes") or []:
                values = [str(value) for value in detailed.get(code, {}).values()]
                reasons.append(f"{code} ({', '.join(values)})" if values else code)
                if code == "capability_not_offered":
                    missing.update(values)
        unavailable[node_id] = sorted(set(reasons))
    return {
        "execution_available": availability.get("execution_available"),
        "reason_codes": availability.get("reason_codes") or [],
        "nodes_available": available,
        "nodes_unavailable": unavailable,
        "capabilities_offered": sorted(offered),
        "capabilities_not_offered": sorted(missing),
        "more": ("catalog_read for node parameters; the system prompt's Cell section "
                 "for targets, frames and signals"),
    }


def _model_result(
    call: _ToolCall, result: dict[str, Any], diagnostics_state: "_DiagnosticsBaseline"
) -> dict[str, Any]:
    """The concise generated tool result the model sees.

    `{ok, changed, diagnostics_new, diagnostics_other, errors}` for edits;
    read results keep their
    content. Request ids, epochs, generations and revisions stay in the
    bridge (they are in the request status and the audit).
    """

    payload = result.get("result") if type(result.get("result")) is dict else {}
    view: dict[str, Any] = {"ok": result.get("success") is True}
    if not view["ok"]:
        detail = payload.get("detail") if type(payload.get("detail")) is dict else {}
        errors = detail.get("errors")
        if type(errors) is not list:
            errors = [{key: value for key, value in (
                ("code", detail.get("code") or payload.get("kind")),
                ("path", detail.get("path")),
                ("message", payload.get("message")),
            ) if value is not None}]
        view["errors"] = [_with_capacity_hint(row) for row in errors]
        if isinstance(detail.get("shape_of"), dict):
            view["shape_of"] = detail["shape_of"]
    elif call.tool_id == "deployment_capabilities_read":
        view.update(_capabilities_view(payload.get("details") or {}))
    elif payload.get("kind") == "headless_read":
        view.update(payload.get("details") or {})
    elif call.tool_id == "program_read":
        details = payload.get("details") or {}
        authoring = details.get("authoring") or {}
        instance_id = call.arguments.get("instance_id")
        if instance_id is not None:
            node = _find_subtree(authoring.get("forest"), instance_id)
            view["node"] = node
            if node is None:
                view = {"ok": False, "errors": [{"code": "assistant_node_not_found",
                                                 "path": "instance_id",
                                                 "message": f"no node {instance_id} in the Draft"}]}
        else:
            view["program"] = authoring or details.get("program")
    elif call.tool_id == "program_validate":
        view.update(payload.get("details") or {})
    elif call.tool_id == "scene_inspect":
        view["scene_resources"] = (payload.get("details") or {}).get("scene", [])
    else:
        view["changed"] = payload.get("details")
        details = payload.get("details") if type(payload.get("details")) is dict else {}
        if (call.tool_id == "tree_draft_insert_node"
                and call.arguments.get("parent_instance_id") == "@root"
                and isinstance(details.get("parent_instance_id"), str)):
            # @root names the root node itself: say where the node went and
            # how to place it elsewhere.
            position = ("last child" if "index" not in call.arguments
                        else f"child {details.get('index')}")
            view["hint"] = (f"inserted as {position} of {details['parent_instance_id']}; "
                            "to put it elsewhere use before_instance_id, after_instance_id or index")
        anchor = call.arguments.get("before_instance_id") or call.arguments.get("after_instance_id")
        placed = details.get("inserted_instance_id") or details.get("moved_instance_id")
        parent_key = ("parent_instance_id" if "inserted_instance_id" in details
                      else "new_parent_instance_id")
        if (isinstance(anchor, str) and isinstance(placed, str)
                and parent_key in details and details[parent_key] is None):
            # Placed beside a top-level node: the Draft now has two roots.
            view["hint"] = (
                f"{placed} is now a second top-level node beside the root {anchor}; "
                f"to run it first in {anchor} use parent_instance_id {anchor} with index 0, "
                "or to put it above the root, wrap @root")
    diagnostics = payload.get("diagnostics")
    if view["ok"] and type(diagnostics) is list:
        rows = [row for row in diagnostics if type(row) is dict]
        # A committed edit carries the reference diagnostics of its Draft;
        # without them the last known reference rows still hold.
        reference = payload.get("referenceDiagnostics")
        if isinstance(payload.get("sceneResourceIds"), list):
            diagnostics_state.selector_facts = {
                **(diagnostics_state.selector_facts or {}),
                "scene_resources": payload["sceneResourceIds"],
            }
        if type(reference) is list:
            diagnostics_state.reference = [
                _reference_row(row, diagnostics_state.selector_facts)
                for row in reference if type(row) is dict
            ]
        rows += diagnostics_state.reference
        # Split against the Draft at request start: what this request
        # introduced, and a count (with a few short paths) of what was open.
        fresh = [row for row in rows if _diagnostic_key(row) not in diagnostics_state.start]
        other = [row for row in rows if _diagnostic_key(row) in diagnostics_state.start]
        built = details_placed(payload)
        view["diagnostics_new"] = [
            _diagnostic_text(row) + (
                f"; expected while building: insert its children next with "
                f"parent_instance_id {built}"
                if built and row.get("code") == "authoring_node_children_incomplete"
                and (row.get("node") or {}).get("instance_id") == built else "")
            for row in fresh
        ]
        view["diagnostics_other"] = len(other)
        paths = [str(row.get("path")) for row in other[:_OTHER_DIAGNOSTIC_PATHS]]
        if other and all(len(path) <= _SHORT_PATH for path in paths):
            view["diagnostics_other_at"] = paths
    if "recovery" in result:
        view["recovery"] = result["recovery"]
    return view


_JSON_QUOTE: Final = 20
_OTHER_DIAGNOSTIC_PATHS: Final = 3
_SHORT_PATH: Final = 60


def _json_parse_message(name: str, kind: str, text: str, error: ValueError) -> str:
    """Where the JSON text of an argument breaks, quoted so it can be fixed."""

    if not isinstance(error, json.JSONDecodeError):
        return f"the {name} {kind} is not valid JSON: {error}"[:200]
    position = error.pos
    if error.msg == "Extra data":
        trailing = text[position:position + _JSON_QUOTE]
        return (f"the {name} {kind} is complete at char {position}; remove the "
                f"trailing text '{trailing}'")
    half = _JSON_QUOTE // 2
    excerpt = text[max(0, position - half):position] + "<HERE>" + text[position:position + half]
    return (f"the {name} {kind} is not valid JSON: {error.msg} at char {position}: "
            f"'{excerpt}'")


def _decode_typed_arguments(
    arguments: dict[str, Any], schema: dict[str, Any]
) -> tuple[FieldError, ...]:
    """Decode object/array arguments the model server passed through as text.

    Some model servers' tool parsers return an argument whose JSON value does not parse
    as the raw string. A declared object or array argument given as a JSON
    string is decoded here, in one place; text that does not parse is a
    typed failure naming the argument and the parse position.
    """

    errors: list[FieldError] = []
    for name, declared in schema.get("properties", {}).items():
        value = arguments.get(name)
        kind = declared.get("type") if isinstance(declared, dict) else None
        if kind not in ("object", "array") or type(value) is not str:
            continue
        try:
            decoded = json.loads(
                value,
                parse_constant=_reject_nonfinite_json,
                object_pairs_hook=_object_without_duplicates,
            )
        except (json.JSONDecodeError, ValueError) as error:
            errors.append(FieldError(name, kind, "json_parse",
                                     _json_parse_message(name, kind, value, error)))
            continue
        if (kind == "object") == (type(decoded) is dict) and (
            kind == "object" or type(decoded) is list
        ):
            arguments[name] = decoded
    return tuple(errors)


def _flatten_text(value: Any) -> str:
    if type(value) is str:
        return value
    if type(value) is dict:
        return value.get("text") if type(value.get("text")) is str else ""
    if type(value) is list:
        parts: list[str] = []
        for item in value:
            if type(item) is str:
                parts.append(item)
            elif type(item) is dict and item.get("type") in {None, "text"}:
                item_text = item.get("text")
                if type(item_text) is str:
                    parts.append(item_text)
        return "".join(parts)
    return ""


def _reject_nonfinite_json(value: str) -> None:
    raise ValueError(f"non-finite JSON constant is unsupported: {value}")


def _object_without_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key is unsupported: {key}")
        result[key] = value
    return result


def _http_error(error: Exception, status_code: int = 400) -> HTTPException:
    return HTTPException(
        status_code=status_code,
        detail={
            "code": getattr(error, "code", "assistant_bridge_error"),
            "message": str(error),
        },
    )


def create_app(config: BridgeConfig, *, service: BridgeService | None = None) -> FastAPI:
    bridge = service or BridgeService(config)

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        yield
        await bridge.close()

    app = FastAPI(title="ManyForge assistant bridge", lifespan=lifespan)
    app.add_middleware(_BodyLimitMiddleware, maximum_bytes=MAXIMUM_PROVIDER_BODY_BYTES)
    app.state.bridge_service = bridge

    @app.get("/health")
    async def health() -> dict[str, Any]:
        return {
            "schemaVersion": "manyforge.assistant.bridge_health.v2",
            "status": "ready",
            "activity": "busy" if bridge.ledger.busy() else "idle",
        }

    @app.post("/v1/manyforge/assistant", status_code=202)
    async def submit(request: ProviderRequest) -> dict[str, Any]:
        try:
            return await bridge.submit(request)
        except BridgeLedgerError as error:
            raise _http_error(error, 409) from error
        except AssistantCatalogError as error:
            raise _http_error(error, 400) from error

    @app.get("/v1/manyforge/assistant/{request_id}")
    async def status(
        request_id: str = ApiPath(min_length=1, max_length=256),
    ) -> dict[str, Any]:
        if _REQUEST_ID_PATTERN.fullmatch(request_id) is None:
            raise _http_error(BridgeLedgerError("assistant_request_invalid", "requestId is invalid"))
        try:
            return await bridge.ledger.status(request_id)
        except BridgeLedgerError as error:
            raise _http_error(error, 404) from error

    @app.post("/v1/manyforge/assistant/{request_id}/cancel")
    async def cancel(
        request_id: str = ApiPath(min_length=1, max_length=256),
    ) -> dict[str, Any]:
        if _REQUEST_ID_PATTERN.fullmatch(request_id) is None:
            raise _http_error(BridgeLedgerError("assistant_request_invalid", "requestId is invalid"))
        try:
            return await bridge.ledger.cancel(request_id)
        except BridgeLedgerError as error:
            raise _http_error(error, 404) from error

    return app


def _config_from_environment() -> BridgeConfig:
    state_root = Path(
        os.environ.get(
            "XDG_STATE_HOME", str(Path.home() / ".local" / "state")
        )
    )
    return BridgeConfig(
        headless_api_url=os.environ.get(
            "MANYFORGE_HEADLESS_API_URL", "http://127.0.0.1:9010"
        ),
        composer_base_url=os.environ.get(
            "MANYFORGE_COMPOSER_URL", "http://127.0.0.1:9000"
        ),
        audit_log_path=Path(
            os.environ.get(
                "MANYFORGE_ASSISTANT_AUDIT_LOG",
                str(state_root / "manyforge" / "assistant-bridge-audit.jsonl"),
            )
        ),
    )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m manyforge_assistant_bridge.bridge")
    parser.add_argument(
        "--fd",
        type=int,
        required=True,
        help="inherited listening socket to serve on (the assistant host binds it)",
    )
    arguments = parser.parse_args(argv)
    uvicorn.run(create_app(_config_from_environment()), fd=arguments.fd)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
