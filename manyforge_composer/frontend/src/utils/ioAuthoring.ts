import type { CatalogJsonSchema, JsonValue, ProgramSignalReference } from '../types/api';
import { CHANGED_OUTSIDE_SUFFIX } from '../types/components';
import type { CandidateSignalFact } from './interruptAuthoring';

/** Clears an output's core-held `changed outside` signal. */
export const IO_CLEAR_OUTSIDE_CHANGE_NODE_ID = 'io.clear_outside_change';
export const IO_INPUT_NODE_IDS: ReadonlySet<string> = new Set([
  'io.get_input', 'io.check_input', 'io.wait_for_input',
]);
export const IO_EDGE_UNSUPPORTED_HINT = 'Edge waits are not supported yet; only level waits run.';

export interface ProgramSignalChoice {
  value: ProgramSignalReference;
  label: string;
  group: string;
  disabled?: boolean;
  reason?: string;
}

/** Exact signal identities from the generation-checked candidate; no Applied fallback. */
export function ioSignalChoices(
  nodeId: string,
  candidateSignals: readonly CandidateSignalFact[] | null,
): ProgramSignalChoice[] | null {
  const direction = IO_INPUT_NODE_IDS.has(nodeId) ? 'input'
    : nodeId === 'io.set_output' || nodeId === IO_CLEAR_OUTSIDE_CHANGE_NODE_ID ? 'output' : null;
  if (direction === null || candidateSignals === null) return null;
  return candidateSignals.map((row) => {
    const outside = nodeId === IO_CLEAR_OUTSIDE_CHANGE_NODE_ID && row.direction === 'output'
      ? candidateSignals.filter((candidate) => candidate.reference.component_id === row.reference.component_id
        && candidate.reference.signal_name === `${row.reference.signal_name}${CHANGED_OUTSIDE_SUFFIX}`
        && !candidate.ambiguous && candidate.direction === 'input' && candidate.value_type === 'bool') : [];
    const reason = row.ambiguous ? 'Signal identity is ambiguous in the Deployment Draft.'
      : row.direction !== direction ? `This node requires an ${direction} signal.`
        : row.value_type !== 'bool' ? 'This node requires a boolean signal.'
          : nodeId === IO_CLEAR_OUTSIDE_CHANGE_NODE_ID && outside.length !== 1
            ? 'This output does not have a uniquely qualified changed-outside signal.' : undefined;
    return {
      value: { ...row.reference },
      label: row.reference.signal_name.endsWith(CHANGED_OUTSIDE_SUFFIX)
        ? `${row.reference.component_id} · ${row.reference.signal_name.slice(0, -CHANGED_OUTSIDE_SUFFIX.length)} → changed outside`
        : `${row.reference.component_id} · ${row.reference.signal_name} (${row.direction ?? 'unknown'})`,
      group: `${row.reference.component_id} signals`,
      ...(reason === undefined ? {} : { disabled: true, reason }),
    };
  });
}

/** The enum values validation refuses for this parameter (edge modes other than level). */
export function ioUnsupportedChoices(
  nodeId: string,
  parameterId: string,
  schema: CatalogJsonSchema,
): { values: JsonValue[]; hint: string } | undefined {
  if (nodeId !== 'io.wait_for_input' || parameterId !== 'edge' || schema.enum === undefined) {
    return undefined;
  }
  const values = schema.enum.filter((value) => value !== 'level') as JsonValue[];
  return values.length === 0 ? undefined : { values, hint: IO_EDGE_UNSUPPORTED_HINT };
}

/** Whether an empty value means "omit" for this parameter (wait forever). */
export function ioEmptyOmits(nodeId: string, parameterId: string): boolean {
  return nodeId === 'io.wait_for_input' && parameterId === 'timeout_s';
}

/** "Empty waits forever; otherwise 0.01–3600 s of Program time …". */
export function ioTimeoutHint(schema: CatalogJsonSchema): string {
  const min = schema.minimum ?? schema.exclusiveMinimum;
  const max = schema.maximum ?? schema.exclusiveMaximum;
  const range = min !== undefined && max !== undefined ? ` ${min}–${max} s` : '';
  return `Empty waits forever. Otherwise the wait fails after${range} of Program time, which stands still while the Program is halted.`;
}
