import { useEffect, useMemo, useState } from 'react';

import type { CCLCController } from '../hooks/useCCLC';
import type { ComponentCatalogState } from '../hooks/useComponentCatalog';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import {
  componentHealthBadge,
  vendorStatusFacts,
} from '../state/componentLifecyclePresentation';
import type { StructuralResourceState } from '../state/structuralResource';
import type { JsonValue, SourceModelProjection } from '../types/api';
import type { ComponentProfileFacts } from '../types/componentCatalog';
import type { ComposerHeadlessState } from '../types/headless';
import type {
  ManualControlGripperDescriptor,
  ManualControlMotionDescriptor,
} from '../types/robotControl';
import {
  COMPONENT_KIND_LABELS,
  componentFrameTargets,
  componentManagerRows,
  componentSignals,
  draftComponents,
  draftGripperFrames,
  roleFeedback,
  rolesOfComponent,
  toolOnFlange,
  type ComponentManagerRow,
  type ComponentManagerTab,
  type ComponentSignal,
} from '../utils/componentManager';
import { toolsWithoutTcp, toolWithoutTcpWarning } from '../utils/componentTcpChoices';
import { AddComponentPanel } from './AddComponentPanel';
import { AddAiModelForm } from './AiModelComponentForm';
import { CCLCPanelView } from './CCLCPanel';
import { ComponentTcpChoices } from './ComponentTcpChoices';
import { ComponentTcpsCard } from './ComponentTcpsCard';
import { CHANGE_LABELS, ComponentList, type ComponentRowHealth } from './ComponentManager';
import {
  AddIoDeviceForm,
  DeploymentFocusedEditor,
  geometricContactRobotOptions,
} from './DeploymentFocusedEditor';

/**
 * The Components section: the cell's components on the left (kind, mount,
 * lifecycle and health, Add component), and the selected component's page on
 * the right (status, vendor facts, frames, I/O signals and its settings). It
 * reuses the Deployment Draft editors and the CCLC lifecycle surface; the
 * cell-level Draft/Applied, role assignment, Changes and Apply stay under
 * Deployment, and jogging stays in Robot Control (both linked).
 */
export interface ComponentsWorkspaceProps {
  enabled: boolean;
  controller: DeploymentWorkspaceController;
  cclc: CCLCController;
  /** Manager Applied component ids: the components with a lifecycle. */
  lifecycleIds: readonly string[];
  snapshot: ComposerHeadlessState | null;
  catalog: ComponentCatalogState;
  sourceModel: StructuralResourceState<SourceModelProjection>;
  motionDescriptors: ManualControlMotionDescriptor[];
  gripperDescriptors: ManualControlGripperDescriptor[];
  /** The Program Draft's scene object ids; null without a Program. */
  programSceneIds?: readonly string[] | null;
  /** The Program Draft's component roles; null without a Program. */
  programComponentRoles?: Record<string, JsonValue> | null;
  /** The Scenario's Program role assignment; null when none is loaded. */
  programRoleAssignment?: Record<string, JsonValue> | null;
  /** Components Robot Control addresses (a motion or gripper target). */
  robotControlIds: readonly string[];
  onOpenRobotControl: (componentId: string) => void;
  onOpenDeployment: (tab: ComponentManagerTab) => void;
}

const NO_PROFILES: Readonly<Record<string, ComponentProfileFacts>> = {};

function lifecycleAdmission(snapshot: ComposerHeadlessState | null) {
  return {
    mutationAvailable: snapshot?.authoringAdmission.authoringAllowed === true,
    mutationUnavailableReason: snapshot === null
      ? 'Current backend admission is unavailable.'
      : snapshot.programCycle.state === 'terminal_awaiting_reset'
        ? 'Reset the completed Program cycle before changing lifecycle or routes.'
        : 'Lifecycle and route changes require current Idle admission.',
  };
}

