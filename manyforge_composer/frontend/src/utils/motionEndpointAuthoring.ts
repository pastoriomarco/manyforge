import type { JsonValue, ProgramFrameReference, ProgramTargetReference, ProgramTreeNode } from '../types/api';

export type TcpSelector =
  | { kind: 'current' }
  | { kind: 'default_of_target'; target: ProgramTargetReference }
  | { kind: 'entry'; target: ProgramTargetReference; frame: ProgramFrameReference };

/** Qualified alternatives supplied by one generation-checked Draft candidate. */
export interface CandidateTcpEntry {
  entry: { target: ProgramTargetReference; frame: ProgramFrameReference };
  end_effector: boolean;
  component_default: boolean;
  available: boolean;
  reason: string | null;
  mounted_here: boolean;
  mounted_elsewhere: boolean;
}
export interface CandidateTcpTarget {
  target: ProgramTargetReference;
  tcp_entries: readonly CandidateTcpEntry[];
  default_tcp: { target: ProgramTargetReference; frame: ProgramFrameReference } | null;
}
export interface TcpSelectorChoice {
  key: string;
  label: string;
  selector: TcpSelector;
  disabled?: boolean;
  reason?: string;
}

function objectValue(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value) ? value : null;
}
function targetValue(value: JsonValue | undefined): ProgramTargetReference | null {
  const row = objectValue(value);
  return row !== null && Object.keys(row).length === 2 && typeof row.component_id === 'string'
    && typeof row.command_target_id === 'string' ? { component_id: row.component_id, command_target_id: row.command_target_id } : null;
}
function frameValue(value: JsonValue | undefined): ProgramFrameReference | null {
  const row = objectValue(value);
  return row !== null && Object.keys(row).length === 2 && typeof row.component_id === 'string'
    && typeof row.frame_id === 'string' ? { component_id: row.component_id, frame_id: row.frame_id } : null;
}
function sameTarget(a: ProgramTargetReference, b: ProgramTargetReference): boolean {
  return a.component_id === b.component_id && a.command_target_id === b.command_target_id;
}
export function tcpSelectorKey(selector: TcpSelector): string {
  if (selector.kind === 'current') return JSON.stringify(['current']);
  if (selector.kind === 'default_of_target') return JSON.stringify(['default_of_target', selector.target.component_id, selector.target.command_target_id]);
  return JSON.stringify(['entry', selector.target.component_id, selector.target.command_target_id,
    selector.frame.component_id, selector.frame.frame_id]);
}

/** Never infer TCP eligibility from a generic frame or an Applied-only descriptor. */
export function programTcpSelectorChoices(
  movingTarget: ProgramTargetReference | null,
  candidateTargets: readonly CandidateTcpTarget[] | null,
  showOtherRobots = false,
  /** The moving target's Active TCP right now, when known. */
  activeTcpNow: string | null = null,
): TcpSelectorChoice[] {
  const current: TcpSelectorChoice = { key: tcpSelectorKey({ kind: 'current' }),
    label: `Current: Active TCP at run time${activeTcpNow === null ? '' : ` (now ${activeTcpNow})`}`,
    selector: { kind: 'current' } };
  if (movingTarget === null || candidateTargets === null) return [current];
  const matches = candidateTargets.filter((row) => sameTarget(row.target, movingTarget));
  if (matches.length !== 1) return [current];
  const entries = matches[0].tcp_entries.filter((row) => showOtherRobots || row.mounted_elsewhere !== true);
  const choices: TcpSelectorChoice[] = [current];
  for (const owner of candidateTargets) {
    if (owner.default_tcp === null) continue;
    const routes = entries.filter((row) => sameTarget(row.entry.target, owner.default_tcp!.target)
      && row.entry.frame.component_id === owner.default_tcp!.frame.component_id
      && row.entry.frame.frame_id === owner.default_tcp!.frame.frame_id);
    const selector: TcpSelector = { kind: 'default_of_target', target: { ...owner.target } };
    const reason = routes.length === 0 ? 'This default is not qualified for the selected moving target.'
      : routes.length !== 1 ? 'This default has ambiguous TCP eligibility.'
        : !routes[0].available ? routes[0].reason ?? 'This TCP is not compatible with the selected moving target.' : undefined;
    const name = `${owner.target.component_id} · ${owner.target.command_target_id}`;
    const frame = `${owner.default_tcp.frame.component_id} / ${owner.default_tcp.frame.frame_id}`;
    // The moving target's own default is its flange-side TCP: mounted tools don't change it.
    const label = sameTarget(owner.target, movingTarget)
      ? `Own default TCP of ${name}, ignores mounted tools (${frame})` : `Default TCP of ${name} (${frame})`;
    choices.push({ key: tcpSelectorKey(selector), label,
      selector, ...(reason === undefined ? {} : { disabled: true, reason }) });
  }
  const seen = new Set<string>();
  for (const row of entries) {
    const selector: TcpSelector = { kind: 'entry', target: { ...row.entry.target }, frame: { ...row.entry.frame } };
    const key = tcpSelectorKey(selector);
    if (seen.has(key)) continue;
    seen.add(key);
    const duplicates = entries.filter((candidate) => tcpSelectorKey({ kind: 'entry', ...candidate.entry }) === key);
    const reason = duplicates.length !== 1 ? 'This TCP has ambiguous eligibility.'
      : row.available ? undefined : row.reason ?? 'This TCP is not compatible with the selected moving target.';
    choices.push({ key,
      label: `${row.entry.target.component_id} · ${row.entry.target.command_target_id} / ${row.entry.frame.component_id} · ${row.entry.frame.frame_id}${row.component_default ? ' (default)' : ''}`,
      selector, ...(reason === undefined ? {} : { disabled: true, reason }) });
  }
  return choices;
}

export function motionEndpointKeyOfNode(node: ProgramTreeNode, parameterId = 'tcp_endpoint'): string | null {
  if (!Object.prototype.hasOwnProperty.call(node.parameters, parameterId)) {
    return parameterId === 'tcp_endpoint' ? tcpSelectorKey({ kind: 'current' }) : null;
  }
  const endpoint = objectValue(node.parameters[parameterId]);
  if (endpoint === null) return null;
  if (endpoint.kind === 'current' && Object.keys(endpoint).length === 1) return tcpSelectorKey({ kind: 'current' });
  const target = targetValue(endpoint.target);
  if (endpoint.kind === 'default_of_target' && Object.keys(endpoint).length === 2 && target !== null) {
    return tcpSelectorKey({ kind: 'default_of_target', target });
  }
  const frame = frameValue(endpoint.frame);
  if (endpoint.kind === 'entry' && Object.keys(endpoint).length === 3 && target !== null && frame !== null) {
    return tcpSelectorKey({ kind: 'entry', target, frame });
  }
  return null;
}

export function buildMotionEndpointMutation(node: ProgramTreeNode, endpoint: TcpSelector | null, parameterId = 'tcp_endpoint') {
  const parameters = { ...node.parameters };
  const valueSources = { ...(node.value_sources ?? {}) };
  delete valueSources[parameterId];
  if (endpoint === null) delete parameters[parameterId];
  else parameters[parameterId] = endpoint as unknown as JsonValue;
  return { mutation: 'tree_draft_update_node_params' as const, args: {
    instance_id: node.instance_id, expected_node_id: node.node_id, merge: false, parameters,
    value_sources: valueSources as unknown as JsonValue,
  } };
}
