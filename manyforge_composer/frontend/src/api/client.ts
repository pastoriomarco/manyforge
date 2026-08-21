import {
  parseCatalogResponse,
  parseAppliedProgramSelection,
  parseCapabilityCommandPreviewResponse,
  parseDraftMutationResponse,
  parseFileBrowserResponse,
  parseHealthResponse,
  parseHeadlessRuntimeSnapshot,
  parseHistorianPage,
  parseExecutionOwnerStatus,
  parseManagerApplyResponse,
  parseManagerDiffProjection,
  parseManagerGraphProjection,
  parseManagerStatusProjection,
  parseMotionPreviewResponse,
  parseNodeEnvironmentResponse,
  parseObjectResponse,
  parseOperationAcceptance,
  parseOperationObservation,
  parseOperatorAlarmSnapshot,
  parseProgramWorkspaceResponse,
  parseProgramWorkspaceTransitionResponse,
  parseProgramRunIndex,
  parseProgramRunStatus,
  parseScenarioCatalogResponse,
  parseSceneTransactionResponse,
  parseSceneMeshAssetMetadata,
  parseSceneUrdfManifest,
  parseSourceModelProjection,
} from './parsers';
import {
  parseAssistantModeIndex,
  parseAssistantProviderStatus,
} from './assistantParsers';
import {
  parseCCLCAdmission,
  parseCCLCComponentSnapshot,
  parseCCLCRouteTemplateSet,
  parseCCLCStatusEnvelope,
} from './cclcParsers';
import type {
  ArtifactKind,
  AppliedProgramSelection,
  CatalogResponse,
  ManagerApplyRequest,
  DraftFenceRequest,
  DraftMutationResponse,
  FileBrowserResponse,
  HealthResponse,
  JsonValue,
  ManagerApplyResponse,
  NodeEnvironmentResponse,
  ManagerDiffProjection,
  ManagerGraphProjection,
  ManagerStatusProjection,
  ProgramWorkspaceResponse,
  ProgramWorkspaceTransitionResponse,
  ProgramRunIndex,
  ProgramRunStatus,
  ScenarioCatalogResponse,
  SceneTransactionRequest,
  SceneTransactionResponse,
  SceneMeshAssetMetadata,
  SceneUrdfManifest,
  SourceModelProjection,
} from '../types/api';
import type {
  AlarmMembership,
  HeadlessRuntimeSnapshot,
  HistorianPage,
  OperatorAlarmRecord,
  OperatorAlarmSnapshot,
} from '../types/headless';
import type {
  CapabilityCommandPreviewReceipt,
  DirectControlOperationIntent,
  ExecutionOwnerStatus,
  MotionPreviewReceipt,
  OperationAcceptance,
  OperationObservation,
} from '../types/robotControl';
import type {
  AssistantModeIndex,
  AssistantProviderStatus,
  AssistantRequestInput,
} from '../types/assistant';
import type {
  CCLCAdmission,
  CCLCComponentSnapshot,
  CCLCLifecycleRequestBody,
  CCLCRouteRequestBody,
  CCLCRouteTemplateSet,
  CCLCTransactionStatus,
} from '../types/cclc';

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

export class ApiError extends Error {
  readonly status: number;
  readonly payload: unknown;

  constructor(message: string, status: number, payload: unknown) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.payload = payload;
  }
}

