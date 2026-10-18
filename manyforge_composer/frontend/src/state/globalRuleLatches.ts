import type { GlobalRuleRecoveryOffer } from '../types/robotControl';
import type { WorkbenchNotice } from './workbenchNotices';

/**
 * One latched cell-level (global) interrupt rule as the operator recovery
 * offer publishes it (get_stop_fault global_rule_recovery_offers, SX-2206,
 * contracts 78/86). `currentCondition` is null while unknown (a source not
 * fresh), never false.
 */
export interface GlobalRuleLatch {
  ruleId: string;
  /** Contract 86: a lost source latches the rule even when nothing moves. */
  latchCause: 'condition' | 'source_loss';
  currentCondition: boolean | null;
  postLatchDisposition: GlobalRuleRecoveryOffer['postLatchDisposition'];
  /** The latest operator Clear request of this rule, while relevant. */
  clear?: GlobalRuleLatchClearStatus;
}

export interface GlobalRuleLatchClearStatus {
  pending: boolean;
  /** The owner's result or why it is unknown; null while pending. */
  message: string | null;
  /** The send's outcome is unknown: no Clear until the next offer read. */
  outcomeUnknown: boolean;
}

export const GLOBAL_RULE_LATCH_CAUSE_LABELS: Record<GlobalRuleLatch['latchCause'], string> = {
  condition: 'condition held',
  source_loss: 'source lost',
};

/** The notice view of one owner offer. */
export function globalRuleLatchFromOffer(
  offer: GlobalRuleRecoveryOffer,
  clear?: GlobalRuleLatchClearStatus,
): GlobalRuleLatch {
  return {
    ruleId: offer.selector.rule_id,
    latchCause: offer.latchCause,
    currentCondition: offer.currentCondition,
    postLatchDisposition: offer.postLatchDisposition,
    ...(clear === undefined ? {} : { clear }),
  };
}

/** A condition latch the owner releases itself once its condition is false. */
export function globalRuleLatchClearsAutomatically(latch: GlobalRuleLatch): boolean {
  // Source loss is operator-only (contract 86) whatever the rule's disposition.
  return latch.latchCause === 'condition'
    && latch.postLatchDisposition === 'auto_clear_on_signal_release';
}

function conditionText(condition: boolean | null): string {
  if (condition === null) return 'unknown (a source is not fresh)';
  return condition ? 'true (still holds)' : 'false';
}

/**
 * The cell recovery notices of latched global rules. The operator Clear latch
 * action (contract 78) is offered once the source is fresh and the condition
 * false; a condition latch with the automatic disposition has no action, it
 * clears when its condition is false.
 */
export function globalRuleLatchNotices(
  latches: readonly GlobalRuleLatch[],
  onClear?: (latch: GlobalRuleLatch) => void,
): WorkbenchNotice[] {
  return latches.map((latch) => {
    const cause = GLOBAL_RULE_LATCH_CAUSE_LABELS[latch.latchCause];
    const automatic = globalRuleLatchClearsAutomatically(latch);
    const clear = latch.clear;
    const disabledReason = latch.currentCondition === null
      ? 'The rule’s sources are not all fresh, so its condition is unknown.'
      : latch.currentCondition
        ? 'The rule’s condition still holds.'
        : clear?.pending
          ? 'A clear request for this rule is in progress.'
          : clear?.outcomeUnknown
            ? 'The previous clear request’s outcome is unknown; wait for the next rule status.'
            : onClear === undefined ? 'Clearing a latch is not available from this runtime yet.' : undefined;
    return {
      id: `recovery.global-rule.${latch.ruleId}`,
      severity: 'error',
      domain: 'recovery',
      title: latch.latchCause === 'source_loss'
        ? `Global rule ${latch.ruleId} latched: source lost`
        : `Global rule ${latch.ruleId} latched`,
      message: `Cause: ${cause}. Current condition: ${conditionText(latch.currentCondition)}. `
        + 'Motion in the rule’s scope stays blocked until the latch clears; '
        + (latch.latchCause === 'source_loss'
          ? 'a source of the rule stopped reporting fresh readings. '
          : '')
        + (automatic
          ? 'It clears automatically when the condition is false.'
          : 'The operator can clear it once every source is fresh and the condition is false.')
        + ' A Program Reset does not clear it.'
        + (clear?.message ? ` ${clear.message}` : ''),
      busy: clear?.pending === true,
      actions: automatic ? undefined : [{
        id: 'clear-global-rule-latch',
        label: clear?.pending ? 'Clearing latch' : 'Clear latch',
        title: disabledReason ?? 'Clear this rule’s latch. This does not move the robot.',
        disabled: disabledReason !== undefined,
        run: () => { if (disabledReason === undefined) onClear?.(latch); },
      }],
    };
  });
}
