export interface AlarmAcknowledgementMember {
  conditionId: string;
  activationId: string;
  recordRevision: number;
}

export interface AlarmAcknowledgementFence {
  /** The start of the admitted snapshot the alarm set came from. */
  applicationStartId: string;
  generation: number;
  members: AlarmAcknowledgementMember[];
}

export function acknowledgementFence(
  applicationStartId: string,
  generation: number,
  members: AlarmAcknowledgementMember[],
): AlarmAcknowledgementFence {
  return {
    applicationStartId,
    generation,
    members: members.map((member) => ({ ...member })),
  };
}

function memberKey(member: AlarmAcknowledgementMember): string {
  return `${JSON.stringify(member.conditionId)}:${JSON.stringify(member.activationId)}:${member.recordRevision}`;
}

/** Confirmation remains bound to exact observed membership and generation. */
export function acknowledgementFenceMatches(
  fence: AlarmAcknowledgementFence,
  applicationStartId: string,
  generation: number,
  members: AlarmAcknowledgementMember[],
): boolean {
  if (
    fence.applicationStartId !== applicationStartId
    || fence.generation !== generation
    || fence.members.length !== members.length
  ) {
    return false;
  }
  const expected = fence.members.map(memberKey).sort();
  const current = members.map(memberKey).sort();
  return expected.every((value, index) => value === current[index]);
}

export function hasNewAlarmActivation(
  previous: string[],
  current: string[],
): boolean {
  const previousSet = new Set(previous);
  return current.some((activation) => !previousSet.has(activation));
}
