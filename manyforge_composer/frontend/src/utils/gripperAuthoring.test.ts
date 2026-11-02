import { describe, expect, it } from 'vitest';
import { gripperPositionDraft } from './gripperAuthoring';

describe('gripper authoring drafts', () => {
  it('keeps articulated prismatic values in metres', () => {
    expect(
      gripperPositionDraft('robot-1', 'gripper', 'prismatic', 0.042, 'gripper_slider'),
    ).toEqual({
      source: 'gripper_slider',
      address: { componentId: 'robot-1', targetId: 'gripper' },
      coordinate: { kind: 'prismatic', value: 0.042, unit: 'm' },
    });
  });

  it('keeps revolute values in radians and rejects unaddressed drafts', () => {
    expect(
      gripperPositionDraft('tool-1', 'fingers', 'revolute', 0.5, 'gripper_preset'),
    ).toEqual(expect.objectContaining({
      coordinate: { kind: 'revolute', value: 0.5, unit: 'rad' },
    }));
    expect(
      gripperPositionDraft('', 'gripper', 'prismatic', 0.01, 'gripper_slider'),
    ).toBeNull();
  });
});
