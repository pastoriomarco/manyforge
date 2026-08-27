import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

import type {
  AppliedProgramSelection,
  ProgramWorkspaceResponse,
  ScenarioCatalogResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import {
  buildProgramResetRequest,
  buildProgramStartRequest,
  programRuntimeWakeKey,
  retainedScenarioSelection,
  scenarioSelectionAfterRequest,
} from './useProgramRuntime';

const digest = (character: string) => `sha256:${character.repeat(64)}`;

function admissionFixtures() {
  const applied = {
    scenarioId: 'applied',
    scenarioCatalogRevision: digest('a'),
    programRevision: digest('b'),
    recordDigest: digest('c'),
  } as AppliedProgramSelection;
  const workspace = {
    draft: {
      draftId: 'draft-1', draftRevision: 5, programLoaded: true,
      programRevision: digest('b'),
    },
  } as ProgramWorkspaceResponse;
  const snapshot = {
    authoringAdmission: { authoringAllowed: true, admissionGeneration: 9 },
    programCycle: {
      programRunnerOwnerEpoch: 'runner.epoch.1', cycleGeneration: 3,
      state: 'idle_after_reset', programRunId: null,
      cycleClaimGeneration: null, terminalOrSettlementReference: null,
      runtimeControlRevision: null, recoveryOffer: null,
    },
    draftIdentity: { draftId: 'draft-1', draftRevision: 5 },
  } as ComposerHeadlessState;
  return { applied, workspace, snapshot };
}

const catalog = {
  apiVersion: 'manyforge.scenario_catalog.v1',
  scenarioCatalogRevision: `sha256:${'a'.repeat(64)}`,
  scenarios: [
    {
      scenarioId: 'first',
      description: 'First scenario',
      aliases: [],
      deploymentArtifactId: 'deployment.first',
      deploymentRevision: `sha256:${'b'.repeat(64)}`,
      retainedBundleId: `sha256:${'c'.repeat(64)}`,
      programBindingId: `sha256:${'d'.repeat(64)}`,
      assistantMode: 'optional',
    },
    {
      scenarioId: 'applied',
      description: 'Applied scenario',
      aliases: [],
      deploymentArtifactId: 'deployment.applied',
      deploymentRevision: `sha256:${'e'.repeat(64)}`,
      retainedBundleId: `sha256:${'f'.repeat(64)}`,
      programBindingId: `sha256:${'1'.repeat(64)}`,
      assistantMode: 'forbidden',
    },
  ],
} as ScenarioCatalogResponse;

describe('Program runtime idle selection', () => {
  it('uses semantic owner revisions instead of a fixed browser poll', () => {
    const source = readFileSync(new URL('./useProgramRuntime.ts', import.meta.url), 'utf8');
    expect(source).not.toContain('setInterval');
    const snapshot = {
      receiverEpoch: 'receiver.1',
      runtime: {
        sections: {
          manager: { sourceOwnerEpoch: 'manager.1', sourceGeneration: 4 },
        },
      },
      authoringAdmission: { admissionGeneration: 7 },
      programCycle: {
        programRunnerOwnerEpoch: 'runner.1',
        cycleGeneration: 2,
        state: 'idle_after_reset',
        runtimeControlRevision: null,
        recoveryOffer: null,
      },
      draftIdentity: { draftId: 'draft-1', draftRevision: 5 },
    } as ComposerHeadlessState;
    expect(programRuntimeWakeKey(snapshot)).toContain(
      'manager.1:no-retained-bundle:no-graph-revision:7',
    );
    expect(programRuntimeWakeKey({
      ...snapshot,
      runtime: {
        ...snapshot.runtime,
        sections: {
          ...snapshot.runtime.sections,
          manager: { ...snapshot.runtime.sections.manager, sourceGeneration: 99 },
        },
      },
    })).toBe(programRuntimeWakeKey(snapshot));
    expect(programRuntimeWakeKey({
      ...snapshot,
      draftIdentity: { draftId: 'draft-1', draftRevision: 6 },
    })).toBe(programRuntimeWakeKey(snapshot));
    expect(programRuntimeWakeKey({
      ...snapshot,
      authoringAdmission: {
        ...snapshot.authoringAdmission,
        admissionGeneration: 8,
      },
    })).not.toBe(programRuntimeWakeKey(snapshot));
    expect(programRuntimeWakeKey({
      ...snapshot,
      programCycle: {
        ...snapshot.programCycle,
        runtimeControlRevision: 2,
      },
    })).not.toBe(programRuntimeWakeKey(snapshot));
  });

  it('does not derive operator selection from the catalog or Applied projection', () => {
    expect(retainedScenarioSelection(null, catalog)).toBeNull();

    const source = readFileSync(new URL('./useProgramRuntime.ts', import.meta.url), 'utf8');
    const refresh = source.slice(
      source.indexOf('const refresh = useCallback'),
      source.indexOf('useEffect(() =>', source.indexOf('const refresh = useCallback')),
    );
    expect(refresh).not.toContain('api.programs.start');
    expect(refresh).not.toContain('applyDraft');
    expect(refresh).toContain('api.programs.currentScenario()');
  });

  it('retains only a still-available explicit operator selection', () => {
    expect(retainedScenarioSelection('applied', catalog)).toBe('applied');
    expect(retainedScenarioSelection('removed', catalog)).toBeNull();
  });

  it('selects and then explicitly clears without substituting another scenario', () => {
    const selected = scenarioSelectionAfterRequest(null, catalog, 'applied');
    expect(selected).toBe('applied');
    expect(scenarioSelectionAfterRequest(selected, catalog, null)).toBeNull();
  });
});

describe('Program runtime authority requests', () => {
  it('starts only from one complete Idle snapshot and carries exact fences', () => {
    const { applied, workspace, snapshot } = admissionFixtures();
    expect(buildProgramStartRequest(
      applied, true, null, snapshot, workspace,
      'composer-program-owner.fixture', 'program-run-start.fixture',
    )).toEqual({
      request_id: 'program-run-start.fixture',
      request_owner_epoch: 'composer-program-owner.fixture',
      source_label: 'operator_ui',
      correlation_id: 'program-run-start.fixture',
      scenario_id: 'applied',
      scenario_catalog_revision: digest('a'),
      expected_applied_record_digest: digest('c'),
      draft_program_revision: digest('b'),
      expected_admission_generation: 9,
    });

    expect(buildProgramStartRequest(
      applied, true, null, null, workspace,
      'composer-program-owner.fixture', 'program-run-start.fixture',
    )).toBeNull();
    expect(buildProgramStartRequest(
      applied, true, null,
      { ...snapshot, programCycle: { ...snapshot.programCycle, state: 'running' } },
      workspace, 'composer-program-owner.fixture', 'program-run-start.fixture',
    )).toBeNull();
    expect(buildProgramStartRequest(
      applied, true, null, snapshot,
      { ...workspace, draft: { ...workspace.draft, draftRevision: 6 } },
      'composer-program-owner.fixture', 'program-run-start.fixture',
    )).toBeNull();
  });

  it('resets only the exact terminal cycle and reuses owner terminal evidence', () => {
    const { snapshot } = admissionFixtures();
    const terminalReference = digest('d');
    const terminalSnapshot = {
      ...snapshot,
      programCycle: {
        programRunnerOwnerEpoch: 'runner.epoch.1', cycleGeneration: 4,
        state: 'terminal_awaiting_reset' as const,
        programRunId: 'run-1', cycleClaimGeneration: 12,
        terminalOrSettlementReference: terminalReference,
        runtimeControlRevision: null, recoveryOffer: null,
      },
    };
    expect(buildProgramResetRequest(
      terminalSnapshot, 'composer-program-owner.fixture', 'reset.fixture',
    )).toEqual({
      request_id: 'reset.fixture',
      request_owner_epoch: 'composer-program-owner.fixture',
      source_label: 'operator_ui',
      correlation_id: null,
      program_run_id: 'run-1',
      expected_owner_epoch: 'runner.epoch.1',
      expected_cycle_generation: 4,
      expected_claim_generation: 12,
      terminal_reference: terminalReference,
      settlement_evidence_digest: terminalReference,
    });
    expect(buildProgramResetRequest(
      {
        ...terminalSnapshot,
        programCycle: {
          ...terminalSnapshot.programCycle,
          terminalOrSettlementReference: null,
        },
      },
      'composer-program-owner.fixture', 'reset.fixture',
    )).toBeNull();
  });
});
