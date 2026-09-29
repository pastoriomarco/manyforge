import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from 'react';

import { api, composerSettingsConflict } from '../api/client';
import {
  DEFAULT_COMPOSER_SETTINGS,
  type ComposerSettings,
  type ComposerSettingsNotice,
  type ComposerSettingsSnapshot,
} from '../state/composerSettings';

/**
 * The one source of installation Composer settings (UA7W.11). Every control
 * that shows a setting reads and writes it here, so toolbars and settings
 * panels can never disagree. Composer HTTP only: never the Headless control
 * connection, and no polling.
 */
export interface ComposerSettingsController {
  /** What the controls show: pending local edits first, then the saved state. */
  settings: ComposerSettings;
  /** The last saved state from Composer; null until first read (or unreachable). */
  snapshot: ComposerSettingsSnapshot | null;
  notice: ComposerSettingsNotice | null;
  /** Changes apply to this session only and are never saved. */
  sessionOnly: boolean;
  saving: boolean;
  busy: boolean;
  error: string | null;
  /**
   * The last save of installation settings. A change is always saved: when
   * settings changed elsewhere meanwhile, the fields changed here are applied
   * on top of that version, so both are kept and the latest change to a
   * field wins (`merged`).
   */
  lastSave: { sequence: number; merged: boolean } | null;
  /** The last card restored to defaults, until the next restore. */
  restored: RestoredSection | null;
  update: (change: (current: ComposerSettings) => ComposerSettings) => void;
  refresh: () => void;
  /** Restores one settings section (one card) to its defaults. */
  restoreSection: (section: ComposerSettingsSection) => void;
  /** Puts back the values the last restore replaced. */
  undoRestore: () => void;
  restoreDefaults: () => void;
  resetDamaged: () => void;
  saveProfile: (name: string) => void;
  loadProfile: (name: string) => void;
  deleteProfile: (name: string) => void;
}

export type ComposerSettingsSection = keyof ComposerSettings;

export interface RestoredSection {
  section: ComposerSettingsSection;
  previous: ComposerSettings[ComposerSettingsSection];
  /** Increases with every restore, so each one is announced once. */
  sequence: number;
}

interface State {
  snapshot: ComposerSettingsSnapshot | null;
  desired: ComposerSettings | null;
  lastSave: { sequence: number; merged: boolean } | null;
  /** The pending edits were rebased onto a change made elsewhere. */
  mergedPending: boolean;
  restored: RestoredSection | null;
  saving: boolean;
  busy: boolean;
  error: string | null;
}

const INITIAL: State = {
  snapshot: null,
  desired: null,
  lastSave: null,
  mergedPending: false,
  restored: null,
  saving: false,
  busy: false,
  error: null,
};

const ComposerSettingsContext = createContext<ComposerSettingsController | null>(null);

function sameSettings(left: ComposerSettings, right: ComposerSettings): boolean {
  return JSON.stringify(left) === JSON.stringify(right);
}

type SettingsTree = { [key: string]: SettingsTree | boolean | number | string };

function rebaseTree(base: SettingsTree, mine: SettingsTree, theirs: SettingsTree): SettingsTree {
  const merged: SettingsTree = { ...theirs };
  for (const [key, value] of Object.entries(mine)) {
    const before = base[key];
    const current = theirs[key];
    merged[key] = typeof value === 'object' && typeof before === 'object' && typeof current === 'object'
      ? rebaseTree(before, value, current)
      : value !== before ? value : current;
  }
  return merged;
}

/**
 * The values `mine` changed from `base`, applied on top of `theirs`. Settings
 * are nested records of plain values, so a single value is the unit of change.
 */
export function rebaseSettings(
  base: ComposerSettings,
  mine: ComposerSettings,
  theirs: ComposerSettings,
): ComposerSettings {
  return rebaseTree(
    base as unknown as SettingsTree,
    mine as unknown as SettingsTree,
    theirs as unknown as SettingsTree,
  ) as unknown as ComposerSettings;
}

function message(error: unknown): string {
  return error instanceof Error ? error.message : 'Settings request failed.';
}

