import type { ManagerApplyStatus } from '../types/api';
import type { WorkbenchNotice } from './workbenchNotices';

const WORKBENCH_REPOSITORY_ROOT = '/workspace';
const CLOSE_WORKBENCH_COMMAND = './scripts/compose-dev.sh stop --keep-evidence';
const SINGLE_FILE_SCENARIO_SUFFIXES = ['.scenario.yaml', '.scenario.yml'] as const;

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
    return 'The current owner confirms authoring admission is open, so a cold restart is not required.';
  }
  if (authoringAdmissionOpen === false) {
    return 'The current owner still reports authoring admission closed. Do not retry while it remains closed; inspect Runtime/history for the current admission holder. This Apply record alone does not justify a cold restart.';
  }
  return 'Current authoring admission is unavailable, so settlement cannot be confirmed from this Apply record alone. Inspect Runtime/history before retrying; the record alone does not justify a cold restart.';
}

/**
 * A terminal activation failure as the Manager reported it: its message,
 * the phase it failed in, its failure code and whether its effects are
 * known. The notice is an unresolved error; it never claims the applied
 * state, and it is not dismissed as a success.
 */
export function activationFailureNotice(
  id: string,
  applyId: string,
  status: {
    state: Extract<ManagerApplyStatus['state'], 'failed' | 'unknown_effect'>;
    failure: ManagerApplyStatus['failure'];
  },
  savedScenarioPath: string | null,
  authoringAdmissionOpen: boolean | null,
): WorkbenchNotice {
  const failure = status.failure;
  const detail = failure === null
    ? 'Manager reported a terminal failure without detail.'
    : `${failure.message} Failed during ${failure.phase.replaceAll('_', ' ')} (${failure.code}); `
      + (failure.effectDisposition === 'known'
        ? 'its effects are known.'
        : 'its effects are unknown, so admission stays closed until explicit diagnosis.');
  const isKnownPreEffectRefusal = status.state === 'failed'
    && failure !== null
    && failure.phase === 'accepted'
    && failure.effectDisposition === 'known';
  if (isKnownPreEffectRefusal) {
    return {
      id,
      severity: 'error',
      domain: 'activation',
      title: 'Activation failed',
      message: `${detail} Applied state was not confirmed (${applyId}). `
        + 'Manager recorded a known pre-effect refusal. '
        + `${preEffectAdmissionDetail(authoringAdmissionOpen)} Correct the reported cause before trying again.`,
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
      + commands,
  };
}
