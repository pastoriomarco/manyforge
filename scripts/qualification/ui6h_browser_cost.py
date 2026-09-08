"""Short software-browser/cycle cost check over the real isolated workbench.

Consumes already-built native owners and a qualified retained package. Does
not qualify Stop, hardware, maximum scene size or the maintainer's panel review.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import httpx
import importlib.util
import json
from pathlib import Path
import shutil
from tempfile import TemporaryDirectory
import time

from current_workbench_browser_collector import RawWebDriver, _execute
from manyforge_headless_api.client import HeadlessHttpClient


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--webdriver", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--manual", action="store_true", help="Measure one real native Joint preview/approval first"
    )
    parser.add_argument(
        "--recovery",
        action="store_true",
        help="Deliberately hide/freeze the viewer during this isolated cycle",
    )
    args = parser.parse_args()
    workspace = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location(
        "cost_workbench", workspace / "scripts/dev-workbench.py"
    )
    workbench = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(workbench)
    result = {
        "scope": "isolated Lite6 native cycle, Firefox software WebGL, 1440x900; no Stop or hardware"
    }
    with TemporaryDirectory(prefix="h6b-") as temporary:
        root = Path(temporary)
        workbench.STATE_ROOT = root / "owners"
        workbench.PACKAGE_ROOT = root / "package"
        shutil.copytree(args.package_root, workbench.PACKAGE_ROOT)
        workbench.HEADLESS_BUILD = args.build_root / "headless"
        workbench.ROBOPLAN_BUILD = args.build_root / "roboplan"
        workbench.MANAGER_BINARY = workbench.HEADLESS_BUILD / "manyforge_manager_daemon"
        host = workbench.ROBOPLAN_BUILD / "manyforge_native_execution_host"
        worker = workbench.ROBOPLAN_BUILD / "manyforge_roboplan_candidate_worker"
        workbench.NATIVE_EXECUTION_HOST, workbench.NATIVE_CANDIDATE_WORKER = (
            host,
            worker,
        )
        # Locate the caller's already-built inputs, without changing owner wiring.
        workbench._import_package = lambda: None
        workbench._ensure_current_backend = lambda: next(
            workbench.HEADLESS_BUILD.glob("manyforge_headless_py*.so")
        )
        workbench._ensure_current_native_backend = lambda: (host, worker)
        workbench._ensure_deployment_compiler = lambda: (
            args.build_root / "deployment/manyforge_maintained_deployment_occurrences",
            tuple(
                (family, args.build_root / "deployment/generated" / directory)
                for family, directory in (
                    ("lite6_lite_gripper", "lite6_lite_gripper_structural"),
                    ("ur10e_robotiq_2f_140", "ur10e_robotiq_2f_140_structural"),
                    ("ur5_roboplan", "ur5_roboplan_structural"),
                )
            ),
        )
        stack = workbench.Workbench(
            scenario_id="lite6_pick_and_place",
            headless_port=9101,
            runner_port=9102,
            composer_port=9001,
            frontend_port=3001,
        )
        driver = RawWebDriver(args.webdriver)
        js = lambda script: _execute(driver, script)
        wait = lambda label, read: workbench._wait_until(
            label, read, stack.processes, timeout_seconds=30
        )
        ready = lambda: js(
            """const q=window.manyforgeViewerQualification;
            return q?.status().model.ready === true
              && q.status().renderer?.software === true
              && q.status().headless.activeSockets === 1
              && q.observationFlow().appliedFrames > 0;"""
        )
        source_status = lambda: httpx.get(
            "http://127.0.0.1:9001/api/scene/viewer/status", timeout=2
        ).json()

        def collect(name):
            value = js(
                """const q=window.manyforgeViewerQualification;
              const l=window.__h6Lag.splice(0).sort((a,b)=>a-b);
              return {viewer:q.take(), observations:q.observationFlow(),
                loopLagMs:{samples:l.length,p95:l[Math.floor(l.length*.95)]??null,
                  p99:l[Math.floor(l.length*.99)]??null,max:l.at(-1)??null},
                runningNodes:document.querySelectorAll('.bt-node.status-running').length,
                successfulNodes:document.querySelectorAll('.bt-node.status-success').length,
                observationStatus:document.querySelector('.viewer-observation-status')?.textContent??null,
                visibility:document.visibilityState};"""
            )
            value["source"] = source_status()
            result[name] = value
            assert value["viewer"]["status"]["renderer"]["software"] is True, value
            assert not value["viewer"]["status"]["errors"], value
            assert value["observations"]["maxUnappliedFrames"] <= 1, value
            js("window.manyforgeViewerQualification.resetObservationFlow(); return true;")
            return value

        def click(selector):
            element = driver.command(
                "POST", "/element", {"using": "css selector", "value": selector}
            )["element-6066-11e4-a52e-4f735466cecf"]
            driver.command("POST", f"/element/{element}/click", {})
            return element

        def manual_move(client):
            # Same production panel/transport/owners as the operator; no mocked
            # receipts or status. The private simulated arm moves one degree.
            baseline = client.execution_control_state()
            assert baseline["control_state"]["manual_operations"] == [], baseline
            manual_owner = baseline["identity"]["owner_epoch"]
            click(".viewer-surface-launchers button:first-child")
            selector = '.robot-control-number input[aria-label$="target degrees"]'
            wait(
                "manual Joint input available",
                lambda: js(f"return document.querySelector('{selector}')?.disabled === false;"),
            )
            target = js(f"return Number(document.querySelector('{selector}').value) + 1;")
            js(
                """const panel=document.querySelector('.robot-control-panel');
                const trace=window.__h6Manual={input:null,frame:null,preview:null,approve:null,states:[]};
                panel.addEventListener('input',e=>{
                  if(!e.target.closest('.robot-control-number'))return;
                  trace.input=performance.now();
                  requestAnimationFrame(()=>{trace.frame=performance.now();});
                });
                panel.querySelector('.approve-button').addEventListener('click',()=>{
                  trace.approve=performance.now();
                },true);
                window.__h6ManualObserver=new MutationObserver(()=>{
                  if(trace.input!==null && trace.preview===null
                    && !panel.querySelector('.approve-button').disabled)trace.preview=performance.now();
                  const state=panel.querySelector('.robot-control-operation')?.textContent??'';
                  if(state && trace.states.at(-1)?.state!==state && trace.states.length<32)
                    trace.states.push({state,at:performance.now()});
                });
                window.__h6ManualObserver.observe(panel,{subtree:true,childList:true,attributes:true,characterData:true});
                return true;"""
            )
            element = click(selector)
            driver.command(
                "POST",
                "/actions",
                {
                    "actions": [
                        {
                            "type": "key",
                            "id": "manual-keyboard",
                            "actions": [
                                {"type": "keyDown", "value": "\ue009"},
                                {"type": "keyDown", "value": "a"},
                                {"type": "keyUp", "value": "a"},
                                {"type": "keyUp", "value": "\ue009"},
                            ],
                        }
                    ]
                },
            )
            driver.command("POST", f"/element/{element}/value", {"text": f"{target}\ue004"})
            wait(
                "real native preview ready",
                lambda: js("return document.querySelector('.approve-button')?.disabled === false;"),
            )
            click(".approve-button")

            def settled():
                current = client.execution_control_state()
                assert current["identity"]["owner_epoch"] == manual_owner, current
                rows = current["control_state"]["manual_operations"]
                assert len(rows) <= 1, rows
                for row in rows:
                    participants = row["operation"]["participants"]
                    assert len(participants) == 1, row
                    assert participants[0]["command_target_id"] == "arm", row
                    state = row["operation"]["state"]
                    assert state not in {"failed", "interrupted", "intervention_required"}, row
                    if state == "completed" and not row["resident"]:
                        return row
                return None

            result["manual_owner_completion"] = wait("native manual clean retirement", settled)
            wait(
                "manual completion presented",
                lambda: js(
                    "return document.querySelector('.robot-control-operation')?.textContent.includes('completed')"
                    " && !document.querySelector('.robot-control-operation-actions');"
                ),
            )
            trace = js("window.__h6ManualObserver.disconnect(); return window.__h6Manual;")
            result["manual_timing"] = {
                "target_degrees": target,
                "input_to_animation_frame_ms": trace["frame"] - trace["input"],
                "input_to_validated_preview_ms": trace["preview"] - trace["input"],
                "approve_to_first_owner_state_ms": trace["states"][0]["at"] - trace["approve"],
                "trace": trace,
            }
            assert result["manual_timing"]["input_to_animation_frame_ms"] < 500, result[
                "manual_timing"
            ]
            assert not js(
                "return Boolean(document.querySelector('.robot-control-error')?.textContent);"
            ), trace
            measured = collect("manual")
            assert measured["loopLagMs"]["max"] < 500, measured
            # No UI Reset, Revert, Stop or retirement command is issued.
            click(".viewer-surface-launchers button:first-child")
            wait(
                "Program Run after backend manual retirement",
                lambda: js("return document.querySelector('.transport-run')?.disabled === false;"),
            )

        try:
            stack.start()
            driver.create_session(firefox_args=("-no-remote",))
            driver.set_window_rect(1440, 900)
            driver.command(
                "POST",
                "/url",
                {"url": "http://127.0.0.1:3001/?manyforge_viewer_qualification=1"},
            )
            wait(
                "real software-rendered model",
                ready,
            )
            wait(
                "GUI Run eligibility",
                lambda: js("return document.querySelector('.transport-run')?.disabled === false;"),
            )
            js(
                """window.__h6Lag=[]; let due=performance.now()+25;
              window.__h6Timer=setInterval(()=>{const now=performance.now();
                if(window.__h6Lag.length<2048)window.__h6Lag.push(Math.max(0,now-due)); due=now+25;},25);
              window.manyforgeViewerQualification.reset();
              window.manyforgeViewerQualification.resetObservationFlow(); return true;"""
            )
            time.sleep(3)
            collect("idle")
            # Resize without changing the underlying scene/model.
            driver.set_window_rect(1100, 750)
            driver.set_window_rect(1440, 900)
            time.sleep(1)
            collect("resize")
            with HeadlessHttpClient("http://127.0.0.1:9101", timeout_seconds=2) as client:
                if args.manual:
                    manual_move(client)
                run_button = driver.command(
                    "POST",
                    "/element",
                    {"using": "css selector", "value": ".transport-run"},
                )["element-6066-11e4-a52e-4f735466cecf"]
                driver.command("POST", f"/element/{run_button}/click", {})
                run = wait(
                    "GUI Run reaches Program owner",
                    lambda: client.program_cycle_status().get("run"),
                )
                run_id = run["capture"]["program_run_id"]
                begun = time.monotonic()
                tree_states = set()
                disrupted = False
                while time.monotonic() - begun < 60:
                    run = client.program_run(run_id)
                    tree_states.add(
                        js(
                            "return [...document.querySelectorAll('.bt-node')].map(n=>n.className).join('|');"
                        )
                    )
                    if run["state"] in {"succeeded", "failed", "interrupted"}:
                        break
                    if args.recovery and not disrupted and time.monotonic() - begun > 2:
                        disrupted = True
                        collect("before_disruption")
                        original = driver.command("GET", "/window")
                        blank = driver.command("POST", "/window/new", {"type": "tab"})["handle"]
                        driver.command("POST", "/window", {"handle": blank})
                        wait(
                            "hidden viewer releases demand",
                            lambda: source_status()["fanout"]["client_count"] == 0,
                        )
                        before = source_status()
                        time.sleep(1)
                        after = source_status()
                        result["hidden"] = {"before": before, "after": after}
                        assert after["source"]["active"] is False, after
                        assert (
                            after["source"]["wake_count"] == before["source"]["wake_count"]
                        ), result["hidden"]
                        driver.command("DELETE", "/window")
                        driver.command("POST", "/window", {"handle": original})
                        wait("visible viewer resumes current frame", ready)
                        collect("visibility_recovery")
                        # Real main-thread blockage, not synthetic owner state.
                        # Observe the server while the browser cannot return credit.
                        with ThreadPoolExecutor(max_workers=1) as executor:
                            frozen = executor.submit(
                                js,
                                "const until=performance.now()+1250; while(performance.now()<until){} return true;",
                            )
                            time.sleep(0.2)
                            before = source_status()
                            time.sleep(0.65)
                            after = source_status()
                            result["frozen"] = {"before": before, "after": after}
                            assert (
                                after["fanout"]["sent_count"] - before["fanout"]["sent_count"] <= 1
                            ), result["frozen"]
                            frozen.result(timeout=3)
                        wait("frozen viewer resumes current frame", ready)
                        collect("frozen_recovery")
                    time.sleep(0.2)
                result["run_state"] = run["state"]
                result["run_duration_s"] = time.monotonic() - begun
                result["distinct_tree_presentations"] = len(tree_states)
                assert run["state"] == "succeeded", run
                assert len(tree_states) > 3, "tree presentation did not follow active nodes"
                wait(
                    "last streamed pose rendered",
                    lambda: js(
                        """const f=window.manyforgeViewerQualification.observationFlow();
                    return f.lastReceived?.sourceSequence === f.lastApplied?.sourceSequence;"""
                    ),
                )
                motion = collect("motion")
                assert motion["observations"]["appliedFrames"] > 20, motion
                assert motion["viewer"]["performance"]["modelBuilds"] == 0, motion
                if not args.recovery:
                    assert motion["loopLagMs"]["max"] < 500, motion
                    assert motion["observations"]["receiveToApplyMaxMs"] < 500, motion
                else:
                    assert disrupted, "recovery profile never ran"
            # Cold reconnect shows current facts, not an old motion replay.
            driver.command("POST", "/refresh", {})
            wait(
                "reconnected current model",
                ready,
            )
            result["reconnect"] = {
                "viewer": js("return window.manyforgeViewerQualification.snapshot();"),
                "observations": js("return window.manyforgeViewerQualification.observationFlow();"),
            }
            result["passed"] = True
        except BaseException as error:
            result["failure"] = str(error)
            raise
        finally:
            try:
                if driver.session_id:
                    driver.delete_session()
            finally:
                try:
                    stack.close()
                finally:
                    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
