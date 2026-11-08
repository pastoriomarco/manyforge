import { Fragment, type ReactNode } from 'react';
import { programCategoryFamily } from '../styles/tokens';

// Program node glyphs. Reference drawings and the fitting rule:
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
    transform: 'translate(0.615 0.116) scale(0.9231)',
    shapes: (
      <><path d="M1.5 14 5 10.5h9.5L11 14z" /><path className="dashed" d="M2.8 8.2C3.8 3.6 7.6 2.2 10 3.6" /><circle cx="12" cy="5" r="1.9" /></>
    ),
  },
  assistant: {
    transform: 'translate(-0.727 -1.000) scale(1.0909)',
    shapes: (
      <><path d="M2.5 3.5h11v7H7.2l-3 2.5v-2.5H2.5z" /><circle className="solid" cx="5.8" cy="7" r="0.9" /><circle className="solid" cx="8" cy="7" r="0.9" /><circle className="solid" cx="10.2" cy="7" r="0.9" /></>
    ),
  },
};

// Dedicated icons for the control-flow and recovery nodes, keyed by catalog
// id; they take precedence over the category icon and use the same fitting
// rule. Sequence (→) and Parallel (⇉) are the standard behavior-tree symbols;
// a cross means failure (Fallback, Retry). Stop uses a circle; Full Stop
// uses an octagon because it stops the whole cell.
const NODE_ICONS: Readonly<Record<string, CategoryIconDrawing>> = {
  sequence: {
    transform: 'translate(-0.087 -0.348) scale(1.0435)',
    shapes: (
      <><path d="M2 8h11" /><path d="M9 3.5 13.5 8 9 12.5" /></>
    ),
  },
  fallback: {
    transform: 'translate(0.000 -0.150) scale(1.0000)',
    shapes: (
      <><path d="M2 4.5h6" /><path d="M10 2.8l3.4 3.4M13.4 2.8 10 6.2" /><path d="M5 4.5V10a1.5 1.5 0 0 0 1.5 1.5H14" /><path d="M12 9.5l2 2-2 2" /></>
    ),
  },
  parallel: {
    transform: 'translate(0.742 0.258) scale(0.9677)',
    shapes: (
      <><path d="M2 4h11" /><path d="M10.8 1.8 13 4l-2.2 2.2" /><path d="M2 12h11" /><path d="M10.8 9.8 13 12l-2.2 2.2" /></>
    ),
  },
  inverter: {
    transform: 'translate(-0.727 -0.727) scale(1.0909)',
    shapes: (
      <><circle cx="8" cy="8" r="5.5" /><path className="solid" d="M8 2.5a5.5 5.5 0 0 1 0 11z" /></>
    ),
  },
  repeat: {
    transform: 'translate(0.381 0.381) scale(0.9524)',
    shapes: (
      <><path d="M2.5 8.5V6A2.5 2.5 0 0 1 5 3.5h8.3" /><path d="M11.5 1.7 13.3 3.5l-1.8 1.8" /><path d="M13.5 7.5V10a2.5 2.5 0 0 1-2.5 2.5H2.7" /><path d="M4.5 10.7 2.7 12.5l1.8 1.8" /></>
    ),
  },
  retry: {
    transform: 'translate(0.080 0.560) scale(0.9600)',
    shapes: (
      <><path d="M2 2.5l3 3M5 2.5l-3 3" /><path d="M7.5 4H11a3.5 3.5 0 0 1 0 7H4" /><path d="M6 9l-2 2 2 2" /></>
    ),
  },
  timer: {
    transform: 'translate(0.615 0.615) scale(0.9231)',
    shapes: (
      <><circle cx="8" cy="9" r="5.5" /><path d="M8 3.5v-2" /><path d="M6.3 1.5h3.4" /><path d="M8 9l2.4-2.4" /><path d="M12.3 4.7l1-1" /></>
    ),
  },
  'kernel.stop_operation': {
    transform: 'translate(0.000 0.000) scale(1.0000)',
    shapes: (
      <><circle cx="8" cy="8" r="6" /><rect className="solid" x="5.6" y="5.6" width="4.8" height="4.8" rx="0.6" /></>
    ),
  },
  'kernel.full_stop': {
    transform: 'translate(0.000 0.000) scale(1.0000)',
    shapes: (
      <><path d="M5.5 2h5L14 5.5v5L10.5 14h-5L2 10.5v-5z" /><rect className="solid" x="5.6" y="5.6" width="4.8" height="4.8" rx="0.6" /></>
    ),
  },
  'kernel.reset_operation': {
    transform: 'translate(-0.736 0.034) scale(1.0922)',
    shapes: (
      <><path d="M3.2 8a4.8 4.8 0 1 0 1.4-3.4" /><path d="M4.4 1.8v2.9h2.9" /><circle className="solid" cx="8" cy="8" r="1.3" /></>
    ),
  },
  'recovery.apply_offered_action': {
    transform: 'translate(0.000 0.000) scale(1.0000)',
    shapes: (
      <><circle cx="8" cy="8" r="6" /><circle cx="8" cy="8" r="2.6" /><path d="M3.8 3.8l2.4 2.4M12.2 3.8 9.8 6.2M3.8 12.2l2.4-2.4M12.2 12.2 9.8 9.8" /></>
    ),
  },
};

const UNKNOWN_CATEGORY_ICON: CategoryIconDrawing = {
  transform: 'translate(0 0) scale(1)',
  shapes: <rect x="2" y="2" width="12" height="12" rx="2.5" />,
};

/**
 * Node icon in its category family colour (colour set by CSS on the family):
 * the node's own icon when it has one, otherwise its category icon.
 */
export function ProgramCategoryIcon({ category, catalogId }: { category: string; catalogId?: string }) {
  const drawing = (catalogId === undefined ? undefined : NODE_ICONS[catalogId])
    ?? CATEGORY_ICONS[category]
    ?? UNKNOWN_CATEGORY_ICON;
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

const TEMPLATE_ICONS: Readonly<Record<'pick' | 'place', CategoryIconDrawing>> = {
  pick: {
    transform: 'translate(0.131 0.230) scale(0.9836)',
    shapes: (
      <><path d="M2.5 14h11" /><rect x="5" y="9.5" width="6" height="4.5" rx="0.8" /><path d="M8 7.5V1.8" /><path d="M5.8 4 8 1.8 10.2 4" /></>
    ),
  },
  place: {
    transform: 'translate(0.131 0.230) scale(0.9836)',
    shapes: (
      <><path d="M2.5 14h11" /><rect x="5" y="9.5" width="6" height="4.5" rx="0.8" /><path d="M8 1.8v5.7" /><path d="M5.8 5.3 8 7.5l2.2-2.2" /></>
    ),
  },
};

/** Pick/Place template pictograms: an object lifted off / set down on a surface. */
export function ProgramTemplateIcon({ kind }: { kind: 'pick' | 'place' }) {
  const drawing = TEMPLATE_ICONS[kind];
  return (
    <svg
      className="program-category-icon"
      data-category-family="gripper"
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

/** Pause mark of a node whose motion waits for Run (status ``held``). */
export function ProgramHeldBadge() {
  return (
    <span className="bt-node-held-badge" aria-hidden="true">
      <svg viewBox="0 0 10 10">
        <rect x="2" y="1.5" width="2.2" height="7" rx="0.6" />
        <rect x="5.8" y="1.5" width="2.2" height="7" rx="0.6" />
      </svg>
    </span>
  );
}
