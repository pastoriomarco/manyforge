import type { JsonValue, ProgramValueSource } from '../types/api';

/**
 * Value paths name one value inside a node's parameters
 * (docs/authoring/EXPRESSIONS.md §9): the parameter name, then `.field` for
 * object fields and `[index]` for list positions, for example
 * `goal.position_m[2]`. The text is canonical, so equal paths are equal
 * strings and an ancestor is a string prefix followed by `.` or `[`.
 */
export type ValuePathSegment = string | number;

export type ValueSources = Record<string, ProgramValueSource>;

/** A list whose items were inserted, deleted or reordered: `indexMap[old]` is the new index, or null when deleted. */
export interface ListMove {
  path: string;
  indexMap: readonly (number | null)[];
}

// The one grammar shared with the Python and C++ validators; its executable
// specification is manyforge_deployment/resources/expression/value_path_conformance.v1.yaml.
const MAX_PATH_LENGTH = 1024;
const MAX_PATH_SEGMENTS = 33;
const MAX_NAME_LENGTH = 256;
const MAX_INDEX_DIGITS = 9;
const NAME = /^[a-z][a-z0-9_-]*/;
const DIGITS = /^[0-9]*/;

export type ValuePathRefusal =
  | 'path_empty'
  | 'path_too_long'
  | 'path_name_invalid'
  | 'path_index_invalid'
  | 'path_syntax_invalid'
  | 'path_too_deep';

/** The segments of a canonical path, or the reason the text is not one. */
export function checkValuePath(
  text: string,
): { segments: ValuePathSegment[] } | { refusal: ValuePathRefusal } {
  if (text === '') return { refusal: 'path_empty' };
  if (text.length > MAX_PATH_LENGTH) return { refusal: 'path_too_long' };
  const segments: ValuePathSegment[] = [];
  let rest = text;
  const name = (): ValuePathRefusal | null => {
    const match = NAME.exec(rest);
    if (match === null || match[0].length > MAX_NAME_LENGTH) return 'path_name_invalid';
    rest = rest.slice(match[0].length);
    segments.push(match[0]);
    return null;
  };
  const index = (): ValuePathRefusal | null => {
    const digits = DIGITS.exec(rest)![0];
    if (
      digits === '' || (digits.length > 1 && digits.startsWith('0'))
      || digits.length > MAX_INDEX_DIGITS || rest[digits.length] !== ']'
    ) return 'path_index_invalid';
    rest = rest.slice(digits.length + 1);
    segments.push(Number(digits));
    return null;
  };
  let refusal = name();
  while (refusal === null && rest !== '') {
    const separator = rest[0];
    rest = rest.slice(1);
    if (separator === '.') refusal = name();
    else if (separator === '[') refusal = index();
    else refusal = 'path_syntax_invalid';
    if (refusal === null && segments.length > MAX_PATH_SEGMENTS) refusal = 'path_too_deep';
  }
  return refusal === null ? { segments } : { refusal };
}

/** The segments of a canonical path, or null when the text is not one. */
export function parseValuePath(text: string): ValuePathSegment[] | null {
  const checked = checkValuePath(text);
  return 'segments' in checked ? checked.segments : null;
}

export function formatValuePath(segments: readonly ValuePathSegment[]): string {
  return segments.map((segment, index) => typeof segment === 'number'
    ? `[${segment}]`
    : index === 0 ? segment : `.${segment}`).join('');
}

/** The parameter a path starts at. */
export function valuePathParameter(path: string): string {
  return NAME.exec(path)?.[0] ?? path;
}

/** Whether `path` is `ancestor` itself or a value inside it. */
export function valuePathWithin(path: string, ancestor: string): boolean {
  return path === ancestor
    || (path.startsWith(ancestor) && (path[ancestor.length] === '.' || path[ancestor.length] === '['));
}

export function valueAtPath(
  parameters: Readonly<Record<string, JsonValue>>,
  path: string,
): JsonValue | undefined {
  const segments = parseValuePath(path);
  if (segments === null) return undefined;
  let current: JsonValue | undefined = parameters as Record<string, JsonValue>;
  for (const segment of segments) {
    if (typeof segment === 'number') {
      current = Array.isArray(current) ? current[segment] : undefined;
    } else {
      current = current !== null && typeof current === 'object' && !Array.isArray(current)
        && Object.prototype.hasOwnProperty.call(current, segment)
        ? current[segment]
        : undefined;
    }
    if (current === undefined) return undefined;
  }
  return current;
}

function replaced(
  container: JsonValue | undefined,
  segments: readonly ValuePathSegment[],
  value: JsonValue,
): JsonValue | undefined {
  if (segments.length === 0) return value;
  const [segment, ...rest] = segments;
  if (typeof segment === 'number') {
    if (!Array.isArray(container) || segment >= container.length) return undefined;
    const child = replaced(container[segment], rest, value);
    if (child === undefined) return undefined;
    const next = [...container];
    next[segment] = child;
    return next;
  }
  if (container === null || typeof container !== 'object' || Array.isArray(container)) {
    return undefined;
  }
  if (rest.length > 0 && !Object.prototype.hasOwnProperty.call(container, segment)) {
    return undefined;
  }
  const child = replaced(container[segment], rest, value);
  return child === undefined ? undefined : { ...container, [segment]: child };
}

