import type { ReactNode } from 'react';

/**
 * The header document controls of the open Scenario: the shown document's
 * name, the Scenario commands and the Scenario-wide Discard/Revert/Apply set.
 * One component type for every workspace tab, so switching tabs never
 * remounts the commands or an action in progress.
 */
export function DocumentControls({
  label,
  name,
  title,
  scenarioCommands,
  actions,
  notice,
}: {
  label: string;
  name: string;
  title?: string;
  /** The workspace-independent Scenario New/Save/Save as/Load commands. */
  scenarioCommands?: ReactNode;
  /** The Scenario-wide Discard / Revert / Apply set. */
  actions?: ReactNode;
  /** A document-specific notice shown below the controls. */
  notice?: ReactNode;
}) {
  return (
    <>
      <div className="document-controls" aria-label={label}>
        <div className="document-identity">
          <strong title={title}>{name}</strong>
        </div>
        <div className="document-actions">
          {scenarioCommands && (
            <>
              {scenarioCommands}
              <span className="header-action-divider" aria-hidden="true" />
            </>
          )}
          {actions}
        </div>
      </div>
      {notice}
    </>
  );
}
