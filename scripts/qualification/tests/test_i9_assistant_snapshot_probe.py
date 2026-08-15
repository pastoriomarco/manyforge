from __future__ import annotations

from dataclasses import asdict
import importlib.util
from pathlib import Path
import sys
from types import ModuleType

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]


def load_probe() -> ModuleType:
    path = REPO_ROOT / "scripts/qualification/run_i9_assistant_snapshot_probe.py"
    spec = importlib.util.spec_from_file_location("i9_snapshot_probe", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def probe() -> ModuleType:
    return load_probe()


def request(probe: ModuleType) -> dict[str, object]:
    return {
        "type": "snapshot_request",
        "requestId": "a" * 32,
        "width": 640,
        "height": 480,
        "mime": "image/jpeg",
        "timeoutMs": 1999,
        "requiredFrame": {
            "ownerEpoch": "headless.epoch.1",
            "generation": 19,
        },
    }


def result(probe: ModuleType) -> dict[str, object]:
    return asdict(
        probe.SnapshotProbeResult(
            schema_version=probe.RESULT_SCHEMA,
            passed=True,
            fixture_kind="owner_fenced_transport_jpeg",
            viewer_owner_epoch="headless.epoch.1",
            viewer_generation=19,
            image_sha256=(
                "sha256:" + probe.sha256(probe._JPEG).hexdigest()  # noqa: SLF001
            ),
            image_bytes=len(probe._JPEG),  # noqa: SLF001
            snapshot_completed_delta=1,
            snapshot_rejected_delta=0,
            model_requests=1,
            model_payload_sha256="sha256:" + "b" * 64,
            elapsed_s=0.25,
        )
    )


def test_snapshot_request_parser_requires_exact_owner_fenced_wire(
    probe: ModuleType,
) -> None:
    parsed = probe.parse_snapshot_request(request(probe))
    assert parsed.owner_epoch == "headless.epoch.1"
    assert parsed.generation == 19

    crossed = request(probe)
    crossed["requiredFrame"] = {
        "ownerEpoch": "../other",
        "generation": 19,
    }
    with pytest.raises(probe.SnapshotProbeError, match="values"):
        probe.parse_snapshot_request(crossed)

    extra = request(probe)
    extra["ownerId"] = "invented"
    with pytest.raises(probe.SnapshotProbeError, match="fields"):
        probe.parse_snapshot_request(extra)


def test_composer_url_is_one_credential_free_origin(probe: ModuleType) -> None:
    assert probe._composer_urls("http://127.0.0.1:9000") == (  # noqa: SLF001
        "http://127.0.0.1:9000/api/scene/snapshot/render",
        "http://127.0.0.1:9000/api/scene/snapshot/status",
        "http://127.0.0.1:9000/api/scene/snapshot/upload",
        "ws://127.0.0.1:9000/ws/scene/snapshot",
    )
    for invalid in (
        "http://user:secret@127.0.0.1:9000",
        "http://127.0.0.1:9000/path",
        "file:///tmp/composer",
    ):
        with pytest.raises(probe.SnapshotProbeError):
            probe._composer_urls(invalid)  # noqa: SLF001


def test_result_verdict_is_recomputed_from_exact_counters(
    probe: ModuleType,
) -> None:
    value = result(probe)
    assert probe.validate_probe_result(value) == value

    invented = result(probe)
    invented["snapshot_completed_delta"] = 0
    with pytest.raises(probe.SnapshotProbeError, match="verdict"):
        probe.validate_probe_result(invented)

    rejected = result(probe)
    rejected["snapshot_rejected_delta"] = 1
    rejected["passed"] = False
    assert probe.validate_probe_result(rejected)["passed"] is False
