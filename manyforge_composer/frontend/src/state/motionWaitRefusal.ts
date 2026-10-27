/**
 * Contract 75: motion admission waits while a global interrupt rule's source
 * is not ready yet (`global_interrupt_source_not_ready`, details {rule_id}).
 * A gate rule (`on_trip: gate`) refuses motion in its scope while its
 * condition holds (`global_interrupt_gated`, details {rule_id}) and allows it
 * again when the condition clears. Nothing failed and no alarm is raised:
 * Composer shows both as an amber notice (warning), never as an operational
 * error. The owner may carry the reason as the code or as `details.reason`.
 */

export const GLOBAL_INTERRUPT_SOURCE_NOT_READY = 'global_interrupt_source_not_ready';
export const GLOBAL_INTERRUPT_GATED = 'global_interrupt_gated';

const PREFIX = 'Motion waits: ';

/** The readable refusal for this code, or null for any other code. */
export function motionWaitRefusalMessage(
  code: string | null | undefined,
  details?: Readonly<Record<string, unknown>> | null,
): string | null {
  const reason = code === GLOBAL_INTERRUPT_SOURCE_NOT_READY || code === GLOBAL_INTERRUPT_GATED
    ? code : details?.reason;
  if (reason !== GLOBAL_INTERRUPT_SOURCE_NOT_READY && reason !== GLOBAL_INTERRUPT_GATED) return null;
  const ruleId = typeof details?.rule_id === 'string' && details.rule_id !== '' ? details.rule_id : null;
  const rule = ruleId === null ? 'a global interrupt rule' : `global interrupt rule ${ruleId}`;
  return reason === GLOBAL_INTERRUPT_GATED
    ? `${PREFIX}${rule} is active. Motion is allowed again when its condition clears.`
    : `${PREFIX}${rule} is waiting for its source. Try again once the source reports.`;
}

/** A presented message that is this wait, not a failure. */
export function isMotionWaitRefusal(message: string | null | undefined): boolean {
  return typeof message === 'string' && message.startsWith(PREFIX);
}
