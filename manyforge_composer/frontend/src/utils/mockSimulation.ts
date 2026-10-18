import type { JsonValue, ProgramBindingDocument, ProgramTreeNode } from '../types/api';
import { conditionKind } from './interruptAuthoring';
import { declaredIoSources, ioSourceKey, type IoSourceProfile } from './ioSignalConfiguration';
import { INTERRUPT_MOTION_NODE_IDS } from './programAuthoring';
import { valuePathParameter } from './valuePaths';

/**
 * Per-node Mock signal simulation (contracts 133/135): the literal-only
 * `mock_simulation` rows of a signal-reading node, catalog schema type
 * `mock_signal_simulation`. Each row names one signal the node itself reads,
 * its value at node start and optionally one new value after an offset from
 * node start. Mirrors `program._validate_mock_simulation`; the backend stays
 * the authority.
 */

export const MOCK_SIMULATION_PARAMETER = 'mock_simulation';
export const IO_SIGNAL_INPUT_NODE_IDS = ['io.get_input', 'io.check_input', 'io.wait_for_input'] as const;
export const MOCK_SIMULATION_NODE_IDS: ReadonlySet<string> = new Set<string>([
  ...INTERRUPT_MOTION_NODE_IDS, ...IO_SIGNAL_INPUT_NODE_IDS,
]);

/** The backend's stable refusal reasons, used verbatim in the GUI. */
export const MOCK_SIMULATION_INVALID = 'mock_simulation_invalid';
export const MOCK_SIMULATION_SIGNAL_UNUSED = 'mock_simulation_signal_unused';
export const MOCK_SIMULATION_SIGNAL_DUPLICATE = 'mock_simulation_signal_duplicate';
export const MOCK_SIMULATION_SOURCE_REFUSED = 'mock_simulation_value_source_refused';

export interface MockSignal {
  targetRole: string;
  resourceRole: string;
}

export interface MockSimulationRow {
  signal: MockSignal;
  startValue: boolean;
  change?: { afterMs: number; value: boolean };
}

export type ParsedMockSimulation =
  | { ok: true; rows: MockSimulationRow[] }
  | { ok: false; problem: string };

function objectOf(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? value : null;
}

function closed(record: Record<string, JsonValue>, required: readonly string[], optional: readonly string[] = []): boolean {
  return required.every((key) => Object.hasOwn(record, key))
    && Object.keys(record).every((key) => required.includes(key) || optional.includes(key));
}

export function mockSignalId(signal: MockSignal): string {
  return `${signal.targetRole}/${signal.resourceRole}`;
}

const compareSignals = (left: MockSignal, right: MockSignal) => (
  mockSignalId(left) < mockSignalId(right) ? -1 : mockSignalId(left) > mockSignalId(right) ? 1 : 0);

/** The signals a condition compares (`condition_signals`), unique and sorted. */
export function conditionSignals(condition: JsonValue | undefined): MockSignal[] {
  const found = new Map<string, MockSignal>();
  const walk = (value: JsonValue | undefined) => {
    const kind = conditionKind(value);
    const record = objectOf(value);
    if (kind === null || record === null) return;
    if (kind === 'compare') {
      const signal = objectOf(objectOf(objectOf(record.compare)?.operand)?.signal);
      if (typeof signal?.target_role === 'string' && typeof signal.resource_role === 'string') {
        const item = { targetRole: signal.target_role, resourceRole: signal.resource_role };
        found.set(mockSignalId(item), item);
      }
      return;
    }
    if (kind === 'not') walk(record.not);
    else if (Array.isArray(record[kind])) (record[kind] as JsonValue[]).forEach(walk);
  };
  walk(condition);
  return [...found.values()].sort(compareSignals);
}

/**
 * The signals a node's condition reads, i.e. those it may simulate: the
 * interrupt condition's signals of the two interrupt motion nodes, the one
 * `(target_role, resource_role)` signal of the I/O input nodes, else none.
 */
export function nodeMockSignals(node: Pick<ProgramTreeNode, 'node_id' | 'parameters'>): MockSignal[] {
  const parameters = node.parameters ?? {};
  if ((INTERRUPT_MOTION_NODE_IDS as readonly string[]).includes(node.node_id)) {
    return conditionSignals(parameters.interrupt_condition);
  }
  if ((IO_SIGNAL_INPUT_NODE_IDS as readonly string[]).includes(node.node_id)
    && typeof parameters.target_role === 'string' && typeof parameters.resource_role === 'string') {
    return [{ targetRole: parameters.target_role, resourceRole: parameters.resource_role }];
  }
  return [];
}

