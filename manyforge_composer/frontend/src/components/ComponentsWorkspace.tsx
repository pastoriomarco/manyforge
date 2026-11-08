import { useMemo, useState } from 'react';

import type { ComponentCatalogState } from '../hooks/useComponentCatalog';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { StructuralResourceState } from '../state/structuralResource';
import type { SourceModelProjection } from '../types/api';
import type { ComponentProfileFacts } from '../types/componentCatalog';
import type { CellState, ComponentState } from '../types/components';
import type {
  ManualControlGripperDescriptor,
  ManualControlMotionDescriptor,
} from '../types/robotControl';
import {
  COMPONENT_KIND_LABELS,
  componentFrameTargets,
  componentManagerRows,
  mountParents,
  componentSignals,
  draftComponents,
  draftGripperFrames,
  toolOnFlange,
  type ComponentManagerRow,
  type ComponentManagerTab,
  type ComponentSignal,
} from '../utils/componentManager';
import { toolsWithoutTcp, toolWithoutTcpWarning } from '../utils/componentTcpChoices';
import { AddComponentPanel } from './AddComponentPanel';
import { AddAiModelForm } from './AiModelComponentForm';
import { missingComponentStateText } from '../state/componentPresentation';
import { CellComponentsCard, ComponentStatePanel } from './ComponentStatePanel';
import { ComponentPlacementEditor } from './ComponentPlacementEditor';
import { ComponentTcpChoices } from './ComponentTcpChoices';
import { ComponentTcpsCard } from './ComponentTcpsCard';
import { CHANGE_LABELS, ComponentList } from './ComponentManager';
import { ComponentDetailsCard } from './ComponentDetailsCard';
import { MachineCard } from './MachineCard';
import {
  AddIoDeviceForm,
  DeploymentFocusedEditor,
  geometricContactRobotOptions,
} from './DeploymentFocusedEditor';

/**
 * The Components section: the cell's components on the left (kind, mount,
 * state, Add component), and the selected component's page on the right
 * (state and actions, placement and lane, frames, I/O signals and settings).
 * Every state shown is the pushed `ComponentState`; edits go through the
 * Deployment Draft and the one Preview → Apply path. Jogging stays in Robot
 * Control (linked).
 */
export interface ComponentsWorkspaceProps {
  enabled: boolean;
  controller: DeploymentWorkspaceController;
  /** The pushed component states. */
  cellState: CellState;
  catalog: ComponentCatalogState;
  sourceModel: StructuralResourceState<SourceModelProjection>;
  motionDescriptors: ManualControlMotionDescriptor[];
  gripperDescriptors: ManualControlGripperDescriptor[];
  /** The Program Draft's scene object ids; null without a Program. */
  programSceneIds?: readonly string[] | null;
  sceneResourceIds?: readonly string[];
  /** Components Robot Control addresses (a motion or gripper target). */
  robotControlIds: readonly string[];
  onOpenRobotControl: (componentId: string) => void;
  onOpenDeployment: (tab: ComponentManagerTab) => void;
}

const NO_PROFILES: Readonly<Record<string, ComponentProfileFacts>> = {};

export function sceneResourceChoices(authored: readonly string[] | null, runtime: readonly string[]): string[] {
  return [...new Set([...(authored ?? []), ...runtime])].sort();
}

export function ComponentsWorkspace({
  enabled,
  controller,
  cellState,
  catalog,
  sourceModel,
  motionDescriptors,
  gripperDescriptors,
  programSceneIds = null,
  sceneResourceIds = [],
  robotControlIds,
  onOpenRobotControl,
  onOpenDeployment,
}: ComponentsWorkspaceProps) {
  const sceneChoices = useMemo(() => sceneResourceChoices(programSceneIds, sceneResourceIds),
    [programSceneIds, sceneResourceIds]);
  const document = controller.workspace?.draft.document ?? null;
  const appliedIds = controller.workspace?.managerApplied.activationOrder?.componentIds ?? null;
  const rows = useMemo(
    () => componentManagerRows(document, appliedIds, catalog.catalog?.componentProfiles),
    [appliedIds, catalog.catalog, document],
  );
  const mounts = useMemo(() => mountParents(document), [document]);
  const states = useMemo(() => Object.fromEntries((cellState.components ?? [])
    .map((state): [string, ComponentState] => [state.component_id, state])), [cellState.components]);
  const [selected, setSelected] = useState<string | null>(null);
  const row = rows.find((candidate) => candidate.componentId === selected) ?? rows[0] ?? null;
  const selectedId = row?.componentId ?? null;
  // A machine is one card: selecting its owner or a member shows it.
  const owner = row === null ? null : row.kind === 'machine' ? row
    : rows.find((candidate) => candidate.componentId === row.owner && candidate.kind === 'machine') ?? null;
  const members = owner === null ? [] : rows.filter((candidate) => candidate.owner === owner.componentId);
  const pageProps = (pageRow: ComponentManagerRow) => ({
    row: pageRow,
    enabled,
    controller,
    state: states[pageRow.componentId] ?? null,
    states: cellState.components ?? [],
    statesKnown: cellState.components !== null,
    sourceModel,
    profileFacts: pageRow.profileId === null ? undefined : catalog.catalog?.componentProfiles[pageRow.profileId],
    profiles: catalog.catalog?.componentProfiles ?? NO_PROFILES,
    motionDescriptors,
    gripperDescriptors,
    programSceneIds,
    sceneResourceIds: sceneChoices,
    robotControl: robotControlIds.includes(pageRow.componentId),
    onOpenRobotControl,
    onOpenDeployment,
    onRemoved: () => setSelected(null),
  });

  return (
    <section className="panel dpanel components-workspace" aria-labelledby="components-workspace-title">
      <header className="panel-header">
        <div>
          <p className="eyebrow">Cell</p>
          <h2 id="components-workspace-title">Components</h2>
        </div>
        <button type="button" className="quiet-button" disabled={!enabled}
          onClick={() => { void controller.refresh(); catalog.reload(); }}>
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
          {enabled && <CellComponentsCard states={cellState.components} enabled={enabled} mounts={mounts} />}
          <ComponentList rows={rows} states={states} selectedId={selectedId} onSelect={setSelected} />
          <AddComponentPanel
            controller={controller}
            catalog={catalog.catalog}
            catalogError={catalog.error}
            onAdded={setSelected}
          />
          <CustomComponentAdd controller={controller} sceneResourceIds={sceneChoices}
            geometricContactRobots={geometricContactRobotOptions(motionDescriptors)} />
        </div>
        <div className="components-column">
          {row === null ? (
            <p className="panel-state">Add a component, or open a Scenario, to see its page.</p>
          ) : owner !== null ? (
            <>
              <MachineCard
                owner={owner}
                members={members}
                machine={catalog.catalog?.machines.find((machine) => machine.modelId === owner.profileId)}
                profiles={catalog.catalog?.componentProfiles ?? NO_PROFILES}
                controller={controller}
                states={cellState.components}
                enabled={enabled}
                selectedId={row.componentId}
                onSelect={setSelected}
                onRemoved={() => setSelected(null)}
              />
              {row !== owner && <ComponentPage key={row.componentId} {...pageProps(row)} member />}
            </>
          ) : (
            <ComponentPage key={row.componentId} {...pageProps(row)} />
          )}
        </div>
      </div>
    </section>
  );
}

