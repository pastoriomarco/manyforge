import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

describe('DeploymentCompositionPanel Apply contract', () => {
  it('submits one Manager Apply and observes only Manager projection state', () => {
    const source = readFileSync(
      new URL('./DeploymentCompositionPanel.tsx', import.meta.url),
      'utf8',
    );

    expect(source).toContain('await api.manager.apply(request, controller.signal)');
    expect(source).toContain('applyControllerRef.current !== null');
    expect(source).toContain('Manager Apply inventory');
    expect(source).toContain('readModel.status.status.apply.records');
    expect(source).toContain('never resubmit or');
    expect(source.match(/api\.manager\.apply\(/g)).toHaveLength(1);
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
