import { expect, it } from 'vitest';
import { operationProgressValue, parseOperationProgress } from './operationProgress';

const operation = { operationId: 'op.1', operationRevision: 2, state: 'executing' as const, progress: 0.1 };
const sample = { owner_epoch: 'owner.1', operation_id: 'op.1', operation_revision: 2, progress: 0.6 };

it('advances only the exact active operation and leaves terminal state authoritative', () => {
  expect(operationProgressValue(operation, 'owner.1', [sample])).toBe(0.6);
  for (const patch of [{ owner_epoch: 'old' }, { operation_id: 'old' }, { operation_revision: 1 }]) {
    expect(operationProgressValue(operation, 'owner.1', [{ ...sample, ...patch }])).toBe(0.1);
  }
  expect(operationProgressValue({ ...operation, state: 'completed', progress: 1 }, 'owner.1', [sample])).toBe(1);
  expect(operationProgressValue({ ...operation, state: 'stopping' }, 'owner.1', [sample])).toBe(0.1);
});

it('strictly parses bounded progress samples', () => {
  expect(parseOperationProgress([sample])).toEqual([sample]);
  for (const value of [[{ ...sample, extra: true }], [{ ...sample, progress: NaN }], [sample, sample], [{ ...sample, operation_revision: 0 }]]) {
    expect(() => parseOperationProgress(value)).toThrow();
  }
});
