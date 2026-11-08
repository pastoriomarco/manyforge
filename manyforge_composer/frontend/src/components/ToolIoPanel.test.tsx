import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { IoSignalReading } from '../api/parsers/ioSignals';
import {
  ioSignalBindingsAt,
  ioSignalBindingsFromProgramBinding,
  parseIoSignalResponse,
  parseMockIoInputReceipt,
  type MockIoInputReceipt,
  type SystemSignalProjection,
} from '../api/parsers/ioSignals';
import type { ProgramBindingDocument } from '../types/api';
import type { CellState, ComponentState, SignalSample } from '../types/components';
import { reactPropsHost } from '../test/reactPropsHost';
import type { HeadlessStateSource } from '../hooks/useHeadlessState';
import {
  ToolIoPanel,
  ToolIoPanelView,
  motionExecuting,
  type OutputWriteState,
  type ToolIoPanelViewProps,
  type ToolIoSignal,
  toolIoSignalKey,
} from './ToolIoPanel';

const api = vi.hoisted(() => ({
  readIoSignal: vi.fn(),
  setMockIoInput: vi.fn(),
  setOutput: vi.fn(),
  getWorkspace: vi.fn(),
}));

vi.mock('../api/client', () => ({
  api: {
    execution: { readIoSignal: api.readIoSignal, setMockIoInput: api.setMockIoInput },
    io: { setOutput: api.setOutput },
    deploymentWorkspace: { get: api.getWorkspace },
  },
}));

const BINDING_ID = `sha256:${'a'.repeat(64)}`;
const REVISION = `sha256:${'d'.repeat(64)}`;

function reading(patch: Partial<IoSignalReading> = {}): IoSignalReading {
  return {
    liveness: { mode: 'in_process', warning: null },
    signalName: 'part_present', value: true, quality: 'fresh', sourceId: 'station_mock',
    sourceEpoch: '3', sourceSequence: '9', receiptAgeNs: 12_000_000,
    continuityLossGeneration: '0', errorCode: null, ...patch,
  };
}

function signal(patch: Partial<ToolIoSignal> = {}): ToolIoSignal {
  return {
    programBindingId: BINDING_ID, controllableComponentId: 'station', targetId: 'io',
    signalComponentId: 'station', signalName: 'part_present', direction: 'input',
    declared: { profile: 'mock', sourceId: 'station_mock', topic: null, liveness: 'in_process' },
    ...patch,
  };
}

const valve = signal({ signalName: 'valve', direction: 'output',
  declared: { profile: 'ros_bool', sourceId: 'valve_src', topic: '/valve', liveness: 'periodic' } });

function sample(signalId: string, patch: Partial<SignalSample> = {}): SignalSample {
  return { signal_id: signalId, value: true, source: 'device', age_ns: 12_000_000, fresh: true, ...patch };
}

function receipt(patch: Partial<MockIoInputReceipt> = {}): MockIoInputReceipt {
  return {
    reading: reading({ value: true }), modelValue: true,
    publishedCursor: { ownerEpoch: 'owner.1', catalogGeneration: '1', sourceId: 'station_mock', recordSequence: '10' },
    publicationError: null, ...patch,
  };
}

function view(patch: Partial<ToolIoPanelViewProps> = {}) {
  return renderToStaticMarkup(
    <ToolIoPanelView unavailableReason={null} signals={[]} samples={new Map()} latched={false}
      writes={{}} mocks={{}} sourcesNotice={null}
      onSetOutput={() => undefined} onSetMock={() => undefined} {...patch} />,
  );
}

const samplesOf = (...rows: SignalSample[]) => new Map(rows.map((row) => [row.signal_id, row]));

afterEach(() => {
  vi.unstubAllGlobals();
  vi.clearAllMocks();
});

