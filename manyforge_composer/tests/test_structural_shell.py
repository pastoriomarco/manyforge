"""Focused contracts for the Composer cutover shell."""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest
from fastapi.testclient import TestClient
from pydantic import BaseModel
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.node_environment import BUILTIN_CATALOG_PATH

from manyforge_composer.backend import routes_filesystem
from manyforge_composer.backend.app import _find_static_dir, create_app
from manyforge_composer.backend.models import CatalogParameterResponse
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState


def _client(
    *,
    allowed_fs_roots: list[str] | None = None,
    cors_origins: list[str] | None = None,
) -> TestClient:
    security = SecurityConfig(
        allowed_fs_roots=allowed_fs_roots or ["/workspace"],
        cors_origins=cors_origins or ["http://localhost:3000"],
    )
    return TestClient(create_app(ComposerState(), security=security))


def test_health_and_catalog_report_structural_only_state() -> None:
    client = _client()

    health = client.get("/api/health")
    assert health.status_code == 200
    assert health.json()["status"] == "degraded"
    assert health.json()["mode"] == "structural_only"
    assert health.json()["components"]["runtime"] == "unavailable"
    assert health.json()["runtimeAvailability"]["code"] == (
        "composable_cell_runtime_unavailable"
    )
    assert health.json()["runtimeAvailability"]["restorationGates"] == [
        "N7",
        "L0",
        "P5",
        "I",
        "R",
        "T",
    ]

    catalog = client.get("/api/catalog")
    assert catalog.status_code == 200
    payload = catalog.json()
    assert len(payload["entries"]) == 52  # rules.set_active, io.clear_outside_change
    assert payload["apiVersion"] == "manyforge.node_catalog.v2"
    assert payload["contentVersion"] == NodeCatalog(BUILTIN_CATALOG_PATH).content_version
    assert payload["catalogId"] == "manyforge_builtin"
    assert payload["catalogDigest"].startswith("sha256:")
    assert payload["environmentDigest"].startswith("sha256:")
    assert "generation" not in payload
    assert "sources" not in payload
    assert "effectiveCatalogHash" not in payload
    gripper = next(entry for entry in payload["entries"] if entry["id"] == "gripper.command_position")
    dwell = next(parameter for parameter in gripper["parameters"] if parameter["id"] == "dwell_while_held")
    assert dwell["required"] is False
    assert dwell["hasDefault"] is True
    assert dwell["default"] == "pause"
    assert dwell["schema"] == {"type": "string", "enum": ["count", "pause"]}
    duration = next(parameter for parameter in gripper["parameters"] if parameter["id"] == "command_duration_s")
    assert duration["required"] is False
    assert duration["hasDefault"] is False
    assert duration["schema"]["minimum"] == 0.0
    sequence = next(entry for entry in payload["entries"] if entry["id"] == "sequence")
    assert sequence["execution"] == {
        "blocking": False,
        "cancellable": False,
        "ownerApi": "orchestration.control_flow",
    }
    assert sequence["runners"] == {"py_trees": "sequence_v1"}
    assert "bbReaderParams" not in sequence
    parameter_without_default = next(
        parameter
        for entry in payload["entries"]
        for parameter in entry["parameters"]
        if not parameter["hasDefault"]
    )
    assert "default" not in parameter_without_default

    environment = client.get("/api/nodes/environment")
    assert environment.status_code == 200
    environment_payload = environment.json()
    assert environment_payload["catalog"]["catalogDigest"] == payload["catalogDigest"]
    assert len(environment_payload["executableRegistry"]) == 52


def test_removed_runtime_routes_fail_with_one_typed_unavailable_reason() -> None:
    client = _client()
    for method, path in (
        ("get", "/api/scene/state"),
        ("post", "/api/scene/edit"),
        ("put", "/api/scene/state"),
        ("patch", "/api/scene/state"),
        ("delete", "/api/scene/state"),
        ("get", "/api/nodes"),
        ("get", "/api/execution/history"),
        ("get", "/api/scene/viewer/status"),
        ("get", "/api/scene/snapshot/status"),
        ("get", "/api/scene/pointcloud/status"),
        ("post", "/api/mcp/messages"),
    ):
        response = getattr(client, method)(path)
        assert response.status_code == 503
        assert response.json()["detail"]["code"] == (
            "composable_cell_runtime_unavailable"
        )


