"""Bounded real Firefox/React tests with controlled command replies.

This tests input, focus, layout and hook races, NOT native motion. The caller
owns an isolated Vite server and WebDriver. No product backend is contacted.
"""
from __future__ import annotations

import argparse
import json
import time

from current_workbench_browser_collector import RawWebDriver, _execute


def run(endpoint: str, base_url: str) -> dict:
    driver = RawWebDriver(endpoint, timeout_s=15)
    passed = []

    def js(expression: str):
        return _execute(driver, f"return (() => {{ const h = window.robotControlHarness; return ({expression}); }})();")

    def wait(expression: str):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if js(expression):
                return
            time.sleep(0.025)
        inputs = js("({inputs:[...document.querySelectorAll('input')].map(n => ({label:n.getAttribute('aria-label'),value:n.value,invalid:n.getAttribute('aria-invalid'),focused:document.activeElement===n})), events:window.inputTrace})")
        raise AssertionError(f"DOM condition failed: {expression}; {js('h?.snapshot()')}; inputs={inputs}")

    def element(selector: str) -> str:
        return driver.command("POST", "/element", {"using": "css selector", "value": selector})[
            "element-6066-11e4-a52e-4f735466cecf"
        ]

    def click(selector: str):
        driver.command("POST", f"/element/{element(selector)}/click", {})

    def key(selector: str, text: str):
        driver.command("POST", f"/element/{element(selector)}/value", {"text": text})

    def fresh():
        driver.command("POST", "/url", {"url": base_url.rstrip('/') + '/tests/browser/robot-control.html'})
        wait("h && h.snapshot().phase === 'idle'")

    try:
        driver.create_session(firefox_args=("-headless", "-no-remote"))
        driver.set_window_rect(960, 800)
        fresh()
        js("h.editBoth(), true")
        wait("h.snapshot().phase === 'ready'")
        assert js("JSON.stringify(h.snapshot().positions)") == '[0.25,0.35]'
        passed.append('batched joint edits accumulate in the actual hook')

        joint = 'input[aria-label="joint1 target degrees"]'
        js("(() => { window.inputTrace=[]; for(const kind of ['keydown','beforeinput','input','change','blur']) document.addEventListener(kind,e=>{if(window.inputTrace.length<32)window.inputTrace.push({kind,key:e.key,type:e.inputType,value:e.target.value})},true); return true; })()")
        click(joint)
        driver.command('POST', '/actions', {'actions': [{
            'type': 'key', 'id': 'keyboard', 'actions': [
                {'type': 'keyDown', 'value': '\ue009'}, {'type': 'keyDown', 'value': 'a'},
                {'type': 'keyUp', 'value': 'a'}, {'type': 'keyUp', 'value': '\ue009'},
            ],
        }]})
        key(joint, '999')
        wait("document.querySelector('.approve-button').disabled")
        # An actual click outside the input causes native blur/validation.
        click('button[aria-label="Preview information"]')
        wait("document.querySelector('[role=dialog]') !== null")
        assert js("h.snapshot().commands.length") == 0
        assert js("document.querySelector('.approve-button').disabled")
        key('button[aria-label="Close Preview information"]', '\ue00c')
        wait("document.querySelector('[role=dialog]') === null")
        wait("document.activeElement?.getAttribute('aria-label') === 'Preview information'")
        js("[...document.querySelectorAll('button')].find(b => b.textContent.trim() === 'Revert').click(), true")
        wait("h.snapshot().phase === 'idle' && document.querySelector('input[aria-label=\"joint1 target degrees\"]').getAttribute('aria-invalid') !== 'true'")
        assert js("Number(document.querySelector('input[aria-label=\"joint1 target degrees\"]').value)") == 0
        passed.append('invalid text blocks approval; Revert clears it; information supports Escape/focus return')

        js("h.setPreviewFailure(true), h.editBoth(), true")
        wait("h.snapshot().phase === 'blocked'")
        count = js("h.snapshot().previewCount")
        js("h.setPreviewFailure(false), h.retry(), true")
        wait("h.snapshot().phase === 'ready'")
        assert js("h.snapshot().previewCount") == count + 1
        assert js("h.snapshot().commands.length") == 0
        js("h.refresh(), true")
        wait("h.snapshot().phase === 'ready'")
        assert js("h.snapshot().previewCount") == count + 1
        passed.append('preview retry is local; ordinary source refresh preserves a valid preview')

        click('.approve-button')
        wait("h.snapshot().phase === 'approving' && h.snapshot().commands.length === 1")
        js("h.replaceOwner(), true")
        wait("h.snapshot().phase === 'idle'")
        js("h.editBoth(), true")
        wait("h.snapshot().phase === 'ready'")
        click('.approve-button')
        wait("h.snapshot().phase === 'approving' && h.snapshot().commands.length === 2")
        js("h.finish(0, true), true")
        wait("h.snapshot().phase === 'approving' && h.snapshot().error === null")
        js("h.finish(1, true), true")
        wait("h.snapshot().phase === 'blocked' && h.snapshot().error !== null")
        passed.append('old approval receipt cannot block or reject a new owner draft')

        fresh()
        js("h.operation('A'), true")
        wait("h.snapshot().operation?.operationId === 'A'")
        js("h.command('pause'), true")
        wait("h.snapshot().controlPending === 'pause'")
        js("h.command('stop'), true")
        wait("h.snapshot().controlPending === 'stop'")
        js("h.finish(0, true), true")
        wait("h.snapshot().controlPending === 'stop' && h.snapshot().error === null")
        js("h.operation('B'), true")
        wait("h.snapshot().operation?.operationId === 'B' && h.snapshot().controlPending === null")
        js("h.finish(1, true), true")
        wait("h.snapshot().error === null")
        passed.append('Stop overtakes Pause; delayed replies cannot affect a successor operation')

        js("h.command('fullStop'), h.command('fullStop'), true")
        wait("h.snapshot().fullStopPending")
        assert js("h.snapshot().commands.length") == 3
        js("h.replaceOwner(), true")
        wait("!h.snapshot().fullStopPending && h.snapshot().operation === null")
        js("h.command('fullStop'), true")
        wait("h.snapshot().commands.length === 4 && h.snapshot().fullStopPending")
        js("h.finish(2, true), true")
        wait("h.snapshot().fullStopPending && h.snapshot().fullStopError === null")
        js("h.finish(3), true")
        wait("!h.snapshot().fullStopPending")
        passed.append('Full Stop is single-flight per owner, independent of old-owner replies')

        js("h.operation('fault', 'intervention_required', 'retained owner fault'), true")
        wait("h.snapshot().error === 'retained owner fault'")
        js("[...document.querySelectorAll('button')].find(b => b.textContent.trim() === 'Revert').click(), true")
        wait("h.snapshot().operation?.resident && h.snapshot().error === 'retained owner fault'")
        assert js("document.querySelector('input[type=range]').disabled")
        assert js("h.snapshot().commands.length") == 4
        passed.append('local Revert preserves resident fault; authoring cannot hide it')

        for width, height in [(280, 480), (360, 640), (640, 600)]:
            js(f"Object.assign(document.querySelector('.viewer-inspector-section').style, {{width:'{width}px',height:'{height}px'}}), true")
            assert js("(() => { const p=document.querySelector('[data-testid=robot-control-panel]').getBoundingClientRect(); const f=document.querySelector('[data-testid=robot-control-candidate-footer]').getBoundingClientRect(); return f.bottom<=p.bottom+1 && Math.abs(f.bottom-p.bottom)<=1 && f.left>=p.left && f.right<=p.right+1; })()")
        passed.append('footer remains pinned at three inspector sizes with fault details')

        fresh()
        js("h.enableScale(), true")
        wait("h.snapshot().runtimeScale !== null")
        js("h.scale(0.5), true")
        wait("h.snapshot().commands.length === 1 && h.snapshot().runtimeScalePending")
        js("h.replaceOwner(), true")
        wait("!h.snapshot().runtimeScalePending")
        js("h.enableScale(), true")
        wait("h.snapshot().runtimeScale !== null")
        js("h.scale(0.25), true")
        wait("h.snapshot().commands.length === 2 && h.snapshot().runtimeScalePending")
        js("h.finish(0, true), true")
        wait("h.snapshot().runtimeScalePending && h.snapshot().runtimeScaleError === null")
        # A queued successor value must stay behind its own request, not the retired one.
        js("h.scale(0.3), true")
        assert js("h.snapshot().commands.length") == 2
        js("h.finish(1, true), true")
        wait("h.snapshot().commands.length === 3")
        assert js("h.snapshot().commands[2].payload.scale") == 0.3
        js("h.finish(2, true), true")
        wait("!h.snapshot().runtimeScalePending")
        passed.append('scale requests are single-flight per scope; retired replies cannot block or corrupt successors')

        fresh()
        js("h.enableScale(), true")
        wait("h.snapshot().runtimeScale !== null")
        js("h.scale(0.5), true")
        wait("h.snapshot().commands.length === 1")
        js("h.freezeScale(), true")
        wait("h.snapshot().runtimeScale.frozen")
        js("h.finish(0), true")
        wait("!h.snapshot().runtimeScalePending")
        assert js("h.snapshot().runtimeScale.frozen && h.snapshot().runtimeScale.applicationState === 'applied' && h.snapshot().runtimeScaleError === null")
        passed.append('equal-generation owner scale facts survive a delayed acceptance')

        fresh()
        js("h.measured([0.5,0.5]), true")
        wait("Math.abs(Number(document.querySelector('input[aria-label=\"joint2 target degrees\"]').value)-28.6478898)<0.001")
        js("h.editJoint(0,0.6), true")
        wait("h.snapshot().phase === 'ready'")
        assert js("JSON.stringify(h.snapshot().positions)") == '[0.6,0.5]'
        passed.append('first joint edit preserves untouched current observed joints')

        click(joint)
        key(joint, '9')
        wait("document.querySelector('.approve-button').disabled")
        key(joint, '\ue00c')
        wait("!document.querySelector('.approve-button').disabled")
        assert js("h.snapshot().commands.length") == 0
        passed.append('numeric Escape discards typed text without changing the approved draft')

        click(joint)
        key(joint, '9')
        js("h.program(true), true")
        wait("document.querySelector('input[type=range]').disabled")
        js("h.program(false), true")
        wait("!document.querySelector('input[type=range]').disabled && h.snapshot().phase === 'idle'")
        key(joint, '\ue007')
        assert js("h.snapshot().commands.length") == 0
        assert js("h.snapshot().phase") == 'idle'
        assert js("document.querySelector('input[aria-label=\"joint1 target degrees\"]').getAttribute('aria-invalid') !== 'true'")
        passed.append('Program handover discards focused input text and cannot recommit it afterward')

        fresh()
        js("h.settled(), true")
        wait("h.snapshot().phase === 'completed'")
        js("h.measured([0.5,0.5]), true")
        js("h.editJoint(0,0.6), true")
        wait("h.snapshot().phase === 'ready'")
        assert js("JSON.stringify(h.snapshot().positions)") == '[0.6,0.5]'
        passed.append('a retained terminal result cannot seed the next edit with old observations')

        fresh()
        js("h.mode('move_to'), true")
        wait("document.querySelector('.robot-control-move-to select') !== null")
        select = "document.querySelector('.robot-control-move-to select')"
        js(f"{select}.value='frame:base', {select}.dispatchEvent(new Event('change',{{bubbles:true}})), true")
        wait("h.snapshot().phase === 'ready'")
        js(f"{select}.value='', {select}.dispatchEvent(new Event('change',{{bubbles:true}})), true")
        wait("h.snapshot().phase === 'idle' && document.querySelector('.approve-button').disabled")
        js(f"{select}.value='observed:tcp', {select}.dispatchEvent(new Event('change',{{bubbles:true}})), true")
        click('input[aria-label="X local offset metres"]')
        key('input[aria-label="X local offset metres"]', '\ue013')
        key('input[aria-label="X local offset metres"]', '\ue007')
        wait("h.snapshot().phase === 'ready'")
        js("h.measured([0.4,0.5]), true")
        wait("h.snapshot().phase === 'idle' && document.querySelector('.robot-control-move-to select').value === ''")
        assert js("h.snapshot().commands.length") == 0
        passed.append('cleared or changed Move To source discards its unapproved target without a command')

        fresh()
        js("h.editBoth(), true")
        wait("h.snapshot().phase === 'ready'")
        js("Object.assign(document.querySelector('.viewer-inspector-section').style,{width:'280px',height:'480px'}), true")
        click('.approve-button')
        wait("h.snapshot().commands.length === 1")
        passed.append('Approve is pointer-reachable in the narrow pinned footer')
        return {'schema': 'manyforge.robot-control.dom-qualification.v1', 'passed': passed,
                'nativeMotionEvidence': False}
    finally:
        driver.delete_session()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--webdriver', required=True)
    parser.add_argument('--base-url', required=True)
    args = parser.parse_args()
    print(json.dumps(run(args.webdriver, args.base_url), indent=2))
