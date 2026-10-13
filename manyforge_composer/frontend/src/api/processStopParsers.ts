import type { ComponentProcessStopRequest, ComponentProcessStopResult } from '../types/cclc';

type ObjectValue = Record<string, unknown>;
/** Optional correlation has one wire form: omit null or absent values. */
export function normalizePublicRequestContext(value: unknown): unknown {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return value;
  const result = { ...value } as ObjectValue;
  if (result.correlation_id == null) delete result.correlation_id;
  return result;
}
function normalizeProcessRequest(value: unknown): unknown {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return value;
  const result = { ...value } as ObjectValue;
  if ('request_context' in result) result.request_context = normalizePublicRequestContext(result.request_context);
  return result;
}
function object(value: unknown, keys: string[], label: string): ObjectValue {
  if (value === null || typeof value !== 'object' || Array.isArray(value)
    || Object.keys(value).length !== keys.length || keys.some((key) => !(key in value))) {
    throw new Error(`${label} has an invalid closed shape.`);
  }
  return value as ObjectValue;
}
function text(value: unknown, maximum: number, label: string, nonempty = true): string {
  if (typeof value !== 'string' || (nonempty && value.length === 0)
    || value.normalize('NFC') !== value || new TextEncoder().encode(value).length > maximum) {
    throw new Error(`${label} must be bounded NFC text.`);
  }
  return value;
}
/** Compare every field, including nested Scene and attribution, irrespective of key order. */
function exactEcho(value: unknown, expected: unknown): boolean {
  if (expected === null || typeof expected !== 'object') return value === expected;
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return false;
  const keys = Object.keys(expected);
  const row = value as ObjectValue;
  return Object.keys(row).length === keys.length && keys.every((key) => key in row
    && exactEcho(row[key], (expected as ObjectValue)[key]));
}
export function parseComponentProcessStopResult(
  value: unknown, expected: ComponentProcessStopRequest,
): ComponentProcessStopResult {
  const result = object(value, ['request', 'state', 'receipt', 'error'], 'Process Stop result');
  if (!exactEcho(normalizeProcessRequest(result.request), normalizeProcessRequest(expected))) {
    throw new Error('Process Stop result does not match the exact request.');
  }
  const state = result.state;
  if (state !== 'not_requested' && state !== 'pending' && state !== 'unknown' && state !== 'drained') {
    throw new Error('Process Stop has no known cleanup state.');
  }
  let error: ComponentProcessStopResult['error'] = null;
  if (result.error !== null) {
    const raw = object(result.error, ['code', 'message', 'details'], 'Process Stop error');
    if (raw.details === null || typeof raw.details !== 'object' || Array.isArray(raw.details)
      || Object.keys(raw.details).length > 4096) throw new Error('Process Stop error details are invalid.');
    const errorText = (value: unknown, nonempty = false): string => {
      if (typeof value !== 'string' || (nonempty && value.length === 0)) {
        throw new Error('Process Stop error must contain the existing string ErrorDTO.');
      }
      return value;
    };
    error = {
      code: errorText(raw.code, true),
      message: errorText(raw.message, true),
      details: Object.fromEntries(Object.entries(raw.details).map(([key, value]) => [
        errorText(key), errorText(value),
      ])),
    };
  }
  let receipt: ComponentProcessStopResult['receipt'] = null;
  if (state === 'drained') {
    const raw = object(result.receipt, ['leader_pid', 'controllable_component_id', 'launch_identity', 'observed_utc'], 'Drain receipt');
    if (raw.leader_pid !== expected.leader_pid || raw.controllable_component_id !== expected.controllable_component_id
      || raw.launch_identity !== expected.launch_identity || result.error !== null) {
      throw new Error('Process drain receipt does not match the exact installed launch.');
    }
    receipt = { leader_pid: expected.leader_pid,
      controllable_component_id: expected.controllable_component_id,
      launch_identity: expected.launch_identity, observed_utc: text(raw.observed_utc, 128, 'Drain observation time') };
  } else if (result.receipt !== null) throw new Error('Only drained may carry a process drain receipt.');
  return { request: normalizeProcessRequest(expected) as ComponentProcessStopRequest, state, receipt, error };
}

/** The passive offer carries the producer's actual scope and complete occurrences. */
export function parseComponentProcessStopOffer(value: unknown, componentId: string): import('../types/cclc').ComponentProcessStopOffer {
  const offer = object(value, ['controllable_component_id', 'process_inventories'], 'Process Stop offer');
  if (offer.controllable_component_id !== componentId || !Array.isArray(offer.process_inventories)) {
    throw new Error('Process Stop offer does not match the component.');
  }
  let count = 0;
  for (const value of offer.process_inventories) {
    const inventory = object(value, ['request', 'process_occurrences', 'error'], 'Process inventory');
    if (inventory.error !== null || !Array.isArray(inventory.process_occurrences)) throw new Error('Process inventory is unknown.');
    const scope = object(inventory.request, ['predecessor_projection_revision',
      'expected_predecessor_publication_generation', 'expected_predecessor_public_pointer_digest',
      'runtime_unit_id', 'runtime_epoch', 'manager_runtime_epoch'], 'Installed process scope');
    if (!Number.isSafeInteger(scope.expected_predecessor_publication_generation)
      || Number(scope.expected_predecessor_publication_generation) < 1) throw new Error('Process publication is invalid.');
    for (const value of inventory.process_occurrences) {
      const row = object(value, ['controllable_component_id', 'launch_identity', 'leader_pid',
        'process_presence', 'cleanup_state', 'receipt', 'installed_runtime_identity'], 'Installed process occurrence');
      if (++count > 4096 || row.controllable_component_id !== componentId
        || typeof row.launch_identity !== 'string' || !row.launch_identity
        || !Number.isInteger(row.leader_pid) || Number(row.leader_pid) < 1 || Number(row.leader_pid) > 2147483647
        || !['unknown', 'absent', 'present'].includes(String(row.process_presence))
        || !['not_requested', 'pending', 'unknown', 'drained'].includes(String(row.cleanup_state))) {
        throw new Error('Process Stop occurrence is invalid.');
      }
      if (row.cleanup_state === 'drained') {
        if (row.process_presence === 'present') throw new Error('Drained process is still present.');
        const receipt = object(row.receipt, ['leader_pid', 'controllable_component_id', 'launch_identity', 'observed_utc'], 'Drain receipt');
        if (receipt.leader_pid !== row.leader_pid || receipt.controllable_component_id !== componentId
          || receipt.launch_identity !== row.launch_identity) throw new Error('Drain receipt has another occurrence.');
        text(receipt.observed_utc, 128, 'Drain observation time');
      } else if (row.receipt !== null) throw new Error('Unresolved process has a drain receipt.');
    }
  }
  return offer as unknown as import('../types/cclc').ComponentProcessStopOffer;
}
