import { beforeEach, describe, expect, it, vi } from 'vitest';

import { api } from '../api/client';
import type {
  OperatorAlertAcknowledgeResponse,
  OperatorAlertInbox,
} from '../types/api';
import {
  operatorAttentionBlocksMotion,
  useOperatorAlertStore,
} from './operatorAlertStore';

vi.mock('../api/client', () => ({
  api: {
    execution: {
      alerts: vi.fn(),
      acknowledgeAlert: vi.fn(),
      acknowledgeAllAlerts: vi.fn(),
    },
  },
}));

function inbox(generation: number, ids: string[]): OperatorAlertInbox {
  return {
    schemaVersion: 'manyforge.operator_alert_inbox.v1',
    generation,
    blockingCount: ids.length,
    unacknowledgedAlertIds: ids,
    alerts: ids.slice(0, 3).map((id, index) => ({
      schemaVersion: 'manyforge.operator_alert.v1',
      alertId: id,
      recoveryContextId: `context-${id}`,
      incidentId: `incident-${id}`,
      incidentIds: [`incident-${id}`],
      executionId: index + 1,
      severity: 'ERROR',
      errorCode: `error_${id}`,
      message: `Error ${id}`,
      errorDetails: {},
      firstSeenAtNs: index + 1,
      lastSeenAtNs: index + 1,
      occurrenceCount: 1,
      revision: 1,
    })),
    truncated: ids.length > 3,
    timestampNs: generation * 100,
  };
}

beforeEach(() => {
  vi.clearAllMocks();
  useOperatorAlertStore.setState({
    alerts: [],
    blockingCount: 0,
    unacknowledgedAlertIds: [],
    generation: 0,
    timestampNs: 0,
    truncated: false,
    loaded: false,
    busy: false,
    error: null,
  });
});

describe('operator alert store', () => {
  it('fails closed only for motion-producing controls', () => {
    expect(operatorAttentionBlocksMotion(true, false, null, 0)).toBe(true);
    expect(operatorAttentionBlocksMotion(true, true, 'status unavailable', 0)).toBe(true);
    expect(operatorAttentionBlocksMotion(true, true, null, 1)).toBe(true);
    expect(operatorAttentionBlocksMotion(true, true, null, 0)).toBe(false);
    expect(operatorAttentionBlocksMotion(false, false, 'status unavailable', 3)).toBe(false);
  });

  it('retains the backend total while displaying only the newest three', async () => {
    vi.mocked(api.execution.alerts).mockResolvedValue(
      inbox(4, ['alert-4', 'alert-3', 'alert-2', 'alert-1']),
    );
    await useOperatorAlertStore.getState().refresh();
    const state = useOperatorAlertStore.getState();
    expect(state.blockingCount).toBe(4);
    expect(state.alerts.map((alert) => alert.alertId)).toEqual([
      'alert-4', 'alert-3', 'alert-2',
    ]);
    expect(state.unacknowledgedAlertIds).toHaveLength(4);
    expect(state.truncated).toBe(true);
  });

  it('applies individual acknowledgement response that reveals the next alert', async () => {
    useOperatorAlertStore.setState({ ...inbox(4, ['alert-4', 'alert-3', 'alert-2', 'alert-1']), loaded: true });
    vi.mocked(api.execution.acknowledgeAlert).mockResolvedValue({
      acknowledgedAlertIds: ['alert-4'],
      alreadyAcknowledged: false,
      inbox: inbox(5, ['alert-3', 'alert-2', 'alert-1']),
    });
    await useOperatorAlertStore.getState().acknowledge('alert-4');
    expect(useOperatorAlertStore.getState().alerts.map((alert) => alert.alertId)).toEqual([
      'alert-3', 'alert-2', 'alert-1',
    ]);
  });

  it('binds acknowledge-all to the complete ids and generation snapshot', async () => {
    const current = inbox(7, ['alert-3', 'alert-2', 'alert-1']);
    useOperatorAlertStore.setState({ ...current, loaded: true });
    vi.mocked(api.execution.acknowledgeAllAlerts).mockResolvedValue({
      acknowledgedAlertIds: current.unacknowledgedAlertIds,
      alreadyAcknowledged: false,
      inbox: inbox(8, []),
    });
    await useOperatorAlertStore.getState().acknowledgeAll();
    expect(api.execution.acknowledgeAllAlerts).toHaveBeenCalledWith(
      current.unacknowledgedAlertIds,
      7,
      true,
    );
    expect(useOperatorAlertStore.getState().blockingCount).toBe(0);
  });

  it('accepts a later observation from a restarted process with a reset generation', async () => {
    useOperatorAlertStore.setState({
      ...inbox(9, ['old-process-alert']),
      timestampNs: 900,
      loaded: true,
    });
    vi.mocked(api.execution.alerts).mockResolvedValue({
      ...inbox(0, []),
      timestampNs: 1_000,
    });

    await useOperatorAlertStore.getState().refresh();
    expect(useOperatorAlertStore.getState().generation).toBe(0);
    expect(useOperatorAlertStore.getState().blockingCount).toBe(0);
  });

  it('does not let a delayed acknowledgement overwrite a newer poll', async () => {
    useOperatorAlertStore.setState({
      ...inbox(4, ['alert-4', 'alert-3']),
      loaded: true,
    });
    let resolveAcknowledgement!: (value: OperatorAlertAcknowledgeResponse) => void;
    vi.mocked(api.execution.acknowledgeAlert).mockImplementation(
      () => new Promise((resolve) => { resolveAcknowledgement = resolve; }),
    );

    const acknowledgement = useOperatorAlertStore.getState().acknowledge('alert-4');
    useOperatorAlertStore.setState({
      ...inbox(6, ['newer-alert']),
      loaded: true,
    });
    resolveAcknowledgement({
      acknowledgedAlertIds: ['alert-4'],
      alreadyAcknowledged: false,
      inbox: inbox(5, ['alert-3']),
    });
    await acknowledgement;

    const state = useOperatorAlertStore.getState();
    expect(state.generation).toBe(6);
    expect(state.unacknowledgedAlertIds).toEqual(['newer-alert']);
    expect(state.busy).toBe(false);
  });

  it('refreshes the complete inbox after a stale bulk acknowledgement', async () => {
    useOperatorAlertStore.setState({
      ...inbox(7, ['alert-2', 'alert-1']),
      loaded: true,
    });
    vi.mocked(api.execution.acknowledgeAllAlerts).mockRejectedValue(
      new Error('The alert inbox changed'),
    );
    vi.mocked(api.execution.alerts).mockResolvedValue(
      inbox(8, ['alert-3', 'alert-2', 'alert-1']),
    );

    await expect(useOperatorAlertStore.getState().acknowledgeAll()).rejects.toThrow(
      'The alert inbox changed',
    );
    const state = useOperatorAlertStore.getState();
    expect(state.generation).toBe(8);
    expect(state.blockingCount).toBe(3);
    expect(state.error).toBe('The alert inbox changed');
  });
});
