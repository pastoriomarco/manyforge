import { describe, expect, it } from 'vitest';

import type { CycleRecoveryOffer } from '../types/api';
import type { OperationObservation } from '../types/robotControl';
import {
  acknowledgedUncertainOutputs,
  allUncertainOutputsApproved,
  mergeUncertainOutputs,
  programRunUncertainOutputs,
  scenarioCloseUncertainOutputRefusal,
  uncertainOutputKey,
  uncertainOutputs,
  UNCERTAIN_OUTPUT_WARNING,
} from './uncertainOutputs';

const operation = (id: string, revision: number, patch: Partial<OperationObservation>): OperationObservation => ({
  operationId: id, operationRevision: revision, resident: true, state: 'failed', progress: 1,
  activeStopId: null, activeStopGeneration: null, terminalError: null, ...patch,
});

describe('uncertain outputs (contract 66)', () => {
  const observed = [
    operation('op-b', 3, { outputEvidence: 'acceptance_unknown', controllableComponentId: 'station', commandTargetId: 'io' }),
    operation('op-a', 2, { outputEvidence: 'acceptance_unknown' }),
    operation('op-c', 1, { outputEvidence: 'source_accepted' }),
    operation('op-d', 1, { outputEvidence: 'acceptance_unknown', resident: false }),
  ];

  it('lists only retained acceptance_unknown outputs, sorted', () => {
    expect(uncertainOutputs(observed)).toEqual([
      { operationId: 'op-a', operationRevision: 2, label: 'op-a' },
      { operationId: 'op-b', operationRevision: 3, label: 'station · io (op-b)' },
    ]);
    expect(uncertainOutputs(null)).toEqual([]);
  });

  it('acknowledges only the targeted operation’s own approved outputs', () => {
    const outputs = uncertainOutputs(observed);
    expect(acknowledgedUncertainOutputs('op-b', outputs)).toEqual([{ operation_id: 'op-b', operation_revision: 3 }]);
    expect(acknowledgedUncertainOutputs('op-c', outputs)).toBeNull();
    expect(allUncertainOutputsApproved(outputs, new Set([uncertainOutputKey(outputs[0])]))).toBe(false);
    expect(allUncertainOutputsApproved(outputs, new Set(outputs.map(uncertainOutputKey)))).toBe(true);
  });

  it('refuses Scenario Close with the same guidance while they are retained', () => {
    expect(scenarioCloseUncertainOutputRefusal([])).toBeNull();
    const message = scenarioCloseUncertainOutputRefusal(uncertainOutputs(observed));
    expect(message).toContain('Close never acknowledges');
    expect(message).toContain('op-a');
    expect(message).toContain(UNCERTAIN_OUTPUT_WARNING.toLowerCase());
    // Item 37: the stuck case names the operator's way out.
    expect(message).toContain('use Force close');
  });
});

describe('Program-run uncertain outputs (contract 66)', () => {
  const offer = (operationId: string | null, operationRevision: number | null, participants: unknown[]) => (
    { operationId, operationRevision, participants } as unknown as CycleRecoveryOffer
  );

  it('lists each held operation whose participant output_evidence is acceptance_unknown', () => {
    const outputs = programRunUncertainOutputs([
      offer('op-p', 4, [{ output_evidence: 'acceptance_unknown', controllable_component_id: 'cell_io', command_target_id: 'valve' }]),
      offer('op-q', 1, [{ output_evidence: 'source_accepted' }]),
      offer('op-r', 2, [{}]),
      offer(null, null, [{ output_evidence: 'acceptance_unknown' }]),
      offer('op-o', 7, [{ output_evidence: 'acceptance_unknown' }]),
      null,
      // The focused offer repeats a hold: listed once.
      offer('op-p', 4, [{ output_evidence: 'acceptance_unknown' }]),
    ]);
    expect(outputs).toEqual([
      { operationId: 'op-o', operationRevision: 7, label: 'op-o' },
      { operationId: 'op-p', operationRevision: 4, label: 'cell_io · valve (op-p)' },
    ]);
    expect(acknowledgedUncertainOutputs('op-p', outputs)).toEqual([{ operation_id: 'op-p', operation_revision: 4 }]);
  });

  it('merges manual and Program-run outputs into one sorted, unique list', () => {
    const manual = uncertainOutputs([operation('op-z', 1, { outputEvidence: 'acceptance_unknown' })]);
    const program = programRunUncertainOutputs([offer('op-a', 2, [{ output_evidence: 'acceptance_unknown' }])]);
    expect(mergeUncertainOutputs(manual, program, program).map((output) => output.operationId)).toEqual(['op-a', 'op-z']);
  });
});
