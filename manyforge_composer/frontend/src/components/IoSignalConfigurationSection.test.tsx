import { act } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import { reactPropsHost, type MountedProps } from '../test/reactPropsHost';
import { renderToStaticMarkup } from 'react-dom/server';

import {
  IoSignalConfigurationSection,
  type GeometricContactEndpointOption,
} from './IoSignalConfigurationSection';

const existing = {
  controllable_component_id: 'station',
  command_target_id: 'io',
  resource_role_id: 'part_present',
  signal_name: 'part_present',
  source_id: 'station_mock',
  value_type: 'bool',
  direction: 'input',
  update_mode: 'change_only',
  transition_capability: 'sampled_only',
  freshness: { mode: 'provider_validity' },
  provider: { profile: 'mock', initial_value: true },
};
const output = { ...existing, resource_role_id: 'valve', signal_name: 'valve', direction: 'output' };

afterEach(() => vi.unstubAllGlobals());

async function mount(
  rows: JsonValue[] | undefined,
  ioProvider = true,
  geometricContactEndpoints: GeometricContactEndpointOption[] = [],
) {
  const configuration: Record<string, JsonValue> = { mode: 'commanded_sim', ...(rows === undefined ? {} : { io_signals: rows }) };
  const controller = {
    mutationAvailable: true,
    busy: false,
    mutate: vi.fn().mockResolvedValue({}),
  } as unknown as DeploymentWorkspaceController;
  // An I/O device owns its rows (component station); an arm only lists its existing rows.
  const runtimes = [{
    index: 0, runtimeUnitId: ioProvider ? 'station' : 'arm', configuration,
    configurationPath: ['graph', 'runtime_units', 0, 'configuration'], intentComponent: false, ioProvider,
  }];
  const { host, find, all } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<IoSignalConfigurationSection controller={controller} runtimes={runtimes}
    geometricContactEndpoints={geometricContactEndpoints} />));
  const button = (text: string) => find((props) => props.type === 'button' && props.children === text);
  const texts = () => all(() => true).map((props) => (
    Array.isArray(props.children) ? props.children.filter((item) => typeof item === 'string').join('')
      : typeof props.children === 'string' ? props.children : ''));
  const control = (label: string) => find((props) => props['aria-label'] === label);
  const type = (label: string, value: string) => act(() => control(label).onChange?.({ target: { value } }));
  return {
    controller, button, control, type, all, texts,
    click: (text: string) => act(() => button(text).onClick?.()),
    unmount: () => act(() => root.unmount()),
  };
}

function hasAlert(all: (match: (props: MountedProps) => boolean) => MountedProps[], text: string) {
  return all((props) => props.role === 'alert').some((props) => String(props.children).includes(text));
}

