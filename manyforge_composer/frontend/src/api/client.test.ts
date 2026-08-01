import { afterEach, describe, expect, it, vi } from 'vitest';
import { ApiError, api, resolveStructuralAssetUrl } from './client';

afterEach(() => {
  vi.unstubAllGlobals();
});

function response(
  payload: unknown,
  options: { ok?: boolean; status?: number; statusText?: string } = {},
): Response {
  return {
    ok: options.ok ?? true,
    status: options.status ?? 200,
    statusText: options.statusText ?? 'OK',
    json: vi.fn().mockResolvedValue(payload),
  } as unknown as Response;
}

describe('structural API client', () => {
  it('encodes sandbox browse parameters and validates the response', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({
      currentPath: '/workspace/cells',
      parentPath: '/workspace',
      roots: ['/workspace'],
      entries: [
        {
          name: 'cell.deployment.yaml',
          path: '/workspace/cells/cell.deployment.yaml',
          entryType: 'file',
          selectable: true,
        },
      ],
    }));
    vi.stubGlobal('fetch', fetchMock);

    const result = await api.files.browse('deployment', '/workspace/cells');

    expect(fetchMock).toHaveBeenCalledWith(
      '/api/files/browse?kind=deployment&path=%2Fworkspace%2Fcells',
      expect.objectContaining({ method: 'GET' }),
    );
    expect(result.entries[0]?.selectable).toBe(true);
  });

  it('preserves typed HTTP status and backend detail', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response(
      { detail: 'No URDF available for the active deployment' },
      { ok: false, status: 404, statusText: 'Not Found' },
    )));

    const error = await api.urdf.manifest().catch((caught: unknown) => caught);

    expect(error).toBeInstanceOf(ApiError);
    expect(error).toMatchObject({
      status: 404,
      message: 'No URDF available for the active deployment',
    });
  });

  it('reports invalid successful payloads rather than inventing defaults', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response({ status: 'ok' })));

    await expect(api.health.get()).rejects.toThrow(
      'Invalid response from /api/health',
    );
  });
});

describe('resolveStructuralAssetUrl', () => {
  it('preserves absolute asset URLs and same-origin paths', () => {
    expect(resolveStructuralAssetUrl('https://assets.example/model.glb')).toBe(
      'https://assets.example/model.glb',
    );
    expect(resolveStructuralAssetUrl('/api/model.glb')).toMatch(/\/api\/model\.glb$/);
  });
});
