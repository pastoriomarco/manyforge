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
  nodeStatuses: Record<string, NodePresentationStatus>;
  edgeStyle: EdgeStyle;
  layoutDirection: LayoutDirection;
}

export type ComposerViewAction =
  | { type: 'set_node_status'; nodeId: string; status: NodePresentationStatus }
  | { type: 'replace_node_statuses'; statuses: Record<string, NodePresentationStatus> }
  | { type: 'clear_node_statuses' }
  | { type: 'set_edge_style'; edgeStyle: EdgeStyle }
  | { type: 'set_layout_direction'; layoutDirection: LayoutDirection };

export const INITIAL_COMPOSER_VIEW_STATE: ComposerViewState = {
  nodeStatuses: {},
  edgeStyle: 'default',
  layoutDirection: 'vertical',
};

/** Pure presentation state; it has no cycle subscription or runtime authority. */
export function composerViewReducer(
  state: ComposerViewState,
  action: ComposerViewAction,
): ComposerViewState {
  switch (action.type) {
    case 'set_node_status':
      return {
        ...state,
        nodeStatuses: {
          ...state.nodeStatuses,
          [action.nodeId]: action.status,
        },
      };
    case 'replace_node_statuses':
      return { ...state, nodeStatuses: { ...action.statuses } };
    case 'clear_node_statuses':
      return { ...state, nodeStatuses: {} };
    case 'set_edge_style':
      return { ...state, edgeStyle: action.edgeStyle };
    case 'set_layout_direction':
      return { ...state, layoutDirection: action.layoutDirection };
  }
}

export function useComposerViewState() {
  return useReducer(composerViewReducer, INITIAL_COMPOSER_VIEW_STATE);
}
