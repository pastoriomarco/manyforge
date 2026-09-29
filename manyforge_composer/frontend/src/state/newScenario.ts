// Field rules of the New Scenario dialog.

/** The backend's limit for a Scenario name. */
export const SCENARIO_NAME_MAX = 256;
/**
 * A new Scenario's name is one word: letters, digits, `_` and `-`, starting
 * with a letter or digit. It doubles as the project folder name. Mirrors
 * `NEW_SCENARIO_NAME_PATTERN` in the backend.
 */
const NEW_SCENARIO_NAME_PATTERN = /^[A-Za-z0-9][A-Za-z0-9_-]*$/;

/** Project folder name suggested for a Scenario name: `my_cell.scenario`. */
export function scenarioFolderName(name: string): string {
  return `${name.trim() || 'scenario'}.scenario`;
}

/** The folder that contains `path`. */
export function parentFolder(path: string): string {
  const trimmed = path.replace(/\/+$/, '');
  const index = trimmed.lastIndexOf('/');
  return index > 0 ? trimmed.slice(0, index) : '/';
}

/** What is wrong with a new Scenario's name, or null. */
export function newScenarioNameError(name: string): string | null {
  if (!name) return 'Enter a name for the Scenario.';
  if (name.length > SCENARIO_NAME_MAX) return `Use at most ${SCENARIO_NAME_MAX} characters.`;
  if (/\s/.test(name)) return 'Spaces aren’t allowed. Join words with “_” or “-”, e.g. pick_and_place.';
  if (!/^[A-Za-z0-9]/.test(name)) return 'Start with a letter or digit.';
  if (!NEW_SCENARIO_NAME_PATTERN.test(name)) {
    return 'Only letters, digits, “_” and “-” are allowed.';
  }
  return null;
}

/** What is wrong with a new project folder name, or null. */
export function newScenarioFolderError(
  folder: string,
  existing: ReadonlySet<string> | null,
): string | null {
  const trimmed = folder.trim();
  if (!trimmed) return 'Enter a folder name.';
  if (trimmed.includes('/')) return 'A folder name cannot contain “/”.';
  if (trimmed === '.' || trimmed === '..') return 'Choose a real folder name.';
  if (existing?.has(trimmed)) return 'Something with this name already exists here. Choose another name.';
  return null;
}
