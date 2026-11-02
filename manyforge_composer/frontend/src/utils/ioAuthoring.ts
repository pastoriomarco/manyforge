import { CHANGED_OUTSIDE_SUFFIX, ioSignalId } from '../types/components';
import type {
  CatalogJsonSchema, JsonValue, ProgramAuthoringDocument, ProgramBindingDocument,
} from '../types/api';
import type { ProgramSemanticChoice } from './programAuthoring';
import { resolvedTargetRoleBindings, resolvedTargetRoles } from './targetRoleResolution';
import {
  CARRIED_OBJECT_LOST_RESOURCE_ROLE,
  systemSignalOriginSuffix,
  type CarriedObjectLostSignalChoice,
} from './systemSignalAuthoring';

/** The authored Draft facts that resolve target roles while the binding is null. */
export interface TargetRoleDraftFacts {
  program: Pick<ProgramAuthoringDocument, 'target_roles'>;
  roleAssignment: Record<string, JsonValue> | null | undefined;
  deploymentDocument: Record<string, JsonValue> | null | undefined;
}

interface IoTargetRoleRow {
  target_role_id: string;
  controllable_component_id: string;
  command_target_id: string;
  signal_role_bindings?: Array<{
    resource_role_id: string;
    signal_name: string;
    direction: 'input' | 'output';
  }>;
}

const NO_CHANGED_OUTSIDE: ReadonlySet<string> = new Set();
/** CONTRACTS §9: clears an output's core-held `changed outside` signal. */
export const IO_CLEAR_OUTSIDE_CHANGE_NODE_ID = 'io.clear_outside_change';

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
 * applies. Without a binding, `draftFacts` resolve the target and its declared
 * signals (targetRoleResolution).
 */
export function ioSignalRoleChoices(
  nodeId: string,
  targetRole: JsonValue | undefined,
  binding: ProgramBindingDocument | null | undefined,
  draftFacts?: TargetRoleDraftFacts,
  systemSignals: readonly CarriedObjectLostSignalChoice[] = [],
  changedOutside: ReadonlySet<string> = NO_CHANGED_OUTSIDE,
): ProgramSemanticChoice[] | null {
  const direction = IO_INPUT_NODE_IDS.has(nodeId) ? 'input'
    : nodeId === 'io.set_output' || nodeId === IO_CLEAR_OUTSIDE_CHANGE_NODE_ID ? 'output' : null;
  if (direction === null || typeof targetRole !== 'string') return null;
  let targets: IoTargetRoleRow[] | null = binding != null ? binding.target_role_bindings : null;
  if (targets === null && draftFacts !== undefined) {
    if (draftFacts.deploymentDocument == null) {
      // With no Deployment signal catalog, retain the old null result for
      // ordinary inputs/outputs. Only synthesize a target for this generated
      // input when the Program requires it and its current role assignment
      // exactly matches an Applied system-signal row.
      if (direction !== 'input') return null;
      const targetRows = resolvedTargetRoles(draftFacts.program, draftFacts.roleAssignment);
      const relevant = targetRows.filter((target) => {
        const role = draftFacts.program.target_roles?.[target.target_role_id];
        const required = role !== null && typeof role === 'object' && !Array.isArray(role)
          && Array.isArray(role.required_resource_roles)
          && role.required_resource_roles.includes(CARRIED_OBJECT_LOST_RESOURCE_ROLE);
        return required && systemSignals.some((signal) => (
          signal.controllableComponentId === target.controllable_component_id
          && signal.commandTargetId === target.command_target_id
        ));
      });
      if (relevant.length === 0) return null;
      targets = relevant.map((target) => ({ ...target, signal_role_bindings: [] }));
    } else {
      targets = resolvedTargetRoleBindings(
        draftFacts.program, draftFacts.roleAssignment, draftFacts.deploymentDocument,
      );
    }
  }
  if (targets === null) return null;
  const target = targets.find((row) => row.target_role_id === targetRole);
  if (target === undefined) return null;
  const systemSignal = systemSignals.find((row) => (
    row.controllableComponentId === target.controllable_component_id
    && row.commandTargetId === target.command_target_id
  ));
  const programTarget = draftFacts?.program.target_roles?.[target.target_role_id];
  const requiredRoles = programTarget !== null && typeof programTarget === 'object' && !Array.isArray(programTarget)
    ? programTarget.required_resource_roles : undefined;
  const requiresCarriedObjectLost = Array.isArray(requiredRoles)
    ? requiredRoles.includes(CARRIED_OBJECT_LOST_RESOURCE_ROLE)
    : (target.signal_role_bindings ?? []).some((row) => (
      row.resource_role_id === CARRIED_OBJECT_LOST_RESOURCE_ROLE
    ));
  // V2-7: an output's core-held `changed outside` signal, by its eligible outputs.
  const eligibleOutputs = (target.signal_role_bindings ?? []).filter((row) => row.direction === 'output'
    && changedOutside.has(ioSignalId(target.controllable_component_id, row.signal_name)));
  if (nodeId === IO_CLEAR_OUTSIDE_CHANGE_NODE_ID) {
    return eligibleOutputs.map((row) => ({
      value: row.resource_role_id,
      label: `${row.resource_role_id} · ${row.signal_name} (output)`,
      group: `${targetRole} outputs with changed outside`,
    }));
  }
  const choices = (target.signal_role_bindings ?? [])
    .filter((row) => row.direction === direction)
    .filter((row) => row.resource_role_id !== CARRIED_OBJECT_LOST_RESOURCE_ROLE
      || (direction === 'input' && requiresCarriedObjectLost && systemSignal?.signalName === row.signal_name))
    .map((row) => {
      const originSuffix = row.resource_role_id === CARRIED_OBJECT_LOST_RESOURCE_ROLE
        ? systemSignalOriginSuffix(systemSignal) : '';
      return {
        value: row.resource_role_id,
        label: `${row.resource_role_id} · ${row.signal_name} (${direction})${originSuffix}`,
        group: `${targetRole} ${direction} signals`,
      };
    });
  if (direction === 'input') {
    for (const row of eligibleOutputs) {
      choices.push({
        value: `${row.resource_role_id}${CHANGED_OUTSIDE_SUFFIX}`,
        label: `${row.resource_role_id} · ${row.signal_name} → changed outside`,
        group: `${targetRole} output signals`,
      });
    }
  }
  if (direction === 'input' && requiresCarriedObjectLost && systemSignal !== undefined
    && !choices.some((choice) => choice.value === CARRIED_OBJECT_LOST_RESOURCE_ROLE)) {
    choices.push({
      value: CARRIED_OBJECT_LOST_RESOURCE_ROLE,
      label: `${CARRIED_OBJECT_LOST_RESOURCE_ROLE} · ${systemSignal.signalName} (input)${systemSignalOriginSuffix(systemSignal)}`,
      group: `${targetRole} input signals`,
    });
  }
  return choices;
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
