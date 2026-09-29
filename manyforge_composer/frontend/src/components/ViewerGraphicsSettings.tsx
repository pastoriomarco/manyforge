import type { ReactNode } from 'react';

import { useComposerSettings } from '../hooks/useComposerSettings';
import {
  VIEWER_PIXEL_RATIO,
  VIEWER_POSE_RATE_HZ,
  type ViewerGraphicsProfile,
  type ViewerRendererKind,
} from '../state/composerSettings';
import { SettingsSubheading } from './SettingsScope';

const KIND_LABEL: Record<ViewerRendererKind, string> = {
  gpu: 'GPU',
  software: 'Software',
};

const KIND_DESCRIPTION: Record<ViewerRendererKind, string> = {
  gpu: 'the GPU',
  software: 'software rendering (no GPU acceleration)',
};

const PIXEL_RATIOS = Array.from(
  { length: Math.round((VIEWER_PIXEL_RATIO.max - VIEWER_PIXEL_RATIO.min) / VIEWER_PIXEL_RATIO.step) + 1 },
  (_, index) => VIEWER_PIXEL_RATIO.min + index * VIEWER_PIXEL_RATIO.step,
);
const POSE_RATES = [5, 10, 15, 20, 30, 45, 60].filter(
  (rate) => rate >= VIEWER_POSE_RATE_HZ.min && rate <= VIEWER_POSE_RATE_HZ.max,
);

interface GraphicsRow {
  field: keyof ViewerGraphicsProfile;
  label: string;
  /** What it does, its cost, and a suggestion. */
  help: string;
}

const ROWS: GraphicsRow[] = [
  {
    field: 'maxPixelRatio',
    label: 'Resolution',
    help: 'Maximum rendering resolution, relative to screen pixels; never above the screen\'s own. '
      + 'Higher is sharper, and the cost grows with its square: 2× draws four times the pixels of 1×. '
      + 'The largest effect on speed. Suggested: 1–1.5× with a GPU; 1× with software rendering, '
      + '0.75× for a large window.',
  },
  {
    field: 'antialias',
    label: 'Antialiasing',
    help: 'Smooths jagged edges. Costs GPU memory and fill time, and is heavy with software rendering. '
      + 'Changing it restarts the 3D view for a moment. Suggested: on with a GPU, off with software rendering.',
  },
  {
    field: 'outlineSmoothing',
    label: 'Outline smoothing',
    help: 'Smooths the edges of the Object outline (only while Object outline is on). Adds to the cost '
      + 'of the outline pass. Suggested: on with a GPU, off with software rendering.',
  },
  {
    field: 'poseRateHz',
    label: 'Robot update rate',
    help: 'How often live robot poses are redrawn. Higher is smoother live motion; each update redraws '
      + 'the view. It changes only the display, never the robot\'s motion. Suggested: 30 Hz with a GPU, '
      + '10–15 Hz with software rendering.',
  },
  {
    field: 'motionSmoothing',
    label: 'Motion smoothing',
    help: 'Interpolates briefly between robot pose updates so live motion looks continuous. Adds frames '
      + 'between updates. Suggested: on with a GPU, off with software rendering.',
  },
  {
    field: 'cameraDamping',
    label: 'Camera damping',
    help: 'The camera glides to a stop after you rotate or pan, which keeps rendering for a moment '
      + 'after you let go. Suggested: on; off if the view feels sluggish with software rendering.',
  },
];

/**
 * Installation graphics settings: one column per renderer kind. Every browser
 * uses the column matching how it renders, and only that one is editable here.
 */
export function ViewerGraphicsSettings({ rendererKind }: { rendererKind: ViewerRendererKind | null }) {
  const composerSettings = useComposerSettings();
  const graphics = composerSettings.settings.viewerGraphics;
  const update = <K extends keyof ViewerGraphicsProfile>(
    kind: ViewerRendererKind,
    field: K,
    value: ViewerGraphicsProfile[K],
  ) => composerSettings.update((current) => ({
    ...current,
    viewerGraphics: {
      ...current.viewerGraphics,
      [kind]: { ...current.viewerGraphics[kind], [field]: value },
    },
  }));
  const control = (kind: ViewerRendererKind, row: GraphicsRow): ReactNode => {
    const profile = graphics[kind];
    const enabled = kind === rendererKind;
    const label = `${KIND_LABEL[kind]} ${row.label.toLowerCase()}`;
    const title = enabled
      ? undefined
      : rendererKind === null
        ? 'Available once the 3D view has started and detected how this browser renders.'
        : `Used by browsers that render with ${KIND_DESCRIPTION[kind]}; this browser uses ${KIND_DESCRIPTION[rendererKind]}.`;
    switch (row.field) {
      case 'maxPixelRatio':
        return (
          <select
            aria-label={label}
            title={title}
            disabled={!enabled}
            value={profile.maxPixelRatio}
            onChange={(event) => update(kind, 'maxPixelRatio', Number(event.target.value))}
          >
            {PIXEL_RATIOS.map((ratio) => <option key={ratio} value={ratio}>{ratio}×</option>)}
          </select>
        );
      case 'poseRateHz': {
        const rates = POSE_RATES.includes(profile.poseRateHz)
          ? POSE_RATES
          : [...POSE_RATES, profile.poseRateHz].sort((left, right) => left - right);
        return (
          <select
            aria-label={label}
            title={title}
            disabled={!enabled}
            value={profile.poseRateHz}
            onChange={(event) => update(kind, 'poseRateHz', Number(event.target.value))}
          >
            {rates.map((rate) => <option key={rate} value={rate}>{rate} Hz</option>)}
          </select>
        );
      }
      default: {
        const field = row.field;
        return (
          <input
            type="checkbox"
            aria-label={label}
            title={title}
            disabled={!enabled}
            checked={profile[field]}
            onChange={(event) => update(kind, field, event.target.checked)}
          />
        );
      }
    }
  };
  return (
    <section className="dcard" aria-label="Graphics">
      <SettingsSubheading
        title="Graphics"
        scope="installation"
        onRestoreDefaults={() => composerSettings.restoreSection('viewerGraphics')}
      />
      <p>
        {rendererKind === null
          ? 'How this browser renders is detected when the 3D view starts.'
          : `This browser renders with ${KIND_DESCRIPTION[rendererKind]}, so its column applies here. `
            + 'The other column is used by browsers that render the other way.'}
        {' '}Point at a setting for what it does and what it costs.
      </p>
      <table className="viewer-graphics-table">
        <thead>
          <tr>
            <th scope="col"><span className="sr-only">Setting</span></th>
            {(['gpu', 'software'] as const).map((kind) => (
              <th scope="col" key={kind} className={kind === rendererKind ? 'current' : undefined}>
                {KIND_LABEL[kind]}
                {kind === rendererKind && <small>this browser</small>}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {ROWS.map((row) => (
            <tr key={row.field}>
              <th scope="row" title={row.help}>{row.label}</th>
              <td>{control('gpu', row)}</td>
              <td>{control('software', row)}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </section>
  );
}