describe('ToolIoPanelView', () => {
  it('shows generated system signals with their component as read-only rows', () => {
    const systemSignal: SystemSignalProjection = {
      controllableComponentId: 'robot', commandTargetId: 'gripper',
      signalName: 'carried_object_lost.gripper', sourceKind: 'system_carried_object',
      direction: 'input', valueType: 'bool', level: true,
    };
    const html = view({ signals: [valve], systemSignals: [systemSignal] });
    expect(html).toContain('aria-label="robot system signals"');
    expect(html).toContain('<strong>robot</strong><small>System signals · read only</small>');
    expect(html).not.toContain('aria-label="System signals"');
    expect(html.match(/class="runtime-control-section"/g)).toHaveLength(2);
    expect(html).toContain('data-system-signal="carried_object_lost.gripper"');
    expect(html).toContain('data-system-signal-level="true">true</strong>');
    expect(html).toContain('gripper · input · system');
    const systemSectionStart = html.indexOf('aria-label="robot system signals"');
    const systemSectionEnd = html.indexOf('</section>', systemSectionStart);
    expect(html.slice(systemSectionStart, systemSectionEnd)).not.toContain('<button');
  });

  it('hides bound and system rows when the panel is unavailable', () => {
    const html = view({ unavailableReason: 'Tool and I/O signals require the configured headless runtime.',
      signals: [signal()] });
    expect(html).toContain('Tool and I/O signals require the configured headless runtime.');
    expect(html).not.toContain('data-io-signal=');
    expect(html).not.toContain('<button');
  });

  it('shows each pushed sample with its value, source and age', () => {
    const html = view({
      signals: [signal(), valve],
      samples: samplesOf(sample('station/part_present', { value: false, source: 'device', age_ns: 12_000_000 }),
        sample('station/valve', { source: 'outside', age_ns: 2_500_000_000 })),
    });
    expect(html).toContain('data-io-signal="part_present" data-io-fresh="true"');
    expect(html).toContain('data-io-value="true">off');
    expect(html).toContain('data-io-source="device">device');
    expect(html).toContain('data-io-age="true">12 ms');
    expect(html).toContain('data-io-source="outside"><span class="status-pill status-pill-danger">outside');
    expect(html).toContain('data-io-age="true">2.5 s');
    expect(html).toContain('Declared source: ROS Bool topic · valve_src · /valve');
  });

  it('marks a stale sample and a missing sample', () => {
    const stale = view({ signals: [valve], samples: samplesOf(sample('station/valve', { fresh: false, source: 'device' })) });
    expect(stale).toContain('on (stale)');
    expect(stale).toContain('device');
    expect(view({ signals: [valve]})).toContain('no sample');
    expect(view({ signals: [valve], samples: null })).toContain('unavailable');
  });

  it('shows an output without read-back as not readable, never as its last write', () => {
    const html = view({ signals: [valve], samples: samplesOf(sample('station/valve', { value: null, fresh: false, source: 'not_readable', age_ns: 0 })) });
    expect(html).toContain('data-io-value="true">not readable');
    expect(html).toContain('data-io-source="not_readable">no read-back');
    expect(html).not.toContain('(stale)');
  });

  it('nests the changed outside mark under its output', () => {
    const changed = view({ signals: [valve], samples: samplesOf(sample('station/valve'),
      sample('station/valve.changed_outside', { value: true, source: 'device' })) });
    expect(changed).toContain('data-io-changed-outside="true"');
    expect(changed).toContain('→ changed outside');
    expect(changed).toContain('changed outside ManyForge');
    const clear = view({ signals: [valve], samples: samplesOf(sample('station/valve'),
      sample('station/valve.changed_outside', { value: false })) });
    expect(clear).toContain('data-io-changed-outside="false"');
    expect(clear).toContain('no outside change');
    expect(view({ signals: [valve], samples: samplesOf(sample('station/valve')) })).not.toContain('changed outside');
  });

  it('offers output writes, disabled during a latch with the reason', () => {
    const open = view({ signals: [valve], samples: samplesOf(sample('station/valve')) });
    expect(open).toContain('aria-label="Write valve on"');
    expect(open).toContain('aria-label="Write valve off"');
    expect(open).not.toContain('disabled=""');
    const latched = view({ signals: [valve], samples: samplesOf(sample('station/valve')), latched: true });
    expect(latched).toMatch(/disabled="" title="Cell latched: a component needs attention" aria-label="Write valve on"/);
    expect(latched).toContain('Outputs: Cell latched: a component needs attention');
  });

  it('disables output writes only while motion executes', () => {
    const executing = view({ signals: [valve], samples: samplesOf(sample('station/valve')), motionExecuting: true });
    expect(executing).toMatch(/disabled="" title="Motion is executing[^"]*" aria-label="Write valve on"/);
    expect(executing).toContain('Outputs: Motion is executing');
    const arms = new Set(['robot', 'gripper']);
    const state = (component_id: string, activity: 'busy' | 'idle') =>
      ({ component_id, activity }) as ComponentState;
    expect(motionExecuting([state('robot', 'idle'), state('gripper', 'busy')], arms)).toBe(true);
    expect(motionExecuting([state('robot', 'idle')], arms)).toBe(false);
    // An I/O component's own write, a sensor or an owner being busy is no motion.
    expect(motionExecuting([state('station_io', 'busy'), state('lite6', 'busy')], arms)).toBe(false);
    expect(motionExecuting(null, arms)).toBe(false);
  });

  it('shows a controller-owned output read-only', () => {
    const html = view({ signals: [valve], samples: samplesOf(sample('station/valve', { controller_owned: true })) });
    expect(html).toContain('data-io-controller-owned');
    expect(html).not.toContain('Write valve');
  });

  it('shows the write outcome: accepted, unknown, failed or refused', () => {
    const key = toolIoSignalKey(valve);
    const outcome = (write: OutputWriteState) => view({ signals: [valve], writes: { [key]: write } });
    expect(outcome({ value: true, sending: false, result: { outcome: 'succeeded' }, error: null }))
      .toContain('Written: on.');
    expect(outcome({ value: false, sending: false, result: { outcome: 'unknown' }, error: null }))
      .toContain('Write off: no response, the outcome is unknown. It is not resent.');
    expect(outcome({ value: true, sending: false, error: null,
      result: { outcome: 'failed', error: { code: 'not_a_catalog_code', message: 'refused by the device' } } }))
      .toContain('Write on failed: refused by the device');
    expect(outcome({ value: true, sending: false, result: null, error: 'Cell latched' }))
      .toContain('data-io-write="refused">Cell latched');
    expect(outcome({ value: true, sending: true, result: null, error: null }))
      .toMatch(/disabled="" title="A write is in flight."/);
  });

  it('offers the Mock toggle for Mock inputs only, with its receipt', () => {
    const real = signal({ signalName: 'door',
      declared: { profile: 'ros_bool', sourceId: 'door_src', topic: '/door', liveness: 'periodic' } });
    const html = view({
      signals: [signal(), real],
      samples: samplesOf(sample('station/part_present', { value: true })),
      mocks: { [toolIoSignalKey(signal())]: { sending: false, error: null,
        receipt: receipt({ publishedCursor: null }) } },
    });
    expect(html).toContain('aria-label="Set part_present false"');
    expect(html).not.toContain('aria-label="Set door');
    expect(html).toContain('data-io-receipt="not_delivered"');
  });
});

