import { describe, expect, it } from 'vitest';

import type { JsonValue } from '../types/api';
import {
  IO_DEVICE_DEFAULT_LIFECYCLE_TIMEOUT_S,
  declaredIoSources,
  ioDeviceComponentValue,
  inputLivenessMode,
  ioDeviceHasAcknowledgedOutput,
  ioDeviceHasGpioOutput,
  rowInputLiveness,
  ioProviderDeviceTimeout,
  ioProviderDeviceValue,
  outputAckTimeoutProblem,
  ioSignalRows,
  ioSignalRowsValue,
  ioSignalsConfiguration,
  ioSourceKey,
  msTextToNs,
  newIoSignalInput,
  ioInputSourceText,
  nsToMsText,
  lifecycleTimeoutProblem,
  newIoDeviceDraft,
  rosServiceNameProblem,
  validateIoDeviceDraft,
  validateIoSignalRows,
  withIoDirection,
  type IoDeviceDraft,
  type IoSignalInputDraft,
  type IoSignalOutputDraft,
  type IoSignalRow,
} from './ioSignalConfiguration';

const mockRow = {
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
  provider: { profile: 'mock' },
};

const rosRow = {
  ...mockRow,
  resource_role_id: 'door_closed',
  signal_name: 'door_closed',
  source_id: 'door_topic',
  update_mode: 'sampled',
  freshness: { mode: 'sample_age', maximum_age_ns: 200_000_000 },
  provider: { profile: 'ros_bool', topic: '/cell/door_closed', ingress_depth: 64, liveness: { mode: 'periodic' } },
};

/** An output with an input-only field: not an authored output shape, kept verbatim. */
const outputRow = {
  ...mockRow,
  resource_role_id: 'valve',
  signal_name: 'valve',
  direction: 'output',
  output_completion: 'source_acknowledged',
};

const setBoolRow = {
  controllable_component_id: 'station',
  command_target_id: 'io',
  resource_role_id: 'clamp',
  signal_name: 'clamp',
  source_id: 'clamp_service',
  value_type: 'bool',
  direction: 'output',
  provider: { profile: 'ros_set_bool', service: '/cell/clamp/set' },
};

function output(patch: Partial<IoSignalOutputDraft>): IoSignalRow {
  return {
    kind: 'output',
    draft: {
      componentId: 'station', targetId: 'io', resourceRoleId: 'clamp', signalName: 'clamp',
      sourceId: 'clamp_service', profile: 'ros_set_bool', service: '/cell/clamp/set', channel: '',
      topic: '', group: '', interfaceName: '', ...patch,
    },
  };
}

function input(patch: Partial<IoSignalInputDraft>): IoSignalRow {
  return {
    kind: 'input',
    draft: {
      ...newIoSignalInput('station'),
      targetId: 'io', resourceRoleId: 'sensor', signalName: 'sensor', sourceId: 'station_mock',
      ...patch,
    },
  };
}

