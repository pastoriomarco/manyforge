import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramRunStateName } from '../types/api';
import { ProgramRuntimePanel } from './ProgramRuntimePanel';

function controller(
  state: ProgramRunStateName = 'running',
): ProgramRuntimeController {
  return {
    state: {
      catalog: null,
      applied: {
        schemaVersion: 'manyforge.applied_program_selection.v1',
        scenarioId: 'cell-pick',
        scenarioCatalogRevision: `sha256:${'a'.repeat(64)}`,
        programId: 'next-pick',
        programRevision: `sha256:${'b'.repeat(64)}`,
        programBindingId: `sha256:${'c'.repeat(64)}`,
        retainedBundleId: `sha256:${'a'.repeat(64)}`,
        graphRevision: `sha256:${'b'.repeat(64)}`,
        nodeEnvironmentDigest: `sha256:${'c'.repeat(64)}`,
        recordDigest: `sha256:${'a'.repeat(64)}`,
        sourceOwnerId: 'headless.program-apply',
        sourceOwnerEpoch: 'headless.epoch.1',
        sourceGeneration: 1,
      },
      run: {
        schemaVersion: 'manyforge.program_run_status.v1',
        ownerId: 'program-runner.cell-a',
        ownerEpoch: 'program-runner.epoch.1',
        runRevision: 4,
        state,
        capture: {
          programRunId: 'program-run.1',
          scenarioId: 'cell-pick',
          scenarioCatalogRevision: `sha256:${'a'.repeat(64)}`,
          programId: 'pick',
          programRevision: `sha256:${'b'.repeat(64)}`,
          programBindingId: `sha256:${'c'.repeat(64)}`,
          retainedBundleId: `sha256:${'a'.repeat(64)}`,
          graphRevision: `sha256:${'b'.repeat(64)}`,
          nodeEnvironmentDigest: `sha256:${'c'.repeat(64)}`,
          ownerIdentities: [{
            ownerRole: 'program_runner',
            ownerId: 'program-runner.cell-a',
            ownerEpoch: 'program-runner.epoch.1',
          }],
        },
        treeStatus: 'running',
        activeOperationCount: 1,
        reasonCode: null,
        message: '',
      },
      selectedScenarioId: null,
      runnerAvailable: true,
      loading: false,
      pending: null,
      error: null,
    },
    active: ['running', 'intervention_required', 'stopping', 'finalizing']
      .includes(state),
    refresh: vi.fn(),
    selectScenario: vi.fn(),
    clearScenarioSelection: vi.fn(),
    run: vi.fn(),
    stop: vi.fn(),
  };
}

describe('Program runtime projection', () => {
  it('shows exact owner status and retains one-click Stop for one active run', () => {
    const markup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={controller()} />,
    );

    expect(markup).toContain('Headless-owned execution');
    expect(markup).toContain('program-run.1');
    expect(markup).toContain('revision 4');
    expect(markup).toContain('next-pick');
    expect(markup).toContain('Selection for the next Run');
    expect(markup).toContain('Captured run Program');
    expect(markup).toContain('immutable for this run');
    expect(markup).toContain('Stop Program');
    expect(markup).not.toContain('Stop all');
    expect(markup).toContain('never sends Stop implicitly');
  });

  it('enables Run only after the owner-issued run is terminal', () => {
    const active = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={controller()} />,
    );
    const terminal = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={controller('succeeded')} />,
    );

    expect(active).toMatch(/disabled=""[^>]*>Run Applied Program/);
    expect(terminal).not.toMatch(/disabled=""[^>]*>Run Applied Program/);
  });

  it('never turns an unavailable or unknown owner fact into idle or zero work', () => {
    const unavailable = controller();
    unavailable.state.run = null;
    unavailable.state.runnerAvailable = false;
    const unavailableMarkup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={unavailable} />,
    );
    expect(unavailableMarkup).toContain('unavailable');
    expect(unavailableMarkup).toContain('execution state is unknown');
    expect(unavailableMarkup).toContain('Runner loss never proves');

    const unknownCount = controller();
    if (unknownCount.state.run) {
      unknownCount.state.run.activeOperationCount = null;
    }
    const unknownMarkup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={unknownCount} />,
    );
    expect(unknownMarkup).toContain('<strong>unknown</strong>');
  });
});