export function ComponentsWorkspace({
  enabled,
  controller,
  cclc,
  lifecycleIds,
  snapshot,
  catalog,
  sourceModel,
  motionDescriptors,
  gripperDescriptors,
  programSceneIds = null,
  programComponentRoles = null,
  programRoleAssignment = null,
  robotControlIds,
  onOpenRobotControl,
  onOpenDeployment,
}: ComponentsWorkspaceProps) {
  const document = controller.workspace?.draft.document ?? null;
  const appliedIds = controller.workspace?.managerApplied.activationOrder?.componentIds ?? null;
  const rows = useMemo(
    () => componentManagerRows(document, appliedIds, catalog.catalog?.componentProfiles),
    [appliedIds, catalog.catalog, document],
  );
  const feedback = useMemo(() => roleFeedback(
    programComponentRoles, programRoleAssignment, rows, catalog.catalog?.componentProfiles ?? {},
  ), [catalog.catalog, programComponentRoles, programRoleAssignment, rows]);
  const health = useMemo(() => Object.fromEntries(lifecycleIds.map((id): [string, ComponentRowHealth] => {
    const live = id === cclc.controllableComponentId ? cclc.snapshot : null;
    const observed = cclc.componentHealth[id];
    return [id, componentHealthBadge(live ?? observed?.snapshot ?? null, observed?.error ?? null)];
  })), [cclc.componentHealth, cclc.controllableComponentId, cclc.snapshot, lifecycleIds]);
  const [selected, setSelected] = useState<string | null>(cclc.controllableComponentId || null);
  const row = rows.find((candidate) => candidate.componentId === selected) ?? rows[0] ?? null;
  const selectedId = row?.componentId ?? null;
  // The lifecycle surface follows the page; never while a request is in flight.
  const { busy: cclcBusy, controllableComponentId, setControllableComponentId } = cclc;
  useEffect(() => {
    if (selectedId !== null && !cclcBusy && lifecycleIds.includes(selectedId)
      && controllableComponentId !== selectedId) setControllableComponentId(selectedId);
  }, [cclcBusy, controllableComponentId, lifecycleIds, selectedId, setControllableComponentId]);
  const admission = lifecycleAdmission(snapshot);

  return (
    <section className="panel dpanel components-workspace" aria-labelledby="components-workspace-title">
      <header className="panel-header">
        <div>
          <p className="eyebrow">Cell</p>
          <h2 id="components-workspace-title">Components</h2>
        </div>
        <button type="button" className="quiet-button" disabled={!enabled}
          onClick={() => { void controller.refresh(); catalog.reload(); void cclc.refresh(); }}>
          Refresh
        </button>
      </header>
      {controller.mutationUnavailableReason && (
        <div className="notice severity-info notice-strip">{controller.mutationUnavailableReason}</div>
      )}
      {controller.feedback && (
        <div className="program-feedback" role="status">
          {controller.feedback}
          <button type="button" className="quiet-button" onClick={controller.clearFeedback}>Dismiss</button>
        </div>
      )}
      <div className="components-layout">
        <div className="components-column">
          {enabled && (
            <CCLCPanelView part="cell" enabled={enabled} controller={cclc}
              componentIds={[...lifecycleIds]} {...admission} />
          )}
          <ComponentList rows={rows} health={health} selectedId={selectedId} onSelect={setSelected} />
          <AddComponentPanel
            controller={controller}
            catalog={catalog.catalog}
            catalogError={catalog.error}
            feedback={feedback}
            onAdded={setSelected}
          />
          <CustomComponentAdd controller={controller}
            geometricContactRobots={geometricContactRobotOptions(motionDescriptors)} />
        </div>
        <div className="components-column">
          {row === null ? (
            <p className="panel-state">Add a component, or open a Scenario, to see its page.</p>
          ) : (
            <ComponentPage
              key={row.componentId}
              row={row}
              enabled={enabled}
              controller={controller}
              cclc={cclc}
              lifecycle={lifecycleIds.includes(row.componentId)}
              admission={admission}
              sourceModel={sourceModel}
              profileFacts={row.profileId === null ? undefined : catalog.catalog?.componentProfiles[row.profileId]}
              profiles={catalog.catalog?.componentProfiles ?? NO_PROFILES}
              motionDescriptors={motionDescriptors}
              gripperDescriptors={gripperDescriptors}
              programSceneIds={programSceneIds}
              roles={rolesOfComponent(feedback, row.componentId).map((role) => role.roleId)}
              robotControl={robotControlIds.includes(row.componentId)}
              onOpenRobotControl={onOpenRobotControl}
              onOpenDeployment={onOpenDeployment}
            />
          )}
        </div>
      </div>
    </section>
  );
}

