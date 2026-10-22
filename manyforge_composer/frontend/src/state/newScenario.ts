// Scenario naming rules shared by New and Save As.

import type { ScenarioDocumentEntry } from '../types/api';

/** The backend's limit for a Scenario name. */
export const SCENARIO_NAME_MAX = 128;
/**
 * A Scenario's one name: letters, digits, `_` and `-`, starting with a letter
 * or digit, no dots or spaces. It is the project folder name without
 * `.scenario` and also its scenario ID. Mirrors `SCENARIO_NAME_PATTERN` in
 * `backend/scenario_artifacts.py`.
 */
export const SCENARIO_NAME_PATTERN = /^[A-Za-z0-9][A-Za-z0-9_-]{0,127}$/;
/** The folder suffix that marks a Scenario project. */
export const SCENARIO_PROJECT_SUFFIX = '.scenario';

/** Project folder name of a Scenario name: `my_cell.scenario`. */
export function scenarioFolderName(name: string): string {
  return `${name}${SCENARIO_PROJECT_SUFFIX}`;
}

/** The project folder `<directory>/<name>.scenario`. */
export function scenarioProjectPath(directory: string, name: string): string {
  return `${directory.replace(/\/+$/, '')}/${scenarioFolderName(name)}`;
}

/** The last component of `path`. */
function baseName(path: string): string {
  const trimmed = path.replace(/\/+$/, '');
  return trimmed.slice(trimmed.lastIndexOf('/') + 1);
}

/** The name a project folder shows: its folder name without `.scenario`. */
export function scenarioProjectStem(path: string): string {
  const folder = baseName(path);
  return folder.endsWith(SCENARIO_PROJECT_SUFFIX) && folder.length > SCENARIO_PROJECT_SUFFIX.length
    ? folder.slice(0, -SCENARIO_PROJECT_SUFFIX.length)
    : folder;
}

/** The folder that contains `path`. */
export function parentFolder(path: string): string {
  const trimmed = path.replace(/\/+$/, '');
  const index = trimmed.lastIndexOf('/');
  return index > 0 ? trimmed.slice(0, index) : '/';
}

/** What is wrong with a Scenario name, or null. */
export function scenarioNameError(name: string): string | null {
  if (!name) return 'Enter a name for the Scenario.';
  if (name.length > SCENARIO_NAME_MAX) return `Use at most ${SCENARIO_NAME_MAX} characters.`;
  if (/\s/.test(name)) return 'Spaces aren’t allowed. Join words with “_” or “-”, e.g. pick_and_place.';
  if (!/^[A-Za-z0-9]/.test(name)) return 'Start with a letter or digit.';
  if (!SCENARIO_NAME_PATTERN.test(name)) {
    return 'Only letters, digits, “_” and “-” are allowed.';
  }
  return null;
}

/** Whether `<name>.scenario` already exists among a folder's entry names. */
export function scenarioNameTaken(name: string, existing: ReadonlySet<string> | null): boolean {
  return existing?.has(scenarioFolderName(name)) === true;
}

/**
 * The label of one listed Scenario: its name (the folder name without
 * `.scenario`). A refused folder shows its whole folder name.
 */
export function scenarioEntryLabel(entry: ScenarioDocumentEntry): string {
  return entry.displayStatus === 'unsupported' ? baseName(entry.path) : entry.folderName;
}
