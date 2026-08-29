import { useReducer } from 'react';

export type NodePresentationStatus = 'idle' | 'running' | 'success' | 'failure';
export type EdgeStyle =
  | 'smoothstep'
  | 'default'
  | 'simplebezier'
  | 'straight'
  | 'step';
export type LayoutDirection = 'vertical' | 'horizontal';

export interface ComposerViewState {
  edgeStyle: EdgeStyle;
  layoutDirection: LayoutDirection;
}

export type ComposerViewAction =
  | { type: 'set_edge_style'; edgeStyle: EdgeStyle }
  | { type: 'set_layout_direction'; layoutDirection: LayoutDirection };

export const INITIAL_COMPOSER_VIEW_STATE: ComposerViewState = {
  edgeStyle: 'default',
  layoutDirection: 'vertical',
};

/** Pure presentation state; it has no cycle subscription or runtime authority. */
export function composerViewReducer(
  state: ComposerViewState,
  action: ComposerViewAction,
): ComposerViewState {
  switch (action.type) {
    case 'set_edge_style':
      return { ...state, edgeStyle: action.edgeStyle };
    case 'set_layout_direction':
      return { ...state, layoutDirection: action.layoutDirection };
  }
}

export function useComposerViewState() {
  return useReducer(composerViewReducer, INITIAL_COMPOSER_VIEW_STATE);
}
