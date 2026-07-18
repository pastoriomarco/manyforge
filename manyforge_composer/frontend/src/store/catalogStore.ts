import { create } from 'zustand';
import { api } from '../api/client';
import type { CatalogEntry, NodeCatalogSource } from '../types/api';

interface CatalogStore {
  entries: CatalogEntry[];
  version: string;
  effectiveCatalogHash: string;
  generation: number;
  sources: NodeCatalogSource[];
  loading: boolean;
  error: string | null;
  fetchCatalog: () => Promise<void>;
  getByCategory: () => Record<string, CatalogEntry[]>;
}

export const useCatalogStore = create<CatalogStore>((set, get) => ({
  entries: [],
  version: '',
  effectiveCatalogHash: '',
  generation: 0,
  sources: [],
  loading: false,
  error: null,

  fetchCatalog: async () => {
    set({ loading: true, error: null });
    try {
      const data = await api.catalog.list();
      set({
        entries: data.entries,
        version: data.version,
        effectiveCatalogHash: data.effectiveCatalogHash,
        generation: data.generation,
        sources: data.sources,
        loading: false,
      });
    } catch (e) {
      set({ error: (e as Error).message, loading: false });
    }
  },

  getByCategory: () => {
    const grouped: Record<string, CatalogEntry[]> = {};
    for (const entry of get().entries) {
      const cat = entry.category || 'Other';
      if (!grouped[cat]) grouped[cat] = [];
      grouped[cat].push(entry);
    }
    return grouped;
  },
}));
