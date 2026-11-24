import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import { parseToolPreviewResponse } from '../api/toolParametersParser';
import { GeneratedToolEditor, TOOL_PREVIEW_OPACITY, ToolPreview } from './GeneratedToolEditor';
import { DeploymentFocusedEditor } from './DeploymentFocusedEditor';
import { relativeMeshPath } from '../utils/generatedToolAuthoring';

vi.mock('@react-three/fiber', () => ({ Canvas: () => null }));
vi.mock('@react-three/drei', () => ({ OrbitControls: () => null }));

const parameters = {
  api_version: 'manyforge.parametric_tool.v1', kind: 'tool',
  body: [{ name: 'housing', collision: { cylinder: { radius_m: 0.02, length_m: 0.06 } } }],
  chains: [{ attach_to: 'body', elements: [{ jaw_pair: {
    id: 'outer', positive_jaw: [{ name: 'f', collision: { box: { size_m: [0.01, 0.01, 0.01] } } }],
    min_aperture_m: 0, max_aperture_m: 0.04 } }] }],
  tcps: [{ id: 'tcp', attach_to: 'outer' }],
};
const valve = { outputs: [{ signal: { controllable_component_id: 'gripper_io', signal_name: 'valve' }, open_value: false, close_value: true }] };

function controller(source: JsonValue = parameters): DeploymentWorkspaceController {
  return {
    workspace: { draft: { document: {
      api_version: 'manyforge.deployment_intent.v3',
      components: [{
        component_id: 'gripper', component_profile_id: 'manyforge.tool', target_settings: {},
        model_source: { parameters: source },
        configuration: { runtime_kind: 'ros2_control', runtime_configuration: {
          component_streams: [],
          gripper_target_configurations: [
            { controllable_component_id: 'gripper', command_target_id: 'outer', pneumatic_io: valve }],
        } },
      }],
    } } },
    mutationAvailable: true, busy: false, loading: false,
    mutate: vi.fn().mockResolvedValue({}), updateMotionTargetLimit: vi.fn(),
  } as unknown as DeploymentWorkspaceController;
}

