import { Fragment, type ReactNode } from 'react';
import { programCategoryFamily } from '../styles/tokens';

// UI6-I-VC D3 Program node glyphs. Reference drawings and the fitting rule:
// frontend/design/program-node-design.html. Each category icon is drawn on a
// 16-unit grid; its transform (precomputed from the drawing's bounding box)
// scales and centres it so its longer side fills 12 units, so every icon is
// a square of the same visual size. Stroke width is set in CSS relative to
// the icon size and does not scale with the fit.

interface CategoryIconDrawing {
  transform: string;
  shapes: ReactNode;
}

const CATEGORY_ICONS: Readonly<Record<string, CategoryIconDrawing>> = {
  control_flow: {
    transform: 'translate(-1.000 -0.727) scale(1.0909)',
    shapes: (
      <><path d="M4 2.5v11" /><path d="M4 6.5c0 2.5 2 3.5 4.5 3.5H12" /><path d="M10.5 8 12.5 10l-2 2" /></>
    ),
  },
  recovery: {
    transform: 'translate(-0.600 0.065) scale(1.0749)',
    shapes: (
      <><path d="M13 8a5 5 0 1 1-1.6-3.7" /><path d="M11.8 1.8v2.8H9" /></>
    ),
  },
  motion: {
    transform: 'translate(2.167 1.250) scale(0.8333)',
    shapes: (
      <><circle className="solid" cx="3" cy="14" r="1.3" /><path d="M3 14h6.5a2.8 2.8 0 0 0 0-5.6h-3a2.8 2.8 0 0 1 0-5.6h5.4" /><path d="M10.2 0.9 12.3 2.8 10.2 4.7" /></>
    ),
  },
  gripper: {
    transform: 'translate(-0.276 0.138) scale(1.0345)',
    shapes: (
      <><rect x="2.5" y="1.8" width="11" height="3.6" rx="1" /><path d="M4.6 5.4v5.4l2.1 2.6" /><path d="M11.4 5.4v5.4l-2.1 2.6" /><path d="M4.6 8.2h1.4" /><path d="M11.4 8.2H10" /></>
    ),
  },
  scene: {
    transform: 'translate(0.258 0.258) scale(0.9677)',
    shapes: (
      <><path d="M8 1.8 13.6 5v6L8 14.2 2.4 11V5z" /><path d="M2.4 5 8 8.2 13.6 5" /><path d="M8 8.2v6" /></>
    ),
  },
  model: {
    transform: 'translate(-0.316 -0.526) scale(1.0526)',
    shapes: (
      <><path d="M6 10V2.4" /><path d="M4.4 4 6 2.4 7.6 4" /><path d="M6 10h7.6" /><path d="M12 8.4l1.6 1.6-1.6 1.6" /><path d="M6 10l-3.8 3.8" /><path d="M4.4 13.8H2.2v-2.2" /><circle className="solid" cx="6" cy="10" r="1" /></>
    ),
  },
  geometry: {
    transform: 'translate(-0.727 -1.000) scale(1.0909)',
    shapes: (
      <><path d="M2.5 13.5h11" /><path d="M2.5 13.5 10 3" /><path d="M8 13.5a5.5 5.5 0 0 0-2.3-4.48" /></>
    ),
  },
  logic: {
    transform: 'translate(0.615 0.615) scale(0.9231)',
    shapes: (
      <><rect x="1.5" y="4.5" width="13" height="7" rx="3.5" /><circle className="solid" cx="11" cy="8" r="2" /></>
    ),
  },
  io: {
    transform: 'translate(-0.667 -0.205) scale(1.0256)',
    shapes: (
      <><path d="M2.6 3.6v8.8" /><path d="M8.6 3.2 5.4 12.8" /><ellipse cx="11.9" cy="8" rx="2.4" ry="4.4" /></>
    ),
  },
  simulation: {
    transform: 'translate(0.000 0.000) scale(1.0000)',
    shapes: (
      <><rect x="2" y="3" width="12" height="10" rx="1.5" /><path className="solid" d="M6.6 5.8v4.4L10.4 8z" /></>
    ),
  },
  assistant: {
    transform: 'translate(-0.727 -1.000) scale(1.0909)',
    shapes: (
      <><path d="M2.5 3.5h11v7H7.2l-3 2.5v-2.5H2.5z" /><circle className="solid" cx="5.8" cy="7" r="0.9" /><circle className="solid" cx="8" cy="7" r="0.9" /><circle className="solid" cx="10.2" cy="7" r="0.9" /></>
    ),
  },
};

const UNKNOWN_CATEGORY_ICON: CategoryIconDrawing = {
  transform: 'translate(0 0) scale(1)',
  shapes: <rect x="2" y="2" width="12" height="12" rx="2.5" />,
};

/** Category icon in its family colour (colour set by CSS on the family). */
export function ProgramCategoryIcon({ category }: { category: string }) {
  const drawing = CATEGORY_ICONS[category] ?? UNKNOWN_CATEGORY_ICON;
  return (
    <svg
      className="program-category-icon"
      data-category-family={programCategoryFamily(category)}
      viewBox="0 0 16 16"
      aria-hidden="true"
      focusable="false"
    >
      <g transform={drawing.transform}>{drawing.shapes}</g>
    </svg>
  );
}

/** One drawn kind symbol set for the canvas and the node palette. */
export function ProgramKindSymbol({ kind }: { kind: string }) {
  return (
    <svg className="program-kind-symbol" viewBox="0 0 12 12" aria-hidden="true" focusable="false">
      {kind === 'composite' && <path d="M6 0.6 11.4 6 6 11.4 0.6 6Z" fill="currentColor" />}
      {kind === 'decorator' && (
        <path d="M6 1.4 10.6 6 6 10.6 1.4 6Z" fill="none" stroke="currentColor" strokeWidth="1.8" />
      )}
      {(kind === 'macro' || kind === 'authoring_macro') && (
        <>
          <rect x="1.4" y="1.4" width="9.2" height="9.2" rx="1" fill="none" stroke="currentColor" strokeWidth="1.4" />
          <path d="M1.4 6h9.2M6 1.4v9.2" fill="none" stroke="currentColor" strokeWidth="1.2" />
        </>
      )}
      {kind !== 'composite' && kind !== 'decorator' && kind !== 'macro' && kind !== 'authoring_macro' && (
        <circle cx="6" cy="6" r="4" fill="currentColor" />
      )}
    </svg>
  );
}

/**
 * Preferred line-break points for user names, without changing the text:
 * after "_", before a capital that follows a lowercase letter, and between
 * letters and digits. Returns the segments in order.
 */
export function programNodeNameSegments(name: string): string[] {
  return name
    .replace(/_/g, '_\u0000')
    .replace(/([a-z])([A-Z])/g, '$1\u0000$2')
    .replace(/([A-Za-z])([0-9])/g, '$1\u0000$2')
    .replace(/([0-9])([A-Za-z])/g, '$1\u0000$2')
    .split('\u0000')
    .filter((segment) => segment.length > 0);
}

/** The name with <wbr> at the preferred break points. */
export function ProgramNodeName({ name }: { name: string }) {
  const segments = programNodeNameSegments(name);
  return (
    <>
      {segments.map((segment, index) => (
        <Fragment key={index}>
          {segment}
          {index < segments.length - 1 && <wbr />}
        </Fragment>
      ))}
    </>
  );
}
