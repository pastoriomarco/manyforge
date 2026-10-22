import type { JsonValue, ProgramAuthoringDocument } from '../types/api';
import { declaredIoInputs, declaredIoOutputs } from './ioSignalConfiguration';

/**
 * Target-role resolution from the authored Draft facts, for when the
 * Deployment workspace carries no compiled Program binding (`binding: null`):
 * the Program's `target_roles.<role>.component_role`, the scenario's Program
 * role assignment (`component_roles[]`: component role → controllable
 * component, `target_roles[]`: target role → command target) and the
 * Deployment Draft's declared I/O inputs and outputs. The backend stays the authority.
 */

/** A target role resolved to its component and command target (binding-row field names). */
export interface ResolvedTargetRole {
  target_role_id: string;
  controllable_component_id: string;
  command_target_id: string;
}

/** One declared input signal of a resolved target role. */
export interface ResolvedInputSignal {
  resource_role_id: string;
  signal_name: string;
  value_type: 'bool';
}

/** One declared output signal of a resolved target role (same shape as an input). */
export type ResolvedOutputSignal = ResolvedInputSignal;

/** A resolved target role with its declared signals (binding-row field names). */
export interface ResolvedTargetRoleBinding extends ResolvedTargetRole {
  signal_role_bindings: Array<ResolvedInputSignal & { direction: 'input' | 'output' }>;
}

type ProgramRoles = Pick<ProgramAuthoringDocument, 'target_roles'>;

function object(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value)
    ? value : null;
}

/** `rows[].<key>` → `rows[].<value>` for the assignment's string rows. */
function assignmentMap(rows: JsonValue | undefined, key: string, value: string): Map<string, string> {
  const result = new Map<string, string>();
  for (const row of Array.isArray(rows) ? rows : []) {
    const record = object(row);
    const from = record?.[key];
    const to = record?.[value];
    if (typeof from === 'string' && typeof to === 'string') result.set(from, to);
  }
  return result;
}

/** Every Program target role the role assignment resolves, in Program order. */
export function resolvedTargetRoles(
  program: ProgramRoles,
  roleAssignment: Record<string, JsonValue> | null | undefined,
): ResolvedTargetRole[] {
  if (!roleAssignment) return [];
  const components = assignmentMap(roleAssignment.component_roles, 'component_role_id', 'controllable_component_id');
  const targets = assignmentMap(roleAssignment.target_roles, 'target_role_id', 'command_target_id');
  return Object.entries(program.target_roles ?? {}).flatMap(([targetRoleId, role]) => {
    const componentRole = object(role)?.component_role;
    const componentId = typeof componentRole === 'string' ? components.get(componentRole) : undefined;
    const targetId = targets.get(targetRoleId);
    return componentId === undefined || targetId === undefined ? [] : [{
      target_role_id: targetRoleId,
      controllable_component_id: componentId,
      command_target_id: targetId,
    }];
  });
}

/** The declared rows of `drafts` that belong to `target` and one of its `required_resource_roles`. */
function requiredSignals(
  program: ProgramRoles,
  target: ResolvedTargetRole,
  drafts: ReadonlyArray<{ componentId: string; targetId: string; resourceRoleId: string; signalName: string }>,
): ResolvedInputSignal[] {
  const required = object(program.target_roles?.[target.target_role_id])?.required_resource_roles;
  const roles = new Set(Array.isArray(required) ? required.filter((role) => typeof role === 'string') : []);
  return drafts
    .filter((draft) => draft.componentId === target.controllable_component_id
      && draft.targetId === target.command_target_id && roles.has(draft.resourceRoleId))
    .map((draft) => ({ resource_role_id: draft.resourceRoleId, signal_name: draft.signalName, value_type: 'bool' }));
}

/**
 * The Deployment Draft's declared inputs of one resolved target role, limited
 * to the role's `required_resource_roles`: the backend binds signal roles only
 * for those (program_io_condition_compiler.cpp), as the binding path does.
 */
export function resolvedInputSignals(
  program: ProgramRoles,
  target: ResolvedTargetRole,
  deploymentDocument: Record<string, JsonValue> | null | undefined,
): ResolvedInputSignal[] {
  return requiredSignals(program, target, declaredIoInputs(deploymentDocument ?? null));
}

/** The Deployment Draft's declared outputs of one resolved target role, limited as the inputs are. */
export function resolvedOutputSignals(
  program: ProgramRoles,
  target: ResolvedTargetRole,
  deploymentDocument: Record<string, JsonValue> | null | undefined,
): ResolvedOutputSignal[] {
  return requiredSignals(program, target, declaredIoOutputs(deploymentDocument ?? null));
}

/**
 * Binding-shaped target rows (inputs, then outputs) from the Draft facts;
 * empty when the role assignment or the Deployment Draft is unavailable.
 */
export function resolvedTargetRoleBindings(
  program: ProgramRoles,
  roleAssignment: Record<string, JsonValue> | null | undefined,
  deploymentDocument: Record<string, JsonValue> | null | undefined,
): ResolvedTargetRoleBinding[] {
  if (!deploymentDocument) return [];
  // Parse the Deployment once, then filter per target.
  const inputs = declaredIoInputs(deploymentDocument);
  const outputs = declaredIoOutputs(deploymentDocument);
  return resolvedTargetRoles(program, roleAssignment).map((target) => ({
    ...target,
    signal_role_bindings: [
      ...requiredSignals(program, target, inputs).map((row) => ({ ...row, direction: 'input' as const })),
      ...requiredSignals(program, target, outputs).map((row) => ({ ...row, direction: 'output' as const })),
    ],
  }));
}
