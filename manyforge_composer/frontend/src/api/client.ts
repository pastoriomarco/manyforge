import {
  parseAuthoringSubmissionOutcome,
  parseAuthoringWorkspaceSnapshot,
  parseCatalogResponse,
  parseAppliedProgramSelection,
  parseAppliedProgramStatus,
  parseApiErrorDto,
  parsePersistentKeyEdit,
  parseExpressionCheckResult,
  parsePersistentKeyValues,
  parsePersistentKeyValuesCleared,
  parseCapabilityCommandPreviewResponse,
  parseDraftMutationResponse,
  parseDeploymentCandidateOperationResponse,
  parseDeploymentWorkspaceResponse,
  parseFileBrowserResponse,
  parseHealthResponse,
  parseHistorianPage,
  parseHistorianDetail,
  parseHistorianCapturePage,
  parseHistorianDiscovery,
  parseExecutionIdentity,
  parseManagerApplyStatus,
  parseManagerDiffProjection,
  parseManagerGraphProjection,
  parseManagerStatusProjection,
  parseMotionPreviewResponse,
  parseManualControlAvailability,
  parseStopFaultRecoveryOffer,
  parseStopFaultRecoveryResult,
  parseNodeEnvironmentResponse,
  parseObjectResponse,
  parseProgramWorkspaceResponse,
  parseProgramWorkspaceTransitionResponse,
  parseCurrentScenarioResponse,
  parseRuntimeOverrideMaterializationResponse,
  parseRuntimeScaleControlStatus,
  parseScenarioDocumentIndexResponse,
  parseScenarioDocumentExportResponse,
  parseScenarioDocumentLoadResponse,
  parseScenarioDocumentSaveResponse,
  parseSceneTransactionResponse,
  parseSceneMeshAssetMetadata,
  parseProgramDraftMeshAssetMetadata,
  parseSceneUrdfManifest,
  parseSourceModelProjection,
} from './parsers';
import {
  parseComposerSettingsSnapshot,
  type ComposerSettings,
  type ComposerSettingsSnapshot,
} from '../state/composerSettings';
import {
  parseAssistantModeIndex,
  parseAssistantProviderStatus,
} from './assistantParsers';
import {
  parseCCLCAdmission,
  parseCCLCComponentSnapshot,
  parseCCLCOwnerIdentity,
  parseCCLCRouteTemplateSet,
  parseCCLCStatusEnvelope,
} from './cclcParsers';
import type {
  ArtifactKind,
  NewScenarioDocumentInput,
  AppliedProgramSelection,
  AppliedProgramStatus,
  PersistentKeyValues,
  PersistentKeyValuesCleared,
  ExpressionCheckRequest,
  ExpressionCheckResult,
  AuthoringWorkspaceSnapshot,
  CatalogResponse,
  DraftFenceRequest,
  LocalWorkspaceFenceRequest,
  DraftMutationResponse,
  DeploymentControlledStopUpdate,
  DeploymentCandidateOperationResponse,
  DeploymentCandidatePreparation,
  DeploymentWorkspaceResponse,
  FileBrowserResponse,
  HealthResponse,
  JsonValue,
  ManagerApplyStatus,
  NodeEnvironmentResponse,
  ManagerDiffProjection,
  ManagerGraphProjection,
  ManagerStatusProjection,
  ProgramWorkspaceResponse,
  ProgramWorkspaceTransitionResponse,
  CurrentScenarioResponse,
  RuntimeOverrideMaterializationResponse,
  ScenarioDocumentFenceRequest,
  ScenarioDocumentExportResponse,
  ScenarioDocumentIndexResponse,
  ScenarioDocumentLoadResponse,
  ScenarioDocumentSaveResponse,
  SceneTransactionRequest,
  SceneTransactionResponse,
  SceneMeshAssetMetadata,
  ProgramDraftMeshAssetMetadata,
  SceneUrdfManifest,
  SourceModelProjection,
  StructuredError,
} from '../types/api';
import type {
  AlarmMembership,
  HistorianPage,
  HistorianDetail,
  HistorianCapturePage,
  HistorianDiscovery,
  OperatorAlarmRecord,
} from '../types/headless';
import type {
  CapabilityCommandPreviewReceipt,
  DirectControlOperationIntent,
  ExecutionIdentity,
  MotionPreviewReceipt,
  ManualControlAvailability,
  StopFaultRecoveryOffer,
  RuntimeScaleControlSelector,
  RuntimeScaleControlStatus,
} from '../types/robotControl';
import type {
  AssistantModeIndex,
  AssistantProviderStatus,
  AssistantRequestInput,
} from '../types/assistant';
import type {
  CCLCAdmission,
  CCLCComponentSnapshot,
  CCLCOwnerIdentity,
  CCLCLifecycleRequestBody,
  CCLCRouteRequestBody,
  CCLCRouteTemplateSet,
  CCLCTransactionStatus,
} from '../types/cclc';

export interface HistorianListQuery {
  startNs?: string;
  endNs?: string;
  components?: string[];
  eventTypes?: string[];
  beforeSequence?: number;
  limit?: number;
  startUtcNs?: string;
  endUtcNs?: string;
  recentSeconds?: number;
  clockOwner?: string;
  severities?: string[];
  programRunId?: string;
  operationId?: string;
  actionSpanId?: string;
  nodeInstanceId?: string;
  interventionSessionId?: string;
  contactPolicyId?: string;
  incidentId?: string;
  conditionId?: string;
  channel?: string;
  alarmPhase?: 'all' | 'activated' | 'returned' | 'acknowledged' | 'raw';
  search?: string;
  /** Restrict to one retained failure capture's protected range. */
  captureId?: number;
}

export const HISTORIAN_PAGE_LIMIT = 32;

function historianNanoseconds(value: string, label: string): string {
  if (
    value.length > 19
    || !/^(0|[1-9]\d*)$/.test(value)
    || BigInt(value) > 9_223_372_036_854_775_807n
  ) {
    throw new Error(`${label} must be a canonical nonnegative int64 decimal string`);
  }
  return value;
}

