import {
  COMPONENT_STATES_UNAVAILABLE,
  componentSummary,
  latchSummary,
  ownerIds,
} from '../state/componentPresentation';
import type { CellState, ComponentState } from '../types/components';
import type {
  ComponentOverviewComponent,
  ComponentOverviewProjection,
} from '../types/headless';

export type RuntimeComponentView = ComponentOverviewComponent;
export type RuntimeStatusView = ComponentOverviewProjection;

export type RuntimeReadiness = 'ready' | 'fault' | 'degraded' | 'offline' | 'waiting';

export interface RuntimeStatusPresentation {
  readiness: RuntimeReadiness;
  accessibleLabel: string;
  attentionCount: number;
  detail: string;
  backendConnected: boolean;
}

function knownReadiness(readiness: string | undefined): RuntimeReadiness {
  if (
    readiness === 'ready'
    || readiness === 'fault'
    || readiness === 'degraded'
    || readiness === 'offline'
    || readiness === 'waiting'
  ) return readiness;
  return 'waiting';
}

/**
 * Derives the stable Deployment-tab signal from observation-only runtime facts.
 * It never changes runtime state and deliberately keeps the visible tab label
 * independent from readiness.
 */
export function runtimeStatusPresentation({
  status,
  backendConnected,
  error = null,
  connectionDetail,
  unacknowledgedAlarms = 0,
  clientErrorCount = 0,
}: {
  status: RuntimeStatusView | null;
  backendConnected: boolean;
  error?: string | null;
  connectionDetail: string;
  unacknowledgedAlarms?: number;
  clientErrorCount?: number;
}): RuntimeStatusPresentation {
  const readiness = knownReadiness(status?.readiness ?? (error ? 'offline' : 'waiting'));
  return {
    readiness,
    accessibleLabel: status?.summaryLabel || (error ? 'Unavailable' : 'Waiting'),
    attentionCount: unacknowledgedAlarms + clientErrorCount,
    detail: status?.userMessage || error || connectionDetail,
    backendConnected,
  };
}

/** The Components tab's colour: Deployment's readiness classes, of the cell's components only. */
export type ComponentsTabReadiness = Extract<RuntimeReadiness, 'ready' | 'degraded' | 'fault' | 'waiting'>;

export interface ComponentsTabStatus {
  readiness: ComponentsTabReadiness;
  /** The worst component and its state, or that all are ready. */
  summary: string;
}

/**
 * 2 for a fault, 1 away from its normal running state (offline, not prepared,
 * a non-owner only prepared, activity unknown), 0 at it: an owner (§16) runs
 * prepared, every other component enabled.
 */
function componentSeverity(state: ComponentState, owner: boolean): 0 | 1 | 2 {
  if (state.lifecycle === 'fault') return 2;
  if (state.activity === 'unknown') return 1;
  if (state.lifecycle === 'enabled' || (owner && state.lifecycle === 'prepared')) return 0;
  return 1;
}

/**
 * The worst state among the cell's components (CONTRACTS §1; deactivated
 * members never reach the cell) and the cell latch (§5): fault for a fault or
 * the latch, degraded for any other component away from its running state.
 */
export function componentsTabStatus({ components, latch }: Pick<CellState, 'components' | 'latch'>): ComponentsTabStatus {
  if (components === null) return { readiness: 'waiting', summary: COMPONENT_STATES_UNAVAILABLE };
  const owners = ownerIds(components);
  let worst: ComponentState | null = null;
  let worstSeverity = 0;
  for (const state of components) {
    const severity = componentSeverity(state, owners.has(state.component_id));
    if (severity > worstSeverity) {
      worst = state;
      worstSeverity = severity;
    }
  }
  const latched = latch?.latched === true;
  const readiness = worstSeverity === 2 || latched ? 'fault' : worstSeverity === 1 ? 'degraded' : 'ready';
  const summary = worst !== null ? `${worst.component_id}: ${componentSummary(worst, components)}`
    : latched ? latchSummary(latch) ?? 'Cell latched'
      : components.length === 0 ? 'No components in the cell' : 'All active components ready';
  return { readiness, summary };
}
