import { act } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type { ComponentProfileFacts } from '../types/componentCatalog';
import type { ComponentManagerRow } from '../utils/componentManager';
import { reactPropsHost } from '../test/reactPropsHost';
import { ComponentPlacementEditor } from './ComponentPlacementEditor';

afterEach(() => vi.unstubAllGlobals());

function modelRow(id: string, profile: string, placed: boolean): Record<string, JsonValue> {
  return {
    component_id: id,
    component_profile_id: profile,
    model_source: { urdf: { project_path: `${id}.urdf` }, package_roots: {} },
    target_settings: {},
    configuration: { runtime_kind: 'ros2_control' },
    ...(placed ? { root_pose_in_cell: { position_m: [0, 0, 0], orientation_xyzw: [0, 0, 0, 1] } } : {}),
  };
}

const profiles: Record<string, ComponentProfileFacts> = {
  arm_v0: { tags: ['articulated-arm'], attachmentInterfaces: ['flange'], tcpTargets: [], lanes: ['mock'] },
  'ufactory.lite_gripper': { tags: ['gripper', 'tool'], attachmentInterfaces: ['mount'], tcpTargets: [], lanes: ['mock'] },
  'manyforge.passive_tool': { tags: ['tool'], attachmentInterfaces: ['mount'], tcpTargets: [], lanes: [] },
};

const gripperMount = {
  parent: { component_id: 'arm', interface: 'flange' },
  child: { component_id: 'gripper', interface: 'mount' },
};

async function mount(componentId: 'gripper' | 'probe', attachments: JsonValue[]) {
  const document = {
    api_version: 'manyforge.deployment_intent.v3',
    components: [
      modelRow('arm', 'arm_v0', true),
      modelRow('gripper', 'ufactory.lite_gripper', false),
      // Added with no robot (OD-366): no mount, no root pose.
      modelRow('probe', 'manyforge.passive_tool', false),
    ],
    attachments,
  };
  const controller = {
    workspace: { draft: { document } },
    mutationAvailable: true,
    busy: false,
    loading: false,
    busyOperation: null,
    mutate: vi.fn().mockResolvedValue({}),
    replaceTool: vi.fn().mockResolvedValue({ lacks: null }),
    removeComponent: vi.fn(),
    previewApply: vi.fn(),
  } as unknown as DeploymentWorkspaceController;
  const row: ComponentManagerRow = {
    componentId,
    profileId: componentId === 'gripper' ? 'ufactory.lite_gripper' : 'manyforge.passive_tool',
    kind: 'tool',
    mount: componentId === 'gripper' && attachments.length > 0 ? 'arm.flange' : null,
    owner: null,
    role: null,
    active: true,
    change: 'added',
    depth: 0,
  };
  const onReplaced = vi.fn();
  const { host, find, all } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<ComponentPlacementEditor row={row} controller={controller}
    profiles={profiles} onRemoved={vi.fn()} onReplaced={onReplaced} />));
  const control = (label: string) => find((props) => props['aria-label'] === label);
  return {
    controller, control, find, all, onReplaced,
    choose: (label: string, value: string) => act(() => control(label).onChange?.({ target: { value } })),
    click: (text: string) => act(() => find((props) => props.type === 'button' && props.children === text).onClick?.()),
    unmount: () => act(() => root.unmount()),
  };
}

describe('Tool mount on its component page', () => {
  it('mounts a tool added without a robot in one Draft edit (OD-366)', async () => {
    const mounted = await mount('probe', []);
    try {
      expect(mounted.control('probe mounted on').value).toBe('');
      expect(mounted.find((props) => props.role === 'status'
        && String(props.children).startsWith('probe is not mounted'))).toBeDefined();
      // An unmounted tool has nothing to replace.
      expect(mounted.all((props) => props['aria-label'] === 'probe replace with')).toEqual([]);
      await mounted.choose('probe mounted on', 'arm');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(['attachments'], 'set', [{
        parent: { component_id: 'arm', interface: 'flange' },
        child: { component_id: 'probe', interface: 'mount' },
      }]);
    } finally { await mounted.unmount(); }
  });

  it('unmounts a mounted tool: its attachment row leaves, the tool stays', async () => {
    const mounted = await mount('gripper', [gripperMount]);
    try {
      expect(mounted.control('gripper mounted on').value).toBe('arm');
      await mounted.choose('gripper mounted on', '');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(['attachments'], 'set', []);
    } finally { await mounted.unmount(); }
  });

  it('replaces a mounted tool with another tool of the Draft (OD-367)', async () => {
    const mounted = await mount('gripper', [gripperMount]);
    try {
      const options = mounted.all((props) => props.value === 'probe' && props.children === 'probe');
      expect(options).toHaveLength(1);
      expect(mounted.all((props) => props.value === 'arm' && props.children === 'arm')).toEqual([]);
      expect(mounted.find((props) => props.type === 'button' && props.children === 'Replace').disabled).toBe(true);
      await mounted.choose('gripper replace with', 'probe');
      await mounted.click('Replace');
      expect(mounted.controller.replaceTool).toHaveBeenCalledExactlyOnceWith('gripper', 'probe');
      expect(mounted.onReplaced).toHaveBeenCalledExactlyOnceWith('probe');
    } finally { await mounted.unmount(); }
  });
});
