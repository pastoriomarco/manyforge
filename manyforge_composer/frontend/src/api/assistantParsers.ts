import type {
  AssistantModeIndex,
  AssistantProposal,
  AssistantProviderResponse,
  AssistantProviderStatus,
  AssistantRequestState,
  AssistantToolCallResult,
} from '../types/assistant';
import type { JsonValue } from '../types/api';

type JsonObject = Record<string, unknown>;
const UTF8_ENCODER = new TextEncoder();

function fail(path: string, message: string): never {
  throw new Error(`${path} must be ${message}`);
}

function objectAt(value: unknown, path: string): JsonObject {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    return fail(path, 'an object');
  }
  return value as JsonObject;
}

function exact(value: JsonObject, keys: string[], path: string): void {
  const actual = Object.keys(value).sort();
  const expected = [...keys].sort();
  if (actual.length !== expected.length || actual.some((key, index) => key !== expected[index])) {
    fail(path, `an exact ${expected.join(', ')} object`);
  }
}

function allowed(value: JsonObject, required: string[], optional: string[], path: string): void {
  const keys = Object.keys(value);
  if (
    required.some((key) => !keys.includes(key))
    || keys.some((key) => !required.includes(key) && !optional.includes(key))
  ) {
    fail(path, 'a supported closed object');
  }
}

function text(value: unknown, path: string, maximumBytes = 16_384): string {
  if (
    typeof value !== 'string'
    || !value
    || value !== value.trim()
    || UTF8_ENCODER.encode(value).byteLength > maximumBytes
  ) {
    return fail(path, 'bounded non-empty text');
  }
  return value;
}

function nullableText(value: unknown, path: string): string | null {
  return value === null ? null : text(value, path);
}

function bool(value: unknown, path: string): boolean {
  if (typeof value !== 'boolean') return fail(path, 'a boolean');
  return value;
}

function integer(value: unknown, path: string): number {
  if (!Number.isSafeInteger(value) || (value as number) < 0) {
    return fail(path, 'a non-negative safe integer');
  }
  return value as number;
}

function array(value: unknown, path: string, maximum = 4096): unknown[] {
  if (!Array.isArray(value) || value.length > maximum) return fail(path, 'a bounded array');
  return value;
}

function digest(value: unknown, path: string): string {
  const result = text(value, path);
  if (!/^sha256:[0-9a-f]{64}$/.test(result)) return fail(path, 'a SHA-256 digest');
  return result;
}

function jsonValue(value: unknown, path: string, depth = 0): JsonValue {
  if (depth > 64) return fail(path, 'bounded JSON');
  if (value === null || typeof value === 'string' || typeof value === 'boolean') return value;
  if (typeof value === 'number') {
    if (!Number.isFinite(value) || (Number.isInteger(value) && !Number.isSafeInteger(value))) {
      return fail(path, 'an I-JSON number');
    }
    return value;
  }
  if (Array.isArray(value)) return value.map((item, index) => jsonValue(item, `${path}[${index}]`, depth + 1));
  const record = objectAt(value, path);
  return Object.fromEntries(Object.entries(record).map(([key, item]) => [
    key,
    jsonValue(item, `${path}.${key}`, depth + 1),
  ]));
}

