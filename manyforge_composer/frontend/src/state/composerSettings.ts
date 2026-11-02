import type { EdgeStyle, LayoutDirection } from '../store/composerStore';

/**
 * Installation-level Composer presentation settings (UA7W.11). Presentation
 * only: nothing here reaches the Program, the Deployment or execution.
 * Mirrors `manyforge_composer/backend/composer_settings.py`.
 */
/** How the 3D viewer renders on one kind of browser renderer. */
export interface ViewerGraphicsProfile {
  /** Caps the rendered resolution relative to CSS pixels. */
  maxPixelRatio: number;
  antialias: boolean;
  outlineSmoothing: boolean;
  /** How often live robot poses are redrawn. */
  poseRateHz: number;
  motionSmoothing: boolean;
  cameraDamping: boolean;
}

export type ViewerRendererKind = 'gpu' | 'software';

export interface ComposerSettings {
  viewer: {
    wireframeOverlays: boolean;
    objectOutline: boolean;
    collisionGeometry: boolean;
    pointCloud: boolean;
  };
  /** One profile per renderer kind; each browser uses the one it renders with. */
  viewerGraphics: Record<ViewerRendererKind, ViewerGraphicsProfile>;
  /** `layoutDirection` is the direction the tree editor starts in; the
   * toolbar toggle changes it for the session only. `toolsWidthPx` is the
   * width the Program tools pane opens at; dragging its divider changes it
   * for the session only. */
  treeEditor: {
    linkStyle: EdgeStyle;
    layoutDirection: LayoutDirection;
    toolsWidthPx: number;
  };
  motionPanel: {
    confirmHalt: boolean;
    confirmReset: boolean;
    controlWidthRem: number;
  };
}

export interface ComposerSettingsNotice {
  code: string;
  message: string;
}

export interface ComposerSettingsSnapshot {
  revision: number;
  settings: ComposerSettings;
  profiles: string[];
  /** False when nothing can be saved (no location, or an unreadable file). */
  writable: boolean;
  notice: ComposerSettingsNotice | null;
}

export const MOTION_CONTROL_WIDTH_REM = { min: 6, max: 11 } as const;
/** Program tools pane (Nodes / Params) width limits, in CSS pixels. */
export const PROGRAM_TOOLS_WIDTH_PX = { min: 160, max: 360 } as const;

export const VIEWER_PIXEL_RATIO = { min: 0.5, max: 2, step: 0.25 } as const;
export const VIEWER_POSE_RATE_HZ = { min: 5, max: 60 } as const;

export const DEFAULT_COMPOSER_SETTINGS: ComposerSettings = {
  viewer: {
    wireframeOverlays: false,
    objectOutline: false,
    collisionGeometry: false,
    pointCloud: false,
  },
  viewerGraphics: {
    gpu: {
      maxPixelRatio: 1.5,
      antialias: true,
      outlineSmoothing: true,
      poseRateHz: 30,
      motionSmoothing: false,
      cameraDamping: true,
    },
    software: {
      maxPixelRatio: 1,
      antialias: false,
      outlineSmoothing: false,
      poseRateHz: 10,
      motionSmoothing: false,
      cameraDamping: true,
    },
  },
  treeEditor: {
    linkStyle: 'default',
    layoutDirection: 'vertical',
    toolsWidthPx: 224,
  },
  motionPanel: {
    confirmHalt: true,
    confirmReset: true,
    controlWidthRem: 7,
  },
};

const EDGE_STYLES: readonly EdgeStyle[] = ['smoothstep', 'default', 'simplebezier', 'straight', 'step'];
const LAYOUT_DIRECTIONS: readonly LayoutDirection[] = ['vertical', 'horizontal'];

function record(value: unknown, path: string): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error(`${path} must be an object`);
  }
  return value as Record<string, unknown>;
}

function exactKeys(value: Record<string, unknown>, keys: readonly string[], path: string): void {
  const actual = Object.keys(value).sort();
  const expected = [...keys].sort();
  if (actual.length !== expected.length || actual.some((key, index) => key !== expected[index])) {
    throw new Error(`${path} must have exactly ${expected.join(', ')}`);
  }
}

function bool(value: unknown, path: string): boolean {
  if (typeof value !== 'boolean') throw new Error(`${path} must be a boolean`);
  return value;
}

function oneOf<T extends string>(value: unknown, options: readonly T[], path: string): T {
  if (typeof value !== 'string' || !options.includes(value as T)) {
    throw new Error(`${path} must be one of ${options.join(', ')}`);
  }
  return value as T;
}

const GRAPHICS_KEYS = [
  'maxPixelRatio', 'antialias', 'outlineSmoothing', 'poseRateHz', 'motionSmoothing', 'cameraDamping',
] as const;

