import { act } from 'react';
import { afterEach, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import { reactPropsHost, type MountedProps } from '../test/reactPropsHost';
import { GeneratedToolEditor } from './GeneratedToolEditor';

const apiMock = vi.hoisted(() => ({
  deploymentWorkspace: { toolPreview: vi.fn(), toolExport: vi.fn() },
  files: { browse: vi.fn() },
}));
vi.mock('../api/client', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/client')>()), api: apiMock,
}));
vi.mock('@react-three/fiber', () => ({ Canvas: () => null }));
vi.mock('@react-three/drei', () => ({ OrbitControls: () => null }));

afterEach(() => {
  vi.unstubAllGlobals();
  vi.clearAllMocks();
});

const parameters = {
  api_version: 'manyforge.parametric_tool.v1', kind: 'tool',
  body: [{ name: 'housing', collision: { mesh: { path: 'meshes/part.stl' } } }],
  chains: [{ attach_to: 'body', elements: [{ tip: {
    id: 'tip', parts: [{ name: 'ball', collision: { sphere: { radius_m: 0.003 } } }] } }] }],
  tcps: [{ id: 'tcp', attach_to: 'tip' }],
};

function controller(): DeploymentWorkspaceController {
  return {
    workspace: { draft: { document: {
      api_version: 'manyforge.deployment_intent.v3',
      components: [{
        component_id: 'probe', component_profile_id: 'manyforge.tool', target_settings: {},
        model_source: { parameters },
        configuration: { runtime_kind: 'ros2_control', runtime_configuration: {
          component_streams: [], gripper_target_configurations: [] } },
      }],
    } } },
    mutationAvailable: true, busy: false, loading: false,
    mutate: vi.fn().mockResolvedValue(null), updateMotionTargetLimit: vi.fn(),
  } as unknown as DeploymentWorkspaceController;
}

const holds = (props: MountedProps, text: string) => [props.children].flat(3).some((child) => (
  child === text || (child as { props?: { children?: unknown } } | null)?.props?.children === text));

it('writes the picked mesh relative to the Scenario folder and exports with the copy option', async () => {
  apiMock.deploymentWorkspace.toolPreview.mockResolvedValue({
    ok: true, meshFolder: '/work/cell.scenario', shapes: [], tcpFrames: [],
    toolEnds: [], collisionBodies: ['body'], tcps: [],
  });
  apiMock.files.browse.mockResolvedValue({
    currentPath: '/work/meshes', parentPath: '/work', roots: ['/work'],
    entries: [{ name: 'jaw.stl', path: '/work/meshes/jaw.stl', entryType: 'file', selectable: true }],
  });
  apiMock.deploymentWorkspace.toolExport.mockResolvedValue({ written: [] });
  const workspace = controller();
  const { host, find } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(async () => root.render(<GeneratedToolEditor controller={workspace} componentId="probe" />));

  await act(async () => find((props) => typeof props.onClick === 'function'
    && holds(props, 'Browse approved meshes')).onClick?.());
  expect(apiMock.files.browse).toHaveBeenCalledWith('tool_mesh', '/work/cell.scenario', expect.any(AbortSignal));
  await act(async () => find((props) => typeof props.onClick === 'function'
    && holds(props, 'jaw.stl')).onClick?.());
  expect(workspace.mutate).toHaveBeenCalledWith(
    ['components', 0, 'model_source', 'parameters'], 'set',
    expect.objectContaining({ body: [{ name: 'housing', collision: { mesh: { path: '../meshes/jaw.stl' } } }] }));

  await act(async () => find((props) => props['aria-label'] === 'Copy meshes into the export folder')
    .onChange?.({ target: { checked: true } }));
  await act(async () => find((props) => typeof props.onClick === 'function'
    && holds(props, 'Export')).onClick?.());
  expect(apiMock.deploymentWorkspace.toolExport).toHaveBeenCalledWith('probe', parameters, true);
  await act(async () => root.unmount());
});
