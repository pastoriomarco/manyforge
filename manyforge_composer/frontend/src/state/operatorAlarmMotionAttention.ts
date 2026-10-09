/**
 * Composer's anticipation of the Kernel's operator-alarm motion-attention
 * admission rule (spec 450 §5.2, spec 530 §11). The Kernel is the
 * enforcement point; Composer disables Run, Resume and the motion-producing
 * recovery actions from the same immutable owner projection and names the
 * remedy. Manual Robot Control is never gated by alarms.
 */

import type { CycleRecoveryActionId, CycleRecoveryOffer } from '../types/api';
import type {
  ActiveConfigurationIdentity,
  AlarmMotionAttentionMember,
  OperatorAlarmSnapshot,
} from '../types/headless';
import {
  type OperatorAlarmRefusalCode,
  operatorAlarmRefusalMessage,
} from './operatorAlarmRefusal';

/** Recovery actions that produce motion, hence are attention-gated. */
export const MOTION_PRODUCING_RECOVERY_ACTIONS: ReadonlySet<CycleRecoveryActionId> = new Set([
  'retry_motion_from_current_state',
  'retry_after_scene_repair',
  'replan_from_stopped_state',
  'retry_runtime_step',
]);

export interface MotionAttentionBlock {
  code: OperatorAlarmRefusalCode;
  message: string;
  /** The gating records that intersect the candidate resources. */
  members: AlarmMotionAttentionMember[];
}

function gates(member: AlarmMotionAttentionMember, resources: ReadonlySet<string>): boolean {
  const scoped = member.subject.resourceIds;
  // A cell-global subject gates every operation; a resource-scoped one gates
  // exactly the operations whose reserved resources it intersects.
  return (member.subject.subjectKind === 'cell' && scoped.length === 0)
    || scoped.some((resource) => resources.has(resource));
}

/**
 * Whether the Kernel holds a reconciled projection of the owner's epoch for
 * `activeConfiguration`. The Kernel admits by its held copy alone, which
 * lags the owner snapshot by up to one publication period plus its deadline
 * (spec 530 §11): after an Apply, or when the owner reconciles, the owner
 * snapshot may already read reconciled while the Kernel still refuses.
 */
export function kernelAttentionAvailable(
  alarms: OperatorAlarmSnapshot,
  activeConfiguration: ActiveConfigurationIdentity | null,
): boolean {
  const held = alarms.kernelAttention;
  return held !== null
    && held.reconciled
    && held.ownerEpoch === alarms.ownerEpoch
    && activeConfiguration !== null
    && held.activeConfiguration !== null
    && held.activeConfiguration.retainedBundleId === activeConfiguration.retainedBundleId
    && held.activeConfiguration.graphRevision === activeConfiguration.graphRevision;
}

/**
 * Why the owner's attention projection refuses motion on `resourceIds` (the
 * exact resources the operation reserves), or null when it admits it.
 * `activeConfiguration` is the configuration Manager publishes as active;
 * motion is anticipated only once the Kernel holds a reconciled projection
 * of it.
 */
export function motionAttentionBlock(
  alarms: OperatorAlarmSnapshot | null | undefined,
  resourceIds: readonly string[],
  activeConfiguration: ActiveConfigurationIdentity | null,
): MotionAttentionBlock | null {
  if (
    alarms === null || alarms === undefined || !alarms.reconciled
    || !kernelAttentionAvailable(alarms, activeConfiguration)
  ) {
    const code = 'operator_alarm_state_unavailable';
    return { code, message: operatorAlarmRefusalMessage(code) ?? code, members: [] };
  }
  const resources = new Set(resourceIds);
  const members = alarms.motionAttentionMembership.filter((member) => gates(member, resources));
  // Only a condition interlock gates while acknowledged (spec 530 §4.2);
  // every acknowledgement-gated member is unacknowledged.
  const interlocks = members.filter((member) => member.lifecycleState === 'active_acknowledged');
  const unacknowledged = members.filter((member) => member.lifecycleState !== 'active_acknowledged');
  if (interlocks.length === 0 && unacknowledged.length === 0) return null;
  const code: OperatorAlarmRefusalCode = interlocks.length > 0
    ? 'operator_alarm_interlock_active'
    : 'unacknowledged_operator_alarms';
  const message = operatorAlarmRefusalMessage(code, {
    unacknowledged_alarms: JSON.stringify(
      unacknowledged.map((member) => ({ condition_id: member.conditionId })),
    ),
  }) ?? code;
  return { code, message, members };
}

/** The exact resources a recovery offer's held operation reserves. */
export function recoveryOfferResourceIds(offer: CycleRecoveryOffer): string[] {
  const resources = new Set<string>();
  for (const participant of offer.participants) {
    if (participant === null || typeof participant !== 'object' || Array.isArray(participant)) continue;
    const ids = (participant as Record<string, unknown>).resource_ids;
    if (!Array.isArray(ids)) continue;
    for (const id of ids) if (typeof id === 'string') resources.add(id);
  }
  return [...resources].sort();
}
