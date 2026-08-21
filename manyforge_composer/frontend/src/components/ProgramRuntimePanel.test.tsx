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
        program: {
          api_version: 'manyforge.program.v1',
          program_id: 'next-pick',
          program_revision: `sha256:${'b'.repeat(64)}`,
          program_profile: 'composable-cell-v1',
          name: 'Next pick',
          catalog: {
            api_version: 'manyforge.node_catalog.v2',
            content_version: '1.1.0',
          },
          component_roles: { component: {} },
          target_roles: {},
          tree: {
            node_id: 'logic.check_bool',
            instance_id: 'root',
            parameters: { actual_value: true, expected_value: true },
            children: [],
          },
        },
        programBinding: {
          api_version: 'manyforge.program_binding.v1',
          program_binding_id: `sha256:${'c'.repeat(64)}`,
          program_id: 'next-pick',
          program_revision: `sha256:${'b'.repeat(64)}`,
          retained_bundle_id: `sha256:${'a'.repeat(64)}`,
          graph_revision: `sha256:${'b'.repeat(64)}`,
          node_environment: {
            catalog_id: 'manyforge.default',
            content_version: '1.1.0',
            catalog_digest: `sha256:${'a'.repeat(64)}`,
            environment_digest: `sha256:${'c'.repeat(64)}`,
          },
          component_role_bindings: [],
          target_role_bindings: [],
          node_policies: [],
          contact_grasp_bindings: [],
          provenance: {
            binding_compiler_contract_digest: `sha256:${'a'.repeat(64)}`,
            binding_compiler_build_digest: `sha256:${'b'.repeat(64)}`,
            role_assignment_digest: `sha256:${'c'.repeat(64)}`,
            source_records: [],
            resolved_output_records: [],
          },
        },
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
