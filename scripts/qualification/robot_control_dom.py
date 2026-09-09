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

    def replace(selector: str, text: str):
        click(selector)
        driver.command('POST', '/actions', {'actions': [{
            'type': 'key', 'id': 'keyboard', 'actions': [
                {'type': 'keyDown', 'value': '\ue009'}, {'type': 'keyDown', 'value': 'a'},
                {'type': 'keyUp', 'value': 'a'}, {'type': 'keyUp', 'value': '\ue009'},
            ],
        }]})
        key(selector, text)

    def fresh():
        driver.command("POST", "/url", {"url": base_url.rstrip('/') + '/tests/browser/robot-control.html'})
        wait("h && h.snapshot().phase === 'idle'")

    try:
        driver.create_session(firefox_args=("-headless", "-no-remote"))
        driver.set_window_rect(960, 800)
        fresh()
        joint = 'input[aria-label="joint1 target degrees"]'
        # Real native Firefox spinner hit (inside the padded input), not a
        # synthetic React change or keyboard event. Firefox does not focus the
        # number field and emits insertReplacementText for this interaction.
        js("h.editJoint(0, -0.23), true")
        wait("h.snapshot().phase === 'ready'")
        for direction in [1, -1]:
            for index in range(6):
                if index == 3:
                    js("document.querySelector('input[aria-label=\"joint1 target degrees\"]').focus(), true")
                previous = js("h.snapshot().positions[0]")
                y = 0.4 if direction == 1 else 0.6
                rect = js("(() => { const n=document.querySelector('input[aria-label=\"joint1 target degrees\"]'); const r=n.getBoundingClientRect(); return {x:Math.round(r.right-parseFloat(getComputedStyle(n).paddingRight)-6),y:Math.round(r.top+r.height*" + str(y) + ")}; })()")
                driver.command('POST', '/actions', {'actions': [{
                    'type': 'pointer', 'id': 'mouse', 'parameters': {'pointerType': 'mouse'},
                    'actions': [
                        {'type': 'pointerMove', 'duration': 0, **rect},
                        {'type': 'pointerDown', 'button': 0},
                        {'type': 'pointerUp', 'button': 0},
                    ],
                }]})
                wait(f"h.snapshot().phase === 'ready' && Math.abs(h.snapshot().positions[0] - ({previous}+{direction}*5*Math.PI/180)) < 1e-7")
                js("h.refresh(), true")
                wait("!document.querySelector('.approve-button').disabled && Math.abs(Number(document.querySelector('input[aria-label=\"joint1 target degrees\"]').value)*Math.PI/180-h.snapshot().positions[0]) < 1e-7")
        assert js("h.snapshot().commands.length") == 0
        key(joint, '\ue013')
        wait("h.snapshot().phase === 'ready' && !document.querySelector('.approve-button').disabled")
        passed.append('repeated native Firefox spinner clicks step both ways, preserve approval and survive owner refresh')

        fresh()
        js("h.editBoth(), true")
        wait("h.snapshot().phase === 'ready'")
        assert js("JSON.stringify(h.snapshot().positions)") == '[0.25,0.35]'
        passed.append('batched joint edits accumulate in the actual hook')

        count = js("h.snapshot().previewCount")
        js("h.sceneChanged(), h.sceneChanged(), h.sceneChanged(), true")
        wait("h.snapshot().phase === 'planning' && h.snapshot().ghostStale && document.querySelector('.approve-button').disabled")
        wait("h.snapshot().phase === 'ready' && !h.snapshot().ghostStale")
        assert js("h.snapshot().previewCount") == count + 1
        assert js("JSON.stringify(h.snapshot().positions)") == '[0.25,0.35]'
        assert js("h.snapshot().commands.length") == 0
        passed.append('scene changes coalesce into one no-motion revalidation of the same draft')

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

        # The pinned notice is out of flow: appearance, dismissal and recovery
        # must not move controls, steal field focus or change the draft.
        for width in [280, 360]:
            fresh()
            js(f"document.querySelector('.viewer-inspector-section').style.width='{width}px', true")
            js("h.editJoint(0, 0.2), true")
            wait("h.snapshot().phase === 'ready'")
            locations = "[...document.querySelectorAll('.robot-control-modes, .robot-control-step-controls, input[aria-label=\"joint1 target degrees\"]')].map(n=>{const r=n.getBoundingClientRect();return [r.x,r.y,r.width,r.height]})"
            before = js(locations)
            click(joint)
            js("h.setPreviewFailure(true, 'native_ik_no_solution'), h.editJoint(0, 0.3), true")
            wait("h.snapshot().phase === 'blocked' && document.querySelector('.robot-control-preview-notice') !== null")
            assert js(locations) == before
            assert js("document.activeElement?.getAttribute('aria-label')") == 'joint1 target degrees'
            assert js("getComputedStyle(document.querySelector('.robot-control-preview-notice')).position") == 'absolute'
            assert js("document.querySelector('.robot-control-preview-notice').textContent.includes('may be out of reach')")
            assert js("h.snapshot().error === null && document.querySelector('.approve-button').disabled")
            positions = js("h.snapshot().positions")
            click('button[aria-label="Hide preview notice"]')
            wait("document.querySelector('.robot-control-preview-notice') === null")
            assert js(locations) == before
            assert js("h.snapshot().positions") == positions
            assert js("h.snapshot().phase === 'blocked' && document.querySelector('.approve-button').disabled")
            click('button[aria-label="Preview information"]')
            wait("Boolean(document.querySelector('[role=dialog]')?.textContent.includes('may be out of reach'))")
            key('button[aria-label="Close Preview information"]', '\ue00c')
            wait("document.querySelector('[role=dialog]') === null")
            js("h.setPreviewFailure(true, 'native_goal_state_collision'), h.editJoint(0, 0.4), true")
            wait("Boolean(document.querySelector('.robot-control-preview-notice')?.textContent.includes('would collide at the target'))")
            assert js(locations) == before
            assert js("h.snapshot().error === null && h.snapshot().commands.length === 0")
            js("h.setPreviewFailure(false), h.editJoint(0, 0.5), true")
            wait("h.snapshot().phase === 'ready' && document.querySelector('.robot-control-preview-notice') === null")
            assert js(locations) == before
        passed.append('pinned rejection overlay never reflows fields; details survive dismissal; collision and ambiguous IK remain distinct local feedback')

        fresh()
        js("h.mode('move_to'), true")
        wait("document.querySelector('select[aria-label=\"Move To target type\"]') !== null")
        js("(() => { const s=document.querySelector('select[aria-label=\"Move To target type\"]'); s.value='current'; s.dispatchEvent(new Event('change', {bubbles:true})); return true; })()")
        js("(() => { const s=document.querySelector('select[aria-label=\"Move To target\"]'); s.value='observed:tcp'; s.dispatchEvent(new Event('change', {bubbles:true})); return true; })()")
        wait("document.querySelector('.robot-control-offset-details') !== null")
        click('.robot-control-offset-details summary')
        for width in [280, 360]:
            js(f"document.querySelector('.viewer-inspector-section').style.width='{width}px', true")
            assert js("(() => { const p=document.querySelector('.robot-control-move-offsets'); const n=[...p.querySelectorAll('input')].map(i=>i.getBoundingClientRect()); return n.length===6 && n[0].y===n[1].y && n[1].y===n[2].y && n[3].y===n[4].y && n[4].y===n[5].y && n[0].y<n[3].y && p.scrollWidth<=p.clientWidth+1 && n.every(r=>r.width>40); })()")
        passed.append('Move To offsets use X/Y/Z then R/P/Yaw rows without horizontal overflow')

        fresh()
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
        js("h.editBoth(), true")
        wait("h.snapshot().phase === 'ready' && h.snapshot().ghost")
        js("h.operation('A'), true")
        wait("h.snapshot().operation?.operationId === 'A'")
        assert js("!h.snapshot().ghost && !h.snapshot().ghostStale")
        assert js("!document.querySelector('.robot-control-preview').textContent.includes('TCP predicted')")
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
        js("h.revert(), true")
        wait("h.snapshot().operation?.resident && h.snapshot().error === 'retained owner fault'")
        assert js("document.querySelector('input[type=range]').disabled")
        assert js("h.snapshot().commands.length") == 4
        assert js("document.querySelector('[data-testid=robot-control-candidate-footer] button').textContent === 'Reset operation'")
        passed.append('local Revert preserves resident fault; the pinned footer exposes explicit Reset instead of irrelevant preview actions')

        for width, height in [(280, 480), (360, 640), (640, 600)]:
            js(f"Object.assign(document.querySelector('.viewer-inspector-section').style, {{width:'{width}px',height:'{height}px'}}), true")
            assert js("(() => { const p=document.querySelector('[data-testid=robot-control-panel]').getBoundingClientRect(); const f=document.querySelector('[data-testid=robot-control-candidate-footer]').getBoundingClientRect(); return f.bottom<=p.bottom+1 && Math.abs(f.bottom-p.bottom)<=1 && f.left>=p.left && f.right<=p.right+1; })()")
            assert js("(() => { const p=document.querySelector('.robot-control-preview'); const e=p.querySelector('[role=alert]'); return e && e.getBoundingClientRect().bottom<=p.getBoundingClientRect().bottom+1 && p.scrollWidth<=p.clientWidth+1; })()")
            assert js("(() => { const b=[...document.querySelectorAll('.robot-control-operation-actions button')]; return b.every(n=>n.scrollWidth<=n.clientWidth+1); })()")
        passed.append('footer is pinned, fault text visible and operation controls fit at three inspector sizes')

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
        type_select = "document.querySelector('select[aria-label=\"Move To target type\"]')"
        select = "document.querySelector('select[aria-label=\"Move To target\"]')"
        wait(f"{type_select} !== null")
        assert js(f"{select}.disabled")
        assert js(f"[...{type_select}.options].map(o=>o.value).join(',')") == ',current,frame'
        js(f"{type_select}.value='frame', {type_select}.dispatchEvent(new Event('change',{{bubbles:true}})), true")
        wait(f"!{select}.disabled")
        assert js(f"[...{select}.options].every(o=>o.value==='' || o.value.startsWith('frame:'))")
        js(f"{select}.value='frame:base', {select}.dispatchEvent(new Event('change',{{bubbles:true}})), true")
        wait("h.snapshot().phase === 'ready'")
        js(f"{type_select}.value='current', {type_select}.dispatchEvent(new Event('change',{{bubbles:true}})), true")
        wait("h.snapshot().phase === 'idle' && document.querySelector('.approve-button').disabled")
        assert js(f"{select}.value === '' && !h.snapshot().ghost")
        assert js(f"[...{select}.options].every(o=>o.value==='' || o.value==='observed:tcp')")
        js(f"{select}.value='observed:tcp', {select}.dispatchEvent(new Event('change',{{bubbles:true}})), true")
        click('.robot-control-offset-details summary')
        click('input[aria-label="X local offset metres"]')
        key('input[aria-label="X local offset metres"]', '\ue013')
        key('input[aria-label="X local offset metres"]', '\ue007')
        wait("h.snapshot().phase === 'ready'")
        retained_move = js('h.snapshot().moveTo')
        retained_previews = js('h.snapshot().previewCount')
        js('h.showPanel(false), true')
        wait("document.querySelector('[data-testid=robot-control-panel]') === null")
        js('h.showPanel(true), true')
        wait(f"{select}?.value === 'observed:tcp'")
        time.sleep(0.2)
        assert js('h.snapshot().moveTo') == retained_move
        assert js('h.snapshot().previewCount') == retained_previews
        assert js('h.beginDrag()')
        js('h.endDrag(0.02), true')
        wait(f'h.snapshot().phase === "ready" && h.snapshot().previewCount === {retained_previews + 1}')
        assert abs(js('h.snapshot().moveTo.options.offsetXyzM[0]') - (retained_move['options']['offsetXyzM'][0] + 0.02)) < 1e-8
        passed.append('Move To survives inspector hide/show; numeric and released gizmo edits share the retained offsets')
        js("h.measured([0.4,0.5]), true")
        wait(f"h.snapshot().phase === 'idle' && {select}.value === ''")
        assert js("h.snapshot().commands.length") == 0
        passed.append('Move To filters available types then targets; type/source changes discard the old preview without a command')

        fresh()
        js("h.showPanel(false), h.showTransport(true), h.operation('gripper-fault', 'intervention_required', 'Execution timeout exceeded', 'gripper'), true")
        wait("document.querySelector('.transport-guidance')?.textContent.includes('Manual gripper recovery required')")
        assert js("document.querySelector('[data-testid=robot-control-panel]') === null")
        js("h.nativeAvailable(false), true")
        wait("document.querySelector('.transport-guidance')?.textContent.includes('Robot status unavailable')")
        assert js("!document.querySelector('.transport-guidance').textContent.includes('Manual gripper recovery required') && document.querySelector('.transport-guidance button') === null")
        js("h.nativeAvailable(true), true")
        wait("document.querySelector('.transport-guidance button') !== null")
        click('.transport-guidance button')
        wait("document.querySelector('[data-testid=robot-control-panel]') !== null && h.snapshot().operation?.operationId === 'gripper-fault'")
        assert js("document.querySelector('.robot-control-recovery-guidance').textContent.includes('Reset operation required')")
        assert js("!document.querySelector('button[aria-label=\"Reset operation\"]').disabled && h.snapshot().commands.length === 0")
        js("h.nativeAvailable(false), true")
        wait("document.querySelector('button[aria-label=\"Reset operation\"]').disabled")
        js("h.nativeAvailable(true), true")
        wait("!document.querySelector('button[aria-label=\"Reset operation\"]').disabled")
        click('button[aria-label="Reset operation"]')
        wait("h.snapshot().commands.length === 1")
        assert js("h.snapshot().commands[0].kind === 'operation_reset' && h.snapshot().commands[0].payload.operation_id === 'gripper-fault'")
        js("h.finish(0), h.releaseOperation(), true")
        wait("document.querySelector('.transport-guidance') === null && document.querySelector('.robot-control-recovery-guidance') === null")
        assert js("h.snapshot().commands.length === 1")
        passed.append('global recovery notice opens the exact hidden target; only explicit Reset commands it and owner release clears guidance')

        fresh()
        js("h.editBoth(), true")
        wait("h.snapshot().phase === 'ready'")
        js("Object.assign(document.querySelector('.viewer-inspector-section').style,{width:'280px',height:'480px'}), true")
        defaults = js("h.snapshot().intent.participants[0].payload")
        assert defaults['speed_profile'] == {
            'velocity_scale': 1, 'acceleration_scale': 1,
            'max_tcp_linear_speed_mps': 0.1,
        }
        assert defaults['per_joint_dynamics_limits'] == {}
        click('.approve-button')
        wait("h.snapshot().commands.length === 1")
        passed.append('Approve is pointer-reachable in the narrow pinned footer')
        fresh()
        velocity = 'input[aria-label="Manual Velocity (%)"]'
        click(velocity)
        key(velocity, '\ue015')
        wait("h.snapshot().motionSettings.velocityScale === 0.95")
        assert js("h.snapshot().previewCount") == 0
        js("h.editBoth(), true")
        wait("h.snapshot().phase === 'ready'")
        click('.robot-control-joint-settings > summary')
        joint_velocity = 'input[aria-label="Manual joint1 velocity (deg/s)"]'
        click(joint_velocity)
        key(joint_velocity, '\ue015')
        wait("h.snapshot().phase === 'ready' && h.snapshot().intent.participants[0].payload.per_joint_dynamics_limits.joint1?.max_velocity_rad_s > 0")
        payload = js("h.snapshot().intent.participants[0].payload")
        assert payload['speed_profile']['velocity_scale'] == 0.95
        assert payload['speed_profile']['acceleration_scale'] == 1
        assert 0 < payload['per_joint_dynamics_limits']['joint1']['max_velocity_rad_s'] < 1.5
        js("h.refresh(), true")
        wait("h.snapshot().phase === 'ready' && !document.querySelector('.approve-button').disabled")
        click('.approve-button')
        wait("h.snapshot().commands.length === 1")
        assert js("h.snapshot().commands[0].payload.participants[0].payload") == payload
        passed.append('manual speed and individual caps use actual inputs, survive refresh and reach Approve unchanged')

        fresh()
        js("h.editBoth(), true")
        wait("h.snapshot().phase === 'ready'")
        click('.robot-control-joint-settings > summary')
        for label, value in [
            ('Manual Velocity (%)', '101'),
            ('Manual Acceleration (%)', '101'),
            ('Manual TCP speed (m/s)', '0.3'),
            ('Manual joint1 velocity (deg/s)', '90'),
            ('Manual joint1 acceleration (deg/s²)', '145'),
        ]:
            selector = f'input[aria-label="{label}"]'
            previous = js("h.snapshot().motionSettings")
            count = js("h.snapshot().previewCount")
            replace(selector, value)
            wait("document.querySelector('.robot-control-limit-error')?.textContent.includes('Exceeds global limit') && document.querySelector('.approve-button').disabled")
            assert js("h.snapshot().motionSettings") == previous
            js("h.refresh(), true")
            assert js("document.querySelector('.robot-control-limit-error') !== null && document.querySelector('.approve-button').disabled")
            assert js("h.snapshot().previewCount") == count
            key(selector, '\ue00c')
            wait("document.querySelector('.robot-control-limit-error') === null && !document.querySelector('.approve-button').disabled")
            assert js("h.snapshot().commands.length") == 0
        # Step increments at an effective ceiling cannot raise it; this also
        # exercises the degree/radian round trip at an exact global boundary.
        key('input[aria-label="Manual joint1 velocity (deg/s)"]', '\ue013')
        wait("h.snapshot().phase === 'ready' && h.snapshot().motionSettings.jointLimits.joint1?.max_velocity_rad_s === 1.5")
        passed.append('above-global typed limits explain the rejection, block approval without a command, and ceiling steps stay capped')

        fresh()
        js("h.mode('cartesian_path'), true")
        wait("document.querySelector('input[aria-label=\"Manual TCP accel. (m/s²)\"]') !== null")
        acceleration = 'input[aria-label="Manual TCP accel. (m/s²)"]'
        click(acceleration)
        key(acceleration, '\ue013')
        wait("Math.abs(h.snapshot().motionSettings.tcpLinearAccelerationMps2 - 0.6) < 1e-9")
        for width in [280, 360]:
            js(f"document.querySelector('.viewer-inspector-section').style.width='{width}px', true")
            assert js("(() => { const p=document.querySelector('.robot-control-motion-settings'); return p.scrollWidth<=p.clientWidth+1; })()")
            assert js("(() => { const inputs=[...document.querySelectorAll('.robot-control-motion-settings-grid input')]; return inputs.every((p,i)=>i%2===0 || Math.abs(p.getBoundingClientRect().bottom-inputs[i-1].getBoundingClientRect().bottom)<1); })()")
            assert js("(() => { const steps=[...document.querySelectorAll('.robot-control-step')]; return steps.length===2 && steps.every(p=>{ const label=p.firstElementChild.getBoundingClientRect(), select=p.querySelector('select').getBoundingClientRect(); return p.childElementCount===2 && /^Step \\(.*\\)$/.test(p.firstElementChild.textContent) && label.right<=select.left && label.top<select.bottom && label.bottom>select.top && parseFloat(getComputedStyle(p).fontSize)>=12 && p.scrollWidth<=p.clientWidth+1; }); })()")
            assert js("parseFloat(getComputedStyle(document.querySelector('.robot-control-motion-settings h4')).fontSize)>=15")
        js("h.mode('pose_goal'), true")
        wait("document.querySelector('input[aria-label=\"Manual TCP accel. (m/s²)\"]') === null")
        js("h.mode('gripper'), true")
        wait("document.querySelector('.robot-control-motion-settings') === null")
        passed.append('TCP acceleration is Cartesian-only, settings fit narrow panels and unsupported gripper limits are absent')
        fresh()
        js("h.mode('pose_goal'), true")
        wait("document.querySelector('input[aria-label=\"x target metres\"]') !== null")
        js("h.holdPreviews(true), h.editPoseX(0.25), true")
        wait("h.snapshot().previewCount === 1")
        assert js("h.beginDrag()")
        wait("h.snapshot().dragging && document.querySelector('.approve-button').disabled")
        js("h.releasePreview(0), true")
        time.sleep(0.5)
        assert js("h.snapshot().previewCount === 1 && h.snapshot().dragging && h.snapshot().phase !== 'ready'")
        js("h.holdPreviews(false), h.endDrag(0.03), true")
        wait("h.snapshot().phase === 'ready' && !h.snapshot().dragging")
        assert js("h.snapshot().previewCount === 2 && Math.abs(h.snapshot().pose.position_m.x_m - 0.28)<1e-9 && h.snapshot().commands.length===0")
        assert js("h.beginDrag()")
        wait("h.snapshot().dragging")
        js("h.endDrag(null), true")
        wait("!h.snapshot().dragging && !document.querySelector('.approve-button').disabled")
        time.sleep(0.18)
        assert js("h.snapshot().previewCount === 2")
        passed.append('drag blocks approval and all preview scheduling, ignores an earlier reply, releases once and cancels without a new request')

        replace('input[aria-label="x target metres"]', '-')
        wait("h.snapshot().authoringPending")
        assert not js("h.beginDrag()")
        key('input[aria-label="x target metres"]', '\ue00c')
        wait("!h.snapshot().authoringPending")
        assert js("h.beginDrag()")
        wait("h.snapshot().dragging")
        js("h.sceneChanged(), true")
        wait("!h.snapshot().dragging")
        js("h.endDrag(2), true")
        wait("h.snapshot().phase === 'ready'")
        assert js("Math.abs(h.snapshot().pose.position_m.x_m-0.28)<1e-9 && h.snapshot().commands.length===0")
        passed.append('pending numeric text prevents a gesture and scene replacement fences an obsolete release')
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
