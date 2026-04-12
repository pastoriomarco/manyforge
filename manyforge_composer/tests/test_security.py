"""Security tests for ManyForge Composer backend."""
from __future__ import annotations

from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.security import (
    SecurityConfig,
    is_path_within_roots,
    normalize_allowed_roots,
)
from manyforge_composer.backend.state import ComposerState

_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")


def _make_app(
    *,
    api_token: str | None = None,
    cors_origins: list[str] | None = None,
    allowed_fs_roots: list[str] | None = None,
) -> tuple[TestClient, ComposerState]:
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    sec = SecurityConfig(
        cors_origins=cors_origins or ["http://localhost:3000"],
        api_token=api_token,
        allowed_fs_roots=allowed_fs_roots or ["/workspace"],
    )
    app = create_app(state, security=sec)
    return TestClient(app), state


# =========================================================================
# Bearer token authentication
# =========================================================================

class TestBearerAuth:
    def test_no_token_configured_allows_all(self):
        client, _ = _make_app(api_token=None)
        resp = client.get("/api/health")
        assert resp.status_code == 200

    def test_token_configured_rejects_unauthenticated(self):
        client, _ = _make_app(api_token="secret-token-42")
        resp = client.get("/api/kernel/snapshot")
        assert resp.status_code == 401
        assert resp.json()["detail"] == "Unauthorized"

    def test_token_configured_allows_valid_bearer(self):
        client, _ = _make_app(api_token="secret-token-42")
        resp = client.get(
            "/api/kernel/snapshot",
            headers={"Authorization": "Bearer secret-token-42"},
        )
        assert resp.status_code == 200

    def test_token_configured_rejects_wrong_bearer(self):
        client, _ = _make_app(api_token="secret-token-42")
        resp = client.get(
            "/api/kernel/snapshot",
            headers={"Authorization": "Bearer wrong-token"},
        )
        assert resp.status_code == 401

    def test_health_always_public(self):
        client, _ = _make_app(api_token="secret-token-42")
        resp = client.get("/api/health")
        assert resp.status_code == 200

    def test_websocket_rejected_without_token(self):
        client, _ = _make_app(api_token="secret-token-42")
        with client:
            with pytest.raises(Exception):
                with client.websocket_connect("/ws/state") as ws:
                    ws.receive_json()

    def test_websocket_accepted_with_query_token(self):
        client, _ = _make_app(api_token="secret-token-42")
        with client:
            with client.websocket_connect("/ws/state?token=secret-token-42") as ws:
                data = ws.receive_json()
                assert "state" in data


# =========================================================================
# CORS
# =========================================================================

class TestCors:
    def test_allowed_origin_reflected(self):
        client, _ = _make_app(cors_origins=["http://localhost:3000"])
        resp = client.options(
            "/api/health",
            headers={
                "Origin": "http://localhost:3000",
                "Access-Control-Request-Method": "GET",
            },
        )
        assert resp.headers.get("access-control-allow-origin") == "http://localhost:3000"

    def test_disallowed_origin_not_reflected(self):
        client, _ = _make_app(cors_origins=["http://localhost:3000"])
        resp = client.options(
            "/api/health",
            headers={
                "Origin": "http://evil.example.com",
                "Access-Control-Request-Method": "GET",
            },
        )
        assert resp.headers.get("access-control-allow-origin") != "http://evil.example.com"


# =========================================================================
# Filesystem path traversal protection
# =========================================================================