function cellSource(state: CellState) {
  return {
    subscribeGlobalRules: () => () => undefined,
    getGlobalRules: () => null,
    getGlobalRulesCurrent: () => true,
    subscribeCellState: () => () => undefined,
    getCellState: () => state,
  } as unknown as HeadlessStateSource;
}

describe('ToolIoPanel', () => {
  const stationTarget = { component_id: 'station', command_target_id: 'io' };
  const binding = {
    api_version: 'manyforge.program_binding.v3',
    program_binding_id: BINDING_ID,
    retained_bundle_id: 'bundle.1',
    target_bindings: [{
      target: stationTarget,
      signal_references: [
        { component_id: 'station', signal_name: 'part_present' },
        { component_id: 'station', signal_name: 'valve' },
      ],
    }],
    signal_bindings: [
      { reference: { component_id: 'station', signal_name: 'part_present' }, target: stationTarget,
        runtime_unit_id: 'station', value_type: 'bool', direction: 'input', transition_capability: 'sampled_only',
        signal_descriptor_digest: REVISION },
      { reference: { component_id: 'station', signal_name: 'valve' }, target: stationTarget,
        runtime_unit_id: 'station', value_type: 'bool', direction: 'output', transition_capability: 'sampled_only',
        signal_descriptor_digest: REVISION },
    ],
  } as unknown as ProgramBindingDocument;
  const row = (resource: string, direction: string, provider: Record<string, unknown>, extra: Record<string, unknown>) => ({
    controllable_component_id: 'station', command_target_id: 'io', resource_role_id: resource,
    signal_name: resource, source_id: `${resource}_source`, value_type: 'bool', direction,
    transition_capability: 'sampled_only', provider, ...extra,
  });
  const cell: CellState = {
    components: null, readiness: null, latch: { latched: false, causes: [] },
    io: [sample('station/part_present', { value: false }), sample('station/valve', { value: false })],
  };

  async function mount(state: CellState) {
    api.getWorkspace.mockResolvedValue({
      draft: {
        deploymentRevision: REVISION,
        document: { graph: { runtime_units: [{ configuration: { io_signals: [
          row('part_present', 'input', { profile: 'mock' }, { update_mode: 'change_only', freshness: { mode: 'provider_validity' } }),
          row('valve', 'output', { profile: 'ros_bool', topic: '/valve' }, {}),
        ] } }] } },
      },
      managerApplied: { deploymentRevision: REVISION },
      binding,
    });
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(async () => {
      root.render(<ToolIoPanel enabled ownerEpoch="owner.1" retainedBundleId="bundle.1"
        controlSource={cellSource(state)} />);
    });
    await act(async () => { await Promise.resolve(); });
    return { root, find };
  }

  it('reads nothing on its own and writes an output once', async () => {
    api.setOutput.mockResolvedValue({ outcome: 'unknown' });
    const { root, find } = await mount(cell);
    try {
      expect(api.readIoSignal).not.toHaveBeenCalled();
      const write = find((props) => props['aria-label'] === 'Write valve on');
      expect(write.disabled).toBe(false);
      await act(async () => { write.onClick?.(); });
      expect(api.setOutput).toHaveBeenCalledExactlyOnceWith('station/valve', true);
      expect(find((props) => props['data-io-write'] === 'unknown')).toBeTruthy();
    } finally {
      await act(() => root.unmount());
    }
  });

  it('disables writes while the cell is latched', async () => {
    const { root, find } = await mount({ ...cell, latch: { latched: true, causes: [] } });
    try {
      expect(find((props) => props['aria-label'] === 'Write valve on').disabled).toBe(true);
    } finally {
      await act(() => root.unmount());
    }
  });

  it('sets a Mock input with the source epoch it reads just before', async () => {
    api.readIoSignal.mockResolvedValue(reading({ value: false }));
    api.setMockIoInput.mockResolvedValue(receipt());
    const { root, find } = await mount(cell);
    try {
      await act(async () => { find((props) => props['aria-label'] === 'Set part_present true').onClick?.(); });
      expect(api.setMockIoInput).toHaveBeenCalledExactlyOnceWith({
        ownerEpoch: 'owner.1', programBindingId: BINDING_ID, componentId: 'station', signalName: 'part_present',
      }, '3', true);
      expect(find((props) => props['data-io-receipt'] === 'delivered')).toBeTruthy();
    } finally {
      await act(() => root.unmount());
    }
  });

  it('shows why nothing is shown before the owner or a bound signal exists', () => {
    expect(renderToStaticMarkup(<ToolIoPanel enabled ownerEpoch={null} retainedBundleId="bundle.1" />))
      .toContain('Waiting for the execution owner');
    expect(renderToStaticMarkup(<ToolIoPanel enabled ownerEpoch="owner.1" retainedBundleId="bundle.1" />))
      .toContain('No applied Program binding');
    expect(ioSignalBindingsFromProgramBinding(binding)).toHaveLength(1);
  });
});

