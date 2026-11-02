import type { JsonValue } from '../types/api';

export const CARRIED_OBJECT_LOST_RESOURCE_ROLE = 'carried_object_lost';
export const CARRIED_OBJECT_LOST_SIGNAL_PREFIX = 'carried_object_lost.';

export interface CarriedObjectLostSignalChoice {
  controllableComponentId: string;
  commandTargetId: string;
  signalName: string;
  resourceRoleId: typeof CARRIED_OBJECT_LOST_RESOURCE_ROLE;
  direction: 'input';
  valueType: 'bool';
  /** Applied v2 owners remain candidates while the Draft is being edited. */
  provenance?: 'last_apply';
}

export interface SystemSignalOwnerIdentity {
  graphRevision: string;
  retainedBundleId: string;
}

export interface DraftSystemSignalRows {
  /** True when the Draft contains enough compiled identity to derive this table exactly. */
  deterministic: boolean;
  rows: CarriedObjectLostSignalChoice[];
}

export interface SystemSignalChoiceResolution {
  choices: CarriedObjectLostSignalChoice[];
  needApply: boolean;
}

export function systemSignalOriginSuffix(
  signal: Pick<CarriedObjectLostSignalChoice, 'provenance'> | undefined,
): string {
  return signal?.provenance === 'last_apply' ? ' · from last Apply' : '';
}

function objectOf(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? value : null;
}

function identity(value: JsonValue | undefined): string | null {
  return typeof value === 'string' && value.trim() !== '' ? value : null;
}

function compareText(left: string, right: string): number {
  return left < right ? -1 : left > right ? 1 : 0;
}

function canonicalRows(
  pairs: ReadonlyArray<{ controllableComponentId: string; commandTargetId: string }>,
): CarriedObjectLostSignalChoice[] {
  const unique = new Map<string, { controllableComponentId: string; commandTargetId: string }>();
  for (const pair of pairs) {
    unique.set(JSON.stringify([pair.controllableComponentId, pair.commandTargetId]), pair);
  }
  return [...unique.values()]
    .sort((left, right) => compareText(left.controllableComponentId, right.controllableComponentId)
      || compareText(left.commandTargetId, right.commandTargetId))
    .map(({ controllableComponentId, commandTargetId }): CarriedObjectLostSignalChoice => ({
      controllableComponentId,
      commandTargetId,
      signalName: `${CARRIED_OBJECT_LOST_SIGNAL_PREFIX}${commandTargetId}`,
      resourceRoleId: CARRIED_OBJECT_LOST_RESOURCE_ROLE,
      direction: 'input',
      valueType: 'bool',
    }));
}

/**
 * The compiled `manyforge.deployment.v1` Draft carries the exact command
 * capability rows used by the backend catalog. Source-referenced
 * `manyforge.deployment_intent.v2` Drafts do not carry the resolved finger
 * joints needed by `has_gripper_target()`, so their gripper configuration rows
 * alone are not enough to derive the generated signals safely.
 */
export function draftCarriedObjectLostSignals(
  document: Record<string, JsonValue> | null,
): DraftSystemSignalRows {
  if (document?.api_version !== 'manyforge.deployment.v1') {
    return { deterministic: false, rows: [] };
  }
  const graph = objectOf(document?.graph);
  const bindings = graph?.control_bindings;
  if (!Array.isArray(bindings)) return { deterministic: false, rows: [] };

  const pairs = bindings.flatMap((value) => {
    const binding = objectOf(value);
    if (binding?.capability_id !== 'tool.gripper.command') return [];
    const controllableComponentId = identity(binding.controllable_component_id);
    const commandTargetId = identity(binding.command_target_id);
    return controllableComponentId === null || commandTargetId === null
      ? [] : [{ controllableComponentId, commandTargetId }];
  });
  return { deterministic: true, rows: canonicalRows(pairs) };
}

/**
 * v1 has exact Draft bindings. v2 uses the last Applied projection during
 * Draft edits; unknown versions require Draft=Applied. Every Applied source
 * must still match the current owner's graph and retained bundle.
 */
export function resolveCarriedObjectLostSignalChoices(
  draft: DraftSystemSignalRows,
  appliedRows: readonly CarriedObjectLostSignalChoice[],
  draftApiVersion: string | null,
  draftMatchesApplied: boolean,
  currentOwner: SystemSignalOwnerIdentity | null,
  appliedOwner: SystemSignalOwnerIdentity | null,
): SystemSignalChoiceResolution {
  if (draft.deterministic) return { choices: draft.rows, needApply: false };
  const ownerMatches = currentOwner !== null && appliedOwner !== null
    && currentOwner.graphRevision === appliedOwner.graphRevision
    && currentOwner.retainedBundleId === appliedOwner.retainedBundleId;
  const mayUseAppliedRows = draftApiVersion === 'manyforge.deployment_intent.v2' || draftMatchesApplied;
  if (!ownerMatches || !mayUseAppliedRows) return { choices: [], needApply: true };
  return {
    choices: appliedRows.map((row): CarriedObjectLostSignalChoice => ({ ...row, provenance: 'last_apply' })),
    needApply: false,
  };
}
