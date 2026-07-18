import { afterEach, describe, expect, it, vi } from 'vitest';

import { viewerProtocolFixture as fixture } from '../types/viewerProtocol.fixture.generated';
import type {
  ViewerFrameV1,
  ViewerManifestV1,
} from '../types/viewerProtocol.generated';
import {
  __viewerFrameStoreTest,
  applyLatestViewerFrame,
  getLatestViewerSnapshot,
  publishViewerFrame,
  publishViewerManifest,
  registerViewerFrameApplier,
  subscribeViewerFrameWake,
  waitForViewerFrameGeneration,
} from './viewerFrameStore';

const manifest = fixture.manifest as ViewerManifestV1;
const frame = fixture.frame as ViewerFrameV1;

describe('authoritative viewer frame slot', () => {
  afterEach(() => __viewerFrameStoreTest.reset());

  it('keeps only the latest self-contained frame and applies it once per sequence', () => {
    const wake = vi.fn();
    const apply = vi.fn(() => 1);
    subscribeViewerFrameWake(wake);
    publishViewerManifest(manifest);
    publishViewerFrame(frame, 10);
    registerViewerFrameApplier(apply);

    expect(getLatestViewerSnapshot()?.frame).toEqual(frame);
    expect(apply).toHaveBeenCalledTimes(1);
    expect(applyLatestViewerFrame().applied).toBe(true);
    expect(apply).toHaveBeenCalledTimes(2);
    expect(applyLatestViewerFrame().applied).toBe(false);

    publishViewerFrame({
      ...frame,
      frame_sequence: frame.frame_sequence + 1,
      render_revision: frame.render_revision + 1,
      timestamp_ns: 20,
    }, 20);
    publishViewerFrame({
      ...frame,
      frame_sequence: frame.frame_sequence + 2,
      render_revision: frame.render_revision + 2,
      timestamp_ns: 30,
    }, 30);
    expect(applyLatestViewerFrame().applied).toBe(true);
    expect(apply).toHaveBeenLastCalledWith(
      manifest,
      expect.objectContaining({ frame_sequence: frame.frame_sequence + 2 }),
      expect.objectContaining({ frame_sequence: frame.frame_sequence + 1 }),
      1,
    );
    expect(wake).toHaveBeenCalled();
  });

  it('retains time-only authority frames without invalidating an identical scene', () => {
    const wake = vi.fn();
    subscribeViewerFrameWake(wake);
    publishViewerManifest(manifest);
    publishViewerFrame(frame, 10);
    const wakesAfterInitialFrame = wake.mock.calls.length;

    publishViewerFrame({
      ...frame,
      frame_sequence: frame.frame_sequence + 1,
      timestamp_ns: frame.timestamp_ns + 1,
      source_sequence: frame.source_sequence + 1,
    }, 20);

    expect(getLatestViewerSnapshot()?.frame.frame_sequence).toBe(frame.frame_sequence + 1);
    expect(wake).toHaveBeenCalledTimes(wakesAfterInitialFrame);
  });

  it('rejects mismatched ordered blocks before replacing the latest frame', () => {
    publishViewerManifest(manifest);
    publishViewerFrame(frame);
    expect(() => publishViewerFrame({
      ...frame,
      entity_transforms: [],
      frame_sequence: frame.frame_sequence + 1,
    })).toThrow('entity transform count');
    expect(getLatestViewerSnapshot()?.frame.frame_sequence).toBe(frame.frame_sequence);
  });

  it('invalidates a frame when a new manifest generation is published', () => {
    publishViewerManifest(manifest);
    publishViewerFrame(frame);
    publishViewerManifest({
      ...manifest,
      producer_epoch: 'viewer-epoch-next',
      manifest_revision: manifest.manifest_revision + 1,
    });
    expect(getLatestViewerSnapshot()).toBeNull();
  });

  it('makes manifest and frame resync duplicates idempotent', () => {
    const wake = vi.fn();
    subscribeViewerFrameWake(wake);
    expect(publishViewerManifest(manifest)).toBe(true);
    publishViewerFrame(frame);
    const wakeCount = wake.mock.calls.length;

    expect(publishViewerManifest({ ...manifest })).toBe(false);
    publishViewerFrame({ ...frame });

    expect(wake).toHaveBeenCalledTimes(wakeCount);
    expect(getLatestViewerSnapshot()?.frame).toBe(frame);
  });

  it('rejects manifest regression and conflicting reused identities', () => {
    publishViewerManifest({ ...manifest, manifest_revision: 2 });

    expect(() => publishViewerManifest({
      ...manifest,
      manifest_revision: 1,
    })).toThrow('manifest revision regressed');
    expect(() => publishViewerManifest({
      ...manifest,
      manifest_revision: 2,
      content_hash: 'f'.repeat(64),
    })).toThrow('manifest revision was reused');
  });

  it('rejects a frame sequence reused with a conflicting render revision', () => {
    publishViewerManifest(manifest);
    publishViewerFrame(frame);

    expect(() => publishViewerFrame({
      ...frame,
      render_revision: frame.render_revision + 1,
    })).toThrow('frame sequence was reused');
  });

  it('resolves a snapshot barrier only after the required generation arrives', async () => {
    publishViewerManifest(manifest);
    publishViewerFrame(frame);
    const requiredSequence = frame.frame_sequence + 1;
    const waiting = waitForViewerFrameGeneration(
      frame.producer_epoch,
      requiredSequence,
      100,
    );

    publishViewerFrame({
      ...frame,
      frame_sequence: requiredSequence,
      render_revision: frame.render_revision + 1,
    });

    await expect(waiting).resolves.toBe(true);
    await expect(waitForViewerFrameGeneration(
      frame.producer_epoch,
      requiredSequence,
      100,
    )).resolves.toBe(true);
  });

  it('interpolates only between authoritative frames and never extrapolates', () => {
    const apply = vi.fn(() => 1);
    publishViewerManifest(manifest);
    publishViewerFrame(frame, 100);
    registerViewerFrameApplier(apply);
    apply.mockClear();

    const nextFrame: ViewerFrameV1 = {
      ...frame,
      frame_sequence: frame.frame_sequence + 1,
      render_revision: frame.render_revision + 1,
      entity_transforms: frame.entity_transforms.map((pose, index) => (
        index === 0 && pose !== null
          ? { ...pose, position_m: [pose.position_m[0] + 0.1, ...pose.position_m.slice(1)] }
          : pose
      )),
    };
    publishViewerFrame(nextFrame, 200);

    expect(applyLatestViewerFrame(200, 100)).toEqual({
      applied: true,
      interpolationPending: true,
    });
    expect(apply).toHaveBeenLastCalledWith(manifest, nextFrame, frame, 0);
    expect(applyLatestViewerFrame(250, 100)).toEqual({
      applied: true,
      interpolationPending: true,
    });
    expect(apply).toHaveBeenLastCalledWith(manifest, nextFrame, frame, 0.5);
    expect(applyLatestViewerFrame(300, 100)).toEqual({
      applied: true,
      interpolationPending: false,
    });
    expect(apply).toHaveBeenLastCalledWith(manifest, nextFrame, frame, 1);
    expect(applyLatestViewerFrame(400, 100)).toEqual({
      applied: false,
      interpolationPending: false,
    });
    expect(getLatestViewerSnapshot()?.frame).toBe(nextFrame);
  });

  it('applies stale evidence immediately and rejects sequence regression', () => {
    const apply = vi.fn(() => 1);
    publishViewerManifest(manifest);
    publishViewerFrame(frame, 100);
    registerViewerFrameApplier(apply);
    const staleFrame: ViewerFrameV1 = {
      ...frame,
      stale: true,
      frame_sequence: frame.frame_sequence + 1,
      render_revision: frame.render_revision + 1,
    };
    publishViewerFrame(staleFrame, 200);
    expect(applyLatestViewerFrame(200, 100)).toEqual({
      applied: true,
      interpolationPending: false,
    });
    expect(apply).toHaveBeenLastCalledWith(manifest, staleFrame, frame, 1);
    expect(() => publishViewerFrame({
      ...frame,
      frame_sequence: frame.frame_sequence - 1,
    }, 300)).toThrow('sequence regressed');
  });
});
