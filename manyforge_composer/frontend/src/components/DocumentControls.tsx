import type { ReactNode } from 'react';

/**
 * The open Scenario's name: its saved project folder without `.scenario`
 * (OD-368). The built-in Untitled Scenario, or none, reads "Untitled".
 */
export function scenarioName(savedPath: string | null | undefined): string {
  const folder = savedPath?.match(/([^/]+)\.scenario\/[^/]+$/)?.[1];
  return folder ?? 'Untitled';
}

/**
 * The header document controls of the open Scenario: the Scenario's name
 * (its folder in the tooltip), the Scenario commands and the Scenario-wide Discard/Revert/Apply set.
 * One component type for every workspace tab, so switching tabs never
 * remounts the commands or an action in progress.
 */
export function DocumentControls({
  label,
  scenarioPath,
  scenarioCommands,
  actions,
  notice,
}: {
  label: string;
  /** A saved path inside the open Scenario folder (its Program or Deployment file). */
  scenarioPath: string | null;
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
          <strong title={`Scenario: ${scenarioPath?.replace(/\/[^/]+$/, '') ?? 'not saved'}`}>
            {scenarioName(scenarioPath)}
          </strong>
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
