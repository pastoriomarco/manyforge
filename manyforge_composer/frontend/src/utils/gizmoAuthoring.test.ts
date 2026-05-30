import { describe, expect, it, vi } from 'vitest';

import { authorGizmoRelease, gizmoCartesianRequest } from './gizmoAuthoring';
import type { PoseTuple } from './poseTransforms';

const ANCHOR: PoseTuple = {
  position: [0.5, 0.1, 0.4],
  quaternion: [0, 0, 0, 1],
};
const BASE: PoseTuple = {
  position: [0, 0, 0],
  quaternion: [0, 0, 0, 1],
};

describe('gizmoCartesianRequest', () => {
  it('returns null when the base world pose is unknown (cannot resolve frame)', () => {
    expect(gizmoCartesianRequest(ANCHOR, null, 'tool0')).toBeNull();
  });

  it('returns a base-frame cartesian request when the base world pose is known', () => {
    const req = gizmoCartesianRequest(ANCHOR, BASE, 'tool0');
    expect(req).not.toBeNull();
    expect(req?.sourceSurface).toBe('cartesian_gizmo');
    expect(req?.inputMode).toBe('cartesian');
    expect(req?.cartesianTarget?.frameId).toBe('base');
    expect(req?.cartesianTarget?.tcpFrame).toBe('tool0');
    expect(req?.cartesianTarget?.positionM).toHaveLength(3);
    expect(req?.cartesianTarget?.orientationXyzw).toHaveLength(4);
  });
});

describe('authorGizmoRelease', () => {
  it('does NOT author and rejects the draft when the frame cannot be resolved', () => {
    const author = vi.fn();
    const rejectDraft = vi.fn();
    authorGizmoRelease(ANCHOR, null, 'tool0', { author, rejectDraft });
    // Fail closed: no candidate authored for a mis-framed release.
    expect(author).not.toHaveBeenCalled();
    // And the prior draft is made non-approvable.
    expect(rejectDraft).toHaveBeenCalledTimes(1);
  });

  it('authors exactly once when the frame can be resolved', () => {
    const author = vi.fn();
    const rejectDraft = vi.fn();
    authorGizmoRelease(ANCHOR, BASE, 'tool0', { author, rejectDraft });
    expect(author).toHaveBeenCalledTimes(1);
    expect(rejectDraft).not.toHaveBeenCalled();
    expect(author.mock.calls[0][0].cartesianTarget.frameId).toBe('base');
  });
});
