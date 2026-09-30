"""Run the bounded schema/property-editor regressions in a real isolated browser.

Run inside the development container with an isolated Vite server and Firefox
WebDriver. No live backend or motion commands are used. The catalog fixture is
generated from the production loader and Composer serializer, not copied by hand.
"""
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

from current_workbench_browser_collector import RawWebDriver, _execute


def write_catalog(destination: Path) -> None:
    from manyforge_behavior.catalog import NodeCatalog
    from manyforge_composer.backend.routes_catalog import _entry_response

    catalog = NodeCatalog(Path(__file__).resolve().parents[2]
                          / "manyforge_behavior/resources/node_catalog.yaml")
    destination.write_text(json.dumps({
        "apiVersion": catalog.api_version,
        "contentVersion": catalog.content_version,
        "catalogId": catalog.catalog_id,
        "catalogDigest": catalog.catalog_digest,
        "environmentDigest": "isolated-editor-fixture",
        "entries": [_entry_response(entry, catalog=catalog) for entry in catalog.entries],
    }), encoding="utf-8")


def run(endpoint: str, base_url: str, repeats: int = 1, viewer: bool = False) -> dict:
    driver = RawWebDriver(endpoint, timeout_s=15)
    try:
        # WebGL qualification uses the container's virtual display; headless
        # Firefox can refuse WebGL even when software rendering is available.
        driver.create_session(firefox_args=("-no-remote",) if viewer else ("-headless", "-no-remote"))
        driver.set_window_rect(1280, 1000)
        driver.command("POST", "/url", {
            "url": base_url.rstrip("/") + "/tests/browser/editor-validation.html"
            + ("?viewer=1&manyforge_viewer_qualification=1" if viewer else ""),
        })
        samples = []
        for cycle in range(repeats):
            if cycle:
                _execute(driver, "window.repeatEditorValidation(); return true;")
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                result = _execute(driver, "return window.editorValidationTests || {};")
                if result and result["done"]:
                    if result["error"]:
                        raise AssertionError(json.dumps(result, indent=2))
                    break
                time.sleep(0.05)
            else:
                raise AssertionError(f"Editor fixture did not finish: {result}")
            # Browser scheduling/scrolling remains serviceable after the edits.
            settled = driver.command("POST", "/execute/async", {"script": """
                const done = arguments[arguments.length - 1];
                window.scrollTo(0, document.body.scrollHeight);
                requestAnimationFrame(() => {
                  window.scrollTo(0, 0);
                  requestAnimationFrame(() => done({
                    renders: window.editorValidationTests.renders,
                    reports: window.editorValidationTests.reports,
                    domNodes: document.querySelectorAll('*').length,
                  }));
                });
            """, "args": []})
            if settled["renders"] != result["renders"] or settled["reports"] != result["reports"]:
                raise AssertionError(f"Editor did not settle: {result}; {settled}")
            samples.append(settled)
        viewer_status = _execute(driver, "return window.manyforgeViewerQualification?.status() || {};") if viewer else None
        if viewer and not viewer_status.get("renderer"):
            raise AssertionError(f"Viewer renderer unavailable: {viewer_status}")
        if viewer:
            rendered = driver.command("POST", "/execute/async", {"script": """
                const done = arguments[arguments.length - 1];
                const probe = window.manyforgeViewerQualification;
                const before = probe.snapshot().performance.renderedFrames;
                document.querySelector('.viewer-stage canvas').dispatchEvent(
                  new WheelEvent('wheel', { deltaY: 20, bubbles: true, cancelable: true }));
                requestAnimationFrame(() => requestAnimationFrame(() =>
                  requestAnimationFrame(() => done({before, after: probe.snapshot().performance.renderedFrames}))));
            """, "args": []})
            if rendered["after"] <= rendered["before"] or viewer_status["errors"]:
                raise AssertionError(f"Viewer did not respond cleanly: {rendered}; {viewer_status}")
            viewer_status["interactionFrames"] = rendered
        return {**result, "cycles": samples, "viewer": viewer_status}
    finally:
        driver.delete_session()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog-output", type=Path, required=True)
    parser.add_argument("--webdriver", required=True)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--viewer", action="store_true")
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    write_catalog(args.catalog_output)
    print(json.dumps(run(args.webdriver, args.base_url, args.repeat, args.viewer), indent=2))
