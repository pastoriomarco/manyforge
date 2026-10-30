import type { ManagerApplyStatus } from '../types/api';
import { errorCatalogRow, errorPresentation } from './commandRefusal';
import type { WorkbenchNotice } from './workbenchNotices';

const WORKBENCH_REPOSITORY_ROOT = '/workspace';
const CLOSE_WORKBENCH_COMMAND = './scripts/compose-dev.sh stop --keep-evidence';
const SINGLE_FILE_SCENARIO_SUFFIXES = ['.scenario.yaml', '.scenario.yml'] as const;
const SCENARIO_COMPOSITION_REFUSED = 'scenario_composition_refused';

function shellQuote(value: string): string {
  return `'${value.replaceAll("'", `'"'"'`)}'`;
}

function repositoryRelativeScenarioPath(path: string): string | null {
  if (path === WORKBENCH_REPOSITORY_ROOT) return '.';
  const prefix = `${WORKBENCH_REPOSITORY_ROOT}/`;
  if (!path.startsWith(prefix)) return null;
  const relative = path.slice(prefix.length);
  return relative.split('/').some((part) => part === '' || part === '.' || part === '..')
    ? null
    : relative;
}

function isSingleFileScenarioPath(path: string): boolean {
  const lowerPath = path.toLowerCase();
  return SINGLE_FILE_SCENARIO_SUFFIXES.some((suffix) => lowerPath.endsWith(suffix));
}

function reloadUnavailableReason(
  savedScenarioPath: string | null,
  repositoryRelativePath: string | null,
): string | null {
  if (savedScenarioPath === null) {
    return 'No Saved Scenario path is available, so this workbench can only be closed.';
  }
  if (isSingleFileScenarioPath(savedScenarioPath)) {
    return 'The Saved Scenario is a single-file artifact, while this workbench start command accepts Scenario project directories; only Close is available.';
  }
  if (repositoryRelativePath === null) {
    return 'The Saved Scenario is outside the mounted repository, so a new workbench cannot reload it; only Close is available.';
  }
  return null;
}

function preEffectAdmissionDetail(authoringAdmissionOpen: boolean | null): string {
  if (authoringAdmissionOpen === true) {
    return 'The current owner confirms authoring admission is open, so a cold restart is not required. The failed Apply remains recorded; this observation does not confirm acknowledgement.';
  }
  if (authoringAdmissionOpen === false) {
    return 'The current owner still reports authoring admission closed. An admission claim and Kernel fence may remain, and readiness may remain closed. Do not retry while it remains closed; inspect Runtime/history for the current admission holder. This Apply record alone does not justify a cold restart.';
  }
  return 'Current authoring admission is unavailable, so settlement cannot be confirmed from this Apply record alone. An admission claim and Kernel fence may remain, and readiness may remain closed. Inspect Runtime/history before retrying; the record alone does not justify a cold restart.';
}

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
 * unknown nothing is resent: the Manager permits one attempt and keeps it
 * incomplete, and only a cold workbench restart settles it.
 */
export function activationFailureNotice(
  id: string,
  applyId: string,
  status: {
    state: Extract<ManagerApplyStatus['state'], 'failed' | 'unknown_effect'>;
    failure: ManagerApplyStatus['failure'];
    failureAcknowledgement: ManagerApplyStatus['failureAcknowledgement'];
  },
  savedScenarioPath: string | null,
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
): WorkbenchNotice | null {
  if (status.state === 'failed'
    && status.failureAcknowledgement === 'acknowledged') return null;
  const failure = status.failure;
  if (status.state === 'failed' && failure?.causeCode === SCENARIO_COMPOSITION_REFUSED) {
    // The runtime refused composing the Scenario: nothing was applied and the
    // runtime keeps running. The refused Apply keeps authoring closed, so the
    // next Load needs a workbench restart. The producer's message names the
    // cause to fix.
    const component = failure.causeDetails.component_id;
    const guidance = errorCatalogRow(SCENARIO_COMPOSITION_REFUSED)?.[1] ?? '';
    return {
      id,
      severity: 'error',
      domain: 'activation',
      title: 'Scenario refused',
      message: `${failure.message}${component === undefined ? '' : ` (component ${component})`}. `
        + `${guidance} Close and preserve evidence: ${CLOSE_WORKBENCH_COMMAND}, then start the `
        + `workbench again. (${applyId})`,
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
        : 'its effects are unknown, so admission stays closed until explicit diagnosis.') + cause;
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
      ? ' The acknowledgement outcome is unknown. The Manager permits no second attempt, so nothing is resent and Acknowledge only re-reads the status. '
        + 'Nothing in the Manager settles this attempt short of a cold restart: if admission stays closed, close and preserve evidence '
        + `(${CLOSE_WORKBENCH_COMMAND}) and start the workbench again.`
      : !durable && isBeforeEffectFailure && !isKnownPreEffectRefusal
        ? ' Manager reports acknowledgement unavailable, which does not prove the Apply hold is released; Acknowledge re-reads the status.'
        : '';
    return {
      id,
      severity: 'error',
      domain: 'activation',
      title: 'Activation failed',
      message: `${detail} Applied state was not confirmed (${applyId}). `
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
  const relativePath = savedScenarioPath === null
    ? null
    : repositoryRelativeScenarioPath(savedScenarioPath);
  const reloadPath = relativePath !== null && savedScenarioPath !== null
    && !isSingleFileScenarioPath(savedScenarioPath)
    ? relativePath
    : null;
  const reloadAvailability = reloadUnavailableReason(savedScenarioPath, relativePath);
  const commands = reloadPath === null
    ? `Close and preserve evidence: ${CLOSE_WORKBENCH_COMMAND}`
    : `Close and preserve evidence: ${CLOSE_WORKBENCH_COMMAND} `
      + `Reload: ${CLOSE_WORKBENCH_COMMAND} && ./scripts/compose-dev.sh start --project ${shellQuote(reloadPath)}`;
  return {
    id,
    severity: 'error',
    domain: 'activation',
    title: status.state === 'failed' ? 'Activation failed' : 'Activation outcome unknown',
    message: `${detail} Applied state was not confirmed (${applyId}). The runtime remains locked. `
      + `Development workbench recovery must be run from the repository root; unsaved changes will be lost. `
      + 'Evidence is written under .manyforge-dev/evidence/ and its exact path is printed in the terminal. '
      + 'The page disconnects when the workbench stops and may need a refresh after Reload. '
      + 'An invalid Saved Scenario leaves the runtime closed. Execution never resumes automatically. '
      + (reloadAvailability === null ? '' : `${reloadAvailability} `)
      + commands
      + acknowledgementError,
  };
}
