import { describe, expect, it, vi } from 'vitest';
import { statusFixture } from '../testFixtures/robotControl';
import type { SetOutputResult } from '../types/components';
import type { ExecutionOwnerStatus } from '../types/robotControl';
import { manualPneumaticValves, writeManualPneumaticValves } from './pneumaticManual';
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
  return status;
}

describe('manual pneumatic outputs', () => {
  it('resolves the Applied valves by signal id, false writes first', () => {
    const status = mapped();
    expect(manualPneumaticValves(status, 0.04).map((row) => [row.signalId, row.value]))
      .toEqual([['a_io/open_valve', false], ['z_io/close_valve', true]]);
    expect(manualPneumaticValves(status, 0.02).map((row) => [row.signalId, row.value]))
      .toEqual([['z_io/close_valve', false], ['a_io/open_valve', true]]);
    const logical = buildGripperCommandIntent(status, 0.02, 'logical');
    expect(logical.participants[0].payload).toMatchObject({ command_value: {
      kind: 'gripper_position', position: { kind: 'linear_m', value_m: 0 },
    } });
    status.directControlGripper!.targetDescriptor.namedPresets[0].masterPosition = 0;
    expect(() => manualPneumaticValves(status, 0)).toThrow('different positions');
  });

  it('writes each valve through the one output write path, one after the other', async () => {
    const events: string[] = [];
    const setOutput = vi.fn(async (signalId: string, value: boolean): Promise<SetOutputResult> => {
      events.push(`${signalId}=${value}`);
      return { outcome: 'succeeded' };
    });
    await writeManualPneumaticValves(mapped(), 0.04, { checkCurrent: () => events.push('check'), setOutput });
    expect(events).toEqual(['check', 'a_io/open_valve=false', 'check', 'z_io/close_valve=true']);
  });

  it.each([
    ['failed', 'Valve write a_io/open_valve failed'],
    ['unknown', 'Valve write a_io/open_valve outcome unknown (not resent)'],
  ] as const)('stops on a %s write with its code and never resends it', async (outcome, text) => {
    const setOutput = vi.fn(async (): Promise<SetOutputResult> => ({
      outcome, error: { code: 'io_output_write_failed', message: 'device refused' },
    }));
    const failure = writeManualPneumaticValves(mapped(), 0.04, { checkCurrent: vi.fn(), setOutput });
    await expect(failure).rejects.toThrow(text);
    await expect(writeManualPneumaticValves(mapped(), 0.04, { checkCurrent: vi.fn(), setOutput }))
      .rejects.toThrow('io_output_write_failed');
    expect(setOutput).toHaveBeenCalledTimes(2);
  });

  it('sends nothing once the Deployment changed', async () => {
    const setOutput = vi.fn();
    await expect(writeManualPneumaticValves(mapped(), 0.04, {
      checkCurrent: () => { throw new Error('Deployment changed'); }, setOutput,
    })).rejects.toThrow('Deployment changed');
    expect(setOutput).not.toHaveBeenCalled();
  });
});
