"""Composer-owned, revision-fenced Deployment document Draft."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import json
from pathlib import Path
import threading
from typing import Any, Callable, NoReturn, Sequence
import uuid


_MAX_DOCUMENT_BYTES = 16 * 1024 * 1024
_MAX_PATH_DEPTH = 32
_MAX_COLLECTION_INDEX = 100_000
_RETAINED_SCHEMA = "manyforge.composer_deployment_draft_state.v1"


class DeploymentDraftError(RuntimeError):
    def __init__(self, code: str, message: str, *, status_code: int = 400) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code


def _fail(code: str, message: str, *, status_code: int = 400) -> NoReturn:
    raise DeploymentDraftError(code, message, status_code=status_code)


@dataclass(frozen=True, slots=True)
class CompiledDeploymentDocument:
    document: dict[str, Any]
    canonical_json: str
    deployment_artifact_id: str
    deployment_revision: str
    graph_revision: str


DeploymentCompiler = Callable[[str], CompiledDeploymentDocument]


def compile_deployment_document(source: str) -> CompiledDeploymentDocument:
    """Call the one C++ deployment compiler without recreating its schema."""

    try:
        import manyforge_headless_py  # type: ignore[import-not-found]

        result = manyforge_headless_py.compile_deployment_draft(source)
    except ImportError:
        _fail(
            "deployment_compiler_unavailable",
            "the build-qualified Deployment compiler is unavailable",
            status_code=503,
        )
    except Exception as error:
        _fail("deployment_draft_invalid", str(error))
    expected = {
        "canonical_json",
        "deployment_artifact_id",
        "deployment_revision",
        "graph_revision",
    }
    if not isinstance(result, dict) or set(result) != expected:
        _fail(
            "deployment_compiler_response_invalid",
            "the Deployment compiler returned an invalid result",
            status_code=502,
        )
    canonical = result["canonical_json"]
    if not isinstance(canonical, str) or len(canonical.encode("utf-8")) > _MAX_DOCUMENT_BYTES:
        _fail(
            "deployment_compiler_response_invalid",
            "the Deployment compiler returned invalid canonical bytes",
            status_code=502,
        )
    try:
        document = json.loads(canonical)
    except (TypeError, ValueError) as error:
        _fail(
            "deployment_compiler_response_invalid",
            f"the Deployment compiler returned invalid JSON: {error}",
            status_code=502,
        )
    if not isinstance(document, dict):
        _fail(
            "deployment_compiler_response_invalid",
            "the compiled Deployment root is not an object",
            status_code=502,
        )
    identities = (
        result["deployment_artifact_id"],
        result["deployment_revision"],
        result["graph_revision"],
    )
    if not all(isinstance(value, str) and value for value in identities):
        _fail(
            "deployment_compiler_response_invalid",
            "the Deployment compiler omitted an identity",
            status_code=502,
        )
    return CompiledDeploymentDocument(
        document=document,
        canonical_json=canonical,
        deployment_artifact_id=identities[0],
        deployment_revision=identities[1],
        graph_revision=identities[2],
    )


def _source(document: dict[str, Any]) -> str:
    try:
        source = json.dumps(
            document,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
        )
    except (TypeError, ValueError) as error:
        _fail("deployment_draft_invalid", f"Deployment value is invalid: {error}")
    if len(source.encode("utf-8")) > _MAX_DOCUMENT_BYTES:
        _fail(
            "deployment_draft_oversized",
            "Deployment Draft exceeds the artifact byte bound",
            status_code=413,
        )
    return source


def _edit_at_path(
    document: dict[str, Any],
    path: Sequence[str | int],
    value: Any,
    operation: str,
) -> dict[str, Any]:
    if not path or len(path) > _MAX_PATH_DEPTH:
        _fail("deployment_draft_path_invalid", "Deployment edit path is invalid")
    candidate = deepcopy(document)
    current: Any = candidate
    for segment in path[:-1]:
        if isinstance(segment, str) and isinstance(current, dict) and segment in current:
            current = current[segment]
        elif (
            type(segment) is int
            and 0 <= segment <= _MAX_COLLECTION_INDEX
            and isinstance(current, list)
            and segment < len(current)
        ):
            current = current[segment]
        else:
            _fail("deployment_draft_path_invalid", "Deployment edit path does not exist")
    final = path[-1]
    if operation == "append":
        target = current.get(final) if isinstance(final, str) and isinstance(current, dict) else (
            current[final]
            if type(final) is int and isinstance(current, list) and 0 <= final < len(current)
            else None
        )
        if not isinstance(target, list):
            _fail("deployment_draft_path_invalid", "Deployment append target is not a list")
        target.append(deepcopy(value))
        return candidate
    if isinstance(final, str) and isinstance(current, dict):
        if operation == "remove":
            if final not in current:
                _fail("deployment_draft_path_invalid", "Deployment remove target does not exist")
            del current[final]
        elif operation == "set":
            current[final] = deepcopy(value)
        else:
            _fail("deployment_draft_operation_invalid", "Deployment edit operation is invalid")
    elif (
        type(final) is int
        and 0 <= final <= _MAX_COLLECTION_INDEX
        and isinstance(current, list)
        and final < len(current)
    ):
        if operation == "remove":
            del current[final]
        elif operation == "set":
            current[final] = deepcopy(value)
        else:
            _fail("deployment_draft_operation_invalid", "Deployment edit operation is invalid")
    else:
        _fail("deployment_draft_path_invalid", "Deployment edit target does not exist")
    return candidate


class DeploymentDraftSession:
    """Own one optional exact Deployment Draft and Saved baseline."""

    def __init__(self, *, compiler: DeploymentCompiler = compile_deployment_document) -> None:
        self._lock = threading.RLock()
        self._compiler = compiler
        self._draft_id = f"deployment-draft-{uuid.uuid4().hex}"
        self._revision = 0
        self._compiled: CompiledDeploymentDocument | None = None
        self._saved_path: str | None = None
        self._saved_document: dict[str, Any] | None = None

    @property
    def loaded(self) -> bool:
        with self._lock:
            return self._compiled is not None

    def _require_current(self, draft_id: str, expected_revision: int) -> None:
        if draft_id != self._draft_id or expected_revision != self._revision:
            _fail(
                "deployment_draft_stale",
                "Deployment Draft changed; refresh before editing",
                status_code=409,
            )

    def require_current(self, draft_id: str, expected_revision: int) -> None:
        with self._lock:
            self._require_current(draft_id, expected_revision)

    def require_open_allowed(
        self,
        draft_id: str,
        expected_revision: int,
        *,
        confirm_discard: bool,
    ) -> None:
        """Fence an Open before its intent is recorded and any effect begins."""
        with self._lock:
            self._require_current(draft_id, expected_revision)
            if (
                self._compiled is not None
                and self._compiled.document != self._saved_document
                and not confirm_discard
            ):
                _fail(
                    "deployment_discard_confirmation_required",
                    "opening another Deployment would discard Draft changes",
                    status_code=409,
                )

    def preview_source(self, source: str) -> CompiledDeploymentDocument:
        return self._compiler(source)

    def preview_update(
        self,
        path: Sequence[str | int],
        value: Any,
        operation: str = "set",
        *,
        draft_id: str,
        expected_revision: int,
    ) -> CompiledDeploymentDocument:
        with self._lock:
            self._require_current(draft_id, expected_revision)
            if self._compiled is None:
                _fail(
                    "deployment_draft_unavailable",
                    "open a Deployment artifact before editing",
                    status_code=404,
                )
            candidate = _edit_at_path(self._compiled.document, path, value, operation)
        return self._compiler(_source(candidate))

    def commit_compiled(
        self,
        compiled: CompiledDeploymentDocument,
        *,
        draft_id: str,
        expected_revision: int,
    ) -> None:
        with self._lock:
            self._require_current(draft_id, expected_revision)
            if (
                self._compiled is not None
                and self._compiled.document == compiled.document
            ):
                return
            self._compiled = compiled
            self._revision += 1

    def retained_state(self) -> dict[str, Any]:
        with self._lock:
            return {
                "schema_version": _RETAINED_SCHEMA,
                "draft_id": self._draft_id,
                "draft_revision": self._revision,
                "document": (
                    deepcopy(self._compiled.document)
                    if self._compiled is not None
                    else None
                ),
                "saved_path": self._saved_path,
                "saved_document": deepcopy(self._saved_document),
            }

    def restore_retained_state(self, state: dict[str, Any]) -> None:
        expected = {
            "schema_version",
            "draft_id",
            "draft_revision",
            "document",
            "saved_path",
            "saved_document",
        }
        if not isinstance(state, dict) or set(state) != expected:
            _fail(
                "deployment_retained_draft_invalid",
                "retained Deployment Draft has an invalid shape",
                status_code=503,
            )
        if (
            state["schema_version"] != _RETAINED_SCHEMA
            or not isinstance(state["draft_id"], str)
            or not state["draft_id"]
            or type(state["draft_revision"]) is not int
            or state["draft_revision"] < 0
            or (state["saved_path"] is not None and not isinstance(state["saved_path"], str))
            or (state["saved_document"] is not None and not isinstance(state["saved_document"], dict))
        ):
            _fail(
                "deployment_retained_draft_invalid",
                "retained Deployment Draft fields are invalid",
                status_code=503,
            )
        compiled = (
            None
            if state["document"] is None
            else self._compiler(_source(state["document"]))
        )
        if state["saved_document"] is not None:
            self._compiler(_source(state["saved_document"]))
        with self._lock:
            self._draft_id = state["draft_id"]
            self._revision = state["draft_revision"]
            self._compiled = compiled
            self._saved_path = state["saved_path"]
            self._saved_document = deepcopy(state["saved_document"])

    def commit_open(
        self,
        compiled: CompiledDeploymentDocument,
        source_path: str,
        *,
        draft_id: str,
        expected_revision: int,
        confirm_discard: bool,
    ) -> None:
        with self._lock:
            self._require_current(draft_id, expected_revision)
            # The coordinator called require_open_allowed while holding the
            # authoring claim. Retain the argument in this boundary so callers
            # cannot accidentally bypass that explicit discard decision.
            if not confirm_discard and self._compiled is not None:
                if self._compiled.document != self._saved_document:
                    _fail(
                        "deployment_discard_confirmation_required",
                        "opening another Deployment would discard Draft changes",
                        status_code=409,
                    )
            resolved_source_path = str(Path(source_path).resolve())
            if (
                self._compiled is not None
                and self._compiled.document == compiled.document
                and self._saved_document == compiled.document
                and self._saved_path == resolved_source_path
            ):
                return
            self._compiled = compiled
            self._saved_path = resolved_source_path
            self._saved_document = deepcopy(compiled.document)
            self._revision += 1

    def mark_saved(
        self, path: str, *, draft_id: str, expected_revision: int
    ) -> None:
        with self._lock:
            self._require_current(draft_id, expected_revision)
            if self._compiled is None:
                _fail(
                    "deployment_draft_unavailable",
                    "open a Deployment artifact before saving",
                    status_code=404,
                )
            self._saved_path = str(Path(path).resolve())
            self._saved_document = deepcopy(self._compiled.document)

    def canonical_json(self, *, draft_id: str, expected_revision: int) -> str:
        with self._lock:
            self._require_current(draft_id, expected_revision)
            if self._compiled is None:
                _fail(
                    "deployment_draft_unavailable",
                    "open a Deployment artifact before saving",
                    status_code=404,
                )
            return self._compiled.canonical_json

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            compiled = self._compiled
            document = deepcopy(compiled.document) if compiled is not None else None
            return {
                "schemaVersion": "manyforge.composer_deployment_workspace.v1",
                "draft": {
                    "id": self._draft_id,
                    "revision": self._revision,
                    "document": document,
                    "deploymentArtifactId": (
                        compiled.deployment_artifact_id if compiled is not None else None
                    ),
                    "deploymentRevision": (
                        compiled.deployment_revision if compiled is not None else None
                    ),
                    "graphRevision": compiled.graph_revision if compiled is not None else None,
                    "differsFromSaved": (
                        document != self._saved_document if document is not None else False
                    ),
                },
                "saved": {
                    "path": self._saved_path,
                    "deploymentRevision": (
                        self._saved_document.get("deployment_revision")
                        if self._saved_document is not None
                        else None
                    ),
                },
            }
