"""Opt-in live vLLM integration tests for QueryModelNode vision requests.

These tests are intentionally disabled by default because they require a live
OpenAI-compatible model endpoint with image support. Enable them with:

    MANYFORGE_RUN_LIVE_VLM_TESTS=1 pytest -q test_query_model_live_vlm.py
"""

from __future__ import annotations

import io
import os
import time

import py_trees
import pytest

from manyforge_behavior.blackboard_namespace import read_namespace_bundle
from manyforge_behavior.deployment import ModelEndpoint, PromptTemplate
from manyforge_behavior.nodes.model import QueryModelNode


def _require_live_vlm() -> tuple[str, str, float]:
    enabled = os.environ.get("MANYFORGE_RUN_LIVE_VLM_TESTS", "").lower()
    if enabled not in {"1", "true", "yes", "on"}:
        pytest.skip("set MANYFORGE_RUN_LIVE_VLM_TESTS=1 to enable live vLLM tests")

    try:
        import httpx
    except ImportError as exc:  # pragma: no cover - environment guard
        pytest.skip(f"httpx not available: {exc}")

    base_url = os.environ.get("MANYFORGE_LIVE_VLM_BASE_URL", "http://localhost:8000/v1").rstrip("/")
    timeout_s = float(os.environ.get("MANYFORGE_LIVE_VLM_TIMEOUT_S", "90"))
    try:
        response = httpx.get(f"{base_url}/models", timeout=5.0)
        response.raise_for_status()
        data = response.json()
    except Exception as exc:  # pragma: no cover - environment guard
        pytest.skip(f"live vLLM endpoint not available at {base_url}: {exc}")

    model_id = os.environ.get("MANYFORGE_LIVE_VLM_MODEL_ID")
    if not model_id:
        models = data.get("data", [])
        if not models:
            pytest.skip(f"no models advertised by {base_url}")
        model_id = models[0]["id"]

    return base_url, model_id, timeout_s


def _make_test_image_bytes() -> bytes:
    try:
        from PIL import Image, ImageDraw
    except ImportError as exc:  # pragma: no cover - environment guard
        pytest.skip(f"Pillow not available: {exc}")

    img = Image.new("RGB", (320, 240), color=(32, 32, 32))
    draw = ImageDraw.Draw(img)
    draw.rectangle((0, 0, 159, 239), fill=(220, 20, 60))
    draw.rectangle((160, 0, 319, 239), fill=(35, 90, 220))
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def _tick_until_done(tree: py_trees.trees.BehaviourTree, timeout_s: float) -> py_trees.common.Status:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        tree.tick()
        if tree.root.status != py_trees.common.Status.RUNNING:
            return tree.root.status
        time.sleep(0.1)
    return tree.root.status


@pytest.fixture(autouse=True)
def reset_blackboard():
    py_trees.blackboard.Blackboard.enable_activity_stream()
    yield
    py_trees.blackboard.Blackboard.clear()


def test_live_query_model_with_scene_snapshot():
    """Exercise the real OpenAI-compatible vision endpoint through QueryModelNode."""
    base_url, model_id, timeout_s = _require_live_vlm()
    image_bytes = _make_test_image_bytes()

    endpoint = ModelEndpoint(
        alias="live_vlm",
        provider="openai",
        model_id=model_id,
        base_url=base_url,
        max_tokens=256,
    )
    template = PromptTemplate(
        template_id="vision_color_probe_v1",
        version="1",
        template_text=(
            "Return JSON only with keys left_is_red, right_is_blue, confidence. "
            "Set the booleans from the image only."
        ),
        output_schema={
            "type": "object",
            "required": ["left_is_red", "right_is_blue", "confidence"],
            "properties": {
                "left_is_red": {"type": "boolean"},
                "right_is_blue": {"type": "boolean"},
                "confidence": {"type": "number"},
            },
        },
    )
    node = QueryModelNode(
        name="live_vlm_probe",
        model_endpoint_alias=endpoint.alias,
        prompt_template_id=template.template_id,
        output_key_prefix="live_vlm/probe",
        timeout_s=timeout_s,
        min_confidence=0.5,
        include_scene_snapshot=True,
        scene_snapshot_width=320,
        scene_snapshot_height=240,
        scene_snapshot_format="png",
        endpoint=endpoint,
        template=template,
        scene_snapshot_provider=lambda width, height, format: image_bytes,
    )

    tree = py_trees.trees.BehaviourTree(root=node)
    tree.setup()

    status = _tick_until_done(tree, timeout_s=timeout_s + 5.0)
    assert status == py_trees.common.Status.SUCCESS

    bb = py_trees.blackboard.Client(name="live_vlm_reader")
    bundle = read_namespace_bundle(bb, "live_vlm/probe")
    assert bundle["value"]["left_is_red"] is True
    assert bundle["value"]["right_is_blue"] is True
    assert float(bundle["meta"]["confidence"]) >= 0.5
    assert bundle["meta"]["model_version"]