describe('parseIoSignalResponse', () => {
  const selector = { ownerEpoch: 'owner.1', programBindingId: BINDING_ID, componentId: 'station', signalName: 'part_present' };
  const response = (value: Record<string, unknown>, extra: Record<string, unknown> = {}) => ({
    api_version: 'manyforge.execution_command_response.v4', ok: true, command: 'query_target',
    target_observation: {
      schema_version: 'manyforge.target_observation.v2', owner_epoch: 'owner.1', program_binding_id: BINDING_ID,
      signal: { component_id: 'station', signal_name: 'part_present' }, kind: 'io_signal',
      evaluated_at_ns: 5000, value, ...extra,
    },
  });
  const value = {
    liveness: { mode: 'periodic', warning: null },
    signal_name: 'part_present', value_type: 'bool', value: false, quality: 'stale', source_id: 's',
    source_epoch: '2', source_sequence: '5', receipt_age_ns: '9000000000', continuity_loss_generation: '0', error: null,
  };

  it('keeps the last reported value with its quality', () => {
    expect(parseIoSignalResponse(response(value), 'query_target', selector)).toMatchObject({
      value: false, quality: 'stale', receiptAgeNs: 9_000_000_000, sourceEpoch: '2',
      liveness: { mode: 'periodic', warning: null },
    });
  });

  it('keeps the host opaque sha256 source epoch; the sequence stays a counter', () => {
    const epoch = `sha256:${'5c'.repeat(32)}`;
    expect(parseIoSignalResponse(response({ ...value, source_epoch: epoch }), 'query_target', selector).sourceEpoch)
      .toBe(epoch);
    for (const bad of ['', 's'.repeat(257), 'e\u0301']) {
      expect(() => parseIoSignalResponse(response({ ...value, source_epoch: bad }), 'query_target', selector))
        .toThrow();
    }
    expect(parseIoSignalResponse(response({ ...value, source_epoch: 's'.repeat(256) }), 'query_target', selector)
      .sourceEpoch).toHaveLength(256);
    expect(() => parseIoSignalResponse(response({ ...value, source_sequence: epoch }), 'query_target', selector))
      .toThrow();
  });

  it('parses the descriptor liveness projection', () => {
    expect(parseIoSignalResponse(response({ ...value, liveness: { mode: 'change_only', warning: 'liveness_unproven' } }),
      'query_target', selector).liveness).toEqual({ mode: 'change_only', warning: 'liveness_unproven' });
    expect(parseIoSignalResponse(response({ ...value, liveness: null }), 'query_target', selector).liveness).toBeNull();
  });

  it.each([
    ['another selector', response(value, { signal: { component_id: 'station', signal_name: 'other' } })],
    ['another component', response(value, { signal: { component_id: 'other', signal_name: 'part_present' } })],
    ['an unknown quality', response({ ...value, quality: 'ok' })],
    ['a fresh reading without a value', response({ ...value, quality: 'fresh', value: null })],
    ['an extra field', response(value, { source_epoch: '2' })],
    ['no evaluation time', response(value, { evaluated_at_ns: undefined })],
    ['a negative evaluation time', response(value, { evaluated_at_ns: -1 })],
    ['no liveness', response({ ...value, liveness: undefined })],
    ['a change-only mode without its warning', response({ ...value, liveness: { mode: 'change_only', warning: null } })],
    ['a warning on a periodic mode', response({ ...value, liveness: { mode: 'periodic', warning: 'liveness_unproven' } })],
    ['an unknown mode', response({ ...value, liveness: { mode: 'sampled', warning: null } })],
  ])('refuses %s', (_name, body) => {
    expect(() => parseIoSignalResponse(body, 'query_target', selector)).toThrow();
  });
});

