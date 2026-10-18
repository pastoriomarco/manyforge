import type { CatalogJsonSchema, JsonValue, ProgramBindingDocument } from '../types/api';
import type { ProgramSemanticChoice } from './programAuthoring';

/** The executable io nodes that read one input signal. */
export const IO_INPUT_NODE_IDS: ReadonlySet<string> = new Set([
  'io.get_input',
  'io.check_input',
  'io.wait_for_input',
]);

export const IO_EDGE_UNSUPPORTED_HINT = 'Edge waits are not supported yet; only level waits run.';

/**
 * The signals one io node can address: the `signal_role_bindings` of its
 * target in the current binding, shown with signal name and direction. Input
 * nodes get the inputs; `io.set_output` gets the outputs. An input node is
 * never offered an output: reading one needs a declared readback input. Null
 * when the binding does not know the node's target (no binding, no target
 * chosen, or the target is not bound), so the ordinary resource-role picker
 * applies.
 */
export function ioSignalRoleChoices(
  nodeId: string,
  targetRole: JsonValue | undefined,
  binding: ProgramBindingDocument | null | undefined,
): ProgramSemanticChoice[] | null {
  const direction = IO_INPUT_NODE_IDS.has(nodeId) ? 'input'
    : nodeId === 'io.set_output' ? 'output' : null;
  if (direction === null || binding == null || typeof targetRole !== 'string') {
    return null;
  }
  const target = binding.target_role_bindings.find((row) => row.target_role_id === targetRole);
  if (target === undefined) return null;
  return (target.signal_role_bindings ?? [])
    .filter((row) => row.direction === direction)
    .map((row) => ({
      value: row.resource_role_id,
      label: `${row.resource_role_id} · ${row.signal_name} (${direction})`,
      group: `${targetRole} ${direction} signals`,
    }));
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
