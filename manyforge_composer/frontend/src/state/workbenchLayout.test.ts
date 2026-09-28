import { describe, expect, it } from 'vitest';

import {
  INSPECTOR_WIDTH_MAX,
  INSPECTOR_WIDTH_MIN,
  inspectorWidthMax,
} from './workbenchLayout';

describe('centered inspector width', () => {
  it('reaches the maximum on a 1440 px workbench', () => {
    expect(inspectorWidthMax(1440)).toBe(INSPECTOR_WIDTH_MAX);
  });

  it('keeps both side panes usable on a narrow workbench', () => {
    expect(inspectorWidthMax(1100)).toBe(1100 - 2 * 360 - 2 * 8);
  });

  it('never goes below the inspector minimum', () => {
    expect(inspectorWidthMax(800)).toBe(INSPECTOR_WIDTH_MIN);
  });
});
