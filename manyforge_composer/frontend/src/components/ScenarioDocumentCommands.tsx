import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type MouseEvent,
  type ReactNode,
} from 'react';

import { api } from '../api/client';
import { sameAuthoringWorkspaceIdentity } from '../hooks/useHeadlessState';
import { managerPublicationMatches } from '../state/scenarioClose';
import type { ProgramWorkspaceController, ScenarioReplacementIdentity, ScenarioCloseSceneProgress } from '../hooks/useProgramWorkspace';
import type {
  ScenarioDocumentEntry,
  ScenarioDocumentIndexResponse,
  ManagerActiveProjection,
} from '../types/api';
import { FolderPicker } from './FolderPicker';
import { OverlayPanel } from './OverlayPanel';
import {
  parentFolder,
  scenarioEntryLabel,
  scenarioNameError,
  scenarioNameTaken,
  scenarioProjectPath,
  scenarioProjectStem,
  SCENARIO_NAME_MAX,
} from '../state/newScenario';
import { errorMessage } from '../state/structuralResource';

type ScenarioAction = 'new' | 'save_as' | 'load' | 'close';
type SaveAsMode = 'project' | 'portable';
interface ReplacementConfirmation {
  kind: 'new' | 'load' | 'close';
  path: string | null;
  name: string;
  confirmDiscard: boolean;
  predecessor: ManagerActiveProjection;
  confirmed: ScenarioReplacementIdentity;
}
interface AfterCloseReplacement extends Omit<ReplacementConfirmation, 'kind' | 'path'> {
  kind: 'new' | 'load' | 'close';
  path: string | null;
  closed: ManagerActiveProjection;
  sceneProgress?: ScenarioCloseSceneProgress;
}

interface ScenarioDocumentCommandsProps {
  controller: ProgramWorkspaceController;
  /** The Deployment Draft differs from its Saved baseline. */
  deploymentDirty?: boolean;
  localInvalidEdit?: boolean;
  /** GUI composition of the existing owner commands; resolves only on exact empty proof. */
  closeScenario?: (expected: ManagerActiveProjection, confirmed?: ScenarioReplacementIdentity, callerSignal?: AbortSignal) => Promise<ManagerActiveProjection | null>;
  closeUnavailableReason?: string | null;
  closeProgress?: string | null;
  /** Robots are moving (Program motion not held, or a manual operation). */
  motionActive?: boolean;
}

function scenarioEntryKey(entry: ScenarioDocumentEntry): string {
  return `${entry.sourceKind}:${entry.path ?? entry.scenarioId ?? entry.name}`;
}

/** One icon-only header command; the tooltip shows at once on hover/focus. */
function ScenarioIconButton({
  label,
  reason,
  disabled,
  onClick,
  children,
}: {
  label: string;
  reason?: string | null;
  disabled: boolean;
  onClick: (event: MouseEvent<HTMLButtonElement>) => void;
  children: ReactNode;
}) {
  return (
    <button
      type="button"
      className="scenario-command"
      aria-label={label}
      data-tooltip={disabled && reason ? `${label}: ${reason}` : label}
      disabled={disabled}
      onClick={onClick}
    >
      <svg viewBox="0 0 16 16" aria-hidden="true">{children}</svg>
    </button>
  );
}

/**
 * The Scenario-level document commands shared by every workspace.
 *
 * Program and Deployment are separately editable Drafts, but the operator
 * saves and loads them together as one Scenario project.
 */
