import { describe, expect, it } from 'vitest';
import { buildGripperGhostPreview } from './gripperGhost';

const input = {
  previewEligible: true,
  gripperJoint: {
    name: 'finger_slide',
    kind: 'prismatic' as const,
    value: 0.042,
    unit: 'm' as const,
  },
  limitLower: 0,
  limitUpper: 0.08,
  currentCoordinates: [
    { name: 'joint_1', kind: 'revolute' as const, value: 0.1, unit: 'rad' as const },
  ],
  subtreeLinks: ['gripper_base', 'left_finger', 'left_finger'],
};

describe('gripper ghost preview', () => {
  it('appends a prismatic target in metres and deduplicates subtree links', () => {
    expect(buildGripperGhostPreview(input)).toEqual({
      coordinates: [
        { name: 'joint_1', kind: 'revolute', value: 0.1, unit: 'rad' },
        { name: 'finger_slide', kind: 'prismatic', value: 0.042, unit: 'm' },
      ],
      linkNames: ['gripper_base', 'left_finger'],
    });
  });

  it('fails closed for a unit mismatch, limit violation, or unavailable preview', () => {
    expect(buildGripperGhostPreview({
      ...input,
      gripperJoint: { ...input.gripperJoint, unit: 'rad' as const },
    })).toBeNull();
    expect(buildGripperGhostPreview({
      ...input,
      gripperJoint: { ...input.gripperJoint, value: 0.2 },
    })).toBeNull();
    expect(buildGripperGhostPreview({ ...input, previewEligible: false })).toBeNull();
  });

  it('replaces an existing same-name coordinate without duplicating it', () => {
    const preview = buildGripperGhostPreview({
      ...input,
      currentCoordinates: [
        ...input.currentCoordinates,
        { name: 'finger_slide', kind: 'prismatic', value: 0.01, unit: 'm' },
      ],
    });
    expect(preview?.coordinates.filter(({ name }) => name === 'finger_slide')).toEqual([
      { name: 'finger_slide', kind: 'prismatic', value: 0.042, unit: 'm' },
    ]);
  });

  it('requires at least one non-blank gripper subtree link', () => {
    expect(buildGripperGhostPreview({ ...input, subtreeLinks: [] })).toBeNull();
    expect(buildGripperGhostPreview({
      ...input,
      subtreeLinks: ['', '   '],
    })).toBeNull();
  });
});