describe('io_signals configuration', () => {
  it('validates a Mock echo output', () => {
    const valve = output({ profile: 'mock_output', channel: 'valve', sourceId: 'valve_source' });
    const echoed = input({ echoEnabled: true, echoOutputSignalName: 'clamp', echoDelayMs: '50' });
    expect(validateIoSignalRows([echoed])).toContain('must name a Mock output');
    expect(validateIoSignalRows([echoed, valve])).toBeNull();
    const values = ioSignalRowsValue([echoed, valve]);
    expect(values[0]).toMatchObject({ provider: { echo: { output_signal_name: 'clamp', delay_ms: 50 } } });
    expect(ioSignalRowsValue(ioSignalRows({ io_signals: values }))).toEqual(values);
  });

  it('round-trips authored rows exactly and keeps outputs verbatim', () => {
    const rows = ioSignalRows({ io_signals: [mockRow, rosRow, outputRow] as JsonValue[] });
    expect(rows.map((row) => row.kind)).toEqual(['input', 'input', 'preserved']);
    expect(rows[2]).toMatchObject({ kind: 'preserved', reason: expect.stringContaining('not a ROS SetBool, ros2_control GPIO or Mock output shape') });
    expect(validateIoSignalRows(rows)).toBeNull();
    expect(ioSignalRowsValue(rows)).toEqual([mockRow, rosRow, outputRow]);
  });

  it('round-trips a ROS SetBool output with no input-only field', () => {
    const rows = ioSignalRows({ io_signals: [mockRow, setBoolRow] as JsonValue[] });
    expect(rows[1]).toEqual(output({}));
    expect(validateIoSignalRows(rows)).toBeNull();
    expect(ioSignalRowsValue(rows)).toEqual([mockRow, setBoolRow]);
    for (const extra of [
      { freshness: { mode: 'provider_validity' } }, { update_mode: 'change_only' },
      { transition_capability: 'sampled_only' }, { output_completion: 'source_acknowledged' },
      { readback_signal: 'clamp_state' }, { provider: { ...setBoolRow.provider, ingress_depth: 64 } },
    ] as Array<Record<string, JsonValue>>) {
      expect(ioSignalRows({ io_signals: [{ ...setBoolRow, ...extra }] as JsonValue[] })[0].kind).toBe('preserved');
    }
  });

  it('switches direction keeping the shared identity', () => {
    const switched = withIoDirection(input({}) as Extract<IoSignalRow, { kind: 'input' }>, 'output');
    expect(switched).toEqual(output({ resourceRoleId: 'sensor', signalName: 'sensor', sourceId: 'station_mock', service: '' }));
    expect(validateIoSignalRows([switched])).toContain('enter the ROS service name');
    const back = withIoDirection(switched as Extract<IoSignalRow, { kind: 'output' }>, 'input');
    expect(back).toMatchObject({ kind: 'input', draft: { componentId: 'station', profile: 'mock', resourceRoleId: 'sensor' } });
  });

  it.each([
    ['', 'enter the ROS service name'],
    ['/a\0b', 'NUL'],
    [`/${'a'.repeat(4096)}`, '4096 bytes'],
    ['cell/clamp', 'absolute'],
    ['~/clamp', 'absolute'],
    ['/{node}/clamp', 'fully resolved'],
    ['/cell/~clamp', 'fully resolved'],
    ['/cell//clamp', 'repeated or trailing slash'],
    ['/cell/clamp/', 'repeated or trailing slash'],
    ['/', 'repeated or trailing slash'],
  ])('refuses the service name %j (%s)', (service, text) => {
    expect(validateIoSignalRows([output({ service })])).toContain(text);
  });

  it('accepts a service name at the bounds', () => {
    expect(rosServiceNameProblem(`/${'a'.repeat(4095)}`)).toBeNull();
    expect(rosServiceNameProblem('/clamp')).toBeNull();
    expect(validateIoSignalRows([output({ signalName: 'a b' })])).toContain('signal name');
    expect(validateIoSignalRows([output({}), output({ resourceRoleId: 'other', signalName: 'other', service: '/x' })]))
      .toContain('source profile');
    expect(validateIoSignalRows([input({ resourceRoleId: 'clamp' }), output({ signalName: 'c2' })]))
      .toContain('only one signal');
  });

  it('materializes Mock defaults without writing them, and writes edits exactly', () => {
    const [row] = ioSignalRows({ io_signals: [mockRow] as JsonValue[] });
    expect(row).toMatchObject({ kind: 'input', draft: {
      initialValue: false, ingressDepth: '64',
    } });
    const edited: IoSignalRow = row.kind === 'input'
      ? { kind: 'input', draft: { ...row.draft, initialValue: true, ingressDepth: '8' } }
      : row;
    expect(ioSignalRowsValue([edited])).toEqual([{
      ...mockRow,
      provider: { profile: 'mock', ingress_depth: 8, initial_value: true },
    }]);
  });

  it('keeps an explicitly authored default', () => {
    const explicit = { ...mockRow, provider: { profile: 'mock', initial_value: false } };
    expect(ioSignalRowsValue(ioSignalRows({ io_signals: [explicit] as JsonValue[] }))).toEqual([explicit]);
  });

  it('writes a ROS Bool input with its required liveness (periodic: sample age; change only: no age)', () => {
    const base = { ...mockRow, resource_role_id: 'sensor', signal_name: 'sensor', source_id: 'door' };
    expect(ioSignalRowsValue([input({ profile: 'ros_bool', topic: ' /door ', maximumAgeMs: '100', sourceId: 'door' })]))
      .toEqual([{ ...base, update_mode: 'sampled', freshness: { mode: 'sample_age', maximum_age_ns: 100_000_000 },
        provider: { profile: 'ros_bool', topic: '/door', liveness: { mode: 'periodic' } } }]);
    const changeOnly = { ...base, update_mode: 'change_only', freshness: { mode: 'provider_validity' },
      provider: { profile: 'ros_bool', topic: '/door', liveness: { mode: 'change_only' } } };
    expect(ioSignalRowsValue([input({ profile: 'ros_bool', topic: '/door', liveness: 'change_only', maximumAgeMs: '', sourceId: 'door' })]))
      .toEqual([changeOnly]);
    const rows = ioSignalRows({ io_signals: [changeOnly] as JsonValue[] });
    expect(rows[0]).toMatchObject({ kind: 'input', draft: { liveness: 'change_only', maximumAgeMs: '' } });
    expect(ioSignalRowsValue(rows)).toEqual([changeOnly]);
    // No liveness, an unknown mode, an extra field, or freshness disagreeing with the mode: kept verbatim.
    for (const provider of [
      { profile: 'ros_bool', topic: '/door' },
      { profile: 'ros_bool', topic: '/door', liveness: { mode: 'sampled' } },
      { profile: 'ros_bool', topic: '/door', liveness: { mode: 'periodic', min_rate_hz: 10 } },
    ]) {
      expect(ioSignalRows({ io_signals: [{ ...rosRow, provider }] as JsonValue[] })[0].kind).toBe('preserved');
    }
    expect(ioSignalRows({ io_signals: [{ ...changeOnly, freshness: rosRow.freshness, update_mode: 'sampled' }] as JsonValue[] })[0].kind)
      .toBe('preserved');
  });

  it('round-trips a geometric contact input through a robot component id', () => {
    const geometric = {
      ...mockRow,
      resource_role_id: 'part_contact',
      signal_name: 'part_contact',
      source_id: 'part_contact_geometry',
      update_mode: 'sampled',
      freshness: { mode: 'sample_age', maximum_age_ns: 75_000_000 },
      provider: {
        profile: 'geometric_contact',
        ingress_depth: 64,
        robot_component_id: 'arm',
        target_scene_resource_id: 'workpiece',
      },
    };
    const rows = ioSignalRows({ io_signals: [geometric] as JsonValue[] });
    expect(rows[0]).toMatchObject({
      kind: 'input',
      draft: {
        profile: 'geometric_contact',
        robotComponentId: 'arm',
        targetSceneResourceId: 'workpiece',
        maximumAgeMs: '75',
        ingressDepth: '64',
      },
    });
    expect(rows[0].kind === 'input' ? inputLivenessMode(rows[0].draft) : null).toBe('periodic');
    expect(validateIoSignalRows(rows)).toBeNull();
    expect(ioSignalRowsValue(rows)).toEqual([geometric]);

    const fixedEndpoint = { ...geometric, provider: {
      profile: 'geometric_contact',
      endpoint: {
        controllable_component_id: 'arm', command_target_id: 'tool', frame_role_id: 'tcp',
      },
      target_scene_resource_id: 'workpiece',
    } };
    const legacyRows = ioSignalRows({ io_signals: [fixedEndpoint] as JsonValue[] });
    expect(legacyRows[0].kind).toBe('preserved');
    expect(ioSignalRowsValue(legacyRows)).toEqual([fixedEndpoint]);

    const omittedDepth = { ...geometric, provider: {
      profile: 'geometric_contact', robot_component_id: geometric.provider.robot_component_id,
      target_scene_resource_id: 'workpiece',
    } };
    const omittedRows = ioSignalRows({ io_signals: [omittedDepth] as JsonValue[] });
    expect(omittedRows[0]).toMatchObject({ kind: 'input', draft: { ingressDepth: '64' } });
    expect(ioSignalRowsValue(omittedRows)).toEqual([omittedDepth]);
    const nondefaultDepth = { ...geometric, provider: { ...geometric.provider, ingress_depth: 32 } };
    expect(ioSignalRowsValue(ioSignalRows({ io_signals: [nondefaultDepth] as JsonValue[] })))
      .toEqual([nondefaultDepth]);

    for (const provider of [
      { ...geometric.provider, contact_body_role_ids: ['finger'] },
      {
        profile: 'geometric_contact',
        endpoint: { controllable_component_id: 'arm', command_target_id: 'tool', frame_role_id: 'tcp' },
        target_scene_resource_id: 'workpiece',
      },
    ]) {
      expect(ioSignalRows({ io_signals: [{ ...geometric, provider }] as JsonValue[] })[0].kind)
        .toBe('preserved');
    }
    for (const freshness of [
      { mode: 'provider_validity' },
      { mode: 'sample_age', maximum_age_ns: 0 },
    ]) {
      expect(ioSignalRows({ io_signals: [{ ...geometric, freshness }] as JsonValue[] })[0].kind)
        .toBe('preserved');
    }
  });

  it('round-trips ros2_control GPIO inputs (periodic, exact addresses) and preserves the deleted UR profiles', () => {
    const gpio = { ...rosRow, resource_role_id: 'grip', signal_name: 'grip', source_id: 'gpio',
      provider: { profile: 'ros2_control_gpio', topic: '/gpio_controller/gpio_states', group: 'flange vacuum', interface_name: ' state ' } };
    const rows = ioSignalRows({ io_signals: [gpio] as JsonValue[] });
    expect(rows.map((row) => row.kind)).toEqual(['input']);
    expect(validateIoSignalRows(rows)).toBeNull();
    expect(ioSignalRowsValue(rows)).toEqual([gpio]);
    expect(rows.map((row) => (row.kind === 'input' ? inputLivenessMode(row.draft) : null))).toEqual(['periodic']);
    // The UR profiles are gone: such rows are not authored, only kept verbatim.
    const ur = { ...rosRow, resource_role_id: 'di3', signal_name: 'di3', source_id: 'ur_io',
      provider: { profile: 'ur_io_states', topic: '/io_and_status_controller/io_states', pin: 17 } };
    const urOut = { ...setBoolRow, provider: { profile: 'ur_set_io', service: '/io_and_status_controller/set_io', pin: 0 } };
    expect(ioSignalRows({ io_signals: [ur, urOut] as JsonValue[] }).map((row) => row.kind))
      .toEqual(['preserved', 'preserved']);
    expect(rowInputLiveness(ur as Record<string, JsonValue>)).toBeNull();
  });

  it('round-trips ros2_control GPIO outputs', () => {
    const gpioOut = { ...setBoolRow, resource_role_id: 'lamp', signal_name: 'lamp', source_id: 'gpio_cmd',
      provider: { profile: 'ros2_control_gpio', topic: '/gpio_controller/commands', group: 'flange', interface_name: 'lamp' } };
    const rows = ioSignalRows({ io_signals: [gpioOut] as JsonValue[] });
    expect(rows.map((row) => row.kind)).toEqual(['output']);
    expect(validateIoSignalRows(rows)).toBeNull();
    expect(ioSignalRowsValue(rows)).toEqual([gpioOut]);
    expect(validateIoSignalRows([output({ profile: 'ros2_control_gpio', topic: 'gpio', group: 'g', interfaceName: 'i' })])).toContain('absolute');
    expect(validateIoSignalRows([output({ profile: 'ros2_control_gpio', topic: '/gpio', group: '', interfaceName: 'i' })])).toContain('GPIO group');
    expect(validateIoSignalRows([output({ profile: 'ros2_control_gpio', topic: '/gpio', group: 'g', interfaceName: 'a\0' })])).toContain('NUL');
  });

  it('reads the declared liveness of raw rows (contract 81)', () => {
    expect(rowInputLiveness(mockRow as Record<string, JsonValue>)).toBe('in_process');
    expect(rowInputLiveness(rosRow as Record<string, JsonValue>)).toBe('periodic');
    expect(rowInputLiveness({ ...rosRow, provider: { profile: 'ros_bool', topic: '/t', liveness: { mode: 'change_only' } } } as Record<string, JsonValue>))
      .toBe('change_only');
    expect(rowInputLiveness({ ...rosRow, provider: { profile: 'ros_bool', topic: '/t' } } as Record<string, JsonValue>)).toBeNull();
    expect(rowInputLiveness(setBoolRow as Record<string, JsonValue>)).toBeNull();
  });

  it('keeps an input row of an unknown shape verbatim', () => {
    const unknown = { ...rosRow, provider: { profile: 'future_input', topic: '/future/io', input_bit: 3 } };
    const rows = ioSignalRows({ io_signals: [unknown] as JsonValue[] });
    expect(rows[0]).toMatchObject({ kind: 'preserved', reason: expect.stringContaining('Not an executable boolean signal shape') });
    expect(ioSignalRowsValue(rows)).toEqual([unknown]);
  });

  it.each([
    [{ signalName: '' }, 'signal name'],
    [{ componentId: '-station' }, 'component'],
    [{ sourceId: 'a b' }, 'source'],
    [{ resourceRoleId: 'x'.repeat(129) }, 'resource role'],
    [{ ingressDepth: '0' }, 'ingress depth'],
    [{ ingressDepth: '9007199254740992' }, 'ingress depth'],
    [{ ingressDepth: '1.5' }, 'ingress depth'],
    [{ profile: 'ros_bool', topic: ' ', maximumAgeMs: '10' }, 'topic'],
    [{ profile: 'ros_bool', topic: '/t', maximumAgeMs: '0' }, 'maximum sample age'],
    [{ profile: 'ros_bool', topic: '/t', maximumAgeMs: '' }, 'maximum sample age'],
    [{ profile: 'ros2_control_gpio', topic: 'gpio_states', group: 'g', interfaceName: 'i', maximumAgeMs: '1' }, 'absolute'],
    [{ profile: 'ros2_control_gpio', topic: '/t', group: '', interfaceName: 'i', maximumAgeMs: '1' }, 'GPIO group'],
    [{ profile: 'ros2_control_gpio', topic: '/t', group: 'g', interfaceName: '', maximumAgeMs: '1' }, 'GPIO interface name'],
    [{ profile: 'ros2_control_gpio', topic: '/t', group: 'g', interfaceName: 'i', maximumAgeMs: '0' }, 'maximum sample age'],
  ] as Array<[Partial<IoSignalInputDraft>, string]>)('refuses %o (%s)', (patch, text) => {
    expect(validateIoSignalRows([input(patch)])).toContain(text);
  });

  it('accepts the bounds exactly', () => {
    expect(validateIoSignalRows([input({ ingressDepth: '1' })])).toBeNull();
    expect(validateIoSignalRows([input({ profile: 'ros_bool', topic: '/t', maximumAgeMs: '0.000001' })])).toBeNull();
    // A change-only ROS Bool has no sample age to validate.
    expect(validateIoSignalRows([input({ profile: 'ros_bool', topic: '/t', liveness: 'change_only', maximumAgeMs: '' })])).toBeNull();
    expect(validateIoSignalRows([input({ profile: 'ros2_control_gpio', topic: '/t', group: 'g', interfaceName: 'i', maximumAgeMs: '1' })])).toBeNull();
    // Rows sharing a source agree on its liveness too.
    expect(validateIoSignalRows([
      input({ profile: 'ros_bool', topic: '/a', maximumAgeMs: '1', sourceId: 'bus' }),
      input({ profile: 'ros_bool', topic: '/a', liveness: 'change_only', sourceId: 'bus', resourceRoleId: 'b', signalName: 'b' }),
    ])).toContain('liveness');
  });

  it('validates geometric freshness independently and excludes it from shared-source identity', () => {
    const geometric = {
      profile: 'geometric_contact' as const,
      robotComponentId: 'arm',
      targetSceneResourceId: 'workpiece', sourceId: 'contact_geometry',
    };
    expect(validateIoSignalRows([input({ ...geometric, maximumAgeMs: '' })]))
      .toContain('maximum sample age must be above 0 ms');
    expect(validateIoSignalRows([input({ ...geometric, robotComponentId: '', maximumAgeMs: '25' })]))
      .toContain('select a robot');
    expect(validateIoSignalRows([
      input({ ...geometric, maximumAgeMs: '25' }),
      input({ ...geometric, maximumAgeMs: '100', resourceRoleId: 'contact_b', signalName: 'contact_b' }),
    ])).toBeNull();
  });

  it('refuses duplicate keys, duplicate names and a disagreeing shared source', () => {
    expect(validateIoSignalRows([input({}), input({ signalName: 'other' })])).toContain('only one signal');
    expect(validateIoSignalRows([input({}), input({ resourceRoleId: 'b' })])).toContain('used twice');
    expect(validateIoSignalRows([
      input({ profile: 'ros_bool', topic: '/a', maximumAgeMs: '1', sourceId: 'bus' }),
      input({ profile: 'ros_bool', topic: '/b', maximumAgeMs: '1', sourceId: 'bus', resourceRoleId: 'b', signalName: 'b' }),
    ])).toContain('source profile');
    // Mock per-input settings stay independent on one shared source.
    expect(validateIoSignalRows([
      input({ initialValue: false }),
      input({ resourceRoleId: 'b', signalName: 'b', initialValue: true }),
    ])).toBeNull();
  });

  it('converts ns and ms exactly', () => {
    expect(nsToMsText(250_000_000)).toBe('250');
    expect(nsToMsText(1)).toBe('0.000001');
    expect(msTextToNs('1.5')).toBe(1_500_000);
    expect(msTextToNs('1e3')).toBeNull();
  });

  it('finds the nested intent runtime configuration and the declared sources', () => {
    const configuration = { runtime_kind: 'ros2_control', runtime_configuration: { io_signals: [mockRow] } } as Record<string, JsonValue>;
    expect(ioSignalsConfiguration(configuration, ['components', 0, 'configuration']).path)
      .toEqual(['components', 0, 'configuration', 'runtime_configuration', 'io_signals']);
    expect(ioSignalsConfiguration({ mode: 'simulated' }, ['graph', 'runtime_units', 1, 'configuration']).path)
      .toEqual(['graph', 'runtime_units', 1, 'configuration', 'io_signals']);
    const sources = declaredIoSources({ components: [{ configuration }] } as Record<string, JsonValue>);
    expect(sources.get(ioSourceKey('station', 'io', 'part_present'))).toEqual({
      profile: 'mock', sourceId: 'station_mock', topic: null,
      liveness: 'in_process',
    });
  });

  it('round-trips a Mock output (profile + channel) exactly', () => {
    const mockOutput = { ...setBoolRow, resource_role_id: 'lamp', signal_name: 'lamp', source_id: 'lamp_mock',
      provider: { profile: 'mock_output', channel: 'lamp' } };
    const rows = ioSignalRows({ io_signals: [mockOutput] as JsonValue[] });
    expect(rows[0]).toMatchObject({ kind: 'output', draft: { profile: 'mock_output', channel: 'lamp', service: '' } });
    expect(validateIoSignalRows(rows)).toBeNull();
    expect(ioSignalRowsValue(rows)).toEqual([mockOutput]);
    expect(validateIoSignalRows([output({ profile: 'mock_output', channel: 'a b' })])).toContain('Mock output channel');
    expect(ioSignalRows({ io_signals: [{ ...mockOutput, provider: { profile: 'mock_output', channel: 'x', service: '/x' } }] as JsonValue[] })[0].kind)
      .toBe('preserved');
  });
});

