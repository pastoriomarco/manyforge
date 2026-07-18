import type {
  CycleStatus,
  CycleCommand,
  CycleRecoveryActionRequest,
  CycleRecoveryActionResult,
  CycleRecoveryOffer,
  VelocityScaleResponse,
  ExecutionChannelsResponse,
  CatalogResponse,
  CatalogEntry,
  ProgramInfo,
  ProgramParameter,
  ProgramBlackboard,
  ProgramCompatibility,
  ProgramTree,
  ProgramValidateResponse,
  TreeNode,
  RuntimeOverrideStatus,
  DeploymentInfo,
  FullDeployment,
  ApplySessionResponse,
  FileBrowserResponse,
  InfraStatus,
  RuntimeStatus,
  ModelEndpointCatalog,
  SceneStateResponse,
  SceneEditRequest,
  SceneEditResponse,
  SceneFramesResponse,
  SceneUrdfManifest,
  SceneFrameGizmoSizeResponse,
  SceneFrameVisibilityResponse,
  SceneObjectFrameVisibilityResponse,
  SceneObjectAxesGizmoSizeResponse,
  SceneGripperCommandResponse,
  PoseSourceApplyResponse,
  PreviewLinkPosesRequest,
  PreviewLinkPosesResponse,
  AssistantChatRequest,
  AssistantChatResponse,
  AssistantProviderCatalog,
  AssistantRequestStatus,
  AssistantCancelResponse,
  CandidateCreateRequest,
  CandidateResponse,
  CandidateExecuteResponse,
  SimulationControlResponse,
  SimulationSetEntityStateRequest,
  SimulationSetStateRequest,
  SimulationStepRequest,
  SimulationResetResponse,
  NodeDiscoveryReport,
  ScenarioCatalogResponse,
} from '../types/api';

const BASE_URL = import.meta.env.VITE_API_URL || '';

/**
 * Phase 2 of issue #1 + #3 (rich error rendering): cycle command routes now
 * return `detail: { message, errorCode, errorDetails }` instead of a single
 * string when a kernel/cycle-manager fault is the cause. This helper keeps
 * back-compat with the legacy string-detail shape used by other routes.
 */
function errorMessageFromPayload(payload: unknown, fallback: string): string {
  if (!payload || typeof payload !== 'object') {
    return fallback;
  }
  const data = payload as Record<string, unknown>;
  const detail = data.detail;
  if (typeof detail === 'string' && detail.trim()) {
    return detail;
  }
  if (detail && typeof detail === 'object') {
    const detailMessage = (detail as Record<string, unknown>).message;
    if (typeof detailMessage === 'string' && detailMessage.trim()) {
      return detailMessage;
    }
  }
  const error = data.error;
  if (error && typeof error === 'object') {
    const errorDetail = (error as Record<string, unknown>).detail;
    if (typeof errorDetail === 'string' && errorDetail.trim()) {
      return errorDetail;
    }
    const errorCode = (error as Record<string, unknown>).code;
    if (typeof errorCode === 'string' && errorCode.trim()) {
      return errorCode;
    }
  }
  const message = data.message;
  if (typeof message === 'string' && message.trim()) {
    return message;
  }
  return fallback;
}

/**
 * Pull the structured (errorCode, errorDetails) tuple out of an HTTPException
 * response body when present. Returns null fields when the payload is the
 * legacy string-detail shape. Used by the viewer to render a kernel-cause
 * panel for cycle command rejections.
 */
export function structuredErrorFromPayload(payload: unknown): {
  errorCode: string | null;
  errorDetails: Record<string, unknown> | null;
  recoveryOffer: CycleRecoveryOffer | null;
} {
  if (!payload || typeof payload !== 'object') {
    return { errorCode: null, errorDetails: null, recoveryOffer: null };
  }
  const data = payload as Record<string, unknown>;
  const detail = data.detail;
  if (!detail || typeof detail !== 'object') {
    return { errorCode: null, errorDetails: null, recoveryOffer: null };
  }
  const detailObj = detail as Record<string, unknown>;
  const code = detailObj.errorCode;
  const details = detailObj.errorDetails;
  const recoveryOffer = detailObj.recoveryOffer;
  return {
    errorCode: typeof code === 'string' && code.trim() ? code : null,
    errorDetails:
      details && typeof details === 'object'
        ? (details as Record<string, unknown>)
        : null,
    recoveryOffer:
      recoveryOffer && typeof recoveryOffer === 'object'
        ? (recoveryOffer as CycleRecoveryOffer)
        : null,
  };
}

