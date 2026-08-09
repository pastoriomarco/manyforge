import {
  parseCatalogResponse,
  parseFileBrowserResponse,
  parseHealthResponse,
  parseHeadlessRuntimeSnapshot,
  parseHistorianPage,
  parseExecutionOwnerStatus,
  parseMotionPreviewResponse,
  parseObjectResponse,
  parseOperationAcceptance,
  parseOperationObservation,
  parseOperatorAlarmSnapshot,
  parseSceneUrdfManifest,
} from './parsers';
import type {
  ArtifactKind,
  CatalogResponse,
  FileBrowserResponse,
  HealthResponse,
  SceneUrdfManifest,
} from '../types/api';
import type {
  AlarmMembership,
  HeadlessRuntimeSnapshot,
  HistorianPage,
  OperatorAlarmRecord,
  OperatorAlarmSnapshot,
} from '../types/headless';
import type {
  ExecutionOwnerStatus,
  JointMotionOperationIntent,
  MotionPreviewReceipt,
  OperationAcceptance,
  OperationObservation,
} from '../types/robotControl';

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
  execution: {
    status: (signal?: AbortSignal): Promise<ExecutionOwnerStatus> =>
      requestJson(
        '/api/execution/status',
        parseExecutionOwnerStatus,
        signal,
      ),
    previewMotion: (
      intent: JointMotionOperationIntent,
      signal?: AbortSignal,
    ): Promise<MotionPreviewReceipt> => requestJson(
      '/api/execution/motion-previews',
      (value) => {
        const receipt = parseMotionPreviewResponse(value);
        const participant = intent.participants[0];
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
          || receipt.sourceJointNames.length
            !== participant.payload.motion.joint_names.length
          || receipt.sourceJointNames.some(
            (name, index) => name
              !== participant.payload.motion.joint_names[index],
          )
        ) {
          throw new Error('preview receipt does not match the authored intent');
        }
        return receipt;
      },
      signal,
      { method: 'POST', body: JSON.stringify(intent) },
    ),
    submit: (
      intent: JointMotionOperationIntent,
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
