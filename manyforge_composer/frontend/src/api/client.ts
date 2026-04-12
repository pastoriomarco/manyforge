import type {
  CycleStatus,
  CycleCommand,
  VelocityScaleResponse,
  CatalogResponse,
  CatalogEntry,
  ProgramInfo,
  ProgramTree,
  TreeNode,
  RuntimeOverrideStatus,
  DeploymentInfo,
  FullDeployment,
  ApplySessionResponse,
  FileBrowserResponse,
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
    load: (path: string, deploymentPath?: string) =>
      request<ProgramInfo>('POST', '/api/program/load', { path, deploymentPath }),
    get: () => request<ProgramInfo>('GET', '/api/program'),
    tree: () => request<ProgramTree>('GET', '/api/program/tree'),
    savedTree: () => request<ProgramTree>('GET', '/api/program/saved/tree'),
    updateTree: (tree: TreeNode) =>
      request<unknown>('PUT', '/api/program/tree', { tree }),
    runtimeOverrides: () =>
      request<RuntimeOverrideStatus>('GET', '/api/program/runtime-overrides'),
    activateRuntimeOverrides: () =>
      request<RuntimeOverrideStatus>('POST', '/api/program/runtime-overrides/activate'),
    clearRuntimeOverrides: () =>
      request<RuntimeOverrideStatus>('POST', '/api/program/runtime-overrides/clear'),
    save: (path: string) =>
      request<unknown>('POST', '/api/program/save', { path }),
    saveTree: (path: string, tree: TreeNode) =>
      request<unknown>('POST', '/api/program/save', { path, tree }),
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
    apply: (payload: { tree?: TreeNode; deployment?: FullDeployment }) =>
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
};