describe('parseMockIoInputReceipt', () => {
  const selector = { ownerEpoch: 'owner.1', programBindingId: BINDING_ID, componentId: 'station', signalName: 'part_present' };
  const unavailable = {
    liveness: { mode: 'in_process', warning: null },
    signal_name: 'part_present', value_type: 'bool', value: null, quality: 'unavailable', source_id: 'station_mock',
    source_epoch: null, source_sequence: null, receipt_age_ns: null, continuity_loss_generation: '0',
    error: { code: 'io_source_unavailable', message: 'disabled' },
  };
  const fresh = { ...unavailable, value: true, quality: 'fresh', source_epoch: '3', source_sequence: '10',
    receipt_age_ns: 0, error: null };
  const cursor = { owner_epoch: 'owner.1', catalog_generation: '1', source_id: 'station_mock', record_sequence: '10' };
  const body = (value: Record<string, unknown>, extra: Record<string, unknown> = {}) => ({
    api_version: 'manyforge.execution_command_response.v4', ok: true, command: 'set_mock_io_input',
    target_observation: {
      schema_version: 'manyforge.target_observation.v2', owner_epoch: 'owner.1', program_binding_id: BINDING_ID,
      signal: { component_id: 'station', signal_name: 'part_present' }, kind: 'io_signal', evaluated_at_ns: 5000, value,
    },
    model_value: true, published_cursor: cursor, ...extra,
  });

  it('parses a delivered toggle with its exact cursor', () => {
    expect(parseMockIoInputReceipt(body(fresh), selector, true)).toMatchObject({
      modelValue: true, publicationError: null,
      publishedCursor: { ownerEpoch: 'owner.1', catalogGeneration: '1', sourceId: 'station_mock', recordSequence: '10' },
      reading: { quality: 'fresh', value: true },
    });
  });

  it('accepts a Disabled source: model set, no cursor, unavailable observation, no error', () => {
    expect(parseMockIoInputReceipt(body(unavailable, { published_cursor: null }), selector, true)).toMatchObject({
      modelValue: true, publishedCursor: null, publicationError: null, reading: { quality: 'unavailable' },
    });
  });

  it('carries an ingress failure as a publication error', () => {
    expect(parseMockIoInputReceipt(body(unavailable, {
      published_cursor: null, publication_error: { code: 'io_ingress_failed', message: 'full', details: {} },
    }), selector, true).publicationError).toEqual({ code: 'io_ingress_failed', message: 'full' });
  });

  it.each([
    ['another model value', body(fresh, { model_value: false })],
    ['a non-boolean model value', body(fresh, { model_value: 'true' })],
    ['no cursor field', (() => { const value: Record<string, unknown> = body(fresh); delete value.published_cursor; return value; })()],
    ['a non-canonical sequence', body(fresh, { published_cursor: { ...cursor, record_sequence: '010' } })],
    ['a numeric counter', body(fresh, { published_cursor: { ...cursor, catalog_generation: 1 } })],
    ['a zero record sequence', body(fresh, { published_cursor: { ...cursor, record_sequence: '0' } })],
    ['another owner epoch', body(fresh, { published_cursor: { ...cursor, owner_epoch: 'owner.2' } })],
    ['another source', body(fresh, { published_cursor: { ...cursor, source_id: 'other' } })],
    ['an extra cursor field', body(fresh, { published_cursor: { ...cursor, source_epoch: '3' } })],
    ['a cursor with an error', body(fresh, { publication_error: { code: 'x', message: 'y' } })],
    ['a null error', body(unavailable, { published_cursor: null, publication_error: null })],
    ['an unknown field', body(fresh, { delivered: true })],
    ['the query command', { ...body(fresh), command: 'query_target' }],
  ])('refuses %s', (_name, value) => {
    expect(() => parseMockIoInputReceipt(value, selector, true)).toThrow();
  });
});

