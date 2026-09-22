import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

describe('DeploymentCompositionPanel Apply contract', () => {
  it('observes Manager state but leaves mutation to the prepared header workflow', () => {
    const source = readFileSync(
      new URL('./DeploymentCompositionPanel.tsx', import.meta.url),
      'utf8',
    );

    expect(source).not.toContain('api.manager.apply(');
    expect(source).not.toContain('api.manager.previewDeploymentCandidate(');
    expect(source).toContain('Deployment changes use the header Apply/Revert workflow.');
    expect(source).toContain('Manager Apply inventory');
    expect(source).toContain('readModel.status.status.apply.records');
    expect(source).not.toContain('completeManagerApply');
    expect(source).not.toContain('api.manager.applyStatus');
    expect(source).not.toContain('setInterval');
    expect(source).not.toContain('localStorage');
    expect(source).not.toContain('sessionStorage');
    expect(source).not.toContain('indexedDB');
    expect(source).not.toContain('api.programRun');
    expect(source).not.toContain('/api/program/runs');
  });
});
