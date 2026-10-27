from __future__ import annotations

import http.server
import importlib.util
import json
from pathlib import Path
import threading

import pytest

from manyforge_ros_bringup import ai_model_probe as probe

_REPOSITORY_ROOT = Path(__file__).resolve().parents[2]


def _site(**overrides) -> dict:
    doc = {
        "api_version": probe.SITE_API_VERSION,
        "cell_id": "cell",
        "component_id": "assistant_model",
        "stream_id": "endpoint",
        "base_url": "http://127.0.0.1:8000/v1/",
        "model": "gemma",
        "api_key_env": None,
        "probe_period_ms": 2000,
    }
    doc.update(overrides)
    return doc


def _load(tmp_path: Path, **overrides) -> probe.ProbeSite:
    path = tmp_path / "ai_model.json"
    path.write_text(json.dumps(_site(**overrides)), encoding="utf-8")
    return probe.load_site(path)


class _Sink:
    def __init__(self) -> None:
        self.samples = 0

    def record(self, stamp_ns: int | None = None) -> None:
        self.samples += 1


class _Logger:
    def __init__(self) -> None:
        self.lines: list[str] = []

    def info(self, line: str) -> None:
        self.lines.append(line)

    def warning(self, line: str) -> None:
        self.lines.append(line)


def _listing(*ids: str):
    body = json.dumps({"object": "list", "data": [{"id": i} for i in ids]}).encode()
    return lambda url, headers, timeout_s: (200, body)


def test_site_document_is_loaded_and_validated(tmp_path: Path) -> None:
    site = _load(tmp_path, api_key_env="MODEL_TOKEN")
    assert site.base_url == "http://127.0.0.1:8000/v1"
    assert site.api_key_env == "MODEL_TOKEN"
    assert site.probe_period_s == 2.0
    for bad in (
        {"api_version": "other"},
        {"base_url": "ftp://host"},
        {"model": ""},
        {"api_key_env": "1BAD"},
        {"probe_period_ms": 0},
        {"probe_period_ms": True},
    ):
        with pytest.raises(ValueError):
            _load(tmp_path, **bad)


def test_missing_token_variable_is_a_config_error(tmp_path: Path) -> None:
    site = _load(tmp_path, api_key_env="MODEL_TOKEN")
    with pytest.raises(ValueError, match="MODEL_TOKEN"):
        probe.bearer_token(site, {})
    assert probe.bearer_token(site, {"MODEL_TOKEN": "secret"}) == "secret"
    assert probe.bearer_token(_load(tmp_path), {}) is None


def test_probe_classifies_served_unreachable_and_not_served(tmp_path: Path) -> None:
    site = _load(tmp_path)
    seen = {}

    def fetch(url, headers, timeout_s):
        seen.update(url=url, headers=dict(headers), timeout_s=timeout_s)
        return _listing("other", "gemma")(url, headers, timeout_s)

    assert probe.probe_endpoint(site, "secret", fetch) == (None, "")
    assert seen["url"] == "http://127.0.0.1:8000/v1/models"
    assert seen["headers"]["Authorization"] == "Bearer secret"
    assert seen["timeout_s"] == 2.0

    def refused(url, headers, timeout_s):
        raise ConnectionRefusedError()

    assert probe.probe_endpoint(site, None, refused)[0] == probe.UNREACHABLE
    assert probe.probe_endpoint(site, None, lambda *a: (503, b""))[0] == probe.UNREACHABLE
    assert probe.probe_endpoint(site, None, _listing("other"))[0] == probe.NOT_SERVED
    assert probe.probe_endpoint(site, None, lambda *a: (200, b"[]"))[0] == probe.NOT_SERVED


def test_loop_records_only_when_served_and_logs_changes(tmp_path: Path) -> None:
    site = _load(tmp_path)
    sink, logger = _Sink(), _Logger()
    answers = [_listing("gemma"), _listing("gemma"), _listing(), _listing(), _listing("gemma")]
    loop = probe.ProbeLoop(site, {}, sink, logger, lambda *a: answers.pop(0)(*a))
    for _ in range(5):
        loop.step()
    assert sink.samples == 3
    assert len(logger.lines) == 3
    assert logger.lines[1].startswith(probe.NOT_SERVED)


def test_real_http_fetch_against_a_local_server(tmp_path: Path) -> None:
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            authorized = self.headers.get("Authorization") == "Bearer secret"
            body = json.dumps({"data": [{"id": "gemma"}]}).encode()
            self.send_response(200 if authorized and self.path == "/v1/models" else 401)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        site = _load(tmp_path, base_url=f"http://127.0.0.1:{server.server_port}/v1")
        assert probe.probe_endpoint(site, "secret") == (None, "")
        assert probe.probe_endpoint(site, "wrong")[0] == probe.UNREACHABLE
    finally:
        server.shutdown()
        server.server_close()


def test_missing_token_variable_keeps_the_stream_stale(tmp_path: Path) -> None:
    site = _load(tmp_path, api_key_env="MANYFORGE_TEST_ABSENT")
    sink, logger = _Sink(), _Logger()
    loop = probe.ProbeLoop(site, {}, sink, logger, _listing("gemma"))
    loop.step()
    assert sink.samples == 0
    assert logger.lines[0].startswith(probe.TOKEN_UNAVAILABLE)
    probe.ProbeLoop(site, {"MANYFORGE_TEST_ABSENT": "t"}, sink, logger, _listing("gemma")).step()
    assert sink.samples == 1


def test_launch_runs_the_probe_with_the_site_document() -> None:
    pytest.importorskip("launch_ros")
    spec = importlib.util.spec_from_file_location(
        "ai_model_launch", _REPOSITORY_ROOT / "manyforge_ros_bringup/launch/ai_model.launch.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    kwargs = module.node_kwargs("/run/ai_model.json")
    assert kwargs["executable"] == "ai_model_probe"
    assert kwargs["parameters"] == [{"site_config_path": "/run/ai_model.json"}]
    with pytest.raises(ValueError):
        module.node_kwargs("relative.json")
