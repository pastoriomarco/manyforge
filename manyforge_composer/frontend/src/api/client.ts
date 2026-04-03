import type {
  CycleStatus,
  CycleCommand,
  VelocityScaleResponse,
  CatalogResponse,
  CatalogEntry,
  ProgramInfo,
  ProgramTree,
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
  },
};
