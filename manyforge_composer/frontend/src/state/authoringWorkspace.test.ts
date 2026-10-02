import { describe, expect, it } from 'vitest';

import {
  authoringUnavailableCause,
  authoringUnavailableReason,
  programApplyScenarioReason,
  requiredComponentFaultNotices,
  authoringWorkspaceIdentity,
  programLocalFence,
  projectFacts,
  publicationIsCurrent,
  publicationSupersedesDeployment,
  publicationSupersedesProgram,
  scenarioLocalFence,
  withSharedDeploymentAuthoring,
  withSharedProgramAuthoring,
  type PublishedAuthoringWorkspace,
} from './authoringWorkspace';
import type {
  AuthoringWorkspaceSnapshot,
  DeploymentWorkspaceResponse,
  ProgramAuthoringDocument,
  ProgramDocument,
  ProgramWorkspaceResponse,
  ScenarioSourceDiagnostic,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';

function workspaceSnapshot(
  workspaceRevision: number,
  programDraftRevision: number,
  deploymentDraftRevision: number,
): AuthoringWorkspaceSnapshot {
  return {
    schemaVersion: 'manyforge.composer_authoring_workspace_snapshot.v1',
    workspaceEpoch: 'workspace.epoch.1',
    workspaceRevision,
    program: {
      draft: {
        draftId: 'draft.1',
        draftRevision: programDraftRevision,
        programLoaded: true,
        authoring: { forest: [] } as unknown as ProgramAuthoringDocument,
        candidateAvailable: true,
        programRevision: `sha256:${'b'.repeat(64)}`,
        program: { api_version: 'manyforge.program.v2' } as unknown as ProgramDocument,
        diagnostics: [],
        formulaReferences: { computedKeys: {}, nodes: {} },
      },
      saved: {
        available: true,
        matchesDraft: false,
        path: '/workspace/saved.program.yaml',
        programRevision: `sha256:${'a'.repeat(64)}`,
      },
    },
    deployment: {
      draftId: 'deployment-draft.1',
      draftRevision: deploymentDraftRevision,
      document: { deployment_revision: 'deployment.2' },
      savedPath: '/workspace/saved.deployment.yaml',
      savedDocument: { deployment_revision: 'deployment.1' },
      differsFromSaved: true,
    },
    project: {
      documentPath: '/workspace/cell',
      documentId: 'cell',
      documentName: 'Cell',
      documentDescription: 'Fixture',
      sourceDiagnostics: [
        { code: 'scenario_source_changed', sourceId: 'mesh.1', message: 'changed' },
      ] satisfies ScenarioSourceDiagnostic[],
      sourceDocumentDigest: `sha256:${'c'.repeat(64)}`,
      programRoleAssignment: { arm: 'role.arm' },
      typedParameterOverrides: {},
      sourceProgramDigest: null,
      sourceDeploymentDigest: null,
    },
  };
}

function headlessState(
  workspaceRevision: number,
  overrides: Partial<ComposerHeadlessState['authoringWorkspace']> = {},
  state: Partial<ComposerHeadlessState> = {},
): ComposerHeadlessState {
  return {
    schemaVersion: 'manyforge.composer_headless_state.v2',
    complete: true,
    executionControlStatus: null,
    componentOverview: {} as never,
    alarms: {} as never,
    authoringAdmission: { authoringAllowed: true, admissionGeneration: 7 },
    programCycle: { state: 'idle_after_reset' } as never,
    authoringWorkspace: {
      workspaceEpoch: 'workspace.epoch.1',
      workspaceRevision,
      programDraftId: 'draft.1',
      programDraftRevision: 3,
      deploymentDraftId: 'deployment-draft.1',
      deploymentDraftRevision: 4,
      ...overrides,
    },
    receiverEpoch: 'receiver.epoch.1',
    snapshotSequence: workspaceRevision,
    ...state,
    runtime: state.runtime ?? ({} as ComposerHeadlessState['runtime']),
  };
}

function published(
  snapshot: AuthoringWorkspaceSnapshot,
): PublishedAuthoringWorkspace {
  return { identity: authoringWorkspaceIdentity(snapshot), snapshot };
}

describe('authoringWorkspace', () => {
  it('derives one identity from the snapshot and requires the observed one exactly', () => {
    const snapshot = workspaceSnapshot(5, 3, 4);
    expect(authoringWorkspaceIdentity(snapshot)).toEqual({
      workspaceEpoch: 'workspace.epoch.1',
      workspaceRevision: 5,
      programDraftId: 'draft.1',
      programDraftRevision: 3,
      deploymentDraftId: 'deployment-draft.1',
      deploymentDraftRevision: 4,
    });

    const installed = published(snapshot);
    expect(publicationIsCurrent(installed, headlessState(5))).toBe(true);
    // A newer revision, and each domain revision, supersedes the installation.
    expect(publicationIsCurrent(installed, headlessState(6))).toBe(false);
    expect(
      publicationIsCurrent(
        installed,
        headlessState(5, { programDraftRevision: 4 }),
      ),
    ).toBe(false);
    expect(
      publicationIsCurrent(
        installed,
        headlessState(5, { deploymentDraftRevision: 5 }),
      ),
    ).toBe(false);
    expect(
      publicationIsCurrent(installed, headlessState(5, { workspaceEpoch: 'other' })),
    ).toBe(false);
    expect(publicationIsCurrent(installed, null)).toBe(false);
  });

  it('types the authoring gate cause alongside its stable reason', () => {
    const installed = published(workspaceSnapshot(5, 3, 4));
    expect(authoringUnavailableCause(installed, headlessState(5))).toBeNull();
    expect(authoringUnavailableCause(null, null)).toBe('disconnected');
    expect(authoringUnavailableCause(null, headlessState(5))).toBe('not_read');
    expect(authoringUnavailableCause(installed, headlessState(6))).toBe('synchronizing');
    expect(authoringUnavailableCause(
      installed,
      headlessState(5, {}, { programCycle: { state: 'terminal_awaiting_reset' } as never }),
    )).toBe('execution_active');
    expect(authoringUnavailableCause(
      installed,
      headlessState(5, {}, {
        authoringAdmission: { authoringAllowed: false, admissionGeneration: 7 },
      }),
    )).toBe('not_admitted');
  });

  it('gates authoring with one stable reason per cause', () => {
    const snapshot = workspaceSnapshot(5, 3, 4);
    const installed = published(snapshot);

    expect(authoringUnavailableReason(installed, headlessState(5))).toBeNull();
    expect(authoringUnavailableReason(null, null)).toBe(
      'Backend status is disconnected or has not arrived.',
    );
    expect(authoringUnavailableReason(null, headlessState(5))).toBe(
      'The shared authoring workspace has not been read yet.',
    );
    expect(authoringUnavailableReason(installed, headlessState(6))).toBe(
      'The displayed workspace is still synchronizing with the backend.',
    );
    expect(
      authoringUnavailableReason(
        installed,
        headlessState(5, {}, { programCycle: { state: 'running' } as never }),
      ),
    ).toBe('Program execution is running; authoring requires Idle.');
    expect(
      authoringUnavailableReason(
        installed,
        headlessState(5, {}, {
          authoringAdmission: { authoringAllowed: false, admissionGeneration: 7 },
        }),
      ),
    ).toBe(
      'Authoring is currently locked by the backend.',
    );
  });

  it('explains only live manual operations and keeps owner faults visible', () => {
    const installed = published(workspaceSnapshot(5, 3, 4));
    const state = headlessState(5, {}, {
      authoringAdmission: { authoringAllowed: false, admissionGeneration: 7 },
    });
    for (const operationState of ['executing', 'paused', 'stopping']) {
      state.executionControlStatus = {
        manualOperations: [{ resident: true, state: operationState }],
      } as never;
      expect(authoringUnavailableReason(installed, state)).toContain('manual operation is active');
    }
    state.executionControlStatus = {
      manualOperations: [{ resident: false, state: 'completed' }],
    } as never;
    expect(authoringUnavailableReason(installed, state)).toBe('Authoring is currently locked by the backend.');
    state.componentOverview = {
      components: [{ id: 'arm', label: 'Arm', required: true, status: 'fault', detail: 'Owner reports a fault' }],
    } as never;
    expect(requiredComponentFaultNotices(state)).toEqual([expect.objectContaining({
      severity: 'error', title: 'Arm unavailable', message: 'Owner reports a fault',
      expectedDuringTransition: true,
    })]);
    state.authoringAdmission.authoringAllowed = true;
    expect(authoringUnavailableCause(installed, state)).toBeNull();
    expect(requiredComponentFaultNotices(state)).toHaveLength(1);
  });

  it('builds the one local fence and its Scenario extension from the publication', () => {
    const snapshot = workspaceSnapshot(5, 3, 4);
    const installed = published(snapshot);
    const state = headlessState(5);

    expect(programLocalFence(installed, state, 'owner.1', 'request.1')).toEqual({
      draftId: 'draft.1',
      expectedDraftRevision: 3,
      expectedWorkspaceEpoch: 'workspace.epoch.1',
      expectedWorkspaceRevision: 5,
      expectedAdmissionGeneration: 7,
      requestOwnerEpoch: 'owner.1',
      requestId: 'request.1',
    });
    expect(scenarioLocalFence(installed, state, 'owner.1', 'request.2')).toEqual({
      draftId: 'draft.1',
      expectedDraftRevision: 3,
      expectedWorkspaceEpoch: 'workspace.epoch.1',
      expectedWorkspaceRevision: 5,
      expectedAdmissionGeneration: 7,
      requestOwnerEpoch: 'owner.1',
      requestId: 'request.2',
      deploymentDraftId: 'deployment-draft.1',
      expectedDeploymentDraftRevision: 4,
    });
    expect(projectFacts(installed)?.documentPath).toBe('/workspace/cell');
    expect(projectFacts(null)).toBeNull();
  });

  it('never lets a lagging publication revert a just-committed projection', () => {
    const installed = published(workspaceSnapshot(5, 3, 4));
    const program = {
      draft: { draftId: 'draft.1', draftRevision: 4 },
    } as ProgramWorkspaceResponse;
    const deployment = {
      draft: { id: 'deployment-draft.1', revision: 5 },
    } as DeploymentWorkspaceResponse;

    expect(publicationSupersedesProgram(installed, program)).toBe(false);
    expect(publicationSupersedesDeployment(installed, deployment)).toBe(false);
    // A different Draft identity always means the workspace was replaced, and
    // then the publication is authoritative.
    expect(
      publicationSupersedesProgram(
        installed,
        { draft: { draftId: 'draft.2', draftRevision: 1 } } as ProgramWorkspaceResponse,
      ),
    ).toBe(true);
    expect(
      publicationSupersedesDeployment(
        installed,
        { draft: { id: 'deployment-draft.2', revision: 1 } } as DeploymentWorkspaceResponse,
      ),
    ).toBe(true);
  });

  it('replaces only the authoring half of each owner projection', () => {
    const snapshot = workspaceSnapshot(5, 3, 4);
    const program = {
      schemaVersion: 'manyforge.composer_program_workspace.v2',
      authoringWorkspace: {
        workspaceEpoch: 'workspace.epoch.1', workspaceRevision: 7,
        programDraftId: 'draft.1', programDraftRevision: 2,
        deploymentDraftId: 'deployment-draft.1', deploymentDraftRevision: 1,
      },
      draft: { draftId: 'draft.1', draftRevision: 2, programLoaded: false },
      applicability: { applyReady: true, affectedInstanceIds: [], unavailableReason: null },
      saved: { available: false, matchesDraft: false, path: null, programRevision: null },
      applied: {
        available: true,
        matchesDraft: null,
        programRevision: null,
        unavailableReason: null,
      },
      hasUnsavedChanges: false,
      hasUnappliedChanges: null,
    } as unknown as ProgramWorkspaceResponse;

    const mergedProgram = withSharedProgramAuthoring(program, snapshot);
    expect(mergedProgram.authoringWorkspace).toEqual(authoringWorkspaceIdentity(snapshot));
    expect(mergedProgram.draft).toEqual(snapshot.program.draft);
    expect(mergedProgram.saved).toEqual(snapshot.program.saved);
    expect(mergedProgram.hasUnsavedChanges).toBe(true);
    // Applied/applicability truth stays with its owner projection.
    expect(mergedProgram.applied).toBe(program.applied);
    expect(mergedProgram.applicability).toBe(program.applicability);

    const deployment = {
      schemaVersion: 'manyforge.composer_deployment_workspace.v1',
      authoringWorkspace: {
        workspaceEpoch: 'workspace.epoch.1', workspaceRevision: 7,
        programDraftId: 'draft.1', programDraftRevision: 2,
        deploymentDraftId: 'deployment-draft.1', deploymentDraftRevision: 1,
      },
      draft: {
        id: 'deployment-draft.1',
        revision: 1,
        document: null,
        deploymentArtifactId: 'artifact.1',
        deploymentRevision: 'deployment.1',
        graphRevision: 'graph.1',
        differsFromSaved: false,
      },
      saved: { path: null, deploymentRevision: null },
      managerApplied: { deploymentRevision: null, scenarioId: null },
      runAdmission: { ready: true, reason: null },
      binding: null,
      scenarioCandidates: [],
      savedScenarioCandidates: [],
      unavailableReasons: [],
    } as DeploymentWorkspaceResponse;

    const mergedDeployment = withSharedDeploymentAuthoring(deployment, snapshot);
    expect(mergedDeployment.authoringWorkspace).toEqual(authoringWorkspaceIdentity(snapshot));
    expect(mergedDeployment.draft.document).toEqual(
      snapshot.deployment.document,
    );
    expect(mergedDeployment.draft.differsFromSaved).toBe(true);
    expect(mergedDeployment.saved.path).toBe('/workspace/saved.deployment.yaml');
    expect(mergedDeployment.saved.deploymentRevision).toBe('deployment.1');
    // The compiled artifact identity and Manager truth stay with their owner.
    expect(mergedDeployment.draft.deploymentArtifactId).toBe('artifact.1');
    expect(mergedDeployment.managerApplied).toBe(deployment.managerApplied);
  });

  it('lets a first Program Apply to a Scenario applied without a Program', () => {
    const projection = (currentScenario: object | null, unavailableReasons: string[]) => ({
      schemaVersion: 'manyforge.current_scenario.v1' as const,
      ready: currentScenario !== null,
      currentScenario: currentScenario as never,
      unavailableReasons,
    });
    expect(programApplyScenarioReason(null)).toMatch(/still being read/);
    expect(programApplyScenarioReason(projection({ scenarioId: 'cell' }, []))).toBeNull();
    expect(programApplyScenarioReason(projection(null, ['program_not_selected']))).toBeNull();
    expect(programApplyScenarioReason(projection(null, ['program_not_applied'])))
      .toBe('program not applied');
    expect(programApplyScenarioReason(projection(null, ['program_not_selected', 'deployment_not_published'])))
      .toBe('program not selected, deployment not published');
    expect(programApplyScenarioReason(projection(null, []))).toBe('No exact current Scenario exists.');
  });
});
