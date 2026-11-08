import { describe, expect, it } from 'vitest';
import { programTcpSelectorChoices } from '../utils/motionEndpointAuthoring';

describe('Pick/Place template TCP choices', () => {
  it('keeps Current without fabricating TCP entries when Draft facts are unavailable', () => {
    expect(programTcpSelectorChoices({ component_id: 'arm', command_target_id: 'motion' }, null))
      .toEqual([{ key: JSON.stringify(['current']), label: 'Current: Active TCP at run time', selector: { kind: 'current' } }]);
  });
});
