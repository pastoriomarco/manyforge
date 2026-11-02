import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

describe('useManagerApplyStatus observation contract', () => {
  it('observes owner notifications without polling or replaying an effect', () => {
    const source = readFileSync(
      new URL('./useManagerApplyStatus.ts', import.meta.url),
      'utf8',
    );

    expect(source.match(/api\.manager\.applyStatus\(/g)).toHaveLength(1);
    expect(source).toContain('parseManagerApplyAvailability');
    expect(source).not.toContain('snapshotSequence');
    expect(source).toContain('controller.abort()');
    expect(source).not.toContain('setInterval');
    expect(source).not.toContain('setTimeout');
    expect(source).not.toContain('api.manager.apply(');
    expect(source).not.toContain('previewDeploymentCandidate');
  });
});
