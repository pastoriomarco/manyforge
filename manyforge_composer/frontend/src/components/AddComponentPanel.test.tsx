import { act } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type { ComponentCatalog, ComponentCatalogEntry } from '../types/componentCatalog';
import { reactPropsHost, type MountedProps } from '../test/reactPropsHost';
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
  entryId: 'mock_lite6_pick_and_place.scenario/lite6_robot',
  kind: 'robot',
  componentId: 'lite6_robot',
  componentProfileId: 'lite6_mock_commanded_v0',
  modelFamilyId: 'lite6',
  tags: ['articulated-arm'],
  lane: 'manyforge-only',
  description: 'Mock Lite6',
  sources: ['examples/mock_lite6_pick_and_place.scenario'],
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
  entryId: 'mock_lite6_pick_and_place.scenario/lite_gripper',
  kind: 'tool',
  componentId: 'lite_gripper',
  componentProfileId: 'lite_gripper_v0',
  modelFamilyId: 'lite_gripper',
  tags: ['gripper', 'tool'],
  lane: 'manyforge-only',
  description: 'Lite gripper',
  sources: ['examples/mock_lite6_pick_and_place.scenario'],
  components: [toolRow],
};

const sensorRow = {
  component_id: 'pointcloud_source',
  component_profile_id: 'manyforge.pointcloud_source',
  target_settings: {},
  configuration: { runtime_kind: 'sensor', runtime_configuration: {} },
};

const sensor: ComponentCatalogEntry = {
  entryId: 'model/manyforge.pointcloud_source',
  kind: 'sensor',
  componentId: 'pointcloud_source',
  componentProfileId: 'manyforge.pointcloud_source',
  modelFamilyId: null,
  tags: [],
  lane: null,
  description: 'Point-cloud source (PointCloud2 to the 3D view)',
  sources: ['components/sensor/manyforge.pointcloud_source/component.yaml'],
  components: [sensorRow],
};

const mountRow = (robotId: string, toolId: string) => ({
  parent: { component_id: robotId, interface: 'flange' },
  child: { component_id: toolId, interface: 'mount' },
});

const catalog: ComponentCatalog = {
  schemaVersion: 'manyforge.composer_component_catalog.v1',
  projectRoot: '/projects/cell.scenario',
  entries: [robot, tool, sensor],
  machines: [{
    modelId: 'ufactory.lite6_machine', title: 'UFactory Lite 6', lanes: ['mock', 'real'],
    defaultOwnerId: 'lite6',
    ownerRow: { component_id: 'lite6', component_profile_id: 'ufactory.lite6_machine', configuration: {} },
    members: [
      { role: 'arm', modelId: 'lite6_mock_commanded_v0', title: 'Arm', row: {
        ...robotRow, component_id: 'lite6_arm',
        configuration: { runtime_kind: 'ros2_control', runtime_configuration: {
          component_streams: [{ controllable_component_id: 'lite6_arm', stream_id: 'measured_joints' }] } } } },
      { role: 'controller_io', modelId: 'ufactory.lite6_controller_io', title: 'Controller I/O',
        row: { component_id: 'lite6_controller_io', component_profile_id: 'ufactory.lite6_controller_io' } },
    ],
  }],
  profilesWithoutEntry: ['uf850_mock_commanded_v0'],
  componentProfiles: {
    lite6_mock_commanded_v0: { tags: ['articulated-arm'], attachmentInterfaces: ['flange'], tcpTargets: [] },
    lite_gripper_v0: { tags: ['gripper', 'tool'], attachmentInterfaces: [], tcpTargets: [] },
    'manyforge.pointcloud_source': { tags: [], attachmentInterfaces: [], tcpTargets: [], lanes: [] },
  },
};

afterEach(() => vi.unstubAllGlobals());

