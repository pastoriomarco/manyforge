import { describe, expect, it } from 'vitest';

import {
  INSPECTOR_WIDTH_MIN,
  workbenchWidthLimits,
} from './workbenchLayout';

describe('workbench width limits', () => {
  it('lets the viewer reach half of a 1440 px workbench at the default inspector width', () => {
    const limits = workbenchWidthLimits(1440, 320);

    expect(limits.viewerWidthMax).toBeGreaterThanOrEqual(720);
  });

  it('lets the viewer reach two thirds of a 1920 px workbench when the inspector is minimized', () => {
    const limits = workbenchWidthLimits(1920, INSPECTOR_WIDTH_MIN);

    expect(limits.viewerWidthMax).toBe(1280);
  });

  it('retains the established viewer and inspector minima', () => {
    const limits = workbenchWidthLimits(1440, 320);

    expect(limits.viewerWidthMin).toBe(420);
    expect(limits.inspectorWidthMax).toBe(480);
  });
});
