import { describe, expect, it } from 'vitest';

import {
  newScenarioFolderError,
  newScenarioNameError,
  scenarioFolderName,
} from './newScenario';

describe('New Scenario fields', () => {
  it('requires a one-word name of letters, digits, "_" and "-"', () => {
    for (const name of ['lite6_pick', 'Cell-2', '9cell', 'a']) {
      expect(newScenarioNameError(name)).toBeNull();
    }
    expect(newScenarioNameError('')).toBe('Enter a name for the Scenario.');
    expect(newScenarioNameError('pick and place')).toMatch(/Spaces aren’t allowed/);
    expect(newScenarioNameError('pick\tplace')).toMatch(/Spaces aren’t allowed/);
    expect(newScenarioNameError(' cell')).toMatch(/Spaces aren’t allowed/);
    for (const name of ['-cell', '_cell']) {
      expect(newScenarioNameError(name)).toBe('Start with a letter or digit.');
    }
    for (const name of ['cell.v2', 'cell/x', 'cellé']) {
      expect(newScenarioNameError(name)).toBe('Only letters, digits, “_” and “-” are allowed.');
    }
    expect(newScenarioNameError('a'.repeat(257))).toMatch(/at most 256/);
  });

  it('suggests the name as the project folder', () => {
    expect(scenarioFolderName('lite6_pick')).toBe('lite6_pick.scenario');
    expect(scenarioFolderName('')).toBe('scenario.scenario');
  });

  it('refuses an empty, nested or existing project folder', () => {
    const existing = new Set(['taken.scenario']);
    expect(newScenarioFolderError('new.scenario', existing)).toBeNull();
    expect(newScenarioFolderError('new.scenario', null)).toBeNull();
    expect(newScenarioFolderError('', existing)).toBe('Enter a folder name.');
    expect(newScenarioFolderError('a/b', existing)).toMatch(/cannot contain/);
    expect(newScenarioFolderError('..', existing)).toBe('Choose a real folder name.');
    expect(newScenarioFolderError('taken.scenario', existing)).toMatch(/already exists/);
  });
});
