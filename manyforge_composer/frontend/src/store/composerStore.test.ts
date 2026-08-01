import { describe, expect, it } from 'vitest';
import {
  composerViewReducer,
  INITIAL_COMPOSER_VIEW_STATE,
} from './composerStore';

describe('composer view reducer', () => {
  it('retains local graph presentation preferences', () => {
    const horizontal = composerViewReducer(INITIAL_COMPOSER_VIEW_STATE, {
      type: 'set_layout_direction',
      layoutDirection: 'horizontal',
    });
    expect(
      composerViewReducer(horizontal, {
        type: 'set_edge_style',
        edgeStyle: 'smoothstep',
      }),
    ).toEqual({
      nodeStatuses: {},
      edgeStyle: 'smoothstep',
      layoutDirection: 'horizontal',
    });
  });

  it('updates one node without dropping other presentation statuses', () => {
    const initial = {
      ...INITIAL_COMPOSER_VIEW_STATE,
      nodeStatuses: { node_a: 'running' as const },
    };
    expect(
      composerViewReducer(initial, {
        type: 'set_node_status',
        nodeId: 'node_b',
        status: 'success',
      }).nodeStatuses,
    ).toEqual({ node_a: 'running', node_b: 'success' });
  });
});
