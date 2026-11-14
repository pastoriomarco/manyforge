import type { ManagerApplyStatus } from '../types/api';
import { errorCatalogRow, errorPresentation } from './commandRefusal';
import type { WorkbenchNotice } from './workbenchNotices';

const SCENARIO_COMPOSITION_REFUSED = 'scenario_composition_refused';

function preEffectAdmissionDetail(authoringAdmissionOpen: boolean | null): string {
  if (authoringAdmissionOpen === true) {
    return 'Authoring is available again; nothing is held.';
  }
  if (authoringAdmissionOpen === false) {
    return 'The failed Apply still holds the cell, so authoring is unavailable. Acknowledge it; if authoring stays unavailable, use New: it clears the failed Apply and empties the cell.';
  }
  return 'Whether the failed Apply still holds the cell is not known yet. Acknowledge re-reads it; if authoring stays unavailable, use New: it clears the failed Apply and empties the cell.';
}

/**
 * What still holds the cell after an ended Apply: `nothing_held` while the
 * authoring admission is open, `new_clears` otherwise (New clears whatever the
 * Apply still holds).
 */
export type FailedApplyHold = 'new_clears' | 'nothing_held';

export function failedApplyHold(authoringAdmissionOpen: boolean | null): FailedApplyHold {
  return authoringAdmissionOpen === true ? 'nothing_held' : 'new_clears';
}

const NEW_CLEARS_APPLY = 'What the Apply changed in the cell is not confirmed. '
  + 'Use New to clear the failed Apply and empty the cell, then Load again.';

/**
 * A terminal activation failure as the Manager reported it: its message,
 * the phase it failed in, its failure code and whether its effects are
 * known. The notice is an unresolved error; it never claims the applied
 * state, and it is not dismissed as a success.
 *
 * A failure before any effect is red with one Acknowledge. While the Manager
 * offers the durable acknowledgement (it still holds the failed Apply's claim
 * or fence), Acknowledge sends it, also again after a proven refusal, whose
 * reason the notice shows. A refusal before the fence (phase `accepted`)
 * never held anything: that is positive evidence, so Acknowledge hides the
 * notice here (`dismiss`). Otherwise Acknowledge only re-reads the status; the
 * notice goes only on positive evidence (acknowledged, or no failed Apply).
 * `unavailable` proves nothing is released. After an attempt whose outcome is
 * unknown nothing is resent; New clears the failed Apply.
 *
 * The way out it names follows `hold` (:func:`failedApplyHold`): New where
 * something is held, nothing where nothing is held.
 */