/** One component: what it is, its live status, frames, I/O and settings. */
function ComponentPage({
  row, enabled, controller, cclc, lifecycle, admission, sourceModel, profileFacts, profiles, motionDescriptors,
  gripperDescriptors, programSceneIds, roles, robotControl, onOpenRobotControl, onOpenDeployment,
}: {
  row: ComponentManagerRow;
  enabled: boolean;
  controller: DeploymentWorkspaceController;
  cclc: CCLCController;
  lifecycle: boolean;
  admission: ReturnType<typeof lifecycleAdmission>;
  sourceModel: StructuralResourceState<SourceModelProjection>;
  /** The component type's catalog facts; undefined until the catalog is read. */
  profileFacts: ComponentProfileFacts | undefined;
  /** Every profile's catalog facts (empty until the catalog is read). */
  profiles: Readonly<Record<string, ComponentProfileFacts>>;
  motionDescriptors: ManualControlMotionDescriptor[];
  gripperDescriptors: ManualControlGripperDescriptor[];
  programSceneIds: readonly string[] | null;
  roles: readonly string[];
  robotControl: boolean;
  onOpenRobotControl: (componentId: string) => void;
  onOpenDeployment: (tab: ComponentManagerTab) => void;
}) {
  const id = row.componentId;
  const document = controller.workspace?.draft.document ?? null;
  const snapshot = lifecycle && cclc.controllableComponentId === id ? cclc.snapshot : null;
  const vendor = snapshot === null ? [] : vendorStatusFacts(snapshot);
  const mountedTool = toolOnFlange(document, id);
  const frames = componentFrameTargets(sourceModel.status === 'ready' ? sourceModel.data.targets : null, document, id);
  const gripperFrames = draftGripperFrames(document, id);
  const signals = componentSignals(document, id);
  // B-run-1: a robot without an end-effector TCP, on its page and its tool's.
  const withoutTcp = toolsWithoutTcp(document, profiles)
    .filter((item) => item.robotId === id || item.toolId === id);
  return (
    <>
      <section className="dcard" aria-labelledby="component-page-title">
        <header>
          <h3 id="component-page-title">{row.mount === null ? id : `${id} → ${row.mount}`}</h3>
          <span className="dcard-meta">{row.kind === null ? 'Component' : COMPONENT_KIND_LABELS[row.kind]}</span>
        </header>
        <dl>
          <div><dt>Profile</dt><dd>{row.profileId ?? 'not in the Draft'}</dd></div>
          {row.mount !== null && <div><dt>Mounted on</dt><dd>{row.mount}</dd></div>}
          {mountedTool !== null && <div><dt>Tool on its flange</dt><dd>{mountedTool}</dd></div>}
          <div><dt>Deployment</dt><dd>{CHANGE_LABELS[row.change]}</dd></div>
          <div><dt>Program roles</dt><dd>{roles.length === 0 ? 'none' : roles.join(', ')}</dd></div>
        </dl>
        {withoutTcp.map((item) => (
          <p className="inline-message severity-warning" role="status" key={`${item.robotId}:${item.toolId}`}>
            {toolWithoutTcpWarning(item)}
          </p>
        ))}
        <div className="dactions">
          {robotControl && (
            <button type="button" title="Jog, move or command this component in the viewer's Robot Control."
              onClick={() => onOpenRobotControl(id)}>Open in Robot Control</button>
          )}
          {roles.length > 0 && (
            <button type="button" onClick={() => onOpenDeployment('roles')}>Roles in Deployment</button>
          )}
          {row.change !== 'applied' && (
            <button type="button" onClick={() => onOpenDeployment('changes')}>Review in Deployment Changes</button>
          )}
        </div>
      </section>

      {!lifecycle ? (
        <section className="dcard" aria-label={`${id} status`}>
          <h4>Status</h4>
          <p className="muted">Not running: the next Apply brings it up (Deployment, Changes).</p>
        </section>
      ) : snapshot === null && cclc.controllableComponentId !== id ? (
        <p className="panel-state" role="status">Reading {id} lifecycle…</p>
      ) : (
        <CCLCPanelView part="component" enabled={enabled} controller={cclc} componentIds={[id]} {...admission} />
      )}
      {vendor.length > 0 && (
        <section className="dcard" aria-labelledby="component-vendor-title">
          <h4 id="component-vendor-title">Vendor status</h4>
          <dl>
            {vendor.map((fact) => <div key={fact.label}><dt>{fact.label}</dt><dd>{fact.value}</dd></div>)}
          </dl>
          <p className="field-help">As the driver reports it; Prepare or Recover clears an error left after an emergency stop when the driver supports it.</p>
        </section>
      )}

      <ComponentTcpsCard componentId={id} controller={controller} frames={frames}
        gripperFrames={gripperFrames} motionDescriptors={motionDescriptors} />
      <ComponentTcpChoices componentId={id} controller={controller} profiles={profiles} />

      {row.kind === 'tool' && profileFacts !== undefined && (
        <section className="dcard" aria-labelledby="component-payload-title">
          <h4 id="component-payload-title">Payload</h4>
          <dl aria-label={`${id} payload`}>
            <div><dt>Mass</dt><dd>{profileFacts.payload === undefined
              ? '0 kg (the model defines none)'
              : `${profileFacts.payload.massKg} kg`}</dd></div>
            {profileFacts.payload !== undefined && (
              <div><dt>Centre of mass</dt><dd>{profileFacts.payload.centerOfMassM.join(', ')} m (in the tool's mount)</dd></div>
            )}
          </dl>
          <p className="field-help">Defined by the component's model. It is package data, not editable here; the robot's payload adds it, and the mass of an attached object, while this tool is mounted.</p>
        </section>
      )}

      {(signals.owned.length + signals.uses.length + signals.usedBy.length) > 0 && (
        <section className="dcard" aria-labelledby="component-signals-title">
          <h4 id="component-signals-title">I/O signals</h4>
          <SignalRows title="Declared by this component" signals={signals.owned} own />
          <SignalRows title="Uses" signals={signals.uses} />
          <SignalRows title="Used by" signals={signals.usedBy} />
          <p className="field-help">Live values are in the viewer's Tool &amp; I/O panel; declarations are edited under I/O signals below.</p>
        </section>
      )}

      {row.change === 'removed' ? (
        <p className="panel-state">Removed in the Deployment Draft: it has no settings to edit; it stops at the next Apply.</p>
      ) : controller.loading ? (
        <div className="panel-state" role="status">Reading Deployment Draft…</div>
      ) : (
        <DeploymentFocusedEditor
          controller={controller}
          scope={{ componentId: id }}
          sourceModel={sourceModel}
          motionDescriptors={motionDescriptors}
          gripperDescriptors={gripperDescriptors}
          programSceneIds={programSceneIds}
        />
      )}
    </>
  );
}

function SignalRows({ title, signals, own = false }: {
  title: string;
  signals: readonly ComponentSignal[];
  own?: boolean;
}) {
  if (signals.length === 0) return null;
  return (
    <dl aria-label={title}>
      <div><dt><strong>{title}</strong></dt><dd>{signals.length}</dd></div>
      {signals.map((signal) => (
        <div key={`${signal.componentId}:${signal.signalName}:${signal.detail}`}>
          <dt>{own ? signal.signalName : `${signal.componentId} · ${signal.signalName}`} ({signal.direction})</dt>
          <dd>{signal.detail}</dd>
        </div>
      ))}
    </dl>
  );
}

/** I/O devices and AI models that no catalog entry has, written by hand. */
function CustomComponentAdd({ controller, geometricContactRobots }: {
  controller: DeploymentWorkspaceController;
  geometricContactRobots: ReturnType<typeof geometricContactRobotOptions>;
}) {
  const [form, setForm] = useState<'io_device' | 'ai_model' | null>(null);
  const document = controller.workspace?.draft.document ?? null;
  if (document === null) return null;
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;
  const intent = document.api_version === 'manyforge.deployment_intent.v2';
  return (
    <details className="dcard" open={form !== null}>
      <summary>Custom I/O device or AI model</summary>
      {form === null && (
        <div className="dactions">
          {intent && <button type="button" disabled={disabled} onClick={() => setForm('io_device')}>Add I/O device</button>}
          <button type="button" disabled={disabled} onClick={() => setForm('ai_model')}>Add AI model</button>
        </div>
      )}
      {form === 'io_device' && (
        <AddIoDeviceForm controller={controller} document={document}
          geometricContactRobots={geometricContactRobots} onDone={() => setForm(null)} />
      )}
      {form === 'ai_model' && (
        <AddAiModelForm controller={controller}
          existing={draftComponents(document).map((row) => row.component_id as string)}
          onDone={() => setForm(null)} />
      )}
      <p className="field-help">For a device or endpoint that no example Scenario has. It is added to the Deployment Draft only; nothing runs until Apply.</p>
    </details>
  );
}
