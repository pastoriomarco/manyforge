import { useEffect } from 'react';
import { create } from 'zustand';

import { api } from '../api/client';
import type {
  OperatorAlarm,
  OperatorAlarmAcknowledgementMember,
  OperatorAlarmDomainStatus,
  OperatorAlarmHealth,
  OperatorAlarmSnapshot,
} from '../types/api';

const ATTENTION_STATES = new Set([
  'active_unacknowledged',
  'returned_unacknowledged',
]);

export interface OperatorAlarmAcknowledgementFence {
  generation: number;
  members: OperatorAlarmAcknowledgementMember[];
}

interface OperatorAlarmStore {
  alarms: OperatorAlarm[];
  attentionAlarms: OperatorAlarm[];
  standingAlarms: OperatorAlarm[];
  liveCount: number;
  unacknowledgedCount: number;
  motionAttentionCount: number;
  activeCount: number;
  activeAcknowledgedCount: number;
  unacknowledgedMembership: OperatorAlarmAcknowledgementMember[];
  generation: number;
  stateRevision: number;
  timestampNs: number;
  truncated: boolean;
  reconciled: boolean;
  health: OperatorAlarmHealth;
  domains: Record<string, OperatorAlarmDomainStatus>;
  loaded: boolean;
  busy: boolean;
  error: string | null;
  refresh: () => Promise<void>;
  acknowledge: (
    conditionId: string,
    activationId: string,
    recordRevision: number,
  ) => Promise<void>;
  acknowledgeAll: (fence: OperatorAlarmAcknowledgementFence) => Promise<void>;
}

function snapshotPatch(snapshot: OperatorAlarmSnapshot) {
  return {
    alarms: snapshot.alarms,
    attentionAlarms: snapshot.alarms.filter((alarm) => (
      ATTENTION_STATES.has(alarm.lifecycleState)
    )),
    standingAlarms: snapshot.alarms.filter((alarm) => (
      alarm.lifecycleState === 'active_acknowledged'
    )),
    liveCount: snapshot.liveCount,
    unacknowledgedCount: snapshot.unacknowledgedCount,
    motionAttentionCount: snapshot.motionAttentionCount,
    activeCount: snapshot.activeCount,
    activeAcknowledgedCount: snapshot.activeAcknowledgedCount,
    unacknowledgedMembership: snapshot.unacknowledgedMembership,
    generation: snapshot.generation,
    stateRevision: snapshot.stateRevision,
    timestampNs: snapshot.timestampNs,
    truncated: snapshot.truncated,
    reconciled: snapshot.reconciled,
    health: snapshot.health,
    domains: snapshot.domains,
    loaded: true,
  };
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : 'Operator-alarm state unavailable';
}

export function operatorAttentionBlocksMotion(
  startsMotion: boolean,
  loaded: boolean,
  transportError: string | null,
  reconciled: boolean,
  healthState: OperatorAlarmHealth['state'],
  motionAttentionCount: number,
): boolean {
  return startsMotion && (
    !loaded
    || transportError !== null
    || !reconciled
    || healthState !== 'available'
    || motionAttentionCount > 0
  );
}

function mergeSnapshot(
  state: OperatorAlarmStore,
  snapshot: OperatorAlarmSnapshot,
): Partial<OperatorAlarmStore> {
  // time.monotonic_ns() survives a Composer process restart on the same host,
  // while manager generations intentionally restart. The revision tie-breaker
  // prevents an older in-flight response from replacing a newer projection.
  const stale = snapshot.timestampNs < state.timestampNs
    || (
      snapshot.timestampNs === state.timestampNs
      && snapshot.stateRevision < state.stateRevision
    );
  return stale ? { loaded: true } : snapshotPatch(snapshot);
}

export function acknowledgementFence(
  generation: number,
  members: OperatorAlarmAcknowledgementMember[],
): OperatorAlarmAcknowledgementFence {
  return {
    generation,
    members: members.map((member) => ({ ...member })),
  };
}

function acknowledgementMemberKey(member: OperatorAlarmAcknowledgementMember): string {
  return `${JSON.stringify(member.conditionId)}:${JSON.stringify(member.activationId)}:${member.recordRevision}`;
}

