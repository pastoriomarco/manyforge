/**
 * Application-start identity, roles, HTTP request keys and id display
 * (bulk change 1, contracts 1.1, 1.2, 1.4 and 1.5).
 *
 * `A` is minted once per application start by the Manager and is the same in
 * every process for its whole life. It travels as 16 lowercase hex characters
 * and is never converted to a number (a JavaScript number cannot hold 64 bits).
 */

/** 16 lowercase hex characters, not all zeros. */
export type ApplicationStartId = string;

/** The fixed process and owner roles (contract 1.2), by their text. */
export type ApplicationRole =
  | 'manager'
  | 'logical_scene'
  | 'scene_runtime'
  | 'authored_scene_owner'
  | 'execution_host'
  | 'kernel'
  | 'program_runner'
  | 'headless'
  | 'headless_projection'
  | 'headless_alarm'
  | 'composer'
  | 'authoring_owner'
  | 'composer_snapshot'
  | 'manual_operation_owner'
  | 'intervention_or_recovery_owner'
  | 'pose_source';

const APPLICATION_START_ID = /^[0-9a-f]{16}$/;
const REQUEST_KEY = /^[0-9a-f]{32}$/;

export function isApplicationStartId(value: unknown): value is ApplicationStartId {
  return typeof value === 'string'
    && APPLICATION_START_ID.test(value)
    && value !== '0000000000000000';
}

export function isRequestKey(value: unknown): value is string {
  return typeof value === 'string' && REQUEST_KEY.test(value);
}

/** A fresh HTTP request key: 16 random bytes as 32 lowercase hex characters. */
export function newRequestKey(): string {
  const bytes = new Uint8Array(16);
  globalThis.crypto.getRandomValues(bytes);
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');
}

/**
 * Whether a recorded request key names this WebSocket command: a class 0 key
 * holds the session number in bytes 1-7 and the client sequence in bytes
 * 8-15, big-endian (contract 1.4). Byte 0 must name class 0; its part is not
 * compared.
 */
export function requestKeyNamesWebSocketCommand(
  requestKey: string,
  sessionNumber: number,
  clientSequence: string,
): boolean {
  const origin = BigInt(sessionNumber).toString(16).padStart(14, '0');
  const sequence = BigInt(clientSequence).toString(16).padStart(16, '0');
  return isRequestKey(requestKey)
    && Number.parseInt(requestKey.slice(0, 2), 16) >> 6 === 0
    && requestKey.slice(2) === `${origin}${sequence}`;
}

/** Kinds of start-minted integer ids a person reads (BS 2.9; `apply`: 1.16.1). */
export type DisplayIdKind = 'op' | 'stop' | 'run' | 'apply';

/** Short display form, for example `3f9a/op7`. */
export function displayId(
  applicationStartId: ApplicationStartId,
  kind: DisplayIdKind,
  value: number,
): string {
  return `${applicationStartId.slice(0, 4)}/${kind}${value}`;
}

/** The Manager's Apply number as a person reads it: `3f9a/apply4` (`apply4` before the start is known). */
export function displayApplyId(
  applicationStartId: ApplicationStartId | null | undefined,
  applyId: number,
): string {
  return applicationStartId == null
    ? `apply${applyId}`
    : displayId(applicationStartId, 'apply', applyId);
}

/** Full copy form, for example `<16 hex>/op7`. */
export function fullId(
  applicationStartId: ApplicationStartId,
  kind: DisplayIdKind,
  value: number,
): string {
  return `${applicationStartId}/${kind}${value}`;
}

/** A WebSocket command: session number and client sequence (`3f9a/s4.118`). */
export function displaySessionCommand(
  applicationStartId: ApplicationStartId,
  sessionNumber: number,
  clientSequence: number | string,
): string {
  return `${applicationStartId.slice(0, 4)}/s${sessionNumber}.${clientSequence}`;
}

/** An HTTP request key: its first 8 hex characters (`3f9a/k1a2b3c4d`). */
export function displayRequestKey(
  applicationStartId: ApplicationStartId,
  requestKey: string,
): string {
  return `${applicationStartId.slice(0, 4)}/k${requestKey.slice(0, 8)}`;
}

export interface ParsedIdFilter {
  /** The full `A` when the text gave all 16 characters, otherwise null. */
  applicationStartId: ApplicationStartId | null;
  /** The 4-character prefix when the text gave the display form, otherwise null. */
  applicationStartPrefix: string | null;
  value: number;
}

/**
 * Read an id typed into a filter: the display form (`3f9a/op7`), the full
 * form (`<16 hex>/op7`) or the bare number (`7`). Returns null for any other
 * text.
 */
export function parseIdFilter(text: string, kind: DisplayIdKind): ParsedIdFilter | null {
  const match = new RegExp(`^(?:([0-9a-f]{4}|[0-9a-f]{16})/${kind})?([1-9][0-9]{0,15})$`)
    .exec(text.trim().toLowerCase());
  if (match === null) return null;
  const value = Number(match[2]);
  if (!Number.isSafeInteger(value)) return null;
  const start = match[1] ?? null;
  if (start !== null && start.length === 16 && !isApplicationStartId(start)) return null;
  return {
    applicationStartId: start !== null && start.length === 16 ? start : null,
    applicationStartPrefix: start !== null && start.length === 4 ? start : null,
    value,
  };
}