describe('IoSignalConfigurationSection', () => {
  it('authors xArm input with the standard 250 ms age and selected bank', async () => {
    const mounted = await mount([existing]);
    try {
      await mounted.type('part_present Source profile', 'xarm_digital');
      await mounted.type('part_present Robot component', 'lite6_robot');
      await mounted.type('part_present Bank', 'controller');
      await mounted.type('part_present Pin', '15');
      expect(mounted.control('part_present Maximum sample age').value).toBe('250');
      await mounted.click('Update I/O Draft');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'set',
        [{ ...existing, update_mode: 'sampled', freshness: { mode: 'sample_age', maximum_age_ns: 250_000_000 },
          provider: { profile: 'xarm_digital', robot_component_id: 'lite6_robot', bank: 'controller', pin: 15 } }],
      );
    } finally { await mounted.unmount(); }
  });

  it('commits the whole io_signals array, keeping existing rows and outputs exact', async () => {
    const mounted = await mount([existing, output]);
    try {
      expect(mounted.button('Update I/O Draft').disabled).toBe(true);
      await mounted.click('Add input signal');
      for (const [label, value] of [
        ['Signal 3 Signal name', 'door_closed'],
      ] as const) await mounted.type(label, value);
      for (const [label, value] of [
        ['door_closed Component', 'station'],
        ['door_closed Target', 'io'],
        ['door_closed Resource role', 'door_closed'],
        ['door_closed Source profile', 'ros_bool'],
        ['door_closed Source', 'door_topic'],
      ] as const) await mounted.type(label, value);
      await mounted.type('door_closed Topic', '/cell/door');
      await mounted.type('door_closed Maximum sample age', '200');
      await mounted.click('Update I/O Draft');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'],
        'set',
        [existing, output, {
          ...existing,
          resource_role_id: 'door_closed', signal_name: 'door_closed', source_id: 'door_topic',
          update_mode: 'sampled',
          freshness: { mode: 'sample_age', maximum_age_ns: 200_000_000 },
          provider: { profile: 'ros_bool', topic: '/cell/door', liveness: { mode: 'periodic' } },
        }],
      );
    } finally { await mounted.unmount(); }
  });

  it('refuses an out-of-bounds value before any mutation', async () => {
    const mounted = await mount([existing]);
    try {
      await mounted.type('part_present Ingress depth', '0');
      await mounted.click('Update I/O Draft');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      expect(hasAlert(mounted.all, 'ingress depth')).toBe(true);
    } finally { await mounted.unmount(); }
  });

  it('declares a ROS SetBool output by switching direction', async () => {
    const mounted = await mount([existing]);
    try {
      const direction = mounted.control('part_present Direction');
      const options = direction.children as Array<{ props: { value: string; disabled?: boolean } }>;
      expect(options.map((option) => [option.props.value, option.props.disabled ?? false]))
        .toEqual([['input', false], ['output', false]]);
      await mounted.type('part_present Direction', 'output');
      await mounted.click('Update I/O Draft');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      expect(hasAlert(mounted.all, 'enter the ROS service name')).toBe(true);
      await mounted.type('part_present Service', 'cell/clamp');
      await mounted.click('Update I/O Draft');
      expect(hasAlert(mounted.all, 'absolute')).toBe(true);
      await mounted.type('part_present Service', '/cell/clamp/set');
      await mounted.click('Update I/O Draft');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'],
        'set',
        [{
          controllable_component_id: 'station', command_target_id: 'io', resource_role_id: 'part_present',
          signal_name: 'part_present', source_id: 'station_mock', value_type: 'bool', direction: 'output',
          provider: { profile: 'ros_set_bool', service: '/cell/clamp/set' },
        }],
      );
    } finally { await mounted.unmount(); }
  });

  it('removes the key when no signal remains (not on an I/O device, which needs one)', async () => {
    const mounted = await mount([existing], false);
    try {
      await mounted.click('Remove signal');
      await mounted.click('Update I/O Draft');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'remove',
      );
    } finally { await mounted.unmount(); }
  });

  it('declares a change-only ROS Bool: no sample age, a visible liveness warning', async () => {
    const mounted = await mount([existing]);
    try {
      await mounted.type('part_present Source profile', 'ros_bool');
      await mounted.type('part_present Topic', '/cell/door');
      await mounted.type('part_present Liveness', 'change_only');
      expect(mounted.all((props) => props['data-io-liveness-warning'] !== undefined)).toHaveLength(1);
      await mounted.click('Update I/O Draft');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'set',
        [{ ...existing, update_mode: 'change_only', freshness: { mode: 'provider_validity' },
          provider: { profile: 'ros_bool', topic: '/cell/door', liveness: { mode: 'change_only' } } }],
      );
    } finally { await mounted.unmount(); }
  });

  it('declares a UR io_states input (periodic, no liveness selector) with the driver note', async () => {
    const mounted = await mount([existing]);
    try {
      await mounted.type('part_present Source profile', 'ur_io_states');
      expect(() => mounted.control('part_present Liveness')).toThrow();
      expect(mounted.all((props) => props['data-io-driver-note'] !== undefined)).toHaveLength(1);
      await mounted.type('part_present Topic', '/io_and_status_controller/io_states');
      await mounted.type('part_present Pin', '17');
      await mounted.type('part_present Maximum sample age', '50');
      await mounted.click('Update I/O Draft');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'set',
        [{ ...existing, update_mode: 'sampled', freshness: { mode: 'sample_age', maximum_age_ns: 50_000_000 },
          provider: { profile: 'ur_io_states', topic: '/io_and_status_controller/io_states', pin: 17 } }],
      );
    } finally { await mounted.unmount(); }
  });

  it('declares a read-only geometric contact input from structural endpoint metadata', async () => {
    const endpoints: GeometricContactEndpointOption[] = [
      { componentId: 'arm', targetId: 'tool', frameRoleId: 'camera' },
      { componentId: 'arm', targetId: 'tool', frameRoleId: 'tcp' },
    ];
    const mounted = await mount([existing], true, endpoints);
    try {
      await mounted.type('part_present Source profile', 'geometric_contact');
      expect(mounted.all((props) => props.value === JSON.stringify(['arm', 'tool', 'camera']))).toHaveLength(1);
      expect(mounted.all((props) => props.value === JSON.stringify(['arm', 'tool', 'tcp']))).toHaveLength(1);
      await mounted.type('part_present Contact endpoint', JSON.stringify(['arm', 'tool', 'tcp']));
      await mounted.type('part_present Target Scene resource', 'workpiece');
      await mounted.type('part_present Maximum sample age', '75');
      expect(mounted.texts().some((text) => text.includes("selected endpoint's existing contact-body roles"))).toBe(true);
      expect(mounted.texts().some((text) => text.includes('Manual and timed Mock writes do not apply'))).toBe(true);
      await mounted.click('Update I/O Draft');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'set',
        [{ ...existing, update_mode: 'sampled',
          freshness: { mode: 'sample_age', maximum_age_ns: 75_000_000 }, provider: {
          profile: 'geometric_contact',
          endpoint: {
            controllable_component_id: 'arm', command_target_id: 'tool', frame_role_id: 'tcp',
          },
          target_scene_resource_id: 'workpiece',
        } }],
      );
    } finally { await mounted.unmount(); }
  });

  it('adds no signal to an arm: robot-hosted I/O is its own I/O device (contract 84)', async () => {
    const mounted = await mount([{ ...existing, controllable_component_id: 'arm' }], false);
    try {
      expect(() => mounted.button('Add input signal')).toThrow();
      expect(() => mounted.button('Add output signal')).toThrow();
      expect(mounted.texts().some((text) => text.includes('is its own I/O device'))).toBe(true);
      // Its existing rows can still be removed.
      await mounted.click('Remove signal');
      await mounted.click('Update I/O Draft');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'remove',
      );
    } finally { await mounted.unmount(); }
  });

  it('shows the sample age only for periodic inputs', () => {
    const ros = (mode: string) => ({ ...existing, update_mode: mode === 'periodic' ? 'sampled' : 'change_only',
      freshness: mode === 'periodic' ? { mode: 'sample_age', maximum_age_ns: 1_000_000 } : { mode: 'provider_validity' },
      provider: { profile: 'ros_bool', topic: '/t', liveness: { mode } } }) as unknown as JsonValue;
    const html = (row: JsonValue) => renderToStaticMarkup(
      <IoSignalConfigurationSection
        controller={{ mutationAvailable: true, busy: false, mutate: vi.fn() } as unknown as DeploymentWorkspaceController}
        runtimes={[{ index: 0, runtimeUnitId: 'station', configuration: { io_signals: [row] },
          configurationPath: ['graph', 'runtime_units', 0, 'configuration'], intentComponent: false, ioProvider: true }]} />,
    );
    expect(html(ros('periodic'))).toContain('aria-label="part_present Maximum sample age"');
    expect(html(ros('periodic'))).not.toContain('data-io-liveness-warning');
    expect(html(ros('change_only'))).not.toContain('Maximum sample age');
    expect(html(ros('change_only'))).toContain('data-io-liveness-warning');
  });
});
