"""Runtime-independent Composer transport and filesystem security tests."""

from __future__ import annotations

import os
from pathlib import Path

import pytest
from fastapi import FastAPI, HTTPException, Request
from fastapi.testclient import TestClient

from manyforge_composer.backend.security import (
    BearerAuthMiddleware,
    WS_MAX_MESSAGE_SIZE,
    check_ws_bearer_token,
    is_path_within_roots,
    normalize_allowed_roots,
    normalize_existing_file_path,
    normalize_writable_file_path,
    principal_id_for_api_token,
)


class _WebSocketAuthInput:
    def __init__(
        self,
        *,
        headers: dict[str, str] | None = None,
        query_params: dict[str, str] | None = None,
    ) -> None:
        self.headers = headers or {}
        self.query_params = query_params or {}


def test_security_limits_and_server_derived_principal_remain_available() -> None:
    assert WS_MAX_MESSAGE_SIZE == 256
    assert principal_id_for_api_token("secret-token") == (
        "token:930bbdc51b6aed5c2a5678fd"
    )
    assert principal_id_for_api_token("other-token") != principal_id_for_api_token(
        "secret-token"
    )


def test_bearer_auth_never_promotes_caller_principal_header() -> None:
    app = FastAPI()
    app.add_middleware(BearerAuthMiddleware, token="secret-token")

    @app.get("/principal")
    async def principal(request: Request) -> dict[str, str]:
        return {"principal_id": request.state.principal_id}

    response = TestClient(app).get(
        "/principal",
        headers={
            "Authorization": "Bearer secret-token",
            "X-Principal-ID": "human:caller-supplied",
        },
    )

    assert response.status_code == 200
    assert response.json() == {
        "principal_id": principal_id_for_api_token("secret-token")
    }


@pytest.mark.parametrize(
    "headers",
    [
        {},
        {"Authorization": "Bearer wrong-token"},
    ],
)
def test_bearer_auth_rejects_missing_or_wrong_credentials(
    headers: dict[str, str],
) -> None:
    app = FastAPI()
    app.add_middleware(BearerAuthMiddleware, token="secret-token")

    @app.get("/protected")
    async def protected() -> dict[str, bool]:
        return {"available": True}

    response = TestClient(app).get("/protected", headers=headers)

    assert response.status_code == 401
    assert response.json() == {"detail": "Unauthorized"}


def test_health_probe_is_the_only_public_middleware_exception() -> None:
    app = FastAPI()
    app.add_middleware(BearerAuthMiddleware, token="secret-token")

    @app.get("/api/health")
    async def health() -> dict[str, str]:
        return {"status": "ok"}

    @app.get("/api/health/details")
    async def health_details() -> dict[str, str]:
        return {"status": "ok"}

    client = TestClient(app)

    assert client.get("/api/health").json() == {"status": "ok"}
    assert client.get("/api/health/details").status_code == 401


def test_websocket_token_check_accepts_only_the_configured_credential() -> None:
    assert check_ws_bearer_token(
        _WebSocketAuthInput(headers={"authorization": "Bearer secret-token"}),
        "secret-token",
    )
    assert check_ws_bearer_token(
        _WebSocketAuthInput(query_params={"token": "secret-token"}),
        "secret-token",
    )
    assert not check_ws_bearer_token(
        _WebSocketAuthInput(query_params={"token": "wrong-token"}),
        "secret-token",
    )


def test_existing_file_must_remain_inside_an_allowed_root(tmp_path: Path) -> None:
    allowed = tmp_path / "allowed"
    allowed.mkdir()
    artifact = allowed / "program.yaml"
    artifact.write_text("name: example\n", encoding="utf-8")

    assert normalize_existing_file_path(
        str(artifact), [str(allowed)], kind_label="Program"
    ) == artifact.resolve()
    assert is_path_within_roots(artifact.resolve(), [str(allowed)]) is True

    outside = tmp_path / "outside.yaml"
    outside.write_text("name: outside\n", encoding="utf-8")
    with pytest.raises(HTTPException) as caught:
        normalize_existing_file_path(
            str(outside), [str(allowed)], kind_label="Program"
        )
    assert caught.value.status_code == 403


def test_writable_path_rejects_symlink_targets(tmp_path: Path) -> None:
    allowed = tmp_path / "allowed"
    allowed.mkdir()
    outside = tmp_path / "outside.yaml"
    outside.write_text("outside\n", encoding="utf-8")
    linked = allowed / "program.yaml"
    linked.symlink_to(outside)

    with pytest.raises(HTTPException) as caught:
        normalize_writable_file_path(
            str(linked), [str(allowed)], kind_label="Program"
        )
    assert caught.value.status_code == 400

    dangling = allowed / "dangling.yaml"
    dangling.symlink_to(tmp_path / "not-created.yaml")
    with pytest.raises(HTTPException) as caught:
        normalize_writable_file_path(
            str(dangling), [str(allowed)], kind_label="Program"
        )
    assert caught.value.status_code == 400

    fifo = allowed / "named-pipe.yaml"
    os.mkfifo(fifo)
    with pytest.raises(HTTPException) as caught:
        normalize_writable_file_path(
            str(fifo), [str(allowed)], kind_label="Program"
        )
    assert caught.value.status_code == 400
    assert "regular file" in str(caught.value.detail)


def test_allowed_roots_are_normalized_and_deduplicated(tmp_path: Path) -> None:
    roots = normalize_allowed_roots([str(tmp_path), str(tmp_path / ".")])

    assert roots == [str(tmp_path.resolve())]

    missing = tmp_path / "missing-root"
    assert normalize_allowed_roots([str(missing)]) == [str(missing.resolve())]


def test_explicit_empty_or_invalid_allowed_roots_do_not_widen_to_workspace() -> None:
    assert normalize_allowed_roots(None) == ["/workspace"]
    assert normalize_allowed_roots([]) == []
    assert normalize_allowed_roots([""]) == []
    assert normalize_allowed_roots(["invalid\x00root"]) == []