/** A copy with the value at `path` replaced; undefined when its container does not exist. */
export function setValueAtPath(
  parameters: Readonly<Record<string, JsonValue>>,
  path: string,
  value: JsonValue,
): Record<string, JsonValue> | undefined {
  const segments = parseValuePath(path);
  if (segments === null) return undefined;
  return replaced(parameters as Record<string, JsonValue>, segments, value) as
    Record<string, JsonValue> | undefined;
}

/** Whether any entry is at `path` or inside it. */
export function hasSourcesWithin(sources: ValueSources | undefined, path: string): boolean {
  return Object.keys(sources ?? {}).some((entry) => valuePathWithin(entry, path));
}

/** Pairs of entries where one is inside the other; the document allows none. */
export function overlappingSourcePaths(sources: ValueSources): Array<[string, string]> {
  const paths = Object.keys(sources).sort();
  const pairs: Array<[string, string]> = [];
  for (const outer of paths) {
    for (const inner of paths) {
      if (inner !== outer && valuePathWithin(inner, outer)) pairs.push([outer, inner]);
    }
  }
  return pairs;
}

/**
 * Set one entry. Entries it would overlap (the values around it and inside
 * it) are removed, so the result never holds a whole value and its fields.
 */
export function withValueSource(
  sources: ValueSources | undefined,
  path: string,
  source: ProgramValueSource,
): ValueSources {
  const next = Object.fromEntries(Object.entries(sources ?? {}).filter(([entry]) =>
    !valuePathWithin(entry, path) && !valuePathWithin(path, entry)));
  next[path] = source;
  return next;
}

/** Move the entries inside a list's items with those items; entries of deleted items are dropped. */
export function moveListSources(sources: ValueSources, move: ListMove): ValueSources {
  const list = parseValuePath(move.path);
  if (list === null) return sources;
  const next: ValueSources = {};
  for (const [entry, source] of Object.entries(sources)) {
    const segments = parseValuePath(entry);
    const index = segments?.[list.length];
    if (
      segments === null
      || typeof index !== 'number'
      || !list.every((segment, position) => segments[position] === segment)
    ) {
      next[entry] = source;
      continue;
    }
    const moved = move.indexMap[index];
    if (moved === null || moved === undefined) continue;
    const target = [...segments];
    target[list.length] = moved;
    next[formatValuePath(target)] = source;
  }
  return next;
}

/** The index map of inserting one item at `index` into a list of `length` items. */
export function listInsertMove(path: string, length: number, index: number): ListMove {
  return { path, indexMap: Array.from({ length }, (_unused, old) => old < index ? old : old + 1) };
}

/** The index map of deleting the item at `index`. */
export function listDeleteMove(path: string, length: number, index: number): ListMove {
  return {
    path,
    indexMap: Array.from({ length }, (_unused, old) =>
      old === index ? null : old < index ? old : old - 1),
  };
}

/** The index map of moving the item at `from` to `to`, shifting the items between. */
export function listReorderMove(path: string, length: number, from: number, to: number): ListMove {
  const order = Array.from({ length }, (_unused, old) => old);
  const [item] = order.splice(from, 1);
  order.splice(to, 0, item!);
  const indexMap = Array<number>(length);
  order.forEach((old, position) => { indexMap[old] = position; });
  return { path, indexMap };
}

/** Whether `path` ends at a key of an existing object: an entry a source may supply. */
function namesObjectEntry(parameters: Readonly<Record<string, JsonValue>>, path: string): boolean {
  const segments = parseValuePath(path);
  const last = segments?.at(-1);
  if (segments === null || segments.length < 2 || typeof last !== 'string') return false;
  const container = valueAtPath(parameters, formatValuePath(segments.slice(0, -1)));
  return container !== undefined && container !== null
    && typeof container === 'object' && !Array.isArray(container);
}

/**
 * The entries after typing a new value for one parameter: list moves carry
 * entries along, a whole-parameter entry is replaced by the typed value, and
 * entries whose path no longer names a value are dropped. An entry that
 * supplies a map entry without a literal (it had none in `previous` either)
 * stays while its map does.
 */
export function sourcesAfterLiteral(
  sources: ValueSources | undefined,
  parameterId: string,
  value: JsonValue,
  moves: readonly ListMove[] = [],
  previous?: JsonValue,
): ValueSources {
  const moved = moves.reduce(moveListSources, { ...(sources ?? {}) });
  const parameters = { [parameterId]: value };
  const before = previous === undefined ? null : { [parameterId]: previous };
  return Object.fromEntries(Object.entries(moved).filter(([entry]) =>
    !valuePathWithin(entry, parameterId)
    || (entry !== parameterId && (
      valueAtPath(parameters, entry) !== undefined
      || (before !== null && valueAtPath(before, entry) === undefined
        && namesObjectEntry(parameters, entry))
    ))));
}