function parseGraphicsProfile(raw: unknown, path: string): ViewerGraphicsProfile {
  const profile = record(raw, path);
  exactKeys(profile, GRAPHICS_KEYS, path);
  const ratio = profile.maxPixelRatio;
  if (typeof ratio !== 'number' || !Number.isFinite(ratio)
    || ratio < VIEWER_PIXEL_RATIO.min || ratio > VIEWER_PIXEL_RATIO.max
    || !Number.isInteger(ratio / VIEWER_PIXEL_RATIO.step)) {
    throw new Error(`${path}.maxPixelRatio is out of range`);
  }
  const rate = profile.poseRateHz;
  if (typeof rate !== 'number' || !Number.isInteger(rate)
    || rate < VIEWER_POSE_RATE_HZ.min || rate > VIEWER_POSE_RATE_HZ.max) {
    throw new Error(`${path}.poseRateHz is out of range`);
  }
  return {
    maxPixelRatio: ratio,
    antialias: bool(profile.antialias, `${path}.antialias`),
    outlineSmoothing: bool(profile.outlineSmoothing, `${path}.outlineSmoothing`),
    poseRateHz: rate,
    motionSmoothing: bool(profile.motionSmoothing, `${path}.motionSmoothing`),
    cameraDamping: bool(profile.cameraDamping, `${path}.cameraDamping`),
  };
}

export function parseComposerSettings(raw: unknown, path = 'settings'): ComposerSettings {
  const root = record(raw, path);
  exactKeys(root, ['viewer', 'viewerGraphics', 'treeEditor', 'motionPanel'], path);
  const graphics = record(root.viewerGraphics, `${path}.viewerGraphics`);
  exactKeys(graphics, ['gpu', 'software'], `${path}.viewerGraphics`);
  const viewer = record(root.viewer, `${path}.viewer`);
  exactKeys(viewer, ['wireframeOverlays', 'objectOutline', 'collisionGeometry', 'pointCloud'], `${path}.viewer`);
  const tree = record(root.treeEditor, `${path}.treeEditor`);
  exactKeys(tree, ['linkStyle', 'layoutDirection', 'toolsWidthPx'], `${path}.treeEditor`);
  const toolsWidth = tree.toolsWidthPx;
  if (typeof toolsWidth !== 'number' || !Number.isInteger(toolsWidth)
    || toolsWidth < PROGRAM_TOOLS_WIDTH_PX.min || toolsWidth > PROGRAM_TOOLS_WIDTH_PX.max) {
    throw new Error(`${path}.treeEditor.toolsWidthPx is out of range`);
  }
  const motion = record(root.motionPanel, `${path}.motionPanel`);
  exactKeys(motion, ['confirmHalt', 'confirmReset', 'controlWidthRem'], `${path}.motionPanel`);
  const width = motion.controlWidthRem;
  if (typeof width !== 'number' || !Number.isFinite(width)
    || width < MOTION_CONTROL_WIDTH_REM.min || width > MOTION_CONTROL_WIDTH_REM.max) {
    throw new Error(`${path}.motionPanel.controlWidthRem is out of range`);
  }
  return {
    viewer: {
      wireframeOverlays: bool(viewer.wireframeOverlays, `${path}.viewer.wireframeOverlays`),
      objectOutline: bool(viewer.objectOutline, `${path}.viewer.objectOutline`),
      collisionGeometry: bool(viewer.collisionGeometry, `${path}.viewer.collisionGeometry`),
      pointCloud: bool(viewer.pointCloud, `${path}.viewer.pointCloud`),
    },
    viewerGraphics: {
      gpu: parseGraphicsProfile(graphics.gpu, `${path}.viewerGraphics.gpu`),
      software: parseGraphicsProfile(graphics.software, `${path}.viewerGraphics.software`),
    },
    treeEditor: {
      linkStyle: oneOf(tree.linkStyle, EDGE_STYLES, `${path}.treeEditor.linkStyle`),
      layoutDirection: oneOf(tree.layoutDirection, LAYOUT_DIRECTIONS, `${path}.treeEditor.layoutDirection`),
      toolsWidthPx: toolsWidth,
    },
    motionPanel: {
      confirmHalt: bool(motion.confirmHalt, `${path}.motionPanel.confirmHalt`),
      confirmReset: bool(motion.confirmReset, `${path}.motionPanel.confirmReset`),
      controlWidthRem: width,
    },
  };
}

export function parseComposerSettingsSnapshot(raw: unknown): ComposerSettingsSnapshot {
  const root = record(raw, 'composerSettings');
  exactKeys(root, ['schemaVersion', 'revision', 'settings', 'profiles', 'writable', 'notice'], 'composerSettings');
  if (root.schemaVersion !== 'manyforge.composer_settings.v1') {
    throw new Error('composerSettings.schemaVersion is unsupported');
  }
  if (typeof root.revision !== 'number' || !Number.isSafeInteger(root.revision) || root.revision < 0) {
    throw new Error('composerSettings.revision must be a non-negative integer');
  }
  if (!Array.isArray(root.profiles) || root.profiles.some((name) => typeof name !== 'string')) {
    throw new Error('composerSettings.profiles must be names');
  }
  let notice: ComposerSettingsNotice | null = null;
  if (root.notice !== null) {
    const raw = record(root.notice, 'composerSettings.notice');
    exactKeys(raw, ['code', 'message'], 'composerSettings.notice');
    if (typeof raw.code !== 'string' || typeof raw.message !== 'string') {
      throw new Error('composerSettings.notice must have a code and message');
    }
    notice = { code: raw.code, message: raw.message };
  }
  return {
    revision: root.revision,
    settings: parseComposerSettings(root.settings),
    profiles: root.profiles as string[],
    writable: bool(root.writable, 'composerSettings.writable'),
    notice,
  };
}
