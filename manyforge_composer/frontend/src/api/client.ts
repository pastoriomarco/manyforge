import type {
  CycleStatus,
  CycleCommand,
  VelocityScaleResponse,
  CatalogResponse,
  CatalogEntry,
  ProgramInfo,
  ProgramParameter,
  ProgramBlackboard,
  ProgramTree,
  ProgramValidateResponse,
  TreeNode,
  RuntimeOverrideStatus,
  DeploymentInfo,
  FullDeployment,
  ApplySessionResponse,
  FileBrowserResponse,
  InfraStatus,
  ModelEndpointCatalog,
  SceneStateResponse,
  SceneEditRequest,
  SceneEditResponse,
  SceneFramesResponse,
  SceneFrameVisibilityResponse,
  AssistantChatRequest,
  AssistantChatResponse,
  AssistantProviderCatalog,
  AssistantRequestStatus,
  AssistantCancelResponse,
} from '../types/api';

const BASE_URL = import.meta.env.VITE_API_URL || '';

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(`${BASE_URL}${path}`, {
    method,
    headers: body ? { 'Content-Type': 'application/json' } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  });
  if (!res.ok) {
    const detail = await res.json().catch(() => ({ detail: res.statusText }));
    throw new Error(detail.detail || res.statusText);
  }
  return res.json();
}

export const api = {
  cycle: {
    status: () => request<CycleStatus>('GET', '/api/cycle/status'),
    run: () => request<CycleCommand>('POST', '/api/cycle/run'),
    stop: () => request<CycleCommand>('POST', '/api/cycle/stop'),
    reset: () => request<CycleCommand>('POST', '/api/cycle/reset'),
    setVelocityScale: (value: number) =>
      request<VelocityScaleResponse>('PUT', '/api/cycle/velocity-scale', { value }),
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
    activateRuntimeOverrides: () =>
      request<RuntimeOverrideStatus>('POST', '/api/program/runtime-overrides/activate'),
    clearRuntimeOverrides: () =>
      request<RuntimeOverrideStatus>('POST', '/api/program/runtime-overrides/clear'),
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
    ) =>
      request<unknown>('POST', '/api/program/save', {
        path,
        tree,
        scene,
        parameters,
        blackboard,
      }),
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
  scene: {
    state: () => request<SceneStateResponse>('GET', '/api/scene/state'),
    edit: (payload: SceneEditRequest) =>
      request<SceneEditResponse>('POST', '/api/scene/edit', payload),
    frames: () => request<SceneFramesResponse>('GET', '/api/scene/frames'),
    setFrameVisibility: (frameName: string, visible: boolean) =>
      request<SceneFrameVisibilityResponse>('POST', '/api/scene/frames/visibility', { frameName, visible }),
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
