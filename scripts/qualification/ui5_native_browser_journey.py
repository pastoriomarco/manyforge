"""Exercise the bounded O1.5-UI5 native browser journey.

The caller owns the native development stack and geckodriver.  This script
creates one isolated Firefox session, uses only visible Composer controls, and
records a compact result.  It never writes a Program or Deployment artifact.
Each Robot Control mode is first previewed and reverted without motion, then
previewed, approved, observed, and allowed to settle through the native owner.
The Program transport journey separately exercises Run/Stop/Resume/Reset.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import time
from typing import Any, Callable
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin
from urllib.request import Request, urlopen

from current_workbench_browser_collector import (
    CollectorError,
    QUALIFICATION_GLOBAL,
    RawWebDriver,
    _execute,
    _url_with_opt_in,
)


RESULT_SCHEMA = "manyforge.ui5.native-browser-journey.v1"
POLL_INTERVAL_S = 0.05
DEFAULT_TIMEOUT_S = 30.0
TERMINAL_TIMEOUT_S = 75.0


DOM_STATE_SCRIPT = f"""return (() => {{
  const run = document.querySelector('.viewer-transport .transport-run');
  const stop = document.querySelector('.viewer-transport .transport-stop-primary');
  const reset = document.querySelector('.viewer-transport .transport-reset');
  const velocity = document.querySelector('.viewer-transport .transport-velocity input');
  const qualification = window.{QUALIFICATION_GLOBAL}?.snapshot?.() ?? null;
  const deployment = document.querySelector('.deployment-status-button');
  const runtime = document.querySelector('.runtime-observability-surface');
  const stage = document.querySelector('.viewer-stage');
  const text = document.body?.innerText ?? '';
  return {{
    runText: run?.textContent?.trim() ?? null,
    runDisabled: run instanceof HTMLButtonElement ? run.disabled : null,
    stopText: stop?.textContent?.trim() ?? null,
    stopDisabled: stop instanceof HTMLButtonElement ? stop.disabled : null,
    resetText: reset?.textContent?.trim() ?? null,
    resetDisabled: reset instanceof HTMLButtonElement ? reset.disabled : null,
    velocityValue: velocity instanceof HTMLInputElement ? velocity.value : null,
    velocityDisabled: velocity instanceof HTMLInputElement ? velocity.disabled : null,
    velocityFocused: document.activeElement === velocity,
    velocityIdentityStable: window.__manyforgeUi5Velocity === undefined
      || window.__manyforgeUi5Velocity === velocity,
    deploymentReady: deployment?.classList.contains('readiness-ready') === true,
    deploymentConnected: deployment?.dataset.backendConnected === 'true',
    deploymentLabel: deployment?.getAttribute('aria-label') ?? null,
    activeWorkspace: document.querySelector(
      '.workspace-switcher [data-workspace][aria-pressed="true"]',
    )?.getAttribute('data-workspace') ?? null,
    runtimePresent: runtime !== null,
    runtimeExpanded: runtime?.classList.contains('expanded') ?? false,
    runtimeSummary: runtime?.querySelector(
      '.runtime-observability-rail-summary',
    )?.textContent?.trim() ?? null,
    runtimeGeometry: runtime === null || stage === null ? null : (() => {{
      const panel = runtime.getBoundingClientRect();
      const viewport = stage.getBoundingClientRect();
      return {{
        leftInset: panel.left - viewport.left,
        rightInset: viewport.right - panel.right,
        topInset: panel.top - viewport.top,
        height: panel.height,
      }};
    }})(),
    retryHealthVisible: /Retry health/i.test(text),
    measuredStateVisible: /MEASURED STATE/i.test(text),
    canvasCount: document.querySelectorAll('.viewer-stage canvas').length,
    scenePresentation: stage?.getAttribute('data-scene-presentation') ?? null,
    clientErrors: qualification?.status?.errors ?? [],
    socketCount: qualification?.status?.headless?.activeSockets ?? null,
    socketCreations: qualification?.status?.headless?.socketCreations ?? null,
    modelBuilds: qualification?.performance?.modelBuilds ?? null,
  }};
}})();"""


ROBOT_STATE_SCRIPT = r"""return (() => {
  const panel = document.querySelector('[data-testid="robot-control-panel"]');
  const buttons = panel === null ? [] : [...panel.querySelectorAll('button')];
  const approve = panel?.querySelector('.approve-button');
  const revert = buttons.find((button) => button.textContent?.trim() === 'Revert');
  const stop = buttons.find((button) => button.textContent?.trim() === 'Stop operation');
  const phase = panel?.querySelector('.robot-control-phase');
  const operation = panel?.querySelector('.robot-control-operation');
  return {
    present: panel !== null,
    mode: buttons.find((button) => (
      button.closest('.robot-control-modes') !== null
      && button.getAttribute('aria-pressed') === 'true'
    ))?.textContent?.trim() ?? null,
    approveAvailable: approve instanceof HTMLButtonElement && !approve.disabled,
    revertAvailable: revert instanceof HTMLButtonElement && !revert.disabled,
    stopAvailable: stop instanceof HTMLButtonElement && !stop.disabled,
    phase: phase?.textContent?.trim() ?? null,
    operationText: operation?.textContent?.replace(/\s+/g, ' ').trim() ?? null,
    ghostPresent: panel?.querySelector('.robot-control-ghost-joints') !== null,
    previewReceiptPresent: panel?.querySelector('.robot-control-tcp') !== null,
    tcpToolsPresent: panel?.querySelector(
      '[aria-label="TCP transform controls"]',
    ) !== null,
    error: panel?.querySelector('[role="alert"]')?.textContent?.trim() ?? null,
  };
})();"""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise CollectorError(message)


def _dom_state(driver: RawWebDriver) -> dict[str, Any]:
    value = _execute(driver, DOM_STATE_SCRIPT)
    if not isinstance(value, dict):
        raise CollectorError("UI5 browser state is malformed")
    return value


def _robot_state(driver: RawWebDriver) -> dict[str, Any]:
    value = _execute(driver, ROBOT_STATE_SCRIPT)
    if not isinstance(value, dict):
        raise CollectorError("Robot Control browser state is malformed")
    return value


def _wait(
    read: Callable[[], dict[str, Any]],
    predicate: Callable[[dict[str, Any]], bool],
    label: str,
    timeout_s: float,
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    deadline = time.monotonic() + timeout_s
    trace: list[dict[str, Any]] = []
    while True:
        current = read()
        trace.append(current)
        if predicate(current):
            return current, trace
        if time.monotonic() >= deadline:
            raise CollectorError(
                f"timed out waiting for {label}: {json.dumps(current, sort_keys=True)}"
            )
        time.sleep(POLL_INTERVAL_S)


def _click_selector(driver: RawWebDriver, selector: str, label: str) -> None:
    script = """return (() => {
      const node = document.querySelector(%s);
      if (!(node instanceof HTMLButtonElement) || node.disabled) return false;
      node.click();
      return true;
    })();""" % json.dumps(selector)
    if _execute(driver, script) is not True:
        raise CollectorError(f"{label} is not available")


def _click_button_text(
    driver: RawWebDriver,
    container: str,
    text: str,
    label: str,
) -> None:
    script = """return (() => {
      const root = document.querySelector(%s);
      const node = [...(root?.querySelectorAll('button') ?? [])].find(
        (candidate) => candidate.textContent?.trim() === %s,
      );
      if (!(node instanceof HTMLButtonElement) || node.disabled) return false;
      node.click();
      return true;
    })();""" % (json.dumps(container), json.dumps(text))
    if _execute(driver, script) is not True:
        raise CollectorError(f"{label} is not available")


def _select_workspace(driver: RawWebDriver, workspace: str) -> None:
    _click_selector(
        driver,
        f'[data-workspace="{workspace}"]',
        f"{workspace} workspace",
    )
    _wait(
        lambda: _dom_state(driver),
        lambda state: state["activeWorkspace"] == workspace,
        f"{workspace} workspace selection",
        DEFAULT_TIMEOUT_S,
    )


def _set_range(driver: RawWebDriver, value: float) -> dict[str, Any]:
    _require(math.isfinite(value) and 0.0 <= value <= 1.0, "invalid velocity value")
    script = """return (() => {
      const input = document.querySelector(
        '.viewer-transport .transport-velocity input',
      );
      if (!(input instanceof HTMLInputElement) || input.disabled) return null;
      window.__manyforgeUi5Velocity ??= input;
      input.focus();
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(
        input,
        %s,
      );
      input.dispatchEvent(new Event('input', { bubbles: true }));
      input.dispatchEvent(new Event('change', { bubbles: true }));
      return {
        value: input.value,
        identityStable: input === window.__manyforgeUi5Velocity,
        focused: document.activeElement === input,
      };
    })();""" % json.dumps(str(value))
    result = _execute(driver, script)
    if not isinstance(result, dict):
        raise CollectorError("runtime velocity slider is unavailable")
    return result


def _set_number_input(
    driver: RawWebDriver,
    aria_label: str,
    offset: float,
) -> dict[str, float]:
    script = """return (() => {
      const input = document.querySelector(%s);
      if (!(input instanceof HTMLInputElement) || input.disabled) return null;
      const before = Number(input.value);
      const after = before + %s;
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(
        input,
        String(after),
      );
      input.dispatchEvent(new Event('input', { bubbles: true }));
      return { before, after };
    })();""" % (
        json.dumps(f'[aria-label="{aria_label}"]'),
        json.dumps(offset),
    )
    result = _execute(driver, script)
    if not isinstance(result, dict):
        raise CollectorError(f"{aria_label} is unavailable")
    return {"before": float(result["before"]), "after": float(result["after"])}


def _set_first_move_source(driver: RawWebDriver) -> str:
    value = _execute(driver, """return (() => {
      const select = document.querySelector('.robot-control-move-to select');
      if (!(select instanceof HTMLSelectElement) || select.disabled) return null;
      // The observed TCP source is intentionally a no-op until an offset is
      // authored.  Use a retained pose so this journey proves a real preview.
      const option = [...select.options].find(
        (candidate) => candidate.value === 'node:move_to_pick_approach',
      );
      if (option === undefined) return null;
      Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set.call(
        select,
        option.value,
      );
      select.dispatchEvent(new Event('change', { bubbles: true }));
      return option.textContent?.trim() ?? option.value;
    })();""")
    if not isinstance(value, str) or not value:
        raise CollectorError("Move To has no current eligible source")
    return value


def _set_gripper_preview_target(driver: RawWebDriver) -> dict[str, float]:
    value = _execute(driver, """return (() => {
      const input = document.querySelector('[aria-label="Gripper target value"]');
      if (!(input instanceof HTMLInputElement) || input.disabled) return null;
      const before = Number(input.value);
      const minimum = Number(input.min || 0);
      const maximum = Number(input.max || 0.0081);
      const midpoint = (minimum + maximum) / 2;
      const after = Math.abs(before - midpoint) > 0.0001 ? midpoint : minimum;
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(
        input,
        String(after),
      );
      input.dispatchEvent(new Event('input', { bubbles: true }));
      return { before, after };
    })();""")
    if not isinstance(value, dict):
        raise CollectorError("Gripper target is unavailable")
    return {"before": float(value["before"]), "after": float(value["after"])}


def _http_json(composer_url: str, path: str) -> dict[str, Any]:
    request = Request(urljoin(composer_url.rstrip("/") + "/", path.lstrip("/")))
    try:
        with urlopen(request, timeout=5.0) as response:
            raw = response.read(2_000_001)
    except (HTTPError, URLError, TimeoutError, OSError) as error:
        raise CollectorError(f"Composer {path} read failed") from error
    if len(raw) > 2_000_000:
        raise CollectorError(f"Composer {path} response is oversized")
    try:
        value = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CollectorError(f"Composer {path} response is malformed") from error
    if not isinstance(value, dict):
        raise CollectorError(f"Composer {path} response is unsupported")
    return value


def _control_observation(composer_url: str) -> dict[str, Any]:
    response = _http_json(composer_url, "/api/execution/control-state")
    state = response.get("control_state")
    if not isinstance(state, dict):
        raise CollectorError("execution control-state projection is missing")
    motion = state.get("observed_motion")
    gripper = state.get("observed_gripper")
    if not isinstance(motion, dict) or not isinstance(gripper, dict):
        raise CollectorError("native observed control state is incomplete")
    positions = motion.get("positions_rad")
    tcp = motion.get("current_tcp_pose")
    gripper_position = gripper.get("position")
    if (
        not isinstance(positions, list)
        or not all(isinstance(value, (int, float)) for value in positions)
        or not isinstance(tcp, dict)
        or not isinstance(tcp.get("position_m"), dict)
        or not isinstance(gripper_position, dict)
    ):
        raise CollectorError("native observed control-state values are malformed")
    tcp_position = tcp["position_m"]
    gripper_value = gripper_position.get(
        "value_m",
        gripper_position.get("value_rad"),
    )
    if not isinstance(gripper_value, (int, float)):
        raise CollectorError("native observed gripper position is malformed")
    return {
        "motionSequence": int(motion["sequence"]),
        "positionsRad": [float(value) for value in positions],
        "tcpPositionM": [
            float(tcp_position["x_m"]),
            float(tcp_position["y_m"]),
            float(tcp_position["z_m"]),
        ],
        "gripperSequence": int(gripper["sequence"]),
        "gripperPosition": float(gripper_value),
    }


def _observation_delta(
    before: dict[str, Any],
    after: dict[str, Any],
    mode: str,
) -> float:
    if mode == "Gripper":
        return abs(after["gripperPosition"] - before["gripperPosition"])
    joint_delta = max(
        abs(right - left)
        for left, right in zip(before["positionsRad"], after["positionsRad"], strict=True)
    )
    tcp_delta = math.sqrt(sum(
        (right - left) ** 2
        for left, right in zip(before["tcpPositionM"], after["tcpPositionM"], strict=True)
    ))
    return max(joint_delta, tcp_delta)


def _surface_inventory(driver: RawWebDriver) -> dict[str, Any]:
    initial = _dom_state(driver)
    _require(initial["deploymentReady"], "Deployment status is not Ready")
    _require(initial["deploymentConnected"], "Deployment status feed is disconnected")
    _require(initial["canvasCount"] == 1, "UI5 requires exactly one Canvas")
    _require(initial["socketCount"] == 1, "UI5 requires exactly one viewer socket")
    _require(not initial["retryHealthVisible"], "Retry health remains visible")
    _require(not initial["measuredStateVisible"], "redundant Measured state label remains")
    _require(initial["scenePresentation"] == "owner", "startup selected Draft presentation")

    catalog = _execute(driver, """return (() => {
      const available = document.querySelectorAll(
        '.node-info-button.execution-available',
      ).length;
      const unavailable = document.querySelectorAll(
        '.node-info-button.execution-unavailable',
      ).length;
      const inlineAvailability = [...document.querySelectorAll('.node-palette-item')]
        .some((node) => /execution (?:unavailable|available)/i.test(node.textContent ?? ''));
      const nodes = document.querySelectorAll('.node-palette-item').length;
      const categories = document.querySelectorAll(
        '.program-palette-category-toggle',
      ).length;
      return { available, unavailable, inlineAvailability, nodes, categories };
    })();""")
    _require(isinstance(catalog, dict), "node catalog inventory is unavailable")
    _require(catalog["available"] > 0, "no executable catalog node is visible")
    _require(catalog["unavailable"] > 0, "genuinely unavailable catalog rows are not distinguished")
    _require(not catalog["inlineAvailability"], "availability detail still clutters catalog rows")
    palette_interaction = _execute(driver, """return (() => {
      const before = document.querySelectorAll('.react-flow__node').length;
      const item = document.querySelector('.node-palette-item');
      const assistantToggle = document.querySelector(
        '[aria-label="Expand Assistant nodes"]',
      );
      if (!(item instanceof HTMLButtonElement)) return null;
      item.click();
      return {
        before,
        after: document.querySelectorAll('.react-flow__node').length,
        unavailableCategoryCompacted: assistantToggle !== null,
        fontSizePx: Number.parseFloat(getComputedStyle(item).fontSize),
      };
    })();""")
    _require(isinstance(palette_interaction, dict), "node palette interaction is unavailable")
    _require(
        palette_interaction["before"] == palette_interaction["after"],
        "one catalog click inserted a node",
    )
    _require(
        palette_interaction["unavailableCategoryCompacted"],
        "all-unavailable Assistant category is not compacted by default",
    )
    _require(palette_interaction["fontSizePx"] >= 14.0, "node catalog text remains undersized")

    _click_selector(driver, '[aria-label="Program-wide settings"]', "Program settings")
    settings = _execute(driver, """return (() => {
      const velocity = document.querySelector(
        '[aria-label="Program global velocity scale"]',
      );
      const acceleration = document.querySelector(
        '[aria-label="Program global acceleration scale"]',
      );
      const tcp = document.querySelector(
        '[aria-label="Program maximum TCP linear speed"]',
      );
      return {
        velocity: velocity?.value ?? null,
        acceleration: acceleration?.value ?? null,
        tcp: tcp?.value ?? null,
      };
    })();""")
    _require(isinstance(settings, dict) and all(settings.values()), "Program motion settings are incomplete")
    _click_selector(driver, '[aria-label="Close Program settings"]', "close Program settings")

    _select_workspace(driver, "deployment")
    overview = _execute(driver, r"""return (() => {
      const root = document.querySelector('.component-overview');
      return {
        present: root !== null,
        scopes: [...(root?.querySelectorAll('.runtime-components > *') ?? [])].length,
        text: root?.textContent?.replace(/\s+/g, ' ').trim() ?? '',
      };
    })();""")
    _require(
        isinstance(overview, dict)
        and overview["present"]
        and "Observe / Render" in overview["text"]
        and "Plan / Preview" in overview["text"]
        and "Execute / Control" in overview["text"],
        "Deployment Component Overview is incomplete",
    )
    _select_workspace(driver, "program")

    _click_button_text(driver, ".viewer-surface-launchers", "Tools", "Viewer Tools")
    tools = _execute(driver, """return (() => {
      const root = document.querySelector('.viewer-options-body');
      const sections = [...(root?.querySelectorAll('.viewer-axes-section') ?? [])];
      const ranges = sections.map((section) => section.querySelector('input[type="range"]'));
      const summaries = sections.map((section) => section.querySelector('summary')?.textContent?.trim());
      return {
        summaries,
        rowCounts: sections.map((section) => section.querySelectorAll(
          '.viewer-axes-list > label',
        ).length),
        rangeBounds: ranges.map((range) => ({ min: range?.min, max: range?.max })),
        comparisonDisabled: [...(root?.querySelectorAll('label') ?? [])]
          .find((label) => label.textContent?.includes('Comparison ghost'))
          ?.querySelector('input')?.disabled === true,
        frameAllAvailable: !document.querySelector(
          '[aria-label="Frame all visible content"]',
        )?.disabled,
      };
    })();""")
    _require(isinstance(tools, dict), "Viewer Tools inventory is unavailable")
    _require(tools["summaries"] == ["Frames and joints", "Object axes"], "axes sections are incomplete")
    _require(tools["rowCounts"][0] >= 13 and tools["rowCounts"][1] >= 3, "axes rows are incomplete")
    _require(
        all(bounds == {"min": "0.02", "max": "1"} for bounds in tools["rangeBounds"]),
        "axes size bounds are incorrect",
    )
    _require(tools["comparisonDisabled"], "nonexistent comparison ghost is enabled")
    _require(tools["frameAllAvailable"], "Frame all visible content is unavailable")
    _click_selector(driver, '[aria-label="Frame all visible content"]', "Frame all visible content")

    _click_selector(driver, '.global-surface-launchers button', "Runtime / history")
    runtime, _ = _wait(
        lambda: _dom_state(driver),
        lambda state: state["runtimePresent"],
        "Runtime/history rail",
        DEFAULT_TIMEOUT_S,
    )
    geometry = runtime["runtimeGeometry"]
    _require(not runtime["runtimeExpanded"], "manually opened Runtime/history did not start collapsed")
    _require(runtime["runtimeSummary"] == "No messages require attention", "unexpected attention is active")
    _require(isinstance(geometry, dict), "Runtime/history geometry is unavailable")
    _require(abs(geometry["leftInset"] - geometry["rightInset"]) <= 2.0, "Runtime/history rail is not centered")
    _require(geometry["topInset"] >= 0.0 and geometry["height"] <= 64.0, "Runtime/history rail obscures the viewer")
    _click_selector(driver, '[aria-label="Close runtime messages"]', "close Runtime/history")
    _wait(
        lambda: _dom_state(driver),
        lambda state: not state["runtimePresent"],
        "closed Runtime/history rail",
        DEFAULT_TIMEOUT_S,
    )

    layout = _execute(driver, """return (() => {
      const workbench = document.querySelector('.workbench')?.getBoundingClientRect();
      const separators = [...document.querySelectorAll('[role="separator"]')].map(
        (separator) => ({
          label: separator.getAttribute('aria-label'),
          orientation: separator.getAttribute('aria-orientation'),
          minimum: Number(separator.getAttribute('aria-valuemin')),
          maximum: Number(separator.getAttribute('aria-valuemax')),
          value: Number(separator.getAttribute('aria-valuenow')),
        }),
      );
      return {
        workbenchWidth: workbench?.width ?? 0,
        separators,
        paletteCollapse: document.querySelector(
          '[aria-label="Collapse node palette"]',
        ) !== null,
        inspectorMinimize: document.querySelector(
          '[aria-label="Minimize Program inspector"]',
        ) !== null,
      };
    })();""")
    _require(isinstance(layout, dict), "workbench layout inventory is unavailable")
    _require(len(layout["separators"]) >= 3, "independent workbench dividers are missing")
    viewer_separator = next(
        (
            separator
            for separator in layout["separators"]
            if separator["label"] == "Resize viewer and live controls pane"
        ),
        None,
    )
    _require(viewer_separator is not None, "viewer/live-control divider is missing")
    _require(
        viewer_separator["maximum"] >= layout["workbenchWidth"] / 2.0,
        "viewer region cannot reach one half of the supported workbench width",
    )
    if layout["workbenchWidth"] >= 1900:
        inspector_separator = next(
            (
                separator
                for separator in layout["separators"]
                if separator["label"] == "Resize central inspector pane"
            ),
            None,
        )
        _require(inspector_separator is not None, "central inspector divider is missing")
        _execute(driver, """return (() => {
          const separator = document.querySelector(
            '[aria-label="Resize central inspector pane"]',
          );
          if (!(separator instanceof HTMLElement)) return false;
          separator.focus();
          separator.dispatchEvent(new KeyboardEvent('keydown', {
            key: 'Home',
            bubbles: true,
          }));
          return true;
        })();""")
        two_thirds, _ = _wait(
            lambda: {
                "maximum": int(_execute(
                    driver,
                    "return Number(document.querySelector('[aria-label=\"Resize viewer and live controls pane\"]')?.getAttribute('aria-valuemax'));",
                )),
                "workbenchWidth": float(_execute(
                    driver,
                    "return document.querySelector('.workbench')?.getBoundingClientRect().width ?? 0;",
                )),
            },
            lambda state: state["maximum"] >= math.floor(state["workbenchWidth"] * 2.0 / 3.0),
            "viewer two-thirds resize range",
            DEFAULT_TIMEOUT_S,
        )
        viewer_separator["maximum"] = two_thirds["maximum"]
    _require(layout["paletteCollapse"] and layout["inspectorMinimize"], "Program columns are not independently collapsible")
    original_viewer_width = viewer_separator["value"]
    moved = _execute(driver, """return (() => {
      const separator = document.querySelector(
        '[aria-label="Resize viewer and live controls pane"]',
      );
      if (!(separator instanceof HTMLElement)) return false;
      separator.focus();
      separator.dispatchEvent(new KeyboardEvent('keydown', {
        key: 'ArrowLeft',
        bubbles: true,
      }));
      return document.activeElement === separator;
    })();""")
    _require(moved is True, "viewer divider did not accept keyboard focus")
    keyboard_width, _ = _wait(
        lambda: {
            "value": int(_execute(
                driver,
                "return Number(document.querySelector('[aria-label=\"Resize viewer and live controls pane\"]')?.getAttribute('aria-valuenow'));",
            )),
        },
        lambda state: state["value"] != original_viewer_width,
        "viewer divider keyboard resize",
        DEFAULT_TIMEOUT_S,
    )
    _execute(driver, """return (() => {
      const separator = document.querySelector(
        '[aria-label="Resize viewer and live controls pane"]',
      );
      separator?.dispatchEvent(new KeyboardEvent('keydown', {
        key: 'ArrowRight',
        bubbles: true,
      }));
      return true;
    })();""")
    _wait(
        lambda: {
            "value": int(_execute(
                driver,
                "return Number(document.querySelector('[aria-label=\"Resize viewer and live controls pane\"]')?.getAttribute('aria-valuenow'));",
            )),
        },
        lambda state: state["value"] == original_viewer_width,
        "viewer divider keyboard restoration",
        DEFAULT_TIMEOUT_S,
    )

    return {
        "initial": {
            "deploymentLabel": initial["deploymentLabel"],
            "scenePresentation": initial["scenePresentation"],
            "canvasCount": initial["canvasCount"],
            "socketCount": initial["socketCount"],
        },
        "catalog": catalog,
        "paletteInteraction": palette_interaction,
        "programMotionSettings": settings,
        "componentOverview": {"present": overview["present"], "scopes": overview["scopes"]},
        "viewerTools": tools,
        "runtimeRail": geometry,
        "layout": {
            **layout,
            "keyboardResizedTo": keyboard_width["value"],
            "keyboardRestoredTo": original_viewer_width,
        },
    }


def _preview_mode(
    driver: RawWebDriver,
    mode: str,
    edit: Callable[[], Any],
    *,
    require_ghost: bool,
) -> dict[str, Any]:
    _click_button_text(driver, ".robot-control-modes", mode, f"Robot Control {mode}")
    mounted, _ = _wait(
        lambda: _robot_state(driver),
        lambda state: state["mode"] == mode,
        f"Robot Control {mode} mount",
        DEFAULT_TIMEOUT_S,
    )
    if mode in {"Pose plan", "Cartesian path"}:
        _require(mounted["tcpToolsPresent"], f"{mode} TCP tools are absent")
    edit_result = edit()
    preview, _ = _wait(
        lambda: _robot_state(driver),
        lambda state: state["approveAvailable"] and state["revertAvailable"],
        f"Robot Control {mode} preview",
        DEFAULT_TIMEOUT_S,
    )
    _require(preview["error"] is None, f"Robot Control {mode} reported an error")
    _require(
        preview["ghostPresent"] if require_ghost else preview["previewReceiptPresent"],
        f"Robot Control {mode} did not present its validated candidate",
    )
    _click_button_text(
        driver,
        '[data-testid="robot-control-panel"]',
        "Revert",
        f"Robot Control {mode} Revert",
    )
    reverted, _ = _wait(
        lambda: _robot_state(driver),
        lambda state: not state["approveAvailable"] and not state["revertAvailable"],
        f"Robot Control {mode} Revert settlement",
        DEFAULT_TIMEOUT_S,
    )
    _require(not reverted["ghostPresent"], f"Robot Control {mode} left a stale ghost")
    return {
        "edit": edit_result,
        "validated": True,
        "reverted": True,
        "tcpToolsPresent": mounted["tcpToolsPresent"],
    }


def _approve_mode(
    driver: RawWebDriver,
    composer_url: str,
    mode: str,
    edit: Callable[[], Any],
    *,
    require_ghost: bool,
) -> dict[str, Any]:
    _click_button_text(driver, ".robot-control-modes", mode, f"Robot Control {mode}")
    _wait(
        lambda: _robot_state(driver),
        lambda state: state["mode"] == mode,
        f"Robot Control {mode} execution mount",
        DEFAULT_TIMEOUT_S,
    )
    before = _control_observation(composer_url)
    edit_result = edit()
    preview, _ = _wait(
        lambda: _robot_state(driver),
        lambda state: state["approveAvailable"] and state["revertAvailable"],
        f"Robot Control {mode} executable preview",
        DEFAULT_TIMEOUT_S,
    )
    _require(preview["error"] is None, f"Robot Control {mode} preview reported an error")
    _require(
        preview["ghostPresent"] if require_ghost else preview["previewReceiptPresent"],
        f"Robot Control {mode} executable preview is not visible",
    )
    _click_button_text(
        driver,
        '[data-testid="robot-control-panel"]',
        "Approve",
        f"Robot Control {mode} Approve",
    )
    settled, trace = _wait(
        lambda: _robot_state(driver),
        lambda state: (
            state["phase"] is None
            and not state["approveAvailable"]
            and not state["revertAvailable"]
            and state["operationText"] is None
        ),
        f"Robot Control {mode} owner settlement",
        TERMINAL_TIMEOUT_S,
    )
    _require(settled["error"] is None, f"Robot Control {mode} execution reported an error")
    phases = [state["phase"] for state in trace if state["phase"] is not None]
    _require(
        any(phase in {"Submitting", "Executing", "Settling", "Completed"} for phase in phases),
        f"Robot Control {mode} exposed no execution transition",
    )
    after = _control_observation(composer_url)
    if mode == "Gripper":
        _require(
            after["gripperSequence"] > before["gripperSequence"],
            "Robot Control Gripper produced no newer owner observation",
        )
    else:
        _require(
            after["motionSequence"] > before["motionSequence"],
            f"Robot Control {mode} produced no newer owner observation",
        )
    delta = _observation_delta(before, after, mode)
    _require(delta > 1e-6, f"Robot Control {mode} settled without observed motion")
    return {
        "edit": edit_result,
        "approved": True,
        "settled": True,
        "observedDelta": delta,
        "phases": list(dict.fromkeys(phases)),
    }


def _robot_control_inventory(
    driver: RawWebDriver,
    composer_url: str,
) -> dict[str, Any]:
    _click_button_text(driver, ".viewer-surface-launchers", "Robot", "Robot Control")
    _wait(
        lambda: _robot_state(driver),
        lambda state: state["present"] and state["mode"] is not None,
        "Robot Control owner contract",
        DEFAULT_TIMEOUT_S,
    )
    modes: list[tuple[str, Callable[[], Any], bool]] = [
        ("Joint", lambda: _set_number_input(driver, "joint1 target degrees", 0.5), True),
        ("Pose plan", lambda: _set_number_input(driver, "x target metres", 0.001), True),
        ("Move To", lambda: _set_first_move_source(driver), True),
        ("Cartesian path", lambda: _set_number_input(driver, "z target metres", 0.001), True),
        ("Gripper", lambda: _set_gripper_preview_target(driver), False),
    ]
    results: dict[str, Any] = {}
    for mode, edit, require_ghost in modes:
        results[mode] = {
            "revert": _preview_mode(
                driver,
                mode,
                edit,
                require_ghost=require_ghost,
            ),
            "execute": _approve_mode(
                driver,
                composer_url,
                mode,
                edit,
                require_ghost=require_ghost,
            ),
        }
    _click_selector(driver, '[aria-label="Close viewer inspector"]', "close Robot Control")
    return results


def _transport_summary(state: dict[str, Any]) -> dict[str, Any]:
    return {
        key: state[key]
        for key in (
            "runText",
            "runDisabled",
            "stopText",
            "stopDisabled",
            "resetText",
            "resetDisabled",
            "velocityValue",
            "velocityDisabled",
        )
    }


def _assert_transition_fences(trace: list[dict[str, Any]], label: str) -> None:
    for state in trace:
        if state["runText"] in {"Starting", "Resuming", "Running"}:
            _require(state["runDisabled"] is True, f"{label} exposed a clickable {state['runText']}")
        if state["stopText"] == "Stopping":
            _require(state["stopDisabled"] is True, f"{label} exposed a clickable Stopping")
        _require(state["velocityIdentityStable"], f"{label} rebound the velocity control")


def _program_transport_journey(
    driver: RawWebDriver,
    composer_url: str,
) -> dict[str, Any]:
    idle, _ = _wait(
        lambda: _dom_state(driver),
        lambda state: state["runText"] == "Run" and state["runDisabled"] is False,
        "automatic native Idle readiness",
        DEFAULT_TIMEOUT_S,
    )
    _require(idle["velocityDisabled"] is False, "velocity is unavailable while native Idle")
    before_socket_creations = idle["socketCreations"]
    _execute(driver, f"return window.{QUALIFICATION_GLOBAL}.take();")

    idle_scale = _set_range(driver, 0.82)
    scaled_idle, _ = _wait(
        lambda: _dom_state(driver),
        lambda state: state["velocityValue"] == "0.82",
        "Idle velocity update",
        DEFAULT_TIMEOUT_S,
    )
    _require(idle_scale["identityStable"] and idle_scale["focused"], "Idle velocity update lost focus")

    _click_selector(driver, ".transport-run", "Run")
    running, run_trace = _wait(
        lambda: _dom_state(driver),
        lambda state: (
            state["runText"] == "Running"
            and state["runDisabled"] is True
            and state["stopDisabled"] is False
        ),
        "Running",
        DEFAULT_TIMEOUT_S,
    )
    _assert_transition_fences(run_trace, "Run")

    velocity_trace: list[dict[str, Any]] = []
    for value in (0.73, 0.61, 0.69):
        dispatch = _set_range(driver, value)
        time.sleep(0.12)
        current = _dom_state(driver)
        _require(dispatch["identityStable"] and dispatch["focused"], "velocity dispatch lost focus")
        _require(current["velocityFocused"], "velocity owner reconciliation lost focus")
        _require(current["velocityIdentityStable"], "velocity owner reconciliation rebound the input")
        velocity_trace.append({"requested": value, "observed": current["velocityValue"]})
    applied_scale, _ = _wait(
        lambda: _http_json(composer_url, "/api/execution/control-state")
        ["control_state"]["runtime_scale_control"],
        lambda state: (
            state["requested_scale"] == 0.69
            and state["effective_scale"] == 0.69
            and state["active_operation_id"] is not None
        ),
        "owner-applied in-motion runtime velocity",
        DEFAULT_TIMEOUT_S,
    )

    _click_selector(driver, ".transport-stop-primary", "Stop")
    stopped, stop_trace = _wait(
        lambda: _dom_state(driver),
        lambda state: (
            state["stopText"] == "Stopped"
            and state["stopDisabled"] is True
            and state["runText"] == "Resume Motion"
            and state["runDisabled"] is False
        ),
        "Stopped",
        DEFAULT_TIMEOUT_S,
    )
    _assert_transition_fences(stop_trace, "Stop")
    _require(any(state["stopText"] == "Stopping" for state in stop_trace), "Stop never presented Stopping")

    _click_selector(driver, ".transport-run", "Resume Motion")
    resumed, resume_trace = _wait(
        lambda: _dom_state(driver),
        lambda state: (
            state["runText"] == "Running"
            and state["runDisabled"] is True
            and state["stopDisabled"] is False
        ),
        "resumed Running",
        DEFAULT_TIMEOUT_S,
    )
    _assert_transition_fences(resume_trace, "Resume")

    terminal, terminal_trace = _wait(
        lambda: _dom_state(driver),
        lambda state: state["resetDisabled"] is False and state["runDisabled"] is True,
        "terminal awaiting Reset",
        TERMINAL_TIMEOUT_S,
    )
    _assert_transition_fences(terminal_trace, "terminal run")

    _click_selector(driver, ".transport-reset", "Reset")
    reset_idle, reset_trace = _wait(
        lambda: _dom_state(driver),
        lambda state: (
            state["runText"] == "Run"
            and state["runDisabled"] is False
            and state["resetDisabled"] is True
        ),
        "automatic readiness after Reset",
        DEFAULT_TIMEOUT_S,
    )
    _assert_transition_fences(reset_trace, "Reset")
    _require(any(state["resetText"] == "Resetting" for state in reset_trace), "Reset never presented Resetting")
    _require(reset_idle["velocityValue"] == "0.69", "runtime velocity did not persist across Reset")
    _require(not reset_idle["retryHealthVisible"], "Reset required Retry health")
    _require(reset_idle["scenePresentation"] == "owner", "Reset flashed Draft presentation")

    performance = _execute(driver, f"return window.{QUALIFICATION_GLOBAL}.take();")
    _require(isinstance(performance, dict), "viewer performance result is unavailable")
    perf = performance["performance"]
    status = performance["status"]
    _require(perf["modelBuilds"] == 0, "Run/Stop/Resume/Reset rebuilt viewer geometry")
    _require(perf["renderedFrames"] > 0, "native motion produced no viewer frames")
    _require(status["canvasCount"] == 1, "transport journey changed Canvas count")
    _require(status["headless"]["activeSockets"] == 1, "transport journey changed socket count")
    _require(status["headless"]["socketCreations"] == before_socket_creations, "transport journey recreated the viewer socket")
    _require(not status["errors"], "viewer reported a browser/runtime error")

    restore = _set_range(driver, 1.0)
    restored, _ = _wait(
        lambda: _dom_state(driver),
        lambda state: state["velocityValue"] == "1",
        "runtime velocity restoration",
        DEFAULT_TIMEOUT_S,
    )
    _require(restore["identityStable"] and restored["velocityIdentityStable"], "velocity restoration rebound the control")

    return {
        "idle": _transport_summary(idle),
        "scaledIdle": _transport_summary(scaled_idle),
        "running": _transport_summary(running),
        "velocityTrace": velocity_trace,
        "ownerAppliedScale": {
            "requestedScale": applied_scale["requested_scale"],
            "effectiveScale": applied_scale["effective_scale"],
            "controlGeneration": applied_scale["control_generation"],
        },
        "stopped": _transport_summary(stopped),
        "resumed": _transport_summary(resumed),
        "terminal": _transport_summary(terminal),
        "resetIdle": _transport_summary(reset_idle),
        "performance": perf,
        "restored": _transport_summary(restored),
    }


def collect_ui5_native_browser_journey(
    geckodriver_endpoint: str,
    composer_url: str,
    *,
    viewport_width: int = 1440,
    viewport_height: int = 960,
) -> dict[str, Any]:
    driver = RawWebDriver(geckodriver_endpoint, timeout_s=5.0)
    try:
        driver.create_session()
        driver.set_window_rect(viewport_width, viewport_height)
        target_url = _url_with_opt_in(composer_url)
        driver.command("POST", "/url", {"url": target_url})
        _wait(
            lambda: _dom_state(driver),
            lambda state: (
                state["deploymentConnected"]
                and state["canvasCount"] == 1
                and state["socketCount"] == 1
                and state["runText"] == "Run"
                and state["runDisabled"] is False
            ),
            "connected native workbench",
            DEFAULT_TIMEOUT_S,
        )
        surfaces = _surface_inventory(driver)
        robot_controls = _robot_control_inventory(driver, composer_url)
        transport = _program_transport_journey(driver, composer_url)

        # A hard reload must recover one fresh snapshot automatically without
        # presenting Draft state or accumulating an active socket.
        driver.command("POST", "/url", {"url": target_url})
        reloaded, _ = _wait(
            lambda: _dom_state(driver),
            lambda state: (
                state["deploymentConnected"]
                and state["canvasCount"] == 1
                and state["socketCount"] == 1
                and state["runText"] == "Run"
                and state["runDisabled"] is False
            ),
            "hard-reload full-snapshot recovery",
            DEFAULT_TIMEOUT_S,
        )
        _require(reloaded["scenePresentation"] == "owner", "hard reload selected Draft presentation")
        _require(not reloaded["retryHealthVisible"], "hard reload required Retry health")
        _require(not reloaded["runtimePresent"], "empty Runtime/history auto-opened after reload")

        return {
            "schemaVersion": RESULT_SCHEMA,
            "success": True,
            "scope": "O1.5-UI5 grouped native browser closure",
            "viewport": {"width": viewport_width, "height": viewport_height},
            "surfaces": surfaces,
            "robotControls": robot_controls,
            "programTransport": transport,
            "reload": {
                "scenePresentation": reloaded["scenePresentation"],
                "canvasCount": reloaded["canvasCount"],
                "socketCount": reloaded["socketCount"],
                "socketCreations": reloaded["socketCreations"],
                "automaticRunReady": not reloaded["runDisabled"],
            },
            "claims": [
                "truthful native catalog availability and scoped Deployment status",
                "discoverable Program motion settings and complete Viewer Tools inventory",
                "all five Robot Control modes preview/revert without effect and Approve through observed settlement",
                "Run/Stop/Resume/Reset labels remain disabled through exact settlement",
                "runtime velocity is focus-stable, latest-wins, target-persistent, and owner-applied",
                "one Canvas/socket and zero geometry rebuilds across control transitions",
                "automatic hard-reload/full-snapshot recovery without Retry health",
            ],
            "manualAcceptanceExcluded": True,
        }
    finally:
        if driver.session_id is not None:
            driver.delete_session()


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--geckodriver-endpoint", required=True)
    parser.add_argument("--composer-url", required=True)
    parser.add_argument("--viewport-width", type=int, default=1440)
    parser.add_argument("--viewport-height", type=int, default=960)
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        result = collect_ui5_native_browser_journey(
            arguments.geckodriver_endpoint,
            arguments.composer_url,
            viewport_width=arguments.viewport_width,
            viewport_height=arguments.viewport_height,
        )
    except CollectorError as error:
        print(json.dumps({
            "schemaVersion": RESULT_SCHEMA,
            "success": False,
            "error": str(error),
        }, separators=(",", ":"), sort_keys=True))
        return 1
    print(json.dumps(result, separators=(",", ":"), sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
