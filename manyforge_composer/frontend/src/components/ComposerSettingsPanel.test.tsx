import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { ComposerSettingsController } from '../hooks/useComposerSettings';
import { DEFAULT_COMPOSER_SETTINGS } from '../state/composerSettings';

const controller: { current: ComposerSettingsController } = { current: undefined as never };
vi.mock('../hooks/useComposerSettings', () => ({
  useComposerSettings: () => controller.current,
}));

const { ComposerSettingsBody } = await import('./ComposerSettingsPanel');

function settings(overrides: Partial<ComposerSettingsController> = {}): ComposerSettingsController {
  const noop = () => {};
  return {
    settings: DEFAULT_COMPOSER_SETTINGS,
    snapshot: {
      revision: 3, settings: DEFAULT_COMPOSER_SETTINGS, profiles: ['Desk'], writable: true, notice: null,
    },
    notice: null,
    sessionOnly: false,
    saving: false,
    busy: false,
    error: null,
    lastSave: null,
    restored: null,
    update: noop, refresh: noop, restoreSection: noop, undoRestore: noop, restoreDefaults: noop,
    resetDamaged: noop, saveProfile: noop, loadProfile: noop, deleteProfile: noop,
    ...overrides,
  };
}

const render = () => renderToStaticMarkup(<ComposerSettingsBody onOpenSurfaceSettings={() => {}} />);

afterEach(() => {
  controller.current = settings();
});

describe('Composer settings panel', () => {
  it('lists profiles, Restore defaults and the way to each surface', () => {
    controller.current = settings();
    const markup = render();
    expect(markup).toContain('<span>Desk</span>');
    expect(markup).toMatch(/<button type="button">Load<\/button>/);
    expect(markup).toContain('Restore defaults…');
    expect(markup).toContain('3D viewer settings');
    expect(markup).not.toContain('Tree editor settings');
    expect(markup).toContain('Program settings');
    expect(markup).toContain('they never change the Program');
  });

  it('keeps an unreadable file read-only with an explicit reset that keeps a backup', () => {
    const notice = { code: 'composer_settings_file_unreadable', message: 'The settings file was not used.' };
    controller.current = settings({
      notice,
      sessionOnly: true,
      snapshot: {
        revision: 0, settings: DEFAULT_COMPOSER_SETTINGS, profiles: [], writable: false, notice,
      },
    });
    const markup = render();
    expect(markup).toContain('Reset settings (keeps a backup)');
    expect(markup).toContain('Changes you make now last for this session only.');
    expect(markup).toMatch(/<button type="button" disabled="">Restore defaults…<\/button>/);
  });
});
