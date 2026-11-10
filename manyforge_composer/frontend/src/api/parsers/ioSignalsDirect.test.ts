import { describe, expect, it } from 'vitest';
import type { ProgramBindingDocument } from '../../types/programBindingTransport.generated';
import { ioSignalBindingsAt, ioSignalBindingsFromProgramBinding, ioSignalReadBody, mockIoInputBody,
  parseIoSignalResponse } from './ioSignals';

const digest = `sha256:${'a'.repeat(64)}`;
const target = { component_id: 'station', command_target_id: 'io' };
const reference = { component_id: 'station', signal_name: 'ready' };
const signal = { reference, target, runtime_unit_id: 'unit', value_type: 'bool' as const,
  direction: 'input' as const, transition_capability: 'sampled_only' as const, signal_descriptor_digest: digest };
const binding = { program_binding_id: digest, target_bindings: [{ target, signal_references: [reference] }],
  signal_bindings: [signal] } as unknown as ProgramBindingDocument;
const selector = { ownerEpoch: 'epoch', programBindingId: digest, componentId: 'station', signalName: 'ready' };

describe('direct I/O signal transport', () => {
  it('joins exact target signal references without role coordinates', () => {
    const rows = ioSignalBindingsFromProgramBinding(binding);
    expect(rows).toEqual([{ programBindingId: digest, componentId: 'station', commandTargetId: 'io',
      signals: [{ componentId: 'station', signalName: 'ready', valueType: 'bool', direction: 'input',
        transitionCapability: 'sampled_only', signalDescriptorDigest: digest }] }]);
    expect(ioSignalBindingsAt([{ program_binding_id: digest, target, signals: [signal],
      signal_sources: [{ signal: reference, source_kind: 'mock' }] }], 'bindings')).toEqual(rows);
  });

  it('refuses missing or cross-target signal bindings', () => {
    const missing = structuredClone(binding);
    missing.signal_bindings = [];
    expect(() => ioSignalBindingsFromProgramBinding(missing)).toThrow(/exact target/);
    const foreign = structuredClone(binding);
    foreign.signal_bindings[0].target = { component_id: 'station', command_target_id: 'other' };
    expect(() => ioSignalBindingsFromProgramBinding(foreign)).toThrow(/exact target/);
    const duplicate = structuredClone(binding);
    duplicate.signal_bindings.push(structuredClone(duplicate.signal_bindings[0]));
    expect(() => ioSignalBindingsFromProgramBinding(duplicate)).toThrow(/exact target/);
    const host = { program_binding_id: digest, target, signals: [structuredClone(signal)],
      signal_sources: [{ signal: reference, source_kind: 'mock' }] };
    host.signals[0].target = { component_id: 'station', command_target_id: 'other' };
    expect(() => ioSignalBindingsAt([host], 'bindings')).toThrow(/exact selected target/);
  });

  it('uses direct v2 requests and checks the exact returned signal', () => {
    const read = ioSignalReadBody(selector);
    expect(read).toEqual({ schema_version: 'manyforge.target_observation_query.v2', owner_epoch: 'epoch',
      program_binding_id: digest, kind: 'io_signal', signal: reference });
    expect(mockIoInputBody(selector, 'source', true)).toEqual({ schema_version: 'manyforge.mock_io_input_request.v2',
      owner_epoch: 'epoch', program_binding_id: digest, signal: reference, expected_source_epoch: 'source', value: true });
    const value = { liveness: { mode: 'in_process', warning: null }, signal_name: 'ready', value_type: 'bool',
      value: true, quality: 'fresh', source_id: 'source', source_epoch: '1', source_sequence: '1',
      receipt_age_ns: 1, continuity_loss_generation: '0', error: null };
    const response = { api_version: 'manyforge.execution_command_response.v4', command: 'query_target', ok: true,
      target_observation: { schema_version: 'manyforge.target_observation.v2', owner_epoch: 'epoch',
        program_binding_id: digest, kind: 'io_signal', signal: reference, evaluated_at_ns: 1, value } };
    expect(parseIoSignalResponse(response, 'query_target', selector).value).toBe(true);
    expect(() => parseIoSignalResponse(response, 'query_target', { ...selector, componentId: 'other' })).toThrow();
    const wrongName = structuredClone(response);
    wrongName.target_observation.value.signal_name = 'other';
    expect(() => parseIoSignalResponse(wrongName, 'query_target', selector)).toThrow(/signal_name/);
    const legacy = structuredClone(response);
    legacy.target_observation.schema_version = 'manyforge.target_observation.v1';
    expect(() => parseIoSignalResponse(legacy, 'query_target', selector)).toThrow();
  });
});
