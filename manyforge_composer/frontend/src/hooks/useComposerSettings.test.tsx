import { act, useEffect } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { api, ApiError } from '../api/client';
import {
  DEFAULT_COMPOSER_SETTINGS,
  parseComposerSettingsSnapshot,
  type ComposerSettings,
  type ComposerSettingsSnapshot,
} from '../state/composerSettings';
import {
  ComposerSettingsProvider,
  useComposerSettings,
  type ComposerSettingsController,
} from './useComposerSettings';

afterEach(() => {
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

// Minimal React DOM host (see workbenchNotices.hold.test.tsx).
function stubDom() {
  const node = (tag: string): Record<string, unknown> => ({
    nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
    namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
    setAttribute: () => undefined, removeAttribute: () => undefined,
  });
  const document: Record<string, unknown> = {
    nodeType: 9, activeElement: null, defaultView: {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
  };
  document.createElement = node;
  document.documentElement = node('html');
  const window = {
    document, HTMLIFrameElement: class {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    setTimeout: globalThis.setTimeout, clearTimeout: globalThis.clearTimeout,
  };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  return node('div');
}

function snapshot(
  revision: number,
  settings: ComposerSettings = DEFAULT_COMPOSER_SETTINGS,
  writable = true,
): ComposerSettingsSnapshot {
  return { revision, settings, profiles: [], writable, notice: null };
}

function withOutline(settings: ComposerSettings, objectOutline: boolean): ComposerSettings {
  return { ...settings, viewer: { ...settings.viewer, objectOutline } };
}

async function mount() {
  const host = stubDom();
  let controller: ComposerSettingsController | null = null;
  function Probe() {
    const current = useComposerSettings();
    useEffect(() => { controller = current; });
    return null;
  }
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(async () => {
    root.render(<ComposerSettingsProvider><Probe /></ComposerSettingsProvider>);
  });
  return {
    get: () => controller!,
    unmount: () => act(() => root.unmount()),
  };
}

const flushPromises = () => act(async () => { await Promise.resolve(); });

describe('composer settings parser', () => {
  it('accepts exactly the backend shape and rejects unknown or invalid fields', () => {
    const wire = {
      schemaVersion: 'manyforge.composer_settings.v1',
      revision: 2,
      settings: DEFAULT_COMPOSER_SETTINGS,
      profiles: ['Desk'],
      writable: true,
      notice: null,
    };
    expect(parseComposerSettingsSnapshot(wire).profiles).toEqual(['Desk']);
    expect(() => parseComposerSettingsSnapshot({
      ...wire, settings: { ...DEFAULT_COMPOSER_SETTINGS, speedLimit: 1 },
    })).toThrow(/exactly/);
    expect(() => parseComposerSettingsSnapshot({
      ...wire,
      settings: { ...DEFAULT_COMPOSER_SETTINGS, motionPanel: {
        ...DEFAULT_COMPOSER_SETTINGS.motionPanel, controlWidthRem: 40,
      } },
    })).toThrow(/out of range/);
    expect(() => parseComposerSettingsSnapshot({ ...wire, schemaVersion: 'v2' }))
      .toThrow(/unsupported/);
  });
});

describe('composer settings controller', () => {
  it('shows an edit at once and saves it against the revision it started from', async () => {
    vi.spyOn(api.composerSettings, 'read').mockResolvedValue(snapshot(3));
    const replace = vi.spyOn(api.composerSettings, 'replace')
      .mockImplementation(async (revision, settings) => snapshot(revision + 1, settings));
    const view = await mount();
    expect(view.get().snapshot?.revision).toBe(3);

    await act(async () => {
      view.get().update((current) => withOutline(current, true));
    });
    await flushPromises();
    expect(replace).toHaveBeenCalledWith(3, withOutline(DEFAULT_COMPOSER_SETTINGS, true));
    expect(view.get().settings.viewer.objectOutline).toBe(true);
    expect(view.get().snapshot?.revision).toBe(4);
    await view.unmount();
  });

  it('always saves: a change made elsewhere meanwhile is kept and mine is applied on top', async () => {
    const elsewhere = snapshot(5, {
      ...DEFAULT_COMPOSER_SETTINGS,
      treeEditor: { ...DEFAULT_COMPOSER_SETTINGS.treeEditor, linkStyle: 'step' },
    });
    vi.spyOn(api.composerSettings, 'read').mockResolvedValue(snapshot(4));
    const replace = vi.spyOn(api.composerSettings, 'replace').mockRejectedValueOnce(
      new ApiError('stale', 409, {
        detail: {
          code: 'composer_settings_revision_stale',
          message: 'Settings changed elsewhere; nothing was saved.',
          current: { schemaVersion: 'manyforge.composer_settings.v1', ...elsewhere },
        },
      }),
    ).mockImplementation(async (revision, settings) => snapshot(revision + 1, settings));
    const view = await mount();
    await act(async () => {
      view.get().update((current) => withOutline(current, true));
    });
    await flushPromises();
    await flushPromises();
    // Resent against their revision with both changes; no question asked.
    expect(replace).toHaveBeenLastCalledWith(5, withOutline(elsewhere.settings, true));
    expect(view.get().settings.viewer.objectOutline).toBe(true);
    expect(view.get().settings.treeEditor.linkStyle).toBe('step');
    expect(view.get().lastSave).toEqual({ sequence: 1, merged: true });
    await view.unmount();
  });

  it('restores one section to its defaults and can undo it', async () => {
    const customised = {
      ...DEFAULT_COMPOSER_SETTINGS,
      treeEditor: { linkStyle: 'step' as const, layoutDirection: 'horizontal' as const, toolsWidthPx: 300 },
      viewer: { ...DEFAULT_COMPOSER_SETTINGS.viewer, objectOutline: true },
    };
    vi.spyOn(api.composerSettings, 'read').mockResolvedValue(snapshot(1, customised));
    const replace = vi.spyOn(api.composerSettings, 'replace')
      .mockImplementation(async (revision, settings) => snapshot(revision + 1, settings));
    const view = await mount();
    await act(async () => { view.get().restoreSection('treeEditor'); });
    await flushPromises();
    // Only that section changes; the viewer setting stays.
    expect(replace).toHaveBeenLastCalledWith(1, {
      ...customised, treeEditor: DEFAULT_COMPOSER_SETTINGS.treeEditor,
    });
    expect(view.get().restored?.section).toBe('treeEditor');
    await act(async () => { view.get().undoRestore(); });
    await flushPromises();
    expect(replace).toHaveBeenLastCalledWith(2, customised);
    expect(view.get().restored).toBeNull();
    await view.unmount();
  });

  it('applies edits for this session only when settings cannot be saved', async () => {
    vi.spyOn(api.composerSettings, 'read').mockResolvedValue({
      ...snapshot(0, DEFAULT_COMPOSER_SETTINGS, false),
      notice: { code: 'composer_settings_file_unreadable', message: 'kept' },
    });
    const replace = vi.spyOn(api.composerSettings, 'replace');
    const view = await mount();
    await act(async () => { view.get().update((current) => withOutline(current, true)); });
    await flushPromises();
    expect(view.get().sessionOnly).toBe(true);
    expect(view.get().settings.viewer.objectOutline).toBe(true);
    // The unreadable file is never overwritten by a toggle.
    expect(replace).not.toHaveBeenCalled();
    await view.unmount();
  });
});
