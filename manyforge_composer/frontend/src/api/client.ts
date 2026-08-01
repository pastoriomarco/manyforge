import {
  parseCatalogResponse,
  parseFileBrowserResponse,
  parseHealthResponse,
  parseSceneUrdfManifest,
} from './parsers';
import type {
  ArtifactKind,
  CatalogResponse,
  FileBrowserResponse,
  HealthResponse,
  SceneUrdfManifest,
} from '../types/api';

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
): Promise<T> {
  const response = await fetch(resolveStructuralAssetUrl(path), {
    method: 'GET',
    headers: { Accept: 'application/json' },
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
};
