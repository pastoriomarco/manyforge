import { act } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import { reactPropsHost, type MountedProps } from '../test/reactPropsHost';
import { renderToStaticMarkup } from 'react-dom/server';

import {
  IoSignalConfigurationSection,
  type GeometricContactRobotOption,
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
  geometricContactRobots: GeometricContactRobotOption[] = [],
  sceneResourceIds: string[] = [],
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
    configurationPath: ['graph', 'runtime_units', 0, 'configuration'], intentComponent: true, ioProvider,
  }];
  const { host, find, all } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<IoSignalConfigurationSection controller={controller} runtimes={runtimes}
    geometricContactRobots={geometricContactRobots} sceneResourceIds={sceneResourceIds} />));
  const button = (text: string) => find((props) => props.type === 'button' && props.children === text);
  const texts = () => all(() => true).map((props) => (
    Array.isArray(props.children) ? props.children.filter((item) => typeof item === 'string').join('')
      : typeof props.children === 'string' ? props.children : ''));
  const control = (label: string) => find((props) => props['aria-label'] === label);
  const type = (label: string, value: string) => act(() => {
    const props = control(label);
    (props.onFocus as ((event: unknown) => void) | undefined)?.({ target: { value } });
    props.onChange?.({ target: { value } });
  });
  const blur = (label: string) => act(() => (control(label).onBlur as (() => void) | undefined)?.());
  return {
    controller, button, control, type, blur, all, texts,
    click: (text: string) => act(() => button(text).onClick?.()),
    unmount: () => act(() => root.unmount()),
  };
}

function hasAlert(all: (match: (props: MountedProps) => boolean) => MountedProps[], text: string) {
  return all((props) => props.role === 'alert').some((props) => String(props.children).includes(text));
}