export function parseAssistantModeIndex(value: unknown): AssistantModeIndex {
  const root = objectAt(value, 'assistantModes');
  exact(root, [
    'assistant_projection_digest',
    'graph_revision',
    'instrument_registry_digest',
    'modes',
    'node_environment',
    'retained_bundle_id',
    'schema_version',
    'source',
  ], 'assistantModes');
  if (root.schema_version !== 'manyforge.assistant_mode_index.v1') {
    fail('assistantModes.schema_version', 'the supported version');
  }
  const source = objectAt(root.source, 'assistantModes.source');
  exact(source, [
    'available', 'freshness', 'known_gaps', 'projection_generation',
    'projection_owner_epoch', 'receiver_age_ns', 'selection_projection_generation',
    'selection_source_content_digest', 'selection_source_generation',
    'selection_source_owner_epoch', 'selection_source_owner_id', 'source_content_digest',
    'source_generation', 'source_owner_epoch', 'source_owner_id', 'source_owner_kind',
  ], 'assistantModes.source');
  if (bool(source.available, 'assistantModes.source.available') !== true
    || source.freshness !== 'fresh'
    || array(source.known_gaps, 'assistantModes.source.known_gaps').length !== 0
    || source.source_owner_kind !== 'assistant_mode_index') {
    fail('assistantModes.source', 'a fresh complete mode-index projection');
  }
  for (const field of [
    'projection_generation', 'receiver_age_ns', 'selection_projection_generation',
    'selection_source_generation', 'source_generation',
  ]) {
    integer(source[field], `assistantModes.source.${field}`);
  }
  for (const field of [
    'projection_owner_epoch', 'selection_source_owner_epoch',
    'selection_source_owner_id', 'source_owner_epoch', 'source_owner_id',
  ]) {
    text(source[field], `assistantModes.source.${field}`);
  }
  digest(source.selection_source_content_digest, 'assistantModes.source.selection_source_content_digest');
  digest(source.source_content_digest, 'assistantModes.source.source_content_digest');

  const environment = objectAt(root.node_environment, 'assistantModes.node_environment');
  exact(environment, [
    'catalog_digest', 'catalog_id', 'content_version', 'environment_digest',
  ], 'assistantModes.node_environment');
  digest(environment.catalog_digest, 'assistantModes.node_environment.catalog_digest');
  digest(environment.environment_digest, 'assistantModes.node_environment.environment_digest');
  text(environment.catalog_id, 'assistantModes.node_environment.catalog_id');
  text(environment.content_version, 'assistantModes.node_environment.content_version');
  const modes = array(root.modes, 'assistantModes.modes').map((raw, index) => {
    const row = objectAt(raw, `assistantModes.modes[${index}]`);
    exact(row, ['description', 'grant_policy', 'mode'], `assistantModes.modes[${index}]`);
    const policy = objectAt(row.grant_policy, `assistantModes.modes[${index}].grant_policy`);
    exact(policy, ['approval_mode', 'review_policy', 'session_kind'], `assistantModes.modes[${index}].grant_policy`);
    const reviewPolicy = text(policy.review_policy, 'grantPolicy.review_policy');
    if (!['required', 'bounded_by_recovery_profile', 'not_required'].includes(reviewPolicy)) {
      fail('grantPolicy.review_policy', 'a supported review policy');
    }
    const mode = text(row.mode, `assistantModes.modes[${index}].mode`, 256);
    if (!/^[a-z][a-z0-9.-]*$/.test(mode)) {
      fail(`assistantModes.modes[${index}].mode`, 'a canonical mode ID');
    }
    return {
      mode,
      description: text(row.description, `assistantModes.modes[${index}].description`),
      grantPolicy: {
        approval_mode: nullableText(policy.approval_mode, 'grantPolicy.approval_mode'),
        review_policy: reviewPolicy,
        session_kind: nullableText(policy.session_kind, 'grantPolicy.session_kind'),
      },
    };
  });
  const ids = modes.map((mode) => mode.mode);
  if (ids.some((id, index) => index > 0 && id <= ids[index - 1])) {
    fail('assistantModes.modes', 'strictly sorted unique modes');
  }
  digest(root.assistant_projection_digest, 'assistantModes.assistant_projection_digest');
  digest(root.instrument_registry_digest, 'assistantModes.instrument_registry_digest');
  return {
    retainedBundleId: text(root.retained_bundle_id, 'assistantModes.retained_bundle_id', 256),
    graphRevision: text(root.graph_revision, 'assistantModes.graph_revision', 256),
    assistantProjectionDigest: root.assistant_projection_digest as string,
    instrumentRegistryDigest: root.instrument_registry_digest as string,
    modes,
  };
}

