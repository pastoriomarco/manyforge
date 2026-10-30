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
    expect(source).toContain('Deployment changes use the one Apply workflow');
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

  it('keeps Deployment and Components thin clients of the Draft, preview and Apply owners', () => {
    const manager = readFileSync(new URL('./ComponentManager.tsx', import.meta.url), 'utf8');
    const add = readFileSync(new URL('./AddComponentPanel.tsx', import.meta.url), 'utf8');
    const components = readFileSync(new URL('./ComponentsWorkspace.tsx', import.meta.url), 'utf8');

    // Changes previews and applies through the shared Deployment controller.
    expect(manager).toContain('controller.previewDraft()');
    expect(manager).toContain('controller.applyDraft({');
    expect(manager).toContain('expectedDraft:');
    // Add component edits only the Draft; no Manager, CCLC or Apply call.
    expect(add).toContain('controller.addComponents(rows, attachments)');
    for (const text of [manager, add, components]) {
      expect(text).not.toContain('api.manager');
      expect(text).not.toContain('api.cclc');
      expect(text).not.toContain('localStorage');
    }
    expect(add).not.toContain('applyDraft');
    // Components edits the Draft through the shared editors; Apply stays under Deployment.
    expect(components).not.toContain('applyDraft');
    expect(components).not.toContain('previewDraft');
    expect(components).toContain('scope={{ componentId: id }}');
    expect(components).not.toContain('<DeploymentDocumentEditor');
  });

  it('keeps the cell-level settings and role assignment under Deployment', () => {
    const deployment = readFileSync(new URL('./DeploymentCompositionPanel.tsx', import.meta.url), 'utf8');
    expect(deployment).toContain('scope="cell"');
    expect(deployment).toContain('<RoleFeedbackCard');
    expect(deployment).toContain('<DeploymentBindingProjection');
    expect(deployment).toContain('<ComponentManagerChanges');
    // Add component and per-component editing moved to Components.
    expect(deployment).not.toContain('<AddComponentPanel');
    expect(deployment).not.toContain('<ComponentList');
    expect(deployment).not.toContain('api.deploymentWorkspace.componentCatalog');
  });
});
