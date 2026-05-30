import { describe, expect, it } from 'vitest';

import { gripperCandidateRequest } from './gripperAuthoring';

describe('gripperCandidateRequest', () => {
  it('builds a gripper candidate (not an immediate command) for a slider release', () => {
    const req = gripperCandidateRequest('primary', 0.42, 'gripper_slider');
    expect(req.inputMode).toBe('gripper');
    expect(req.sourceSurface).toBe('gripper_slider');
    expect(req.gripperTarget).toEqual({ gripperId: 'primary', targetPosition: 0.42 });
    // Authoring only: no joint/cartesian payload, no direct command shape.
    expect(req.jointTarget).toBeUndefined();
    expect(req.cartesianTarget).toBeUndefined();
  });

  it('builds a gripper candidate for a preset click', () => {
    const req = gripperCandidateRequest('primary', 0.0, 'gripper_preset');
    expect(req.inputMode).toBe('gripper');
    expect(req.sourceSurface).toBe('gripper_preset');
    expect(req.gripperTarget).toEqual({ gripperId: 'primary', targetPosition: 0.0 });
  });
});
