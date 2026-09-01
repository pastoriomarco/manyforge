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

function accessibleStatusLabel(readiness: RuntimeReadiness, summaryLabel: string): string {
  if (readiness === 'ready') return 'Ready';
  if (readiness === 'fault') return 'Fault';
  if (readiness === 'degraded') return 'Warning';
  if (readiness === 'offline') return 'Off';
  if (readiness === 'waiting') return 'Waiting';
  return summaryLabel || 'Status unavailable';
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
    accessibleLabel: accessibleStatusLabel(readiness, status?.summaryLabel ?? ''),
    attentionCount: unacknowledgedAlarms + clientErrorCount,
    detail: status?.userMessage || error || connectionDetail,
    backendConnected,
  };
}
