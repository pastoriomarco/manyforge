import { act } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type { ComponentCatalog, ComponentCatalogEntry } from '../types/componentCatalog';
import { reactPropsHost, type MountedProps } from '../test/reactPropsHost';
import { roleFeedback, componentManagerRows } from '../utils/componentManager';
import { AddComponentPanel } from './AddComponentPanel';

const robotRow = {
  component_id: 'lite6_robot',
  component_profile_id: 'lite6_mock_commanded_v0',
  frame_prefix: '',
  model_source: { urdf: { project_path: '../models/lite6.urdf' }, package_roots: {} },
  root_pose_in_cell: { position_m: [0, 0, 0], orientation_xyzw: [0, 0, 0, 1] },
  target_settings: {},
  configuration: {
    runtime_kind: 'ros2_control',
    runtime_configuration: {
      component_streams: [{ controllable_component_id: 'lite6_robot', stream_id: 'measured_joints' }],
    },
  },
};

const robot: ComponentCatalogEntry = {
  entryId: 'lite6_pick_and_place.scenario/lite6_robot',
  kind: 'robot',
  componentId: 'lite6_robot',
  componentProfileId: 'lite6_mock_commanded_v0',
  modelFamilyId: 'lite6',
  tags: ['articulated-arm'],
  lane: 'manyforge-only',
  description: 'Mock Lite6',
  sources: ['examples/lite6_pick_and_place.scenario'],
  components: [robotRow],
};

// A mounted tool row carries no root_pose_in_cell (its mount row places it).
const toolRow = {
  component_id: 'lite_gripper',
  component_profile_id: 'lite_gripper_v0',
  frame_prefix: '',
  model_source: { urdf: { project_path: '../models/lite_gripper.urdf' }, package_roots: {} },
  target_settings: {},
  configuration: {
    runtime_kind: 'ros2_control',
    runtime_configuration: {
      gripper_target_configurations: [{ controllable_component_id: 'lite_gripper', command_target_id: 'gripper' }],
    },
  },
};

const tool: ComponentCatalogEntry = {
  entryId: 'lite6_pick_and_place.scenario/lite_gripper',
  kind: 'tool',
  componentId: 'lite_gripper',
  componentProfileId: 'lite_gripper_v0',
  modelFamilyId: 'lite_gripper',
  tags: ['gripper', 'tool'],
  lane: 'manyforge-only',
  description: 'Lite gripper',
  sources: ['examples/lite6_pick_and_place.scenario'],
  components: [toolRow],
};

const mountRow = (robotId: string, toolId: string) => ({
  parent: { component_id: robotId, interface: 'flange' },
  child: { component_id: toolId, interface: 'mount' },
});

const catalog: ComponentCatalog = {
  schemaVersion: 'manyforge.composer_component_catalog.v1',
  projectRoot: '/projects/cell.scenario',
  entries: [robot, tool],
  profilesWithoutEntry: ['uf850_mock_commanded_v0'],
  componentProfiles: {
    lite6_mock_commanded_v0: { tags: ['articulated-arm'], attachmentInterfaces: ['flange'], tcpTargets: [] },
    lite_gripper_v0: { tags: ['gripper', 'tool'], attachmentInterfaces: [], tcpTargets: [] },
  },
};

afterEach(() => vi.unstubAllGlobals());

async function mount(components: JsonValue[], {
  apiVersion = 'manyforge.deployment_intent.v2',
  assignment = null as Record<string, JsonValue> | null,
  attachments = [] as JsonValue[],
} = {}) {
  const document = {
    api_version: apiVersion,
    deployment_artifact_id: 'cell',
    name: 'Cell',
    components,
    attachments,
    policies: {},
  };
  const controller = {
    workspace: { draft: { id: 'draft', revision: 3, document } },
    mutationAvailable: true,
    mutationUnavailableReason: null,
    busy: false,
    addComponents: vi.fn().mockResolvedValue({}),
  } as unknown as DeploymentWorkspaceController;
  const rows = componentManagerRows(document, null, catalog.componentProfiles);
  const feedback = roleFeedback(
    { robot: { required_component_type_tags: ['articulated-arm'] } },
    assignment, rows, catalog.componentProfiles,
  );
  const onAdded = vi.fn();
  const { host, find, all } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<AddComponentPanel controller={controller} catalog={catalog}
    catalogError={null} feedback={feedback} onAdded={onAdded} />));
  const control = (label: string) => find((props) => props['aria-label'] === label);
  return {
    controller, control, all, find, onAdded,
    type: (label: string, value: string) => act(() => control(label).onChange?.({ target: { value } })),
    click: (text: string) => act(() => find((props) => props.type === 'button' && props.children === text).onClick?.()),
    pick: () => act(() => find((props) => props.role === 'option').onClick?.()),
    kind: (label: string) => act(() => find((props) => props.role === 'radio' && props.children === label).onClick?.()),
    unmount: () => act(() => root.unmount()),
  };
}

