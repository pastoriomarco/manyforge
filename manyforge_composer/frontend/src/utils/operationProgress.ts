import type { OperationObservation } from '../types/robotControl';

// The Kernel's progress rows; the stream is the one current owner's, so a
// row names no owner (execution_event_stream.v4).
export interface OperationProgressSample {
  operation_id: number;
  operation_revision: number;
  progress: number;
}

export const EMPTY_OPERATION_PROGRESS: readonly OperationProgressSample[] = [];

export function parseOperationProgress(value: unknown): readonly OperationProgressSample[] {
  if (!Array.isArray(value) || value.length > 4096) throw new Error('Invalid operation progress list.');
  const identities = new Set<number>();
  for (const row of value) {
    if (row === null || typeof row !== 'object' || Array.isArray(row)
      || Object.keys(row).sort().join(',') !== 'operation_id,operation_revision,progress'
      || !Number.isSafeInteger(row.operation_id) || row.operation_id < 1
      || !Number.isSafeInteger(row.operation_revision) || row.operation_revision < 1
      || typeof row.progress !== 'number' || !Number.isFinite(row.progress)
      || row.progress < 0 || row.progress > 1
      || identities.has(row.operation_id)) throw new Error('Invalid operation progress sample.');
    identities.add(row.operation_id);
  }
  return value;
}

export function operationProgressValue(
  operation: Pick<OperationObservation, 'operationId' | 'operationRevision' | 'state' | 'progress'>,
  samples: readonly OperationProgressSample[],
): number {
  // Terminal/Stop truth always wins, including a late progress delta.
  const sample = operation.state === 'executing' ? samples.find((row) => (
    row.operation_id === operation.operationId
    && row.operation_revision === operation.operationRevision
  )) : undefined;
  return sample === undefined ? operation.progress : Math.max(operation.progress, sample.progress);
}
