import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import { effectiveField } from '../hooks/useEffectiveValues';
import type { EffectiveValues } from '../types/components';
import { LIFECYCLE_TIMEOUT_HELP, lifecycleTimeoutProblem } from '../utils/ioSignalConfiguration';
import { jsonObject } from '../utils/poseSourceAuthoring';
import { DraftTextInput } from './common/DraftInputs';

/**
 * A component's lifecycle time budget, `configuration.lifecycle_command_timeout_s`
 * of its Scenario row: the time Prepare, Enable and Disable may each take.
 * Shown with its effective value and default from `GET /api/deployment/effective`:
 * writing it adds it to the Draft, blank or Reset to default
 * removes it. The GUI keeps no copy of the default. Like any other setting it
 * takes effect at the confirmed Apply. An I/O device edits its own in its I/O
 * device section.
 */
export function LifecycleBudgetField({ controller, effective, componentIndex, componentId, disabled }: {
  controller: DeploymentWorkspaceController;
  effective: EffectiveValues | null;
  componentIndex: number;
  componentId: string;
  disabled: boolean;
}) {
  const document = controller.workspace?.draft.document ?? null;
  const rows = Array.isArray(document?.components) ? document.components : [];
  const stored = jsonObject(jsonObject(rows[componentIndex])?.configuration)?.lifecycle_command_timeout_s;
  const storedValue = typeof stored === 'number' && Number.isFinite(stored) ? stored : null;
  const set = storedValue !== null;
  const path = ['components', componentIndex, 'configuration', 'lifecycle_command_timeout_s'];
  const row = effectiveField(effective, path);
  const shown = row !== null && typeof row.value === 'number' ? row.value : storedValue;
  const label = `${componentId} lifecycle command time limit`;
  return (
    <div className="deployment-effective-field">
      <label className="deployment-focused-field" title={LIFECYCLE_TIMEOUT_HELP}>
        <span>Lifecycle command time limit</span>
        <span className="deployment-number-with-unit">
          <DraftTextInput aria-label={label} inputMode="decimal"
            value={shown} placeholder="default" disabled={disabled}
            validate={(text) => (text.trim() === '' ? null : lifecycleTimeoutProblem(text))}
            onCommit={(text) => {
              // A refusal shows once, in the yellow Deployment notice row.
              const trimmed = text.trim();
              if (trimmed === '') {
                if (set) void controller.mutate(path, 'remove');
                return;
              }
              const value = Number(trimmed);
              if (value !== shown) void controller.mutate(path, 'set', value);
            }} />
          <small>s</small>
        </span>
      </label>
      {row !== null && <small>{row.set ? `Set · default ${String(row.default)} s` : 'Default'}</small>}
      {set && (
        <button type="button" className="quiet-button" disabled={disabled}
          aria-label={`Reset ${label} to default`}
          title={row === null ? undefined : `Default: ${String(row.default)} s`}
          onClick={() => { void controller.mutate(path, 'remove'); }}>Reset to default</button>
      )}
    </div>
  );
}