def test_program_workspace_is_available_while_effectful_actions_need_headless() -> None:
    empty = _client().get("/api/program/draft")
    assert empty.status_code == 200
    assert empty.json()["program_loaded"] is False
    assert empty.json()["saved"] is False
    assert empty.json()["applied"] is False
    empty_workspace = _client().get("/api/program/workspace")
    assert empty_workspace.status_code == 200
    assert empty_workspace.json()["draft"]["programLoaded"] is False
    assert empty_workspace.json()["hasUnappliedChanges"] is None

    program_path = (
        Path(__file__).resolve().parents[2]
        / "examples"
        / ("pick_and_place_minimal.program.yaml")
    )
    state = ComposerState.from_paths(
        program_path=str(program_path),
    )
    loaded = TestClient(create_app(state)).get("/api/program/draft")
    assert loaded.status_code == 200
    assert loaded.json()["program_loaded"] is True
    assert loaded.json()["program"]["api_version"] == "manyforge.program.v2"
    assert loaded.json()["draft_revision"] == 1
    assert loaded.json()["saved"] is True
    assert loaded.json()["applied"] is False

    unavailable_apply = TestClient(create_app(state)).post(
        "/api/program/apply",
        json={
            "draftId": state.draft_session.draft_id,
            "expectedDraftRevision": state.draft_session.revision,
            "expectedAdmissionGeneration": 1,
            "requestOwnerEpoch": "test-browser.epoch.1",
            "requestId": "test-browser.request.1",
            "expectedWorkspaceEpoch": (
                state.authoring_workspace.identity().workspace_epoch
            ),
            "expectedWorkspaceRevision": (
                state.authoring_workspace.identity().workspace_revision
            ),
        },
    )
    assert unavailable_apply.status_code == 503
    assert unavailable_apply.json()["detail"]["code"] == "program_apply_unavailable"


def test_composer_bounds_callback_bodies_without_narrowing_other_routes() -> None:
    state = ComposerState(headless_client=object())  # type: ignore[arg-type]
    client = TestClient(create_app(state))
    response = client.post(
        "/api/assistant/bridge/tools/scene_draft_add_object",
        content=b"x" * (1024 * 1024 + 1),
        headers={"content-type": "application/json"},
    )

    assert response.status_code == 413
    assert response.json()["detail"]["code"] == ("composer_request_body_oversized")

@pytest.mark.parametrize(
    "path",
    [
        "/api/program/new-draft",
        "/api/program/discard",
        "/api/program/revert",
    ],
)
@pytest.mark.parametrize("streamed", [False, True], ids=["declared", "streamed"])
def test_program_lifecycle_mutations_reject_oversized_bodies_atomically(
    path: str,
    streamed: bool,
) -> None:
    class FailOnHeadlessCall:
        def __init__(self) -> None:
            self.calls: list[str] = []

        def __getattr__(self, name: str) -> object:
            self.calls.append(name)
            raise AssertionError(f"unexpected headless call: {name}")

    headless = FailOnHeadlessCall()
    state = ComposerState(headless_client=headless)  # type: ignore[arg-type]
    headless.calls.clear()
    before = state.draft_session.workspace_snapshot()
    body_size = 1024 * 1024 + 1
    if streamed:
        content = (
            b"x" * min(64 * 1024, body_size - offset)
            for offset in range(0, body_size, 64 * 1024)
        )
        headers = {
            "content-type": "application/json",
            "transfer-encoding": "chunked",
        }
    else:
        content = b"x" * body_size
        headers = {
            "content-type": "application/json",
            "content-length": str(body_size),
        }

    response = TestClient(create_app(state)).post(
        path,
        content=content,
        headers=headers,
    )

    assert response.status_code == 413
    assert response.json()["detail"]["code"] == "composer_request_body_oversized"
    assert state.draft_session.workspace_snapshot() == before
    assert headless.calls == []


def test_unavailable_catch_all_has_stable_unique_openapi_operation_ids() -> None:
    app = create_app(ComposerState())
    catch_all_routes = [
        route
        for route in app.routes
        if getattr(route, "path", None) == "/api/{unavailable_path:path}"
    ]
    expected_ids = {
        "delete": "unavailable_api_delete",
        "get": "unavailable_api_get",
        "patch": "unavailable_api_patch",
        "post": "unavailable_api_post",
        "put": "unavailable_api_put",
    }

    assert len(catch_all_routes) == len(expected_ids)
    assert all(len(route.methods) == 1 for route in catch_all_routes)
    assert {
        next(iter(route.methods)).lower(): route.operation_id
        for route in catch_all_routes
    } == expected_ids

    spec = app.openapi()
    operations = spec["paths"]["/api/{unavailable_path}"]
    assert {
        method: operation["operationId"] for method, operation in operations.items()
    } == expected_ids
    operation_ids = [
        operation["operationId"]
        for path in spec["paths"].values()
        for method, operation in path.items()
        if method in {"delete", "get", "patch", "post", "put"}
    ]
    assert len(operation_ids) == len(set(operation_ids))


