import { describe, expect, it, vi } from 'vitest';

vi.mock('../api/client', () => ({
  api: { scene: { edit: vi.fn().mockResolvedValue(undefined) } },
}));

import { api } from '../api/client';
import { restoreObjectToProgramPose } from './objectMoveDiscard';

const edit = (api.scene as unknown as { edit: ReturnType<typeof vi.fn> }).edit;

describe('restoreObjectToProgramPose', () => {
  it('restores the live object pose to the program pose via scene/edit update_pose', async () => {
    edit.mockClear();
    await restoreObjectToProgramPose('obj-1', {
      position: [1, 2, 3],
      quaternion: [0, 0, 0, 1],
    });
    // No-motion discard: a single update_pose edit carrying the program pose,
    // never a program mutation.
    expect(edit).toHaveBeenCalledTimes(1);
    expect(edit).toHaveBeenCalledWith({
      operation: 'update_pose',
      object_id: 'obj-1',
      pose_position_m: [1, 2, 3],
      pose_orientation_xyzw: [0, 0, 0, 1],
    });
  });
});