async function mount(components: JsonValue[], {
  apiVersion = 'manyforge.deployment_intent.v2',
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
  const onAdded = vi.fn();
  const { host, find, all } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<AddComponentPanel controller={controller} catalog={catalog}
    catalogError={null} onAdded={onAdded} />));
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
  it('adds a machine model: the owner on its lane plus its members, ids following the owner', async () => {
    const mounted = await mount([]);
    try {
      await mounted.click('Add component');
      await mounted.kind('Machine');
      await mounted.pick();
      expect(mounted.control('Component id for lite6').value).toBe('lite6');
      expect(mounted.control('Component id for lite6_arm').value).toBe('lite6_arm');
      expect(mounted.control('Component id for lite6_controller_io').value).toBe('lite6_controller_io');
      // Renaming the owner moves the member ids still at their default.
      await mounted.type('Component id for lite6', 'cell_a');
      expect(mounted.control('Component id for lite6_arm').value).toBe('cell_a_arm');
      await mounted.type('Component id for lite6_controller_io', 'cell_a_plc_io');
      await mounted.type('Component id for lite6', 'cell_b');
      expect(mounted.control('Component id for lite6_arm').value).toBe('cell_b_arm');
      expect(mounted.control('Component id for lite6_controller_io').value).toBe('cell_a_plc_io');
      await mounted.type('New machine lane', 'real');
      await mounted.type('New component base y', '0.5');
      await mounted.click('Add to Draft');
      const [rows, attachments] = vi.mocked(mounted.controller.addComponents).mock.calls[0];
      expect(attachments).toEqual([]);
      expect(rows[0]).toEqual({
        component_id: 'cell_b', component_profile_id: 'ufactory.lite6_machine', lane: 'real', configuration: {},
      });
      expect(rows[1]).toMatchObject({ component_id: 'cell_b_arm', component_profile_id: 'lite6_mock_commanded_v0',
        owner: 'cell_b', role: 'arm', model_source: robotRow.model_source,
        root_pose_in_cell: { position_m: [0, 0.5, 0], orientation_xyzw: [0, 0, 0, 1] } });
      expect(rows[1]).not.toHaveProperty('lane');
      // Inner references follow the rename, as for any catalog entry.
      expect(JSON.stringify(rows[1])).toContain('"controllable_component_id":"cell_b_arm"');
      expect(JSON.stringify(rows[1])).not.toContain('lite6_arm');
      expect(rows[2]).toEqual({ component_id: 'cell_a_plc_io', component_profile_id: 'ufactory.lite6_controller_io',
        owner: 'cell_b', role: 'controller_io' });
      expect(mounted.onAdded).toHaveBeenCalledExactlyOnceWith('cell_b');
    } finally { await mounted.unmount(); }
  });

  it('adds a sensor model without a lane with its topic and source frame; refuses a relative topic or no frame', async () => {
    const mounted = await mount([]);
    try {
      await mounted.click('Add component');
      await mounted.kind('Sensor');
      await mounted.pick();
      // A sensor has no lane: no lane choice.
      expect(() => mounted.control('New sensor lane')).toThrow();
      await mounted.type('New sensor topic', 'camera/points');
      await mounted.click('Add to Draft');
      expect(mounted.controller.addComponents).not.toHaveBeenCalled();
      expect(alerts(mounted.all)).toEqual(['Topic must be an absolute ROS topic, e.g. /camera/points.']);
      await mounted.type('New sensor topic', '/camera/points');
      await mounted.click('Add to Draft');
      expect(mounted.controller.addComponents).not.toHaveBeenCalled();
      expect(alerts(mounted.all)).toEqual(['Enter the frame the cloud is published in.']);
      await mounted.type('New sensor source frame', 'camera_link');
      await mounted.click('Add to Draft');
      expect(mounted.controller.addComponents).toHaveBeenCalledExactlyOnceWith([{
        ...sensorRow,
        configuration: { runtime_kind: 'sensor',
          runtime_configuration: { topic: '/camera/points', source_frame_id: 'camera_link' } },
      }], []);
      expect(mounted.onAdded).toHaveBeenCalledExactlyOnceWith('pointcloud_source');
    } finally { await mounted.unmount(); }
  });

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

  it('binds an added tool\'s contact input device to the robot it is mounted on', async () => {
    // A tool's contact input follows the robot's Active TCP: mounted on the
    // second robot, it must follow that robot.
    const contactRow = {
      component_id: 'contact_io',
      component_profile_id: 'io_device_v0',
      target_settings: {},
      configuration: {
        runtime_kind: 'io_provider',
        runtime_configuration: {
          io_provider_devices: [{ controllable_component_id: 'contact_io' }],
          io_signals: [{
            controllable_component_id: 'contact_io', signal_name: 'contact', source_id: 'contact_io.contact',
            direction: 'input',
            provider: { profile: 'geometric_contact', robot_component_id: 'lite6_robot',
              target_scene_resource_id: 'part' },
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
    catalog.entries[1] = { ...tool, components: [toolRow, contactRow] };
    const mounted = await mount([robotRow, secondRobot]);
    try {
      await mounted.click('Add component');
      await mounted.kind('Tool');
      await mounted.pick();
      await mounted.type('New tool mount', 'lite6_robot_2');
      await mounted.click('Add to Draft');
      const [rows, attachments] = vi.mocked(mounted.controller.addComponents).mock.calls[0];
      expect(attachments).toEqual([mountRow('lite6_robot_2', 'lite_gripper')]);
      const contact = JSON.stringify(rows.find((row) => row.component_id === 'contact_io'));
      expect(contact).toContain('"robot_component_id":"lite6_robot_2"');
      expect(contact).not.toContain('"lite6_robot"');
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
