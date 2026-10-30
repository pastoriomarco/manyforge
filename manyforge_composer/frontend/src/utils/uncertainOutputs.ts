import type { CycleRecoveryOffer } from '../types/api';
import type { OperationObservation } from '../types/robotControl';

/**
 * Retained outputs whose write ended acceptance-unknown (contract 66): they
 * stay blocked until their I/O device is re-synced (Disable then Enable) and
 * the operator explicitly acknowledges each one on a targeted operation Reset.
 */
export const UNCERTAIN_OUTPUT_WARNING = 'A write already queued in the remote device may still act.';

/**
 * I/O output family (contract 53): only `acceptance_unknown` leaves it open
 * whether the source acted; the other evidence values are settled facts.
 */
export function outputMayHaveActed(evidence: unknown): boolean {
  return evidence === 'acceptance_unknown';
}

export interface UncertainOutput {
  operationId: string;
  operationRevision: number;
  label: string;
}

/** The retained acceptance_unknown outputs among the observed operations. */
export function uncertainOutputs(
  operations: readonly OperationObservation[] | null | undefined,
): UncertainOutput[] {
  return (operations ?? [])
    .filter((operation) => operation.resident && outputMayHaveActed(operation.outputEvidence))
    .map((operation) => ({
      operationId: operation.operationId,
      operationRevision: operation.operationRevision,
      label: operation.controllableComponentId !== undefined && operation.commandTargetId !== undefined
        ? `${operation.controllableComponentId} · ${operation.commandTargetId} (${operation.operationId})`
        : operation.operationId,
    }))
    .sort((left, right) => (left.operationId < right.operationId ? -1
      : left.operationId > right.operationId ? 1 : left.operationRevision - right.operationRevision));
}

const record = (value: unknown): Record<string, unknown> | null => (
  typeof value === 'object' && value !== null && !Array.isArray(value) ? value as Record<string, unknown> : null
);
const text = (value: unknown): string | undefined => (typeof value === 'string' && value !== '' ? value : undefined);

/**
 * The retained acceptance_unknown outputs of Program-run operations: the held
 * operation of each recovery offer (the owner's operation projection, with
 * its per-participant output_evidence). An offer exists only while its
 * operation is retained.
 */
export function programRunUncertainOutputs(
  offers: readonly (CycleRecoveryOffer | null | undefined)[],
): UncertainOutput[] {
  const rows: UncertainOutput[] = [];
  for (const offer of offers) {
    if (offer == null || offer.operationId === null || offer.operationRevision === null) continue;
    const participant = offer.participants
      .map(record)
      .find((row) => row !== null && outputMayHaveActed(row.output_evidence));
    if (participant == null) continue;
    const component = text(participant.controllable_component_id);
    const target = text(participant.command_target_id);
    rows.push({
      operationId: offer.operationId,
      operationRevision: offer.operationRevision,
      label: component !== undefined && target !== undefined
        ? `${component} · ${target} (${offer.operationId})`
        : offer.operationId,
    });
  }
  return mergeUncertainOutputs(rows);
}

/** Unique (operation, revision) outputs from several feeds, sorted. */
export function mergeUncertainOutputs(...feeds: readonly (readonly UncertainOutput[])[]): UncertainOutput[] {
  const unique = new Map<string, UncertainOutput>();
  for (const output of feeds.flat()) {
    if (!unique.has(uncertainOutputKey(output))) unique.set(uncertainOutputKey(output), output);
  }
  return [...unique.values()].sort((left, right) => (left.operationId < right.operationId ? -1
    : left.operationId > right.operationId ? 1 : left.operationRevision - right.operationRevision));
}

export function uncertainOutputKey(output: Pick<UncertainOutput, 'operationId' | 'operationRevision'>): string {
  return `${output.operationId}\u0000${output.operationRevision}`;
}

/**
 * The `acknowledged_uncertain_outputs` of one targeted operation Reset: only
 * that operation's own approved outputs, sorted and unique; null when none.
 */
export function acknowledgedUncertainOutputs(
  operationId: string,
  approved: readonly Pick<UncertainOutput, 'operationId' | 'operationRevision'>[],
): { operation_id: string; operation_revision: number }[] | null {
  const rows = [...new Map(approved
    .filter((output) => output.operationId === operationId)
    .map((output) => [uncertainOutputKey(output), output])).values()]
    .sort((left, right) => left.operationRevision - right.operationRevision)
    .map((output) => ({ operation_id: output.operationId, operation_revision: output.operationRevision }));
  return rows.length === 0 ? null : rows;
}

/** Why Scenario Close is refused while uncertain outputs are retained, or null. */
export function scenarioCloseUncertainOutputRefusal(outputs: readonly UncertainOutput[]): string | null {
  if (outputs.length === 0) return null;
  return `Output write(s) with unknown acceptance are retained: ${outputs.map((output) => output.label).join(', ')}. `
    + 'Close never acknowledges them. Disable and Enable the I/O device, then Reset each operation and confirm each output '
    + `(${UNCERTAIN_OUTPUT_WARNING.toLowerCase()}), and Close again. If the device cannot be re-synced, use Force close.`;
}

export function allUncertainOutputsApproved(
  outputs: readonly UncertainOutput[],
  approved: ReadonlySet<string>,
): boolean {
  return outputs.every((output) => approved.has(uncertainOutputKey(output)));
}