export class ApiError extends Error {
  readonly status: number;
  readonly payload: unknown;
  readonly errorCode: string | null;
  readonly errorDetails: Record<string, unknown> | null;
  readonly recoveryOffer: CycleRecoveryOffer | null;

  constructor(message: string, status: number, payload: unknown) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.payload = payload;
    const structured = structuredErrorFromPayload(payload);
    this.errorCode = structured.errorCode;
    this.errorDetails = structured.errorDetails;
    this.recoveryOffer = structured.recoveryOffer;
  }
}

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(`${BASE_URL}${path}`, {
    method,
    headers: body ? { 'Content-Type': 'application/json' } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  });
  if (!res.ok) {
    const payload = await res.json().catch(() => null);
    throw new ApiError(
      errorMessageFromPayload(payload, res.statusText || `HTTP ${res.status}`),
      res.status,
      payload,
    );
  }
  return res.json();
}

export const api = {
  cycle: {
    status: () => request<CycleStatus>('GET', '/api/cycle/status'),
    run: () => request<CycleCommand>('POST', '/api/cycle/run'),
    stop: () => request<CycleCommand>('POST', '/api/cycle/stop'),
    fullStop: () => request<CycleCommand>('POST', '/api/cycle/full-stop'),
    reset: () => request<CycleCommand>('POST', '/api/cycle/reset'),
    applyRecoveryAction: (payload: CycleRecoveryActionRequest) =>
      request<CycleRecoveryActionResult>('POST', '/api/cycle/recovery-actions', payload),
    setVelocityScale: (value: number) =>
      request<VelocityScaleResponse>('PUT', '/api/cycle/velocity-scale', { value }),
  },
  execution: {
    channels: (limit = 80) =>
      request<ExecutionChannelsResponse>(
        'GET',
        `/api/execution/channels?limit=${encodeURIComponent(String(limit))}`,
      ),
  },
  simulation: {
    reset: () => request<SimulationResetResponse>('POST', '/api/simulation/reset'),
    setState: (payload: SimulationSetStateRequest) =>
      request<SimulationControlResponse>('POST', '/api/simulation/state', payload),
    step: (payload: SimulationStepRequest = {}) =>
      request<SimulationControlResponse>('POST', '/api/simulation/step', payload),
    setEntityState: (payload: SimulationSetEntityStateRequest) =>
      request<SimulationControlResponse>('POST', '/api/simulation/entity-state', payload),
  },
  catalog: {
    list: () => request<CatalogResponse>('GET', '/api/catalog'),
    get: (id: string) => request<CatalogEntry>('GET', `/api/catalog/${id}`),
  },
  program: {
    load: (path: string, deploymentPath?: string, forceDiscardOverrides = false) =>
      request<ProgramInfo>('POST', '/api/program/load', { path, deploymentPath, forceDiscardOverrides }),
    get: () => request<ProgramInfo>('GET', '/api/program'),
    saved: () => request<ProgramInfo>('GET', '/api/program/saved'),
    tree: () => request<ProgramTree>('GET', '/api/program/tree'),
    savedTree: () => request<ProgramTree>('GET', '/api/program/saved/tree'),
    validate: (
      tree: TreeNode,
      parameters?: ProgramInfo['parameters'],
      blackboard?: ProgramBlackboard,
    ) =>
      request<ProgramValidateResponse>('POST', '/api/program/validate', {
        tree,
        parameters,
        blackboard,
      }),
    updateTree: (tree: TreeNode) =>
      request<unknown>('PUT', '/api/program/tree', { tree }),
    runtimeOverrides: () =>
      request<RuntimeOverrideStatus>('GET', '/api/program/runtime-overrides'),
    // Day-5c.3: Pose Edit overlay stages node param overrides via this
    // existing route. authoritySource + approvalMode default to
    // operator-driven values that don't require additional auth.
    applyRuntimeOverride: (
      nodeName: string,
      params: Record<string, unknown>,
      reason: string,
    ) =>
      request<RuntimeOverrideStatus>('POST', '/api/program/runtime-overrides', {
        nodeName,
        params,
        authoritySource: 'human',
        approvalMode: 'human_approved',
        reason,
      }),
    activateRuntimeOverrides: () =>
      request<RuntimeOverrideStatus>('POST', '/api/program/runtime-overrides/activate'),
    clearRuntimeOverrides: () =>
      request<RuntimeOverrideStatus>('POST', '/api/program/runtime-overrides/clear'),
    // Promote staged runtime overrides into the loaded program
    // (in-memory), so a subsequent /api/program/save persists them.
    promoteRuntimeOverridesToLoaded: () =>
      request<unknown>('POST', '/api/program/runtime-overrides/promote', {
        target: 'loaded_program',
      }),
    save: (path: string, parameters?: ProgramInfo['parameters'], blackboard?: ProgramBlackboard) =>
      request<unknown>('POST', '/api/program/save', {
        path,
        parameters,
        blackboard,
      }),
    saveTree: (
      path: string,
      tree: TreeNode,
      scene?: ProgramInfo['sceneResources'],
      parameters?: ProgramInfo['parameters'],
      blackboard?: ProgramBlackboard,
      compatibility?: ProgramCompatibility | null,
    ) =>
      request<unknown>('POST', '/api/program/save', {
        path,
        tree,
        scene,
        parameters,
        blackboard,
        compatibility,
      }),
  },
  nodes: {
    diagnostics: () => request<NodeDiscoveryReport>('GET', '/api/nodes/diagnostics'),
    reload: () => request<NodeDiscoveryReport>('POST', '/api/nodes/reload'),
  },
  scenarios: {
    list: () => request<ScenarioCatalogResponse>('GET', '/api/scenarios'),
  },
  deployment: {
    load: (path: string) =>
      request<DeploymentInfo>('POST', '/api/deployment/load', { path }),
    get: () => request<DeploymentInfo>('GET', '/api/deployment'),
    getFull: () => request<FullDeployment>('GET', '/api/deployment/full'),
    getSavedFull: () => request<FullDeployment>('GET', '/api/deployment/saved/full'),
    update: (data: Partial<FullDeployment>) =>
      request<{ status: string }>('PUT', '/api/deployment', data),
    save: (path: string) =>
      request<{ status: string; path: string }>('POST', '/api/deployment/save', { path }),
    saveFull: (path: string, deployment: FullDeployment) =>
      request<{ status: string; path: string }>('POST', '/api/deployment/save', { path, deployment }),
  },
  session: {
    apply: (payload: {
      tree?: TreeNode;
      scene?: ProgramInfo['sceneResources'];
      deployment?: FullDeployment;
      programParameters?: ProgramParameter[];
      blackboard?: ProgramBlackboard;
      compatibility?: ProgramCompatibility | null;
      forceDiscardOverrides?: boolean;
    }) =>
      request<ApplySessionResponse>('POST', '/api/session/apply', payload),
  },
  files: {
    browse: (kind: 'program' | 'deployment', path?: string) => {
      const params = new URLSearchParams({ kind });
      if (path) {
        params.set('path', path);
      }
      return request<FileBrowserResponse>('GET', `/api/files/browse?${params.toString()}`);
    },
  },
  infra: {
    status: () => request<InfraStatus>('GET', '/api/infra/status'),
    modelEndpoints: () => request<ModelEndpointCatalog>('GET', '/api/infra/model-endpoints'),
  },
  runtime: {
    status: () => request<RuntimeStatus>('GET', '/api/runtime/status'),
  },
  urdf: {
    manifest: () => request<SceneUrdfManifest>('GET', '/api/scene/urdf/manifest'),
    meshUrl: (modelKey: string, linkName: string, visualIndex = 0) =>
      `${BASE_URL}/api/scene/urdf/models/${encodeURIComponent(modelKey)}/meshes/${encodeURIComponent(linkName)}/${visualIndex}`,
  },
  direct: {
    // Day 5b: operator-driven single-joint jog from the
    // robot-control overlay. Wraps the existing intervention
    // machinery so the cycle manager + planner + kernel handle
    // the jog through their normal lifecycle.
    jointJog: (jointNames: string[], jointPositionsRad: number[]) =>
      request<unknown>('POST', '/api/direct/joint-jog', {
        jointNames,
        jointPositionsRad,
      }),
    // Day 5c.2: Cartesian end-effector jog via the panel's
    // Cartesian tab + 3D gizmo. positionM and orientationXyzw
    // express the target TCP pose in the robot's base frame.
    cartesianJog: (
      positionM: number[],
      orientationXyzw: number[],
      frameId?: string,
      tcpFrame?: string,
    ) =>
      request<unknown>('POST', '/api/direct/cartesian-jog', {
        positionM,
        orientationXyzw,
        ...(frameId ? { frameId } : {}),
        ...(tcpFrame ? { tcpFrame } : {}),
      }),
  },
  // Phase 4-P5 validated Robot Control: author a server-owned candidate
  // (no motion), poll its validated status, then Approve to move the robot
  // through the normal execution path. Approve is the ONLY method here that
  // can move the robot; create/get/revert never move it.
  robotControl: {
    candidates: {
      create: (payload: CandidateCreateRequest) =>
        request<CandidateResponse>(
          'POST',
          '/api/robot-control/candidates',
          payload,
        ),
      get: (candidateId: string) =>
        request<CandidateResponse>(
          'GET',
          `/api/robot-control/candidates/${encodeURIComponent(candidateId)}`,
        ),
      execute: (candidateId: string) =>
        request<CandidateExecuteResponse>(
          'POST',
          `/api/robot-control/candidates/${encodeURIComponent(candidateId)}/execute`,
        ),
      revert: (candidateId: string) =>
        request<CandidateResponse>(
          'POST',
          `/api/robot-control/candidates/${encodeURIComponent(candidateId)}/revert`,
        ),
    },
  },
  scene: {
    state: () => request<SceneStateResponse>('GET', '/api/scene/state'),
    // Day-5c followup: persist an authored scene object's
    // pose/shape into the program draft. Wraps the assistant
    // tool's scene_draft_update_object helper; operator commits
    // to YAML via ProgramPanel Save afterwards.
    updateProgramSceneResource: (payload: Record<string, unknown>) =>
      request<unknown>('POST', '/api/scene/program-resource/update', payload),
    edit: (payload: SceneEditRequest) =>
      request<SceneEditResponse>('POST', '/api/scene/edit', payload),
    frames: () => request<SceneFramesResponse>('GET', '/api/scene/frames'),
    setFrameVisibility: (frameName: string, visible: boolean) =>
      request<SceneFrameVisibilityResponse>('POST', '/api/scene/frames/visibility', { frameName, visible }),
    setFrameGizmoSize: (axesLengthM: number) =>
      request<SceneFrameGizmoSizeResponse>('POST', '/api/scene/frames/gizmo-size', { axesLengthM }),
    setObjectFrameVisibility: (objectId: string, visible: boolean) =>
      request<SceneObjectFrameVisibilityResponse>('POST', '/api/scene/objects/frame-visibility', { objectId, visible }),
    setObjectAxesGizmoSize: (axesLengthM: number) =>
      request<SceneObjectAxesGizmoSizeResponse>('POST', '/api/scene/objects/gizmo-size', { axesLengthM }),
    applyPoseSources: (policy = 'when_idle', objectIds: string[] = []) =>
      request<PoseSourceApplyResponse>('POST', '/api/scene/pose-sources/apply', {
        policy,
        objectIds,
      }),
    commandGripper: (payload: {
      gripperId: string;
      targetPosition: number;
      maxEffort?: number;
      durationS?: number;
    }) =>
      request<SceneGripperCommandResponse>('POST', '/api/scene/gripper/command', payload),
    // Ghost-robot preview: given a target joint config OR a cartesian
    // TCP pose, returns link_poses at that config (running IK then
    // per-link FK on the kernel's planning context, no scene state
    // mutation). On IK failure returns {reachable: false} + the TCP
    // pose so the frontend can render a red TCP-only ghost as the
    // "won't reach" indicator.
    previewLinkPoses: (payload: PreviewLinkPosesRequest) =>
      request<PreviewLinkPosesResponse>('POST', '/api/scene/preview-link-poses', payload),
  },
  assistant: {
    providers: () =>
      request<AssistantProviderCatalog>('GET', '/api/assistant/providers'),
    chat: (payload: AssistantChatRequest) =>
      request<AssistantChatResponse>('POST', '/api/assistant/chat', payload),
    status: (requestId: string) =>
      request<AssistantRequestStatus>(
        'GET',
        `/api/assistant/requests/${encodeURIComponent(requestId)}`,
      ),
    cancel: (requestId: string) =>
      request<AssistantCancelResponse>(
        'POST',
        `/api/assistant/requests/${encodeURIComponent(requestId)}/cancel`,
        {},
      ),
  },
};
