import { act } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import { reactPropsHost, type MountedProps } from '../test/reactPropsHost';
import { DeploymentFocusedEditor } from './DeploymentFocusedEditor';

const signal = {
  controllable_component_id: 'station_io',
  command_target_id: 'io',
  resource_role_id: 'part_present',
  signal_name: 'part_present',
  source_id: 'station_mock',
  value_type: 'bool',
  direction: 'input',
  update_mode: 'change_only',
  transition_capability: 'sampled_only',
  freshness: { mode: 'provider_validity' },
  provider: { profile: 'mock' },
};

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
      io_signals: [signal],
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
  const has = (label: string) => all((props) => props['aria-label'] === label).length > 0;
  return {
    controller, control, has, all,
    type: (label: string, value: string) => act(() => control(label).onChange?.({ target: { value } })),
    click: (text: string) => act(() => find((props) => props.type === 'button' && props.children === text).onClick?.()),
    unmount: () => act(() => root.unmount()),
  };
}

function alerts(all: (match: (props: MountedProps) => boolean) => MountedProps[]) {
  return all((props) => props.role === 'alert').map((props) => String(props.children));
}

describe('Deployment I/O devices', () => {
  it('adds one model-free I/O device with one components append', async () => {
    const mounted = await mount([arm]);
    try {
      await mounted.click('Add I/O device');
      expect(mounted.control('I/O device component id').value).toBe('io_device_1');
      expect(mounted.control('I/O device lifecycle command time limit').value).toBe('30');
      // The first signal belongs to the device; every R5 source incl. robot-hosted UR / GPIO / xArm is offered (contract 84).
      expect(mounted.control('Signal 1 Component').readOnly).toBe(true);
      const profiles = (mounted.control('Signal 1 Source profile').children as Array<{ props: { value: string } }>)
        .map((option) => option.props.value);
      expect(profiles).toEqual(['mock', 'ros_bool', 'ur_io_states', 'ros2_control_gpio', 'geometric_contact', 'xarm_digital']);
      await mounted.click('Create I/O device');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      expect(alerts(mounted.all).join()).toContain('signal name');
      await mounted.type('I/O device component id', 'station_io');
      await mounted.type('I/O device lifecycle command time limit', '0');
      await mounted.type('Signal 1 Signal name', 'part_present');
      await mounted.type('part_present Source', 'station_mock');
      await mounted.click('Create I/O device');
      expect(alerts(mounted.all).join()).toContain('above 0');
      await mounted.type('I/O device lifecycle command time limit', '3601');
      await mounted.click('Create I/O device');
      expect(alerts(mounted.all).join()).toContain('at most 3600');
      await mounted.type('I/O device lifecycle command time limit', '12');
      await mounted.click('Create I/O device');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(['components'], 'append', device);
    } finally { await mounted.unmount(); }
  });

  it('refuses a duplicate component id', async () => {
    const mounted = await mount([arm]);
    try {
      await mounted.click('Add I/O device');
      await mounted.type('I/O device component id', 'arm');
      await mounted.type('Signal 1 Signal name', 'part_present');
      await mounted.click('Create I/O device');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      expect(alerts(mounted.all).join()).toContain('already exists');
    } finally { await mounted.unmount(); }
  });

  it('edits the device time limit as its one io_provider_devices row and skips motion sections', async () => {
    const mounted = await mount([arm, device]);
    try {
      expect(mounted.control('station_io lifecycle command time limit').value).toBe('12');
      await mounted.type('station_io lifecycle command time limit', '45');
      await act(() => (mounted.control('station_io lifecycle command time limit').onBlur as () => void)());
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['components', 1, 'configuration', 'runtime_configuration', 'io_provider_devices'], 'set',
        [{ controllable_component_id: 'station_io', lifecycle_command_timeout_s: 45 }],
      );
      // The native runtime section lists the arm only; no frame or Stop field for the device.
      const units = mounted.all((props) => props.className === 'deployment-runtime-unit')
        .map((props) => ((props.children as Array<{ props?: { children?: Array<{ props?: { children?: unknown } }> } }>)[0]
          ?.props?.children?.[0]?.props?.children));
      expect(units.filter((id) => id === 'arm')).toHaveLength(1);
      expect(units.filter((id) => id === 'station_io')).toHaveLength(1);
      expect(mounted.has('Motion limits target')).toBe(false);
      await mounted.click('Remove I/O device');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(['components', 1], 'remove');
    } finally { await mounted.unmount(); }
  });

  it('keeps at least one signal on an I/O device and refuses xArm inputs there', async () => {
    const mounted = await mount([device]);
    try {
      await mounted.click('Remove signal');
      await mounted.click('Update I/O Draft');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      expect(alerts(mounted.all).join()).toContain('at least one signal');
      await mounted.click('Discard I/O edits');
      await mounted.click('Add output signal');
      await mounted.type('Signal 2 Signal name', 'lamp');
      await mounted.type('lamp Resource role', 'lamp');
      await mounted.type('lamp Target', 'io');
      await mounted.type('lamp Source', 'lamp_mock');
      await mounted.type('lamp Output profile', 'mock_output');
      await mounted.type('lamp Channel', 'lamp');
      await mounted.click('Update I/O Draft');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['components', 0, 'configuration', 'runtime_configuration', 'io_signals'], 'set',
        [signal, {
          controllable_component_id: 'station_io', command_target_id: 'io', resource_role_id: 'lamp',
          signal_name: 'lamp', source_id: 'lamp_mock', value_type: 'bool', direction: 'output',
          provider: { profile: 'mock_output', channel: 'lamp' },
        }],
      );
    } finally { await mounted.unmount(); }
  });

  it('refuses an authored xArm input row on an I/O device', async () => {
    const xarm = {
      ...signal, resource_role_id: 'tool_in', signal_name: 'tool_in', source_id: 'xarm_io', update_mode: 'sampled',
      freshness: { mode: 'sample_age', maximum_age_ns: 100_000_000 },
      provider: { profile: 'xarm_controller_input', topic: '/xarm/io', input_bit: 0 },
    };
    const withXarm = structuredClone(device) as typeof device;
    (withXarm.configuration.runtime_configuration.io_signals as JsonValue[]).push(xarm);
    const mounted = await mount([withXarm]);
    try {
      await mounted.type('part_present Initial value', 'true');
      await mounted.click('Update I/O Draft');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      expect(alerts(mounted.all).join()).toContain('xArm');
    } finally { await mounted.unmount(); }
  });
});