describe('ioSignalBindingsAt', () => {
  const signalRow = (component: string, target: string) => (name: string) => ({
    reference: { component_id: component, signal_name: name },
    target: { component_id: component, command_target_id: target }, runtime_unit_id: component,
    value_type: 'bool', direction: 'input', transition_capability: 'sampled_only', signal_descriptor_digest: REVISION,
  });
  const row = (component: string, target: string, names = ['a', 'b'], signals = names.map(signalRow(component, target))) => ({
    program_binding_id: BINDING_ID, target: { component_id: component, command_target_id: target },
    signals, signal_sources: [],
  });

  it('parses canonical rows into the panel adapter shape', () => {
    const rows = ioSignalBindingsAt([row('a', 'io'), row('b', 'io')], 'rows');
    expect(rows[1]).toMatchObject({ componentId: 'b', commandTargetId: 'io', programBindingId: BINDING_ID,
      signals: [{ componentId: 'b', signalName: 'a', signalDescriptorDigest: REVISION }, { signalName: 'b' }] });
  });

  it.each([
    ['an empty array', []],
    ['an unordered array', [row('b', 'io'), row('a', 'io')]],
    ['a duplicate row', [row('a', 'io'), row('a', 'io')]],
    ['unsorted signals', [row('a', 'io', ['b', 'a'])]],
    ['no signals', [row('a', 'io', [])]],
    ['an extra field', [{ ...row('a', 'io'), extra: 1 }]],
    ['a non-bool signal', [row('a', 'io', [], [{ ...signalRow('a', 'io')('a'), value_type: 'int' }])]],
    ['a signal of another target', [row('a', 'io', [], [signalRow('a', 'other')('a')])]],
  ])('refuses %s', (_name, value) => {
    expect(() => ioSignalBindingsAt(value, 'rows')).toThrow();
  });
});

