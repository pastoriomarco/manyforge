import { describe, expect, it, vi } from 'vitest';
import { statusFixture } from '../testFixtures/robotControl';
import type { ExecutionOwnerStatus, OperationObservation } from '../types/robotControl';
import { manualPneumaticValves, manualValveIntent, writeManualPneumaticValves } from './pneumaticManual';
import { buildGripperCommandIntent } from './robotControl';

function mapped(): ExecutionOwnerStatus {
  const status = statusFixture();
  const descriptor = status.directControlGripper!.targetDescriptor;
  descriptor.actuationKind = 'pneumatic';
  descriptor.outputMode = 'commanded';
  descriptor.completionPolicy = 'no_output';
  descriptor.graspConfirmationPolicy = 'not_required';
  descriptor.namedPresets = [
    { name: 'open', masterPosition: 0.04, coordinates: [] },
    { name: 'closed', masterPosition: 0, coordinates: [] },
  ];
  descriptor.pneumaticOutputs = [
    { controllableComponentId: 'z_io', signalName: 'close_valve', openValue: true, closeValue: false },
    { controllableComponentId: 'a_io', signalName: 'open_valve', openValue: false, closeValue: true },
  ];
  status.ioSignalBindings = descriptor.pneumaticOutputs.map((row, index) => ({
    programBindingId: `binding-${index}`, targetRoleId: `target-${index}`,
    controllableComponentId: row.controllableComponentId, commandTargetId: 'io',
    outputCommand: { programId: `program-${index}`, programRevision: `revision-${index}`, programRunId: 'direct-control-io-owner',
      componentRoleId: `component-${index}`, capabilityId: 'io.gpio.set_output',
      commandSchemaId: 'io.write', commandSchemaRevision: 'schema-1' },
    signalRoleBindings: [{ resourceRoleId: `signal-${index}`, signalName: row.signalName,
      valueType: 'bool', direction: 'output', transitionCapability: 'sampled_only', signalDescriptorDigest: 'digest' }],
  }));
  return status;
}

function completed(operationId: string, patch: Partial<OperationObservation> = {}): OperationObservation {
  return { operationId, operationRevision: 1, resident: false, state: 'completed', progress: 1,
    activeStopId: null, activeStopGeneration: null, terminalError: null, outputEvidence: 'source_accepted', ...patch };
}

describe('manual pneumatic outputs', () => {
  it('resolves exact Applied I/O bindings, writes false first and creates distinct requests', () => {
    const status = mapped();
    const valves = manualPneumaticValves(status, 0.04);
    expect(valves.map((row) => [row.componentId, row.value])).toEqual([['a_io', false], ['z_io', true]]);
    const first = manualValveIntent(status, valves[0], 'request-a');
    const second = manualValveIntent(status, valves[1], 'request-b');
    expect(first.program_binding_id).toBe('binding-1');
    expect(first.participants[0].payload.command_value).toEqual({ kind: 'io_signal_write', resource_role_id: 'signal-1', value: false });
    expect(first.program_run_id).toBe(second.program_run_id);
    expect(first.request_context.request_id).not.toBe(second.request_context.request_id);
    expect(manualPneumaticValves(status, 0.02).map((row) => [row.componentId, row.value]))
      .toEqual([['z_io', false], ['a_io', true]]);
    const logical = buildGripperCommandIntent(status, 0.02, 'logical');
    expect(logical.participants[0].payload).toMatchObject({ command_value: {
      kind: 'gripper_position', position: { kind: 'linear_m', value_m: 0 },
    } });
    expect(buildGripperCommandIntent(status, 0.03, 'logical-open').participants[0].payload)
      .toMatchObject({ command_value: { position: { kind: 'linear_m', value_m: 0.04 } } });
    status.directControlGripper!.targetDescriptor.namedPresets[0].masterPosition = 0;
    expect(() => manualPneumaticValves(status, 0)).toThrow('different positions');
    status.directControlGripper!.targetDescriptor.namedPresets[0].masterPosition = 0.04;
    status.ioSignalBindings!.push(status.ioSignalBindings![1]);
    expect(() => manualPneumaticValves(status, 0.04)).toThrow('no unique direct I/O binding');
  });

  it('waits for confirmed nonresident completion before issuing the next valve', async () => {
    const events: string[] = [];
    let serial = 0;
    const submit = vi.fn(async (intent: ReturnType<typeof manualValveIntent>) => {
      events.push(`send:${intent.participants[0].payload.command_value.value}`);
      return { result: { accepted: { request_id: intent.request_context.request_id, operation_id: `op-${serial}` } } };
    });
    let readCount = 0;
    const read = vi.fn(async (operationId: string) => {
      events.push(`read:${operationId}`);
      readCount += 1;
      return completed(operationId, readCount === 1 ? { resident: true } : {});
    });
    await writeManualPneumaticValves(mapped(), 0.04, {
      nextRequestId: () => `req-${++serial}`, checkCurrent: vi.fn(), submit, read,
      wait: async () => { events.push('wait'); },
    });
    expect(events).toEqual(['send:false', 'read:op-1', 'wait', 'read:op-1', 'send:true', 'read:op-2']);
  });

  it.each(['acceptance_unknown', 'source_rejected', 'not_submitted'] as const)(
    'stops on %s and does not send the next valve', async (evidence) => {
      const submit = vi.fn(async (intent: ReturnType<typeof manualValveIntent>) => ({
        result: { accepted: { request_id: intent.request_context.request_id, operation_id: 'op' } },
      }));
      await expect(writeManualPneumaticValves(mapped(), 0.04, {
        nextRequestId: () => 'request', checkCurrent: vi.fn(), submit,
        read: async () => completed('op', { state: 'failed', outputEvidence: evidence }), wait: async () => {},
      })).rejects.toThrow(evidence);
      expect(submit).toHaveBeenCalledOnce();
    },
  );

  it('never repeats an unconfirmed submission', async () => {
    const submit = vi.fn(async () => { throw new Error('delivery unconfirmed'); });
    await expect(writeManualPneumaticValves(mapped(), 0.04, {
      nextRequestId: () => 'request', checkCurrent: vi.fn(), submit,
      read: vi.fn(), wait: vi.fn(),
    })).rejects.toThrow('delivery unconfirmed');
    expect(submit).toHaveBeenCalledOnce();
  });

  it('does not send another valve when the accepted operation cannot be observed', async () => {
    const submit = vi.fn(async (intent: ReturnType<typeof manualValveIntent>) => ({
      result: { accepted: { request_id: intent.request_context.request_id, operation_id: 'op' } },
    }));
    await expect(writeManualPneumaticValves(mapped(), 0.04, {
      nextRequestId: () => 'request', checkCurrent: vi.fn(), submit,
      read: async () => { throw new Error('observation unavailable'); }, wait: vi.fn(),
    })).rejects.toThrow('observation unavailable');
    expect(submit).toHaveBeenCalledOnce();
  });
});
