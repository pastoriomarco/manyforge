import type { JsonValue, ProgramNodeWaitValue, ProgramTreeNode, ProgramValueSource } from '../types/api';
import { ioSignalId, type SignalSample } from '../types/components';

/** One condition the running Program waits for, and the nodes waiting on it (OD-354). */
export interface ProgramWait {
  condition: string;
  nodeInstanceIds: string[];
  /** The input an input wait names directly (not through a key), as its sample's `signal_id`. */
  signalId?: string;
}

function valueText(value: JsonValue | undefined): string {
  if (value === undefined) return '?';
  return typeof value === 'string' ? value : JSON.stringify(value);
}

function sourceText(source: ProgramValueSource): string {
  switch (source.kind) {
    case 'blackboard_key':
      return `key ${source.reference}`;
    case 'program_parameter':
      return `parameter ${source.reference}`;
    case 'blackboard_field':
      return `key ${source.reference}.${source.field}`;
    case 'expression':
      return source.expression;
  }
}

/** A parameter as written: its value source when it has one, else its value. */
function parameterText(node: ProgramTreeNode, id: string): string {
  const source = node.value_sources?.[id];
  return source === undefined ? valueText(node.parameters[id]) : sourceText(source);
}

function objectMember(value: JsonValue | undefined, member: string): string | null {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return null;
  const field = value[member];
  return typeof field === 'string' ? field : null;
}

function waitSignalId(node: ProgramTreeNode): string | undefined {
  if (node.node_id !== 'io.wait_for_input' || node.value_sources?.signal !== undefined) return undefined;
  const component = objectMember(node.parameters.signal, 'component_id');
  const name = objectMember(node.parameters.signal, 'signal_name');
  return component !== null && name !== null ? ioSignalId(component, name) : undefined;
}

function waitCondition(node: ProgramTreeNode): string | null {
  switch (node.node_id) {
    case 'logic.wait_for_value': {
      const operator = node.parameters.operator === undefined ? '==' : valueText(node.parameters.operator);
      const tolerance = node.parameters.tolerance === undefined && node.value_sources?.tolerance === undefined
        ? '' : ` ± ${parameterText(node, 'tolerance')}`;
      return `${parameterText(node, 'key')} ${operator} ${parameterText(node, 'value')}${tolerance}`;
    }
    case 'io.wait_for_input': {
      const signal = node.parameters.signal;
      const component = objectMember(signal, 'component_id');
      const name = objectMember(signal, 'signal_name');
      const label = component !== null && name !== null ? `${component}/${name}` : parameterText(node, 'signal');
      const edge = node.parameters.edge === undefined ? '' : ` (${valueText(node.parameters.edge)} edge)`;
      return `input ${label} = ${parameterText(node, 'expected_value')}${edge}`;
    }
    case 'gripper.wait_for_status': {
      const component = objectMember(node.parameters.target, 'component_id');
      return `gripper ${component ?? parameterText(node, 'target')} ${parameterText(node, 'expected_state')}`;
    }
    default:
      return null;
  }
}

/** The conditions of the running wait nodes, grouped by condition, in tree order. */
export function activeProgramWaits(
  tree: ProgramTreeNode,
  nodeStatuses: Readonly<Record<string, string>>,
): ProgramWait[] {
  const waits = new Map<string, ProgramWait>();
  const visit = (node: ProgramTreeNode) => {
    if (nodeStatuses[node.instance_id] === 'running') {
      const condition = waitCondition(node);
      if (condition !== null) {
        const wait = waits.get(condition);
        if (wait !== undefined) wait.nodeInstanceIds.push(node.instance_id);
        else {
          const signalId = waitSignalId(node);
          waits.set(condition, { condition, nodeInstanceIds: [node.instance_id], ...(signalId === undefined ? {} : { signalId }) });
        }
      }
    }
    node.children.forEach(visit);
  };
  visit(tree);
  return [...waits.values()];
}

/** What the run status tells about its running nodes (absent from older owners). */
export interface ProgramWaitObservation {
  /** Running node → UTC nanoseconds (decimal text) it started. */
  runningSinceUtcNs?: Readonly<Record<string, string>>;
  /** Running wait node → the value it waits on, as the runner last read it. */
  waitValues?: Readonly<Record<string, ProgramNodeWaitValue>>;
  nowMs: number;
}

function waitValueText(value: ProgramNodeWaitValue): string | null {
  if (value.kind === 'gripper') return value.state ?? (value.value === undefined ? null : valueText(value.value));
  return value.value === undefined ? null : valueText(value.value);
}

/** Whole seconds the earliest of the wait's nodes has run, or null without a start time. */
export function programWaitElapsedSeconds(wait: ProgramWait, observation: ProgramWaitObservation): number | null {
  const starts = wait.nodeInstanceIds.map((id) => observation.runningSinceUtcNs?.[id])
    .filter((since): since is string => since !== undefined).map((since) => Number(BigInt(since) / 1_000_000n));
  return starts.length === 0 ? null : Math.max(0, Math.floor((observation.nowMs - Math.min(...starts)) / 1000));
}

/**
 * One wait as a line: the condition, the current value it waits on (an
 * input's pushed sample `· now false (stale)`, else the key or gripper value
 * the runner read), how long it has waited (`· 40 s`), then the waiting nodes.
 */
export function programWaitLine(
  wait: ProgramWait,
  io: readonly SignalSample[] | null,
  observation?: ProgramWaitObservation,
): string {
  const sample = wait.signalId === undefined ? undefined
    : io?.find((candidate) => candidate.signal_id === wait.signalId);
  let now = sample === undefined ? ''
    : ` · now ${sample.value === null ? 'unknown' : String(sample.value)}${sample.fresh ? '' : ' (stale)'}`;
  if (now === '' && observation !== undefined) {
    const value = wait.nodeInstanceIds.map((id) => observation.waitValues?.[id])
      .find((candidate) => candidate !== undefined);
    const text = value === undefined ? null : waitValueText(value);
    if (text !== null) now = ` · now ${text}`;
  }
  const seconds = observation === undefined ? null : programWaitElapsedSeconds(wait, observation);
  const elapsed = seconds === null || seconds === 0 ? '' : ` · ${seconds} s`;
  return `${wait.condition}${now}${elapsed} — ${wait.nodeInstanceIds.join(', ')}`;
}
