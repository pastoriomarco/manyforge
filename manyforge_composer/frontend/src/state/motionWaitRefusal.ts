/**
 * Contract 75: motion admission waits while a global interrupt rule's source
 * is not ready yet (`global_interrupt_source_not_ready`, details {rule_id}).
 * Nothing failed and no alarm is raised: Composer shows it as an amber notice
 * (warning), never as an operational error.
 */

export const GLOBAL_INTERRUPT_SOURCE_NOT_READY = 'global_interrupt_source_not_ready';

const PREFIX = 'Motion waits: ';

/** The readable refusal for this code, or null for any other code. */
export function motionWaitRefusalMessage(
  code: string | null | undefined,
  details?: Readonly<Record<string, unknown>> | null,
): string | null {
  if (code !== GLOBAL_INTERRUPT_SOURCE_NOT_READY) return null;
  const ruleId = typeof details?.rule_id === 'string' && details.rule_id !== '' ? details.rule_id : null;
  return `${PREFIX}${ruleId === null ? 'a global interrupt rule' : `global interrupt rule ${ruleId}`}`
    + ' is waiting for its source. Try again once the source reports.';
}

/** A presented message that is this wait, not a failure. */
export function isMotionWaitRefusal(message: string | null | undefined): boolean {
  return typeof message === 'string' && message.startsWith(PREFIX);
}
