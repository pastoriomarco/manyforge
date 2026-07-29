import { useEffect } from 'react';
import { create } from 'zustand';

import { api } from '../api/client';
import type { RobotLifecycleStatus } from '../types/api';

interface RobotLifecycleStore {
  status: RobotLifecycleStatus | null;
  loading: boolean;
  commandPending: boolean;
  error: string | null;
  refresh: () => Promise<void>;
  enable: () => Promise<void>;
  disable: () => Promise<void>;
  recovery: (
    action: 'clear-fault' | 'clear-warning' | 'shutdown-controller',
  ) => Promise<void>;
  reconcile: () => Promise<void>;
}

async function command(
  set: (
    update:
      | Partial<RobotLifecycleStore>
      | ((state: RobotLifecycleStore) => Partial<RobotLifecycleStore>),
  ) => void,
  operation: () => Promise<unknown>,
): Promise<void> {
  set({ commandPending: true, error: null });
  try {
    await operation();
  } catch (error: unknown) {
    set({
      error:
        error instanceof Error
          ? error.message
          : 'Robot lifecycle command failed',
    });
  } finally {
    try {
      const status = await api.robotLifecycle.status();
      set({ status });
    } catch {
      // Preserve the command error and the last useful state.
    }
    set({ commandPending: false });
  }
}

export const useRobotLifecycleStore = create<RobotLifecycleStore>((set) => ({
  status: null,
  loading: false,
  commandPending: false,
  error: null,
  refresh: async () => {
    set({ loading: true });
    try {
      const status = await api.robotLifecycle.status();
      set({ status, error: null });
    } catch (error: unknown) {
      set({
        error:
          error instanceof Error
            ? error.message
            : 'Robot lifecycle status unavailable',
      });
    } finally {
      set({ loading: false });
    }
  },
  enable: () => command(set, api.robotLifecycle.enable),
  disable: () => command(set, api.robotLifecycle.disable),
  recovery: (action) =>
    command(set, () => api.robotLifecycle.recovery(action)),
  reconcile: () => command(set, api.robotLifecycle.reconcile),
}));

export function useRobotLifecyclePolling(): void {
  const refresh = useRobotLifecycleStore((state) => state.refresh);
  const pending = useRobotLifecycleStore((state) => state.commandPending);
  useEffect(() => {
    let cancelled = false;
    let timer: number | null = null;
    const tick = async (): Promise<void> => {
      await refresh();
      if (cancelled) return;
      const hidden = document.hidden;
      timer = window.setTimeout(
        () => void tick(),
        hidden ? 5_000 : pending ? 250 : 750,
      );
    };
    void tick();
    return () => {
      cancelled = true;
      if (timer !== null) window.clearTimeout(timer);
    };
  }, [pending, refresh]);
}
