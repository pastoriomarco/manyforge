import type { JsonValue, ProgramTreeNode } from '../types/api';
import { parseValuePath } from './valuePaths';

export type ManualFillPreviewValues = ReadonlyMap<string, string>;

function isObject(value: JsonValue | undefined): value is Record<string, JsonValue> {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function equal(left: JsonValue | undefined, right: JsonValue | undefined): boolean {
  if (left === right) return true;
  if (Array.isArray(left) && Array.isArray(right)) {
    return left.length === right.length && left.every((value, index) => equal(value, right[index]));
  }
  if (isObject(left) && isObject(right)) {
    const leftKeys = Object.keys(left);
    const rightKeys = Object.keys(right);
    return leftKeys.length === rightKeys.length
      && leftKeys.every((key) => key in right && equal(left[key], right[key]));
  }
  return false;
}

// An omitted field falls back to its catalog or Program default.
const OMITTED = 'default';

function frameLabel(value: JsonValue | undefined): string {
  const frame = isObject(value) ? value : {};
  return `${String(frame.component_id ?? '')} · ${String(frame.frame_id ?? '')}`;
}

function targetLabel(value: JsonValue | undefined): string {
  const target = isObject(value) ? value : {};
  return `${String(target.component_id ?? '')} · ${String(target.command_target_id ?? '')}`;
}

function display(value: JsonValue | undefined): string {
  if (value === undefined) return OMITTED;
  if (value === null) return 'null';
  // Four decimals: 0.1 mm, 0.0001 rad; the field itself keeps the full value.
  if (typeof value === 'number') return String(Number(value.toFixed(4)));
  if (typeof value === 'string' || typeof value === 'boolean') return String(value);
  if (Array.isArray(value)) return value.map((item) => display(item)).join(', ');
  if (value.kind === 'cell_frame') return 'cell frame';
  if (value.kind === 'target_frame') return `${frameLabel(value.frame)} frame`;
  if (value.kind === 'default_of_target') return `${targetLabel(value.target)} default TCP`;
  if (value.kind === 'entry') return frameLabel(value.frame);
  if (value.kind === undefined && typeof value.component_id === 'string') {
    if (typeof value.frame_id === 'string') return `${frameLabel(value)} frame`;
    if (typeof value.command_target_id === 'string') return targetLabel(value);
  }
  return Object.entries(value).map(([key, item]) => `${key}: ${display(item)}`).join(', ');
}

function pathKey(path: Array<string | number>): string {
  return path.reduce<string>((current, part) => typeof part === 'number'
    ? `${current}[${part}]`
    : current === '' ? part : `${current}.${part}`, '');
}

function walkChanges(
  before: JsonValue | undefined,
  after: JsonValue | undefined,
  path: Array<string | number>,
  changes: Map<string, string>,
): void {
  if (equal(before, after)) return;
  const key = pathKey(path);
  // Direct target and base-frame references change as one field, like the
  // role names they replace.
  if (key.endsWith('.frame_ref') || key.endsWith('.tcp_endpoint') || key === 'target' || key === 'base_frame') {
    changes.set(key, display(after));
    return;
  }
  if ((isObject(before) || before === undefined) && (isObject(after) || after === undefined)
    && (isObject(before) || isObject(after))) {
    const oldObject = isObject(before) ? before : {};
    const newObject = isObject(after) ? after : {};
    for (const field of new Set([...Object.keys(oldObject), ...Object.keys(newObject)])) {
      walkChanges(oldObject[field], newObject[field], [...path, field], changes);
    }
    return;
  }
  const jointArray = (value: JsonValue | undefined) => value === undefined
    || (Array.isArray(value)
      && value.every((item) => isObject(item) && typeof item.joint_name === 'string'));
  if (path[path.length - 1] === 'goal' && jointArray(before) && jointArray(after)) {
    const oldRows = Array.isArray(before) ? before : [];
    const newRows = Array.isArray(after) ? after : [];
    const oldByName = new Map(oldRows.map((item) => [
      String((item as Record<string, JsonValue>).joint_name), item,
    ] as const));
    const newByName = new Map(newRows.map((item) => [
      String((item as Record<string, JsonValue>).joint_name), item,
    ] as const));
    for (const name of new Set([...oldByName.keys(), ...newByName.keys()])) {
      const oldValue = oldByName.get(name);
      const newValue = newByName.get(name);
      if (!equal(oldValue, newValue)) {
        const item = isObject(newValue) ? newValue : undefined;
        changes.set(`${key}[${name}].position_rad`, display(item?.position_rad));
      }
    }
    return;
  }
  if (Array.isArray(before) || Array.isArray(after)) {
    const oldArray = Array.isArray(before) ? before : [];
    const newArray = Array.isArray(after) ? after : [];
    for (let index = 0; index < Math.max(oldArray.length, newArray.length); index += 1) {
      walkChanges(oldArray[index], newArray[index], [...path, index], changes);
    }
    return;
  }
  changes.set(key, display(after));
}

function sourceRecord(value: ProgramTreeNode['value_sources']): Record<string, JsonValue> {
  return (value ?? {}) as unknown as Record<string, JsonValue>;
}

function valueAtPath(root: JsonValue | undefined, path: Array<string | number>): JsonValue | undefined {
  let current: JsonValue | undefined = root;
  for (const part of path) {
    if (typeof part === 'number') current = Array.isArray(current) ? current[part] : undefined;
    else current = isObject(current) ? current[part] : undefined;
  }
  return current;
}

/** Values that the same parameter payload will write, including source removal. */
export function manualFillPreviewValues(
  node: ProgramTreeNode,
  parameters: Record<string, JsonValue>,
  valueSources: Record<string, JsonValue>,
): ManualFillPreviewValues {
  const changes = new Map<string, string>();
  walkChanges(node.parameters, parameters, [], changes);
  const oldSources = sourceRecord(node.value_sources);
  for (const path of Object.keys(oldSources)) {
    if (path in valueSources) continue;
    const parsedSourcePath = parseValuePath(path);
    if (parsedSourcePath === null) continue;
    let parsed = parsedSourcePath;
    let mappedValue: JsonValue | undefined;
    let hasMappedValue = false;
    const oldGoal = node.parameters.goal;
    if (parsed[0] === 'goal' && typeof parsed[1] === 'number'
      && Array.isArray(oldGoal)
      && oldGoal.every((item) => isObject(item) && typeof item.joint_name === 'string')) {
      const oldRow = oldGoal[parsed[1]];
      if (isObject(oldRow) && typeof oldRow.joint_name === 'string') {
        parsed = ['goal', oldRow.joint_name, ...parsed.slice(2)];
        const nextGoal = parameters.goal;
        const nextRow = Array.isArray(nextGoal)
          ? nextGoal.find((item) => isObject(item) && item.joint_name === oldRow.joint_name)
          : undefined;
        mappedValue = parsed.length === 2 ? nextRow : valueAtPath(nextRow, parsed.slice(2));
        hasMappedValue = true;
      }
    }
    const jointGoal = Array.isArray(node.parameters.goal)
      && node.parameters.goal.every((item) => isObject(item) && typeof item.joint_name === 'string');
    const key = jointGoal && parsed[0] === 'goal' && typeof parsed[1] === 'string'
      ? `goal[${parsed[1]}]${parsed.slice(2).map((part) => typeof part === 'number'
        ? `[${part}]` : `.${part}`).join('')}`
      : pathKey(parsed);
    if (changes.has(key)) continue;
    const value = hasMappedValue ? mappedValue : valueAtPath(parameters, parsed);
    if (value === undefined) changes.set(key, OMITTED);
    else walkChanges(undefined, value, parsed, changes);
  }
  return changes;
}