describe('I/O device component', () => {
  const draft = (patch: Partial<IoDeviceDraft> = {}): IoDeviceDraft => {
    const base = newIoDeviceDraft([]);
    const next = { ...base, ...patch };
    return { ...next, signal: { ...next.signal, draft: { ...next.signal.draft, componentId: next.componentId } } as IoDeviceDraft['signal'] };
  };
  const named = (patch: Partial<IoDeviceDraft> = {}) => {
    const value = draft(patch);
    return { ...value, signal: { ...value.signal, draft: { ...value.signal.draft, signalName: 'part_present' } } as IoDeviceDraft['signal'] };
  };

  it('prefills a free id, the 10 s default budget and a Mock input on target io', () => {
    const value = newIoDeviceDraft(['arm', 'io_device_1']);
    expect(value.componentId).toBe('io_device_2');
    expect(value.lifecycleTimeoutS).toBe('10');
    expect(IO_DEVICE_DEFAULT_LIFECYCLE_TIMEOUT_S).toBe(10);
    expect(value.signal).toMatchObject({ kind: 'input', draft: { componentId: 'io_device_2', targetId: 'io', profile: 'mock' } });
  });

  it('creates the exact model-free component row', () => {
    const value = named({ componentId: 'station_io' });
    expect(validateIoDeviceDraft(value, ['arm'])).toBeNull();
    const row = ioDeviceComponentValue(value);
    expect(row).toEqual({
      component_id: 'station_io',
      component_profile_id: 'io_device_v0',
      target_settings: {},
      configuration: {
        runtime_kind: 'io_provider',
        runtime_configuration: {
          io_provider_devices: [{ controllable_component_id: 'station_io', lifecycle_command_timeout_s: 10 }],
          io_signals: [{ ...mockRow, controllable_component_id: 'station_io', source_id: 'station_io_source' }],
        },
      },
    });
    for (const forbidden of ['model_source', 'root_pose_in_cell', 'frame_prefix', 'default_fault_stop_profile']) {
      expect(row).not.toHaveProperty(forbidden);
    }
  });

  it.each([
    ['30', null], ['0.5', null], ['3600', null], ['0', 'above 0'], ['-1', 'above 0'], ['3600.5', 'at most 3600'],
    ['', 'above 0'], ['1e3', 'above 0'], ['abc', 'above 0'],
  ])('bounds the lifecycle time limit %j', (text, problem) => {
    const result = lifecycleTimeoutProblem(text);
    if (problem === null) expect(result).toBeNull();
    else expect(result).toContain(problem);
    expect(validateIoDeviceDraft(named({ lifecycleTimeoutS: text }), []) === null).toBe(problem === null);
  });

  it('refuses a bad or duplicate component id and an invalid first signal', () => {
    expect(validateIoDeviceDraft(named({ componentId: 'Station' }), [])).toContain('component id');
    expect(validateIoDeviceDraft(named({ componentId: 'arm' }), ['arm'])).toContain('already exists');
    expect(validateIoDeviceDraft(draft(), [])).toContain('signal name');
  });

  it('requires at least one own signal on an I/O device', () => {
    expect(validateIoSignalRows([], 'station')).toContain('at least one signal');
    expect(validateIoSignalRows([input({})], 'station')).toBeNull();
    expect(validateIoSignalRows([input({ componentId: 'arm' })], 'station')).toContain('must belong to station');
    // Contract 84: ros2_control GPIO signals live on the device.
    expect(validateIoSignalRows([input({ profile: 'ros2_control_gpio', topic: '/gpio/states', group: 'g', interfaceName: 'i', maximumAgeMs: '50' })], 'station')).toBeNull();
  });

  it('creates a device whose first signal is an output', () => {
    const base = draft({ componentId: 'lamp_io' });
    const value: IoDeviceDraft = { ...base, signal: output({ componentId: 'lamp_io', signalName: 'lamp', resourceRoleId: '', sourceId: '', profile: 'mock_output', channel: 'lamp' }) as IoDeviceDraft['signal'] };
    expect(validateIoDeviceDraft(value, [])).toBeNull();
    expect((ioDeviceComponentValue(value).configuration as Record<string, Record<string, JsonValue>>).runtime_configuration.io_signals)
      .toEqual([{ ...setBoolRow, controllable_component_id: 'lamp_io', resource_role_id: 'lamp', signal_name: 'lamp', source_id: 'lamp_io_source', provider: { profile: 'mock_output', channel: 'lamp' } }]);
  });

  it('reads the authored or materialized device time limit', () => {
    expect(ioProviderDeviceTimeout({ io_provider_devices: [{ controllable_component_id: 'd', lifecycle_command_timeout_s: 12 }] }, 'd'))
      .toEqual({ timeoutS: 12, authored: true, exact: true, outputAckTimeoutS: null });
    expect(ioProviderDeviceTimeout({ io_provider_devices: [{ controllable_component_id: 'd' }] }, 'd'))
      .toEqual({ timeoutS: 10, authored: false, exact: true, outputAckTimeoutS: null });
    expect(ioProviderDeviceTimeout({}, 'd')).toEqual({ timeoutS: 10, authored: false, exact: false, outputAckTimeoutS: null });
    expect(ioProviderDeviceTimeout({ io_provider_devices: [{ controllable_component_id: 'other', lifecycle_command_timeout_s: 5 }] }, 'd'))
      .toMatchObject({ timeoutS: 10, exact: false });
  });

  it('reads the output acknowledgement timeout and detects ROS SetBool outputs (contract 68)', () => {
    expect(ioProviderDeviceTimeout({ io_provider_devices: [{
      controllable_component_id: 'd', lifecycle_command_timeout_s: 12, output_source_ack_timeout_s: 0.8,
    }] }, 'd')).toEqual({ timeoutS: 12, authored: true, exact: true, outputAckTimeoutS: 0.8 });
    expect(ioProviderDeviceValue('d', 30)).toEqual({ controllable_component_id: 'd', lifecycle_command_timeout_s: 30 });
    expect(ioProviderDeviceValue('d', 30, 0.5)).toEqual({
      controllable_component_id: 'd', lifecycle_command_timeout_s: 30, output_source_ack_timeout_s: 0.5,
    });
    const output = (profile: string) => ({ io_signals: [{ direction: 'output', provider: { profile } }] });
    expect(ioDeviceHasAcknowledgedOutput(output('ros_set_bool'))).toBe(true);
    expect(ioDeviceHasAcknowledgedOutput(output('ur_set_io'))).toBe(false);
    expect(ioDeviceHasAcknowledgedOutput(output('mock_output'))).toBe(false);
    // ros2_control GPIO outputs are submitted-only: no acknowledgement budget.
    expect(ioDeviceHasAcknowledgedOutput(output('ros2_control_gpio'))).toBe(false);
    expect(ioDeviceHasGpioOutput(output('ros2_control_gpio'))).toBe(true);
    expect(ioDeviceHasAcknowledgedOutput({ io_signals: [{ direction: 'input', provider: { profile: 'ros_set_bool' } }] })).toBe(false);
    expect(ioDeviceHasAcknowledgedOutput(null)).toBe(false);
    expect(outputAckTimeoutProblem('0.5')).toBeNull();
    expect(outputAckTimeoutProblem('0')).not.toBeNull();
    expect(outputAckTimeoutProblem('-1')).not.toBeNull();
    expect(outputAckTimeoutProblem('abc')).not.toBeNull();
  });
});

describe('ioInputSourceText', () => {
  it('says where a declared input comes from', () => {
    const input = newIoSignalInput('cell_io');
    expect(ioInputSourceText({ ...input, profile: 'ros_bool', topic: '/cell/door' })).toBe('ROS topic /cell/door');
    expect(ioInputSourceText({ ...input, profile: 'geometric_contact', robotComponentId: 'lite6',
      targetSceneResourceId: 'workpiece' })).toBe('geometric contact for lite6 with workpiece');
    expect(ioInputSourceText({ ...input, profile: 'ros2_control_gpio', topic: '/gpio', group: 'g', interfaceName: 'i' }))
      .toBe('ROS topic /gpio, GPIO g/i');
    expect(ioInputSourceText(input)).toBe('Mock input');
  });
});
