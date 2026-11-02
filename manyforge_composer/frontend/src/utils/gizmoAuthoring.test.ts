import { describe, expect, it, vi } from 'vitest';
import { gizmoPoseDraft, retainGizmoRelease } from './gizmoAuthoring';
import type { PoseTuple } from './poseTransforms';

const anchor: PoseTuple = {
  position: [0.5, 0.1, 0.4],
  quaternion: [0, 0, 0, 1],
};
const reference: PoseTuple = {
  position: [0.1, 0, 0],
  quaternion: [0, 0, 0, 1],
};
const address = {
  componentId: 'robot-1',
  targetId: 'arm',
  referenceFrame: 'base_link',
};

describe('gizmo authoring drafts', () => {
  it('retains an explicit component/target/frame address', () => {
    expect(gizmoPoseDraft(anchor, reference, address)).toEqual({
      source: 'cartesian_gizmo',
      address,
      pose: {
        relative_to: 'base_link',
        position_m: [0.4, 0.1, 0.4],
        orientation_xyzw: [0, 0, 0, 1],
      },
    });
  });

  it('fails closed instead of producing a mis-framed or unaddressed draft', () => {
    expect(gizmoPoseDraft(anchor, null, address)).toBeNull();
    expect(
      gizmoPoseDraft(anchor, reference, { ...address, componentId: '' }),
    ).toBeNull();

    const retainDraft = vi.fn();
    const rejectDraft = vi.fn();
    retainGizmoRelease(anchor, null, address, { retainDraft, rejectDraft });
    expect(retainDraft).not.toHaveBeenCalled();
    expect(rejectDraft).toHaveBeenCalledTimes(1);
  });

  it('retains one successful release exactly once', () => {
    const retainDraft = vi.fn();
    const rejectDraft = vi.fn();
    retainGizmoRelease(anchor, reference, address, { retainDraft, rejectDraft });
    expect(retainDraft).toHaveBeenCalledTimes(1);
    expect(retainDraft).toHaveBeenCalledWith(gizmoPoseDraft(anchor, reference, address));
    expect(rejectDraft).not.toHaveBeenCalled();
  });
});