export function acknowledgementFenceMatches(
  fence: OperatorAlarmAcknowledgementFence,
  generation: number,
  members: OperatorAlarmAcknowledgementMember[],
): boolean {
  if (fence.generation !== generation || fence.members.length !== members.length) {
    return false;
  }
  const expected = fence.members.map(acknowledgementMemberKey).sort();
  const current = members.map(acknowledgementMemberKey).sort();
  return expected.every((value, index) => value === current[index]);
}

export function hasNewAlarmActivation(
  previous: string[],
  current: string[],
): boolean {
  const previousSet = new Set(previous);
  return current.some((activation) => !previousSet.has(activation));
}

export const useOperatorAlarmStore = create<OperatorAlarmStore>((set, get) => ({
  alarms: [],
  attentionAlarms: [],
  standingAlarms: [],
  liveCount: 0,
  unacknowledgedCount: 0,
  motionAttentionCount: 0,
  activeCount: 0,
  activeAcknowledgedCount: 0,
  unacknowledgedMembership: [],
  generation: 0,
  stateRevision: 0,
  timestampNs: 0,
  truncated: false,
  reconciled: false,
  health: { state: 'unavailable', details: {} },
  domains: {},
  loaded: false,
  busy: false,
  error: null,
  refresh: async () => {
    try {
      const snapshot = await api.execution.alarms(1000);
      set((state) => ({ ...mergeSnapshot(state, snapshot), error: null }));
    } catch (error) {
      set({ loaded: true, error: errorMessage(error) });
    }
  },
  acknowledge: async (conditionId, activationId, recordRevision) => {
    set({ busy: true, error: null });
    try {
      const response = await api.execution.acknowledgeAlarm(
        conditionId,
        activationId,
        recordRevision,
        true,
      );
      set((state) => ({
        ...mergeSnapshot(state, response.snapshot),
        busy: false,
        error: null,
      }));
    } catch (error) {
      const message = errorMessage(error);
      set({ busy: false, error: message });
      await get().refresh();
      set({ error: message });
      throw error;
    }
  },
  acknowledgeAll: async (fence) => {
    set({ busy: true, error: null });
    try {
      const response = await api.execution.acknowledgeAllAlarms(
        fence.members,
        fence.generation,
        true,
      );
      set((state) => ({
        ...mergeSnapshot(state, response.snapshot),
        busy: false,
        error: null,
      }));
    } catch (error) {
      const message = errorMessage(error);
      set({ busy: false, error: message });
      await get().refresh();
      set({ error: message });
      throw error;
    }
  },
}));

const VISIBLE_POLL_MS = 1000;
const HIDDEN_POLL_MS = 30000;
let pollTimer: number | null = null;
let subscriberCount = 0;

function scheduleAlarmPoll(): void {
  if (subscriberCount === 0) {
    pollTimer = null;
    return;
  }
  const delay = typeof document !== 'undefined' && document.hidden
    ? HIDDEN_POLL_MS
    : VISIBLE_POLL_MS;
  pollTimer = window.setTimeout(() => {
    void useOperatorAlarmStore.getState().refresh();
    scheduleAlarmPoll();
  }, delay);
}

function stopAlarmPoll(): void {
  if (pollTimer !== null) {
    window.clearTimeout(pollTimer);
    pollTimer = null;
  }
}

if (typeof document !== 'undefined') {
  document.addEventListener('visibilitychange', () => {
    if (subscriberCount === 0) return;
    stopAlarmPoll();
    if (!document.hidden) void useOperatorAlarmStore.getState().refresh();
    scheduleAlarmPoll();
  });
}

export function useOperatorAlarmPolling(): void {
  useEffect(() => {
    subscriberCount += 1;
    if (subscriberCount === 1) {
      void useOperatorAlarmStore.getState().refresh();
      scheduleAlarmPoll();
    }
    return () => {
      subscriberCount -= 1;
      if (subscriberCount === 0) stopAlarmPoll();
    };
  }, []);
}