def test_catalog_parameter_schema_keeps_its_public_wire_name() -> None:
    projected_schema = {"type": "string", "minLength": 1}
    parameter = CatalogParameterResponse(
        id="target_role",
        required=True,
        schema_=projected_schema,
        schema_hash="sha256:catalog-parameter",
        has_default=False,
    )

    assert parameter.schema_ == projected_schema
    assert parameter.model_dump(by_alias=True, exclude_unset=True) == {
        "id": "target_role",
        "required": True,
        "schema": projected_schema,
        "schemaHash": "sha256:catalog-parameter",
        "hasDefault": False,
    }
    openapi_schema = CatalogParameterResponse.model_json_schema(by_alias=True)
    assert "schema" in openapi_schema["properties"]
    assert "schema_" not in openapi_schema["properties"]


def test_structural_routes_preserve_http_errors_and_request_correlation() -> None:
    client = _client()

    missing = client.get(
        "/api/catalog/does-not-exist",
        headers={"X-Request-ID": "ticket-42$invalid"},
    )

    assert missing.status_code == 404
    assert missing.headers["X-Request-ID"] == "ticket-42invalid"
    assert missing.json()["detail"]


def test_request_log_replays_error_responses_with_repeated_headers() -> None:
    """Reading an error's code must not collapse repeated response headers."""

    from fastapi.responses import JSONResponse

    app = create_app(
        ComposerState(),
        security=SecurityConfig(
            allowed_fs_roots=["/workspace"],
            cors_origins=["http://localhost:3000"],
        ),
    )

    def refuse() -> JSONResponse:
        response = JSONResponse(
            status_code=409, content={"detail": {"code": "test_refused"}}
        )
        response.set_cookie("first", "1")
        response.set_cookie("second", "2")
        return response

    app.add_api_route("/api/test/refused", refuse, methods=["GET"])
    # Ahead of the unavailable catch-all.
    app.router.routes.insert(0, app.router.routes.pop())

    refused = TestClient(app).get(
        "/api/test/refused", headers={"X-Request-ID": "ticket-7"}
    )

    assert refused.status_code == 409
    assert refused.json() == {"detail": {"code": "test_refused"}}
    assert [cookie.split(";")[0] for cookie in refused.headers.get_list("set-cookie")] == [
        "first=1",
        "second=2",
    ]
    assert refused.headers["X-Request-ID"] == "ticket-7"


