import { errorCatalogRow } from '../state/commandRefusal';
import { errorMessage } from '../state/structuralResource';
import {
  parseApplyPreview,
  parseApplyResult,
  parseCellLatch,
  parseComponentStates,
  parseEffectiveValues,
  parseRecoveryPreflight,
  parseRunReadiness,
  parseSetOutputResult,
  parseSignalSamples,
} from './parsers/components';
import { objectAt, stringAt } from './parsers/shared';
import type {
  ApplyPreview,
  ApplyResult,
  CellLatch,
  ComponentAction,
  ComponentState,
  EffectiveValues,
  RecoveryConfirmation,
  RecoveryPreflight,
  RunReadiness,
  SetOutputResult,
  SignalSample,
} from '../types/components';

function parseAcceptedAction(value: unknown): { action: ComponentAction } {
  return { action: stringAt(objectAt(value, 'accepted').action, 'accepted.action') };
}
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
  parseNodeEnvironmentResponse,
  parseObjectResponse,
  parseProgramWorkspaceResponse,
  parseProgramWorkspaceTransitionResponse,
  parseCurrentScenarioResponse,
  parseRuntimeScaleControlStatus,
  parseScenarioDocumentIndexResponse,
  parseScenarioDocumentExportResponse,
  parseScenarioDocumentSaveResponse,
  parseSceneTransactionResponse,
  parseSceneMeshAssetMetadata,
  parseProgramDraftMeshAssetMetadata,
  parseSceneUrdfManifest,
  parseSourceModelProjection,
  parseProgramNodeCorrectionPreview,
  parseProgramRunAdmittedSpeeds,
} from './parsers';
import {
  ioSignalReadBody,
  mockIoInputBody,
  parseIoSignalResponse,
  parseMockIoInputReceipt,
  type IoSignalReading,
  type MockIoInputReceipt,
  type IoSignalSelector,
} from './parsers/ioSignals';
import {
  parseComposerSettingsSnapshot,
  type ComposerSettings,
  type ComposerSettingsSnapshot,
} from '../state/composerSettings';
import {
  parseAssistantModeDetail,
  parseAssistantModeIndex,
  parseAssistantProviderStatus,
} from './assistantParsers';
import type {
  FileBrowseKind,
  SaveScenarioDocumentInput,
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
  DeploymentWorkspaceResponse,
  FileBrowserResponse,
  HealthResponse,
  JsonValue,
  ManagerApplyStatus,
  ManagerApplyAcknowledgementContext,
  NodeEnvironmentResponse,
  ManagerDiffProjection,
  ManagerGraphProjection,
  ManagerStatusProjection,
  ProgramWorkspaceResponse,
  ProgramWorkspaceTransitionResponse,
  CurrentScenarioResponse,
  ScenarioDocumentFenceRequest,
  ScenarioDocumentExportResponse,
  ScenarioDocumentIndexResponse,
  ScenarioDocumentSaveResponse,
  SceneTransactionRequest,
  SceneTransactionResponse,
  SceneMeshAssetMetadata,
  ProgramDraftMeshAssetMetadata,
  SceneUrdfManifest,
  SourceModelProjection,
  StructuredError,
  ResetApplyContext,
  ProgramNodeCorrectionPreview,
  ProgramNodeCorrectionPreviewRequest,
} from '../types/api';
import type {
  AlarmMembership,
  HistorianPage,
  HistorianDetail,
  HistorianCapturePage,
  HistorianDiscovery,
} from '../types/headless';
import type {
  CapabilityCommandPreviewReceipt,
  DirectControlOperationIntent,
  ExecutionIdentity,
  MotionPreviewReceipt,
  ManualControlAvailability,
  RuntimeScaleControlSelector,
  RuntimeScaleControlStatus,
  ProgramRunAdmittedSpeeds,
} from '../types/robotControl';
import type { ComponentCatalog } from '../types/componentCatalog';
import { parseComponentCatalog } from './componentCatalogParser';
import type {
  AssistantModeDetail,
  AssistantModeIndex,
  AssistantProviderStatus,
  AssistantRequestInput,
} from '../types/assistant';

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
    parameters.set('component_id', selector.componentId);
    parameters.set('command_target_id', selector.commandTargetId);
  } else {
    parameters.set('group_targets', JSON.stringify(selector.groupTargets.map((target) => ({
      component_id: target.componentId,
      command_target_id: target.commandTargetId,
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
    return requested.componentId === returned.componentId
      && requested.commandTargetId === returned.commandTargetId;
  }
  if (requested.kind !== 'group' || returned.kind !== 'group') return false;
  const identity = (target: { componentId: string; commandTargetId: string }) => (
    JSON.stringify([target.componentId, target.commandTargetId])
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
 * The reply arrived and states the outcome (e.g. a failed ApplyResult): the
 * request settled, so the browser's submission guard is released.
 */
export class KnownOutcomeError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'KnownOutcomeError';
  }
}

/**
 * Whether the backend refused this request before any of its effects.
 *
 * The backend marks every such refusal in its structured error body
 * (`detail.effect === "none"`): the request left no durable or externally
 * visible effect (no committed Draft/workspace change, no file written or
 * published, no Manager or Kernel submission), so the browser's submission
 * guard is released. A failure after a write names the written path instead.
 * Only that marker proves it, never a code or a bare status: a transport or
 * gateway failure, a malformed body, or a failure after an effect may hide a
 * request that took effect, so it stays unconfirmed.
 */
export function refusedBeforeEffect(error: unknown): boolean {
  if (!(error instanceof ApiError) || error.structuredError === null) return false;
  return (error.payload as { detail: Record<string, unknown> }).detail.effect === 'none';
}

/**
 * Whether the connection dropped before any reply: `fetch` rejects with a
 * TypeError (Firefox: "NetworkError when attempting to fetch resource").
 * The request may still have taken effect, so it is never a refusal.
 */
export function connectionLost(error: unknown): boolean {
  return error instanceof TypeError;
}

/** The current Scenario as one line, read from `/api/scenarios/current`. */
export async function currentScenarioText(): Promise<string> {
  try {
    const current = await api.programs.currentScenario();
    return current.currentScenario === null
      ? `No Scenario is applied (${current.unavailableReasons.join(', ')}).`
      : `Current Scenario: ${current.currentScenario.scenarioId}, applied.`;
  } catch (error) {
    return `The current Scenario could not be read: ${errorMessage(error, 'request failed').replace(/[.\s]*$/, '')}.`;
  }
}

/** Remedies the owner's message does not already name (it lies outside the Draft). */
const OBSERVED_MOVE_REFUSAL_GUIDANCE: Readonly<Record<string, string>> = {
  observed_move_target_unavailable: 'Check that the robot is running in the current configuration.',
  observed_move_unavailable: 'Wait for the robot to report its current state, then try again.',
};

/**
 * The operator text for a refusal the backend raised before any effect, or
 * null for every other failure. Only for authoring requests; never retry an
 * edit after a refused fence.
 */
export function authoringRefusalMessage(error: unknown): string | null {
  if (!refusedBeforeEffect(error)) return null;
  const refusal = error as ApiError;
  const code = apiErrorCode(refusal);
  const message = refusal.message.replace(/[.\s]*$/, '');
  // The observed-pose and observed-object resolvers name what the operator
  // has to correct before a Draft edit can capture the current state.
  if (code !== null && code.startsWith('observed_')) {
    const guidance = OBSERVED_MOVE_REFUSAL_GUIDANCE[code];
    if (!guidance) return `Edit not applied: ${refusal.message}`;
    return `Edit not applied: ${message}. ${guidance}`;
  }
  if (code === 'authoring_not_ready') return 'Edit not applied: authoring is currently unavailable.';
  if (code === 'stale_generation') return 'Edit not applied: backend state changed. Review the current state before trying again.';
  if (code === 'activation_preparation_failed') {
    const details = refusal.structuredError?.details?.preparer_detail;
    const typedCause = preparationCauseCode(refusal);
    const catalogRow = typedCause === null ? null : errorCatalogRow(typedCause);
    if (catalogRow !== null) {
      return 'Not applied: the candidate could not be prepared. '
        + (catalogRow[1] || catalogRow[0]);
    }
    const cause = typeof details === 'string'
      ? details.replace(/^manyforge_prepare_activation:\s*/, '').trim()
      : typedCause ?? '';
    return 'Not applied: the candidate could not be prepared'
      + (cause ? ' (' + cause + ')' : '')
      + '. Correct it and try again.';
  }
  return `Not applied: ${message}. Nothing was changed.`;
}

/**
 * The typed cause the activation preparer named for a refused preparation
 * (`details.cause_code`, a catalog code), or null.
 */
export function preparationCauseCode(error: unknown): string | null {
  if (!(error instanceof ApiError) || apiErrorCode(error) !== 'activation_preparation_failed') return null;
  const cause = error.structuredError?.details?.cause_code;
  return typeof cause === 'string' && cause !== '' ? cause : null;
}

// The document was written to the named path but not read back or adopted as
// Saved (for example a newer workspace revision): a definite outcome, so the
// client shows it, refreshes and adopts the current identity, never retries.
const SAVED_NOT_ADOPTED_CODES = new Set([
  'program_saved_baseline_not_adopted',
  'deployment_saved_baseline_not_adopted',
  'scenario_saved_baseline_not_adopted',
]);

const STALE_WORKSPACE_CODES = new Set([
  // The composite workspace or a Draft identity moved under this request.
  'composer_workspace_stale',
  'composer_draft_id_stale',
  'composer_draft_revision_stale',
  'scenario_workspace_stale',
  // Raised by the Deployment Draft owner before any edit is applied.
  'deployment_draft_stale',
  ...SAVED_NOT_ADOPTED_CODES,
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

/** The backend's text for a definite "written but not adopted as Saved" outcome, or null. */
export function savedNotAdoptedMessage(error: unknown): string | null {
  if (!isStaleWorkspaceError(error)) return null;
  const code = apiErrorCode(error);
  return code !== null && SAVED_NOT_ADOPTED_CODES.has(code) ? (error as ApiError).message : null;
}

export type AuthoringRequestIdentity = {
  requestOwnerEpoch: string;
  requestId: string;
  expectedAdmissionGeneration: number;
  path?: string;
  /** The shared reference an Apply after Reset was sent with. */
  correlationId?: string;
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
  /** The Applied Scenario project folder the operator saw. */
  project_path: string;
  key: string;
  expected_program_revision: string;
  value?: JsonValue;
};

/** The Applied Scenario project's values; an edit names the folder it saw. */
function persistentValuesPath(suffix = ''): string {
  return `/api/program/persistent-values${suffix}`;
}

/** A maintained assistant profile: whether it has a chat mode, and its runtime model aliases. */
export interface AssistantProfile {
  chat: boolean;
  /** The runtime (`query_model`) aliases; the chat alias is bound by the assistant component. */
  modelEndpointAliases: string[];
}

/** Maintained assistant profile id -> its facts. */
export type AssistantProfiles = Record<string, AssistantProfile>;

function parseAssistantProfiles(value: unknown): AssistantProfiles {
  const root = value as { schemaVersion?: unknown; profiles?: unknown } | null;
  if (root?.schemaVersion !== 'manyforge.composer_assistant_profile_aliases.v2'
    || !Array.isArray(root.profiles)) {
    throw new Error('assistantProfiles must be the supported alias listing');
  }
  return Object.fromEntries(root.profiles.map((row: unknown) => {
    const profile = row as { profileId?: unknown; chat?: unknown; modelEndpointAliases?: unknown };
    if (typeof profile.profileId !== 'string' || typeof profile.chat !== 'boolean'
      || !Array.isArray(profile.modelEndpointAliases)
      || profile.modelEndpointAliases.some((alias) => typeof alias !== 'string')) {
      throw new Error('assistantProfiles.profiles must name aliases per profile');
    }
    return [profile.profileId, {
      chat: profile.chat,
      modelEndpointAliases: profile.modelEndpointAliases as string[],
    }];
  }));
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
          ...(typeof body.correlationId === 'string' ? { correlationId: body.correlationId } : {}),
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
    // Night change 10: a body read cut by the caller's abort is that abort,
    // not a malformed reply.
    signal?.throwIfAborted();
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
  /** Component states and actions. */
  components: {
    list: (signal?: AbortSignal): Promise<ComponentState[]> =>
      requestJson('/api/components', parseComponentStates, signal),
    /** 202: accepted; the result arrives in the component's `last_action`. */
    action: (componentId: string, action: ComponentAction): Promise<{ action: ComponentAction }> =>
      requestJson(`/api/components/${encodeURIComponent(componentId)}/actions`, parseAcceptedAction,
        undefined, { method: 'POST', body: JSON.stringify({ action }) }),
    /** The same action on the named components, one at a time in cell order (Prepare all, Enable all). */
    groupAction: (action: ComponentAction, componentIds: readonly string[]): Promise<{ action: ComponentAction }> =>
      requestJson('/api/components/actions', parseAcceptedAction, undefined,
        { method: 'POST', body: JSON.stringify({ action, component_ids: componentIds }) }),
    /** What Enable would have to recover first, for the operator to confirm. */
    recoverEnablePreflight: (componentId: string, signal?: AbortSignal): Promise<RecoveryPreflight> =>
      requestJson(`/api/components/${encodeURIComponent(componentId)}/recover-enable/preflight`,
        parseRecoveryPreflight, signal),
    /** 202: Recover (if confirmed) then Enable; phases arrive in `last_action`. */
    recoverEnable: (componentId: string, confirmation: RecoveryConfirmation): Promise<{ action: ComponentAction }> =>
      requestJson(`/api/components/${encodeURIComponent(componentId)}/recover-enable`, parseAcceptedAction,
        undefined, { method: 'POST', body: JSON.stringify(confirmation) }),
    /** The same, for several components one at a time in cell order; stops at the first change. */
    recoverEnableGroup: (
      targets: readonly { component_id: string; recovery_preflight: RecoveryConfirmation }[],
    ): Promise<{ action: ComponentAction }> =>
      requestJson('/api/components/recover-enable', parseAcceptedAction, undefined,
        { method: 'POST', body: JSON.stringify({ targets }) }),
  },
  cell: {
    latch: (signal?: AbortSignal): Promise<CellLatch> =>
      requestJson('/api/cell/latch', parseCellLatch, signal),
  },
  io: {
    list: (signal?: AbortSignal): Promise<SignalSample[]> =>
      requestJson('/api/io', parseSignalSamples, signal),
    setOutput: (signalId: string, value: boolean | number): Promise<SetOutputResult> =>
      requestJson(`/api/io/${encodeURIComponent(signalId)}/set`, parseSetOutputResult, undefined,
        { method: 'POST', body: JSON.stringify({ value }) }),
  },
  readiness: {
    get: (signal?: AbortSignal): Promise<RunReadiness> =>
      requestJson('/api/program/readiness', parseRunReadiness, signal),
  },
  /** The one server-side Apply path for Apply, Load and New. */
  cellApply: {
    preview: (): Promise<ApplyPreview> =>
      requestJson('/api/deployment/preview', parseApplyPreview, undefined, { method: 'POST', body: '{}' }),
    /** Applies exactly what `scope` previewed; a changed Draft is refused. */
    apply: (scope: ApplyPreview['scope']): Promise<ApplyResult> =>
      requestJson('/api/deployment/apply', parseApplyResult, undefined,
        { method: 'POST', body: JSON.stringify({ scope }) }),
    /** The impact of reverting to the Saved Scenario; effect-free. */
    revertPreview: (): Promise<ApplyPreview> =>
      requestJson('/api/deployment/revert/preview', parseApplyPreview, undefined, { method: 'POST', body: '{}' }),
    /** Apply of the Saved Scenario the Revert preview showed; both Drafts return to Saved. */
    revert: (scope: ApplyPreview['scope']): Promise<ApplyResult> =>
      requestJson('/api/deployment/revert', parseApplyResult, undefined,
        { method: 'POST', body: JSON.stringify({ scope }) }),
    effective: (signal?: AbortSignal): Promise<EffectiveValues> =>
      requestJson('/api/deployment/effective', parseEffectiveValues, signal),
    /** The GUI asks before discarding unsaved Drafts, then confirms here. */
    load: (path: string): Promise<ApplyResult> =>
      requestJson('/api/scenario/load', parseApplyResult, undefined,
        { method: 'POST', body: JSON.stringify({ path, discard_unsaved: true }) }),
    newScenario: (): Promise<ApplyResult> =>
      requestJson('/api/scenario/new', parseApplyResult, undefined,
        { method: 'POST', body: JSON.stringify({ discard_unsaved: true }) }),
  },
  authoringWorkspace: {
    /** Log one confirmed Apply-after-Reset before its Reset; grants no authority. */
    recordResetApplyContext: (
      context: ResetApplyContext,
      signal?: AbortSignal,
    ): Promise<void> => requestJson(
      '/api/authoring/reset-apply-contexts',
      (value) => {
        const record = value as { correlationId?: unknown; recorded?: unknown } | null;
        if (record?.correlationId !== context.correlationId || record.recorded !== true) {
          throw new Error('the Apply context was not recorded for this correlation');
        }
      },
      signal,
      { method: 'POST', body: JSON.stringify(context) },
    ),
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
    validate: (authoring: unknown, signal?: AbortSignal) => requestJson(
      '/api/program/validate', parseObjectResponse, signal,
      { method: 'POST', body: JSON.stringify({ authoring }) },
    ),
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
      /** An Apply after Reset keeps its shared reference in the Manager record. */
      correlationId?: string,
    ): Promise<ProgramWorkspaceTransitionResponse> => requestJson(
      '/api/program/apply',
      parseProgramWorkspaceTransitionResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({
          ...fence,
          ...(correlationId === undefined ? {} : { correlationId }),
        }),
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
  },
  scenarioDocuments: {
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
      input: SaveScenarioDocumentInput,
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
    /** Return every Draft with unapplied changes to Applied (Draft-only). */
    discard: (
      fence: ScenarioDocumentFenceRequest,
      signal?: AbortSignal,
    ): Promise<ProgramWorkspaceResponse> => requestJson(
      '/api/scenario-documents/discard',
      parseProgramWorkspaceResponse,
      signal,
      { method: 'POST', body: JSON.stringify(fence) },
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
    assistantProfiles: (signal?: AbortSignal): Promise<AssistantProfiles> =>
      requestJson('/api/deployment/assistant-profiles', parseAssistantProfiles, signal),
    get: (signal?: AbortSignal): Promise<DeploymentWorkspaceResponse> =>
      requestJson(
        '/api/deployment/workspace',
        parseDeploymentWorkspaceResponse,
        signal,
      ),
    /** Components Add component can compose into the Draft (read-only). */
    componentCatalog: (signal?: AbortSignal): Promise<ComponentCatalog> =>
      requestJson('/api/deployment/component-catalog', parseComponentCatalog, signal),
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
    /** Append component rows and their attachment rows in one Draft edit (Add component). */
    addComponents: (
      fence: LocalWorkspaceFenceRequest,
      components: Array<Record<string, JsonValue>>,
      attachments: Array<Record<string, JsonValue>>,
      signal?: AbortSignal,
    ): Promise<DeploymentWorkspaceResponse> => requestJson(
      '/api/deployment/draft/components',
      parseDeploymentWorkspaceResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({ ...fence, components, attachments }),
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
  },
  programs: {
    currentScenario: (signal?: AbortSignal): Promise<CurrentScenarioResponse> =>
      requestJson('/api/scenarios/current', parseCurrentScenarioResponse, signal),
    applied: (signal?: AbortSignal): Promise<AppliedProgramSelection> =>
      requestJson('/api/program/applied', parseAppliedProgramSelection, signal),
    appliedStatus: (signal?: AbortSignal): Promise<AppliedProgramStatus> =>
      requestJson('/api/program/applied/status', parseAppliedProgramStatus, signal),
    /** Advisory, effect-free preview of one held motion node (D4). */
    nodeCorrectionPreview: (
      request: ProgramNodeCorrectionPreviewRequest,
      signal?: AbortSignal,
    ): Promise<ProgramNodeCorrectionPreview> => requestJson(
      `/api/program/runs/${encodeURIComponent(request.program_run_id)}/node-correction-previews`,
      (value) => parseProgramNodeCorrectionPreview(value, request),
      signal,
      { method: 'POST', body: JSON.stringify(request) },
    ),
    persistentValues: (
      signal?: AbortSignal,
    ): Promise<PersistentKeyValues> => requestJson(
      persistentValuesPath(),
      parsePersistentKeyValues,
      signal,
    ),
    /** Set one stored value, or Reset it to `initial:` (value omitted). */
    editPersistentValue: (
      body: PersistentKeyValueEditBody,
    ): Promise<PersistentKeyValues> => requestJson(
      persistentValuesPath('value' in body ? '' : '/reset'),
      parsePersistentKeyEdit,
      undefined,
      { method: 'POST', body: JSON.stringify(body) },
    ),
    clearPersistentValues: (
      body: PersistentKeyRequestIdentity & { project_path: string },
    ): Promise<PersistentKeyValuesCleared> => requestJson(
      persistentValuesPath('/clear'),
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
    acknowledgeApply: (
      applyId: string,
      applyGeneration: number,
      requestContext: ManagerApplyAcknowledgementContext,
      signal?: AbortSignal,
    ): Promise<ManagerApplyStatus> => {
      const params = new URLSearchParams({
        apply_id: applyId,
        apply_generation: String(applyGeneration),
      });
      return requestJson(
        `/api/manager/apply/acknowledge?${params.toString()}`,
        (value) => {
          const status = parseManagerApplyStatus(value);
          if (
            status.applyId !== applyId
            || status.applyGeneration !== applyGeneration
          ) {
            throw new Error(
              'Manager Apply acknowledgement identity does not match the request',
            );
          }
          return status;
        },
        signal,
        {
          method: 'POST',
          body: JSON.stringify({ request_context: requestContext }),
        },
      );
    },
  },
  files: {
    browse: (
      kind: FileBrowseKind,
      path?: string,
      signal?: AbortSignal,
      rootsOnly = false,
    ): Promise<FileBrowserResponse> => {
      const params = new URLSearchParams({ kind });
      if (path) {
        params.set('path', path);
      }
      if (rootsOnly) params.set('roots_only', 'true');
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
    mode: (mode: string, signal?: AbortSignal): Promise<AssistantModeDetail> =>
      requestJson(`/api/assistant/modes/${encodeURIComponent(mode)}`, parseAssistantModeDetail, signal),
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
    /** Read one bound boolean signal (`query_target` kind `io_signal`); no effect. */
    readIoSignal: (selector: IoSignalSelector, signal?: AbortSignal): Promise<IoSignalReading> =>
      requestJson(
        '/api/execution/io-signals/read',
        (value) => parseIoSignalResponse(value, 'query_target', selector),
        signal,
        { method: 'POST', body: JSON.stringify(ioSignalReadBody(selector)) },
      ),
    /** Set one installed Mock input to an explicit value (simulation only). */
    setMockIoInput: (
      selector: IoSignalSelector,
      expectedSourceEpoch: string,
      value: boolean,
      signal?: AbortSignal,
    ): Promise<MockIoInputReceipt> => requestJson(
      '/api/execution/io-signals/mock-input',
      (response) => parseMockIoInputReceipt(response, selector, value),
      signal,
      { method: 'POST', body: JSON.stringify(mockIoInputBody(selector, expectedSourceEpoch, value)) },
    ),
    /** Each robot's latest owner-admitted TCP speed in one Program run (history evidence). */
    admittedSpeeds: (programRunId: string, signal?: AbortSignal): Promise<ProgramRunAdmittedSpeeds> =>
      requestJson(
        `/api/execution/program-runs/${encodeURIComponent(programRunId)}/admitted-speeds`,
        parseProgramRunAdmittedSpeeds,
        signal,
      ),
    identity: (signal?: AbortSignal): Promise<ExecutionIdentity> =>
      requestJson(
        '/api/execution/identity',
        parseExecutionIdentity,
        signal,
      ),
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
      intent: DirectControlOperationIntent | import('../types/robotControl').DirectControlSetTcpIntent,
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
        if (receipt.resolvedTcp.source === 'joint_flange_fallback'
          && authoredJointNames === null) {
          throw new Error('joint flange fallback is only valid for a joint-goal preview');
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
          || receipt.target.componentId !== participant.target.component_id
          || receipt.target.commandTargetId !== participant.target.command_target_id
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
          || receipt.target.componentId !== participant.target.component_id
          || receipt.target.commandTargetId !== participant.target.command_target_id
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
      alarm: AlarmMembership,
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
