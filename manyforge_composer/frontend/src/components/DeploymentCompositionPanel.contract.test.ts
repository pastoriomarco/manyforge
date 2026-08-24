import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

describe('DeploymentCompositionPanel Apply contract', () => {
  it('observes one Manager Apply and never starts a Program Run', () => {
    const source = readFileSync(
      new URL('./DeploymentCompositionPanel.tsx', import.meta.url),
      'utf8',
    );

    expect(source).toContain('completeManagerApply');
    expect(source).toContain('Manager accepted Apply');
    expect(source).toContain('remains pending after bounded observation');
    expect(source).toContain('api.manager.apply(exactRequest, signal)');
    expect(source).toContain('api.manager.applyStatus(applyId, applyGeneration, signal)');
    expect(source).not.toContain('api.programRun');
    expect(source).not.toContain('/api/program/runs');
  });
});
