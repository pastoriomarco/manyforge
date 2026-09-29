import type {
  AppliedProgramSelection,
  ProgramExecutionSelectionControls,
  ProgramTreeNode,
} from '../types/api';
import type { RuntimeScaleControlSelector } from '../types/robotControl';


type ProgramExecutionSelectionRow = ProgramExecutionSelectionControls['groups'][number];

function runtimeScaleSelector(
  applied: AppliedProgramSelection,
  targetRoles: Set<string>,
): { selector: RuntimeScaleControlSelector | null; reason: string | null } {
  if (targetRoles.size === 0) {
    return { selector: null, reason: 'selection_has_no_motion_targets' };
  }
  const bindings = new Map(applied.programBinding.target_role_bindings.map((binding) => (
    [binding.target_role_id, binding]
  )));
  const targets = [...targetRoles].sort().map((targetRoleId) => {
    const binding = bindings.get(targetRoleId);
    return binding === undefined ? null : {
      componentRoleId: binding.component_role_id,
      targetRoleId,
    };
  });
  if (targets.some((target) => target === null)) {
    return { selector: null, reason: 'captured_motion_target_binding_unavailable' };
  }
  const resolved = targets.filter((target): target is NonNullable<typeof target> => target !== null);
  return {
    selector: resolved.length === 1
      ? {
          kind: 'singular',
          programBindingId: applied.programBindingId,
          ...resolved[0],
        }
      : {
          kind: 'group',
          programBindingId: applied.programBindingId,
          groupTargets: resolved,
        },
    reason: null,
  };
}

/**
 * Project stable group scopes from the exact Applied Program before a run.
 * Runtime owners replace these rows once a Program run exists; this projection
 * only supplies idle presentation and persistent runtime-scale addressing.
 */
export function appliedExecutionSelectionRows(
  applied: AppliedProgramSelection | null,
): ProgramExecutionSelectionRow[] {
  if (applied === null) return [];
  const members = new Map<string, Set<string>>([['all', new Set<string>()]]);
  const motionTargets = new Map<string, Set<string>>([['all', new Set<string>()]]);
  for (const groupId of Object.keys(applied.program.execution_groups ?? {})) {
    members.set(groupId, new Set<string>());
    motionTargets.set(groupId, new Set<string>());
  }
  const pending: Array<{ node: ProgramTreeNode; inherited: string[] }> = [
    { node: applied.program.tree, inherited: [] },
  ];
  while (pending.length > 0) {
    const current = pending.pop();
    if (current === undefined) break;
    const authored = current.node.execution_groups;
    const groupIds = authored === 'all'
      ? []
      : Array.isArray(authored)
        ? authored
        : current.inherited;
    const targetRole = current.node.parameters.target_role;
    const motionTargetRole = current.node.node_id.startsWith('motion.')
      && typeof targetRole === 'string' && targetRole.length > 0
      ? targetRole : null;
    members.get('all')?.add(current.node.instance_id);
    if (motionTargetRole !== null) motionTargets.get('all')?.add(motionTargetRole);
    for (const groupId of groupIds) {
      members.get(groupId)?.add(current.node.instance_id);
      if (motionTargetRole !== null) motionTargets.get(groupId)?.add(motionTargetRole);
    }
    for (const child of current.node.children) {
      pending.push({ node: child, inherited: groupIds });
    }
  }
  return [...members]
    .sort(([left], [right]) => left.localeCompare(right))
    .map(([groupId, nodeIds]) => {
      const scale = runtimeScaleSelector(applied, motionTargets.get(groupId) ?? new Set());
      return {
        groupId,
        nodeCount: nodeIds.size,
        heldNodeCount: 0,
        state: 'unavailable',
        stopAllowed: false,
        startAllowed: false,
        startUnavailableReason: 'program_run_not_active',
        resumeAllowed: false,
        resumeUnavailableReason: 'program_run_not_active',
        runtimeScaleSelector: scale.selector,
        runtimeScaleUnavailableReason: scale.reason,
      } satisfies ProgramExecutionSelectionRow;
    });
}

/**
 * Groups the Motion panel shows. Without a Program choice, named groups are
 * shown and "all" only when there are none. A choice hides the groups it
 * names; at least one group always stays shown (Halt is shown regardless).
 */
export function shownExecutionGroupIds(
  groupIds: readonly string[],
  hidden: readonly string[] | undefined,
): string[] {
  const named = groupIds.filter((groupId) => groupId !== 'all');
  const fallback = named.length > 0 ? named : groupIds.filter((groupId) => groupId === 'all');
  if (hidden === undefined) return fallback;
  const shown = groupIds.filter((groupId) => !hidden.includes(groupId));
  return shown.length > 0 ? shown : fallback;
}
