import { api } from '../api/client';

export interface ProgramRestorePose {
  position: [number, number, number];
  quaternion: [number, number, number, number];
}

/**
 * Discard a pending object move by restoring the LIVE scene object's pose to
 * its program-defined pose via `/api/scene/edit update_pose`.
 *
 * No-motion; does NOT mutate the saved program (that is the explicit
 * "Save to program" action). Shared by the explicit "Discard pending move"
 * button and the Object-mode switch-away path so both behave identically.
 */
export async function restoreObjectToProgramPose(
  objectId: string,
  pose: ProgramRestorePose,
): Promise<void> {
  await api.scene.edit({
    operation: 'update_pose',
    object_id: objectId,
    pose_position_m: pose.position,
    pose_orientation_xyzw: pose.quaternion,
  });
}
