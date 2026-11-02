import { describe, expect, it, vi } from 'vitest';

import type { ViewerModelPresentation } from './viewerModelReadiness';
import { viewerNotices, type ViewerNoticeInput } from './viewerNotices';

const READY = {
  status: 'ready',
  manifest: {},
} as unknown as ViewerModelPresentation;

function input(overrides: Partial<ViewerNoticeInput> = {}): ViewerNoticeInput {
  return {
    modelPresentation: READY,
    staleScenePreview: false,
    robotMeshFailure: null,
    assetFailure: null,
    previewMeshFailure: null,
    pointCloudFailure: null,
    geometryRetrying: false,
    retryModel: vi.fn(),
    reloadModel: vi.fn(),
    ...overrides,
  };
}

describe('viewer notice classification', () => {
  it('has no notice for a verified current view', () => {
    expect(viewerNotices(input())).toEqual([]);
  });

  it('treats expected loading as information that marks the view non-current', () => {
    for (const reason of ['transition', 'loading', 'connecting'] as const) {
      const [notice] = viewerNotices(input({
        modelPresentation: { status: 'synchronizing', reason, message: 'Loading.' },
      }));
      expect(notice).toMatchObject({ severity: 'info', busy: true });
      expect(notice.message).toContain('not current');
    }
  });

  it('makes an unverifiable or unavailable robot model an error with Retry', () => {
    const retryModel = vi.fn();
    for (const reason of ['identity_mismatch', 'projection_stale'] as const) {
      const [notice] = viewerNotices(input({
        retryModel,
        modelPresentation: { status: 'synchronizing', reason, message: 'Mismatch.' },
      }));
      expect(notice).toMatchObject({
        severity: 'error',
        title: 'Applied robot model not verified',
        expectedDuringTransition: true,
        incidentKind: 'viewer_model_unavailable',
      });
      expect(notice.actions?.map((action) => action.label)).toEqual(['Retry model projections']);
    }
    const [unavailable] = viewerNotices(input({
      retryModel,
      modelPresentation: { status: 'unavailable', message: 'Manager unavailable.' },
    }));
    // A failed requirement stays red even during a transition.
    expect(unavailable).toMatchObject({ severity: 'error', title: 'Applied robot model unavailable' });
    expect(unavailable.expectedDuringTransition).toBeUndefined();
    unavailable.actions?.[0].run();
    expect(retryModel).toHaveBeenCalledOnce();
  });

  it('marks a retained robot model not verified without hiding it', () => {
    const retainModel = vi.fn();
    for (const reason of ['transport_delayed', 'transport_stale'] as const) {
      const notices = viewerNotices(input({
        retryModel: retainModel,
        robotMeshFailure: 'link_2.glb failed.',
        modelPresentation: {
          status: 'retained',
          manifest: {},
          reason,
          message: 'Control connection delayed: not confirmed.',
        } as unknown as ViewerModelPresentation,
      }));
      // The hold-then-error rule applies (error after 500 ms, reported as an
      // incident); a transport delay is not a known controlled transition.
      expect(notices[0]).toMatchObject({
        id: 'viewer.model',
        severity: 'error',
        title: 'Applied robot model not verified',
        message: 'Control connection delayed: not confirmed.',
        incidentKind: 'viewer_model_unavailable',
      });
      expect(notices[0].expectedDuringTransition).toBeUndefined();
      expect(notices[0].busy).toBeUndefined();
      // The displayed (retained) robot still reports its missing geometry.
      expect(notices.map((notice) => notice.id)).toContain('viewer.robot-mesh');
    }
  });

  it('makes an unverified Applied Scene an error', () => {
    expect(viewerNotices(input({ staleScenePreview: true }))).toEqual([
      expect.objectContaining({
        id: 'viewer.scene', severity: 'error', title: 'Applied Scene not verified',
        expectedDuringTransition: true, incidentKind: 'viewer_scene_unverified',
      }),
    ]);
  });

  it('separates missing robot geometry (error) from optional detail (warning)', () => {
    const reloadModel = vi.fn();
    const robotMesh = viewerNotices(input({ reloadModel, robotMeshFailure: 'link_3.glb failed.' }));
    expect(robotMesh).toEqual([expect.objectContaining({
      id: 'viewer.robot-mesh', severity: 'error', incidentKind: 'viewer_geometry_unavailable',
    })]);
    expect(robotMesh[0].actions?.map((action) => action.label)).toEqual(['Reload active model']);
    const asset = viewerNotices(input({
      assetFailure: { kind: 'unavailable', message: 'Manifest mesh missing.' },
      robotMeshFailure: 'also failed',
    }));
    // The whole-model failure supersedes the isolated mesh report.
    expect(asset.map((notice) => notice.id)).toEqual(['viewer.asset']);
    expect(asset[0].expectedDuringTransition).toBe(false);
    expect(viewerNotices(input({
      assetFailure: { kind: 'stale_model', message: 'old key' },
    }))[0]).toMatchObject({ title: 'Active model changed', expectedDuringTransition: true });
    expect(viewerNotices(input({
      previewMeshFailure: 'ghost.glb failed.',
      pointCloudFailure: 'Observer offline.',
    })).map((notice) => [notice.id, notice.severity])).toEqual([
      ['viewer.preview-mesh', 'warning'],
      ['viewer.point-cloud', 'warning'],
    ]);
  });

  it('keeps geometry errors red and reported while a reload is still running', () => {
    const notices = viewerNotices(input({
      robotMeshFailure: 'mesh 404',
      assetFailure: { kind: 'unavailable', message: 'model mesh failed' },
      geometryRetrying: true,
    }));
    for (const id of ['viewer.asset']) {
      const notice = notices.find((candidate) => candidate.id === id);
      expect(notice).toMatchObject({
        severity: 'error', busy: true, incidentKind: 'viewer_geometry_unavailable',
      });
      expect(notice?.actions?.[0]).toMatchObject({ label: 'Reloading…', disabled: true });
    }
  });

  it('does not report a robot-mesh failure while the model itself is not ready', () => {
    expect(viewerNotices(input({
      modelPresentation: { status: 'synchronizing', reason: 'transition', message: 'Pending.' },
      robotMeshFailure: 'stale failure',
    })).map((notice) => notice.id)).toEqual(['viewer.model']);
  });
});