function parseToolCall(value: unknown, path: string): AssistantToolCallResult {
  const row = objectAt(value, path);
  allowed(row, ['toolId', 'status', 'effect', 'arguments'], ['result', 'draftMutated'], path);
  const argumentsValue = jsonValue(row.arguments, `${path}.arguments`);
  if (argumentsValue === null || typeof argumentsValue !== 'object' || Array.isArray(argumentsValue)) {
    fail(`${path}.arguments`, 'an object');
  }
  const status = text(row.status, `${path}.status`, 64);
  const effect = text(row.effect, `${path}.effect`, 64);
  if (!['completed', 'failed', 'proposed'].includes(status)) {
    fail(`${path}.status`, 'a supported tool-call status');
  }
  if (!['read_only', 'proposal', 'composer_draft_mutating', 'runtime_mutating'].includes(effect)) {
    fail(`${path}.effect`, 'a supported tool effect');
  }
  return {
    toolId: text(row.toolId, `${path}.toolId`, 256),
    status,
    effect,
    arguments: argumentsValue,
    ...(row.result === undefined ? {} : { result: jsonValue(row.result, `${path}.result`) }),
    ...(row.draftMutated === undefined ? {} : { draftMutated: bool(row.draftMutated, `${path}.draftMutated`) }),
  };
}

function parseProposal(value: unknown, path: string): AssistantProposal {
  const row = objectAt(value, path);
  exact(row, [
    'toolId', 'effect', 'arguments', 'inputSchemaHash', 'status', 'requiresReview',
  ], path);
  const argumentsValue = jsonValue(row.arguments, `${path}.arguments`);
  if (argumentsValue === null || typeof argumentsValue !== 'object' || Array.isArray(argumentsValue)) {
    fail(`${path}.arguments`, 'an object');
  }
  const effect = text(row.effect, `${path}.effect`, 64);
  if (effect !== 'proposal' && effect !== 'runtime_mutating') {
    fail(`${path}.effect`, 'a proposal or runtime-mutating descriptor');
  }
  if (row.status !== 'draft' || bool(row.requiresReview, `${path}.requiresReview`) !== true) {
    fail(path, 'a review-only Draft proposal');
  }
  return {
    toolId: text(row.toolId, `${path}.toolId`, 256),
    effect,
    arguments: argumentsValue,
    inputSchemaHash: digest(row.inputSchemaHash, `${path}.inputSchemaHash`),
    status: 'draft' as const,
    requiresReview: true as const,
  };
}

function parseResponse(value: unknown): AssistantProviderResponse {
  const root = objectAt(value, 'assistantResponse');
  exact(root, [
    'schemaVersion', 'requestId', 'requestOwnerEpoch', 'sourceLabel', 'correlationId',
    'conversationId', 'assistantMode', 'catalogHash', 'message', 'proposals',
    'toolCalls', 'warnings', 'requiresReview', 'mutated', 'draftMutated',
    'needsClarification', 'exitReason', 'turnCount',
  ], 'assistantResponse');
  if (root.schemaVersion !== 'manyforge.assistant.provider_response.v1') {
    fail('assistantResponse.schemaVersion', 'the supported version');
  }
  if (bool(root.requiresReview, 'assistantResponse.requiresReview') !== true
    || bool(root.mutated, 'assistantResponse.mutated') !== false) {
    fail('assistantResponse', 'review-only and non-runtime-mutating');
  }
  return {
    requestId: text(root.requestId, 'assistantResponse.requestId', 256),
    requestOwnerEpoch: text(root.requestOwnerEpoch, 'assistantResponse.requestOwnerEpoch', 256),
    sourceLabel: text(root.sourceLabel, 'assistantResponse.sourceLabel', 256),
    correlationId: nullableText(root.correlationId, 'assistantResponse.correlationId'),
    conversationId: text(root.conversationId, 'assistantResponse.conversationId', 256),
    assistantMode: text(root.assistantMode, 'assistantResponse.assistantMode', 256),
    catalogHash: digest(root.catalogHash, 'assistantResponse.catalogHash'),
    message: typeof root.message === 'string'
      && UTF8_ENCODER.encode(root.message).byteLength <= 65_536
      ? root.message
      : fail('assistantResponse.message', 'bounded text'),
    proposals: array(root.proposals, 'assistantResponse.proposals')
      .map((item, index) => parseProposal(item, `assistantResponse.proposals[${index}]`)),
    toolCalls: array(root.toolCalls, 'assistantResponse.toolCalls').map((item, index) => parseToolCall(item, `assistantResponse.toolCalls[${index}]`)),
    warnings: array(root.warnings, 'assistantResponse.warnings').map((item, index) => text(item, `assistantResponse.warnings[${index}]`)),
    requiresReview: true,
    mutated: false,
    draftMutated: bool(root.draftMutated, 'assistantResponse.draftMutated'),
    needsClarification: bool(root.needsClarification, 'assistantResponse.needsClarification'),
    exitReason: text(root.exitReason, 'assistantResponse.exitReason'),
    turnCount: (() => {
      const count = integer(root.turnCount, 'assistantResponse.turnCount');
      return count >= 1 && count <= 64
        ? count
        : fail('assistantResponse.turnCount', 'between 1 and 64');
    })(),
  };
}