describe('IoSignalConfigurationSection', () => {
  it('does not change an existing row on view and preselects the sole target only for a new row', async () => {
    const mounted = await mount([existing]);
    try {
      expect(() => mounted.button('Update I/O Draft')).toThrow();
      expect(mounted.control('part_present Target').value).toBe('io');
      expect(mounted.control('part_present Component').readOnly).toBe(true);
      expect(mounted.control('part_present Component').className).toBe('io-signal-readonly');
      expect(mounted.control('part_present Target').disabled).toBe(true);
      expect(mounted.control('part_present Target').className).toBe('io-signal-readonly');
      expect(mounted.control('part_present Signal name')).toBeDefined();
      expect(mounted.control('part_present Source')).toBeDefined();
      expect(mounted.texts()).toContain('Signal name');
      expect(mounted.texts()).toContain('Source');
      expect(mounted.texts()).not.toContain('part_present Signal name');
      expect(mounted.texts()).not.toContain('part_present Source');
      await mounted.click('Add input signal');
      expect(mounted.control('Signal 2 Component').value).toBe('station');
      expect(mounted.control('Signal 2 Component').readOnly).toBe(true);
      expect(mounted.control('Signal 2 Target').value).toBe('io');
      await mounted.type('Signal 2 Signal name', 'probe');
      await mounted.blur('Signal 2 Signal name');
      expect(mounted.control('probe Resource role').value).toBe('probe');
      expect(mounted.control('probe Source').value).toBe('station.probe');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      await mounted.click('Cancel');
      expect(() => mounted.control('Signal 2 Signal name')).toThrow();
    } finally { await mounted.unmount(); }
  });

  it('fills blank output role and source only on signal-name blur', async () => {
    const row = { controllable_component_id: 'station', command_target_id: 'io', resource_role_id: '',
      signal_name: 'valve', source_id: '', value_type: 'bool', direction: 'output',
      provider: { profile: 'mock_output', channel: 'valve' } };
    const mounted = await mount([row]);
    try {
      await mounted.type('valve Signal name', 'pump');
      expect(mounted.control('valve Resource role').value).toBe('');
      await mounted.blur('valve Signal name');
      expect(mounted.control('pump Resource role').value).toBe('pump');
      expect(mounted.control('pump Source').value).toBe('station.pump');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'set',
        [{ controllable_component_id: 'station', command_target_id: 'io', resource_role_id: 'pump',
          signal_name: 'pump', source_id: 'station.pump', value_type: 'bool', direction: 'output',
          provider: { profile: 'mock_output', channel: 'valve' } }],
      );
    } finally { await mounted.unmount(); }
  });

  it('does not fill empty values in an existing row just by viewing it', async () => {
    const mounted = await mount([{ ...existing, resource_role_id: '', source_id: '' }]);
    try {
      expect(mounted.control('part_present Resource role').value).toBe('');
      expect(mounted.control('part_present Source').value).toBe('');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      expect(() => mounted.button('Update I/O Draft')).toThrow();
    } finally { await mounted.unmount(); }
  });

  it('keeps an earlier valid field edit when the next field commits', async () => {
    const mounted = await mount([existing]);
    try {
      await mounted.type('part_present Ingress depth', '128');
      await mounted.blur('part_present Ingress depth');
      await mounted.type('part_present Initial value', 'false');
      expect(mounted.controller.mutate).toHaveBeenCalledTimes(2);
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'set',
        [{ ...existing, provider: { profile: 'mock', ingress_depth: 128, initial_value: false } }],
      );
    } finally { await mounted.unmount(); }
  });

  it('adds a new output only after its inline form is valid and can be cancelled', async () => {
    const mounted = await mount([existing]);
    try {
      await mounted.click('Add output signal');
      await mounted.click('Cancel');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      await mounted.click('Add output signal');
      await mounted.type('Signal 2 Signal name', 'valve_cmd');
      await mounted.blur('Signal 2 Signal name');
      await mounted.type('valve_cmd Output profile', 'mock_output');
      await mounted.type('valve_cmd Channel', 'valve_cmd');
      await mounted.blur('valve_cmd Channel');
      await mounted.click('Add signal');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'set',
        [existing, {
          controllable_component_id: 'station', command_target_id: 'io', resource_role_id: 'valve_cmd',
          signal_name: 'valve_cmd', source_id: 'station.valve_cmd', value_type: 'bool', direction: 'output',
          provider: { profile: 'mock_output', channel: 'valve_cmd' },
        }],
      );
    } finally { await mounted.unmount(); }
  });

  it('keeps a typed role and source when the signal name changes', async () => {
    const mounted = await mount([existing]);
    try {
      await mounted.click('Add input signal');
      await mounted.type('Signal 2 Signal name', 'first');
      await mounted.blur('Signal 2 Signal name');
      await mounted.type('first Resource role', 'custom_role');
      await mounted.blur('first Resource role');
      await mounted.type('first Source', 'custom_source');
      await mounted.blur('first Source');
      await mounted.type('first Signal name', 'second');
      await mounted.blur('first Signal name');
      expect(mounted.control('second Resource role').value).toBe('custom_role');
      expect(mounted.control('second Source').value).toBe('custom_source');
    } finally { await mounted.unmount(); }
  });

  it('keeps manually entered role and source when a new signal name changes', async () => {
    const mounted = await mount([existing]);
    try {
      await mounted.click('Add input signal');
      await mounted.type('Signal 2 Signal name', 'first');
      await mounted.blur('Signal 2 Signal name');
      await mounted.type('first Resource role', 'custom_role');
      await mounted.blur('first Resource role');
      await mounted.type('first Source', 'custom_source');
      await mounted.blur('first Source');
      await mounted.type('first Signal name', 'second');
      await mounted.blur('first Signal name');
      expect(mounted.control('second Resource role').value).toBe('custom_role');
      expect(mounted.control('second Source').value).toBe('custom_source');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });

  it('does not overwrite the next row after the preceding row is removed', async () => {
    const second = { ...existing, signal_name: 'other', resource_role_id: 'other_role', source_id: 'other_source' };
    const mounted = await mount([existing, second]);
    try {
      const remove = mounted.all((props) => props.type === 'button' && props.children === 'Remove signal');
      await act(() => remove[0].onClick?.());
      await mounted.type('other Signal name', 'renamed');
      await mounted.blur('other Signal name');
      expect(mounted.control('renamed Resource role').value).toBe('other_role');
      expect(mounted.control('renamed Source').value).toBe('other_source');
      expect(mounted.controller.mutate).toHaveBeenCalledTimes(2);
      expect(mounted.controller.mutate).toHaveBeenNthCalledWith(1,
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'set', [second]);
      expect(mounted.controller.mutate).toHaveBeenNthCalledWith(2,
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'set', [
          { ...second, signal_name: 'renamed' },
        ]);
    } finally { await mounted.unmount(); }
  });

  it('leaves the target empty when the device has multiple existing targets', async () => {
    const second = { ...existing, command_target_id: 'aux', signal_name: 'other', resource_role_id: 'other' };
    const mounted = await mount([existing, second]);
    try {
      await mounted.click('Add input signal');
      expect(mounted.control('Signal 3 Target').value).toBe('');
      expect(mounted.texts().join(' ')).toContain('aux');
      expect(mounted.texts().join(' ')).toContain('io');
    } finally { await mounted.unmount(); }
  });

  it('keeps Scene and Mock echo fields editable when their option lists are empty', async () => {
    const geo = { ...existing, signal_name: 'contact', resource_role_id: 'contact',
      update_mode: 'sampled', freshness: { mode: 'sample_age', maximum_age_ns: 100000000 },
      provider: { profile: 'geometric_contact', robot_component_id: 'robot', target_scene_resource_id: 'old_object' } };
    const echo = { ...existing, signal_name: 'echo', resource_role_id: 'echo', source_id: 'echo_source',
      provider: { profile: 'mock', echo: { output_signal_name: 'old_output', delay_ms: 0 } } };
    const mounted = await mount([geo, echo], true, [{ componentId: 'robot' }]);
    try {
      await mounted.type('contact Target Scene resource', 'probe_block');
      await mounted.type('echo Output signal name', 'future_output');
      expect(mounted.control('contact Target Scene resource').value).toBe('probe_block');
      expect(mounted.control('echo Output signal name').value).toBe('future_output');
    } finally { await mounted.unmount(); }
  });

  it('retains stale Scene and echo values and lists current Scene objects and Mock outputs', async () => {
    const geo = { ...existing, signal_name: 'contact', resource_role_id: 'contact',
      update_mode: 'sampled', freshness: { mode: 'sample_age', maximum_age_ns: 100000000 },
      provider: { profile: 'geometric_contact', robot_component_id: 'robot', target_scene_resource_id: 'old_object' } };
    const mockOutput = { controllable_component_id: 'station', command_target_id: 'io', resource_role_id: 'valve',
      signal_name: 'valve', source_id: 'valve_source', value_type: 'bool', direction: 'output',
      provider: { profile: 'mock_output', channel: 'valve' } };
    const echo = { ...existing, signal_name: 'echo', resource_role_id: 'echo', source_id: 'echo_source',
      provider: { profile: 'mock', echo: { output_signal_name: 'old_output', delay_ms: 0 } } };
    const mounted = await mount([geo, mockOutput, echo], true, [{ componentId: 'robot' }], ['probe_block']);
    try {
      expect(() => mounted.button('Update I/O Draft')).toThrow();
      expect(mounted.control('contact Target Scene resource').value).toBe('old_object');
      expect(mounted.control('echo Output signal name').value).toBe('old_output');
      expect(mounted.control('valve Component').readOnly).toBe(true);
      const text = mounted.texts().join(' ');
      expect(text).toContain('old_object (not currently listed)');
      expect(text).toContain('old_output (not currently listed)');
      expect(text).toContain('probe_block');
      expect(mounted.all((props) => props.value === 'valve' && props.children === 'valve').length).toBeGreaterThan(0);
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });

  it('commits the whole io_signals array, keeping existing rows and outputs exact', async () => {
    const mounted = await mount([existing, output]);
    try {
      expect(() => mounted.button('Update I/O Draft')).toThrow();
      await mounted.click('Add input signal');
      for (const [label, value] of [
        ['Signal 3 Signal name', 'door_closed'],
      ] as const) await mounted.type(label, value);
      await mounted.blur('Signal 3 Signal name');
      for (const [label, value] of [
        ['door_closed Target', 'io'],
        ['door_closed Resource role', 'door_closed'],
        ['door_closed Source profile', 'ros_bool'],
        ['door_closed Source', 'door_topic'],
      ] as const) {
        await mounted.type(label, value);
        if (label.endsWith('Source')) await mounted.blur(label);
      }
      await mounted.type('door_closed Topic', '/cell/door');
      await mounted.blur('door_closed Topic');
      await mounted.type('door_closed Maximum sample age', '200');
      await mounted.blur('door_closed Maximum sample age');
      await mounted.click('Add signal');
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
      await mounted.blur('part_present Ingress depth');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      expect(hasAlert(mounted.all, 'Ingress depth')).toBe(true);
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
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      expect(hasAlert(mounted.all, 'enter the ROS service name')).toBe(true);
      await mounted.type('part_present Service', 'cell/clamp');
      await mounted.blur('part_present Service');
      expect(hasAlert(mounted.all, 'absolute')).toBe(true);
      await mounted.type('part_present Service', '/cell/clamp/set');
      await mounted.blur('part_present Service');
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
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'remove',
      );
    } finally { await mounted.unmount(); }
  });

  it('disables removal of an I/O device’s last signal with a reason while keeping Add available', async () => {
    const mounted = await mount([existing]);
    try {
      const remove = mounted.button('Remove signal');
      expect(remove.disabled).toBe(true);
      expect(remove.title).toBe('An I/O device needs at least one signal; remove the device instead.');
      expect(hasAlert(mounted.all, 'at least one signal')).toBe(false);
      expect(mounted.controller.mutate).not.toHaveBeenCalled();

      expect(mounted.button('Add input signal').disabled).toBe(false);
      await mounted.click('Add input signal');
      expect(mounted.control('Signal 2 Signal name')).toBeDefined();
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
  });

  it('keeps an invalid inline row when Add is available and its append is rejected', async () => {
    const mounted = await mount([existing]);
    try {
      await mounted.type('part_present Signal name', 'invalid name');
      await mounted.blur('part_present Signal name');
      expect(mounted.button('Add input signal').disabled).toBe(false);
      expect(mounted.control('part_present Signal name').value).toBe('invalid name');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();

      await mounted.click('Add input signal');
      await mounted.click('Add signal');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      expect(mounted.control('part_present Signal name').value).toBe('invalid name');
      expect(hasAlert(mounted.all, '1–128 letters')).toBe(true);
    } finally { await mounted.unmount(); }
  });

  it('declares a change-only ROS Bool: no sample age, a visible liveness warning', async () => {
    const mounted = await mount([existing]);
    try {
      await mounted.type('part_present Source profile', 'ros_bool');
      await mounted.type('part_present Topic', '/cell/door');
      await mounted.blur('part_present Topic');
      await mounted.type('part_present Liveness', 'change_only');
      expect(mounted.all((props) => props['data-io-liveness-warning'] !== undefined)).toHaveLength(1);
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'set',
        [{ ...existing, update_mode: 'change_only', freshness: { mode: 'provider_validity' },
          provider: { profile: 'ros_bool', topic: '/cell/door', liveness: { mode: 'change_only' } } }],
      );
    } finally { await mounted.unmount(); }
  });

  it('declares a ros2_control GPIO input (periodic, no liveness selector) with the driver note; offers no UR profile', async () => {
    const mounted = await mount([existing]);
    try {
      const profiles = (mounted.control('part_present Source profile').children as Array<{ props: { value: string } }>)
        .map((option) => option.props.value);
      expect(profiles).not.toContain('ur_io_states');
      await mounted.type('part_present Source profile', 'ros2_control_gpio');
      expect(() => mounted.control('part_present Liveness')).toThrow();
      expect(() => mounted.control('part_present Pin')).toThrow();
      expect(mounted.all((props) => props['data-io-driver-note'] !== undefined)).toHaveLength(1);
      await mounted.type('part_present Topic', '/gpio_controller/gpio_states');
      await mounted.blur('part_present Topic');
      await mounted.type('part_present Group', 'flange');
      await mounted.blur('part_present Group');
      await mounted.type('part_present Interface name', 'di3');
      await mounted.blur('part_present Interface name');
      await mounted.type('part_present Maximum sample age', '50');
      await mounted.blur('part_present Maximum sample age');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'set',
        [{ ...existing, update_mode: 'sampled', freshness: { mode: 'sample_age', maximum_age_ns: 50_000_000 },
          provider: { profile: 'ros2_control_gpio', topic: '/gpio_controller/gpio_states', group: 'flange', interface_name: 'di3' } }],
      );
    } finally { await mounted.unmount(); }
  });

  it('declares a dynamic geometric contact input for a selected robot', async () => {
    const robots: GeometricContactRobotOption[] = [{ componentId: 'arm' }];
    const mounted = await mount([existing], true, robots);
    try {
      await mounted.type('part_present Source profile', 'geometric_contact');
      expect(mounted.all((props) => props.value === 'arm')).toHaveLength(1);
      await mounted.type('part_present Robot', 'arm');
      await mounted.type('part_present Target Scene resource', 'workpiece');
      await mounted.blur('part_present Target Scene resource');
      await mounted.type('part_present Maximum sample age', '75');
      await mounted.blur('part_present Maximum sample age');
      const help = mounted.texts().join(' ');
      expect(help).toContain('This input is true while the selected robot');
      expect(help).toContain('contact bodies touch the selected Scene resource, checked against live geometry.');
      expect(help).toContain("If ManyForge can't make the check, the input is unavailable, never false.");
      expect(help).toContain('Contact bodies are the collision shapes on the robot, or on the mounted tool');
      expect(help).toContain('the tool center point currently selected for that robot');
      expect(help).toContain("a Scene resource is a named object in the cell's Scene.");
      expect(help).toContain('At every sample, ManyForge reads the current Active TCP and collision geometry.');
      expect(help).toContain('does not store those live values or the contact result');
      expect(help).toContain('A live unavailable sample uses');
      expect(help).toContain('io_source_unavailable');
      for (const reason of [
        'active_tcp_unavailable', 'active_tcp_unknown', 'tcp_tool_not_mounted',
        'active_tcp_contact_bodies_unavailable', 'runtime_motion_endpoint_body_unresolved',
        'runtime_motion_endpoint_invalid', 'runtime_motion_endpoint_body_invalid',
        'runtime_collision_body_alias_table_invalid', 'runtime_collision_body_alias_unresolved',
        'observed_state_missing_or_stale', 'collision_geometry_missing_or_stale',
        'collision_geometry_unresolved', 'collision_query_busy',
        'collision_query_inconclusive', 'contact_bodies_static',
      ]) expect(help).toContain(reason);
      expect(help).toContain('at least one contact body is static relative to');
      expect(help).toContain('so it does not move with the robot.');
      expect(help).toContain('older than Maximum sample age');
      expect(help).toContain('below normal sample-production time can make it stale');
      expect(help).toContain('manual and timed Mock writes do not apply');
      expect(mounted.controller.mutate).toHaveBeenCalledExactlyOnceWith(
        ['graph', 'runtime_units', 0, 'configuration', 'io_signals'], 'set',
        [{ ...existing, update_mode: 'sampled',
          freshness: { mode: 'sample_age', maximum_age_ns: 75_000_000 }, provider: {
          profile: 'geometric_contact',
          robot_component_id: 'arm',
          target_scene_resource_id: 'workpiece',
        } }],
      );
    } finally { await mounted.unmount(); }
  });

  it('adds no signal to an arm: ROS-connected I/O is its own I/O device', async () => {
    const mounted = await mount([{ ...existing, controllable_component_id: 'arm' }], false);
    try {
      expect(() => mounted.button('Add input signal')).toThrow();
      expect(() => mounted.button('Add output signal')).toThrow();
      expect(mounted.texts().some((text) => text.includes('is its own I/O device'))).toBe(true);
      // Its existing rows can still be removed.
      await mounted.click('Remove signal');
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
