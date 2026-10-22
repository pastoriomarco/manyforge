import { parseNanoseconds, type Nanoseconds } from '../../types/nanoseconds';
import type {
  JsonValue,
  ProjectionRevision,
  QuaternionXyzwTuple,
  Vector3Tuple,
} from '../../types/api';
import type { SourceFreshness } from '../../types/headless';

export type JsonObject = Record<string, unknown>;

export class StructuralContractError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'StructuralContractError';
  }
}

export function fail(path: string, expected: string): never {
  throw new StructuralContractError(`${path} must be ${expected}`);
}

export function objectAt(value: unknown, path: string): JsonObject {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    return fail(path, 'an object');
  }
  return value as JsonObject;
}

export function exactKeysAt(
  value: JsonObject,
  expected: readonly string[],
  path: string,
): void {
  const actual = Object.keys(value).sort();
  const required = [...expected].sort();
  if (
    actual.length !== required.length
    || actual.some((key, index) => key !== required[index])
  ) {
    fail(path, `an object with exactly [${required.join(', ')}]`);
  }
}

export function allowedKeysAt(
  value: JsonObject,
  required: readonly string[],
  optional: readonly string[],
  path: string,
): void {
  const actual = new Set(Object.keys(value));
  const allowed = new Set([...required, ...optional]);
  if (
    required.some((key) => !actual.has(key))
    || [...actual].some((key) => !allowed.has(key))
  ) {
    fail(
      path,
      `an object with required [${required.join(', ')}] and optional [${optional.join(', ')}] fields`,
    );
  }
}

export function arrayAt(value: unknown, path: string): unknown[] {
  if (!Array.isArray(value)) {
    return fail(path, 'an array');
  }
  return value;
}

export function stringAt(value: unknown, path: string): string {
  if (typeof value !== 'string') {
    return fail(path, 'a string');
  }
  return value;
}

export function nonnegativeInt64TextAt(value: unknown, path: string): string {
  const text = stringAt(value, path);
  if (
    text.length > 19
    || !/^(0|[1-9]\d*)$/.test(text)
    || BigInt(text) > 9_223_372_036_854_775_807n
  ) {
    return fail(path, 'a canonical nonnegative int64 decimal string');
  }
  return text;
}

function positiveUint64TextAt(value: unknown, path: string): string {
  const text = stringAt(value, path);
  if (
    text.length > 20
    || !/^[1-9]\d*$/.test(text)
    || BigInt(text) > 18_446_744_073_709_551_615n
  ) {
    return fail(path, 'a canonical positive uint64 decimal string');
  }
  return text;
}

export function nullablePositiveUint64TextAt(
  value: unknown,
  path: string,
): string | null {
  return value === null ? null : positiveUint64TextAt(value, path);
}

export function digestAt(value: unknown, path: string): string {
  const digest = stringAt(value, path);
  if (!/^sha256:[0-9a-f]{64}$/.test(digest)) {
    return fail(path, 'an exact SHA-256 identity');
  }
  return digest;
}

/**
 * A Scenario project folder as Apply records it: absolute, lexically
 * normalized (no empty, `.` or `..` segment, no trailing slash), no NUL, and
 * at most 4096 UTF-8 bytes. Persistent values are scoped by it.
 */
export function projectPathAt(value: unknown, path: string): string {
  const text = stringAt(value, path);
  const segments = text.split('/').slice(1);
  if (
    !text.startsWith('/') || text.includes('\0')
    || new TextEncoder().encode(text).length > 4096
    || (text !== '/' && segments.some((segment) => (
      segment === '' || segment === '.' || segment === '..'
    )))
  ) {
    return fail(path, 'a normalized absolute project folder path');
  }
  return text;
}

export function nullableStringAt(value: unknown, path: string): string | null {
  if (value === null) {
    return null;
  }
  return stringAt(value, path);
}

export function booleanAt(value: unknown, path: string): boolean {
  if (typeof value !== 'boolean') {
    return fail(path, 'a boolean');
  }
  return value;
}

export function numberAt(value: unknown, path: string): number {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    return fail(path, 'a finite number');
  }
  return value;
}

export function integerAt(value: unknown, path: string): number {
  const number = numberAt(value, path);
  if (!Number.isSafeInteger(number) || number < 0) {
    return fail(path, 'a non-negative safe integer');
  }
  return number;
}

export function positiveIntegerAt(value: unknown, path: string): number {
  const integer = integerAt(value, path);
  if (integer === 0) return fail(path, 'a positive safe integer');
  return integer;
}

export function positiveNumberAt(value: unknown, path: string): number {
  const number = numberAt(value, path);
  if (number <= 0) return fail(path, 'a positive finite number');
  return number;
}

export function monotonicNanosecondsAt(value: unknown, path: string): Nanoseconds {
  return parseNanoseconds(value, path);
}