async function requestJson<T>(
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

export const api = {
  health: {
    get: (signal?: AbortSignal): Promise<HealthResponse> =>
      requestJson('/api/health', parseHealthResponse, signal),
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
  programWorkspace: {
    get: (signal?: AbortSignal): Promise<ProgramWorkspaceResponse> =>
      requestJson(
        '/api/program/workspace',
        parseProgramWorkspaceResponse,
        signal,
      ),
    mutate: (
      fence: DraftFenceRequest,
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
    openDraft: (
      fence: DraftFenceRequest,
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
      fence: DraftFenceRequest,
      signal?: AbortSignal,
    ): Promise<ProgramWorkspaceResponse> => requestJson(
      '/api/program/save',
      parseProgramWorkspaceResponse,
      signal,
      { method: 'POST', body: JSON.stringify(fence) },
    ),
    saveAs: (
      fence: DraftFenceRequest,
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
      scenarioId: string,
      scenarioCatalogRevision: string,
      signal?: AbortSignal,
    ): Promise<ProgramWorkspaceTransitionResponse> => requestJson(
      '/api/program/apply',
      parseProgramWorkspaceTransitionResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({
          ...fence,
          scenarioId,
          scenarioCatalogRevision,
        }),
      },
    ),
    discard: (
      fence: DraftFenceRequest,
      signal?: AbortSignal,
    ): Promise<ProgramWorkspaceResponse> => requestJson(
      '/api/program/discard',
      parseProgramWorkspaceResponse,
      signal,
      { method: 'POST', body: JSON.stringify(fence) },
    ),
    newDraft: (
      fence: DraftFenceRequest,
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
    load: (
      fence: DraftFenceRequest,
      path: string,
      scenarioId: string,
      scenarioCatalogRevision: string,
      confirmDiscard: boolean,
      signal?: AbortSignal,
    ): Promise<ProgramWorkspaceTransitionResponse> => requestJson(
      '/api/program/load',
      parseProgramWorkspaceTransitionResponse,
      signal,
      {
        method: 'POST',
        body: JSON.stringify({
          ...fence,
          path,
          scenarioId,
          scenarioCatalogRevision,
          confirmDiscard,
        }),
      },
    ),
  },
  programs: {
    scenarios: (signal?: AbortSignal): Promise<ScenarioCatalogResponse> =>
      requestJson('/api/scenarios', parseScenarioCatalogResponse, signal),
    applied: (signal?: AbortSignal): Promise<AppliedProgramSelection> =>
      requestJson('/api/program/applied', parseAppliedProgramSelection, signal),
    runs: (
      activeOnly = false,
      limit = 128,
      signal?: AbortSignal,
    ): Promise<ProgramRunIndex> => {
      const params = new URLSearchParams({
        active_only: activeOnly ? 'true' : 'false',
        limit: String(limit),
      });
      return requestJson(
        `/api/program/runs?${params.toString()}`,
        parseProgramRunIndex,
        signal,
      );
    },
    start: (
      body: Record<string, JsonValue>,
      signal?: AbortSignal,
    ): Promise<ProgramRunStatus> => requestJson(
      '/api/program/runs',
      parseProgramRunStatus,
      signal,
      { method: 'POST', body: JSON.stringify(body) },
    ),
    read: (
      programRunId: string,
      signal?: AbortSignal,
    ): Promise<ProgramRunStatus> => requestJson(
      `/api/program/runs/${encodeURIComponent(programRunId)}`,
      parseProgramRunStatus,
      signal,
    ),
    stop: (
      programRunId: string,
      body: Record<string, JsonValue>,
      signal?: AbortSignal,
    ): Promise<ProgramRunStatus> => requestJson(
      `/api/program/runs/${encodeURIComponent(programRunId)}/stop`,
      parseProgramRunStatus,
      signal,
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
    apply: (
      request: ManagerApplyRequest,
      signal?: AbortSignal,
    ): Promise<ManagerApplyResponse> => requestJson(
      '/api/manager/apply',
      parseManagerApplyResponse,
      signal,
      { method: 'POST', body: JSON.stringify(request) },
    ),
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
  runtime: {
    get: (signal?: AbortSignal): Promise<HeadlessRuntimeSnapshot> =>
      requestJson('/api/runtime', parseHeadlessRuntimeSnapshot, signal),
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
    status: (signal?: AbortSignal): Promise<ExecutionOwnerStatus> =>
      requestJson(
        '/api/execution/status',
        parseExecutionOwnerStatus,
        signal,
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
    submit: (
      intent: DirectControlOperationIntent,
      signal?: AbortSignal,
    ): Promise<OperationAcceptance> => requestJson(
      '/api/execution/operations',
      (value) => {
        const accepted = parseOperationAcceptance(value);
        if (accepted.requestId !== intent.request_context.request_id) {
          throw new Error('operation acceptance does not match its request');
        }
        return accepted;
      },
      signal,
      { method: 'POST', body: JSON.stringify(intent) },
    ),
    read: (
      operationId: string,
      signal?: AbortSignal,
    ): Promise<OperationObservation> => requestJson(
      `/api/execution/operations/${encodeURIComponent(operationId)}`,
      (value) => {
        const observation = parseOperationObservation(value);
        if (observation.operationId !== operationId) {
          throw new Error('operation observation does not match its request');
        }
        return observation;
      },
      signal,
    ),
    stop: (
      ownerEpoch: string,
      operation: OperationObservation,
      requestId: string,
      signal?: AbortSignal,
    ): Promise<Record<string, unknown>> => requestJson(
      '/api/execution/stops',
      parseObjectResponse,
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
          operations: [{
            operation_id: operation.operationId,
            expected_operation_revision: operation.operationRevision,
            expected_stop_generation: operation.activeStopGeneration ?? 0,
          }],
          reason: 'operator Robot Control Stop',
        }),
      },
    ),
    fullStop: (
      ownerEpoch: string,
      expectedStopGeneration: number,
      requestId: string,
      signal?: AbortSignal,
    ): Promise<Record<string, unknown>> => requestJson(
      '/api/execution/full-stop',
      parseObjectResponse,
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
          expected_stop_generation: expectedStopGeneration,
        }),
      },
    ),
    stopFault: (
      signal?: AbortSignal,
    ): Promise<Record<string, unknown>> => requestJson(
      '/api/execution/stop-fault',
      parseObjectResponse,
      signal,
    ),
    recoverStopFault: (
      intent: Record<string, JsonValue>,
      signal?: AbortSignal,
    ): Promise<Record<string, unknown>> => requestJson(
      '/api/execution/stop-fault/recover',
      parseObjectResponse,
      signal,
      { method: 'POST', body: JSON.stringify(intent) },
    ),
  },
  alarms: {
    list: (signal?: AbortSignal): Promise<OperatorAlarmSnapshot> =>
      requestJson(
        '/api/execution/alarms?limit=32&ordering=attention_newest',
        parseOperatorAlarmSnapshot,
        signal,
      ),
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
    list: (limit = 32, signal?: AbortSignal): Promise<HistorianPage> =>
      requestJson(
        `/api/execution/history?limit=${encodeURIComponent(String(limit))}`,
        parseHistorianPage,
        signal,
      ),
  },
};