describe('execution targets io_signal_bindings', () => {
  const identity = {
    active_publication_generation: 1, cell_id: 'cell', control_management_mode: 'direct',
    graph_revision: REVISION, owner_epoch: 'owner.1', retained_bundle_id: 'bundle.1',
    schema_version: 'manyforge.execution_identity.v2',
  };
  const targets = (extra: Record<string, unknown>) => ({
    api_version: 'manyforge.execution_command_response.v4', command: 'targets', ok: true, identity,
    targets: { components: [], schema_version: 'manyforge.execution_targets.v4', ...extra },
  });
  const control = {
    api_version: 'manyforge.execution_command_response.v4', command: 'control_state', ok: true, identity,
    control_state: {
      cell_stop_action: { available: false, expected_stop_generation: 1, latest_terminal: null,
        unavailable_reason: 'No motion target.' },
      manual_operations: [], observed_targets: [], runtime_scale_control: null,
      schema_version: 'manyforge.execution_control_state.v7',
    },
  };
  const stationTarget = { component_id: 'station', command_target_id: 'io' };
  const rows = [{
    program_binding_id: BINDING_ID, target: stationTarget,
    signals: [{ reference: { component_id: 'station', signal_name: 'part_present' }, target: stationTarget,
      runtime_unit_id: 'station', value_type: 'bool', direction: 'input', transition_capability: 'sampled_only',
      signal_descriptor_digest: REVISION }],
    signal_sources: [] as unknown[],
  }];

  it('is optional, strict, and carried on the owner status', async () => {
    const { parseExecutionControlStatus } = await import('../api/parsers/execution');
    expect(parseExecutionControlStatus(targets({}), control).ioSignalBindings).toBeUndefined();
    expect(parseExecutionControlStatus(targets({ io_signal_bindings: rows }), control).ioSignalBindings)
      .toEqual(ioSignalBindingsAt(rows, 'rows'));
    expect(() => parseExecutionControlStatus(targets({ io_signal_bindings: [] }), control)).toThrow();
    expect(() => parseExecutionControlStatus(targets({ unknown: 1 }), control)).toThrow();
  });

  it('parses the generated read-only system signal projection', async () => {
    const { parseExecutionControlStatus } = await import('../api/parsers/execution');
    const rows = [{
      controllable_component_id: 'robot', command_target_id: 'gripper',
      signal_name: 'carried_object_lost.gripper', source_kind: 'system_carried_object',
      direction: 'input', value_type: 'bool', level: true,
    }];
    expect(parseExecutionControlStatus(targets({}), control).systemSignals).toEqual([]);
    expect(parseExecutionControlStatus(targets({ system_signals: [] }), control).systemSignals).toEqual([]);
    expect(parseExecutionControlStatus(targets({ system_signals: rows }), control).systemSignals).toEqual([{
      controllableComponentId: 'robot', commandTargetId: 'gripper',
      signalName: 'carried_object_lost.gripper', sourceKind: 'system_carried_object',
      direction: 'input', valueType: 'bool', level: true,
    }]);
    expect(() => parseExecutionControlStatus(targets({ system_signals: [{ ...rows[0], level: 'true' }] }), control))
      .toThrow();
    expect(() => parseExecutionControlStatus(targets({ system_signals: [{ ...rows[0], direction: 'output' }] }), control))
      .toThrow();
    expect(() => parseExecutionControlStatus(targets({ system_signals: [rows[0], {
      ...rows[0], controllable_component_id: 'aaa', command_target_id: 'gripper',
    }] }), control)).toThrow(/sorted by controllable_component_id/);
    expect(() => parseExecutionControlStatus(targets({ system_signals: [{ ...rows[0], extra: true }] }), control))
      .toThrow();
  });

  it('accepts the host signal_sources projection and checks its shape', () => {
    const source = { signal: { component_id: 'station', signal_name: 'part_present' }, source_kind: 'mock' };
    const withSources = [{ ...rows[0], signal_sources: [source] }];
    expect(ioSignalBindingsAt(withSources, 'rows')).toEqual(ioSignalBindingsAt(rows, 'rows'));
    expect(() => ioSignalBindingsAt([{ ...rows[0], signal_sources: null }], 'rows')).toThrow();
    expect(() => ioSignalBindingsAt([{ ...rows[0], signal_sources: {} }], 'rows')).toThrow();
    expect(() => ioSignalBindingsAt(
      [{ ...rows[0], signal_sources: [{ ...source, manual_only: true }] }],
      'rows',
    )).toThrow();
    expect(() => ioSignalBindingsAt([{ ...rows[0], signal_sources: [{ ...source,
      signal: { component_id: 1, signal_name: 'part_present' } }] }], 'rows'))
      .toThrow();
  });
});