export function historianListPath(query: HistorianListQuery = {}): string {
  const limit = query.limit ?? HISTORIAN_PAGE_LIMIT;
  if (!Number.isSafeInteger(limit) || limit < 1 || limit > HISTORIAN_PAGE_LIMIT) {
    throw new Error(`History page limit must be an integer from 1 to ${HISTORIAN_PAGE_LIMIT}`);
  }
  const parameters = new URLSearchParams();
  parameters.set('limit', String(limit));
  if (query.startNs !== undefined) {
    parameters.set('start_ns', historianNanoseconds(query.startNs, 'History start'));
  }
  if (query.endNs !== undefined) {
    parameters.set('end_ns', historianNanoseconds(query.endNs, 'History end'));
  }
  for (const [key, value] of [['start_utc_ns', query.startUtcNs], ['end_utc_ns', query.endUtcNs]] as const) {
    if (value !== undefined) parameters.set(key, historianNanoseconds(value, key));
  }
  for (const [key, value] of [
    ['recent_seconds', query.recentSeconds], ['clock_owner', query.clockOwner],
    ['program_run_id', query.programRunId], ['operation_id', query.operationId],
    ['action_span_id', query.actionSpanId], ['node_instance_id', query.nodeInstanceId],
    ['intervention_session_id', query.interventionSessionId], ['contact_policy_id', query.contactPolicyId],
    ['incident_id', query.incidentId], ['condition_id', query.conditionId],
    ['channel', query.channel],
    ['alarm_phase', query.alarmPhase], ['search', query.search],
  ] as const) {
    if (value !== undefined) parameters.set(key, String(value));
  }
  if (query.captureId !== undefined) {
    if (!Number.isSafeInteger(query.captureId) || query.captureId < 1) {
      throw new Error('Failure capture id must be a positive safe integer');
    }
    parameters.set('capture_id', String(query.captureId));
  }
  for (const severity of query.severities ?? []) parameters.append('severities', severity);
  if (query.beforeSequence !== undefined) {
    parameters.set('before_sequence', String(query.beforeSequence));
  }
  for (const component of query.components ?? []) parameters.append('components', component);
  for (const eventType of query.eventTypes ?? []) parameters.append('event_types', eventType);
  return `/api/execution/history?${parameters.toString()}`;
}

const BASE_URL = import.meta.env.VITE_API_URL || '';

export function resolveStructuralAssetUrl(path: string): string {
  if (/^https?:\/\//i.test(path) || path.startsWith('blob:')) {
    return path;
  }
  if (!BASE_URL) {
    return path;
  }
  return `${BASE_URL.replace(/\/$/, '')}/${path.replace(/^\//, '')}`;
}

function errorMessageFromPayload(payload: unknown, fallback: string): string {
  if (payload === null || typeof payload !== 'object' || Array.isArray(payload)) {
    return fallback;
  }
  const record = payload as Record<string, unknown>;
  if (typeof record.detail === 'string' && record.detail.trim()) {
    return record.detail;
  }
  if (
    record.detail !== null &&
    typeof record.detail === 'object' &&
    !Array.isArray(record.detail)
  ) {
    const detail = record.detail as Record<string, unknown>;
    if (typeof detail.message === 'string' && detail.message.trim()) {
      return detail.message;
    }
    if (typeof detail.code === 'string' && detail.code.trim()) {
      return detail.code;
    }
  }
  if (typeof record.message === 'string' && record.message.trim()) {
    return record.message;
  }
  if (
    record.error !== null
    && typeof record.error === 'object'
    && !Array.isArray(record.error)
  ) {
    const error = record.error as Record<string, unknown>;
    if (typeof error.message === 'string' && error.message.trim()) {
      return error.message;
    }
    if (typeof error.code === 'string' && error.code.trim()) {
      return error.code;
    }
  }
  return fallback;
}

function runtimeScaleSelectorQuery(selector: RuntimeScaleControlSelector): string {
  const parameters = new URLSearchParams();
  parameters.set('program_binding_id', selector.programBindingId);
  if (selector.kind === 'singular') {
    parameters.set('component_role_id', selector.componentRoleId);
    parameters.set('target_role_id', selector.targetRoleId);
  } else {
    parameters.set('group_targets', JSON.stringify(selector.groupTargets.map((target) => ({
      component_role_id: target.componentRoleId,
      target_role_id: target.targetRoleId,
    }))));
  }
  return parameters.toString();
}

function sameRuntimeScaleSelector(
  requested: RuntimeScaleControlSelector,
  returned: RuntimeScaleControlSelector,
): boolean {
  if (requested.kind !== returned.kind
    || requested.programBindingId !== returned.programBindingId) return false;
  if (requested.kind === 'singular' && returned.kind === 'singular') {
    return requested.componentRoleId === returned.componentRoleId
      && requested.targetRoleId === returned.targetRoleId;
  }
  if (requested.kind !== 'group' || returned.kind !== 'group') return false;
  const identity = (target: { componentRoleId: string; targetRoleId: string }) => (
    JSON.stringify([target.componentRoleId, target.targetRoleId])
  );
  const requestedTargets = new Set(requested.groupTargets.map(identity));
  const returnedTargets = new Set(returned.groupTargets.map(identity));
  return requestedTargets.size === returnedTargets.size
    && [...requestedTargets].every((target) => returnedTargets.has(target));
}

/** The owner-issued error code carried by an API error, if any. */
export function apiErrorCode(error: unknown): string | null {
  return error instanceof ApiError ? error.structuredError?.code ?? null : null;
}

export class ApiError extends Error {
  readonly status: number;
  readonly payload: unknown;
  /** The refusal's ErrorDTO, when the body carries one (origin is the request). */
  readonly structuredError: StructuredError | null;

  constructor(message: string, status: number, payload: unknown) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.payload = payload;
    this.structuredError = parseApiErrorDto(payload);
  }
}

/**
 * The named causes for which "Use current robot pose/joints" refuses before any
 * Draft change: one code per thing the operator has to correct.
 */
const OBSERVED_MOVE_REFUSAL_CODES = new Set([
  'observed_move_node_unsupported',
  'observed_move_role_missing',
  'observed_move_configuration_changed',
  'observed_move_target_unbound',
  'observed_move_target_unavailable',
  'observed_move_frame_mismatch',
  'observed_move_unavailable',
]);

/** Remedies the owner's message does not already name (it lies outside the Draft). */
const OBSERVED_MOVE_REFUSAL_GUIDANCE: Readonly<Record<string, string>> = {
  observed_move_target_unavailable: 'Check that the robot is running in the current configuration.',
  observed_move_unavailable: 'Wait for the robot to report its current state, then try again.',
};

/**
 * Only for authoring requests; never retry an edit after a refused fence.
 *
 * A refusal is recognised only by an explicit, named code in the backend's
 * structured error body, never by a bare status: a 502/503 without such a code
 * (a proxy or gateway failure, a malformed body) may hide a request that
 * succeeded before its reply was lost, so it stays unconfirmed.
 */
