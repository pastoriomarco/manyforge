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
} from '../api/parsers/ioSignals';
import type { ProgramBindingDocument } from '../types/api';
import { reactPropsHost } from '../test/reactPropsHost';
import { acknowledgedUncertainOutputs, uncertainOutputs } from '../utils/uncertainOutputs';
import {
  ToolIoPanel,
  ToolIoPanelView,
  toolIoSignalKey,
  type ToolIoSignal,
} from './ToolIoPanel';

const api = vi.hoisted(() => ({
  readIoSignal: vi.fn(),
  setMockIoInput: vi.fn(),
  getWorkspace: vi.fn(),
}));

vi.mock('../api/client', () => ({
  api: {
    execution: { readIoSignal: api.readIoSignal, setMockIoInput: api.setMockIoInput },
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
    programBindingId: BINDING_ID, controllableComponentId: 'station', targetId: 'io', targetRoleId: 'station',
    resourceRoleId: 'part_present', signalName: 'part_present', direction: 'input',
    declared: { profile: 'mock', sourceId: 'station_mock', topic: null, liveness: 'in_process' },
    ...patch,
  };
}

function receipt(patch: Partial<MockIoInputReceipt> = {}): MockIoInputReceipt {
  return {
    reading: reading({ value: true }), modelValue: true,
    publishedCursor: { ownerEpoch: 'owner.1', catalogGeneration: '1', sourceId: 'station_mock', recordSequence: '10' },
    publicationError: null, ...patch,
  };
}

function view(
  signals: ToolIoSignal[],
  readings: Array<IoSignalReading | null>,
  extra: Array<Partial<{ receipt: MockIoInputReceipt; lastSourceEpoch: string }>> = [],
) {
  return renderToStaticMarkup(
    <ToolIoPanelView
      unavailableReason={null}
      signals={signals}
      states={Object.fromEntries(signals.map((item, index) => [
        toolIoSignalKey(item), { reading: readings[index], error: null, sending: false, ...extra[index] },
      ]))}
      sourcesNotice={null}
      onSetMock={() => undefined}
    />,
  );
}

afterEach(() => {
  vi.unstubAllGlobals();
  vi.clearAllMocks();
});

describe('ToolIoPanelView', () => {
  it('shows a fresh Mock reading with an enabled explicit toggle', () => {
    const html = view([signal()], [reading()]);
    expect(html).toContain('data-io-quality="fresh"');
    expect(html).toContain('status-pill status-pill-connected');
    expect(html).toContain('12 ms');
    expect(html).toContain('Mock · station_mock');
    expect(html).not.toContain('Auto-answer');
    expect(html).toMatch(/<button type="button" aria-label="Set part_present false">/);
  });

  it.each(['stale', 'unavailable', 'missing', 'invalid'] as const)(
    'never presents a %s reading as current', (quality) => {
      const html = view([signal()], [reading({
        quality, value: quality === 'stale' ? false : null, receiptAgeNs: null,
        ...(quality === 'unavailable' ? { sourceEpoch: null } : {}),
      })]);
      expect(html).toContain(`data-io-quality="${quality}"`);
      expect(html).toContain('status-pill status-pill-danger');
      expect(html).toContain(quality === 'stale' ? 'false (last reported)' : '>—<');
      if (quality === 'unavailable') expect(html).toContain('disabled=""');
    },
  );

  it('distinguishes a delivered toggle, a kept model on a Disabled source and a delivery failure', () => {
    const unavailable = reading({ quality: 'unavailable', value: null, sourceEpoch: null, sourceSequence: null, receiptAgeNs: null });
    const delivered = view([signal()], [reading({ value: true })], [{ receipt: receipt() }]);
    expect(delivered).toContain('data-io-receipt="delivered"');
    expect(delivered).toContain('Model set to true; delivered as record 10 of station_mock.');

    const disabled = view([signal()], [unavailable], [{
      receipt: receipt({ reading: unavailable, publishedCursor: null }), lastSourceEpoch: '3',
    }]);
    expect(disabled).toContain('data-io-receipt="not_delivered"');
    expect(disabled).toContain('inline-message severity-info');
    expect(disabled).not.toContain('role="alert"');
    expect(disabled).toContain('data-io-quality="unavailable"');
    // The accepted model is the current value: the next toggle is the opposite, still allowed.
    expect(disabled).toMatch(/<button type="button" aria-label="Set part_present false">/);

    const failed = view([signal()], [unavailable], [{
      receipt: receipt({ reading: unavailable, publishedCursor: null,
        publicationError: { code: 'io_ingress_full', message: 'ingress full' } }),
      lastSourceEpoch: '3',
    }]);
    expect(failed).toContain('data-io-receipt="publication_error"');
    expect(failed).toContain('inline-message severity-warning');
    expect(failed).toContain('io_ingress_full: ingress full');
    expect(failed).not.toContain('role="alert"');
  });

  it('disables the toggle for a real source and shows outputs as not implemented', () => {
    const ros = signal({
      resourceRoleId: 'door', signalName: 'door',
      declared: { profile: 'ros_bool', sourceId: 'door_topic', topic: '/cell/door', liveness: 'periodic' },
    });
    const valve = signal({ resourceRoleId: 'valve', signalName: 'valve', direction: 'output' });
    const mock = signal({ resourceRoleId: 'manual', signalName: 'manual' });
    const html = view([ros, valve, mock], [reading({ sourceId: 'door_topic', liveness: { mode: 'periodic', warning: null } }), null, reading()]);
    expect(html).toContain('ROS Bool topic · door_topic · /cell/door');
    expect(html).toMatch(/disabled="" title="Only Mock inputs can be set; this is a real source."/);
    expect(html).toContain('Output: not implemented');
    // The retired Mock auto-answer (contracts 132/133) is no longer shown.
    expect(html).not.toContain('Auto-answer');
    expect(html).toContain('data-io-liveness="periodic"');
    expect(html).not.toContain('Liveness unproven');
  });

  it('shows geometric contact as a periodic read-only input', () => {
    const contact = signal({
      resourceRoleId: 'part_contact', signalName: 'part_contact',
      declared: {
        profile: 'geometric_contact', sourceId: 'part_contact_geometry', topic: null,
        liveness: 'periodic',
      },
    });
    const html = view([contact], [reading({
      signalName: 'part_contact', sourceId: 'part_contact_geometry', value: false,
      liveness: { mode: 'periodic', warning: null },
    })]);
    expect(html).toContain('Geometric contact · part_contact_geometry');
    expect(html).toContain('data-io-liveness="periodic"');
    expect(html).toMatch(/disabled="" title="Only Mock inputs can be set; this is a real source."/);
  });

  it('shows the owner-projected liveness and a change-only warning (SX-2194)', () => {
    const door = signal({
      resourceRoleId: 'door', signalName: 'door',
      declared: { profile: 'ros_bool', sourceId: 'door_topic', topic: '/cell/door', liveness: 'periodic' },
    });
    // The owner's descriptor projection wins over the Draft declaration.
    const html = view([door], [reading({ liveness: { mode: 'change_only', warning: 'liveness_unproven' } })]);
    expect(html).toContain('data-io-liveness="change_only"');
    expect(html).toContain('Liveness unproven');
    expect(html).toContain('cannot prove it is alive between changes');
    // Before the first reading the declared mode shows.
    const gpio = signal({
      resourceRoleId: 'grip', signalName: 'grip',
      declared: { profile: 'ros2_control_gpio', sourceId: 'gpio', topic: '/gpio', liveness: 'periodic' },
    });
    const pending = view([gpio], [null]);
    expect(pending).toContain('data-io-liveness="periodic"');
    expect(pending).toContain('Freshness depends on the driver reporting read errors');
  });
});

describe('ToolIoPanel', () => {
  const binding = {
    program_binding_id: BINDING_ID,
    retained_bundle_id: 'bundle.1',
    target_role_bindings: [{
      target_role_id: 'station', controllable_component_id: 'station', command_target_id: 'io',
      signal_role_bindings: [
        { resource_role_id: 'part_present', signal_name: 'part_present', value_type: 'bool',
          direction: 'input', transition_capability: 'sampled_only', signal_descriptor_digest: REVISION },
        { resource_role_id: 'door', signal_name: 'door', value_type: 'bool',
          direction: 'input', transition_capability: 'sampled_only', signal_descriptor_digest: REVISION },
      ],
    }],
  } as unknown as ProgramBindingDocument;
  const row = (resource: string, provider: Record<string, unknown>, extra: Record<string, unknown>) => ({
    controllable_component_id: 'station', command_target_id: 'io', resource_role_id: resource,
    signal_name: resource, source_id: `${resource}_source`, value_type: 'bool', direction: 'input',
    transition_capability: 'sampled_only', provider, ...extra,
  });

  it('polls only the bound inputs and sets the Mock input to the explicit opposite value', async () => {
    api.getWorkspace.mockResolvedValue({
      draft: {
        deploymentRevision: REVISION,
        document: { graph: { runtime_units: [{ configuration: { io_signals: [
          row('part_present', { profile: 'mock' }, { update_mode: 'change_only', freshness: { mode: 'provider_validity' } }),
          row('door', { profile: 'ros_bool', topic: '/door' }, {
            update_mode: 'sampled', freshness: { mode: 'sample_age', maximum_age_ns: 1_000_000 } }),
        ] } }] } },
      },
      managerApplied: { deploymentRevision: REVISION },
      binding,
    });
    api.readIoSignal.mockImplementation(async (selector: { resourceRoleId: string }) => reading({
      signalName: selector.resourceRoleId, value: false,
    }));
    api.setMockIoInput.mockResolvedValue(receipt({
      reading: reading({ quality: 'unavailable', value: null, sourceEpoch: null, sourceSequence: null, receiptAgeNs: null }),
      publishedCursor: null,
    }));
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    try {
      await act(async () => {
        root.render(<ToolIoPanel enabled ownerEpoch="owner.1" retainedBundleId="bundle.1"
          pollIntervalMs={60_000} />);
      });
      await act(async () => { await Promise.resolve(); });
      expect(api.readIoSignal).toHaveBeenCalledTimes(2);
      expect(api.readIoSignal).toHaveBeenCalledWith({
        ownerEpoch: 'owner.1', programBindingId: BINDING_ID, targetRoleId: 'station', resourceRoleId: 'part_present',
      }, expect.anything());
      const door = find((props) => props['aria-label'] === 'Set door true');
      expect(door.disabled).toBe(true);
      const mock = find((props) => props['aria-label'] === 'Set part_present true');
      expect(mock.disabled).toBe(false);
      await act(async () => { mock.onClick?.(); });
      expect(api.setMockIoInput).toHaveBeenCalledExactlyOnceWith({
        ownerEpoch: 'owner.1', programBindingId: BINDING_ID, targetRoleId: 'station', resourceRoleId: 'part_present',
      }, '3', true);
      // A Disabled source: the model is accepted, no failure, and the next
      // toggle still uses the last reported epoch.
      expect(find((props) => props['data-io-receipt'] === 'not_delivered')).toBeTruthy();
      const again = find((props) => props['aria-label'] === 'Set part_present false');
      expect(again.disabled).toBe(false);
      await act(async () => { again.onClick?.(); });
      expect(api.setMockIoInput).toHaveBeenLastCalledWith(expect.anything(), '3', false);
    } finally {
      await act(() => root.unmount());
    }
  });

  it('reads the owner projection rows and does not simulate when the Draft differs from Applied', async () => {
    api.getWorkspace.mockResolvedValue({
      draft: { deploymentRevision: null, document: null },
      managerApplied: { deploymentRevision: REVISION },
      binding: null,
    });
    api.readIoSignal.mockResolvedValue(reading({ value: false }));
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    try {
      await act(async () => {
        root.render(<ToolIoPanel enabled ownerEpoch="owner.1" retainedBundleId="bundle.1"
          ioSignalBindings={ioSignalBindingsFromProgramBinding(binding)} pollIntervalMs={60_000} />);
      });
      await act(async () => { await Promise.resolve(); });
      expect(api.readIoSignal).toHaveBeenCalledTimes(2);
      expect(find((props) => props['aria-label'] === 'Set part_present true').disabled).toBe(true);
      expect(api.setMockIoInput).not.toHaveBeenCalled();
    } finally {
      await act(() => root.unmount());
    }
  });

  it('shows why nothing is read before the owner or a bound signal exists', () => {
    expect(renderToStaticMarkup(<ToolIoPanel enabled ownerEpoch={null} retainedBundleId="bundle.1" />))
      .toContain('Waiting for the execution owner');
    expect(renderToStaticMarkup(<ToolIoPanel enabled ownerEpoch="owner.1" retainedBundleId="bundle.1" />))
      .toContain('No applied Program binding');
    expect(api.readIoSignal).not.toHaveBeenCalled();
  });
});

describe('parseIoSignalResponse', () => {
  const selector = { ownerEpoch: 'owner.1', programBindingId: BINDING_ID, targetRoleId: 'station', resourceRoleId: 'part_present' };
  const response = (value: Record<string, unknown>, extra: Record<string, unknown> = {}) => ({
    api_version: 'manyforge.execution_command_response.v1', ok: true, command: 'query_target',
    target_observation: {
      schema_version: 'manyforge.target_observation.v1', owner_epoch: 'owner.1', program_binding_id: BINDING_ID,
      target_role_id: 'station', kind: 'io_signal', resource_role_id: 'part_present',
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

  it('keeps the host opaque sha256 source epoch; the sequence stays a counter (SX-2518)', () => {
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

  it('parses the descriptor liveness projection (SX-2194)', () => {
    expect(parseIoSignalResponse(response({ ...value, liveness: { mode: 'change_only', warning: 'liveness_unproven' } }),
      'query_target', selector).liveness).toEqual({ mode: 'change_only', warning: 'liveness_unproven' });
    expect(parseIoSignalResponse(response({ ...value, liveness: null }), 'query_target', selector).liveness).toBeNull();
  });

  it.each([
    ['another selector', response(value, { resource_role_id: 'other' })],
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
  const selector = { ownerEpoch: 'owner.1', programBindingId: BINDING_ID, targetRoleId: 'station', resourceRoleId: 'part_present' };
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
    api_version: 'manyforge.execution_command_response.v1', ok: true, command: 'set_mock_io_input',
    target_observation: {
      schema_version: 'manyforge.target_observation.v1', owner_epoch: 'owner.1', program_binding_id: BINDING_ID,
      target_role_id: 'station', kind: 'io_signal', resource_role_id: 'part_present', evaluated_at_ns: 5000, value,
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
  const signalRow = (resource: string) => ({
    resource_role_id: resource, signal_name: resource, value_type: 'bool', direction: 'input',
    transition_capability: 'sampled_only', signal_descriptor_digest: REVISION,
  });
  const row = (component: string, target: string, signals = [signalRow('a'), signalRow('b')]) => ({
    program_binding_id: BINDING_ID, target_role_id: 'role', controllable_component_id: component,
    command_target_id: target, signal_role_bindings: signals,
  });

  it('parses canonical rows into the panel adapter shape', () => {
    const rows = ioSignalBindingsAt([row('a', 'io'), row('b', 'io')], 'rows');
    expect(rows[1]).toMatchObject({ controllableComponentId: 'b', programBindingId: BINDING_ID,
      signalRoleBindings: [{ resourceRoleId: 'a', signalDescriptorDigest: REVISION }, { resourceRoleId: 'b' }] });
  });

  it('accepts the sealed direct output command metadata only for an output', () => {
    const output = { ...signalRow('valve'), direction: 'output' };
    const command = { program_id: 'direct-io', program_revision: REVISION, program_run_id: 'direct-control-io-owner', component_role_id: 'io-role',
      capability_id: 'io.gpio.set_output', command_schema_id: 'io.write', command_schema_revision: REVISION };
    const result = ioSignalBindingsAt([{ ...row('a', 'io', [output]), output_command: command }], 'rows');
    expect(result[0].outputCommand).toMatchObject({ programId: 'direct-io', componentRoleId: 'io-role', capabilityId: 'io.gpio.set_output' });
    expect(() => ioSignalBindingsAt([{ ...row('a', 'io', [signalRow('valve')]), output_command: command }], 'rows')).toThrow();
    expect(() => ioSignalBindingsAt([{ ...row('a', 'io', [output]), output_command: { ...command, program_id: '' } }], 'rows')).toThrow();
  });

  it.each([
    ['an empty array', []],
    ['an unordered array', [row('b', 'io'), row('a', 'io')]],
    ['a duplicate row', [row('a', 'io'), row('a', 'io')]],
    ['unsorted signals', [row('a', 'io', [signalRow('b'), signalRow('a')])]],
    ['no signals', [row('a', 'io', [])]],
    ['an extra field', [{ ...row('a', 'io'), extra: 1 }]],
    ['a non-bool signal', [row('a', 'io', [{ ...signalRow('a'), value_type: 'int' }])]],
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
    api_version: 'manyforge.execution_command_response.v1', command: 'targets', ok: true, identity,
    targets: { components: [], schema_version: 'manyforge.execution_targets.v3', ...extra },
  });
  const control = {
    api_version: 'manyforge.execution_command_response.v1', command: 'control_state', ok: true, identity,
    control_state: {
      cell_stop_action: { available: false, expected_stop_generation: 1, fault: null, latest_terminal: null,
        unavailable_reason: 'No motion target.' },
      manual_operations: [], observed_targets: [], runtime_scale_control: null,
      schema_version: 'manyforge.execution_control_state.v6',
    },
  };
  const rows = [{
    program_binding_id: BINDING_ID, target_role_id: 'station', controllable_component_id: 'station',
    command_target_id: 'io', signal_role_bindings: [{ resource_role_id: 'part_present', signal_name: 'part_present',
      value_type: 'bool', direction: 'input', transition_capability: 'sampled_only', signal_descriptor_digest: REVISION }],
  }];

  it('is optional, strict, and carried on the owner status', async () => {
    const { parseExecutionControlStatus } = await import('../api/parsers/execution');
    expect(parseExecutionControlStatus(targets({}), control).ioSignalBindings).toBeUndefined();
    expect(parseExecutionControlStatus(targets({ io_signal_bindings: rows }), control).ioSignalBindings)
      .toEqual(ioSignalBindingsAt(rows, 'rows'));
    expect(() => parseExecutionControlStatus(targets({ io_signal_bindings: [] }), control)).toThrow();
    expect(() => parseExecutionControlStatus(targets({ unknown: 1 }), control)).toThrow();
  });

  it('accepts the host signal_sources projection and checks its shape', () => {
    const withSources = [{ ...rows[0], signal_sources: [{ resource_role_id: 'part_present', source_kind: 'mock' }] }];
    expect(ioSignalBindingsAt(withSources, 'rows')).toEqual(ioSignalBindingsAt(rows, 'rows'));
    expect(() => ioSignalBindingsAt([{ ...rows[0], signal_sources: null }], 'rows')).toThrow();
    expect(() => ioSignalBindingsAt([{ ...rows[0], signal_sources: {} }], 'rows')).toThrow();
    expect(() => ioSignalBindingsAt(
      [{ ...rows[0], signal_sources: [{ resource_role_id: 'part_present', source_kind: 'mock', manual_only: true }] }],
      'rows',
    )).toThrow();
    expect(() => ioSignalBindingsAt([{ ...rows[0], signal_sources: [{ resource_role_id: 1, source_kind: 'mock' }] }], 'rows'))
      .toThrow();
  });

  it('restores an uncertain manual output from the owner snapshot for targeted Reset', async () => {
    const { parseExecutionControlStatus } = await import('../api/parsers/execution');
    const row = { ...rows[0], signal_role_bindings: [{ ...rows[0].signal_role_bindings[0], direction: 'output' }],
      output_command: { program_id: 'direct-io', program_revision: REVISION, program_run_id: 'direct-control-io-owner',
        component_role_id: 'io-role', capability_id: 'io.gpio.set_output', command_schema_id: 'io.write', command_schema_revision: REVISION } };
    const operation = { program_run_id: 'direct-control-io-owner', program_binding_id: BINDING_ID,
      operation_id: 'valve-op', operation_revision: 2, state: 'intervention_required',
      participants: [{ progress: 0, terminal_error: { message: 'acceptance unknown' },
        controllable_component_id: 'station', command_target_id: 'io', output_evidence: 'acceptance_unknown' }],
      active_stop_id: null, active_stop_generation: null };
    const state = { ...control, control_state: { ...control.control_state,
      manual_operations: [{ operation, resident: true }] } };
    const recovered = parseExecutionControlStatus(targets({ io_signal_bindings: [row] }), state);
    const uncertain = uncertainOutputs(recovered.manualOperations);
    expect(uncertain).toEqual([{ operationId: 'valve-op', operationRevision: 2, label: 'station · io (valve-op)' }]);
    expect(acknowledgedUncertainOutputs('valve-op', uncertain))
      .toEqual([{ operation_id: 'valve-op', operation_revision: 2 }]);
    expect(() => parseExecutionControlStatus(targets({ io_signal_bindings: [row] }), {
      ...state, control_state: { ...state.control_state, manual_operations: [{
        operation: { ...operation, program_binding_id: `sha256:${'f'.repeat(64)}` }, resident: true,
      }] },
    })).toThrow(/direct I\/O bindings/);
  });
});

describe('global rule status section', () => {
  const view = (globalRules?: Parameters<typeof ToolIoPanelView>[0]['globalRules']) => renderToStaticMarkup(
    <ToolIoPanelView unavailableReason={null} signals={[]} states={{}} sourcesNotice={null}
      onSetMock={() => undefined} globalRules={globalRules} />,
  );

  it('shows each rule state beside the signals', () => {
    const html = view([
      { ruleId: 'door', state: 'latched', keys: [] },
      { ruleId: 'light', state: 'pending', keys: [] },
      { ruleId: 'part', state: 'waiting_for_program', keys: ['part_ready'] },
    ]);
    expect(html).toContain('data-global-rule="door" data-global-rule-state="latched"');
    expect(html).toContain('latched (see its recovery notice)');
    expect(html).toContain('pending (no current state yet)');
    expect(html).toContain('waiting for a Program that declares part_ready');
    expect(html).toContain('The running cell has no bound I/O signals.');
  });

  it('never shows a retained clear as live while the control state is not current', () => {
    const rules = [{ ruleId: 'door', state: 'clear' as const, keys: [] }];
    const live = view(rules);
    expect(live).toContain('status-pill status-pill-connected');
    expect(live).toContain('data-global-rule-current="true"');
    const retained = renderToStaticMarkup(
      <ToolIoPanelView unavailableReason={null} signals={[]} states={{}} sourcesNotice={null}
        onSetMock={() => undefined} globalRules={rules} globalRulesCurrent={false} />,
    );
    expect(retained).not.toContain('status-pill-connected');
    expect(retained).toContain('data-global-rule-current="false"');
    expect(retained).toContain('last known: clear (not current)');
    expect(retained).toContain('showing the last known rule states');
  });

  it('says unknown, and hides the section with no rules or no control source', () => {
    expect(view(null)).toContain('Global rule states are not known yet.');
    expect(view([])).not.toContain('Global rules');
    expect(view(undefined)).not.toContain('Global rules');
  });
});