def test_composer_ingress_remains_open_when_legacy_token_env_is_set(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("MANYFORGE_API_TOKEN", "stale-legacy-token")
    security = SecurityConfig.from_env()
    client = TestClient(create_app(ComposerState(), security=security))

    assert not hasattr(security, "api_token")
    assert client.get("/api/health").status_code == 200
    assert (
        client.get(
            "/api/catalog",
            headers={"Authorization": "Bearer wrong-token"},
        ).status_code
        == 200
    )
    unavailable = client.get("/api/scene/state")
    assert unavailable.status_code == 503


def test_filesystem_browser_remains_sandboxed(tmp_path: Path) -> None:
    inside = tmp_path / "inside"
    inside.mkdir()
    (inside / "cell.deployment.yaml").write_text("x: 1\n", encoding="utf-8")
    client = _client(allowed_fs_roots=[str(inside)])

    accepted = client.get(
        "/api/files/browse",
        params={"path": str(inside), "kind": "deployment"},
    )
    assert accepted.status_code == 200
    assert accepted.json()["entries"][0]["name"] == "cell.deployment.yaml"

    rejected = client.get("/api/files/browse", params={"path": str(tmp_path)})
    assert rejected.status_code == 403
    assert str(tmp_path) not in rejected.json()["detail"]


@pytest.mark.parametrize("request_params", [{}, {"path": ""}])
def test_filesystem_browser_defaults_to_first_allowed_root_outside_cwd(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    request_params: dict[str, str],
) -> None:
    outside = tmp_path / "outside"
    first = tmp_path / "first"
    second = tmp_path / "second"
    outside.mkdir()
    first.mkdir()
    second.mkdir()
    monkeypatch.chdir(outside)
    client = _client(allowed_fs_roots=[str(first), str(second)])

    response = client.get("/api/files/browse", params=request_params)

    assert response.status_code == 200
    assert response.json()["currentPath"] == str(first.resolve())


def test_filesystem_browser_default_skips_only_unusable_configured_roots(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    missing = tmp_path / "missing"
    available = tmp_path / "available"
    outside = tmp_path / "outside"
    available.mkdir()
    outside.mkdir()
    monkeypatch.chdir(outside)
    client = _client(allowed_fs_roots=[str(missing), str(available)])

    response = client.get("/api/files/browse")

    assert response.status_code == 200
    assert response.json()["currentPath"] == str(available.resolve())


def test_filesystem_browser_default_fails_without_a_usable_configured_root(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    missing = tmp_path / "missing"
    not_a_directory = tmp_path / "file"
    outside = tmp_path / "outside"
    not_a_directory.write_text("not a directory\n", encoding="utf-8")
    outside.mkdir()
    monkeypatch.chdir(outside)
    client = _client(allowed_fs_roots=[str(missing), str(not_a_directory)])

    response = client.get("/api/files/browse")

    assert response.status_code == 503
    assert response.json() == {"detail": "No accessible filesystem root"}
    assert str(outside) not in response.text


def test_filesystem_router_rejects_an_explicit_invalid_allowlist() -> None:
    with pytest.raises(ValueError, match="no valid allowed root"):
        routes_filesystem.create_filesystem_router([""])
    with pytest.raises(ValueError, match="no valid allowed root"):
        routes_filesystem.create_filesystem_router(["invalid\x00root"])
    with pytest.raises(ValueError, match="must be strings"):
        routes_filesystem.create_filesystem_router([True])  # type: ignore[list-item]
    with pytest.raises(ValueError, match="must be a list"):
        routes_filesystem.create_filesystem_router(  # type: ignore[arg-type]
            ("/workspace",)
        )


def test_filesystem_browser_preserves_bounded_sorted_authoring_entries(
    tmp_path: Path,
) -> None:
    allowed = tmp_path / "allowed"
    allowed.mkdir()
    (allowed / "z-directory").mkdir()
    (allowed / "A-directory").mkdir()
    (allowed / "z.program.yaml").write_text("x: 1\n", encoding="utf-8")
    (allowed / "A.program.yml").write_text("x: 1\n", encoding="utf-8")
    (allowed / "ignored.deployment.yaml").write_text("x: 1\n", encoding="utf-8")
    (allowed / "part.glb").write_bytes(b"glTF")
    (allowed / "ignored.txt").write_text("x\n", encoding="utf-8")
    client = _client(allowed_fs_roots=[str(allowed)])

    response = client.get(
        "/api/files/browse",
        params={"path": str(allowed), "kind": "program"},
    )

    assert response.status_code == 200
    entries = response.json()["entries"]
    assert [entry["name"] for entry in entries] == [
        "A-directory",
        "z-directory",
        "A.program.yml",
        "z.program.yaml",
    ]
    assert [entry["entryType"] for entry in entries] == [
        "directory",
        "directory",
        "file",
        "file",
    ]
    assert [entry["selectable"] for entry in entries] == [
        False,
        False,
        True,
        True,
    ]

    browse_operation = client.get("/openapi.json").json()["paths"]["/api/files/browse"][
        "get"
    ]
    response_schema = browse_operation["responses"]["200"]["content"][
        "application/json"
    ]["schema"]
    assert response_schema == {"$ref": "#/components/schemas/FileBrowserResponse"}

    mesh_response = client.get(
        "/api/files/browse",
        params={"path": str(allowed), "kind": "mesh"},
    )
    assert mesh_response.status_code == 200
    assert [entry["name"] for entry in mesh_response.json()["entries"]] == [
        "A-directory",
        "z-directory",
        "part.glb",
    ]


def test_filesystem_browser_stops_at_root_and_rejects_symlink_escape(
    tmp_path: Path,
) -> None:
    allowed = tmp_path / "allowed"
    child = allowed / "child"
    outside = tmp_path / "outside"
    child.mkdir(parents=True)
    outside.mkdir()
    (outside / "secret.program.yaml").write_text("x: 1\n", encoding="utf-8")
    (allowed / "escape").symlink_to(outside, target_is_directory=True)
    client = _client(allowed_fs_roots=[str(allowed)])

    child_response = client.get(
        "/api/files/browse",
        params={"path": str(child)},
    )
    root_response = client.get(
        "/api/files/browse",
        params={"path": str(allowed)},
    )
    escape_response = client.get(
        "/api/files/browse",
        params={"path": str(allowed / "escape")},
    )

    assert child_response.json()["parentPath"] == str(allowed.resolve())
    assert root_response.json()["parentPath"] is None
    assert "escape" not in {entry["name"] for entry in root_response.json()["entries"]}
    assert escape_response.status_code == 403
    assert str(outside) not in escape_response.json()["detail"]


def test_filesystem_browser_does_not_let_a_symlink_redefine_an_allowed_root(
    tmp_path: Path,
) -> None:
    allowed = tmp_path / "allowed"
    moved = tmp_path / "moved"
    outside = tmp_path / "outside"
    allowed.mkdir()
    outside.mkdir()
    (outside / "secret.program.yaml").write_text("x: 1\n", encoding="utf-8")
    client = _client(allowed_fs_roots=[str(allowed)])

    allowed.rename(moved)
    allowed.symlink_to(outside, target_is_directory=True)
    response = client.get("/api/files/browse", params={"path": str(allowed)})
    default_response = client.get("/api/files/browse")

    assert response.status_code == 403
    assert response.json()["detail"] == "Path is outside allowed roots"
    assert str(outside) not in response.text
    assert default_response.status_code == 503
    assert default_response.json() == {"detail": "No accessible filesystem root"}
    assert str(outside) not in default_response.text


def test_filesystem_browser_stops_scanning_at_the_hard_entry_bound(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    allowed = tmp_path / "allowed"
    allowed.mkdir()
    client = _client(allowed_fs_roots=[str(allowed)])
    monkeypatch.setattr(routes_filesystem, "_MAX_SCANNED_DIRECTORY_ENTRIES", 2)

    class FakeEntry:
        def __init__(self, name: str) -> None:
            self.name = name

        def is_symlink(self) -> bool:
            return False

        def is_dir(self, *, follow_symlinks: bool) -> bool:
            assert follow_symlinks is False
            return True

        def is_file(self, *, follow_symlinks: bool) -> bool:
            assert follow_symlinks is False
            return False

    class GuardedScandir:
        def __init__(self) -> None:
            self.pulls = 0

        def __enter__(self) -> GuardedScandir:
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def __iter__(self) -> GuardedScandir:
            return self

        def __next__(self) -> FakeEntry:
            self.pulls += 1
            if self.pulls > 3:
                raise AssertionError("directory iterator was consumed past its bound")
            return FakeEntry(f"entry-{self.pulls}")

    scanner = GuardedScandir()
    monkeypatch.setattr(routes_filesystem.os, "scandir", lambda _fd: scanner)
    monkeypatch.setattr(
        routes_filesystem.Path,
        "iterdir",
        lambda _path: (_ for _ in ()).throw(
            AssertionError("browse must not materialize Path.iterdir()")
        ),
    )

    response = client.get("/api/files/browse", params={"path": str(allowed)})

    assert response.status_code == 413
    assert response.json()["detail"] == "Directory exceeds the browse entry limit"
    assert scanner.pulls == 3


def test_filesystem_browser_bounds_returned_entries_before_sorting(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    allowed = tmp_path / "allowed"
    allowed.mkdir()
    (allowed / "one").mkdir()
    (allowed / "two").mkdir()
    client = _client(allowed_fs_roots=[str(allowed)])
    monkeypatch.setattr(routes_filesystem, "_MAX_RETURNED_DIRECTORY_ENTRIES", 1)

    response = client.get("/api/files/browse", params={"path": str(allowed)})

    assert response.status_code == 413
    assert response.json()["detail"] == "Directory exceeds the returned entry limit"


def test_filesystem_browser_bounds_request_entry_name_and_entry_path(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    allowed = tmp_path / "allowed"
    allowed.mkdir()
    (allowed / "long-name").mkdir()
    client = _client(allowed_fs_roots=[str(allowed)])

    oversized_path = "é" * (routes_filesystem._MAX_BROWSE_PATH_BYTES // 2 + 1)
    path_response = client.get(
        "/api/files/browse",
        params={"path": oversized_path},
    )
    assert path_response.status_code == 414
    assert oversized_path not in path_response.text

    monkeypatch.setattr(routes_filesystem, "_MAX_BROWSE_NAME_BYTES", 4)
    name_response = client.get(
        "/api/files/browse",
        params={"path": str(allowed)},
    )
    assert name_response.status_code == 413
    assert name_response.json()["detail"] == (
        "Directory entry name exceeds the browse limit"
    )

    monkeypatch.setattr(routes_filesystem, "_MAX_BROWSE_NAME_BYTES", 255)
    monkeypatch.setattr(
        routes_filesystem,
        "_MAX_BROWSE_PATH_BYTES",
        len(str(allowed).encode("utf-8")) + 1,
    )
    entry_path_response = client.get(
        "/api/files/browse",
        params={"path": str(allowed)},
    )
    assert entry_path_response.status_code == 413
    assert entry_path_response.json()["detail"] == (
        "Directory entry path exceeds the browse limit"
    )


def test_filesystem_browser_bounds_aggregate_json_output(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    allowed = tmp_path / "allowed"
    allowed.mkdir()
    client = _client(allowed_fs_roots=[str(allowed)])
    monkeypatch.setattr(routes_filesystem, "_MAX_BROWSE_OUTPUT_BYTES", 32)

    response = client.get("/api/files/browse", params={"path": str(allowed)})

    assert response.status_code == 413
    assert response.json()["detail"] == (
        "Filesystem browse response exceeds the output limit"
    )


def test_filesystem_browser_fails_closed_if_directory_moves_during_scan(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    allowed = tmp_path / "allowed"
    moved = tmp_path / "moved"
    outside = tmp_path / "outside"
    allowed.mkdir()
    outside.mkdir()
    client = _client(allowed_fs_roots=[str(allowed)])
    opened_descriptors: list[int] = []

    def replace_directory(
        descriptor: int,
        current_dir: Path,
        _kind: str,
    ) -> list[dict[str, object]]:
        opened_descriptors.append(descriptor)
        current_dir.rename(moved)
        current_dir.symlink_to(outside, target_is_directory=True)
        return []

    monkeypatch.setattr(routes_filesystem, "_scan_directory", replace_directory)

    response = client.get("/api/files/browse", params={"path": str(allowed)})

    assert response.status_code == 409
    assert response.json()["detail"] == "Directory changed while being browsed"
    assert len(opened_descriptors) == 1
    with pytest.raises(OSError):
        os.fstat(opened_descriptors[0])


def test_filesystem_browser_redacts_scan_errors_and_closes_the_descriptor(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    allowed = tmp_path / "allowed"
    allowed.mkdir()
    client = _client(allowed_fs_roots=[str(allowed)])
    opened_descriptors: list[int] = []
    real_open_directory = routes_filesystem._open_directory

    def record_open_directory(
        current_dir: Path,
        roots: list[str],
    ) -> tuple[int, os.stat_result]:
        opened = real_open_directory(current_dir, roots)
        opened_descriptors.append(opened[0])
        return opened

    def fail_scan(_descriptor: int) -> None:
        raise OSError(f"secret path: {allowed}")

    monkeypatch.setattr(routes_filesystem, "_open_directory", record_open_directory)
    monkeypatch.setattr(routes_filesystem.os, "scandir", fail_scan)

    response = client.get("/api/files/browse", params={"path": str(allowed)})

    assert response.status_code == 500
    assert response.json()["detail"] == "Filesystem error"
    assert str(allowed) not in response.text
    assert len(opened_descriptors) == 1
    with pytest.raises(OSError):
        os.fstat(opened_descriptors[0])


def test_structural_cors_reflects_only_configured_origins() -> None:
    client = _client(cors_origins=["http://localhost:3000"])
    request_headers = {
        "Access-Control-Request-Method": "GET",
    }

    allowed = client.options(
        "/api/health",
        headers={"Origin": "http://localhost:3000", **request_headers},
    )
    rejected = client.options(
        "/api/health",
        headers={"Origin": "http://evil.example.com", **request_headers},
    )

    assert allowed.headers["access-control-allow-origin"] == ("http://localhost:3000")
    assert rejected.headers.get("access-control-allow-origin") is None


def test_explicit_static_dir_requires_an_index(tmp_path: Path) -> None:
    missing = tmp_path / "missing"
    assert _find_static_dir(str(missing)) is None

    empty = tmp_path / "empty"
    empty.mkdir()
    assert _find_static_dir(str(empty)) is None

    built = tmp_path / "built"
    built.mkdir()
    (built / "index.html").write_text("<!doctype html>\n", encoding="utf-8")
    assert _find_static_dir(str(built)) == built


def test_scenario_folder_browser_lists_folders_and_marks_projects(
    tmp_path: Path,
) -> None:
    allowed = tmp_path / "allowed"
    (allowed / "cells").mkdir(parents=True)
    (allowed / "pick.scenario").mkdir()
    (allowed / "cell.program.yaml").write_text("x: 1\n", encoding="utf-8")
    (allowed / "pick.scenario.yaml").write_text("x: 1\n", encoding="utf-8")
    client = _client(allowed_fs_roots=[str(allowed)])

    response = client.get(
        "/api/files/browse", params={"path": str(allowed), "kind": "scenario"},
    )

    assert response.status_code == 200, response.text
    assert [
        (entry["name"], entry["entryType"], entry["selectable"], entry["scenarioProject"])
        for entry in response.json()["entries"]
    ] == [
        ("cells", "directory", False, False),
        ("pick.scenario", "directory", False, True),
    ]
    assert response.json()["parentPath"] is None
    assert response.json()["roots"] == [str(allowed.resolve())]


def test_scenario_folder_browser_displays_only_configured_mount_mappings(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    examples = tmp_path / "examples"
    projects = tmp_path / "user projects"
    outside = tmp_path / "outside"
    examples.mkdir()
    projects.mkdir()
    outside.mkdir()
    host_directory = tmp_path / "host project"
    host_directory.mkdir()
    host_path = str(host_directory)
    monkeypatch.setenv("MANYFORGE_PROJECT_ROOT_MAPPINGS", json.dumps([
        {"hostPath": host_path, "containerPath": str(projects)},
        {"hostPath": "/home/operator/outside", "containerPath": str(outside)},
    ]))
    client = _client(allowed_fs_roots=[str(examples), str(projects)])

    initial = client.get("/api/files/browse", params={"kind": "scenario", "roots_only": "true"})
    assert initial.status_code == 200, initial.text
    assert initial.json()["projectRoots"] == [
        {"hostPath": host_path, "containerPath": str(projects)}
    ]
    opened = client.get("/api/files/browse", params={"kind": "scenario", "path": str(projects)})
    assert opened.status_code == 200, opened.text
    assert opened.json()["projectRoots"] == initial.json()["projectRoots"]
    # A displayed host path is metadata, not another filesystem authority.
    refused = client.get("/api/files/browse", params={"kind": "scenario", "path": host_path})
    assert refused.status_code == 403
    program = client.get("/api/files/browse", params={"kind": "program", "roots_only": "true"})
    assert program.status_code == 200 and "projectRoots" not in program.json()


def test_scenario_folder_browser_lists_each_allowed_project_root(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    examples = tmp_path / "examples"
    projects = tmp_path / "user projects"
    outside = tmp_path / "outside"
    examples.mkdir()
    projects.mkdir()
    outside.mkdir()
    (projects / "cell.scenario").mkdir()
    (outside / "private.scenario").mkdir()
    (projects / "escape").symlink_to(outside, target_is_directory=True)
    monkeypatch.chdir(outside)
    client = _client(allowed_fs_roots=[str(examples), str(projects)])

    initial = client.get("/api/files/browse", params={"kind": "scenario"})
    assert initial.status_code == 200, initial.text
    assert initial.json()["roots"] == [str(examples.resolve()), str(projects.resolve())]
    listing = client.get(
        "/api/files/browse", params={"path": str(projects), "kind": "scenario"},
    )
    assert listing.status_code == 200, listing.text
    assert [(entry["name"], entry["scenarioProject"]) for entry in listing.json()["entries"]] == [
        ("cell.scenario", True),
    ]
    assert listing.json()["parentPath"] is None
    assert client.get(
        "/api/files/browse", params={"path": str(outside), "kind": "scenario"},
    ).status_code == 403
    assert client.get(
        "/api/files/browse", params={"path": str(projects / "escape"), "kind": "scenario"},
    ).status_code == 403


def test_scenario_root_list_does_not_scan_a_large_first_root(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    examples = tmp_path / "examples"
    projects = tmp_path / "projects"
    examples.mkdir()
    projects.mkdir()
    for index in range(1025):
        (examples / f"folder_{index}").mkdir()
    (projects / "cell.scenario").mkdir()
    # A child of an allowed root is an existing navigation shortcut, but it
    # must not be presented as a configured root on the initial chooser.
    monkeypatch.chdir(examples / "folder_0")
    client = _client(allowed_fs_roots=[str(examples), str(projects)])

    initial = client.get(
        "/api/files/browse", params={"kind": "scenario", "roots_only": "true"},
    )
    assert initial.status_code == 200, initial.text
    assert initial.json()["roots"] == [str(examples.resolve()), str(projects.resolve())]
    assert initial.json()["entries"] == []
    assert client.get(
        "/api/files/browse", params={"kind": "scenario", "path": str(examples)},
    ).status_code == 413
    opened = client.get(
        "/api/files/browse", params={"kind": "scenario", "path": str(projects)},
    )
    assert opened.status_code == 200, opened.text
    assert [entry["name"] for entry in opened.json()["entries"]] == ["cell.scenario"]
    assert str((examples / "folder_0").resolve()) in opened.json()["roots"]


def test_scenario_folder_browser_stays_inside_the_roots(tmp_path: Path) -> None:
    allowed = tmp_path / "allowed"
    outside = tmp_path / "outside"
    (allowed / "child").mkdir(parents=True)
    outside.mkdir()
    (outside / "secret.scenario").mkdir()
    (allowed / "escape").symlink_to(outside, target_is_directory=True)
    client = _client(allowed_fs_roots=[str(allowed)])

    def browse(path: str):
        return client.get(
            "/api/files/browse", params={"path": path, "kind": "scenario"},
        )

    listing = browse(str(allowed))
    assert "escape" not in {entry["name"] for entry in listing.json()["entries"]}
    for path in (
        str(allowed / ".."),
        str(allowed / "child" / ".." / ".."),
        str(outside),
        "/",
        str(allowed / "escape"),
    ):
        rejected = browse(path)
        assert rejected.status_code == 403, (path, rejected.text)
        assert "secret" not in rejected.text


def test_scenario_folder_browser_rejects_a_regular_file_path(tmp_path: Path) -> None:
    allowed = tmp_path / "allowed"
    allowed.mkdir()
    file_path = allowed / "not-a-project.yaml"
    file_path.write_text("x: 1\n", encoding="utf-8")
    client = _client(allowed_fs_roots=[str(allowed)])

    scenario = client.get(
        "/api/files/browse", params={"kind": "scenario", "path": str(file_path)},
    )
    assert scenario.status_code == 400
    assert scenario.json()["detail"] == "Not a folder or Scenario project"
    # Other artifact pickers retain their existing file-to-parent behavior.
    deployment = client.get(
        "/api/files/browse", params={"kind": "deployment", "path": str(file_path)},
    )
    assert deployment.status_code == 200, deployment.text
    assert deployment.json()["currentPath"] == str(allowed.resolve())


def test_scenario_folder_browser_is_bounded(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    allowed = tmp_path / "allowed"
    allowed.mkdir()
    for index in range(4):
        (allowed / f"cell_{index}.scenario").mkdir()
    monkeypatch.setattr(routes_filesystem, "_MAX_RETURNED_DIRECTORY_ENTRIES", 3)
    client = _client(allowed_fs_roots=[str(allowed)])

    response = client.get(
        "/api/files/browse", params={"path": str(allowed), "kind": "scenario"},
    )

    assert response.status_code == 413


class _ValidatedBody(BaseModel):
    # Module level: under postponed annotations FastAPI resolves an endpoint's
    # annotation in the module namespace, so a function-local model would be
    # read as a query parameter.
    name: str


def test_typed_refusals_name_their_producer_and_share_the_validation_shape() -> None:
    from fastapi import HTTPException

    app = create_app(
        ComposerState(),
        security=SecurityConfig(
            allowed_fs_roots=["/workspace"],
            cors_origins=["http://localhost:3000"],
        ),
    )

    def own_refusal() -> None:
        raise HTTPException(status_code=409, detail={"code": "test_refused", "message": "no"})

    def forwarded_refusal() -> None:
        raise HTTPException(
            status_code=409,
            detail={"code": "stale_generation", "message": "moved", "producer": "kernel"},
        )

    def plain_refusal() -> None:
        raise HTTPException(status_code=404, detail="Not here")

    def validated(body: _ValidatedBody) -> dict[str, str]:
        return {"name": body.name}

    for path, endpoint, methods in (
        ("/api/test/own", own_refusal, ["GET"]),
        ("/api/test/forwarded", forwarded_refusal, ["GET"]),
        ("/api/test/plain", plain_refusal, ["GET"]),
        ("/api/test/validated", validated, ["POST"]),
    ):
        app.add_api_route(path, endpoint, methods=methods)
        # Ahead of the unavailable catch-all.
        app.router.routes.insert(0, app.router.routes.pop())
    client = TestClient(app)

    assert client.get("/api/test/own").json()["detail"] == {
        "code": "test_refused", "message": "no", "producer": "composer",
    }
    assert client.get("/api/test/forwarded").json()["detail"]["producer"] == "kernel"
    assert client.get("/api/test/plain").json() == {"detail": "Not here"}
    refused = client.post("/api/test/validated", json={})
    assert refused.status_code == 422
    detail = refused.json()["detail"]
    assert detail["code"] == "invalid_request"
    assert detail["details"] == {"body.name": "missing"}
    assert detail["producer"] == "composer"
