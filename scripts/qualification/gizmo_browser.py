"""Short rendered gizmo check: native previews only; scene writes only in a fixture.

Uses actual pointer gestures and the loaded R3F scene to inspect geometry, not
pixel goldens. No Approve/Run/Stop/Reset or operator scene mutation is issued.
"""
from __future__ import annotations

import argparse
import base64
import json
import time
from pathlib import Path

from current_workbench_browser_collector import RawWebDriver, _execute


# Install in the page's module realm, not Marionette's isolated script realm.
PROBE = r"""
const module = await import(performance.getEntriesByType('resource').find(r=>r.name.includes('@react-three_fiber.js')).name);
const readRoot = () => module._roots.get(document.querySelector('canvas')).store.getState();
const readControl=()=>{let result;readRoot().scene.traverse(n=>{if(n.isTransformControls&&n.enabled&&n.object)result=n;});return result;};
// Count new pointer registrations by identity; compare after the first remount
// so initial listeners installed before this read-only probe are immaterial.
const listeners=[], add=EventTarget.prototype.addEventListener, remove=EventTarget.prototype.removeEventListener;
const tracked=(target,type)=>(target===document||target instanceof HTMLCanvasElement)&&type.startsWith('pointer');
EventTarget.prototype.addEventListener=function(type,listener,options){
  const capture=typeof options==='boolean'?options:Boolean(options?.capture);
  if(tracked(this,type)&&!listeners.some(r=>r.target===this&&r.type===type&&r.listener===listener&&r.capture===capture))listeners.push({target:this,type,listener,capture});
  return add.call(this,type,listener,options);
};
EventTarget.prototype.removeEventListener=function(type,listener,options){
  const capture=typeof options==='boolean'?options:Boolean(options?.capture);
  const index=listeners.findIndex(r=>r.target===this&&r.type===type&&r.listener===listener&&r.capture===capture);
  if(index>=0)listeners.splice(index,1);return remove.call(this,type,listener,options);
};
window.gizmoProbe = {
  snapshot() {
    const {scene,gl,camera,controls}=readRoot(), ghosts=[],control=readControl();
    scene.traverse(n=>{if(!n.name.startsWith('candidate-robot:'))return;
      const meshes=[];n.traverse(m=>{if(m.isMesh&&m.material?.color)meshes.push({color:m.material.color.getHexString(),opacity:m.material.opacity,transparent:m.material.transparent,geometry:m.geometry.uuid,position:m.getWorldPosition(m.position.clone()).toArray()});});
      ghosts.push({name:n.name,meshes});});
    const target=scene.getObjectByName('owner-preview-final-tcp')||scene.getObjectByName('scene:test-box');
    return {ghosts,target:target?.position.toArray(),quaternion:target?.quaternion.toArray(),geometries:gl.info.memory.geometries,
      control:control?{mode:control.getMode(),space:control.space,dragging:control.dragging,axis:control.axis}:null,camera:camera.position.toArray(),cameraEnabled:controls?.enabled,pointerListeners:listeners.length,
      requests:window.gizmoRequests||[],metrics:window.manyforgeViewerQualification?.snapshot()};
  },
  handle(axis='X',distance=0.025) {
    const {camera,gl}=readRoot(),control=readControl();
    if(!control) return null;
    readRoot().scene.updateMatrixWorld(true);
    const mesh=control.gizmo.gizmo[control.getMode()].children.find(n=>n.isMesh&&n.name===axis);
    if(!mesh)return null;
    mesh.geometry.computeBoundingBox();const p=mesh.geometry.boundingBox.getCenter(mesh.position.clone());mesh.localToWorld(p);
    const delta=p.clone().set(axis==='X'?distance:0,axis==='Y'?distance:0,axis==='Z'?distance:0);
    if(control.space==='local')delta.applyQuaternion(control.object.quaternion);
    const rect=gl.domElement.getBoundingClientRect();
    const screen=v=>{v.project(camera);return {x:Math.round(rect.left+(v.x+1)*rect.width/2),y:Math.round(rect.top+(1-v.y)*rect.height/2)};};
    return {start:screen(p.clone()),end:screen(p.add(delta))};
  }
};
const originalFetch=window.fetch;window.gizmoRequests=[];
window.fetch=async (...args)=>{
  if(String(args[0]).includes('/api/execution/motion-previews')) {
    window.gizmoRequests.push({at:performance.now()});
    if(window.gizmoFailPreview)throw new TypeError('Deliberate qualification transport failure');
    const response=await originalFetch(...args);
    if(window.gizmoDelayPreview)await new Promise(r=>setTimeout(r,window.gizmoDelayPreview));
    return response;
  }
  return originalFetch(...args);
};
"""


