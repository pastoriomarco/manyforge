import { create } from 'zustand';
import { api } from '../api/client';
import type { FullDeployment } from '../types/api';

interface DeploymentStore {
  deployment: FullDeployment | null;
  isDirty: boolean;
  loading: boolean;
  error: string | null;

  loadDeployment: (path: string) => Promise<void>;
  setField: <K extends keyof FullDeployment>(key: K, value: FullDeployment[K]) => void;
  setNestedField: (path: string, value: unknown) => void;
  saveDeployment: (path: string) => Promise<void>;
}

export const useDeploymentStore = create<DeploymentStore>((set, get) => ({
  deployment: null,
  isDirty: false,
  loading: false,
  error: null,

  loadDeployment: async (path: string) => {
    set({ loading: true, error: null });
    try {
      const info = await api.deployment.load(path);
      // Build a FullDeployment from the load response — the load endpoint
      // returns DeploymentInfo (subset). We store what we get and mark empty
      // sections so the form can still render.
      const full: FullDeployment = {
        apiVersion: 'manyforge.deployment.v0',
        schemaVersion: '',
        name: info.name,
        description: info.description,
        programProfile: info.programProfile,
        robotConfig: info.robotConfig,
        adapters: {},
        orchestrator: { engine: '', config: {} },
        launch: { runner: { module: '', args: [] } },
      };
      set({ deployment: full, isDirty: false, loading: false });
    } catch (e) {
      set({ error: (e as Error).message, loading: false });
    }
  },

  setField: (key, value) => {
    const d = get().deployment;
    if (!d) return;
    set({ deployment: { ...d, [key]: value }, isDirty: true });
  },

  setNestedField: (path: string, value: unknown) => {
    const d = get().deployment;
    if (!d) return;
    const clone = JSON.parse(JSON.stringify(d)) as Record<string, unknown>;
    const parts = path.split('.');
    let obj: Record<string, unknown> = clone;
    for (let i = 0; i < parts.length - 1; i++) {
      obj = obj[parts[i]] as Record<string, unknown>;
      if (!obj) return;
    }
    obj[parts[parts.length - 1]] = value;
    set({ deployment: clone as unknown as FullDeployment, isDirty: true });
  },

  saveDeployment: async (path: string) => {
    const d = get().deployment;
    if (!d) return;
    try {
      await api.deployment.update(d);
      await api.deployment.save(path);
      set({ isDirty: false });
    } catch (e) {
      set({ error: (e as Error).message });
    }
  },
}));
