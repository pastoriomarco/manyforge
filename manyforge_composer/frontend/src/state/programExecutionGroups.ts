import type {
  AppliedProgramSelection,
  ProgramExecutionSelectionControls,
  ProgramTreeNode,
} from '../types/api';
import type { RuntimeScaleControlSelector } from '../types/robotControl';


type ProgramExecutionSelectionRow = ProgramExecutionSelectionControls['groups'][number];

/** Owner-confirmed quiescence, including a mixture of Ready and Stopped work. */
export function executionSelectionIsQuiescent(
  group: Pick<ProgramExecutionSelectionRow, 'state' | 'nodeCount' | 'heldNodeCount'> | undefined,
): boolean {
  return group !== undefined && (group.state === 'ready' || group.state === 'held'
    || (group.state === 'mixed' && group.nodeCount > 0
      && group.heldNodeCount === group.nodeCount));
}

/**
 * Hardware-acting nodes: the single rule for visible-control coverage.
 *
 * A node acts on hardware when its node-catalog entry either
 * - has an `effectful` capability requirement and issues a new command through
 *   a robot, tool or I/O owner: owner API `kernel.operation` (robot motion) or
 *   `kernel.command_target` (gripper and I/O commands); or
 * - controls robot operations through a Kernel control owner (stop, Full
 *   Stop, reset, runtime scale): these change what the robots
 *   do, so they need a visible control just like a move (maintainer,
 *   2026-10-02).
 * Gripper/I/O reads and waits are not hardware-acting.
 * `programExecutionGroups.test.ts` checks this set against the built-in catalog.
 */
export const HARDWARE_ACTING_NODE_IDS: ReadonlySet<string> = new Set([
  'gripper.command_position',
  'io.set_output',
  'kernel.full_stop',
  'kernel.reset_operation',
  'kernel.set_runtime_scale',
  'kernel.stop_operation',
  'motion.follow_cartesian_path',
  'motion.move_to_joint_target',
  'motion.move_to_pose',
  'motion.move_until_signal',
  'motion.probe_search',
  'motion.set_tcp',
]);

export function isHardwareActingNode(nodeId: string): boolean {
  return HARDWARE_ACTING_NODE_IDS.has(nodeId);
}

/** Canonical (code-point) order, as group IDs are serialized. */
function compareIds(left: string, right: string): number {
  return left < right ? -1 : left > right ? 1 : 0;
}

export interface ExecutionGroupScope {
  groupId: string;
  nodeIds: string[];
  /** Members that act on hardware (see `HARDWARE_ACTING_NODE_IDS`). */
  hardwareNodeIds: string[];
  /** Direct target keys of the hardware-acting members. */
  targetKeys: string[];
  /** Direct target keys of motion members only: runtime velocity scaling. */
  motionTargetKeys: string[];
}

/** Presentation-only membership shared by Draft settings and Applied controls. */
export function executionGroupScopes(
  roots: readonly ProgramTreeNode[],
  declaredGroups: readonly string[],
): ExecutionGroupScope[] {
  const scopes = new Map(['all', ...declaredGroups].map((groupId) => [groupId, {
    groupId,
    nodeIds: [] as string[],
    hardwareNodeIds: [] as string[],
    targetKeys: new Set<string>(),
    motionTargetKeys: new Set<string>(),
  }]));
  const pending = roots.map((node) => ({ node, inherited: [] as string[] }));
  while (pending.length > 0) {
    const { node, inherited } = pending.pop()!;
    const groups = node.execution_groups === 'all' ? []
      : Array.isArray(node.execution_groups) ? node.execution_groups : inherited;
    const hardware = isHardwareActingNode(node.node_id);
    const target = node.parameters.target;
    const targetKey = hardware && target !== null && typeof target === 'object' && !Array.isArray(target)
      && typeof target.component_id === 'string' && target.component_id.length > 0
      && typeof target.command_target_id === 'string' && target.command_target_id.length > 0
      ? `${target.component_id}/${target.command_target_id}` : null;
    for (const groupId of ['all', ...groups]) {
      const scope = scopes.get(groupId);
      if (scope === undefined) continue; // Undeclared Draft memberships remain backend diagnostics.
      scope.nodeIds.push(node.instance_id);
      if (!hardware) continue;
      scope.hardwareNodeIds.push(node.instance_id);
      if (targetKey === null) continue;
      scope.targetKeys.add(targetKey);
      // Set TCP changes controller state but does not move the robot.
      if (node.node_id.startsWith('motion.') && node.node_id !== 'motion.set_tcp') {
        scope.motionTargetKeys.add(targetKey);
      }
    }
    for (const child of node.children) pending.push({ node: child, inherited: groups });
  }
  return [...scopes.values()].sort((a, b) => compareIds(a.groupId, b.groupId))
    .map((scope) => ({
      ...scope,
      targetKeys: [...scope.targetKeys].sort(compareIds),
      motionTargetKeys: [...scope.motionTargetKeys].sort(compareIds),
    }));
}

