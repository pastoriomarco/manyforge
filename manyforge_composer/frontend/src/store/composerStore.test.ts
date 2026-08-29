import { describe, expect, it } from 'vitest';
import {
  composerViewReducer,
  INITIAL_COMPOSER_VIEW_STATE,
} from './composerStore';

describe('composer view reducer', () => {
  it('starts with vertically stacked node bands', () => {
    expect(INITIAL_COMPOSER_VIEW_STATE.layoutDirection).toBe('vertical');
  });

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
      edgeStyle: 'smoothstep',
      layoutDirection: 'horizontal',
    });
  });
});