describe('global rule status section', () => {
  const rulesView = (globalRules?: ToolIoPanelViewProps['globalRules']) => view({ globalRules });

  it('shows each rule state beside the signals', () => {
    const html = rulesView([
      { ruleId: 'door', state: 'tripped', keys: [] },
      { ruleId: 'off', state: 'inactive', keys: [] },
      { ruleId: 'unknown_rule', state: 'unknown', keys: [], unavailableReason: {
        code: 'motion_interrupt_arm_failed', message: 'Notify rule cannot watch its signals',
        details: { reason: 'io_rule_source_unavailable', source: 'door' },
      } },
      { ruleId: 'light', state: 'pending', keys: [] },
      { ruleId: 'part', state: 'waiting_for_program', keys: ['part_ready'] },
    ]);
    expect(html).toContain('data-global-rule="door" data-global-rule-state="tripped"');
    expect(html).toContain('data-global-rule="off" data-global-rule-state="inactive"');
    expect(html).toContain('data-global-rule="unknown_rule" data-global-rule-state="unknown"');
    expect(html).toContain('>inactive<');
    expect(html).toContain("can&#x27;t watch (its scope is held)");
    expect(html).toContain("Interrupt rule unknown_rule can&#x27;t watch its signals: Interrupt rule source unavailable");
    expect(html).toContain('<summary>Details</summary>');
    expect(html).toContain('io_rule_source_unavailable');
    expect(html).toContain('pending (no current state yet)');
    expect(html).toContain('waiting for a Program that declares part_ready');
    expect(html).toContain('The running cell has no bound I/O signals.');
  });

  it('never shows a retained clear as live while the control state is not current', () => {
    const rules = [{ ruleId: 'door', state: 'clear' as const, keys: [] }];
    const live = rulesView(rules);
    expect(live).toContain('status-pill status-pill-connected');
    expect(live).toContain('data-global-rule-current="true"');
    const retained = view({ globalRules: rules, globalRulesCurrent: false });
    expect(retained).not.toContain('status-pill-connected');
    expect(retained).toContain('data-global-rule-current="false"');
    expect(retained).toContain('last known: clear (not current)');
    expect(retained).toContain('showing the last known rule states');
  });

  it('shows unknown instead of retaining a prior clear state', () => {
    const html = rulesView([{
      ruleId: 'door', state: 'unknown', keys: [], unavailableReason: {
        code: 'motion_interrupt_arm_failed', message: 'Notify rule cannot watch its signals',
        details: { reason: 'io_rule_source_unavailable' },
      },
    }]);
    expect(html).toContain('data-global-rule="door" data-global-rule-state="unknown"');
    expect(html).toContain("can&#x27;t watch (its scope is held)");
    expect(html).toContain("Interrupt rule door can&#x27;t watch its signals: Interrupt rule source unavailable");
    expect(html).not.toContain('last known: clear');
  });

  it('says unknown, and hides the section with no rules or no control source', () => {
    expect(rulesView(null)).toContain('Global rule states are not known yet.');
    expect(rulesView([])).not.toContain('Global rules');
    expect(rulesView(undefined)).not.toContain('Global rules');
  });
});
