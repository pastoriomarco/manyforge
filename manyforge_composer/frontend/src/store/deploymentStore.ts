import { create } from 'zustand';
import { api } from '../api/client';
import type { FullDeployment } from '../types/api';

interface DeploymentStore {
  deployment: FullDeployment | null;
  appliedDeployment: FullDeployment | null;
  savedDeployment: FullDeployment | null;
  sourcePath: string;
  isDirty: boolean;
  loading: boolean;
  error: string | null;

  loadDeployment: (path: string) => Promise<void>;
  fetchDeployment: () => Promise<boolean>;
  setField: <K extends keyof FullDeployment>(key: K, value: FullDeployment[K]) => void;
  setNestedField: (path: string, value: unknown) => void;
  discardToApplied: () => void;
  setAppliedToDraft: () => void;
  setSavedToDraft: () => void;
  saveDeployment: (path: string) => Promise<void>;
}

export const useDeploymentStore = create<DeploymentStore>((set, get) => ({
  deployment: null,
  appliedDeployment: null,
  savedDeployment: null,
  sourcePath: '',
  isDirty: false,
  loading: false,
  error: null,

  loadDeployment: async (path: string) => {
    set({ loading: true, error: null });
    try {
      await api.deployment.load(path);
      const [full, savedFull] = await Promise.all([
        api.deployment.getFull(),
        api.deployment.getSavedFull().catch(() => null),
      ]);
      set({
        deployment: full,
        appliedDeployment: full,
        savedDeployment: savedFull ?? full,
        sourcePath: full.sourcePath,
        isDirty: false,
        loading: false,
        error: null,
      });
    } catch (e) {
      set({ error: (e as Error).message, loading: false });
    }
  },

  fetchDeployment: async () => {
    set({ loading: true, error: null });
    try {
      const [full, savedFull] = await Promise.all([
        api.deployment.getFull(),
        api.deployment.getSavedFull().catch(() => null),
      ]);
      set({
        deployment: full,
        appliedDeployment: full,
        savedDeployment: savedFull ?? full,
        sourcePath: full.sourcePath,
        isDirty: false,
        loading: false,
        error: null,
      });
      return true;
    } catch {
      set({ loading: false });
      return false;
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

  discardToApplied: () => {
    const appliedDeployment = get().appliedDeployment;
    if (!appliedDeployment) return;
    set({
      deployment: JSON.parse(JSON.stringify(appliedDeployment)) as FullDeployment,
      sourcePath: appliedDeployment.sourcePath,
      isDirty: false,
      error: null,
    });
  },

  setAppliedToDraft: () => {
    const deployment = get().deployment;
    if (!deployment) return;
    set({ appliedDeployment: JSON.parse(JSON.stringify(deployment)) as FullDeployment });
  },

  setSavedToDraft: () => {
    const deployment = get().deployment;
    if (!deployment) return;
    set({
      savedDeployment: JSON.parse(JSON.stringify(deployment)) as FullDeployment,
      isDirty: false,
    });
  },

  saveDeployment: async (path: string) => {
    const d = get().deployment;
    if (!d) return;
    try {
      await api.deployment.saveFull(path, d);
      set({
        savedDeployment: JSON.parse(JSON.stringify(d)) as FullDeployment,
        sourcePath: path,
        isDirty: false,
      });
    } catch (e) {
      set({ error: (e as Error).message });
    }
  },
}));
