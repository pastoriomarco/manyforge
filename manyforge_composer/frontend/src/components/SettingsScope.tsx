/**
 * Where a setting is saved, with one vocabulary everywhere in Composer:
 * a small tag on the heading or row it applies to.
 */
export type SettingsScopeName = 'installation' | 'program' | 'session';

const SCOPES: Record<SettingsScopeName, { label: string; title: string }> = {
  installation: {
    label: 'Installation',
    title: 'Saved for this Composer installation: every browser, kept across restarts.',
  },
  program: {
    label: 'Program',
    title: 'Saved with the Program: a change edits the Draft; Save writes it to the Scenario and Apply makes it part of the running Program.',
  },
  session: {
    label: 'Session',
    title: 'Only until this page is reloaded.',
  },
};

export function SettingsScope({ scope }: { scope: SettingsScopeName }) {
  const { label, title } = SCOPES[scope];
  return <span className="settings-scope" data-scope={scope} title={title}>{label}</span>;
}

/** Under a Program setting while the Draft differs from the Applied Program. */
export function PendingApplyNote() {
  return <span className="settings-pending-apply">Changed in the Draft, not applied yet.</span>;
}

/**
 * A heading inside a settings section. Settings are grouped so that each
 * heading has one scope, and the tag only ever sits on a heading.
 */
export function SettingsSubheading({
  title,
  scope,
  minor = false,
  onRestoreDefaults,
}: {
  title: string;
  scope?: SettingsScopeName;
  minor?: boolean;
  /** Installation settings: restore this heading's settings to defaults. */
  onRestoreDefaults?: () => void;
}) {
  return (
    <div className={`settings-subheading${minor ? ' minor' : ''}`}>
      <span>{title}</span>
      {scope !== undefined && <SettingsScope scope={scope} />}
      {onRestoreDefaults !== undefined && (
        <button
          type="button"
          className="settings-restore-defaults"
          title={`Restore the ${title} settings to their defaults for this installation.`}
          onClick={onRestoreDefaults}
        >Defaults</button>
      )}
    </div>
  );
}