/**
 * One component: what it is, its live status, frames, I/O and settings. A
 * machine member's page follows its machine card, which shows its status.
 */
function ComponentPage({
  row, enabled, controller, state, states, statesKnown, sourceModel, profileFacts, profiles, motionDescriptors,
  gripperDescriptors, programSceneIds, sceneResourceIds, robotControl, onOpenRobotControl, onOpenDeployment, onRemoved,
  member = false,
}: {
  row: ComponentManagerRow;
  enabled: boolean;
  controller: DeploymentWorkspaceController;
  /** Its pushed state; null while it is not in the cell. */
  state: ComponentState | null;
  /** The cell's pushed states. */
  states: readonly ComponentState[];
  /** False while the cell's states can't be read (shown as unavailable). */
  statesKnown: boolean;
  member?: boolean;
  sourceModel: StructuralResourceState<SourceModelProjection>;
  /** The component type's catalog facts; undefined until the catalog is read. */
  profileFacts: ComponentProfileFacts | undefined;
  /** Every profile's catalog facts (empty until the catalog is read). */
  profiles: Readonly<Record<string, ComponentProfileFacts>>;
  motionDescriptors: ManualControlMotionDescriptor[];
  gripperDescriptors: ManualControlGripperDescriptor[];
  programSceneIds: readonly string[] | null;
  sceneResourceIds: readonly string[];
  robotControl: boolean;
  onOpenRobotControl: (componentId: string) => void;
  onOpenDeployment: (tab: ComponentManagerTab) => void;
  onRemoved: () => void;
}) {
  const id = row.componentId;
  const document = controller.workspace?.draft.document ?? null;
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
          {row.owner !== null && <div><dt>Machine</dt><dd>{row.owner} · {row.role ?? 'member'}{row.active ? '' : ' · deactivated'}</dd></div>}
          {row.mount !== null && <div><dt>Mounted on</dt><dd>{row.mount}</dd></div>}
          {mountedTool !== null && <div><dt>Tool on its flange</dt><dd>{mountedTool}</dd></div>}
          <div><dt>Deployment</dt><dd>{CHANGE_LABELS[row.change]}</dd></div>
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
          {row.change !== 'applied' && (
            <button type="button" onClick={() => onOpenDeployment('changes')}>Review in Deployment Changes</button>
          )}
        </div>
      </section>

      {member ? null : state === null ? (
        <section className="dcard" aria-label={`${id} status`}>
          <h4>Status</h4>
          <p className="muted">{missingComponentStateText(statesKnown)}</p>
        </section>
      ) : (
        <>
          <ComponentStatePanel state={state} enabled={enabled} states={states} />
          <ComponentDetailsCard state={state} />
        </>
      )}
      {row.change !== 'removed' && (
        <ComponentPlacementEditor row={row} controller={controller} profiles={profiles} onRemoved={onRemoved} />
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
          sceneResourceIds={sceneResourceIds}
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
function CustomComponentAdd({ controller, geometricContactRobots, sceneResourceIds }: {
  controller: DeploymentWorkspaceController;
  geometricContactRobots: ReturnType<typeof geometricContactRobotOptions>;
  sceneResourceIds: readonly string[];
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
          geometricContactRobots={geometricContactRobots} sceneResourceIds={sceneResourceIds} onDone={() => setForm(null)} />
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
