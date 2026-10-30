import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import { lifecycleTimeoutProblem } from '../utils/ioSignalConfiguration';
import { jsonObject } from '../utils/poseSourceAuthoring';
import { DraftTextInput } from './common/DraftInputs';

/**
 * A component's lifecycle time budget, `configuration.lifecycle_command_timeout_s`
 * of its Scenario row: the maximum time one of its lifecycle steps may take,
 * and its share of the wait of an Apply that installs or replaces it.
 * Individual controller calls keep their own short response limits. Blank
 * keeps the default (10 s, every component). Like any
 * other setting it goes into the Draft and takes effect at the confirmed
 * Apply. An I/O device edits its own in its I/O device section.
 */
export function LifecycleBudgetField({ controller, componentIndex, componentId, disabled }: {
  controller: DeploymentWorkspaceController;
  componentIndex: number;
  componentId: string;
  disabled: boolean;
}) {
  const document = controller.workspace?.draft.document ?? null;
  const rows = Array.isArray(document?.components) ? document.components : [];
  const stored = jsonObject(jsonObject(rows[componentIndex])?.configuration)?.lifecycle_command_timeout_s;
  const current = typeof stored === 'number' && Number.isFinite(stored) ? stored : null;
  const path = ['components', componentIndex, 'configuration', 'lifecycle_command_timeout_s'];
  return (
    <label className="deployment-focused-field"
      title="Maximum time a lifecycle step may take; individual controller calls must still answer within a few seconds. Blank: the default, 10 s.">
      <span>Lifecycle command time limit</span>
      <span className="deployment-number-with-unit">
        <DraftTextInput aria-label={`${componentId} lifecycle command time limit`} inputMode="decimal"
          value={current} placeholder="default" disabled={disabled}
          validate={(text) => (text.trim() === '' ? null : lifecycleTimeoutProblem(text))}
          onCommit={(text) => {
            // A refusal shows once, in the yellow Deployment notice row.
            if (text.trim() !== '') void controller.mutate(path, 'set', Number(text.trim()));
            else if (current !== null) void controller.mutate(path, 'remove');
          }} />
        <small>s</small>
      </span>
    </label>
  );
}
