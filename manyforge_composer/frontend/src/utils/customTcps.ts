import type { JsonValue } from '../types/api';
import { rpyDegreesFromQuaternion } from '../state/robotControl';
import { draftComponents } from './componentManager';

/**
 * A custom TCP of one component, authored in the Deployment component row
 * (`custom_tcps: [{tcp_id, parent_frame, pose}]`). Its parent is one of the
 * component's own frames, never another custom TCP. Model (URDF/SRDF) and
 * descriptor TCPs are read-only; only these rows are edited in the GUI.
 */
export interface CustomTcp {
  tcpId: string;
  parentFrame: string;
  positionM: [number, number, number];
  rpyDeg: [number, number, number];
  /** The row as stored in the Draft; an edit of another row writes it back verbatim. */
  stored?: JsonValue;
}

const TCP_ID = /^[a-z][a-z0-9_]*$/;

function record(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, JsonValue>
    : null;
}

function triple(value: JsonValue | undefined): [number, number, number] | null {
  return Array.isArray(value) && value.length === 3
    && value.every((item) => typeof item === 'number' && Number.isFinite(item))
    ? value as [number, number, number]
    : null;
}

/** The component's row index in an intent Draft, or -1. */
export function componentRowIndex(document: Record<string, JsonValue> | null, componentId: string): number {
  const rows = document?.api_version === 'manyforge.deployment_intent.v2' ? document.components : null;
  return Array.isArray(rows)
    ? rows.findIndex((row) => record(row)?.component_id === componentId)
    : -1;
}

/**
 * The component's custom TCPs as authored. Rows that do not read are skipped
 * here; `customTcpsEdit` writes them back unchanged.
 */
export function customTcpsOf(document: Record<string, JsonValue> | null, componentId: string): CustomTcp[] {
  const row = draftComponents(document).find((candidate) => candidate.component_id === componentId);
  const rows = row?.custom_tcps;
  return (Array.isArray(rows) ? rows : []).flatMap((value): CustomTcp[] => {
    const tcp = record(value);
    const pose = record(tcp?.pose);
    const orientation = record(pose?.orientation);
    const position = triple(pose?.position_m);
    const rpy = triple(orientation?.rpy_deg);
    const xyzw = Array.isArray(orientation?.xyzw) && orientation.xyzw.length === 4
      && orientation.xyzw.every((item) => typeof item === 'number' && Number.isFinite(item))
      ? orientation.xyzw as [number, number, number, number] : null;
    if (typeof tcp?.tcp_id !== 'string' || typeof tcp.parent_frame !== 'string'
      || position === null || (rpy === null && xyzw === null)) return [];
    const fromQuaternion = xyzw === null ? null : rpyDegreesFromQuaternion({
      x: xyzw[0], y: xyzw[1], z: xyzw[2], w: xyzw[3],
    });
    return [{
      tcpId: tcp.tcp_id,
      parentFrame: tcp.parent_frame,
      positionM: [...position],
      rpyDeg: rpy !== null ? [...rpy] : fromQuaternion!,
      stored: value,
    }];
  });
}

/** The parent choices: the component's own frames, never a custom TCP. */
export function customTcpParentFrames(
  componentFrames: readonly string[],
  custom: readonly CustomTcp[],
): string[] {
  const customIds = new Set(custom.map((tcp) => tcp.tcpId));
  return [...new Set(componentFrames)].filter((frame) => !customIds.has(frame)).sort();
}

/**
 * Why a custom TCP cannot be saved, or null. `replacing` is the id of the row
 * being edited (its own id is not a clash).
 */
export function customTcpProblem(
  tcp: CustomTcp,
  existing: readonly CustomTcp[],
  componentFrames: readonly string[],
  replacing: string | null,
): string | null {
  if (!TCP_ID.test(tcp.tcpId)) return 'The TCP id must start with a lower-case letter and use only a-z, 0-9 and _.';
  if (componentFrames.includes(tcp.tcpId)) return `${tcp.tcpId} is already a frame of this component.`;
  if (existing.some((row) => row.tcpId === tcp.tcpId && row.tcpId !== replacing)) {
    return `${tcp.tcpId} is already a custom TCP of this component.`;
  }
  if (!customTcpParentFrames(componentFrames, existing).includes(tcp.parentFrame)) {
    return 'Pick the parent from this component\'s frames.';
  }
  if (![...tcp.positionM, ...tcp.rpyDeg].every(Number.isFinite)) return 'Position and orientation must be numbers.';
  return null;
}

function rowValue(tcp: CustomTcp): JsonValue {
  if (tcp.stored !== undefined) return tcp.stored;
  return {
    tcp_id: tcp.tcpId,
    parent_frame: tcp.parentFrame,
    pose: {
      position_m: [...tcp.positionM],
      orientation: { rpy_deg: [...tcp.rpyDeg] },
    },
  };
}

/**
 * The one Deployment Draft edit that stores a component's custom TCPs, sorted
 * by id (rows read from the Draft, with `stored`, are written back unchanged;
 * stored rows that do not read follow them untouched):
 * `set` of its `custom_tcps`, or `remove` when nothing is left. Null when the
 * component has no intent Draft row.
 */
export function customTcpsEdit(
  document: Record<string, JsonValue> | null,
  componentId: string,
  rows: readonly CustomTcp[],
): { path: Array<string | number>; operation: 'set' | 'remove'; value?: JsonValue } | null {
  const index = componentRowIndex(document, componentId);
  if (index < 0) return null;
  const path = ['components', index, 'custom_tcps'];
  const row = record((document!.components as JsonValue[])[index]);
  const storedRows = Array.isArray(row?.custom_tcps) ? row.custom_tcps : [];
  const readable = new Set(customTcpsOf(document, componentId).map((tcp) => tcp.stored));
  const unreadable = storedRows.filter((value) => !readable.has(value));
  if (rows.length === 0 && unreadable.length === 0) {
    return row !== null && 'custom_tcps' in row ? { path, operation: 'remove' } : null;
  }
  // Rows are stored sorted by id, the normalized form both validators expect.
  const sorted = [...rows].sort((left, right) => (left.tcpId < right.tcpId ? -1 : left.tcpId > right.tcpId ? 1 : 0));
  return { path, operation: 'set', value: [...sorted.map(rowValue), ...unreadable] };
}
