export interface JointTargetDraft {
  jointNames: string[];
  positionsRad: number[];
}

/**
 * Accumulate an edited joint into the running set of authored joint drafts.
 *
 * Joint candidates are authored as a PARTIAL target that the backend overlays
 * onto the live joint vector. Authoring only the most-recently-edited joint
 * would silently drop earlier edits (the backend would use the live value for
 * them) while the UI still showed them as amber targets. Accumulating every
 * edited joint keeps the authored candidate consistent with what the operator
 * sees and approves.
 */
export function accumulateJointDrafts(
  prev: Record<string, number>,
  jointName: string,
  valueRad: number,
): Record<string, number> {
  return { ...prev, [jointName]: valueRad };
}

/** Build the partial joint target (names + positions) from accumulated drafts. */
export function jointTargetFromDrafts(
  drafts: Record<string, number>,
): JointTargetDraft {
  const jointNames = Object.keys(drafts);
  return {
    jointNames,
    positionsRad: jointNames.map((name) => drafts[name]),
  };
}