function alerts(all: (match: (props: MountedProps) => boolean) => MountedProps[]) {
  return all((props) => props.role === 'alert').map((props) => String(props.children));
}

describe('Add component', () => {
  it('adds a catalog robot to the Draft with one edit and opens its page', async () => {
    const mounted = await mount([]);
    try {
      await mounted.click('Add component');
      await mounted.pick();
      expect(mounted.control('Component id for lite6_robot').value).toBe('lite6_robot');
      await mounted.type('New component base y', '0.5');
      await mounted.click('Add to Draft');
      expect(mounted.controller.addComponents).toHaveBeenCalledExactlyOnceWith([{
        ...robotRow,
        frame_prefix: '',
        root_pose_in_cell: { position_m: [0, 0.5, 0], orientation_xyzw: [0, 0, 0, 1] },
      }], []);
      // Its Components page opens.
      expect(mounted.onAdded).toHaveBeenCalledExactlyOnceWith('lite6_robot');
    } finally { await mounted.unmount(); }
  });

  it('proposes a free id and a frame prefix for a second copy of the same model', async () => {
    const mounted = await mount([robotRow]);
    try {
      await mounted.click('Add component');
      await mounted.pick();
      expect(mounted.control('Component id for lite6_robot').value).toBe('lite6_robot_2');
      expect(mounted.control('New component frame prefix').value).toBe('lite6_robot_2/');
      await mounted.click('Add to Draft');
      const [rows] = vi.mocked(mounted.controller.addComponents).mock.calls[0];
      expect(rows[0].component_id).toBe('lite6_robot_2');
      expect(JSON.stringify(rows[0])).not.toContain('"lite6_robot"');
    } finally { await mounted.unmount(); }
  });

  it('proposes the id an open Program role is assigned to', async () => {
    const mounted = await mount([], {
      assignment: { component_roles: [{ component_role_id: 'robot', controllable_component_id: 'arm_a' }] },
    });
    try {
      await mounted.click('Add component');
      expect(String(mounted.find((props) => props.role === 'option').title)).toBe('Mock Lite6');
      await mounted.pick();
      expect(mounted.control('Component id for lite6_robot').value).toBe('arm_a');
    } finally { await mounted.unmount(); }
  });

  it('refuses an id the Deployment already has without sending anything', async () => {
    const mounted = await mount([{ ...robotRow, component_id: 'other', model_source: { urdf: { project_path: 'x.urdf' }, package_roots: {} } }]);
    try {
      await mounted.click('Add component');
      await mounted.pick();
      await mounted.type('Component id for lite6_robot', 'other');
      await mounted.click('Add to Draft');
      expect(mounted.controller.addComponents).not.toHaveBeenCalled();
      expect(alerts(mounted.all).join()).toContain('already has a component "other"');
    } finally { await mounted.unmount(); }
  });

  it('points to the notices for an owner refusal, neutral and without an alert', async () => {
    const mounted = await mount([]);
    try {
      vi.mocked(mounted.controller.addComponents).mockResolvedValueOnce(null);
      await mounted.click('Add component');
      await mounted.pick();
      await mounted.click('Add to Draft');
      expect(alerts(mounted.all)).toEqual([]);
      const pointer = mounted.find((props) => props.role === 'status'
        && String(props.children).includes('did not add this component'));
      expect(pointer.className).toBe('inline-message severity-info');
      expect(mounted.onAdded).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });

  it('mounts a tool on a robot with a free flange: the tool and its attachment row in one edit', async () => {
    const mounted = await mount([robotRow]);
    try {
      await mounted.click('Add component');
      await mounted.kind('Tool');
      await mounted.pick();
      expect(mounted.control('New tool mount').value).toBe('lite6_robot');
      expect(mounted.all((props) => props['aria-label'] === 'New component base x')).toEqual([]);
      await mounted.click('Add to Draft');
      expect(mounted.controller.addComponents).toHaveBeenCalledExactlyOnceWith(
        [{ ...toolRow, frame_prefix: '' }],
        [mountRow('lite6_robot', 'lite_gripper')],
      );
      expect(vi.mocked(mounted.controller.addComponents).mock.calls[0][0][0])
        .not.toHaveProperty('root_pose_in_cell');
      expect(mounted.onAdded).toHaveBeenCalledExactlyOnceWith('lite_gripper');
    } finally { await mounted.unmount(); }
  });

  it('binds an added tool\'s I/O companion to the robot it is mounted on', async () => {
    // A Real pneumatic tool's valve is on the robot controller (`xarm_digital`):
    // mounted on the second robot, it must drive that robot's GPIO.
    const valveRow = {
      component_id: 'valve_io',
      component_profile_id: 'io_device_v0',
      target_settings: {},
      configuration: {
        runtime_kind: 'io_provider',
        runtime_configuration: {
          io_provider_devices: [{ controllable_component_id: 'valve_io' }],
          io_signals: [{
            controllable_component_id: 'valve_io', signal_name: 'valve', source_id: 'valve_io.valve',
            direction: 'output',
            provider: { profile: 'xarm_digital', robot_component_id: 'lite6_robot', bank: 'controller', pin: 15 },
          }],
        },
      },
    };
    const secondRobot = {
      ...robotRow,
      component_id: 'lite6_robot_2',
      frame_prefix: 'lite6_robot_2/',
      root_pose_in_cell: { position_m: [0, 1, 0], orientation_xyzw: [0, 0, 0, 1] },
    };
    catalog.entries[1] = { ...tool, components: [toolRow, valveRow] };
    const mounted = await mount([robotRow, secondRobot]);
    try {
      await mounted.click('Add component');
      await mounted.kind('Tool');
      await mounted.pick();
      await mounted.type('New tool mount', 'lite6_robot_2');
      await mounted.click('Add to Draft');
      const [rows, attachments] = vi.mocked(mounted.controller.addComponents).mock.calls[0];
      expect(attachments).toEqual([mountRow('lite6_robot_2', 'lite_gripper')]);
      const valve = JSON.stringify(rows.find((row) => row.component_id === 'valve_io'));
      expect(valve).toContain('"robot_component_id":"lite6_robot_2"');
      expect(valve).not.toContain('"lite6_robot"');
    } finally {
      catalog.entries[1] = tool;
      await mounted.unmount();
    }
  });

  it('never writes a root pose for a tool, even when its catalog row had one', async () => {
    const posed = { ...toolRow, root_pose_in_cell: { position_m: [0, 0, 0], orientation_xyzw: [0, 0, 0, 1] } };
    catalog.entries[1] = { ...tool, components: [posed] };
    const mounted = await mount([robotRow]);
    try {
      await mounted.click('Add component');
      await mounted.kind('Tool');
      await mounted.pick();
      await mounted.click('Add to Draft');
      const [rows] = vi.mocked(mounted.controller.addComponents).mock.calls[0];
      expect(rows[0]).not.toHaveProperty('root_pose_in_cell');
      expect(rows[0].component_id).toBe('lite_gripper');
    } finally {
      catalog.entries[1] = tool;
      await mounted.unmount();
    }
  });

  it('refuses a tool when no robot flange is free (one tool per flange)', async () => {
    const mounted = await mount([robotRow, toolRow], { attachments: [mountRow('lite6_robot', 'lite_gripper')] });
    try {
      await mounted.click('Add component');
      await mounted.kind('Tool');
      await mounted.pick();
      expect(mounted.control('Component id for lite_gripper').value).toBe('lite_gripper_2');
      expect(mounted.control('New component frame prefix').value).toBe('lite_gripper_2/');
      expect(mounted.control('New tool mount').value).toBe('');
      await mounted.click('Add to Draft');
      expect(mounted.controller.addComponents).not.toHaveBeenCalled();
      expect(alerts(mounted.all).join()).toContain('free flange');
    } finally { await mounted.unmount(); }
  });

  it('is unavailable for a compiled (non-Scenario) Deployment', async () => {
    const mounted = await mount([], { apiVersion: 'manyforge.deployment.v1' });
    try {
      const button = mounted.find((props) => props.type === 'button' && props.children === 'Add component');
      expect(button.disabled).toBe(true);
      expect(String(button.title)).toContain('Scenario Deployment');
    } finally { await mounted.unmount(); }
  });
});
