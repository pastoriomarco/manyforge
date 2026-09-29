import { useEffect, useRef, useState } from 'react';

import { usePublishWorkbenchNotices, type WorkbenchNotice } from '../state/workbenchNotices';

import { useComposerSettings } from '../hooks/useComposerSettings';
import type { SettingsSurface } from '../state/settingsNavigation';
import { OverlayPanel } from './OverlayPanel';

const PROFILE_NAME = /^[A-Za-z0-9][A-Za-z0-9 _.-]{0,63}$/;

/**
 * Application-wide Composer settings (UA7W.11): profiles, Restore defaults
 * and the way to each surface's own settings. Everything here is saved by
 * this installation and applies to every browser using it.
 */
export function ComposerSettingsLauncher({
  onOpenSurfaceSettings,
}: {
  onOpenSurfaceSettings: (surface: SettingsSurface) => void;
}) {
  const settings = useComposerSettings();
  const [open, setOpen] = useState(false);
  const button = useRef<HTMLButtonElement>(null);
  const attention = settings.error !== null
    || settings.notice?.code === 'composer_settings_file_unreadable';
  return (
    <>
      <button
        ref={button}
        type="button"
        className="global-settings-button"
        aria-haspopup="dialog"
        aria-expanded={open}
        aria-label={attention ? 'Composer settings; needs attention' : 'Composer settings'}
        title="Composer settings for this installation"
        onClick={() => {
          settings.refresh();
          setOpen(true);
        }}
      >
        <span className="transport-settings-glyph" aria-hidden="true">⚙</span>
        {attention && <span className="global-attention-count" aria-hidden="true">!</span>}
      </button>
      <OverlayPanel
        open={open}
        title="Composer settings"
        titleId="composer-settings-title"
        kind="info"
        onClose={() => setOpen(false)}
        returnFocusRef={button}
      >
        <ComposerSettingsBody
          onOpenSurfaceSettings={(surface) => {
            setOpen(false);
            onOpenSurfaceSettings(surface);
          }}
        />
      </OverlayPanel>
    </>
  );
}

export function ComposerSettingsBody({
  onOpenSurfaceSettings,
}: {
  onOpenSurfaceSettings: (surface: SettingsSurface) => void;
}) {
  const settings = useComposerSettings();
  const [profileName, setProfileName] = useState('');
  const [confirming, setConfirming] = useState<string | null>(null);
  const busy = settings.busy || settings.saving;
  const writable = settings.snapshot?.writable === true;
  const profiles = settings.snapshot?.profiles ?? [];
  const nameValid = PROFILE_NAME.test(profileName);
  const replacing = profiles.includes(profileName);
  const unreadable = settings.notice?.code === 'composer_settings_file_unreadable';
  return (
    <div className="composer-settings">
      <p className="hint">
        Saved by this installation and shared by every browser using it. Settings
        only change how Composer looks and asks; they never change the Program,
        the Deployment or what the robots do.
      </p>

      {settings.notice !== null && (
        <div className="notice severity-warning" role="status">
          <span>{settings.notice.message}</span>
          {settings.sessionOnly && (
            <span> Changes you make now last for this session only.</span>
          )}
          {unreadable && (
            <button type="button" disabled={busy} onClick={settings.resetDamaged}>
              Reset settings (keeps a backup)
            </button>
          )}
        </div>
      )}

      {settings.error !== null && (
        <div className="notice severity-error" role="alert">{settings.error}</div>
      )}

      <section aria-labelledby="composer-settings-profiles">
        <h4 id="composer-settings-profiles">Profiles</h4>
        <p className="hint">
          A profile is a saved copy of all these settings. Loading one changes
          them for this installation.
        </p>
        {profiles.length === 0 ? (
          <p className="muted">No saved profiles.</p>
        ) : (
          <ul className="composer-settings-profiles">
            {profiles.map((name) => (
              <li key={name}>
                <span>{name}</span>
                <button
                  type="button"
                  disabled={!writable || busy}
                  onClick={() => settings.loadProfile(name)}
                >Load</button>
                {confirming === `delete:${name}` ? (
                  <button
                    type="button"
                    className="danger-button"
                    disabled={!writable || busy}
                    onClick={() => {
                      setConfirming(null);
                      settings.deleteProfile(name);
                    }}
                  >Confirm delete</button>
                ) : (
                  <button
                    type="button"
                    disabled={!writable || busy}
                    onClick={() => setConfirming(`delete:${name}`)}
                  >Delete</button>
                )}
              </li>
            ))}
          </ul>
        )}
        <form
          className="composer-settings-save-as"
          onSubmit={(event) => {
            event.preventDefault();
            if (!nameValid || !writable || busy) return;
            settings.saveProfile(profileName);
            setProfileName('');
          }}
        >
          <input
            type="text"
            value={profileName}
            maxLength={64}
            placeholder="Profile name"
            aria-label="New profile name"
            onChange={(event) => setProfileName(event.target.value)}
          />
          <button type="submit" disabled={!nameValid || !writable || busy}>
            {replacing ? 'Replace profile' : 'Save as profile'}
          </button>
        </form>
      </section>

      <section aria-labelledby="composer-settings-defaults">
        <h4 id="composer-settings-defaults">Defaults</h4>
        {confirming === 'defaults' ? (
          <div className="composer-settings-actions">
            <span>Restore every Composer setting to its default? Profiles are kept.</span>
            <button
              type="button"
              disabled={!writable || busy}
              onClick={() => {
                setConfirming(null);
                settings.restoreDefaults();
              }}
            >Restore defaults</button>
            <button type="button" onClick={() => setConfirming(null)}>Cancel</button>
          </div>
        ) : (
          <button
            type="button"
            disabled={!writable || busy}
            onClick={() => setConfirming('defaults')}
          >Restore defaults…</button>
        )}
      </section>

      <section aria-labelledby="composer-settings-surfaces">
        <h4 id="composer-settings-surfaces">Other settings</h4>
        <p className="hint">Each surface keeps its settings next to it, behind its gear.</p>
        <div className="composer-settings-actions">
          <button type="button" onClick={() => onOpenSurfaceSettings('viewer')}>
            3D viewer settings
          </button>
          <button type="button" onClick={() => onOpenSurfaceSettings('program')}>
            Program settings
          </button>
        </div>
      </section>
    </div>
  );
}