class TestPathTraversal:
    def test_is_path_within_roots_allows_child(self, tmp_path):
        assert is_path_within_roots(tmp_path / "subdir", [str(tmp_path)])

    def test_is_path_within_roots_blocks_parent(self, tmp_path):
        assert not is_path_within_roots(tmp_path.parent, [str(tmp_path)])

    def test_is_path_within_roots_blocks_etc_passwd(self):
        assert not is_path_within_roots(Path("/etc/passwd"), ["/workspace"])

    def test_normalize_allowed_roots_does_not_silently_widen_to_workspace(self, tmp_path):
        missing = tmp_path / "missing-root"
        roots = normalize_allowed_roots([str(missing)])
        assert roots == [str(missing.resolve())]

    def test_browse_rejects_path_outside_roots(self, tmp_path):
        outside = tmp_path / "outside"
        outside.mkdir()
        client, _ = _make_app(allowed_fs_roots=["/workspace"])
        resp = client.get("/api/files/browse", params={"path": str(outside)})
        assert resp.status_code == 403
        assert "outside allowed roots" in resp.json()["detail"]

    def test_browse_allows_path_inside_roots(self, tmp_path):
        (tmp_path / "test.program.yaml").write_text("x: 1\n")
        client, _ = _make_app(allowed_fs_roots=[str(tmp_path)])
        resp = client.get("/api/files/browse", params={"path": str(tmp_path)})
        assert resp.status_code == 200

    def test_browse_parent_stops_at_root_boundary(self, tmp_path):
        subdir = tmp_path / "sub"
        subdir.mkdir()
        client, _ = _make_app(allowed_fs_roots=[str(tmp_path)])
        resp = client.get("/api/files/browse", params={"path": str(subdir)})
        assert resp.status_code == 200
        data = resp.json()
        assert data["parentPath"] == str(tmp_path)

        # Browsing the root itself should have no parent
        resp2 = client.get("/api/files/browse", params={"path": str(tmp_path)})
        data2 = resp2.json()
        assert data2["parentPath"] is None

    def test_browse_rejects_symlink_escape(self, tmp_path):
        jail = tmp_path / "jail"
        jail.mkdir()
        secret = tmp_path / "secret"
        secret.mkdir()
        (secret / "data.program.yaml").write_text("x: 1\n")
        link = jail / "escape"
        link.symlink_to(secret)

        client, _ = _make_app(allowed_fs_roots=[str(jail)])
        resp = client.get("/api/files/browse", params={"path": str(link)})
        assert resp.status_code == 403

    def test_program_load_rejects_path_outside_roots(self, tmp_path):
        outside = tmp_path / "outside.program.yaml"
        outside.write_text("api_version: manyforge.program.v0\n")
        client, _ = _make_app(allowed_fs_roots=["/workspace"])
        resp = client.post("/api/program/load", json={"path": str(outside)})
        assert resp.status_code == 403
        assert "outside allowed roots" in resp.json()["detail"]

    def test_deployment_load_rejects_path_outside_roots(self, tmp_path):
        outside = tmp_path / "outside.deployment.yaml"
        outside.write_text("api_version: manyforge.deployment.v0\n")
        client, _ = _make_app(allowed_fs_roots=["/workspace"])
        resp = client.post("/api/deployment/load", json={"path": str(outside)})
        assert resp.status_code == 403
        assert "outside allowed roots" in resp.json()["detail"]

    def test_program_save_rejects_path_outside_roots(self, tmp_path):
        client, _ = _make_app(allowed_fs_roots=["/workspace"])
        program_path = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml")
        deployment_path = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "ur5_roboplan.deployment.yaml")
        client.post("/api/program/load", json={"path": program_path, "deployment_path": deployment_path})
        outside = tmp_path / "saved.program.yaml"
        resp = client.post("/api/program/save", json={"path": str(outside)})
        assert resp.status_code == 403
        assert "outside allowed roots" in resp.json()["detail"]

    def test_deployment_save_rejects_path_outside_roots(self, tmp_path):
        client, _ = _make_app(allowed_fs_roots=["/workspace"])
        program_path = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml")
        deployment_path = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "ur5_roboplan.deployment.yaml")
        client.post("/api/program/load", json={"path": program_path, "deployment_path": deployment_path})
        outside = tmp_path / "saved.deployment.yaml"
        resp = client.post("/api/deployment/save", json={"path": str(outside)})
        assert resp.status_code == 403
        assert "outside allowed roots" in resp.json()["detail"]

    def test_program_save_rejects_symlink_target(self, tmp_path):
        outside_target = tmp_path / "outside.program.yaml"
        outside_target.write_text("placeholder\n")
        allowed_root = tmp_path / "allowed"
        allowed_root.mkdir()
        link = allowed_root / "program-link.program.yaml"
        link.symlink_to(outside_target)

        client, _ = _make_app(allowed_fs_roots=[str(allowed_root), str(_REPO_ROOT)])
        program_path = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml")
        deployment_path = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "ur5_roboplan.deployment.yaml")
        load_resp = client.post("/api/program/load", json={"path": program_path, "deployment_path": deployment_path})
        assert load_resp.status_code == 200

        resp = client.post("/api/program/save", json={"path": str(link)})
        assert resp.status_code == 400
        assert "must not be a symlink" in resp.json()["detail"]


# =========================================================================
# Error message sanitization
# =========================================================================

class TestErrorSanitization:
    def test_load_program_invalid_path_no_stack_trace(self):
        client, _ = _make_app()
        resp = client.post("/api/program/load", json={"path": "/nonexistent/path.program.yaml"})
        assert resp.status_code == 403
        detail = resp.json()["detail"]
        assert "Traceback" not in detail
        assert "/nonexistent" not in detail

    def test_filesystem_error_no_path_leak(self):
        client, _ = _make_app(allowed_fs_roots=["/workspace"])
        resp = client.get("/api/files/browse", params={"path": "/etc/shadow"})
        assert resp.status_code == 403
        detail = resp.json()["detail"]
        assert "/etc/shadow" not in detail


# =========================================================================
# WebSocket message validation
# =========================================================================

class TestWebSocketValidation:
    def test_oversized_message_closes_connection(self):
        client, _ = _make_app()
        with client:
            with client.websocket_connect("/ws/state") as ws:
                # Drain the initial broadcast
                ws.receive_json()
                # Send a message exceeding the limit
                ws.send_text("x" * 1000)
                # The server should close the connection
                with pytest.raises(Exception):
                    ws.receive_json()

    def test_ping_pong_still_works(self):
        client, _ = _make_app()
        with client:
            with client.websocket_connect("/ws/state") as ws:
                ws.receive_json()
                ws.send_text("ping")
                resp = ws.receive_text()
                assert resp == "pong"


# =========================================================================
# Velocity scale bounds
# =========================================================================

class TestVelocityScaleBounds:
    """Pydantic rejects out-of-range values with 422 before the route runs."""

    def test_velocity_rejects_negative(self):
        client, _ = _make_app()
        resp = client.put("/api/cycle/velocity-scale", json={"value": -0.5})
        assert resp.status_code == 422

    def test_velocity_rejects_above_one(self):
        client, _ = _make_app()
        resp = client.put("/api/cycle/velocity-scale", json={"value": 1.5})
        assert resp.status_code == 422

    def test_velocity_accepts_valid_range(self):
        """Needs a loaded program to succeed past the 409 guard."""
        client, state = _make_app()
        program_path = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml")
        deployment_path = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "ur5_roboplan.deployment.yaml")
        client.post("/api/program/load", json={"path": program_path, "deployment_path": deployment_path})
        resp = client.put("/api/cycle/velocity-scale", json={"value": 0.5})
        assert resp.status_code == 200
        assert resp.json()["velocityScale"] == 0.5
