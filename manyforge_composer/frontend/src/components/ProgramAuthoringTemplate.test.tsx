import { describe, expect, it } from 'vitest';

import type { ProgramAuthoringDocument } from '../types/api';
import { programTcpSelectorChoices } from '../utils/motionEndpointAuthoring';

describe('Pick/Place template TCP choices', () => {
  it('defaults to Current and offers role defaults and portable entries', () => {
    const program = {
      target_roles: {
        arm: { required_frame_roles: ['base'] },
        gripper: { required_frame_roles: ['tip'] },
      },
    } as unknown as ProgramAuthoringDocument;
    const choices = programTcpSelectorChoices(program, [], undefined, false);
    expect(choices[0].selector).toEqual({ kind: 'current' });
    expect(choices.map((choice) => choice.selector)).toContainEqual({
      kind: 'default_of_role', role: 'gripper',
    });
    expect(choices.map((choice) => choice.selector)).toContainEqual({
      kind: 'entry', target_role: 'gripper', frame_role: 'tip',
    });
  });
});
