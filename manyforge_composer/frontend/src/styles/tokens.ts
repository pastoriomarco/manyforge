// Colours used outside CSS: React Flow edges, node icons and the three.js
// viewport. Values that also exist in tokens.css are listed in
// CSS_SHARED_TOKENS; tokens.test.ts keeps the two files equal.

export type ProgramExecutionStatus = 'idle' | 'running' | 'success' | 'failure';

/** Execution status on Program canvas node borders and the edges into them. */
export const STATUS_COLORS: Record<ProgramExecutionStatus, string> = {
  idle: '#677b87',
  running: '#e2b94e',
  success: '#65c59b',
  failure: '#d86f66',
};

/** Node availability that is shown (executable nodes show no symbol). */
export const AVAILABILITY_COLORS = {
  configuration_required: '#d5a34b',
  unavailable: '#7d8d96',
} as const;

export type ProgramCategoryFamily =
  | 'execution_control'
  | 'motion'
  | 'gripper'
  | 'world'
  | 'logic';

/** UI6-I-VC D3: each catalog category belongs to one of five families. */
export const CATEGORY_FAMILIES: Readonly<Record<string, ProgramCategoryFamily>> = {
  control_flow: 'execution_control',
  recovery: 'execution_control',
  motion: 'motion',
  gripper: 'gripper',
  scene: 'world',
  model: 'world',
  geometry: 'world',
  simulation: 'world',
  logic: 'logic',
  io: 'logic',
  assistant: 'logic',
};

export const CATEGORY_FAMILY_COLORS: Record<ProgramCategoryFamily | 'unknown', string> = {
  execution_control: '#9daab4',
  motion: '#6798c1',
  gripper: '#b07088',
  world: '#9674b4',
  logic: '#b7a78f',
  unknown: '#91a5af',
};

/** Program canvas background grid (React Flow). */
export const CANVAS_COLORS = {
  grid: '#29404d',
} as const;

/**
 * 3D viewport palette. Preview colours follow spec 450: cyan current receipt,
 * amber stale/pending/unvalidated, red rejected target.
 */
export const VIEWPORT_COLORS = {
  background: '#081019',
  hemisphereSky: '#dbeaff',
  hemisphereGround: '#121d27',
  gridCell: '#243341',
  gridSection: '#496071',
  primitive: '#aebdcc',
  collision: '#ffb44a',
  collisionEmissive: '#6f2a12',
  error: '#ef766f',
  objectSelected: '#65d3f2',
  objectSelectedEmissive: '#144c62',
  objectAttached: '#5fa84a',
  objectAttachedEmissive: '#173d16',
  objectDraft: '#e4ac55',
  objectDraftEmissive: '#573411',
  objectComparison: '#43d7ec',
  objectComparisonEmissive: '#0c5060',
  object: '#8c6fe0',
  objectEmissive: '#251a48',
  previewCurrent: '#58ddff',
  previewPending: '#f2bb56',
  previewRejected: '#ff7066',
  previewTargetEmissive: '#62421d',
  previewSolvedEmissive: '#153c49',
  frameTarget: '#43d7ec',
  frameTargetEmissive: '#0c5060',
  pointCloud: '#75d7ff',
  pointCloudVertexTint: '#ffffff',
} as const;

/** CSS custom properties that must hold the same values (tokens.test.ts). */
export const CSS_SHARED_TOKENS: Readonly<Record<string, string>> = {
  '--mf-status-idle': STATUS_COLORS.idle,
  '--mf-status-running': STATUS_COLORS.running,
  '--mf-status-success': STATUS_COLORS.success,
  '--mf-status-failure': STATUS_COLORS.failure,
  '--mf-availability-configuration-required': AVAILABILITY_COLORS.configuration_required,
  '--mf-availability-unavailable': AVAILABILITY_COLORS.unavailable,
  '--mf-category-execution-control': CATEGORY_FAMILY_COLORS.execution_control,
  '--mf-category-motion': CATEGORY_FAMILY_COLORS.motion,
  '--mf-category-gripper': CATEGORY_FAMILY_COLORS.gripper,
  '--mf-category-world': CATEGORY_FAMILY_COLORS.world,
  '--mf-category-logic': CATEGORY_FAMILY_COLORS.logic,
  '--mf-category-unknown': CATEGORY_FAMILY_COLORS.unknown,
};

export function programCategoryFamily(category: string): ProgramCategoryFamily | 'unknown' {
  return CATEGORY_FAMILIES[category] ?? 'unknown';
}
