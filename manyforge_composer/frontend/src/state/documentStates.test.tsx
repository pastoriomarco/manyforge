import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import { DocumentStateLetters } from '../components/DocumentStateLetters';
import type { DeploymentWorkspaceResponse, ProgramWorkspaceResponse } from '../types/api';
import {
  deploymentDocumentStates,
  deploymentUnapplied,
  programDocumentStates,
} from './documentStates';

function program(overrides: {
  unsaved?: boolean; savedMatches?: boolean; appliedMatches?: boolean;
} = {}): ProgramWorkspaceResponse {
  return {
    draft: { authoring: {}, program: {}, diagnostics: [], programRevision: 'r1' },
    saved: { available: true, matchesDraft: overrides.savedMatches ?? true, path: 'cell/program.yaml' },
    applied: { available: true, matchesDraft: overrides.appliedMatches ?? true, unavailableReason: null },
    hasUnsavedChanges: overrides.unsaved ?? false,
  } as unknown as ProgramWorkspaceResponse;
}

function deployment(draftRevision: string, applied: string, differsFromSaved = false): DeploymentWorkspaceResponse {
  return {
    draft: { document: {}, deploymentRevision: draftRevision, differsFromSaved },
    saved: { path: 'cell/deployment.yaml', deploymentRevision: 'x' },
    managerApplied: { activationOrder: null, deploymentRevision: applied, scenarioId: 'cell' },
    // The backend compares the documents; revisions may be absent.
    runAdmission: { ready: draftRevision === applied, reason: null },
  } as unknown as DeploymentWorkspaceResponse;
}

describe('document state letters', () => {
  it('hide while everything matches and show all three when anything differs', () => {
    const clean = programDocumentStates(program());
    expect(clean.pending).toBe(false);
    expect(renderToStaticMarkup(
      <DocumentStateLetters states={clean} owner="Program" side="left" />,
    )).toBe('');

    // Only Applied differs: all three show, each in its own colour.
    const notApplied = programDocumentStates(program({ appliedMatches: false }));
    expect(notApplied.pending).toBe(true);
    expect(notApplied.letters.map((letter) => letter.state)).toEqual(['draft', 'saved', 'different']);
    const markup = renderToStaticMarkup(
      <DocumentStateLetters states={notApplied} owner="Program" side="left" />,
    );
    expect(markup.match(/document-state-letter /g)).toHaveLength(3);
    expect(markup).toContain('state-draft');
    expect(markup).toContain('Scene edits included');
    expect(markup).toContain('Execute is blocked until then.');
  });

  it('reads the Deployment Draft against the Manager-applied revision', () => {
    expect(deploymentUnapplied(deployment('r1', 'r1'))).toBe(false);
    expect(deploymentUnapplied(deployment('r2', 'r1'))).toBe(true);
    expect(deploymentDocumentStates(deployment('r1', 'r1')).pending).toBe(false);
    const edited = deploymentDocumentStates(deployment('r2', 'r1', true));
    expect(edited.letters.map((letter) => letter.state)).toEqual(['modified', 'different', 'different']);
    expect(edited.letters[1].title).toContain('cell/deployment.yaml');
    // Scenario Deployments without a revision field still match by document.
    const unrevisioned = {
      ...deployment('r1', 'r1'),
      draft: { document: {}, deploymentRevision: null, differsFromSaved: false },
    } as unknown as DeploymentWorkspaceResponse;
    expect(deploymentUnapplied(unrevisioned)).toBe(false);
    expect(deploymentDocumentStates(unrevisioned).pending).toBe(false);
  });
});
