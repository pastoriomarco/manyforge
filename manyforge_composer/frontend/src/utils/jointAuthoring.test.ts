import { describe, expect, it } from 'vitest';
import {
  accumulateJointDrafts,
  jointDraftList,
  type JointCoordinateDrafts,
} from './jointAuthoring';

describe('joint draft accumulation', () => {
  it('keeps multiple edits and their correct units', () => {
    let drafts: JointCoordinateDrafts = {};
    drafts = accumulateJointDrafts(drafts, 'joint_a', 'revolute', 0.1);
    drafts = accumulateJointDrafts(drafts, 'finger_slide', 'prismatic', 0.02);
    expect(jointDraftList(drafts)).toEqual([
      { jointName: 'joint_a', kind: 'revolute', value: 0.1, unit: 'rad' },
      { jointName: 'finger_slide', kind: 'prismatic', value: 0.02, unit: 'm' },
    ]);
  });

  it('updates one edit without dropping the others', () => {
    let drafts: JointCoordinateDrafts = {};
    drafts = accumulateJointDrafts(drafts, 'joint_a', 'revolute', 0.1);
    drafts = accumulateJointDrafts(drafts, 'finger_slide', 'prismatic', 0.02);
    drafts = accumulateJointDrafts(drafts, 'joint_a', 'revolute', 0.5);
    expect(jointDraftList(drafts)).toEqual([
      { jointName: 'joint_a', kind: 'revolute', value: 0.5, unit: 'rad' },
      { jointName: 'finger_slide', kind: 'prismatic', value: 0.02, unit: 'm' },
    ]);
  });

  it('rejects invalid coordinates before they enter retained UI state', () => {
    expect(() => accumulateJointDrafts({}, '', 'revolute', 0)).toThrow(TypeError);
    expect(() => accumulateJointDrafts({}, 'joint_a', 'revolute', Number.NaN)).toThrow(
      TypeError,
    );
  });
});
