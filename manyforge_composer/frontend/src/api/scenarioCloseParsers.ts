import type {
  CanonicalEmptySelection, ForceCloseResult, ForceCloseStep, LifecycleOwnerSettlementResult, LifecycleTargetRetirementIntent,
  LifecycleTargetRetirementOffer, LifecycleTargetRetirementResult,
  LifecycleTargetRetirementRow, LifecycleTargetRetirementScope,
} from '../types/scenarioClose';
import { normalizePublicRequestContext } from './processStopParsers';

function object(value: unknown, keys: string[]): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value)
    || Object.keys(value).length !== keys.length || keys.some((key) => !(key in value))) {
    throw new Error('Close owner response has an invalid closed shape.');
  }
  return value as Record<string, unknown>;
}
function identity(value: unknown): string {
  if (typeof value !== 'string' || value.length === 0 || value.normalize('NFC') !== value
    || new TextEncoder().encode(value).length > 128) throw new Error('Close CAS identity is invalid.');
  return value;
}
function generation(value: unknown): number {
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < 1) {
    throw new Error('Close CAS generation must be a positive safe integer.');
  }
  return value;
}
function digest(value: unknown): string {
  if (typeof value !== 'string' || !/^sha256:[0-9a-f]{64}$/.test(value)) throw new Error('Close digest is invalid.');
  return value;
}
function same(left: unknown, right: unknown): boolean {
  if (left === null || right === null || typeof left !== 'object' || typeof right !== 'object') return left === right;
  if (Array.isArray(left) || Array.isArray(right)) return Array.isArray(left) && Array.isArray(right)
    && left.length === right.length && left.every((item, index) => same(item, right[index]));
  const a = left as Record<string, unknown>; const b = right as Record<string, unknown>;
  return Object.keys(a).length === Object.keys(b).length && Object.keys(a).every((key) => key in b && same(a[key], b[key]));
}
function scope(value: unknown): LifecycleTargetRetirementScope {
  const row = object(value, ['request_owner_epoch', 'publication_generation', 'active_public_pointer_digest']);
  return { request_owner_epoch: identity(row.request_owner_epoch), publication_generation: generation(row.publication_generation),
    active_public_pointer_digest: digest(row.active_public_pointer_digest) };
}
function before(left: string, right: string): boolean {
  // C++ canonical component ordering is UTF-8 byte order, including non-ASCII IDs.
  const a = new TextEncoder().encode(left); const b = new TextEncoder().encode(right);
  for (let i = 0; i < Math.min(a.length, b.length); i += 1) if (a[i] !== b[i]) return a[i] < b[i];
  return a.length < b.length;
}
function targets(value: unknown): LifecycleTargetRetirementRow[] {
  if (!Array.isArray(value) || value.length > 64) throw new Error('Close targets exceed the existing capacity.');
  const rows = value.map((raw): LifecycleTargetRetirementRow => {
    const row = object(raw, ['controllable_component_id', 'requested_state', 'origin', 'target_generation', 'reached', 'recovered_cause']);
    if (row.requested_state !== 'active' && row.requested_state !== 'prepared' && row.requested_state !== 'disabled') {
      throw new Error('Close target state is invalid.');
    }
    if (row.origin !== 'startup' && row.origin !== 'operator' && row.origin !== 'program') throw new Error('Close target origin is invalid.');
    if (typeof row.reached !== 'boolean') throw new Error('Close reached fact must be boolean.');
    const causes = ['none', 'availability', 'emergency_stop', 'protective_stop', 'safeguard_stop',
      'fault', 'configuration', 'unknown', 'collision', 'external_control', 'provider_lost'];
    if (row.recovered_cause !== null && (typeof row.recovered_cause !== 'string' || !causes.includes(row.recovered_cause))) {
      throw new Error('Close recovered cause is invalid.');
    }
    return { controllable_component_id: identity(row.controllable_component_id), requested_state: row.requested_state,
      origin: row.origin, target_generation: generation(row.target_generation), reached: row.reached,
      recovered_cause: row.recovered_cause as LifecycleTargetRetirementRow['recovered_cause'] };
  });
  if (rows.some((row, index) => index > 0 && !before(rows[index - 1].controllable_component_id, row.controllable_component_id))) {
    throw new Error('Close targets must be unique and in canonical component order.');
  }
  return rows;
}
export function parseLifecycleTargetRetirementOffer(value: unknown): LifecycleTargetRetirementOffer {
  const result = object(value, ['scope', 'targets']);
  return { scope: scope(result.scope), targets: targets(result.targets) };
}
export function parseLifecycleTargetRetirementResult(value: unknown, expected: LifecycleTargetRetirementIntent): LifecycleTargetRetirementResult {
  const result = object(value, ['request_context', 'replay_status', 'scope', 'retired_targets']);
  if (result.replay_status !== 'accepted_new' && result.replay_status !== 'replayed_exact') {
    throw new Error('Close retirement was not accepted.');
  }
  if (!same(normalizePublicRequestContext(result.request_context), normalizePublicRequestContext(expected.request_context))) {
    throw new Error('Close retirement request identity does not match.');
  }
  const parsed: LifecycleTargetRetirementResult = { request_context: normalizePublicRequestContext(expected.request_context) as LifecycleTargetRetirementResult['request_context'],
    replay_status: result.replay_status, scope: scope(result.scope), retired_targets: targets(result.retired_targets) };
  if (!same(parsed.scope, expected.scope) || !same(parsed.retired_targets, expected.expected_targets)) {
    throw new Error('Close retirement does not match the complete CAS request.');
  }
  return parsed;
}
export function parseLifecycleTargetRetirementLookup(value: unknown, expected: LifecycleTargetRetirementIntent): LifecycleTargetRetirementResult | null {
  const response = object(value, ['result']);
  return response.result === null ? null : parseLifecycleTargetRetirementResult(response.result, expected);
}
export function parseLifecycleOwnerSettlement(value: unknown, expected: LifecycleTargetRetirementScope): LifecycleOwnerSettlementResult {
  const result = object(value, ['scope', 'settled']); const parsed = scope(result.scope);
  if (result.settled !== true || !same(parsed, expected)) throw new Error('Close owner settlement is not proven for this scope.');
  return { scope: parsed, settled: true };
}
export function parseCanonicalEmptySelection(value: unknown): CanonicalEmptySelection {
  const row = object(value, ['request_owner_epoch', 'cell_id', 'deployment_artifact_id', 'deployment_revision',
    'candidate_retained_bundle_id', 'candidate_graph_revision', 'admission_digest']);
  if (new TextEncoder().encode(JSON.stringify(row)).length > 16 * 1024) throw new Error('Canonical-empty selection exceeds its 16 KiB record bound.');
  return { request_owner_epoch: identity(row.request_owner_epoch), cell_id: identity(row.cell_id), deployment_artifact_id: identity(row.deployment_artifact_id),
    deployment_revision: digest(row.deployment_revision), candidate_retained_bundle_id: digest(row.candidate_retained_bundle_id),
    candidate_graph_revision: digest(row.candidate_graph_revision), admission_digest: digest(row.admission_digest) };
}
/** Item 37: the reply of one Force close step (`ok: true` only; refusals throw). */
export function parseForceCloseResult(value: unknown, step: ForceCloseStep): ForceCloseResult {
  const record = (raw: unknown): Record<string, unknown> => {
    if (raw === null || typeof raw !== 'object' || Array.isArray(raw)) throw new Error('Force close reply is invalid.');
    return raw as Record<string, unknown>;
  };
  const response = record(value);
  const result = record(response.result);
  // Night change 8: the `stop` step's reply is the Kernel's `arm` confirmation.
  if (response.ok !== true || response.command !== 'force_close' || result.step !== (step === 'stop' ? 'arm' : step)
    || !Array.isArray(result.operations)) throw new Error('Force close reply does not match its step.');
  if (step === 'end') return { step, stopGeneration: 0, motionBlockCleared: false, operations: [] };
  return { step, stopGeneration: generation(result.stop_generation), motionBlockCleared: result.motion_block_cleared === true,
    operations: result.operations.map((raw) => {
    const row = record(raw);
    if (!Array.isArray(row.targets) || typeof row.state !== 'string' || typeof row.containment_uncertain !== 'boolean') {
      throw new Error('Force close operation row is invalid.');
    }
    return { operationId: identity(row.operation_id), operationRevision: generation(row.operation_revision),
      state: row.state, containmentUncertain: row.containment_uncertain, targets: row.targets.map((target) => {
        const item = record(target);
        if (typeof item.output_unknown !== 'boolean') throw new Error('Force close target row is invalid.');
        return { controllableComponentId: identity(item.controllable_component_id),
          commandTargetId: identity(item.command_target_id), outputUnknown: item.output_unknown };
      }) };
  }) };
}
