import { describe, expect, it } from 'vitest';

import { accumulateJointDrafts, jointTargetFromDrafts } from './jointAuthoring';

describe('joint draft accumulation', () => {
  it('accumulates multiple edited joints (edit A then B keeps BOTH)', () => {
    let drafts: Record<string, number> = {};
    drafts = accumulateJointDrafts(drafts, 'joint_a', 0.1);
    drafts = accumulateJointDrafts(drafts, 'joint_b', 0.2);
    const target = jointTargetFromDrafts(drafts);
    expect(target.jointNames).toEqual(['joint_a', 'joint_b']);
    expect(target.positionsRad).toEqual([0.1, 0.2]);
  });

  it('re-editing a joint updates its value without dropping the others', () => {
    let drafts: Record<string, number> = {};
    drafts = accumulateJointDrafts(drafts, 'joint_a', 0.1);
    drafts = accumulateJointDrafts(drafts, 'joint_b', 0.2);
    drafts = accumulateJointDrafts(drafts, 'joint_a', 0.5);
    const target = jointTargetFromDrafts(drafts);
    expect(target.jointNames).toEqual(['joint_a', 'joint_b']);
    expect(target.positionsRad).toEqual([0.5, 0.2]);
  });

  it('empty drafts produce an empty target', () => {
    const target = jointTargetFromDrafts({});
    expect(target.jointNames).toEqual([]);
    expect(target.positionsRad).toEqual([]);
  });
});