export function authoringRefusalMessage(error: unknown): string | null {
  if (!(error instanceof ApiError)) return null;
  const code = apiErrorCode(error);
  // The observed-move resolver refuses these before preparing or installing a
  // Draft mutation; no submission outcome remains to reconcile. The code alone
  // proves it, whatever the status (an unreachable owner is reported as 503).
  if (code !== null && OBSERVED_MOVE_REFUSAL_CODES.has(code)) {
    const guidance = OBSERVED_MOVE_REFUSAL_GUIDANCE[code];
    if (!guidance) return `Edit not applied: ${error.message}`;
    return `Edit not applied: ${error.message.replace(/[.\s]*$/, '')}. ${guidance}`;
  }
  if (error.status !== 409) return null;
  if (code === 'authoring_not_ready') return 'Edit not applied: authoring is currently unavailable.';
  if (code === 'stale_generation') return 'Edit not applied: backend state changed. Review the current state before trying again.';
  // Preparation runs before any claim or runtime effect, so it is a definite
  // refusal of this input, never an unconfirmed submission.
  if (code === 'activation_preparation_failed') {
    const details = error.structuredError?.details?.preparer_detail;
    const cause = typeof details === 'string'
      ? details.replace(/^manyforge_prepare_activation:\s*/, '').trim() : '';
    return `Not applied: the candidate could not be prepared${cause ? ` (${cause})` : ''}. Correct it and try again.`;
  }
  return null;
}

const STALE_WORKSPACE_CODES = new Set([
  // The composite workspace or a Draft identity moved under this request.
  'composer_workspace_stale',
  'composer_draft_id_stale',
  'composer_draft_revision_stale',
  'scenario_workspace_stale',
  // The artifact was written, but a newer workspace revision refused to be
  // labelled Saved; the client must adopt the newer identity, not retry.
  'program_saved_baseline_not_adopted',
  'deployment_saved_baseline_not_adopted',
]);

/**
 * Recognize one typed stale rejection.
 *
 * A stale fence means another client's accepted edit moved the workspace, so the
 * only correct response is to refresh the observed identity. It is never a blind
 * retry of the same request.
 */
export function isStaleWorkspaceError(error: unknown): boolean {
  if (!(error instanceof ApiError) || error.status !== 409) return false;
  const code = apiErrorCode(error);
  return code !== null && STALE_WORKSPACE_CODES.has(code);
}

export type AuthoringRequestIdentity = {
  requestOwnerEpoch: string;
  requestId: string;
  expectedAdmissionGeneration: number;
  path?: string;
};

const failedAuthoringRequests = new WeakMap<object, AuthoringRequestIdentity>();

export function failedAuthoringRequest(error: unknown): AuthoringRequestIdentity | null {
  return error !== null && typeof error === 'object'
    ? failedAuthoringRequests.get(error) ?? null : null;
}

export interface PersistentKeyRequestIdentity {
  request_id: string;
  request_owner_epoch: string;
  source_label: string;
}

/**
 * What the operator saw: the live run holding the values and its revision.
 * Without a run the last admitted edit wins (nothing to name); a resent
 * request is still refused by its identity.
 */
export type PersistentKeyValuesFence =
  | { program_run_id: string; expected_run_revision: number }
  | { program_run_id?: never; expected_run_revision?: never };

export type PersistentKeyValueEditBody = PersistentKeyRequestIdentity & PersistentKeyValuesFence & {
  scenario_id: string;
  key: string;
  expected_program_revision: string;
  value?: JsonValue;
};

function persistentValuesPath(scenarioId: string, suffix = ''): string {
  return `/api/program/scenarios/${encodeURIComponent(scenarioId)}/persistent-values${suffix}`;
}

async function requestJson<T>(
  path: string,
  parser: (value: unknown) => T,
  signal?: AbortSignal,
  init: RequestInit = {},
): Promise<T> {
  try {
    return await requestJsonResponse(path, parser, signal, init);
  } catch (error) {
    // Preserve the original error/classification, including parse failures.
    // The exact sent envelope is only readback evidence, never a replay request.
    if (error !== null && typeof error === 'object' && typeof init.body === 'string') {
      const body = JSON.parse(init.body) as Partial<AuthoringRequestIdentity>;
      if (typeof body.requestOwnerEpoch === 'string' && typeof body.requestId === 'string'
        && Number.isSafeInteger(body.expectedAdmissionGeneration) && body.expectedAdmissionGeneration! > 0) {
        failedAuthoringRequests.set(error, {
          requestOwnerEpoch: body.requestOwnerEpoch, requestId: body.requestId,
          expectedAdmissionGeneration: body.expectedAdmissionGeneration!,
          path,
        });
      }
    }
    throw error;
  }
}

async function requestJsonResponse<T>(
  path: string,
  parser: (value: unknown) => T,
  signal?: AbortSignal,
  init: RequestInit = {},
): Promise<T> {
  const headers = new Headers(init.headers);
  headers.set('Accept', 'application/json');
  if (init.body !== undefined) headers.set('Content-Type', 'application/json');
  const response = await fetch(resolveStructuralAssetUrl(path), {
    method: init.method ?? 'GET',
    headers,
    body: init.body,
    signal,
  });
  let payload: unknown;
  try {
    payload = await response.json();
  } catch {
    if (!response.ok) {
      throw new ApiError(
        response.statusText || `HTTP ${response.status}`,
        response.status,
        null,
      );
    }
    throw new Error(`Invalid JSON response from ${path}`);
  }
  if (!response.ok) {
    throw new ApiError(
      errorMessageFromPayload(
        payload,
        response.statusText || `HTTP ${response.status}`,
      ),
      response.status,
      payload,
    );
  }
  try {
    return parser(payload);
  } catch (error) {
    const message = error instanceof Error ? error.message : 'invalid payload';
    throw new Error(`Invalid response from ${path}: ${message}`);
  }
}

const COMPOSER_SETTINGS_PATH = '/api/composer/settings';

function composerSettingsRevision(expectedRevision: number): RequestInit {
  return { method: 'POST', body: JSON.stringify({ expectedRevision }) };
}

/** The current settings a revision conflict (HTTP 409) carries, if any. */
export function composerSettingsConflict(error: unknown): ComposerSettingsSnapshot | null {
  if (!(error instanceof ApiError) || error.status !== 409) return null;
  if (apiErrorCode(error) !== 'composer_settings_revision_stale') return null;
  const detail = (error.payload as { detail?: { current?: unknown } }).detail;
  try {
    return parseComposerSettingsSnapshot(detail?.current);
  } catch {
    return null;
  }
}