describe('GeneratedToolEditor', () => {
  it('shows Body, Chains, TCPs, Wiring per jaw pair and Mass, with Export', () => {
    const html = renderToStaticMarkup(<GeneratedToolEditor controller={controller()} componentId="gripper" />);
    for (const title of ['Tool', 'Body', 'Chains', 'TCPs', 'Wiring', 'Mass']) expect(html).toContain(`>${title}</h4>`);
    expect(html).toContain('gripper · outer');
    expect(html).toContain('Export');
    expect(html).toContain('Default: outer_fingers');
    expect(html).toContain('Jaw pair');
  });

  it('keeps a parameter-file Tool to its wiring and Export', () => {
    const html = renderToStaticMarkup(
      <GeneratedToolEditor controller={controller({ project_path: 'tool/gripper.tool.yaml' })} componentId="gripper" />);
    expect(html).toContain('tool/gripper.tool.yaml');
    expect(html).toContain('>Wiring</h4>');
    expect(html).not.toContain('>Chains</h4>');
  });

  it('leaves a Tool\'s wiring out of the gripper and execution host settings', () => {
    const html = renderToStaticMarkup(<DeploymentFocusedEditor controller={controller()}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[{
        controllableComponentId: 'gripper', commandTargetId: 'outer',
      } as never]} />);
    expect(html).not.toContain('Pneumatic I/O is retained');
    expect(html).not.toContain('Configure pneumatic I/O');
  });

  it('parses a preview and a refusal naming its field', () => {
    const refused = parseToolPreviewResponse({
      schemaVersion: 'manyforge.composer_tool_preview.v1', ok: false, meshFolder: '/work/cell.scenario',
      error: { code: 'parametric_tool_invalid', message: 'bad', field: 'chains[0].elements[0].jaw_pair.max_aperture_m' },
    });
    expect(refused).toEqual({ ok: false, meshFolder: '/work/cell.scenario', error: {
      code: 'parametric_tool_invalid', message: 'bad', field: 'chains[0].elements[0].jaw_pair.max_aperture_m' } });
    const ready = parseToolPreviewResponse({
      schemaVersion: 'manyforge.composer_tool_preview.v1', ok: true, meshFolder: null,
      preview: { shapes: [{ link: 'tool_body', part: 'housing', geometry: { box: { size_m: [1, 1, 1] } },
        position_m: [0, 0, 0], orientation_xyzw: [0, 0, 0, 1] }],
      tcps: [{ id: 'tcp', tool_end: null, position_m: [0, 0, 0.1], orientation_xyzw: [0, 0, 0, 1] }] },
      toolEnds: [{ id: 'outer', kind: 'jaw_pair', actuated: true, collision_body_role_ids: ['outer_fingers'], tcp_ids: ['tcp'] }],
      collisionBodies: ['body', 'outer_fingers'],
      tcps: [{ id: 'tcp', frame: 'tool_tcp', tool_end: 'outer', contact_body_role_ids: ['outer_fingers'] }],
    });
    expect(ready.ok && ready.toolEnds[0].actuated).toBe(true);
    expect(ready.meshFolder).toBeNull();
    expect(ready.ok && ready.shapes[0].mesh_asset).toBeNull();
  });

  it('draws a mesh part from its GLB and names a mesh it cannot draw', () => {
    const url = '/api/deployment/draft/tool-meshes/0123456789abcdef.glb?component_id=gripper';
    const response = (housing: unknown) => ({
      schemaVersion: 'manyforge.composer_tool_preview.v1', ok: true, meshFolder: '/work/cell.scenario',
      preview: { shapes: [
        { link: 'tool_body', part: 'housing', geometry: { mesh: { path: 'meshes/housing.stl', scale: [1, 1, 1] } },
          position_m: [0, 0, 0], orientation_xyzw: [0, 0, 0, 1], mesh_asset: housing },
        { link: 'tool_body', part: 'bracket', geometry: { mesh: { path: 'meshes/bracket.dae', scale: [1, 1, 1] } },
          position_m: [0, 0, 0], orientation_xyzw: [0, 0, 0, 1],
          mesh_asset: { url: null, error: { code: 'scene_mesh_input_invalid', message: 'DAE conversion is unsupported' } } },
      ], tcps: [] },
      toolEnds: [], collisionBodies: [], tcps: [],
    });
    const preview = parseToolPreviewResponse(response({ url, error: null }));
    if (!preview.ok) throw new Error('expected a preview');
    expect(preview.shapes.map((shape) => shape.mesh_asset?.url ?? null)).toEqual([url, null]);
    const html = renderToStaticMarkup(<ToolPreview preview={preview} />);
    expect(html).not.toContain('meshes/housing.stl');
    expect(html).toContain('Meshes not drawn (red boxes): meshes/bracket.dae (DAE conversion is unsupported).');
    expect(() => parseToolPreviewResponse(response(undefined))).toThrow('mesh_asset');
  });

  it('offers the mesh picker beside a mesh path and the export copy option', () => {
    const html = renderToStaticMarkup(<GeneratedToolEditor controller={controller({
      ...parameters, body: [{ name: 'housing', collision: { mesh: { path: '../meshes/housing.stl' } } }],
    })} componentId="gripper" />);
    expect(html).toContain('value="../meshes/housing.stl"');
    // Disabled until the preview names the mesh folder.
    expect(html).toMatch(/<button[^>]*disabled=""[^>]*>Browse approved meshes<\/button>/);
    expect(html).toContain('Copy meshes into the export folder');
  });

  it('offers a spring slider release depth beside its trip depth', () => {
    const html = renderToStaticMarkup(<GeneratedToolEditor controller={controller({
      ...parameters, chains: [{ attach_to: 'body', elements: [{ spring_slider: {
        id: 'rod', parts: [{ name: 'r', collision: { cylinder: { radius_m: 0.005, length_m: 0.1 } } }],
        stroke_m: 0.02, trip_depth_m: 0.0095 } }] }],
      tcps: [{ id: 'tcp', attach_to: 'rod' }],
    })} componentId="gripper" />);
    expect(html.indexOf('Trip depth')).toBeLessThan(html.indexOf('Set release depth'));
  });

  it('fits the preview to the tool and draws it transparent until made opaque', () => {
    const preview = parseToolPreviewResponse({
      schemaVersion: 'manyforge.composer_tool_preview.v1', ok: true, meshFolder: null,
      preview: { shapes: [], tcps: [] }, toolEnds: [], collisionBodies: [], tcps: [],
    });
    if (!preview.ok) throw new Error('expected a preview');
    const html = renderToStaticMarkup(<ToolPreview preview={preview} />);
    expect(html).toContain('aria-label="Fit the tool in the preview"');
    expect(html).toMatch(/<button[^>]*aria-pressed="false"[^>]*>Opaque<\/button>/);
    expect(TOOL_PREVIEW_OPACITY).toBe(0.5);
  });

  it('writes a picked mesh relative to the mesh folder', () => {
    expect(relativeMeshPath('/work/cell.scenario', '/work/cell.scenario/meshes/jaw.stl')).toBe('meshes/jaw.stl');
    expect(relativeMeshPath('/work/cell.scenario', '/work/meshes/jaw.stl')).toBe('../meshes/jaw.stl');
    expect(relativeMeshPath('/work/cell.scenario/', '/shared/parts/jaw.dae')).toBe('../../shared/parts/jaw.dae');
  });
});
