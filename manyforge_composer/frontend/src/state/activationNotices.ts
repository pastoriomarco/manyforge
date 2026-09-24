import type { ManagerApplyStatus } from '../types/api';
import type { WorkbenchNotice } from './workbenchNotices';

/**
 * A terminal activation failure as the Manager reported it: its message,
 * the phase it failed in, its failure code and whether its effects are
 * known. The notice is an unresolved error; it never claims the applied
 * state, and it is not dismissed as a success.
 */
export function activationFailureNotice(
  id: string,
  applyId: string,
  status: Pick<ManagerApplyStatus, 'state' | 'failure'>,
): WorkbenchNotice {
  const failure = status.failure;
  const detail = failure === null
    ? 'Manager reported a terminal failure without detail.'
    : `${failure.message} Failed during ${failure.phase.replaceAll('_', ' ')} (${failure.code}); `
      + (failure.effectDisposition === 'known'
        ? 'its effects are known.'
        : 'its effects are unknown, so admission stays closed until explicit diagnosis.');
  return {
    id,
    severity: 'error',
    domain: 'activation',
    title: status.state === 'failed' ? 'Activation failed' : 'Activation outcome unknown',
    message: `${detail} Applied state was not confirmed (${applyId}).`,
  };
}
