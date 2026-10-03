/**
 * Operator guidance for the Kernel's operator-alarm motion-attention refusals
 * (spec 450 §5.2). The Kernel is the enforcement point; Composer only tells
 * the operator which remedy releases the refused Run, Resume or recovery.
 */

export const OPERATOR_ALARM_REFUSAL_CODES = [
  'operator_alarm_interlock_active',
  'unacknowledged_operator_alarms',
  'operator_alarm_state_unavailable',
] as const;

export type OperatorAlarmRefusalCode = typeof OPERATOR_ALARM_REFUSAL_CODES[number];

const GUIDANCE: Readonly<Record<OperatorAlarmRefusalCode, string>> = {
  operator_alarm_interlock_active:
    'Motion is blocked by an active alarm condition. Resolve its cause; acknowledging the alarm does not release it.',
  unacknowledged_operator_alarms:
    'Motion is blocked by unacknowledged alarms. Acknowledge them, then try again.',
  operator_alarm_state_unavailable:
    'Motion is blocked because the alarm state is unavailable. Wait until alarm monitoring recovers, then try again.',
};

export function isOperatorAlarmRefusalCode(
  code: string | null | undefined,
): code is OperatorAlarmRefusalCode {
  return typeof code === 'string'
    && (OPERATOR_ALARM_REFUSAL_CODES as readonly string[]).includes(code);
}

/** Condition ids in one refusal membership detail (a canonical JSON array). */
export function refusalMembershipConditionIds(value: unknown): string[] {
  if (typeof value !== 'string') return [];
  try {
    const rows: unknown = JSON.parse(value);
    if (!Array.isArray(rows)) return [];
    return rows.flatMap((row) => (
      row !== null && typeof row === 'object' && !Array.isArray(row)
        && typeof (row as Record<string, unknown>).condition_id === 'string'
        ? [(row as Record<string, unknown>).condition_id as string]
        : []
    ));
  } catch {
    return [];
  }
}

/**
 * The operator message for an attention refusal, or null for any other code.
 * When an interlock and unacknowledged alarms both match, both remedies apply.
 */
export function operatorAlarmRefusalMessage(
  code: string | null | undefined,
  details?: Readonly<Record<string, unknown>> | null,
): string | null {
  if (!isOperatorAlarmRefusalCode(code)) return null;
  const guidance = GUIDANCE[code];
  if (
    code === 'operator_alarm_interlock_active'
    && refusalMembershipConditionIds(details?.unacknowledged_alarms).length > 0
  ) {
    return `${guidance} Then acknowledge the unacknowledged alarms.`;
  }
  return guidance;
}