def run(endpoint: str, base_url: str, output: Path) -> dict:
    driver = RawWebDriver(endpoint, timeout_s=20)
    passed = []
    output.mkdir(parents=True, exist_ok=True)

    def js(expression):
        return _execute(driver, f"return ({expression});")

    def wait(expression):
        until = time.monotonic() + 8
        while time.monotonic() < until:
            if js(f"Boolean({expression})"):
                return
            time.sleep(0.04)
        raise AssertionError(f"{expression}; control={js('JSON.stringify(window.gizmoProbe?.snapshot().control) || \"not loaded\"')}; {js('document.body.innerText.slice(-1700)')}")

    def click(selector):
        found = driver.command('POST', '/element', {'using': 'css selector', 'value': selector})
        driver.command('POST', f"/element/{found['element-6066-11e4-a52e-4f735466cecf']}/click", {})

    def text_button(text, prefix=''):
        button = f"[...document.querySelectorAll({json.dumps(prefix+'button')})].find(n=>n.textContent.trim()==={json.dumps(text)})"
        wait(f"{button} && !{button}.disabled")
        js(f"{button}.click(), true")

    def check_snap_label():
        assert js("""(() => {
          const button=[...document.querySelectorAll('.viewer-tool-strip button')].find(n=>n.textContent==='Snap');
          const range=document.createRange(); range.selectNodeContents(button);
          const text=range.getBoundingClientRect(), box=button.getBoundingClientRect(), css=getComputedStyle(button);
          return text.left>=box.left+parseFloat(css.paddingLeft)-1 && text.right<=box.right-parseFloat(css.paddingRight)+1;
        })()"""), 'Snap label exceeds its padded button bounds'

    def input_value(label, value):
        selector = f'input[aria-label="{label}"]'
        click(selector)
        driver.command('POST', '/actions', {'actions': [{'type': 'key', 'id': 'keys', 'actions': [
            {'type': 'keyDown', 'value': '\ue009'}, {'type': 'keyDown', 'value': 'a'},
            {'type': 'keyUp', 'value': 'a'}, {'type': 'keyUp', 'value': '\ue009'},
        ]}]})
        found = driver.command('POST', '/element', {'using': 'css selector', 'value': selector})
        driver.command('POST', f"/element/{found['element-6066-11e4-a52e-4f735466cecf']}/value", {'text': str(value) + '\ue007'})
        js('document.activeElement.blur(), true')

    def load(path):
        driver.command('POST', '/url', {'url': base_url.rstrip('/') + path})
        wait("document.querySelector('canvas') && window.manyforgeViewerQualification?.status().renderer && window.manyforgeViewerQualification.status().model.geometryCount > 0")
        driver.command('POST', '/execute/sync', {'script': "const s=document.createElement('script');s.type='module';s.textContent=arguments[0];document.head.append(s);return true;", 'args': ["try {\n" + PROBE + "\n} catch(e) {window.gizmoProbeError=String(e);}"]})
        wait('window.gizmoProbe || window.gizmoProbeError')
        error = js('window.gizmoProbeError || ""')
        assert not error, error

    def snapshot(name):
        result = js('gizmoProbe.snapshot()')
        output.joinpath(name + '.png').write_bytes(base64.b64decode(driver.command('GET', '/screenshot')))
        return result

    def pointer(actions):
        driver.command('POST', '/actions', {'actions': [{'type': 'pointer', 'id': 'mouse', 'parameters': {'pointerType': 'mouse'}, 'actions': actions}]})

    def key(value):
        driver.command('POST', '/actions', {'actions': [{'type': 'key', 'id': 'keys', 'actions': [
            {'type': 'keyDown', 'value': value}, {'type': 'keyUp', 'value': value}]}]})

    def drag_begin(axis='X'):
        time.sleep(0.1)  # Let the demand renderer apply a just-changed mode.
        points = js(f'gizmoProbe.handle({json.dumps(axis)})')
        assert points is not None
        for point in points.values():
            assert js(f"document.elementFromPoint({point['x']},{point['y']})?.tagName === 'CANVAS'"), points
        pointer([{'type': 'pointerMove', 'duration': 0, **points['start']}, {'type': 'pointerDown', 'button': 0},
                 {'type': 'pointerMove', 'duration': 120, **points['end']}])

    try:
        driver.create_session(firefox_args=('-no-remote',))
        driver.set_window_rect(1440, 1100)
        load('/?manyforge_viewer_qualification=1')
        click('button[aria-label="Reset camera"]')
        text_button('Robot', '.viewer-surface-launchers ')
        text_button('Pose plan', '.robot-control-modes ')
        wait("document.querySelector('input[aria-label=\"x target metres\"]')")
        start_x = js("Number(document.querySelector('input[aria-label=\"x target metres\"]').value)")
        input_value('x target metres', start_x + 0.005)
        wait("!document.querySelector('.approve-button').disabled")
        wait("gizmoProbe.snapshot().ghosts.some(g=>g.meshes.some(m=>m.color==='58ddff'))")
        ready = snapshot('native-ready')
        assert any(m['color'] == '58ddff' for g in ready['ghosts'] for m in g['meshes'])
        key('e')
        wait("gizmoProbe.snapshot().control?.mode==='rotate'")
        assert js("[...document.querySelectorAll('.robot-control-tcp-tools button')].find(n=>n.textContent==='Rotate')?.getAttribute('aria-pressed')==='true'")
        key('r')
        assert js("gizmoProbe.snapshot().control.mode==='rotate'")
        key('q')
        wait("gizmoProbe.snapshot().control.space==='local'")
        js("document.querySelector('input[aria-label=\"x target metres\"]').focus(), true")
        key('w')
        assert js("[...document.querySelectorAll('.robot-control-tcp-tools button')].find(n=>n.textContent==='Rotate')?.getAttribute('aria-pressed')==='true'")
        key('\ue00c')  # Discard invalid numeric text; it correctly disables the handle.
        js('document.activeElement.blur(), true')
        wait("gizmoProbe.snapshot().control?.mode==='rotate'")
        key('w')
        key('q')
        wait("gizmoProbe.snapshot().control.mode==='translate' && gizmoProbe.snapshot().control.space==='world'")
        assert len(js('gizmoProbe.snapshot().requests')) == len(ready['requests'])
        passed.append('Robot W/E/Q update the actual gizmo; R cannot scale it, typing and settings issue no preview')
        text_button('Snap', '.viewer-header-controls ')
        check_snap_label()
        before = js('gizmoProbe.snapshot()')
        js('window.gizmoDelayPreview=400, true')
        drag_begin()
        wait("document.querySelector('.robot-control-preview').textContent.includes('Release to validate')")
        time.sleep(0.48)
        during = snapshot('native-drag')
        assert len(during['requests']) == len(before['requests'])
        assert during['ghosts'] and all(g['name'].endswith('unvalidated-tool-target') for g in during['ghosts'])
        assert all(m['color'] == 'f2bb56' and m['transparent'] for g in during['ghosts'] for m in g['meshes'])
        delta = during['target'][0] - before['target'][0]
        assert abs(delta) > 0 and abs(delta / 0.01 - round(delta / 0.01)) < 1e-6
        pointer([{'type': 'pointerUp', 'button': 0}])
        wait("document.querySelector('.robot-control-preview').textContent.includes('Validating preview')")
        pending = snapshot('native-pending')
        assert all(m['color'] == 'f2bb56' for g in pending['ghosts'] for m in g['meshes'])
        wait("!document.querySelector('.approve-button').disabled")
        assert len(js('gizmoProbe.snapshot().requests')) == len(before['requests']) + 1
        time.sleep(0.25)
        idle = js('gizmoProbe.snapshot()')
        time.sleep(0.4)
        idle_end = js('gizmoProbe.snapshot()')
        assert idle_end['metrics']['performance']['modelBuilds'] == during['metrics']['performance']['modelBuilds']
        assert idle_end['metrics']['performance']['renderedFrames'] - idle['metrics']['performance']['renderedFrames'] <= 2
        passed.append('composed native Lite6: cyan result, local snapped yellow tool during drag, no drag requests, one release preview')
        text_button('Move To', '.robot-control-modes ')
        for label, value in [('Move To target type', 'current'), ('Move To target', 'observed:tcp')]:
            selector = f'select[aria-label="{label}"]'
            wait(f'document.querySelector({json.dumps(selector)}) && !document.querySelector({json.dumps(selector)}).disabled')
            js(f"(() => {{const n=document.querySelector({json.dumps(selector)});n.value={json.dumps(value)};n.dispatchEvent(new Event('change',{{bubbles:true}}));return true;}})()")
        click('.robot-control-offset-details summary')
        input_value('X local offset metres', 0.005)
        wait("!document.querySelector('.approve-button').disabled")
        offsets = "[...document.querySelectorAll('.robot-control-offset-details input')].map(n=>Number(n.value))"
        offsets_before = js(offsets)
        requests_before = len(js('gizmoProbe.snapshot().requests'))
        drag_begin()
        wait("document.querySelector('.robot-control-preview').textContent.includes('Release to validate')")
        move_to_drag = snapshot('native-move-to-drag')
        assert len(move_to_drag['requests']) == requests_before
        pointer([{'type':'pointerUp','button':0}])
        wait(f'gizmoProbe.snapshot().requests.length==={requests_before+1}')
        assert js(offsets) != offsets_before
        assert js("document.querySelector('select[aria-label=\"Move To target\"]').value==='observed:tcp'")
        passed.append('native Move To gizmo shares the selected source/offset fields and releases one preview without moving the source')
        text_button('Pose plan', '.robot-control-modes ')
        input_value('x target metres', 0.9)
        wait("document.querySelector('.robot-control-preview').textContent.includes('Not executable')")
        rejected = snapshot('native-rejected')
        assert rejected['ghosts'] and all(m['color'] == 'ff7066' for g in rejected['ghosts'] for m in g['meshes'])
        js('window.gizmoFailPreview=true, true')
        input_value('x target metres', 0.89)
        wait("document.querySelector('.robot-control-preview').textContent.includes('Unavailable')")
        unavailable = snapshot('native-unavailable')
        assert unavailable['ghosts'] and all(m['color'] == 'f2bb56' for g in unavailable['ghosts'] for m in g['meshes'])
        passed.append('real native unreachable rejection is red; a separately injected transport failure stays yellow/unavailable')
        text_button('Revert', '.robot-control-footer ')
        load('/tests/browser/gizmo-object.html?manyforge_viewer_qualification=1')
        text_button('Object', '.viewer-surface-launchers ')
        key('e')
        wait("gizmoProbe.snapshot().control.mode==='rotate'")
        key('q')
        wait("gizmoProbe.snapshot().control.space==='local'")
        key('r')
        wait("gizmoProbe.snapshot().control.mode==='scale'")
        key('w')
        key('q')
        text_button('Snap', '.viewer-header-controls ')
        check_snap_label()
        before_object = js('gizmoProbe.snapshot()')
        drag_begin()
        time.sleep(0.2)
        assert js('objectGizmoHarness.commits.length') == 0
        pointer([{'type': 'pointerUp', 'button': 0}])
        wait('objectGizmoHarness.commits.length===1')
        after_object = snapshot('fixture-object')
        delta = after_object['target'][0] - before_object['target'][0]
        assert abs(delta) > 0 and abs(delta / 0.01 - round(delta / 0.01)) < 1e-6
        assert js('gizmoProbe.snapshot().requests.length') == 0
        passed.append('disposable Object uses the same snapping with one local scene release and zero motion previews')
        cancellations = []
        for _ in range(3):
            before_cancel = js('gizmoProbe.snapshot()')
            drag_begin()
            key('\ue00c')  # Real Escape while the pointer is held.
            wait('gizmoProbe.snapshot().cameraEnabled')
            pointer([{'type': 'pointerUp', 'button': 0}])
            time.sleep(0.1)
            cancelled = js('gizmoProbe.snapshot()')
            assert cancelled['target'] == before_cancel['target']
            assert js('objectGizmoHarness.commits.length') == 1
            cancellations.append(cancelled)
        assert len({s['pointerListeners'] for s in cancellations}) == 1
        assert len({s['geometries'] for s in cancellations}) == 1
        rect = js("(() => {const r=document.querySelector('canvas').getBoundingClientRect();return {x:Math.round(r.left+40),y:Math.round(r.top+40)}})()")
        camera_before = js('gizmoProbe.snapshot().camera')
        pointer([{'type':'pointerMove','duration':0,**rect},{'type':'pointerDown','button':0},
                 {'type':'pointerMove','duration':120,'x':rect['x']+35,'y':rect['y']+20},{'type':'pointerUp','button':0}])
        wait(f'JSON.stringify(gizmoProbe.snapshot().camera)!==JSON.stringify({json.dumps(camera_before)})')
        passed.append('three cancelled object drags preserve pose/commit count, retain bounded listeners/geometry and release camera orbit')
        return {'passed': passed, 'nativeMotionIssued': False, 'ready': ready, 'drag': during,
                'rejected': rejected, 'object': after_object, 'cancellations': cancellations,
                'idleFramesAdded': idle_end['metrics']['performance']['renderedFrames'] - idle['metrics']['performance']['renderedFrames']}
    finally:
        driver.delete_session()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--webdriver', required=True)
    parser.add_argument('--base-url', required=True)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = run(args.webdriver, args.base_url, args.output)
    args.output.joinpath('result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({'passed': result['passed'], 'output': str(args.output)}, indent=2))
