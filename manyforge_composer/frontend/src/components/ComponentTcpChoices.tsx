import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import { errorCatalogRow } from '../state/commandRefusal';
import type { ComponentProfileFacts } from '../types/componentCatalog';
import {
  defaultTcpChoice,
  defaultTcpEdit,
  primaryToolChoices,
  tcpPairLabel,
  type TcpPair,
} from '../utils/componentTcpChoices';

/** The typed Apply refusals of these choices (B5), explained by the catalog. */
const CHOICE_REFUSALS = [
  'component_default_tcp_required', 'component_default_tcp_unknown', 'mount_primary_tool_required',
];

/**
 * The "Default TCP and primary tool" card of a component page (B5): the
 * component's default TCP when it has TCPs on more than one target (a
 * Deployment Draft edit that runs after Apply), and the primary tool of each
 * of its interfaces that carries two or more tools. The primary mark is on
 * the mount row, so like every mount it is shown read-only. Each appears only
 * in its ambiguous case.
 */
export function ComponentTcpChoices({ componentId, controller, profiles }: {
  componentId: string;
  controller: DeploymentWorkspaceController;
  profiles: Readonly<Record<string, ComponentProfileFacts>>;
}) {
  const document = controller.workspace?.draft.document ?? null;
  const choice = defaultTcpChoice(document, componentId, profiles);
  const primaries = primaryToolChoices(document, componentId, profiles);
  if (choice === null && primaries.length === 0) return null;
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;
  const key = (pair: TcpPair) => JSON.stringify([pair.commandTargetId, pair.frameRoleId]);
  const refusal = controller.error !== null && controller.errorCauseCode !== null
    && CHOICE_REFUSALS.includes(controller.errorCauseCode) ? errorCatalogRow(controller.errorCauseCode) : null;
  const chosen = choice?.options.find((pair) => choice.value !== null && key(pair) === key(choice.value)) ?? null;
  return (
    <section className="dcard" aria-labelledby="component-default-tcp-title">
      <h4 id="component-default-tcp-title">Default TCP and primary tool</h4>
      <div className="deployment-focused-body">
        {choice !== null && (
          <label className="deployment-focused-field">
            <span>Default TCP</span>
            <select aria-label={`${componentId} default TCP`} value={chosen === null ? '' : key(chosen)}
              disabled={disabled}
              onChange={(event) => {
                const pair = choice.options.find((option) => key(option) === event.target.value);
                const edit = pair === undefined ? null : defaultTcpEdit(document, componentId, pair);
                if (edit !== null) void controller.mutate(edit.path, edit.operation, edit.value);
              }}>
              {chosen === null && (
                <option value="" disabled>
                  {choice.value === null ? 'Choose a TCP' : `Unknown: ${tcpPairLabel(choice.value)}`}
                </option>
              )}
              {choice.options.map((pair) => <option key={key(pair)} value={key(pair)}>{tcpPairLabel(pair)}</option>)}
            </select>
          </label>
        )}
      </div>
      {primaries.length > 0 && (
        <dl aria-label={`${componentId} primary tools`}>
          {primaries.map((mount) => (
            <div key={mount.interfaceId}>
              <dt>{`Primary tool on ${mount.interfaceId}`}</dt>
              <dd>{mount.primary ?? `none marked (${mount.tools.join(', ')})`}</dd>
            </div>
          ))}
        </dl>
      )}
      {refusal !== null && <p className="inline-message severity-error" role="alert">{refusal[1]}</p>}
      {choice !== null && (
        <p className="field-help">
          This component has TCPs on more than one target: choose its default TCP. Saved in the Deployment Draft;
          takes effect at Apply.
        </p>
      )}
      {primaries.length > 0 && (
        <p className="field-help">
          Several tools share one mount: the primary tool's default TCP is this robot's end-effector TCP. Mounts
          can't be edited here. To choose the primary tool, mark its mount as primary in the Scenario's Deployment,
          then Load the Scenario again.
        </p>
      )}
    </section>
  );
}