/** How long a settings information notice stays in the Activity row. */
export const SETTINGS_INFO_MS = 6000;

const SECTION_LABEL = {
  viewer: 'Display',
  treeEditor: 'Tree editor',
  motionPanel: 'Layout and confirmations',
} as const;

/** True for `duration` ms after `key` changes to a value other than `idle`. */
function useBriefly(key: number, idle: number, duration: number): boolean {
  const [shownKey, setShownKey] = useState<number | null>(null);
  useEffect(() => {
    if (key === idle) return undefined;
    setShownKey(key);
    const timer = setTimeout(() => setShownKey(null), duration);
    return () => clearTimeout(timer);
  }, [duration, idle, key]);
  return shownKey === key && key !== idle;
}

/**
 * Composer settings as workbench notices, so they stay visible without a
 * settings panel. Installation settings are saved as they change and apply
 * to everyone using this Composer installation; each save is reported
 * briefly, never asked about. An unreadable file or a failed save stays
 * until solved.
 */
export function useComposerSettingsNotices(): void {
  const settings = useComposerSettings();
  const saved = useBriefly(settings.lastSave?.sequence ?? 0, 0, SETTINGS_INFO_MS);
  const restoredSequence = settings.restored?.sequence ?? 0;
  const restored = useBriefly(restoredSequence, 0, SETTINGS_INFO_MS);
  const notices: WorkbenchNotice[] = [];
  if (restored && settings.restored !== null) {
    notices.push({
      id: 'composer-settings.restored',
      severity: 'info',
      domain: 'settings',
      title: `${SECTION_LABEL[settings.restored.section]} restored to defaults`,
      message: 'Installation-wide: applies to everyone using this Composer installation.',
      actions: [{ id: 'undo', label: 'Undo', run: settings.undoRestore }],
    });
  } else if (saved && settings.lastSave !== null) {
    notices.push({
      id: 'composer-settings.saved',
      severity: 'info',
      domain: 'settings',
      title: 'Installation settings updated',
      message: settings.lastSave.merged
        ? 'Applies to everyone using this Composer installation. Settings changed elsewhere meanwhile were kept.'
        : 'Applies to everyone using this Composer installation.',
    });
  }
  if (settings.notice?.code === 'composer_settings_file_unreadable') {
    notices.push({
      id: 'composer-settings.unreadable',
      severity: 'warning',
      domain: 'settings',
      title: 'Composer settings file not used',
      message: settings.notice.message,
      actions: [{
        id: 'reset',
        label: 'Reset settings',
        title: 'Start from defaults; the file is kept as a backup.',
        disabled: settings.busy,
        run: settings.resetDamaged,
      }],
    });
  }
  if (settings.error !== null) {
    notices.push({
      id: 'composer-settings.error',
      severity: 'warning',
      domain: 'settings',
      title: 'Composer settings',
      message: settings.error,
    });
  }
  usePublishWorkbenchNotices('composer-settings', notices);
}