export function executionGroupVisibilityUnavailableReason(
  scopes: readonly ExecutionGroupScope[],
  shown: readonly string[],
): string | null {
  if (shown.length === 0) return 'Keep at least one control group visible.';
  if (shown.includes('all')) return null;
  const all = scopes.find((scope) => scope.groupId === 'all');
  if (all === undefined) return 'Show All until current Program membership is available.';
  const visible = new Set(shown);
  const covered = new Set(scopes.filter((scope) => visible.has(scope.groupId))
    .flatMap((scope) => scope.hardwareNodeIds));
  return all.hardwareNodeIds.some((id) => !covered.has(id))
    ? 'Show All, or another covering group, before hiding this group.' : null;
}

function runtimeScaleSelector(
  applied: AppliedProgramSelection,
  targetKeys: Set<string>,
): { selector: RuntimeScaleControlSelector | null; reason: string | null } {
  if (targetKeys.size === 0) {
    return { selector: null, reason: 'selection_has_no_motion_targets' };
  }
  const bindings = new Map(applied.programBinding.target_bindings.map((binding) => (
    [`${binding.target.component_id}/${binding.target.command_target_id}`, binding.target]
  )));
  const targets = [...targetKeys].sort().map((targetKey) => {
    const binding = bindings.get(targetKey);
    return binding === undefined ? null : {
      componentId: binding.component_id,
      commandTargetId: binding.command_target_id,
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
  return executionGroupScopes([applied.program.tree], Object.keys(applied.program.execution_groups ?? {}))
    .map(({ groupId, nodeIds, motionTargetKeys }) => {
      const scale = runtimeScaleSelector(applied, new Set(motionTargetKeys));
      return {
        groupId,
        nodeCount: nodeIds.length,
        heldNodeCount: 0,
        state: 'unavailable',
        stopAllowed: false,
        startAllowed: false,
        startUnavailableReason: 'program_run_not_active',
        resumeAllowed: false,
        resumeUnavailableReason: 'program_run_not_active',
        runtimeScaleSelector: scale.selector,
        runtimeScaleUnavailableReason: scale.reason,
        // No run reserves anything yet; release is unavailable regardless.
        resourceIds: [],
      } satisfies ProgramExecutionSelectionRow;
    });
}

/**
 * Why All is shown although the visibility preference hides it:
 * `uncovered` — the preferred groups leave a hardware-acting node without a
 * visible control; `empty` — the preference shows nothing; and
 * `membership_unavailable` — membership cannot be matched (for example the
 * current run was captured from another revision than the Applied Program).
 */
export type ExecutionGroupFallback = 'uncovered' | 'empty' | 'membership_unavailable';

export interface ExecutionGroupDisplay {
  /** The saved choice (or the default without one); never the automatic fallback. */
  preferred: string[];
  /** What the Motion panel shows: the preference, plus All when needed. */
  shown: string[];
  fallback: ExecutionGroupFallback | null;
}

/**
 * Groups the Motion panel shows. Without a Program choice, named groups are
 * preferred. Whenever the preference leaves a hardware-acting node uncovered
 * (or membership is unknown), All is also shown, without rewriting the saved
 * preference. This never changes runtime admission.
 */
export function executionGroupDisplay(
  groupIds: readonly string[],
  hidden: readonly string[] | undefined,
  scopes: readonly ExecutionGroupScope[],
): ExecutionGroupDisplay {
  const named = groupIds.filter((groupId) => groupId !== 'all');
  const preferred = hidden === undefined
    ? (named.length > 0 ? named : groupIds.filter((groupId) => groupId === 'all'))
    : groupIds.filter((groupId) => !hidden.includes(groupId));
  if (!groupIds.includes('all')
      || executionGroupVisibilityUnavailableReason(scopes, preferred) === null) {
    return { preferred, shown: preferred, fallback: null };
  }
  return {
    preferred,
    shown: groupIds.filter((groupId) => groupId === 'all' || preferred.includes(groupId)),
    fallback: !scopes.some((scope) => scope.groupId === 'all') ? 'membership_unavailable'
      : preferred.length === 0 ? 'empty' : 'uncovered',
  };
}

export function shownExecutionGroupIds(
  groupIds: readonly string[],
  hidden: readonly string[] | undefined,
  scopes: readonly ExecutionGroupScope[],
): string[] {
  return executionGroupDisplay(groupIds, hidden, scopes).shown;
}

/**
 * The saved hidden list after the user shows or hides one group. Only that
 * group's preference changes: an automatically revealed All is never
 * persisted, and the saved All choice changes only when All itself is
 * toggled. A hide that would leave a hardware-acting node without a
 * preferred covering group is refused with its reason.
 */
export function executionGroupVisibilityChange(
  groupIds: readonly string[],
  hidden: readonly string[] | undefined,
  scopes: readonly ExecutionGroupScope[],
  groupId: string,
  visible: boolean,
): { hidden: string[] } | { refused: string } {
  const { preferred } = executionGroupDisplay(groupIds, hidden, scopes);
  const next = visible
    ? [...new Set([...preferred, groupId])]
    : preferred.filter((id) => id !== groupId);
  const reason = visible ? null : executionGroupVisibilityUnavailableReason(scopes, next);
  return reason !== null ? { refused: reason }
    : { hidden: groupIds.filter((id) => !next.includes(id)) };
}
