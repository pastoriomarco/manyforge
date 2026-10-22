import { describe, expect, it } from 'vitest';

import type { ScenarioDocumentEntry } from '../types/api';
import {
  parentFolder,
  SCENARIO_NAME_PATTERN,
  scenarioEntryLabel,
  scenarioFolderName,
  scenarioNameError,
  scenarioNameTaken,
  scenarioProjectPath,
  scenarioProjectStem,
} from './newScenario';

describe('Scenario name rule', () => {
  it('accepts one word of letters, digits, "_" and "-" starting with a letter or digit', () => {
    for (const name of ['lite6_pick', 'Cell-2', '9cell', 'a', 'A', 'isaac-lite6-pick-place', 'x'.repeat(128)]) {
      expect(scenarioNameError(name)).toBeNull();
      expect(SCENARIO_NAME_PATTERN.test(name)).toBe(true);
    }
  });

  it('refuses spaces, dots, unicode, a leading "_" or "-" and over-long names', () => {
    expect(scenarioNameError('')).toBe('Enter a name for the Scenario.');
    for (const name of ['pick and place', 'pick\tplace', ' cell', 'cell ', 'cell\n']) {
      expect(scenarioNameError(name)).toMatch(/Spaces aren’t allowed/);
    }
    for (const name of ['-cell', '_cell', '.cell', 'écell']) {
      expect(scenarioNameError(name)).toBe('Start with a letter or digit.');
    }
    for (const name of ['cell.v2', 'cell/x', 'cellé', 'cellж', 'name.scenario']) {
      expect(scenarioNameError(name)).toBe('Only letters, digits, “_” and “-” are allowed.');
    }
    expect(scenarioNameError('a'.repeat(129))).toMatch(/at most 128/);
    for (const name of ['', 'a b', 'a.b', '_a', 'é', 'a'.repeat(129)]) {
      expect(SCENARIO_NAME_PATTERN.test(name)).toBe(false);
    }
  });
});

describe('Scenario project folders', () => {
  it('builds <directory>/<name>.scenario and reads the name back', () => {
    expect(scenarioFolderName('lite6_pick')).toBe('lite6_pick.scenario');
    expect(scenarioProjectPath('/cells/', 'Pick')).toBe('/cells/Pick.scenario');
    expect(scenarioProjectStem('/cells/Pick.scenario')).toBe('Pick');
    expect(scenarioProjectStem('/cells/Pick.scenario/')).toBe('Pick');
    expect(scenarioProjectStem('/cells/legacy')).toBe('legacy');
    expect(parentFolder('/cells/Pick.scenario')).toBe('/cells');
  });

  it('detects an existing project of the same name', () => {
    const existing = new Set(['taken.scenario', 'folder']);
    expect(scenarioNameTaken('taken', existing)).toBe(true);
    expect(scenarioNameTaken('folder', existing)).toBe(false);
    expect(scenarioNameTaken('taken', null)).toBe(false);
  });
});

describe('Available Scenario labels', () => {
  const entry = (path: string, folderName: string, displayStatus: ScenarioDocumentEntry['displayStatus'] = 'ready'): ScenarioDocumentEntry => ({
    sourceKind: 'saved', path, scenarioId: displayStatus === 'unsupported' ? null : folderName, name: folderName,
    folderName, location: 'examples', description: '', savedManyforgeVersion: null, displayStatus, statusDetail: '',
  });

  it('shows the Scenario name', () => {
    expect(scenarioEntryLabel(entry('/r/examples/ur5_pick_and_place.scenario', 'ur5_pick_and_place')))
      .toBe('ur5_pick_and_place');
  });

  it('shows a refused folder by its whole folder name', () => {
    expect(scenarioEntryLabel(entry('/r/examples/pick', 'pick', 'unsupported'))).toBe('pick');
    expect(scenarioEntryLabel(entry('/r/examples/v1.2.scenario', 'v1.2', 'unsupported'))).toBe('v1.2.scenario');
  });
});