export function ComposerSettingsProvider({ children }: { children: ReactNode }) {
  const [state, setState] = useState<State>(INITIAL);
  const stateRef = useRef(state);
  const commit = useCallback((next: State) => {
    stateRef.current = next;
    setState(next);
  }, []);
  const patch = useCallback((change: Partial<State>) => {
    commit({ ...stateRef.current, ...change });
  }, [commit]);

  // Saves edits made while a save was in flight, after that save returns.
  const flushRef = useRef<() => void>(() => {});
  const flush = useCallback(() => {
    const current = stateRef.current;
    const { snapshot, desired } = current;
    if (current.saving || current.busy
      || desired === null || snapshot === null || !snapshot.writable) return;
    patch({ saving: true, error: null });
    api.composerSettings.replace(snapshot.revision, desired).then((saved) => {
      const latest = stateRef.current;
      // Keep edits made while this one was in flight; send them next.
      const newer = latest.desired !== null && !sameSettings(latest.desired, desired);
      commit({
        ...latest,
        snapshot: saved,
        desired: newer ? latest.desired : null,
        saving: false,
        lastSave: {
          sequence: (latest.lastSave?.sequence ?? 0) + 1,
          merged: latest.mergedPending,
        },
        mergedPending: false,
      });
      if (newer) flushRef.current();
    }, (error: unknown) => {
      const theirs = composerSettingsConflict(error);
      if (theirs === null) {
        patch({ saving: false, error: `Settings not saved: ${message(error)}` });
        return;
      }
      // Changed elsewhere: keep their changes and apply ours on top.
      const latest = stateRef.current;
      const mine = latest.desired ?? desired;
      const merged = rebaseSettings(snapshot.settings, mine, theirs.settings);
      commit({
        ...latest,
        snapshot: theirs,
        desired: sameSettings(merged, theirs.settings) ? null : merged,
        mergedPending: true,
        saving: false,
      });
      flushRef.current();
    });
  }, [commit, patch]);

  useEffect(() => {
    flushRef.current = flush;
  }, [flush]);

  const refresh = useCallback(() => {
    const current = stateRef.current;
    if (current.saving || current.busy) return;
    api.composerSettings.read().then((snapshot) => {
      const latest = stateRef.current;
      // Pending edits stay; the next save applies them on top.
      commit({ ...latest, snapshot, error: null });
    }, (error: unknown) => {
      patch({ error: `Settings unavailable: ${message(error)}` });
    });
  }, [commit, patch]);

  useEffect(() => {
    refresh();
    const onFocus = () => refresh();
    globalThis.addEventListener?.('focus', onFocus);
    return () => globalThis.removeEventListener?.('focus', onFocus);
  }, [refresh]);

  const update = useCallback((change: (current: ComposerSettings) => ComposerSettings) => {
    const current = stateRef.current;
    const base = current.desired ?? current.snapshot?.settings ?? DEFAULT_COMPOSER_SETTINGS;
    patch({ desired: change(base) });
    flush();
  }, [flush, patch]);

  const restoreSection = useCallback((section: ComposerSettingsSection) => {
    const current = stateRef.current;
    const base = current.desired ?? current.snapshot?.settings ?? DEFAULT_COMPOSER_SETTINGS;
    patch({
      restored: {
        section,
        previous: base[section],
        sequence: (current.restored?.sequence ?? 0) + 1,
      },
    });
    update((settings) => ({ ...settings, [section]: DEFAULT_COMPOSER_SETTINGS[section] }));
  }, [patch, update]);

  const undoRestore = useCallback(() => {
    const restored = stateRef.current.restored;
    if (restored === null) return;
    patch({ restored: null });
    update((settings) => ({ ...settings, [restored.section]: restored.previous }));
  }, [patch, update]);

  const operation = useCallback((
    run: (revision: number) => Promise<ComposerSettingsSnapshot>,
    options: { discardEdits: boolean },
  ) => {
    const current = stateRef.current;
    if (current.snapshot === null || current.saving || current.busy) return;
    patch({ busy: true, error: null });
    run(current.snapshot.revision).then((snapshot) => {
      const latest = stateRef.current;
      commit({
        ...latest,
        snapshot,
        busy: false,
        desired: options.discardEdits ? null : latest.desired,
      });
      flush();
    }, (error: unknown) => {
      const conflict = composerSettingsConflict(error);
      patch({
        busy: false,
        ...(conflict !== null ? { snapshot: conflict } : {}),
        error: conflict !== null
          ? 'Settings changed elsewhere; nothing was changed. Review them and try again.'
          : message(error),
      });
    });
  }, [commit, flush, patch]);

  const controller = useMemo<ComposerSettingsController>(() => ({
    settings: state.desired ?? state.snapshot?.settings ?? DEFAULT_COMPOSER_SETTINGS,
    snapshot: state.snapshot,
    notice: state.snapshot?.notice ?? null,
    sessionOnly: state.snapshot !== null && !state.snapshot.writable,
    saving: state.saving,
    busy: state.busy,
    error: state.error,
    lastSave: state.lastSave,
    restored: state.restored,
    update,
    refresh,
    restoreSection,
    undoRestore,
    restoreDefaults: () => operation(
      (revision) => api.composerSettings.restoreDefaults(revision), { discardEdits: true },
    ),
    resetDamaged: () => operation(
      (revision) => api.composerSettings.resetDamaged(revision), { discardEdits: true },
    ),
    saveProfile: (name) => operation(
      (revision) => api.composerSettings.saveProfile(revision, name), { discardEdits: false },
    ),
    loadProfile: (name) => operation(
      (revision) => api.composerSettings.loadProfile(revision, name), { discardEdits: true },
    ),
    deleteProfile: (name) => operation(
      (revision) => api.composerSettings.deleteProfile(revision, name), { discardEdits: false },
    ),
  }), [operation, refresh, restoreSection, state, undoRestore, update]);

  return (
    <ComposerSettingsContext.Provider value={controller}>
      {children}
    </ComposerSettingsContext.Provider>
  );
}

const noop = () => {};

/**
 * The installation settings. Without a provider (isolated component tests),
 * settings live in local state for that component only and are never saved.
 */
export function useComposerSettings(): ComposerSettingsController {
  const context = useContext(ComposerSettingsContext);
  const [local, setLocal] = useState(DEFAULT_COMPOSER_SETTINGS);
  const update = useCallback(
    (change: (current: ComposerSettings) => ComposerSettings) => setLocal(change),
    [],
  );
  const restoreSection = useCallback(
    (section: ComposerSettingsSection) => setLocal((current) => ({
      ...current,
      [section]: DEFAULT_COMPOSER_SETTINGS[section],
    })),
    [],
  );
  if (context !== null) return context;
  return {
    settings: local,
    snapshot: null,
    notice: null,
    sessionOnly: true,
    saving: false,
    busy: false,
    error: null,
    lastSave: null,
    restored: null,
    update,
    refresh: noop,
    restoreSection,
    undoRestore: noop,
    restoreDefaults: noop,
    resetDamaged: noop,
    saveProfile: noop,
    loadProfile: noop,
    deleteProfile: noop,
  };
}