export function nullableMonotonicNanosecondsAt(
  value: unknown,
  path: string,
): Nanoseconds | null {
  return value === null ? null : monotonicNanosecondsAt(value, path);
}

export function boundedArrayAt(
  value: unknown,
  path: string,
  maximum: number,
): unknown[] {
  const array = arrayAt(value, path);
  if (array.length > maximum) {
    return fail(path, `an array with at most ${maximum} entries`);
  }
  return array;
}

export function jsonValueAt(value: unknown, path: string, depth = 0): JsonValue {
  if (depth > 32) return fail(path, 'a JSON value within the Program depth bound');
  if (value === null || typeof value === 'string' || typeof value === 'boolean') {
    return value;
  }
  if (typeof value === 'number') {
    if (!Number.isFinite(value) || (Number.isInteger(value) && !Number.isSafeInteger(value))) {
      return fail(path, 'a finite I-JSON number');
    }
    return value;
  }
  if (Array.isArray(value)) {
    return value.map((item, index) => jsonValueAt(item, `${path}[${index}]`, depth + 1));
  }
  const record = objectAt(value, path);
  return Object.fromEntries(
    Object.entries(record).map(([key, item]) => [
      key,
      jsonValueAt(item, `${path}.${key}`, depth + 1),
    ]),
  );
}

export function jsonRecordAt(value: unknown, path: string): Record<string, JsonValue> {
  const parsed = jsonValueAt(value, path);
  if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
    return fail(path, 'a JSON object');
  }
  return parsed;
}

export function nullableNumberAt(value: unknown, path: string): number | null {
  if (value === null) {
    return null;
  }
  return numberAt(value, path);
}

export function stringArrayAt(value: unknown, path: string): string[] {
  return arrayAt(value, path).map((item, index) =>
    stringAt(item, `${path}[${index}]`),
  );
}

export function canonicalStringArrayAt(value: unknown, path: string): string[] {
  const strings = stringArrayAt(value, path);
  const canonical = [...new Set(strings)].sort();
  if (
    canonical.length !== strings.length
    || canonical.some((item, index) => item !== strings[index])
  ) {
    return fail(path, 'a sorted array of unique strings');
  }
  return strings;
}

export function vector3At(value: unknown, path: string): Vector3Tuple {
  const array = arrayAt(value, path);
  if (array.length !== 3) {
    return fail(path, 'a three-number tuple');
  }
  return [
    numberAt(array[0], `${path}[0]`),
    numberAt(array[1], `${path}[1]`),
    numberAt(array[2], `${path}[2]`),
  ];
}

export function quaternionAt(value: unknown, path: string): QuaternionXyzwTuple {
  const array = arrayAt(value, path);
  if (array.length !== 4) {
    return fail(path, 'a four-number xyzw quaternion');
  }
  const quaternion: QuaternionXyzwTuple = [
    numberAt(array[0], `${path}[0]`),
    numberAt(array[1], `${path}[1]`),
    numberAt(array[2], `${path}[2]`),
    numberAt(array[3], `${path}[3]`),
  ];
  const norm = Math.hypot(...quaternion);
  if (norm < 1e-12) {
    return fail(path, 'a non-zero xyzw quaternion');
  }
  return quaternion;
}

export function stringRecordAt(value: unknown, path: string): Record<string, string> {
  const record = objectAt(value, path);
  return Object.fromEntries(
    Object.entries(record).map(([key, entry]) => [
      key,
      stringAt(entry, `${path}.${key}`),
    ]),
  );
}

export function parseProjectionRevision(value: unknown, path: string): ProjectionRevision {
  const record = objectAt(value, path);
  exactKeysAt(record, ['retained_bundle_id', 'graph_revision', 'scene_revision'], path);
  const scene = objectAt(record.scene_revision, `${path}.scene_revision`);
  exactKeysAt(scene, ['scene_id', 'revision_id', 'content_hash'], `${path}.scene_revision`);
  return {
    retained_bundle_id: stringAt(record.retained_bundle_id, `${path}.retained_bundle_id`),
    graph_revision: stringAt(record.graph_revision, `${path}.graph_revision`),
    scene_revision: {
      scene_id: stringAt(scene.scene_id, `${path}.scene_revision.scene_id`),
      revision_id: positiveIntegerAt(scene.revision_id, `${path}.scene_revision.revision_id`),
      content_hash: stringAt(scene.content_hash, `${path}.scene_revision.content_hash`),
    },
  };
}

export function nonEmptyStringAt(value: unknown, path: string): string {
  const parsed = stringAt(value, path);
  if (parsed.length === 0) return fail(path, 'a non-empty string');
  return parsed;
}

export function sourceFreshnessAt(value: unknown, path: string): SourceFreshness {
  const freshness = stringAt(value, path);
  if (!['fresh', 'stale', 'unavailable'].includes(freshness)) {
    return fail(path, 'fresh, stale, or unavailable');
  }
  return freshness as SourceFreshness;
}
