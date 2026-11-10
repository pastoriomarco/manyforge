// Recover and enable (OD-54): the facts the operator confirms, sent back
// unchanged. The backend revalidates them before every step.
import type { RecoveryConfirmation, RecoveryPreflight } from '../types/components';
import { codedErrorDetail } from './componentPresentation';

/**
 * Why Recover and enable can't run from this preflight, one line per row;
 * empty when it can. A blocked or excluded row can't be recovered here; an
 * unknown member is fine only behind its recoverable machine.
 */
export function recoveryBlockers(preflight: RecoveryPreflight): string[] {
  const rows = preflight.components;
  return rows.flatMap((row, index) => {
    if (row.eligibility === 'blocked' || row.eligibility === 'excluded') {
      const detail = row.cause === null ? 'It can\'t be recovered here.' : codedErrorDetail(row.cause);
      return [`${row.component_id}: ${detail}`];
    }
    if (row.eligibility === 'unknown' && !(index === 1 && rows[0]?.eligibility === 'recover')) {
      return [`${row.component_id}: its state is unknown and its machine can't be recovered first.`];
    }
    return [];
  });
}

export function recoveryConfirmation(preflight: RecoveryPreflight): RecoveryConfirmation {
  return {
    snapshot_id: preflight.snapshot_id,
    confirmed: preflight.components.map((row) => ({
      component_id: row.component_id,
      cause_code: row.cause?.code ?? null,
      vendor_code: row.cause?.vendor_code ?? null,
      stop_code: row.stop?.code ?? null,
      stop_vendor_code: row.stop?.vendor_code ?? null,
      eligibility: row.eligibility,
    })),
  };
}

/** The causes in a preflight, as `component_id/cause_code/stop_code` keys. */
function causeKeys(preflight: RecoveryPreflight): string[] {
  return preflight.components.filter((row) => row.eligibility !== 'ready')
    .map((row) => `${row.component_id}/${row.cause?.code ?? ''}/${row.stop?.code ?? ''}`);
}

/**
 * Whether a fresh preflight stays within what the operator confirmed: every
 * cause it still shows was among the confirmed ones (a recovered machine
 * simply drops out). A new or different cause needs a new confirmation.
 */
export function withinConfirmed(fresh: RecoveryPreflight, confirmed: readonly RecoveryPreflight[]): boolean {
  const allowed = new Set(confirmed.flatMap(causeKeys));
  return causeKeys(fresh).every((key) => allowed.has(key));
}
