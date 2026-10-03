import type {
  AlarmLifecycleState,
  AlarmMotionAttentionMember,
  OperatorAlarmSnapshot,
} from '../types/headless';

/** One gating member; empty `resourceIds` names the cell-global subject. */
export function gatingMember(
  conditionId: string,
  lifecycleState: AlarmLifecycleState,
  resourceIds: string[],
): AlarmMotionAttentionMember {
  return {
    conditionId,
    activationId: `${conditionId}.activation`,
    recordRevision: 1,
    lifecycleState,
    definitionId: resourceIds.length === 0
      ? 'manyforge.runtime.provider_unavailable'
      : 'manyforge.motion.runtime_collision',
    subject: resourceIds.length === 0
      ? { subjectKind: 'cell', subjectId: 'cell.test', resourceIds: [] }
      : { subjectKind: 'operation', subjectId: conditionId, resourceIds },
  };
}

export function alarmSnapshot(
  members: AlarmMotionAttentionMember[],
  reconciled = true,
): OperatorAlarmSnapshot {
  return {
    schemaVersion: 'manyforge.operator_alarm_snapshot.v2',
    ownerId: 'headless.alarm',
    ownerEpoch: 'alarm.epoch.1',
    generation: 3,
    stateRevision: 3,
    reconciled,
    healthState: reconciled ? 'available' : 'unavailable',
    liveCount: members.length,
    unacknowledgedCount: members.filter(
      (member) => member.lifecycleState !== 'active_acknowledged',
    ).length,
    activeCount: members.length,
    activeAcknowledgedCount: 0,
    motionAttentionCount: members.length,
    motionAttentionMembership: members,
    unacknowledgedMembership: [],
    alarms: [],
    truncated: false,
  };
}
