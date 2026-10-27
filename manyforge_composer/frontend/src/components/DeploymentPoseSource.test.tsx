import { act } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import { reactPropsHost } from '../test/reactPropsHost';
import { DeploymentFocusedEditor } from './DeploymentFocusedEditor';

const arm = {
  component_id: 'arm',
  component_profile_id: 'robot_native_v1',
  model_source: { urdf: { project_path: 'sources/robot.urdf' }, package_roots: {} },
  root_pose_in_cell: { position_m: [0, 0, 0], orientation_xyzw: [0, 0, 0, 1] },
  target_settings: { arm: { maximum_tcp_linear_speed_mps: 0.2 } },
  configuration: { mode: 'simulated', execution_tick_hz: 100 },
};

const device = {
  component_id: 'station_io',
  component_profile_id: 'io_device_v0',
  target_settings: {},
  configuration: {
    runtime_kind: 'io_provider',
    runtime_configuration: {
      io_provider_devices: [{ controllable_component_id: 'station_io', lifecycle_command_timeout_s: 12 }],
      io_signals: [{
        controllable_component_id: 'station_io', command_target_id: 'io', resource_role_id: 'part_present',
        signal_name: 'part_present', source_id: 'station_mock', value_type: 'bool', direction: 'input',
        update_mode: 'change_only', transition_capability: 'sampled_only',
        freshness: { mode: 'provider_validity' }, provider: { profile: 'mock' },
      }],
    },
  },
};

const poseSource = {
  component_id: 'isaac_scene_objects',
  component_profile_id: 'pose_source_v0',
  target_settings: {},
  configuration: {
    runtime_kind: 'pose_source',
    runtime_configuration: {
      pose_sources: [{
        controllable_component_id: 'isaac_scene_objects',
        simulation_poll_period_ms: 250,
        joint_state: {
          eligible: false, lifetime_ns: 1000000000, object_id: 'robot', policy_id: 'viewer.measured_robot',
          required_joint_names: ['joint_1'], topic: '/isaac_joint_states',
        },
        simulation_entities: [{
          eligible: true, entity: '/mac_n_cheese_centered', frame_kind: 'cell_frame', lifetime_ns: 2000000000,
          local_orientation_xyzw: [0, 0, 0, 1], local_position_xyz: [0, 0, 0],
          object_id: 'isaac_cheese_box', policy_id: 'continuous_unattached',
          source_coordinate_frame_id: 'universe', source_header_frame_id: 'mac_n_cheese_centered',
          target_frame_id: 'universe',
        }],
        detection3d_sources: [],
      }],
    },
  },
};

afterEach(() => vi.unstubAllGlobals());

async function mount(components: JsonValue[]) {
  const controller = {
    workspace: {
      draft: {
        document: {
          api_version: 'manyforge.deployment_intent.v2',
          deployment_artifact_id: 'cell-a',
          name: 'Cell A',
          components,
          attachments: [],
          policies: {},
        },
      },
    },
    mutationAvailable: true,
    busy: false,
    mutate: vi.fn().mockResolvedValue({}),
    updateMotionTargetLimit: vi.fn(),
  } as unknown as DeploymentWorkspaceController;
  const { host, find, all } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<DeploymentFocusedEditor controller={controller}
    sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />));
  const control = (label: string) => find((props) => props['aria-label'] === label);
  return {
    controller, control, all,
    type: (label: string, value: string) => act(() => control(label).onChange?.({ target: { value } })),
    unmount: () => act(() => root.unmount()),
  };
}

describe('Deployment pose sources', () => {
  it('keeps a pose source out of the robot sections and shows it read-only', async () => {
    const mounted = await mount([arm, poseSource]);
    try {
      // The native runtime section lists the arm only.
      const units = mounted.all((props) => props.className === 'deployment-runtime-unit')
        .map((props) => ((props.children as Array<{ props?: { children?: Array<{ props?: { children?: unknown } }> } }>)[0]
          ?.props?.children?.[0]?.props?.children));
      expect(units).toEqual(['arm']);
      expect(mounted.control('isaac_scene_objects pose source')).toBeDefined();
      const cells = mounted.all((props) => typeof props.children === 'string').map((props) => props.children);
      for (const text of ['robot', 'viewer.measured_robot', 'isaac_cheese_box', 'continuous_unattached',
        '/mac_n_cheese_centered']) {
        expect(cells).toContain(text);
      }
      expect(cells).toContain(JSON.stringify(poseSource.configuration.runtime_configuration, null, 2));
      // No editable control is offered for it.
      expect(mounted.all((props) => typeof props.onChange === 'function'
        && String(props['aria-label'] ?? '').startsWith('isaac_scene_objects'))).toHaveLength(0);
    } finally { await mounted.unmount(); }
  });

  it('preserves the pose source unchanged when other components are edited', async () => {
    const original = structuredClone(poseSource);
    const mounted = await mount([arm, device, poseSource]);
    try {
      await mounted.type('station_io lifecycle command time limit', '45');
      await act(() => (mounted.control('station_io lifecycle command time limit').onBlur as () => void)());
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['components', 1, 'configuration', 'runtime_configuration', 'io_provider_devices'], 'set',
        [{ controllable_component_id: 'station_io', lifecycle_command_timeout_s: 45 }],
      );
      const document = mounted.controller.workspace!.draft.document as unknown as { components: JsonValue[] };
      expect(document.components[2]).toEqual(original);
    } finally { await mounted.unmount(); }
  });
});