export const api = {
  health: {
    get: (signal?: AbortSignal): Promise<HealthResponse> =>
      requestJson('/api/health', parseHealthResponse, signal),
  },
  /** Installation Composer settings; Composer HTTP only, revision-checked. */
  composerSettings: {
    read: (signal?: AbortSignal): Promise<ComposerSettingsSnapshot> =>
      requestJson(COMPOSER_SETTINGS_PATH, parseComposerSettingsSnapshot, signal),
    replace: (expectedRevision: number, settings: ComposerSettings): Promise<ComposerSettingsSnapshot> =>
      requestJson(COMPOSER_SETTINGS_PATH, parseComposerSettingsSnapshot, undefined, {
        method: 'PUT',
        body: JSON.stringify({ expectedRevision, settings }),
      }),
    restoreDefaults: (expectedRevision: number): Promise<ComposerSettingsSnapshot> =>
      requestJson(`${COMPOSER_SETTINGS_PATH}/restore-defaults`, parseComposerSettingsSnapshot,
        undefined, composerSettingsRevision(expectedRevision)),
    resetDamaged: (expectedRevision: number): Promise<ComposerSettingsSnapshot> =>
      requestJson(`${COMPOSER_SETTINGS_PATH}/reset-damaged`, parseComposerSettingsSnapshot,
        undefined, composerSettingsRevision(expectedRevision)),
    saveProfile: (expectedRevision: number, name: string): Promise<ComposerSettingsSnapshot> =>
      requestJson(`${COMPOSER_SETTINGS_PATH}/profiles/${encodeURIComponent(name)}`,
        parseComposerSettingsSnapshot, undefined, composerSettingsRevision(expectedRevision)),
    loadProfile: (expectedRevision: number, name: string): Promise<ComposerSettingsSnapshot> =>
      requestJson(`${COMPOSER_SETTINGS_PATH}/profiles/${encodeURIComponent(name)}/load`,
        parseComposerSettingsSnapshot, undefined, composerSettingsRevision(expectedRevision)),
    deleteProfile: (expectedRevision: number, name: string): Promise<ComposerSettingsSnapshot> =>
      requestJson(
        `${COMPOSER_SETTINGS_PATH}/profiles/${encodeURIComponent(name)}?expectedRevision=${expectedRevision}`,
        parseComposerSettingsSnapshot, undefined, { method: 'DELETE' },
      ),
  },
  catalog: {
    list: (signal?: AbortSignal): Promise<CatalogResponse> =>
      requestJson('/api/catalog', parseCatalogResponse, signal),
  },
  nodes: {
    environment: (signal?: AbortSignal): Promise<NodeEnvironmentResponse> =>
      requestJson(
        '/api/nodes/environment',
        parseNodeEnvironmentResponse,
        signal,
      ),
  },
  sourceModel: {
    get: (signal?: AbortSignal): Promise<SourceModelProjection> =>
      requestJson('/api/source-model', parseSourceModelProjection, signal),
  },
  cclc: {
    owner: (signal?: AbortSignal): Promise<CCLCOwnerIdentity> => requestJson(
      '/api/cclc/owner',
      parseCCLCOwnerIdentity,
      signal,
    ),
    component: (
      componentId: string,
      signal?: AbortSignal,
    ): Promise<CCLCComponentSnapshot> => requestJson(
      `/api/cclc/components/${encodeURIComponent(componentId)}`,
      (value) => {
        const snapshot = parseCCLCComponentSnapshot(value);
        if (snapshot.componentId !== componentId) {
          throw new Error('component snapshot does not match its request');
        }
        return snapshot;
      },
      signal,
    ),
    routeTemplates: (
      componentId: string,
      signal?: AbortSignal,
    ): Promise<CCLCRouteTemplateSet> => requestJson(
      `/api/cclc/components/${encodeURIComponent(componentId)}/route-templates`,
      (value) => {
        const templates = parseCCLCRouteTemplateSet(value);
        if (templates.componentId !== componentId) {
          throw new Error('route templates do not match their request');
        }
        return templates;
      },
      signal,
    ),
    submitLifecycle: (
      request: CCLCLifecycleRequestBody,
      signal?: AbortSignal,
    ): Promise<CCLCAdmission> => requestJson(
      '/api/cclc/lifecycle/requests',
      (value) => {
        const result = parseCCLCAdmission(value, 'lifecycle');
        if (
          result.requestId !== null
          && result.requestId !== request.request_context.request_id
        ) {
          throw new Error('lifecycle acceptance does not match its request');
        }
        return result;
      },
      signal,
      { method: 'POST', body: JSON.stringify(request) },
    ),
    lifecycleStatus: (
      transactionId: string,
      signal?: AbortSignal,
    ): Promise<CCLCTransactionStatus | null> => requestJson(
      `/api/cclc/lifecycle/transactions/${encodeURIComponent(transactionId)}`,
      (value) => {
        const status = parseCCLCStatusEnvelope(value, 'lifecycle');
        if (status !== null && status.transactionId !== transactionId) {
          throw new Error('lifecycle status does not match its request');
        }
        return status;
      },
      signal,
    ),
    submitRoute: (
      request: CCLCRouteRequestBody,
      signal?: AbortSignal,
    ): Promise<CCLCAdmission> => requestJson(
      '/api/cclc/routes/requests',
      (value) => {
        const result = parseCCLCAdmission(value, 'route');
        if (
          result.requestId !== null
          && result.requestId !== request.request_context.request_id
        ) {
          throw new Error('route acceptance does not match its request');
        }
        return result;
      },
      signal,
      { method: 'POST', body: JSON.stringify(request) },
    ),
    routeStatus: (
      transactionId: string,
      signal?: AbortSignal,
    ): Promise<CCLCTransactionStatus | null> => requestJson(
      `/api/cclc/routes/transactions/${encodeURIComponent(transactionId)}`,
      (value) => {
        const status = parseCCLCStatusEnvelope(value, 'route');
        if (status !== null && status.transactionId !== transactionId) {
          throw new Error('route status does not match its request');
        }
        return status;
      },
      signal,
    ),
  },
  authoringWorkspace: {
    submissionOutcome: (request: AuthoringRequestIdentity) => requestJson(
      `/api/authoring/submission-outcome?${new URLSearchParams({
        request_owner_epoch: request.requestOwnerEpoch,
        request_id: request.requestId,
        expected_admission_generation: String(request.expectedAdmissionGeneration),
      })}`,
      (value) => parseAuthoringSubmissionOutcome(value, request),
    ),
    /**
     * Read the one atomic composite authoring snapshot.
     *
     * Every authoring consumer takes its identity, both Draft projections and
     * the Scenario project facts from this single read, so no consumer
     * establishes an independent authoring identity.
     */
    get: (signal?: AbortSignal): Promise<AuthoringWorkspaceSnapshot> =>
      requestJson(
        '/api/authoring/workspace',
        parseAuthoringWorkspaceSnapshot,
        signal,
      ),
  },
  programWorkspace: {
    get: (signal?: AbortSignal): Promise<ProgramWorkspaceResponse> =>
      requestJson(
        '/api/program/workspace',
        parseProgramWorkspaceResponse,
        signal,
      ),
    mutate: (
      fence: LocalWorkspaceFenceRequest,
      mutation: string,
      args: Record<string, JsonValue>,
      signal?: AbortSignal,
    ): Promise<DraftMutationResponse> => requestJson(
      '/api/program/draft/mutations',
      parseDraftMutationResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({ ...fence, mutation, arguments: args }),
      },
    ),
    /** Check one formula against the current Draft's parameters and keys; never changes the Draft. */
    checkExpression: (
      request: ExpressionCheckRequest,
      signal?: AbortSignal,
    ): Promise<ExpressionCheckResult> => requestJson(
      '/api/program/draft/expression-check',
      parseExpressionCheckResult,
      signal,
      { method: 'POST', body: JSON.stringify(request) },
    ),
    openDraft: (
      fence: LocalWorkspaceFenceRequest,
      path: string,
      confirmDiscard: boolean,
      signal?: AbortSignal,
    ): Promise<ProgramWorkspaceResponse> => requestJson(
      '/api/program/open-draft',
      parseProgramWorkspaceResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({ ...fence, path, confirmDiscard }),
      },
    ),
    save: (
      fence: LocalWorkspaceFenceRequest,
      signal?: AbortSignal,
    ): Promise<ProgramWorkspaceResponse> => requestJson(
      '/api/program/save',
      parseProgramWorkspaceResponse,
      signal,
      { method: 'POST', body: JSON.stringify(fence) },
    ),
    saveAs: (
      fence: LocalWorkspaceFenceRequest,
      path: string,
      confirmOverwrite: boolean,
      signal?: AbortSignal,
    ): Promise<ProgramWorkspaceResponse> => requestJson(
      '/api/program/save-as',
      parseProgramWorkspaceResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({ ...fence, path, confirmOverwrite }),
      },
    ),
    apply: (
      fence: DraftFenceRequest,
      signal?: AbortSignal,
    ): Promise<ProgramWorkspaceTransitionResponse> => requestJson(
      '/api/program/apply',
      parseProgramWorkspaceTransitionResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify(fence),
      },
    ),
    discard: (
      fence: LocalWorkspaceFenceRequest,
      signal?: AbortSignal,
    ): Promise<ProgramWorkspaceResponse> => requestJson(
      '/api/program/discard',
      parseProgramWorkspaceResponse,
      signal,
      { method: 'POST', body: JSON.stringify(fence) },
    ),
    newDraft: (
      fence: LocalWorkspaceFenceRequest,
      confirmDiscard: boolean,
      signal?: AbortSignal,
    ): Promise<ProgramWorkspaceResponse> => requestJson(
      '/api/program/new-draft',
      parseProgramWorkspaceResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({ ...fence, confirmDiscard }),
      },
    ),
    revert: (
      fence: DraftFenceRequest,
      signal?: AbortSignal,
    ): Promise<ProgramWorkspaceTransitionResponse> => requestJson(
      '/api/program/revert',
      parseProgramWorkspaceTransitionResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify(fence),
      },
    ),
    materializeRuntimeOverride: (
      fence: LocalWorkspaceFenceRequest,
      programRunId: string,
      proposalId: string,
      signal?: AbortSignal,
    ): Promise<RuntimeOverrideMaterializationResponse> => requestJson(
      '/api/program/draft/runtime-override-promotions',
      parseRuntimeOverrideMaterializationResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({
          ...fence,
          programRunId,
          proposalId,
        }),
      },
    ),
  },
  scenarioDocuments: {
    new: (
      fence: ScenarioDocumentFenceRequest,
      input: NewScenarioDocumentInput,
      signal?: AbortSignal,
    ): Promise<ScenarioDocumentLoadResponse> => requestJson(
      '/api/scenario-documents/new', parseScenarioDocumentLoadResponse, signal,
      { method: 'POST', body: JSON.stringify({ ...fence, ...input }) },
    ),
    list: (
      directory?: string,
      signal?: AbortSignal,
    ): Promise<ScenarioDocumentIndexResponse> => requestJson(
      `/api/scenario-documents${directory ? `?directory=${encodeURIComponent(directory)}` : ''}`,
      parseScenarioDocumentIndexResponse,
      signal,
    ),
    save: (
      fence: ScenarioDocumentFenceRequest,
      input: {
        path: string;
        scenarioId: string;
        name: string;
        description: string;
        confirmOverwrite: boolean;
      },
      signal?: AbortSignal,
    ): Promise<ScenarioDocumentSaveResponse> => requestJson(
      '/api/scenario-documents/save',
      parseScenarioDocumentSaveResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({ ...fence, ...input }),
      },
    ),
    load: (
      fence: ScenarioDocumentFenceRequest,
      path: string,
      confirmDiscard: boolean,
      signal?: AbortSignal,
    ): Promise<ScenarioDocumentLoadResponse> => requestJson(
      '/api/scenario-documents/load',
      parseScenarioDocumentLoadResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({
          ...fence,
          path,
          confirmDiscard,
        }),
      },
    ),
    export: (
      fence: ScenarioDocumentFenceRequest,
      destinationPath: string,
      signal?: AbortSignal,
    ): Promise<ScenarioDocumentExportResponse> => requestJson(
      '/api/scenario-documents/export',
      parseScenarioDocumentExportResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({ ...fence, destinationPath }),
      },
    ),
  },
  deploymentWorkspace: {
    get: (signal?: AbortSignal): Promise<DeploymentWorkspaceResponse> =>
      requestJson(
        '/api/deployment/workspace',
        parseDeploymentWorkspaceResponse,
        signal,
      ),
    openDraft: (
      fence: LocalWorkspaceFenceRequest,
      path: string,
      confirmDiscard: boolean,
      signal?: AbortSignal,
    ): Promise<DeploymentWorkspaceResponse> => requestJson(
      '/api/deployment/open-draft',
      parseDeploymentWorkspaceResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({ ...fence, path, confirmDiscard }),
      },
    ),
    mutate: (
      fence: LocalWorkspaceFenceRequest,
      path: Array<string | number>,
      operation: 'set' | 'append' | 'remove',
      value: JsonValue | undefined,
      signal?: AbortSignal,
    ): Promise<DeploymentWorkspaceResponse> => requestJson(
      '/api/deployment/draft/mutations',
      parseDeploymentWorkspaceResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({ ...fence, path, operation, value }),
      },
    ),
    updateControlledStop: (
      fence: LocalWorkspaceFenceRequest,
      update: DeploymentControlledStopUpdate,
      signal?: AbortSignal,
    ): Promise<DeploymentWorkspaceResponse> => requestJson(
      '/api/deployment/draft/controlled-stop',
      parseDeploymentWorkspaceResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({ ...fence, ...update }),
      },
    ),
    updateMotionTargetLimit: (
      fence: LocalWorkspaceFenceRequest,
      controllableComponentId: string,
      commandTargetId: string,
      maximumTcpLinearSpeedMps: number,
      signal?: AbortSignal,
    ): Promise<DeploymentWorkspaceResponse> => requestJson(
      '/api/deployment/draft/motion-target-limit',
      parseDeploymentWorkspaceResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({
          ...fence,
          controllable_component_id: controllableComponentId,
          command_target_id: commandTargetId,
          maximum_tcp_linear_speed_mps: maximumTcpLinearSpeedMps,
        }),
      },
    ),
    save: (
      fence: LocalWorkspaceFenceRequest,
      signal?: AbortSignal,
    ): Promise<DeploymentWorkspaceResponse> => requestJson(
      '/api/deployment/save',
      parseDeploymentWorkspaceResponse,
      signal,
      { method: 'POST', body: JSON.stringify(fence) },
    ),
    saveAs: (
      fence: LocalWorkspaceFenceRequest,
      path: string,
      confirmOverwrite: boolean,
      signal?: AbortSignal,
    ): Promise<DeploymentWorkspaceResponse> => requestJson(
      '/api/deployment/save-as',
      parseDeploymentWorkspaceResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({ ...fence, path, confirmOverwrite }),
      },
    ),
    discard: (
      fence: LocalWorkspaceFenceRequest,
      signal?: AbortSignal,
    ): Promise<DeploymentWorkspaceResponse> => requestJson(
      '/api/deployment/discard',
      parseDeploymentWorkspaceResponse,
      signal,
      { method: 'POST', body: JSON.stringify(fence) },
    ),
    revert: (
      fence: DraftFenceRequest,
      signal?: AbortSignal,
    ): Promise<DeploymentCandidateOperationResponse> => requestJson(
      '/api/deployment/revert',
      parseDeploymentCandidateOperationResponse,
      signal,
      { method: 'POST', body: JSON.stringify(fence) },
    ),
    applyDraft: (
      fence: DraftFenceRequest,
      confirmation: {
        token: string;
        preparation: DeploymentCandidatePreparation;
      } | null = null,
      signal?: AbortSignal,
    ): Promise<DeploymentCandidateOperationResponse> => requestJson(
      '/api/deployment/draft/apply',
      parseDeploymentCandidateOperationResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({
          ...fence,
          confirmationToken: confirmation?.token ?? null,
          preparationRequestOwnerEpoch:
            confirmation?.preparation.requestOwnerEpoch ?? null,
          preparationRequestId:
            confirmation?.preparation.requestId ?? null,
        }),
      },
    ),
  },
  programs: {
    currentScenario: (signal?: AbortSignal): Promise<CurrentScenarioResponse> =>
      requestJson('/api/scenarios/current', parseCurrentScenarioResponse, signal),
    applied: (signal?: AbortSignal): Promise<AppliedProgramSelection> =>
      requestJson('/api/program/applied', parseAppliedProgramSelection, signal),
    appliedStatus: (signal?: AbortSignal): Promise<AppliedProgramStatus> =>
      requestJson('/api/program/applied/status', parseAppliedProgramStatus, signal),
    persistentValues: (
      scenarioId: string,
      signal?: AbortSignal,
    ): Promise<PersistentKeyValues> => requestJson(
      persistentValuesPath(scenarioId),
      parsePersistentKeyValues,
      signal,
    ),
    /** Set one stored value, or Reset it to `initial:` (value omitted). */
    editPersistentValue: (
      scenarioId: string,
      body: PersistentKeyValueEditBody,
    ): Promise<PersistentKeyValues> => requestJson(
      persistentValuesPath(scenarioId, 'value' in body ? '' : '/reset'),
      parsePersistentKeyEdit,
      undefined,
      { method: 'POST', body: JSON.stringify(body) },
    ),
    clearPersistentValues: (
      scenarioId: string,
      body: PersistentKeyRequestIdentity & { scenario_id: string },
    ): Promise<PersistentKeyValuesCleared> => requestJson(
      persistentValuesPath(scenarioId, '/clear'),
      parsePersistentKeyValuesCleared,
      undefined,
      { method: 'POST', body: JSON.stringify(body) },
    ),
  },
  manager: {
    status: (signal?: AbortSignal): Promise<ManagerStatusProjection> => requestJson(
      '/api/manager/status',
      parseManagerStatusProjection,
      signal,
    ),
    graph: (signal?: AbortSignal): Promise<ManagerGraphProjection> => requestJson(
      '/api/manager/graph',
      parseManagerGraphProjection,
      signal,
    ),
    diff: (signal?: AbortSignal): Promise<ManagerDiffProjection> => requestJson(
      '/api/manager/diff',
      parseManagerDiffProjection,
      signal,
    ),
    applyStatus: (
      applyId: string,
      applyGeneration: number,
      signal?: AbortSignal,
    ): Promise<ManagerApplyStatus> => {
      const params = new URLSearchParams({
        apply_id: applyId,
        apply_generation: String(applyGeneration),
      });
      return requestJson(
        `/api/manager/apply/status?${params.toString()}`,
        (value) => {
          const status = parseManagerApplyStatus(value);
          if (
            status.applyId !== applyId
            || status.applyGeneration !== applyGeneration
          ) {
            throw new Error(
              'Manager Apply status identity does not match the request',
            );
          }
          return status;
        },
        signal,
      );
    },
  },
  files: {
    browse: (
      kind: ArtifactKind,
      path?: string,
      signal?: AbortSignal,
    ): Promise<FileBrowserResponse> => {
      const params = new URLSearchParams({ kind });
      if (path) {
        params.set('path', path);
      }
      return requestJson(
        `/api/files/browse?${params.toString()}`,
        parseFileBrowserResponse,
        signal,
      );
    },
  },
  urdf: {
    manifest: (signal?: AbortSignal): Promise<SceneUrdfManifest> =>
      requestJson(
        '/api/scene/urdf/manifest',
        parseSceneUrdfManifest,
        signal,
      ),
  },
  assistant: {
    modes: (signal?: AbortSignal): Promise<AssistantModeIndex> =>
      requestJson('/api/assistant/modes', parseAssistantModeIndex, signal),
    submit: (
      body: AssistantRequestInput,
      signal?: AbortSignal,
    ): Promise<AssistantProviderStatus> => requestJson(
      '/api/assistant/requests',
      parseAssistantProviderStatus,
      signal,
      { method: 'POST', body: JSON.stringify(body) },
    ),
    status: (
      requestId: string,
      signal?: AbortSignal,
    ): Promise<AssistantProviderStatus> => requestJson(
      `/api/assistant/requests/${encodeURIComponent(requestId)}`,
      parseAssistantProviderStatus,
      signal,
    ),
    cancel: (
      requestId: string,
      signal?: AbortSignal,
    ): Promise<AssistantProviderStatus> => requestJson(
      `/api/assistant/requests/${encodeURIComponent(requestId)}/cancel`,
      parseAssistantProviderStatus,
      signal,
      { method: 'POST' },
    ),
  },
  scene: {
    resolveMesh: (
      resourceUri: string,
      scale: { x: number; y: number; z: number },
      signal?: AbortSignal,
    ): Promise<SceneMeshAssetMetadata> => {
      const params = new URLSearchParams({
        resource_uri: resourceUri,
        scale_x: String(scale.x),
        scale_y: String(scale.y),
        scale_z: String(scale.z),
      });
      return requestJson(
        `/api/scene/object-meshes/resolve?${params.toString()}`,
        parseSceneMeshAssetMetadata,
        signal,
      );
    },
    resolveProgramDraftMesh: (
      resourceUri: string,
      scale: { x: number; y: number; z: number },
      draftId: string,
      draftRevision: number,
      signal?: AbortSignal,
    ): Promise<ProgramDraftMeshAssetMetadata> => {
      const params = new URLSearchParams({
        resource_uri: resourceUri,
        scale_x: String(scale.x),
        scale_y: String(scale.y),
        scale_z: String(scale.z),
        draft_id: draftId,
        draft_revision: String(draftRevision),
      });
      return requestJson(
        `/api/program/draft/scene-meshes/resolve?${params.toString()}`,
        parseProgramDraftMeshAssetMetadata,
        signal,
      );
    },
    commit: (
      transaction: SceneTransactionRequest,
      signal?: AbortSignal,
    ): Promise<SceneTransactionResponse> => requestJson(
      '/api/scene/transactions',
      parseSceneTransactionResponse,
      signal,
      { method: 'POST', body: JSON.stringify(transaction) },
    ),
  },
  execution: {
    identity: (signal?: AbortSignal): Promise<ExecutionIdentity> =>
      requestJson(
        '/api/execution/identity',
        parseExecutionIdentity,
        signal,
      ),
    stopFault: (signal?: AbortSignal): Promise<StopFaultRecoveryOffer | null> =>
      requestJson(
        '/api/execution/stop-fault',
        parseStopFaultRecoveryOffer,
        signal,
      ),
    recoverStopFault: async (
      ownerEpoch: string,
      requestId: string,
      signal?: AbortSignal,
    ): Promise<void> => {
      // Read the current offer immediately before composing the command. The
      // backend fences every field, so a stale click is rejected safely.
      const offer = await requestJson(
        '/api/execution/stop-fault',
        parseStopFaultRecoveryOffer,
        signal,
      );
      if (offer === null) throw new Error('No cell stop-fault recovery is active.');
      if (offer.stopCertificate === null) {
        throw new Error('Cell recovery is missing its stop certificate.');
      }
      await requestJson(
        '/api/execution/stop-fault/recover',
        (value) => parseStopFaultRecoveryResult(value, requestId),
        signal,
        {
          method: 'POST',
          body: JSON.stringify({
            request_context: {
              request_id: requestId,
              request_owner_epoch: ownerEpoch,
              source_label: 'composer_ui',
              correlation_id: null,
            },
            expected_latch_id: offer.latchId,
            expected_latch_generation: offer.latchGeneration,
            expected_recovery_assessment_revision: offer.recoveryAssessmentRevision,
            expected_stop_certificate_id: offer.stopCertificate.stopCertificateId,
            expected_stop_certificate_revision: offer.stopCertificate.stopCertificateRevision,
            expected_cell_id: offer.cellId,
            expected_retained_bundle_id: offer.retainedBundleId,
            expected_graph_revision: offer.graphRevision,
            configuration_expectations: offer.configurationExpectations.map((row) => ({
              control_domain_id: row.controlDomainId,
              active_control_configuration_id: row.activeControlConfigurationId,
              active_control_configuration_generation:
                row.activeControlConfigurationGeneration,
            })),
            reason: 'operator requested cell recovery',
          }),
        },
      );
    },
    runtimeScaleControl: (
      selector: RuntimeScaleControlSelector,
      signal?: AbortSignal,
    ): Promise<RuntimeScaleControlStatus> => {
      // Selectors are caller-owned objects. Snapshot the complete identity
      // before constructing the URL or awaiting the response so a caller
      // mutation cannot cross the request and its reply validation.
      const requestedSelector: RuntimeScaleControlSelector = selector.kind === 'singular'
        ? { ...selector }
        : {
            ...selector,
            groupTargets: selector.groupTargets.map((target) => ({ ...target })),
          };
      return requestJson(
        `/api/execution/runtime-scale-control?${runtimeScaleSelectorQuery(requestedSelector)}`,
        (value) => {
          const status = parseRuntimeScaleControlStatus(value);
          if (!sameRuntimeScaleSelector(requestedSelector, status.selector)) {
            throw new Error('runtime-scale control status does not match its requested selector');
          }
          return status;
        },
        signal,
      );
    },
    manualAvailability: (
      intent: DirectControlOperationIntent,
      signal?: AbortSignal,
    ): Promise<ManualControlAvailability> => requestJson(
      '/api/execution/manual-handover',
      (value) => {
        const result = parseManualControlAvailability(value);
        if (result.requestId !== intent.request_context.request_id
          || result.requestOwnerEpoch !== intent.request_context.request_owner_epoch) {
          throw new Error('Manual availability does not match the requested owner and target query.');
        }
        return result;
      },
      signal,
      { method: 'POST', body: JSON.stringify(intent) },
    ),
    previewMotion: (
      intent: DirectControlOperationIntent,
      signal?: AbortSignal,
    ): Promise<MotionPreviewReceipt> => requestJson(
      '/api/execution/motion-previews',
      (value) => {
        const receipt = parseMotionPreviewResponse(value);
        const participant = intent.participants[0];
        if (participant.payload.kind !== 'motion') {
          throw new Error('motion preview requires a motion intent');
        }
        const authoredJointNames = participant.payload.motion.kind === 'joint_goal'
          ? participant.payload.motion.joint_names
          : null;
        if (
          receipt.requestId !== intent.request_context.request_id
          || receipt.requestOwnerEpoch
            !== intent.request_context.request_owner_epoch
          || receipt.programId !== intent.program_id
          || receipt.programRevision !== intent.program_revision
          || receipt.programBindingId !== intent.program_binding_id
          || receipt.programRunId !== intent.program_run_id
          || receipt.graphRevision !== intent.expected_graph_revision
          || receipt.retainedBundleId !== intent.expected_retained_bundle_id
          || receipt.sceneRevision.scene_id
            !== intent.expected_scene_revision.scene_id
          || receipt.sceneRevision.revision_id
            !== intent.expected_scene_revision.revision_id
          || receipt.sceneRevision.content_hash
            !== intent.expected_scene_revision.content_hash
          || receipt.componentRoleId !== participant.component_role_id
          || receipt.targetRoleId !== participant.target_role_id
          || receipt.requiredCapabilityId
            !== participant.required_capability_id
          || (authoredJointNames !== null && (
            receipt.sourceJointNames.length !== authoredJointNames.length
            || receipt.sourceJointNames.some(
              (name, index) => name !== authoredJointNames[index],
            )
          ))
        ) {
          throw new Error('preview receipt does not match the authored intent');
        }
        return receipt;
      },
      signal,
      { method: 'POST', body: JSON.stringify(intent) },
    ),
    previewCapabilityCommand: (
      intent: DirectControlOperationIntent,
      signal?: AbortSignal,
    ): Promise<CapabilityCommandPreviewReceipt> => requestJson(
      '/api/execution/capability-command-previews',
      (value) => {
        const receipt = parseCapabilityCommandPreviewResponse(value);
        const participant = intent.participants[0];
        if (participant.payload.kind !== 'capability_command') {
          throw new Error(
            'capability-command preview requires a command intent',
          );
        }
        if (
          receipt.requestId !== intent.request_context.request_id
          || receipt.requestOwnerEpoch
            !== intent.request_context.request_owner_epoch
          || receipt.programId !== intent.program_id
          || receipt.programRevision !== intent.program_revision
          || receipt.programBindingId !== intent.program_binding_id
          || receipt.programRunId !== intent.program_run_id
          || receipt.graphRevision !== intent.expected_graph_revision
          || receipt.retainedBundleId !== intent.expected_retained_bundle_id
          || receipt.sceneRevision.scene_id
            !== intent.expected_scene_revision.scene_id
          || receipt.sceneRevision.revision_id
            !== intent.expected_scene_revision.revision_id
          || receipt.sceneRevision.content_hash
            !== intent.expected_scene_revision.content_hash
          || receipt.componentRoleId !== participant.component_role_id
          || receipt.targetRoleId !== participant.target_role_id
          || receipt.requiredCapabilityId
            !== participant.required_capability_id
          || receipt.commandSchemaId
            !== participant.payload.command_schema_id
          || receipt.commandSchemaRevision
            !== participant.payload.command_schema_revision
          || receipt.coordinateKind
            !== participant.payload.command_value.position.kind
          || receipt.targetPosition !== (
            participant.payload.command_value.position.kind === 'angular_rad'
              ? participant.payload.command_value.position.value_rad
              : participant.payload.command_value.position.value_m
          )
        ) {
          throw new Error(
            'command preview receipt does not match the authored intent',
          );
        }
        return receipt;
      },
      signal,
      { method: 'POST', body: JSON.stringify(intent) },
    ),
  },
  alarms: {
    acknowledge: (
      alarm: OperatorAlarmRecord,
      ownerEpoch: string,
      requestId: string,
      signal?: AbortSignal,
    ): Promise<Record<string, unknown>> => requestJson(
      `/api/execution/alarms/${encodeURIComponent(alarm.conditionId)}/acknowledge`,
      parseObjectResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({
          request_id: requestId,
          request_owner_epoch: ownerEpoch,
          source_label: 'composer_ui',
          correlation_id: null,
          activation_id: alarm.activationId,
          expected_record_revision: alarm.recordRevision,
          reason: null,
          confirmed: true,
        }),
      },
    ),
    acknowledgeAll: (
      ownerEpoch: string,
      generation: number,
      members: AlarmMembership[],
      requestId: string,
      signal?: AbortSignal,
    ): Promise<Record<string, unknown>> => requestJson(
      '/api/execution/alarms/acknowledge-all',
      parseObjectResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({
          request_id: requestId,
          request_owner_epoch: ownerEpoch,
          source_label: 'composer_ui',
          correlation_id: null,
          members: members.map((member) => ({
            condition_id: member.conditionId,
            activation_id: member.activationId,
            record_revision: member.recordRevision,
          })),
          expected_generation: generation,
          reason: null,
          confirmed: true,
        }),
      },
    ),
  },
  history: {
    discovery: (kind: HistorianDiscovery['kind'], after = '', signal?: AbortSignal): Promise<HistorianDiscovery> =>
      requestJson(`/api/execution/history/discovery?${new URLSearchParams({ kind, after, limit: '32' })}`, (value) => {
        const page = parseHistorianDiscovery(value);
        if (page.kind !== kind) throw new Error('History discovery response has a different kind');
        return page;
      }, signal),
    // Browser downloads the bounded bundle directly: large evidence is never
    // parsed, copied into React state, or mixed with control publications.
    exportUrl: (query: HistorianListQuery = {}, normalizedTrace = false): string => resolveStructuralAssetUrl(
      historianListPath(query).replace('/api/execution/history?', `/api/debug/export${normalizedTrace ? '/ros2-medkit' : ''}?`),
    ),
    // One failure capture's retained records and evidence statement, as a
    // direct bounded download; never parsed into React state.
    captureExportUrl: (captureId: number): string => resolveStructuralAssetUrl(
      historianListPath({ captureId }).replace('/api/execution/history?', '/api/execution/history/export?'),
    ),
    captures: (
      beforeCaptureId: string | null = null,
      signal?: AbortSignal,
    ): Promise<HistorianCapturePage> => {
      const parameters = new URLSearchParams({ limit: '32' });
      if (beforeCaptureId !== null) parameters.set('before_capture_id', beforeCaptureId);
      return requestJson(
        `/api/execution/history/captures?${parameters.toString()}`,
        parseHistorianCapturePage,
        signal,
      );
    },
    list: (
      query: HistorianListQuery = {},
      signal?: AbortSignal,
    ): Promise<HistorianPage> =>
      requestJson(
        historianListPath(query),
        parseHistorianPage,
        signal,
      ),
    detail: (sequence: number, signal?: AbortSignal): Promise<HistorianDetail> => {
      if (!Number.isSafeInteger(sequence) || sequence < 1) {
        return Promise.reject(new Error('History sequence must be a positive safe integer'));
      }
      return requestJson(
        `/api/execution/history/${encodeURIComponent(String(sequence))}`,
        parseHistorianDetail,
        signal,
      );
    },
  },
};