/** Strict shape of the `mock_signal_simulation` rows (closed objects, typed literals). */
export function parseMockSimulation(value: JsonValue | undefined): ParsedMockSimulation {
  if (value === undefined) return { ok: true, rows: [] };
  const invalid = (index: number | null, message: string): ParsedMockSimulation => ({
    ok: false,
    problem: `${MOCK_SIMULATION_INVALID}: ${index === null ? 'mock_simulation' : `mock_simulation[${index}]`} ${message}`,
  });
  if (!Array.isArray(value)) return invalid(null, 'must be a list of rows.');
  const rows: MockSimulationRow[] = [];
  for (const [index, item] of value.entries()) {
    const row = objectOf(item);
    if (row === null || !closed(row, ['signal', 'start_value'], ['change'])) {
      return invalid(index, 'needs exactly a signal, a start value and an optional change.');
    }
    const signal = objectOf(row.signal);
    if (signal === null || !closed(signal, ['target_role', 'resource_role'])
      || typeof signal.target_role !== 'string' || typeof signal.resource_role !== 'string') {
      return invalid(index, 'signal must be exactly a target role and a resource role.');
    }
    if (typeof row.start_value !== 'boolean') return invalid(index, 'start value must be true or false.');
    const parsed: MockSimulationRow = {
      signal: { targetRole: signal.target_role, resourceRole: signal.resource_role },
      startValue: row.start_value,
    };
    if (row.change !== undefined) {
      const change = objectOf(row.change);
      if (change === null || !closed(change, ['after_ms', 'value'])) {
        return invalid(index, 'change needs exactly an after_ms offset and a value.');
      }
      if (typeof change.after_ms !== 'number' || !Number.isSafeInteger(change.after_ms) || change.after_ms < 0) {
        return invalid(index, 'change after_ms must be a whole number of milliseconds, 0 or more.');
      }
      if (typeof change.value !== 'boolean') return invalid(index, 'change value must be true or false.');
      parsed.change = { afterMs: change.after_ms, value: change.value };
    }
    rows.push(parsed);
  }
  return { ok: true, rows };
}

export function serializeMockSimulation(rows: readonly MockSimulationRow[]): JsonValue {
  return rows.map((row) => ({
    signal: { target_role: row.signal.targetRole, resource_role: row.signal.resourceRole },
    start_value: row.startValue,
    ...(row.change === undefined ? {} : { change: { after_ms: row.change.afterMs, value: row.change.value } }),
  }));
}

/** The first problem of each parsed row (unused, then duplicate), by row index. */
export function mockSimulationRowProblems(
  rows: readonly MockSimulationRow[],
  used: readonly MockSignal[],
): Map<number, string> {
  const usedIds = new Set(used.map(mockSignalId));
  const seen = new Set<string>();
  const problems = new Map<number, string>();
  rows.forEach((row, index) => {
    const id = mockSignalId(row.signal);
    if (!usedIds.has(id)) {
      problems.set(index, `${MOCK_SIMULATION_SIGNAL_UNUSED}: signal ${id} is not read by this node.`);
    } else if (seen.has(id)) {
      problems.set(index, `${MOCK_SIMULATION_SIGNAL_DUPLICATE}: signal ${id} already has a Mock simulation row.`);
    }
    seen.add(id);
  });
  return problems;
}

/**
 * The first local problem of a node's Mock simulation, or null: a value
 * source inside it, the row shape, then each row's signal (read by this
 * node, at most one row per signal).
 */
export function mockSimulationProblem(
  node: Pick<ProgramTreeNode, 'node_id' | 'parameters' | 'value_sources'>,
): string | null {
  if (Object.keys(node.value_sources ?? {}).some((path) => valuePathParameter(path) === MOCK_SIMULATION_PARAMETER)) {
    return `${MOCK_SIMULATION_SOURCE_REFUSED}: Mock simulation rows are literal values only.`;
  }
  const value = node.parameters?.[MOCK_SIMULATION_PARAMETER];
  const parsed = parseMockSimulation(value);
  if (!parsed.ok) return parsed.problem;
  const problems = mockSimulationRowProblems(parsed.rows, nodeMockSignals(node));
  return problems.size === 0 ? null : [...problems.values()][0];
}

/**
 * The declared source profile of each signal a node reads, by `mockSignalId`,
 * from the current binding and Deployment Draft. A signal is missing when its
 * source is not declared there (unknown: the simulation stays editable).
 */
export function mockSignalProfiles(
  signals: readonly MockSignal[],
  binding: ProgramBindingDocument | null | undefined,
  deploymentDocument: Record<string, JsonValue> | null,
): Map<string, IoSourceProfile> {
  const declared = declaredIoSources(deploymentDocument);
  const result = new Map<string, IoSourceProfile>();
  for (const signal of signals) {
    const target = binding?.target_role_bindings?.find((row) => row.target_role_id === signal.targetRole);
    if (target === undefined) continue;
    const profile = declared.get(ioSourceKey(
      target.controllable_component_id, target.command_target_id, signal.resourceRole,
    ))?.profile;
    if (profile !== undefined) result.set(mockSignalId(signal), profile);
  }
  return result;
}