export function activationFailureNotice(
  id: string,
  /** The Apply's display form (`3f9a/apply4`). */
  apply: string,
  status: {
    state: Extract<ManagerApplyStatus['state'], 'failed' | 'unknown_effect'>;
    failure: ManagerApplyStatus['failure'];
    failureAcknowledgement: ManagerApplyStatus['failureAcknowledgement'];
  },
  authoringAdmissionOpen: boolean | null,
  acknowledgement: {
    busy: boolean;
    error: string | null;
    /** A sent acknowledgement's outcome is unknown (lost reply). */
    outcomeUnknown: boolean;
    run: () => void;
    /** Hide this notice in this browser (a refusal before the fence only). */
    dismiss: () => void;
  } = { busy: false, error: null, outcomeUnknown: false, run: () => {}, dismiss: () => {} },
  hold: FailedApplyHold = 'new_clears',
): WorkbenchNotice | null {
  if (status.state === 'failed'
    && status.failureAcknowledgement === 'acknowledged') return null;
  const failure = status.failure;
  if (status.state === 'failed' && failure?.causeCode === SCENARIO_COMPOSITION_REFUSED) {
    // The runtime refused composing the Scenario: nothing was applied and the
    // runtime keeps running. The producer's message names the cause to fix.
    const component = failure.causeDetails.component_id;
    const guidance = errorCatalogRow(SCENARIO_COMPOSITION_REFUSED)?.[1] ?? '';
    const wayOut = hold === 'new_clears'
      ? 'Use New to clear the failed Apply, then correct the Scenario and Load again.'
      : 'Nothing was changed; correct the Scenario and Load again.';
    return {
      id,
      severity: 'error',
      domain: 'activation',
      title: 'Scenario refused',
      message: `${failure.message}${component === undefined ? '' : ` (component ${component})`}. `
        + `${guidance} ${wayOut} (${apply})`,
    };
  }
  const cause = failure === null ? '' : (() => {
    const shown = errorPresentation({
      code: failure.causeCode, message: '', details: failure.causeDetails,
      producer: failure.producer, origin: null,
    });
    const explanation = shown.detail === null ? shown.text : `${shown.text}: ${shown.detail}`;
    return ` Cause: ${explanation} (${failure.causeCode}; ${failure.producer}).`;
  })();
  const detail = failure === null
    ? 'Manager reported a terminal failure without detail.'
    : `${failure.message} Failed during ${failure.phase.replaceAll('_', ' ')} (${failure.code}); `
      + (failure.effectDisposition === 'known'
        ? 'its effects are known.'
        : 'its effects are unknown.') + cause;
  const isBeforeEffectFailure = status.state === 'failed'
    && failure !== null
    && failure.effectBoundary === 'before_effect';
  const isKnownPreEffectRefusal = status.state === 'failed'
    && failure !== null
    && failure.phase === 'accepted'
    && failure.effectDisposition === 'known';
  const acknowledgementError = acknowledgement.error === null
    ? ''
    : ` Acknowledgement: ${acknowledgement.error}`;
  if (isKnownPreEffectRefusal || isBeforeEffectFailure) {
    // Nothing was applied. Always red with exactly one Acknowledge: the
    // Manager's durable acknowledgement while it offers it; otherwise a status
    // re-read. It is hidden only on positive evidence (see the hook).
    const durable = status.state === 'failed'
      && status.failureAcknowledgement === 'available'
      && !acknowledgement.outcomeUnknown;
    // Refused before the fence: the Manager never held a claim or fence.
    const nothingHeld = !durable && isKnownPreEffectRefusal && !acknowledgement.outcomeUnknown;
    const offerState = acknowledgement.outcomeUnknown
      ? ' The acknowledgement outcome is unknown; Acknowledge re-reads the status.'
      : !durable && isBeforeEffectFailure && !isKnownPreEffectRefusal
        ? ' Manager reports acknowledgement unavailable, which does not prove the Apply hold is released; Acknowledge re-reads the status.'
        : '';
    return {
      id,
      severity: 'error',
      domain: 'activation',
      title: 'Activation failed',
      message: `${detail} Applied state was not confirmed (${apply}). `
        + (isKnownPreEffectRefusal
          ? 'Manager recorded a known pre-effect refusal. '
          : 'Manager reports that this Apply made no configuration, runtime, component, or Scene change. ')
        + `${preEffectAdmissionDetail(authoringAdmissionOpen)} Correct the reported cause before trying again.`
        + offerState
        + acknowledgementError,
      actions: [{
        id: 'acknowledge-apply-failure',
        label: acknowledgement.busy ? 'Acknowledging…' : 'Acknowledge',
        title: durable
          ? 'Release this failed Apply in the Manager. Nothing is applied or retried.'
          : nothingHeld
            ? 'Hide this notice in this browser. The Apply was refused before its fence, so nothing is held.'
            : 'Re-read this Apply\'s status. Nothing is sent; the notice goes once nothing is held.',
        disabled: acknowledgement.busy,
        run: nothingHeld ? acknowledgement.dismiss : acknowledgement.run,
      }],
      busy: acknowledgement.busy,
    };
  }
  return {
    id,
    severity: 'error',
    domain: 'activation',
    title: status.state === 'failed' ? 'Activation failed' : 'Activation outcome unknown',
    message: `${detail} Applied state was not confirmed (${apply}). `
      + (hold === 'new_clears' ? NEW_CLEARS_APPLY : 'Nothing is held; Load again.')
      + acknowledgementError,
  };
}
