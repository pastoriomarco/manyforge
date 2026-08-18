import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

import type { ScenarioCatalogResponse } from '../types/api';
import {
  retainedScenarioSelection,
  scenarioSelectionAfterRequest,
} from './useProgramRuntime';

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
  it('does not derive operator selection from the catalog or Applied projection', () => {
    expect(retainedScenarioSelection(null, catalog)).toBeNull();

    const source = readFileSync(new URL('./useProgramRuntime.ts', import.meta.url), 'utf8');
    const refresh = source.slice(
      source.indexOf('const refresh = useCallback'),
      source.indexOf('useEffect(() =>', source.indexOf('const refresh = useCallback')),
    );
    expect(refresh).not.toContain('api.programs.start');
    expect(refresh).not.toContain('applyDraft');
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