export function parseAssistantProviderStatus(value: unknown): AssistantProviderStatus {
  const root = objectAt(value, 'assistantStatus');
  exact(root, [
    'schemaVersion', 'requestId', 'requestOwnerEpoch', 'sourceLabel', 'correlationId',
    'conversationId', 'assistantMode', 'state', 'response', 'error',
  ], 'assistantStatus');
  if (root.schemaVersion !== 'manyforge.assistant.provider_status.v1') {
    fail('assistantStatus.schemaVersion', 'the supported version');
  }
  const state = text(root.state, 'assistantStatus.state') as AssistantRequestState;
  if (!['running', 'completed', 'failed', 'cancelled'].includes(state)) {
    fail('assistantStatus.state', 'a supported state');
  }
  const response = root.response === null ? null : parseResponse(root.response);
  let error: { code: string; message: string } | null = null;
  if (root.error !== null) {
    const rawError = objectAt(root.error, 'assistantStatus.error');
    exact(rawError, ['code', 'message'], 'assistantStatus.error');
    error = {
      code: text(rawError.code, 'assistantStatus.error.code'),
      message: text(rawError.message, 'assistantStatus.error.message'),
    };
  }
  const result = {
    requestId: text(root.requestId, 'assistantStatus.requestId', 256),
    requestOwnerEpoch: text(root.requestOwnerEpoch, 'assistantStatus.requestOwnerEpoch', 256),
    sourceLabel: text(root.sourceLabel, 'assistantStatus.sourceLabel', 256),
    correlationId: nullableText(root.correlationId, 'assistantStatus.correlationId'),
    conversationId: text(root.conversationId, 'assistantStatus.conversationId', 256),
    assistantMode: text(root.assistantMode, 'assistantStatus.assistantMode', 256),
    state,
    response,
    error,
  };
  if (state === 'completed' && response === null) fail('assistantStatus.response', 'present when completed');
  if (state === 'failed' && error === null) fail('assistantStatus.error', 'present when failed');
  if (state !== 'completed' && response !== null) fail('assistantStatus.response', 'absent unless completed');
  if (state !== 'failed' && error !== null) fail('assistantStatus.error', 'absent unless failed');
  if (response && (
    response.requestId !== result.requestId
    || response.requestOwnerEpoch !== result.requestOwnerEpoch
    || response.sourceLabel !== result.sourceLabel
    || response.correlationId !== result.correlationId
    || response.conversationId !== result.conversationId
    || response.assistantMode !== result.assistantMode
  )) {
    fail('assistantStatus.response', 'attributed to the enclosing request');
  }
  return result;
}
