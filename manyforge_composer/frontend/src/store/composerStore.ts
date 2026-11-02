export type NodePresentationStatus =
  | 'idle'
  | 'running'
  | 'held'
  | 'success'
  | 'failure'
  | 'interrupted';
export type EdgeStyle =
  | 'smoothstep'
  | 'default'
  | 'simplebezier'
  | 'straight'
  | 'step';
export type LayoutDirection = 'vertical' | 'horizontal';

/** Link styles, as named in the tree editor toolbar and settings. */
export const EDGE_STYLE_OPTIONS: { value: EdgeStyle; label: string }[] = [
  { value: 'smoothstep', label: 'Orthogonal' },
  { value: 'simplebezier', label: 'Smooth' },
  { value: 'default', label: 'Bezier' },
  { value: 'straight', label: 'Straight' },
  { value: 'step', label: 'Step' },
];
