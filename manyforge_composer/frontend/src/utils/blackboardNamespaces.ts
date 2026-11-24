/**
 * Blackboard keys grouped by namespace: the prefix before the first `.` of a
 * key (the Program key grammar); keys without one form the root group. The
 * reserved `node_outcome` namespace holds the system-owned node outcome keys.
 */
export const NODE_OUTCOME_NAMESPACE = 'node_outcome';

/** A node outcome key's start value: no result yet for the current attempt. */
export const NODE_OUTCOME_NONE = 'none';

const ENDED_OUTCOMES = ['completed', 'failed', 'preempted', 'superseded', 'refused_by_priority'] as const;

/** Every value of a plain motion node's outcome key, `none` last. */
export const NODE_OUTCOME_VALUES = [...ENDED_OUTCOMES, NODE_OUTCOME_NONE] as const;

/** Every value of an interrupt motion node's outcome key. */
export const INTERRUPT_NODE_OUTCOME_VALUES = [
  ...ENDED_OUTCOMES, 'signal_trip', 'no_signal_trip', NODE_OUTCOME_NONE,
] as const;

/**
 * The reserved namespace of the always-current global-rule state keys
 * `interrupt_state.<rule_id>`: system-owned, read-only.
 */
export const INTERRUPT_STATE_NAMESPACE = 'interrupt_state';

export const INTERRUPT_STATE_VALUES = ['inactive', 'clear', 'tripped'] as const;

/**
 * The reserved namespace of the motion group state keys
 * `motion_group_state.<group_id>` (every execution group and `all`):
 * system-owned, read-only, the group's state in the run's runtime controls.
 */
export const MOTION_GROUP_STATE_NAMESPACE = 'motion_group_state';

/** Every value, as the runtime controls' group state (`unavailable` = no current Run). */
export const MOTION_GROUP_STATE_VALUES = [
  'ready', 'running', 'stopping', 'held', 'mixed', 'faulted', 'unavailable',
] as const;

/** The system namespaces: never a user key, namespace or Program parameter name. */
export const SYSTEM_NAMESPACES: readonly string[] = [
  NODE_OUTCOME_NAMESPACE, INTERRUPT_STATE_NAMESPACE, MOTION_GROUP_STATE_NAMESPACE,
];

/** The system namespace a name is or is under, or null. */
export function systemNamespaceOf(name: string): string | null {
  const head = name.trim().split('.', 1)[0];
  return SYSTEM_NAMESPACES.includes(head) ? head : null;
}

/** True for a system-owned key (`node_outcome.*`, `interrupt_state.*`, `motion_group_state.*`). */
export function isSystemKey(key: string): boolean {
  return key.includes('.') && systemNamespaceOf(key) !== null;
}

/** The refusal of a user name that uses a system namespace, or null. */
export function reservedNameProblem(name: string, what: string): string | null {
  const namespace = systemNamespaceOf(name);
  return namespace === null ? null
    : `${what} cannot use ${namespace}: it is reserved for the system-owned ${namespaceLabel(namespace).toLowerCase()}.`;
}

export interface KeyNamespaceGroup<T> {
  /** The namespace, or '' for keys without one. */
  id: string;
  label: string;
  items: T[];
}

export function keyNamespace(key: string): string {
  const dot = key.indexOf('.');
  return dot < 0 ? '' : key.slice(0, dot);
}

export function namespaceLabel(namespace: string): string {
  if (namespace === NODE_OUTCOME_NAMESPACE) return 'Node outcomes';
  if (namespace === INTERRUPT_STATE_NAMESPACE) return 'Interrupt states';
  if (namespace === MOTION_GROUP_STATE_NAMESPACE) return 'Motion group states';
  return namespace === '' ? 'No namespace' : namespace;
}

/** Groups in display order: root, then namespaces by ID, the system namespaces last. */
export function groupByNamespace<T>(
  items: readonly T[],
  keyOf: (item: T) => string,
): KeyNamespaceGroup<T>[] {
  const groups = new Map<string, T[]>();
  for (const item of items) {
    const namespace = keyNamespace(keyOf(item));
    groups.set(namespace, [...(groups.get(namespace) ?? []), item]);
  }
  const rank = (id: string) => (id === '' ? 0
    : SYSTEM_NAMESPACES.includes(id) ? 2 : 1);
  return [...groups.entries()]
    .sort(([left], [right]) => rank(left) - rank(right) || (left < right ? -1 : left > right ? 1 : 0))
    .map(([id, rows]) => ({ id, label: namespaceLabel(id), items: rows }));
}
