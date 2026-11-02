/**
 * The one pattern for a button that starts a long operation: on click it
 * shows its busy label and is disabled, and every command that would race it
 * is disabled with a tooltip naming the operation, until the operation's
 * result (or its pushed state) arrives.
 */
export interface BusyOperation {
  /** The command that started it; its button shows `label`. */
  command: string;
  /** The busy label of that button, e.g. "Applying…". */
  label: string;
  /** What the conflicting commands' tooltips name, e.g. "Apply". */
  name: string;
}

const AUTHORING_OPERATIONS = {
  edit: ['Saving edit…', 'A Draft edit'],
  open_draft: ['Opening…', 'Open'],
  preview: ['Previewing…', 'Preview'],
  apply_preview: ['Previewing…', 'Apply preview'],
  revert_preview: ['Previewing…', 'Revert preview'],
  apply: ['Applying…', 'Apply'],
  revert: ['Reverting…', 'Revert'],
  discard: ['Discarding…', 'Discard'],
  save: ['Saving…', 'Save'],
  save_as: ['Saving…', 'Save as'],
  export: ['Exporting…', 'Export'],
  load: ['Loading…', 'Load'],
  new: ['Emptying cell…', 'New Scenario'],
} as const;

/** A Program, Deployment or Scenario authoring command. */
export type AuthoringCommand = keyof typeof AUTHORING_OPERATIONS;

export function authoringOperation(command: AuthoringCommand): BusyOperation {
  const [label, name] = AUTHORING_OPERATIONS[command];
  return { command, label, name };
}

/** The tooltip of a command disabled by `operation`, or null when none runs. */
export function busyReason(operation: BusyOperation | null): string | null {
  return operation ? `${operation.name} is in progress.` : null;
}

/** A button's text: its busy label while one of its own commands runs. */
export function busyLabel(
  operation: BusyOperation | null,
  commands: string | readonly string[],
  idle: string,
): string {
  if (!operation) return idle;
  const own = typeof commands === 'string' ? [commands] : commands;
  return own.includes(operation.command) ? operation.label : idle;
}