export function ScenarioDocumentCommands({
  controller,
  deploymentDirty = false,
  localInvalidEdit = false,
  closeScenario,
  closeUnavailableReason = null,
  closeProgress = null,
  motionActive = false,
}: ScenarioDocumentCommandsProps) {
  // Document actions wait while robots move (maintainer, 2026-10-16).
  const motionReason = motionActive ? 'Not available while robots are moving. Stop motion first.' : null;
  const {
    state,
    documentUnavailableReason,
    saveScenarioDocument,
    exportScenarioDocument,
    scenarioDocumentPath,
    scenarioDocument,
  } = controller;
  const [action, setAction] = useState<ScenarioAction | null>(null);
  const [saveAsMode, setSaveAsMode] = useState<SaveAsMode>('project');
  const [confirmOverwrite, setConfirmOverwrite] = useState(false);
  const [scenarioIndex, setScenarioIndex] = useState<ScenarioDocumentIndexResponse | null>(null);
  const [scenarioIndexError, setScenarioIndexError] = useState<string | null>(null);
  const [scenarioIndexLoading, setScenarioIndexLoading] = useState(false);
  const [scenarioDirectory, setScenarioDirectory] = useState('');
  const [selectedScenarioKey, setSelectedScenarioKey] = useState('');
  const [saveDirectory, setSaveDirectory] = useState<string | null>(null);
  const [savePickerOpen, setSavePickerOpen] = useState(false);
  const [scenarioName, setScenarioName] = useState('');
  const [scenarioDescription, setScenarioDescription] = useState('');
  const [currentApplied, setCurrentApplied] = useState<ManagerActiveProjection | null>(null);
  const [newError, setNewError] = useState<string | null>(null);
  const [replacementPending, setReplacementPending] = useState(false);
  const replacementInFlight = useRef<false | 'closing' | 'waiting' | 'submitting'>(false);
  const [confirmation, setConfirmation] = useState<ReplacementConfirmation | null>(null);
  const [afterClose, setAfterClose] = useState<AfterCloseReplacement | null>(null);
  const replacementMounted = useRef(true);
  const closeCallerAbort = useRef<AbortController | null>(null);
  useEffect(() => {
    replacementMounted.current = true;
    return () => {
      replacementMounted.current = false; replacementInFlight.current = false;
      closeCallerAbort.current?.abort(new Error('Scenario Close caller was unmounted.'));
    };
  }, []);
  const latestController = useRef(controller);
  useEffect(() => { latestController.current = controller; }, [controller]);
  const [newWorkspaceKey, setNewWorkspaceKey] = useState<string | null>(null);
  const [newParent, setNewParent] = useState<string | null>(null);
  const [newNameTouched, setNewNameTouched] = useState(false);
  const [newPickerOpen, setNewPickerOpen] = useState(false);
  // Entry names of the folder New or Save As writes into, so a clash shows at once.
  const [locationEntries, setLocationEntries] = useState<ReadonlySet<string> | null>(null);
  // Export writes `<exportParent>/<open project folder name>`, keeping the
  // project's one name; only the parent folder is chosen.
  const [exportParent, setExportParent] = useState<string | null>(null);
  const [exportPickerOpen, setExportPickerOpen] = useState(false);
  const [loadPickerOpen, setLoadPickerOpen] = useState(false);
  const scenarioDirectoryRef = useRef('');
  const returnFocus = useRef<HTMLElement | null>(null);
  const workspace = state.workspace;
  const workspaceKey = workspace === null ? null : JSON.stringify(workspace.authoringWorkspace);
  const newConfirmationStale = action === 'new' && newWorkspaceKey !== workspaceKey;
  const managedComponents = currentApplied?.cellGraph.controllable_components;
  const managedComponentNames = Array.isArray(managedComponents)
    ? managedComponents.flatMap((row) => row !== null && typeof row === 'object'
      && !Array.isArray(row) && typeof row.controllable_component_id === 'string'
      ? [row.controllable_component_id] : []) : [];
  const authoring = workspace?.draft.authoring ?? null;
  const busy = replacementPending || state.status === 'loading'
    || (state.status === 'ready' && state.pendingAction !== null);
  const blockReason = localInvalidEdit
    ? 'Review or discard the invalid local field edit first.'
    : busy
      ? 'Another operation is in progress.'
      : documentUnavailableReason;
  // Ordinary document actions require Idle. Internal replacement retains its
  // existing Close path; standalone Close first prepares the exact stopped/idle cycle.
  const blocked = busy || documentUnavailableReason !== null || localInvalidEdit || workspace === null;
  const closeEntryReason = localInvalidEdit
    ? 'Review or discard the invalid local field edit first.'
    : busy ? 'Another operation is in progress.'
      : workspace === null ? 'Program workspace is not loaded.'
        : closeScenario === undefined ? 'Scenario Close is unavailable.'
          : closeUnavailableReason;
  const replacementEntryReason = closeEntryReason === null ? null : blockReason === null ? null : closeEntryReason;
  const hasUnsavedChanges = workspace?.hasUnsavedChanges === true || deploymentDirty;
  const replaceNeedsConfirmation = workspace?.hasUnsavedChanges === true
    || workspace?.hasUnappliedChanges === true
    || deploymentDirty;
  // Plain Save writes back to the open project folder under its one name.
  const openProject = scenarioDocumentPath !== null && scenarioDocument.id !== null
    ? scenarioDocumentPath : null;
  const saveTarget = openProject !== null
    ? {
        directory: parentFolder(openProject),
        name: scenarioProjectStem(openProject),
        description: scenarioDocument.description ?? '',
      }
    : null;
  const selectedScenarioDocument = scenarioIndex?.entries.find(
    (entry) => scenarioEntryKey(entry) === selectedScenarioKey,
  ) ?? null;
  const selectedScenarioPath = selectedScenarioDocument !== null
    && selectedScenarioDocument.displayStatus !== 'unsupported'
    ? selectedScenarioDocument.path
    : null;
  // The index lists one directory, so two projects never share a name in it.
  const selectedScenarioLabel = selectedScenarioDocument === null ? null : scenarioEntryLabel(selectedScenarioDocument);
  const nameError = scenarioNameError(scenarioName);
  const nameTaken = nameError === null && scenarioNameTaken(scenarioName, locationEntries);
  const newFolderError = newParent === null
    ? 'Choose where to create the project.'
    : nameTaken ? 'A Scenario with this name already exists here. Choose another name.' : null;
  const newPath = newParent === null || nameError !== null ? null : scenarioProjectPath(newParent, scenarioName);
  const savePath = saveDirectory === null || nameError !== null ? null : scenarioProjectPath(saveDirectory, scenarioName);
  // Replacing another existing project needs the explicit overwrite choice;
  // saving the open project back to its own folder does not.
  const saveReplacesOther = nameTaken && savePath !== null
    && (openProject === null || savePath !== openProject.replace(/\/+$/, ''));
  const exportName = scenarioDocumentPath === null ? null : scenarioProjectStem(scenarioDocumentPath);
  const exportPath = exportParent === null || exportName === null ? null : scenarioProjectPath(exportParent, exportName);
  const exportTaken = exportName !== null && scenarioNameTaken(exportName, locationEntries);

  const refreshScenarioIndex = useCallback(async (directory?: string) => {
    setScenarioIndexLoading(true);
    setScenarioIndexError(null);
    try {
      const index = await api.scenarioDocuments.list(directory?.trim() || undefined);
      setScenarioIndex(index);
      setScenarioDirectory(index.directory);
      scenarioDirectoryRef.current = index.directory;
      setSelectedScenarioKey((current) => (
        index.entries.some((entry) => scenarioEntryKey(entry) === current)
          ? current
          : ''
      ));
      return index;
    } catch (error: unknown) {
      setScenarioIndexError(
        errorMessage(error, 'Scenario directory could not be read.'),
      );
      return null;
    } finally {
      setScenarioIndexLoading(false);
    }
  }, []);

  useEffect(() => {
    if (action === 'load') {
      void refreshScenarioIndex(scenarioDirectoryRef.current || undefined);
    }
  }, [action, refreshScenarioIndex]);

  // Browse… for Load picks one project: list its folder and select it there,
  // so Load uses the same listed entry as choosing it from the list.
  const chooseLoadProject = async (projectPath: string) => {
    const index = await refreshScenarioIndex(parentFolder(projectPath));
    if (index === null) return;
    const chosen = projectPath.replace(/\/+$/, '');
    const entry = index.entries.find((candidate) => candidate.path?.replace(/\/+$/, '') === chosen);
    if (entry === undefined || entry.displayStatus === 'unsupported') {
      setSelectedScenarioKey('');
      setScenarioIndexError(entry === undefined
        ? 'The chosen project is not a loadable Scenario.' : entry.statusDetail);
      return;
    }
    setSelectedScenarioKey(scenarioEntryKey(entry));
  };

  useEffect(() => {
    if (action !== 'new' && action !== 'load' && action !== 'close') return;
    const abort = new AbortController();
    setCurrentApplied(null);
    setNewError(null);
    void api.manager.status(abort.signal).then((value) => {
      if (abort.signal.aborted) return;
      if (!value.source.available || value.source.freshness !== 'fresh' || !value.status.managerReady) {
        setNewError('Current Applied Deployment cannot be verified.');
      } else setCurrentApplied(value.status.active);
    }).catch((error: unknown) => {
      if (!abort.signal.aborted) setNewError(errorMessage(error, 'Applied Deployment is unavailable.'));
    });
    return () => abort.abort();
  }, [action]);

  // New Scenario: default location next to the current Scenario, otherwise
  // the Scenario folder the backend offers for Load.
  useEffect(() => {
    if (action !== 'new') return;
    if (scenarioDocumentPath !== null) {
      setNewParent(parentFolder(scenarioDocumentPath));
      return;
    }
    void refreshScenarioIndex(scenarioDirectoryRef.current || undefined).then((index) => {
      if (index) setNewParent((current) => current ?? index.directory);
    });
  }, [action, refreshScenarioIndex, scenarioDocumentPath]);

  // Names already present in the chosen location, so a clash shows at once.
  const locationDirectory = action === 'new' ? newParent
    : action === 'save_as' ? (saveAsMode === 'portable' ? exportParent : saveDirectory) : null;
  useEffect(() => {
    if (locationDirectory === null) return undefined;
    const controller = new AbortController();
    setLocationEntries(null);
    api.files.browse('scenario', locationDirectory, controller.signal).then(
      (listing) => setLocationEntries(new Set(listing.entries.map((entry) => entry.name))),
      () => { if (!controller.signal.aborted) setLocationEntries(null); },
    );
    return () => controller.abort();
  }, [locationDirectory]);

  const closeAction = useCallback(() => {
    if (replacementInFlight.current && replacementInFlight.current !== 'waiting') return;
    if (afterClose?.kind === 'close' && (afterClose.sceneProgress?.removed ?? 0) > 0)
      latestController.current.reportCloseCancellation(afterClose.sceneProgress!.removed);
    replacementInFlight.current = false;
    setAfterClose(null);
    setConfirmation(null);
    setReplacementPending(false);
    setAction(null);
    setConfirmOverwrite(false);
  }, [afterClose]);
  const beginAction = (
    next: ScenarioAction,
    event: MouseEvent<HTMLButtonElement>,
  ) => {
    returnFocus.current = event.currentTarget;
    if (next === 'close') setCurrentApplied(null);
    setConfirmOverwrite(false);
    setLoadPickerOpen(false);
    if (next === 'new') {
      setNewWorkspaceKey(workspaceKey);
      setScenarioName('');
      setNewParent(null);
      setNewNameTouched(false);
      setNewPickerOpen(false);
    }
    if (next === 'save_as') {
      // The open project's folder name is its one name; a legacy folder name
      // outside the rule is shown so the operator corrects it.
      setSaveAsMode('project');
      setSavePickerOpen(false);
      setExportParent(null);
      setExportPickerOpen(false);
      setScenarioName(scenarioDocumentPath !== null
        ? scenarioProjectStem(scenarioDocumentPath) : authoring?.program_id ?? '');
      setScenarioDescription(scenarioDocument.description ?? authoring?.description ?? '');
      setSaveDirectory(scenarioDocumentPath !== null ? parentFolder(scenarioDocumentPath) : null);
      if (scenarioDocumentPath === null) {
        void refreshScenarioIndex(scenarioDirectoryRef.current || undefined)
          .then((index) => {
            if (index) setSaveDirectory((current) => current ?? index.directory);
          });
      }
    }
    setAction(next);
  };
  const complete = (operation: () => void) => {
    operation();
    closeAction();
  };

  // New, Close and every Load (also of the Scenario that is currently
  // applied) go through Close first; Load never reapplies over a Scenario.
  const replacesScenario = action === 'new' || action === 'close' || action === 'load';
  const closeUnavailable = replacesScenario
    && (closeScenario === undefined || closeUnavailableReason !== null);
  const replacementReason = newError ?? (replacementPending ? null
    : (replacesScenario ? closeEntryReason : blockReason)
      ?? (closeUnavailable ? 'Scenario Close is unavailable.' : null));
  const selectedIdentityUnknown = action === 'load' && selectedScenarioDocument !== null
    && selectedScenarioDocument.scenarioId === null;
  const completeReplacement = async (kind: 'new' | 'load' | 'close', acknowledged?: ReplacementConfirmation) => {
    if (replacementInFlight.current || currentApplied === null) return;
    if (replacementReason !== null || (kind === 'load' && selectedIdentityUnknown)) {
      if (acknowledged !== undefined) {
        setConfirmation(null);
        setNewError(replacementReason ?? 'Selected Scenario identity cannot be verified.');
      }
      return;
    }
    const expected = acknowledged?.predecessor ?? currentApplied;
    const confirmedPath = kind === 'close' ? null
      : acknowledged?.path ?? (kind === 'new' ? newPath : selectedScenarioPath);
    const confirmedName = acknowledged?.name ?? scenarioName;
    const confirmed = acknowledged?.confirmed ?? latestController.current.scenarioReplacementIdentity;
    if (confirmed === null) {
      setNewError('The shared workspace is not current. No replacement was requested.');
      return;
    }
    if (acknowledged !== undefined) {
      const current = latestController.current.scenarioReplacementIdentity;
      if (current === null || localInvalidEdit
        || !sameAuthoringWorkspaceIdentity(current.authoring, acknowledged.confirmed.authoring)
        || current.receiverEpoch !== acknowledged.confirmed.receiverEpoch
        || current.managerOwnerId !== acknowledged.confirmed.managerOwnerId
        || current.managerOwnerEpoch !== acknowledged.confirmed.managerOwnerEpoch
        || !managerPublicationMatches(current.managerActive, acknowledged.predecessor)) {
        setConfirmation(null);
        setNewError('The confirmed workspace, Draft or Applied publication changed. No replacement was requested.');
        return;
      }
    }
    if (kind !== 'close' && confirmedPath === null) return;
    if ((!expected.canonicalEmpty || replaceNeedsConfirmation) && acknowledged === undefined) {
      setConfirmation({ kind, path: confirmedPath, name: confirmedName, confirmDiscard: true,
        predecessor: expected, confirmed });
      return;
    }
    setConfirmation(null);
    replacementInFlight.current = 'closing';
    setReplacementPending(true);
    setNewError(null);
    let deferredAfterClose = false;
    try {
      if (closeScenario === undefined) throw new Error('Scenario Close is unavailable.');
      const empty = await closeScenario(expected, confirmed,
        (closeCallerAbort.current = new AbortController()).signal);
      if (!replacementMounted.current) return;
      if (empty === null) {
        setNewError('Scenario Close is refused or unresolved; no replacement was requested.');
        return;
      }
      if (!empty.canonicalEmpty) throw new Error('Scenario Close has no current canonical-empty proof.');
      if (kind !== 'close' && confirmedPath === null) return;
      replacementInFlight.current = 'waiting';
      deferredAfterClose = true;
      setAfterClose({ kind, path: confirmedPath, name: confirmedName, confirmDiscard: true,
        predecessor: expected, closed: empty, confirmed });
    } catch (error: unknown) {
      const message = errorMessage(error, 'Scenario replacement is unresolved.');
      if (!replacementMounted.current) latestController.current.reportCloseCancellation(0, message);
      else setNewError(message);
    } finally {
      closeCallerAbort.current = null;
      if (!deferredAfterClose) {
        replacementInFlight.current = false;
        setReplacementPending(false);
      }
    }
  };
  useEffect(() => {
    if (action === 'close' && currentApplied !== null && confirmation === null
      && !replacementPending && replacementReason === null) void completeReplacement('close');
  }, [action, currentApplied, confirmation, replacementPending, replacementReason]);
  useEffect(() => {
    if (afterClose === null || replacementInFlight.current !== 'waiting') return;
    const admission = afterClose.kind === 'close'
      ? controller.scenarioCloseSceneHandoff(afterClose.predecessor, afterClose.closed, afterClose.confirmed, afterClose.sceneProgress ?? null)
      : controller.scenarioCloseHandoff(afterClose.predecessor, afterClose.closed, afterClose.confirmed);
    if (admission.status === 'waiting' && !localInvalidEdit) return;
    // Claim before invoking the ordinary mutation: StrictMode and later owner
    // publications must never send this already-confirmed replacement twice.
    replacementInFlight.current = 'submitting';
    setAfterClose(null);
    if (admission.status === 'refused' || localInvalidEdit) {
      setNewError(localInvalidEdit ? 'The local edit changed. No replacement was sent.'
        : `${admission.message ?? 'Scenario replacement is not admitted.'}${afterClose.kind === 'close'
          ? ` ${afterClose.sceneProgress?.removed ?? 0} Scene object removal(s) confirmed; editors were kept. Nothing was retried.` : ''}`);
      replacementInFlight.current = false;
      setReplacementPending(false);
      return;
    }
    void (async () => {
      let continuingScene = false;
      try {
        if (!replacementMounted.current) return;
        let result;
        if (afterClose.kind === 'close') {
          const step = controller.scenarioCloseSceneHandoff(afterClose.predecessor, afterClose.closed, afterClose.confirmed, afterClose.sceneProgress ?? null);
          if (step.status !== 'ready' || step.progress === undefined) throw new Error(step.message ?? 'Scene clearing is not admitted.');
          if (step.objectId !== null || step.policyResetRequired) {
            const progress = await controller.removeClosedSceneObject(afterClose.predecessor, afterClose.closed, afterClose.confirmed, step.progress);
            if (!replacementMounted.current) return;
            // The successful receipt names one new revision. Wait for that
            // exact publication and current admission before the next step.
            continuingScene = true;
            replacementInFlight.current = 'waiting';
            setAfterClose({ ...afterClose, sceneProgress: progress });
            return;
          }
          result = await controller.clearClosedScenario(afterClose.predecessor, afterClose.closed, afterClose.confirmed, step.progress);
        } else result = afterClose.kind === 'new' && afterClose.path !== null
          ? await controller.newScenarioDocument({ directory: parentFolder(afterClose.path), name: afterClose.name,
            deploymentChoice: 'empty', expectedRetainedBundleId: afterClose.closed.retainedBundleId,
            expectedGraphRevision: afterClose.closed.graphRevision, confirmDiscard: true })
          : afterClose.path !== null
            ? await controller.loadScenarioDocument(afterClose.path, afterClose.confirmDiscard) : null;
        if (result !== null) setAction(null);
        else setNewError('Scenario closed, but the replacement was refused or is unresolved. Review the current backend status; nothing was retried.');
      } catch (error: unknown) {
        const removed = afterClose.sceneProgress?.removed ?? 0;
        setNewError(`${errorMessage(error, 'Scenario replacement is unresolved.')}${afterClose.kind === 'close'
          ? ` ${removed} Scene object removal(s) confirmed; editors were kept. Nothing was retried.` : ''}`);
      } finally {
        if (!continuingScene) {
          replacementInFlight.current = false;
          setReplacementPending(false);
        }
      }
    })();
  }, [afterClose, controller, localInvalidEdit]);
  const afterCloseMessage = afterClose === null ? null
    : (afterClose.kind === 'close'
      ? controller.scenarioCloseSceneHandoff(afterClose.predecessor, afterClose.closed, afterClose.confirmed, afterClose.sceneProgress ?? null)
      : controller.scenarioCloseHandoff(afterClose.predecessor, afterClose.closed, afterClose.confirmed)).message;
  const emptyConfirmation = (confirmation?.predecessor ?? currentApplied)?.canonicalEmpty === true;
  const closeOnlyConfirmation = confirmation?.kind === 'close';
  const clearDraftMessage = 'The cell will be emptied and the Program and Scene editors cleared. Unsaved and unapplied Draft changes will be discarded. Saved files are unchanged.';
  // Same look as the Reset/Execute confirmations: a short lead, the consequences
  // as a list, then what stays physically true.
  const closeWarning = emptyConfirmation
    ? <>
      <p>{closeOnlyConfirmation ? clearDraftMessage : 'Discard the current Draft and continue?'}</p>
      {!closeOnlyConfirmation && replaceNeedsConfirmation
        && <p>Current unsaved and unapplied Draft changes will be discarded.</p>}
    </>
    : <>
      <p>
        {closeOnlyConfirmation ? 'Close the current Scenario:' : 'The current Scenario is closed first:'}
        {managedComponentNames.length > 0 && ` ${managedComponentNames.join(', ')}.`}
      </p>
      <ul>
        <li>a finished, stopped or held Program is Reset first, ending the run;</li>
        <li>carried objects are detached in the Scene at their current pose;</li>
        <li>active targets are retired, open component incidents acknowledged, components disabled and their processes stopped;</li>
        {closeOnlyConfirmation && <li>{clearDraftMessage}</li>}
        {!closeOnlyConfirmation && replaceNeedsConfirmation
          && <li>Current unsaved and unapplied Draft changes will be discarded.</li>}
      </ul>
      <p>Robot positions may be unknown after Close. Robots and physically held parts may remain;
        account for them before using the next cell.</p>
    </>;

  return (
    <>
      <div className="scenario-commands" role="group" aria-label="Scenario">
        <ScenarioIconButton
          label="New"
          reason={motionReason ?? closeEntryReason}
          disabled={motionReason !== null || closeEntryReason !== null}
          onClick={(event) => beginAction('new', event)}
        >
          <path d="M3.5 2.5h5.75L12.5 5.75v7.75a.5.5 0 0 1-.5.5H3.5a.5.5 0 0 1-.5-.5V3a.5.5 0 0 1 .5-.5z" />
          <path d="M9.25 2.5v3.25h3.25M7.75 7.5v4.5M5.5 9.75h4.5" />
        </ScenarioIconButton>
        <ScenarioIconButton
          label={saveTarget ? 'Save Scenario' : 'Save Scenario (choose a project folder)'}
          reason={motionReason ?? blockReason ?? 'Scenario already matches Saved.'}
          disabled={motionReason !== null || blocked || (saveTarget !== null && !hasUnsavedChanges)}
          onClick={(event) => {
            if (saveTarget === null) {
              beginAction('save_as', event);
              return;
            }
            saveScenarioDocument({ ...saveTarget, confirmOverwrite: false });
          }}
        >
          <path d="M3 2.5h8L13.5 5v8.5a.5.5 0 0 1-.5.5H3a.5.5 0 0 1-.5-.5v-10a.5.5 0 0 1 .5-.5z" />
          <path d="M5 2.5V6h5.5V2.5M5 14v-4.5h6V14" />
        </ScenarioIconButton>
        <ScenarioIconButton
          label="Save Scenario as"
          reason={motionReason ?? blockReason}
          disabled={motionReason !== null || blocked}
          onClick={(event) => beginAction('save_as', event)}
        >
          <path d="M3 2.5h6.5L12 5v3" />
          <path d="M3 2.5a.5.5 0 0 0-.5.5v10a.5.5 0 0 0 .5.5h4.5" />
          <path d="M5 2.5V5.5h4V2.5" />
          <path d="M13.7 9.6l-3.9 3.9-1.4.4.4-1.4 3.9-3.9a1 1 0 0 1 1 1z" />
        </ScenarioIconButton>
        <ScenarioIconButton
          label="Load Scenario"
          reason={motionReason ?? replacementEntryReason}
          disabled={motionReason !== null || replacementEntryReason !== null}
          onClick={(event) => beginAction('load', event)}
        >
          <path d="M2 4.5a.5.5 0 0 1 .5-.5H6l1.5 1.5h6a.5.5 0 0 1 .5.5V7H2z" />
          <path d="M2 7h12.5l-1.6 5.6a.5.5 0 0 1-.5.4H3.3a.5.5 0 0 1-.5-.4L2 7z" />
        </ScenarioIconButton>
        {/* Temporary for testing: remove before release once New opens an untitled Scenario (plan hardening item “Untitled New”) */}
        <ScenarioIconButton label="Close Scenario" reason={motionReason ?? closeEntryReason} disabled={motionReason !== null || closeEntryReason !== null}
          onClick={(event) => beginAction('close', event)}>
          <path d="M3 3l10 10M13 3L3 13" />
        </ScenarioIconButton>
      </div>

      <OverlayPanel
        open={action !== null && confirmation === null}
        title={action === 'new' ? 'New Scenario' : action === 'save_as' ? 'Save Scenario as' : action === 'close' ? 'Close Scenario' : 'Load Scenario'}
        titleId="scenario-action-dialog-title"
        compact={false}
        onClose={closeAction}
        returnFocusRef={returnFocus}
      >
        {action === 'new' && (
          <div className="dpanel new-scenario-dialog">
            <p className="new-scenario-intro">
              Close the current Scenario and start a new saved one with an empty
              Program and Scene. The current Scenario&apos;s saved files are not changed.
            </p>
            <section className="dcard" aria-label="Scenario">
              <h4>Scenario</h4>
              <label className="stacked">
                <span>Name <span className="required-mark" aria-hidden="true">*</span></span>
                <input
                  value={scenarioName}
                  disabled={replacementPending}
                  required
                  aria-required="true"
                  aria-invalid={newNameTouched && nameError !== null}
                  aria-describedby="new-scenario-name-help"
                  maxLength={SCENARIO_NAME_MAX}
                  placeholder="e.g. lite6_pick_and_place"
                  spellCheck={false}
                  autoCapitalize="off"
                  autoFocus
                  onChange={(event) => {
                    setScenarioName(event.target.value);
                    setNewNameTouched(true);
                  }}
                />
                <small
                  id="new-scenario-name-help"
                  className={newNameTouched && nameError !== null ? 'field-error' : 'field-hint'}
                  role={newNameTouched && nameError !== null ? 'alert' : undefined}
                >
                  {newNameTouched && nameError !== null ? nameError : 'Letters, digits, “_” and “-” only, no spaces or dots. Also the project folder name.'}
                </small>
              </label>
              <div className="new-scenario-location">
                <span>Location</span>
                <code title={newParent ?? undefined}>{newParent ?? 'Reading default location…'}</code>
                <button type="button" disabled={replacementPending} onClick={() => setNewPickerOpen((open) => !open)} aria-expanded={newPickerOpen}>
                  Browse…
                </button>
              </div>
              {newPickerOpen && (
                <FolderPicker
                  initialPath={newParent}
                  onCancel={() => setNewPickerOpen(false)}
                  onChoose={(path) => {
                    setNewParent(path);
                    setNewPickerOpen(false);
                  }}
                />
              )}
              <small
                id="new-scenario-folder-help"
                className={newFolderError !== null && newParent !== null ? 'field-error' : 'field-hint'}
                role={newFolderError !== null && newParent !== null ? 'alert' : undefined}
              >
                {newFolderError !== null && newParent !== null
                  ? newFolderError
                  : newPath !== null ? <>Will be created as <code>{newPath}</code></> : 'A new project folder named after the Scenario.'}
              </small>
            </section>
            <p>The new Scenario starts with an empty Deployment. Components are configured
              without activation when you later Apply a Deployment.</p>
            <p className="new-scenario-note">
              The old Scenario&apos;s objects are cleared from the Scene, including any
              held by a gripper. Remove physically held parts yourself.
            </p>
            {newConfirmationStale && <p className="inline-message severity-warning" role="status">The workspace changed. Close and reopen New to review the current state.</p>}
            {replacementReason && <p className="inline-message severity-error" role="status">{replacementReason}</p>}
            {afterCloseMessage && <p role="status">{afterCloseMessage}</p>}
            <div className="dactions new-scenario-actions">
              <button type="button" onClick={closeAction} disabled={replacementPending && afterClose === null}>Cancel</button>
              <button type="button" className="primary-action"
                disabled={busy || newConfirmationStale || currentApplied === null
                  || replacementReason !== null || nameError !== null || newFolderError !== null || newPath === null}
                title={replacementReason ?? nameError ?? newFolderError ?? undefined}
                onClick={() => { void completeReplacement('new'); }}>
                Create Scenario
              </button>
            </div>
          </div>
        )}

        {action === 'save_as' && (
          <div className="dpanel new-scenario-dialog scenario-document-dialog">
            <div className="segmented-control scenario-save-mode" role="radiogroup" aria-label="Save format">
              <button type="button" role="radio" aria-checked={saveAsMode === 'project'} className={saveAsMode === 'project' ? 'active' : ''} onClick={() => setSaveAsMode('project')}>Project</button>
              <button type="button" role="radio" aria-checked={saveAsMode === 'portable'} className={saveAsMode === 'portable' ? 'active' : ''} onClick={() => setSaveAsMode('portable')}>Portable copy</button>
            </div>
            {saveAsMode === 'project' ? (
              <>
                <p className="new-scenario-intro">
                  Save the Program, Scene, Deployment, assignment and overrides as one project folder, and continue working on it.
                </p>
                <section className="dcard" aria-label="Scenario">
                  <h4>Scenario</h4>
                  <label className="stacked">
                    <span>Name <span className="required-mark" aria-hidden="true">*</span></span>
                    <input
                      value={scenarioName}
                      required
                      aria-required="true"
                      aria-invalid={nameError !== null}
                      aria-describedby="save-scenario-name-help"
                      maxLength={SCENARIO_NAME_MAX}
                      spellCheck={false}
                      autoCapitalize="off"
                      onChange={(event) => setScenarioName(event.target.value)}
                    />
                    <small
                      id="save-scenario-name-help"
                      className={scenarioName !== '' && nameError !== null ? 'field-error' : 'field-hint'}
                      role={scenarioName !== '' && nameError !== null ? 'alert' : undefined}
                    >
                      {scenarioName !== '' && nameError !== null ? nameError : 'Letters, digits, “_” and “-” only, no spaces or dots. Also the project folder name.'}
                    </small>
                  </label>
                  <div className="new-scenario-location">
                    <span>Location</span>
                    <code title={saveDirectory ?? undefined}>{saveDirectory ?? 'Reading default location…'}</code>
                    <button type="button" onClick={() => setSavePickerOpen((open) => !open)} aria-expanded={savePickerOpen}>
                      Browse…
                    </button>
                  </div>
                  {savePickerOpen && (
                    <FolderPicker
                      initialPath={saveDirectory}
                      onCancel={() => setSavePickerOpen(false)}
                      onChoose={(path) => {
                        setSaveDirectory(path);
                        setSavePickerOpen(false);
                      }}
                    />
                  )}
                  <small className={saveReplacesOther && !confirmOverwrite ? 'field-error' : 'field-hint'}>
                    {savePath === null ? 'Saved as a project folder named after the Scenario.'
                      : saveReplacesOther
                        ? <>A project <code>{savePath}</code> already exists. Allow replacing it to overwrite it.</>
                        : <>Saved as <code>{savePath}</code></>}
                  </small>
                  <label className="confirmation-check">
                    <input type="checkbox" checked={confirmOverwrite} onChange={(event) => setConfirmOverwrite(event.target.checked)} />
                    Allow replacing an existing project
                  </label>
                  <label className="stacked">
                    <span>Description</span>
                    <input value={scenarioDescription} onChange={(event) => setScenarioDescription(event.target.value)} />
                    <small className="field-hint">Free text, e.g. a readable title. Robot meshes and other referenced sources stay where they are.</small>
                  </label>
                </section>
                <div className="dactions new-scenario-actions">
                  <button type="button" onClick={closeAction}>Cancel</button>
                  <button
                    type="button"
                    className="primary-action"
                    disabled={blocked || saveDirectory === null || nameError !== null
                      || (saveReplacesOther && !confirmOverwrite)}
                    title={blockReason ?? nameError ?? undefined}
                    onClick={() => {
                      if (saveDirectory === null || nameError !== null) return;
                      complete(() => saveScenarioDocument({
                        directory: saveDirectory,
                        name: scenarioName,
                        description: scenarioDescription.trim(),
                        confirmOverwrite,
                      }));
                    }}
                  >
                    Save Scenario
                  </button>
                </div>
              </>
            ) : (
              <>
                <p className="new-scenario-intro">
                  Write a self-contained copy of the saved project, including the robot sources and Scene assets it references. The open project is not changed.
                </p>
                {scenarioDocumentPath === null && (
                  <p className="inline-message severity-warning">Save the Scenario as a project first.</p>
                )}
                <section className="dcard" aria-label="Destination">
                  <h4>Destination</h4>
                  <div className="new-scenario-location">
                    <span>Location</span>
                    <code title={exportParent ?? undefined}>{exportParent ?? 'Choose where to write the copy.'}</code>
                    <button type="button" disabled={scenarioDocumentPath === null} onClick={() => setExportPickerOpen((open) => !open)} aria-expanded={exportPickerOpen}>
                      Browse…
                    </button>
                  </div>
                  {exportPickerOpen && (
                    <FolderPicker
                      initialPath={exportParent ?? (scenarioDocumentPath !== null ? parentFolder(scenarioDocumentPath) : null)}
                      onCancel={() => setExportPickerOpen(false)}
                      onChoose={(path) => {
                        setExportParent(path);
                        setExportPickerOpen(false);
                      }}
                    />
                  )}
                  <small className={exportTaken ? 'field-error' : 'field-hint'} role={exportTaken ? 'alert' : undefined}>
                    {exportPath === null ? 'The copy is a new project folder with the same name.'
                      : exportTaken
                        ? <>A project <code>{exportPath}</code> already exists. Choose another location.</>
                        : <>Written as <code>{exportPath}</code></>}
                  </small>
                </section>
                <div className="dactions new-scenario-actions">
                  <button type="button" onClick={closeAction}>Cancel</button>
                  <button
                    type="button"
                    className="primary-action"
                    disabled={busy || exportPath === null || exportTaken}
                    onClick={() => {
                      if (exportPath === null) return;
                      complete(() => exportScenarioDocument(exportPath));
                    }}
                  >
                    Export copy
                  </button>
                </div>
              </>
            )}
          </div>
        )}

        {action === 'load' && (
          <div className="dpanel new-scenario-dialog scenario-document-dialog">
            <p className="new-scenario-intro">
              Load replaces the current Scenario and applies its Deployment, Scene and Program. It does not start motion.
            </p>
            <section className="dcard" aria-label="Location">
              <h4>Location</h4>
              <div className="scenario-directory-row">
                <label className="stacked">
                  <span>Scenario directory</span>
                  <input value={scenarioDirectory} disabled={replacementPending} onChange={(event) => {
                    setScenarioDirectory(event.target.value);
                    scenarioDirectoryRef.current = event.target.value;
                  }} />
                </label>
                <button type="button" disabled={replacementPending || scenarioIndexLoading} onClick={() => void refreshScenarioIndex(scenarioDirectory)}>
                  {scenarioIndexLoading ? 'Reading…' : 'Refresh'}
                </button>
                <button type="button" disabled={replacementPending} onClick={() => setLoadPickerOpen((open) => !open)} aria-expanded={loadPickerOpen}>
                  Browse…
                </button>
              </div>
              {loadPickerOpen && (
                <FolderPicker
                  chooseProject
                  initialPath={scenarioDirectory || null}
                  onCancel={() => setLoadPickerOpen(false)}
                  onChoose={(path) => {
                    setLoadPickerOpen(false);
                    void chooseLoadProject(path);
                  }}
                />
              )}
              {scenarioIndexError && <div className="document-apply-blocked" role="alert">{scenarioIndexError}</div>}
            </section>
            <section className="dcard" aria-label="Scenario">
              <h4>Scenario</h4>
              <label className="stacked">
                <span>Available Scenario</span>
                <select
                  value={selectedScenarioKey}
                  disabled={replacementPending || scenarioIndexLoading || scenarioIndex === null}
                  onChange={(event) => setSelectedScenarioKey(event.target.value)}
                >
                  <option value="">Select a Scenario</option>
                  {scenarioIndex?.entries.map((entry) => (
                    <option
                      key={scenarioEntryKey(entry)}
                      value={scenarioEntryKey(entry)}
                      disabled={entry.displayStatus === 'unsupported'}
                      title={entry.statusDetail}
                    >
                      {scenarioEntryLabel(entry)}
                    </option>
                  ))}
                </select>
                <small className="field-hint">The list shows saved metadata, not current readiness. The selected Scenario is validated when you load it.</small>
              </label>
              {selectedScenarioDocument && (
                <div className={`scenario-status scenario-status-${selectedScenarioDocument.displayStatus}`}>
                  <strong>{selectedScenarioLabel}</strong>
                  <span>{selectedScenarioDocument.statusDetail}</span>
                  {selectedScenarioDocument.description && (
                    <span>{selectedScenarioDocument.description}</span>
                  )}
                </div>
              )}
            </section>
            {replacementReason && <p className="inline-message severity-error" role="status">{replacementReason}</p>}
            {afterCloseMessage && <p role="status">{afterCloseMessage}</p>}
            {selectedIdentityUnknown && <p role="status">Selected Scenario identity cannot be verified.</p>}
            <div className="dactions new-scenario-actions">
              <button type="button" onClick={closeAction} disabled={replacementPending && afterClose === null}>Cancel</button>
              <button
                type="button"
                className="primary-action"
                disabled={busy || currentApplied === null || replacementReason !== null || selectedIdentityUnknown || selectedScenarioPath === null}
                title={replacementReason ?? undefined}
                onClick={() => {
                  if (selectedScenarioPath === null) return;
                  void completeReplacement('load');
                }}
              >
                Load Scenario
              </button>
            </div>
          </div>
        )}
        {action === 'close' && <div className="document-dialog">
          {currentApplied === null && <p role="status">Checking the current Scenario…</p>}
          {replacementReason && <p className="inline-message severity-error" role="status">{replacementReason}</p>}
          {afterCloseMessage && <p role="status">{afterCloseMessage}</p>}
          <div className="dialog-actions">
            <button type="button" onClick={closeAction} disabled={replacementPending && afterClose === null}>Cancel</button>
          </div>
        </div>}
        {replacementPending && afterClose === null && <p className="scenario-replacement-status" role="status">{action === 'close' && replacementInFlight.current === 'submitting'
          ? 'Clearing the logical Scene before emptying the editors…' : closeProgress ?? 'Scenario replacement is in progress.'}</p>}

      </OverlayPanel>
      <OverlayPanel open={confirmation !== null}
        title="Close Scenario?"
        titleId="scenario-replacement-confirmation-title" kind="dialog" compact
        onClose={() => { if (confirmation?.kind === 'close') closeAction(); else setConfirmation(null); }}>
        <div className="dialog-copy transport-confirmation">
          {closeWarning}
          <div className="dialog-actions">
            <button type="button" onClick={() => { if (confirmation?.kind === 'close') closeAction(); else setConfirmation(null); }}>Cancel</button>
            <button type="button" className="danger-button" onClick={() => {
              if (confirmation !== null) void completeReplacement(confirmation.kind, confirmation);
            }}>{confirmation?.kind === 'close' ? 'Close Scenario' : 'Close and continue'}</button>
          </div>
        </div>
      </OverlayPanel>
    </>
  );
}
