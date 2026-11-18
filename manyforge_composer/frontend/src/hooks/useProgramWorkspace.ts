import {
  useCallback,
  useEffect,
  useMemo,
  useReducer,
  useRef,
  useState,
} from 'react';

import { api, authoringRefusalMessage, connectionLost, currentScenarioText, isStaleWorkspaceError, KnownOutcomeError, savedNotAdoptedMessage } from '../api/client';
import { applyResultText } from '../components/ApplyImpact';
import { errorMessage } from '../state/structuralResource';
import { cellApplyErrorText } from './useDeploymentWorkspace';
import type { ManagerApplyIdentity } from '../state/managerApply';
import type { AuthoringCommand } from '../state/busyOperation';
import {
  applyRowRefusal, awaitCommandEnd, CommandEndedError, withApplyStep,
} from '../state/commandRows';
import { applyResultOfRow } from '../api/parsers/commandRows';
import { codedErrorText } from '../state/componentPresentation';
import { useCommandRowWaiter } from './useCommandRowWaiter';
import {
  programLocalFenceFromAdmission,
  publicationSupersedesProgram,
  programActionUnavailableReason,
  scenarioLocalFence,
  withSharedProgramAuthoring,
  type PublishedAuthoringWorkspace,
} from '../state/authoringWorkspace';
import {
  INITIAL_PROGRAM_WORKSPACE_STATE,
  programWorkspaceReducer,
  type ProgramWorkspaceActionName,
} from '../state/programWorkspace';
import type {
  ApplyCommandRow,
  DraftMutationResponse,
  SaveScenarioDocumentInput,
  JsonValue,
  LocalWorkspaceFenceRequest,
  ProgramNodeRecoveryPolicy,
  ProgramOperatorStopPolicy,
  ProgramTreeNode,
  ProgramWorkspaceResponse,
  ProgramWorkspaceTransitionStatus,
  ScenarioDocumentFenceRequest,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { AuthoringWorkspaceController } from './useAuthoringWorkspace';
import { displayApplyId, newRequestKey, type ApplicationStartId } from '../api/applicationIdentity';
import { programRuntimeWakeKey } from './useProgramRuntime';

/** The exact Program Draft content a caller acts on (a confirmed Apply). */
export interface ExpectedProgramDraft {
  draftId: string;
  draftRevision: number;
  programRevision: string;
}

/**
 * The installed Draft is not the content the caller confirmed: nothing was
 * sent. Admission and workspace fences are refreshed from the current
 * publication, but the Draft identity and content never are.
 */
export class ProgramDraftChangedError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ProgramDraftChangedError';
  }
}

/** Why the published Draft is not `expected`, or null when it is. */
export function programDraftMismatch(
  published: PublishedAuthoringWorkspace | null,
  expected: ExpectedProgramDraft,
): string | null {
  if (published === null) return 'The shared Draft publication is not available.';
  const draft = published.snapshot.program.draft;
  if (
    published.identity.programDraftId !== expected.draftId
    || draft.draftId !== expected.draftId
  ) return 'The Program Draft was replaced.';
  if (
    published.identity.programDraftRevision !== expected.draftRevision
    || draft.draftRevision !== expected.draftRevision
    || draft.programRevision !== expected.programRevision
  ) {
    return `The Program Draft changed (revision ${draft.draftRevision}, expected ${expected.draftRevision}).`;
  }
  return null;
}

export interface ProgramWorkspaceRunResult {
  workspace: ProgramWorkspaceResponse;
  message: string | null;
  transitionStatus: ProgramWorkspaceTransitionStatus | null;
  /** Exact Manager operation accepted by this request, when one was. */
  acceptedApply?: ManagerApplyIdentity | null;
}

function acceptedApplyIdentity(
  applyId: unknown,
  applyGeneration: unknown,
): ManagerApplyIdentity | null {
  return typeof applyId === 'number' && Number.isSafeInteger(applyId) && applyId > 0
    && typeof applyGeneration === 'number' && Number.isSafeInteger(applyGeneration)
    ? { applyId, applyGeneration }
    : null;
}

function workspaceResult(
  workspace: ProgramWorkspaceResponse,
  message: string | null,
): ProgramWorkspaceRunResult {
  return { workspace, message, transitionStatus: null };
}

/**
 * What a Program Apply or Revert that ended succeeded did: the Manager Apply
 * its `applying` step started (followed under Activity and by
 * Reset-and-Apply), or none when Applied already matched.
 */
export function programApplyCommandResult(
  action: 'apply' | 'revert',
  row: ApplyCommandRow,
  workspace: ProgramWorkspaceResponse,
  applicationStartId: ApplicationStartId | null = null,
): ProgramWorkspaceRunResult {
  const source = row.stepSource;
  if (source !== null && source.status === 'manager_apply') {
    return {
      workspace,
      message: `${action === 'apply' ? 'Apply' : 'Revert'} accepted (${displayApplyId(applicationStartId, source.applyId)}); configuration progress is shown under Activity.`,
      transitionStatus: 'accepted',
      acceptedApply: acceptedApplyIdentity(source.applyId, source.applyGeneration),
    };
  }
  return action === 'revert'
    ? {
      workspace,
      message: 'Saved already matched Applied; Draft now matches Saved. No owner Apply was issued.',
      transitionStatus: 'reverted',
    }
    : {
      workspace,
      message: 'Apply completed; the owner-issued Applied state was replaced.',
      transitionStatus: 'applied',
    };
}

/** The operation each Program workspace action names while it runs. */
const PROGRAM_ACTION_COMMANDS: Record<Exclude<ProgramWorkspaceActionName, 'refresh'>, AuthoringCommand> = {
  mutate: 'edit',
  open_draft: 'open_draft',
  apply: 'apply',
  discard: 'discard',
  discard_scenario: 'discard',
  revert: 'revert',
  load: 'load',
  load_scenario_document: 'load',
  new_scenario_document: 'new',
  export_scenario_document: 'export',
  save: 'save',
  save_as: 'save_as',
  save_scenario_document: 'save',
};

export function useProgramWorkspace(
  snapshot: ComposerHeadlessState | null,
  authoring: AuthoringWorkspaceController,
  /** Called after a successful Scenario New or Load reply. */
  onScenarioOpened?: (kind: 'new' | 'load') => void,
) {
  const [state, dispatch] = useReducer(
    programWorkspaceReducer,
    INITIAL_PROGRAM_WORKSPACE_STATE,
  );
  const [acceptedApply, setAcceptedApply] = useState<ManagerApplyIdentity | null>(null);
  const applyRows = useCommandRowWaiter(snapshot?.applyCommands ?? null, snapshot?.applicationStartId ?? null);
  /**
   * Program Apply or Revert: answered at acceptance, its outcome is the row's
   * end (OD-214); the workspace is read once it ended.
   */
  const programApplyCommand = useCallback(async (
    action: 'apply' | 'revert',
    send: () => Promise<ApplyCommandRow>,
    applicationStartId: ApplicationStartId,
  ): Promise<ProgramWorkspaceRunResult> => {
    const ended = await awaitCommandEnd(await send(), applyRows);
    // A new application start keeps no row of it: the current state is read.
    if (ended === null) {
      return { workspace: await api.programWorkspace.get(), message: null, transitionStatus: null };
    }
    if (ended.end?.outcome !== 'succeeded') throw new CommandEndedError(ended);
    return programApplyCommandResult(action, ended, await api.programWorkspace.get(), applicationStartId);
  }, [applyRows]);
  const [programDraftInputResetEpoch, setProgramDraftInputResetEpoch] = useState(0);
  const [scenarioDraftInputResetEpoch, setScenarioDraftInputResetEpoch] = useState(0);
  const scenarioOpened = useRef(onScenarioOpened);
  useEffect(() => { scenarioOpened.current = onScenarioOpened; }, [onScenarioOpened]);
  const generation = useRef(0);
  const mounted = useRef(true);
  const workspaceRead = useRef<Promise<ProgramWorkspaceResponse> | null>(null);
  const workspaceReadGeneration = useRef<number | null>(null);
  const projectionIdentityRead = useRef<string | null>(null);
  const appliedIdentityRead = useRef<string | null>(null);
  // Authoring identity, both Draft projections' fences and the admission gate
  // come from the one shared publication. This hook contributes the
  // Applied/applicability projection it reads from the Program workspace route,
  // which owns runtime truth. A Draft edit (and Discard) is also admitted
  // while a held run has all its motion stopped; every other document action
  // (Save, Open, New, Load, Revert, the ordinary Apply) requires Idle.
  const mutationUnavailableReason = state.workspace === null
    ? 'Program workspace is not loaded.'
    : authoring.editUnavailableReason;
  const mutationAvailable = mutationUnavailableReason === null;
  const documentUnavailableReason = state.workspace === null
    ? 'Program workspace is not loaded.'
    : authoring.unavailableReason;
  // New and Load: a finished or stopped run is the server's to decide.
  const scenarioReplacementUnavailableReason = state.workspace === null
    ? 'Program workspace is not loaded.'
    : authoring.replaceUnavailableReason;
  const admissionObserved = snapshot !== null;
  const published = authoring.published;
  const project = authoring.project;
  // The Draft content the next fence would act upon. A mutation reply can be
  // ahead of the publication; a caller that names exact content waits for it.
  const fenceDraftId = published?.identity.programDraftId ?? null;
  const fenceDraftRevision = published?.identity.programDraftRevision ?? null;
  const fenceProgramRevision = published?.snapshot.program.draft.programRevision ?? null;
  const fenceDraft = useMemo((): {
    draftId: string; draftRevision: number; programRevision: string | null;
  } | null => (
    fenceDraftId === null || fenceDraftRevision === null
      ? null
      : { draftId: fenceDraftId, draftRevision: fenceDraftRevision, programRevision: fenceProgramRevision }
  ), [fenceDraftId, fenceDraftRevision, fenceProgramRevision]);

  const readWorkspace = useCallback((requestGeneration: number) => {
    if (
      workspaceRead.current !== null
      && workspaceReadGeneration.current === requestGeneration
    ) {
      return workspaceRead.current;
    }
    const request = api.programWorkspace.get();
    workspaceRead.current = request;
    workspaceReadGeneration.current = requestGeneration;
    void request.then(
      () => {
        if (workspaceRead.current === request) {
          workspaceRead.current = null;
          workspaceReadGeneration.current = null;
        }
      },
      () => {
        if (workspaceRead.current === request) {
          workspaceRead.current = null;
          workspaceReadGeneration.current = null;
        }
      },
    );
    return request;
  }, []);

  const { beginMutation, finishMutation, refresh: refreshAuthoring } = authoring;
  const run = useCallback(async (
    action: ProgramWorkspaceActionName,
    request: () => Promise<ProgramWorkspaceRunResult>,
    onAuthoringRefusal?: (message: string, failed?: boolean) => void,
  ): Promise<ProgramWorkspaceRunResult | null> => {
    // A Scenario Discard is a Draft edit; whether any Scenario document has
    // unapplied changes is decided by its route and by the backend, never by
    // the Program-only Discard gate below.
    const edit = action === 'mutate' || action === 'discard' || action === 'discard_scenario';
    const replace = action === 'new_scenario_document' || action === 'load_scenario_document';
    if ((action === 'apply' || action === 'revert' || action === 'discard')
      && programActionUnavailableReason(
        state.workspace, action, edit ? mutationUnavailableReason : documentUnavailableReason,
      ) !== null) return null;
    if (action !== 'refresh'
      && !beginMutation(edit ? 'edit' : replace ? 'replace' : 'document', PROGRAM_ACTION_COMMANDS[action])) {
      // The client gate refused before any request: its reason is the refusal.
      if (replace) onAuthoringRefusal?.(scenarioReplacementUnavailableReason ?? 'Another operation is in progress.');
      return null;
    }
    const current = generation.current + 1;
    generation.current = current;
    dispatch({ type: 'request_started', generation: current, action });
    try {
      const result = await request();
      if (action !== 'refresh') finishMutation(result.acceptedApply ?? null);
      if (mounted.current && result.acceptedApply) {
        setAcceptedApply(result.acceptedApply);
      }
      if (mounted.current) {
        dispatch({
          type: 'request_succeeded',
          generation: current,
          workspace: result.workspace,
          message: result.message,
          transitionStatus: result.transitionStatus,
        });
        const acceptedProgramRevert = action === 'revert'
          && (result.transitionStatus === 'accepted' || result.transitionStatus === 'reverted');
        if (action === 'discard_scenario' || action === 'load' || action === 'load_scenario_document'
          || action === 'new_scenario_document' || acceptedProgramRevert) {
          // The owner response carries the commit_saved Draft post-image. An
          // accepted Manager Apply may still be progressing after this rebase.
          setScenarioDraftInputResetEpoch((epoch) => epoch + 1);
        } else if (action === 'discard' || action === 'open_draft') {
          setProgramDraftInputResetEpoch((epoch) => epoch + 1);
        }
      }
      return result;
    } catch (error: unknown) {
      if (action !== 'refresh') finishMutation(null, error);
      // A stale fence is also refused before any effect, but it needs the
      // refresh below, so it is never handled as a plain refusal.
      // A Program Apply/Revert row that ended refused had no effect: its
      // reason shows where the HTTP refusal of that check showed.
      const refusal = action === 'refresh' || isStaleWorkspaceError(error)
        ? null : authoringRefusalMessage(error)
          ?? (error instanceof CommandEndedError && error.row.end?.outcome === 'refused'
            ? error.message : null);
      if (refusal !== null) {
        if (mounted.current) dispatch({ type: 'request_refused', generation: current, message: refusal });
        onAuthoringRefusal?.(refusal);
        return null;
      }
      const failure = errorMessage(error, 'Program workspace request failed.');
      if (mounted.current) {
        dispatch({ type: 'request_failed', generation: current, error: failure });
      }
      // New and Load show the server's typed refusal in their dialog;
      // `failed`: the reply stated a failed outcome, not a refusal.
      if (replace) onAuthoringRefusal?.(failure, error instanceof KnownOutcomeError);
      // Another client's accepted edit moved the workspace ahead of this
      // request, or a Save wrote its document without adopting it as Saved.
      // Refresh both owners' truth once and never repeat the request; the
      // written-but-not-adopted outcome stays visible with its path.
      if (isStaleWorkspaceError(error)) {
        const notAdopted = savedNotAdoptedMessage(error);
        void readWorkspace(current).then(
          (workspace) => {
            if (mounted.current) {
              dispatch({
                type: 'request_succeeded',
                generation: current,
                workspace,
                message: notAdopted,
                transitionStatus: null,
              });
            }
          },
          () => undefined,
        );
        void refreshAuthoring();
      }
      return null;
    }
  }, [beginMutation, finishMutation, refreshAuthoring, state.workspace, documentUnavailableReason, mutationUnavailableReason, scenarioReplacementUnavailableReason, readWorkspace]);

  const refresh = useCallback(() => {
    const result = run(
      'refresh',
      () => readWorkspace(generation.current).then((workspace) =>
        workspaceResult(workspace, null)),
    );
    // The shared publication is the authoring half of this projection, so an
    // operator refresh reads both owners' truth instead of only the Applied
    // projection.
    void authoring.refresh();
    return result;
  }, [authoring, readWorkspace, run]);

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
    };
  }, []);

  useEffect(() => {
    if (!authoring.current || published === null) return;
    const identityKey = `${published.identity.workspaceEpoch}:${published.identity.workspaceRevision}`;
    if (projectionIdentityRead.current === identityKey) return;
    projectionIdentityRead.current = identityKey;
    const requestGeneration = generation.current + 1;
    void run(
      'refresh',
      () => readWorkspace(requestGeneration).then((workspace) =>
        workspaceResult(workspace, null)),
    );
  }, [authoring, published, readWorkspace, run]);

  // The Applied half of this projection changes without a new authoring
  // publication when an accepted Load/Apply/Revert settles. Re-read once per
  // Applied runtime identity, as the runtime projection does, and never over
  // an in-flight operation, whose own result is still to be shown.
  // An explicit Applied re-read (a settled Apply, or the Applied view
  // recovering from a transient readback refusal) joins the same key, so it is
  // also deferred past an in-flight operation instead of being lost.
  const [appliedReadRequest, setAppliedReadRequest] = useState(0);
  const refreshApplied = useCallback(() => setAppliedReadRequest((value) => value + 1), []);
  const appliedIdentity = `${programRuntimeWakeKey(snapshot)}:${appliedReadRequest}`;
  const operationPending = state.status === 'loading'
    || (state.status === 'ready' && state.pendingAction !== null);
  useEffect(() => {
    if (snapshot === null || state.workspace === null || operationPending) return;
    if (appliedIdentityRead.current === appliedIdentity) return;
    const initial = appliedIdentityRead.current === null;
    appliedIdentityRead.current = appliedIdentity;
    if (initial) return;
    const requestGeneration = generation.current + 1;
    void run(
      'refresh',
      () => readWorkspace(requestGeneration).then((workspace) =>
        workspaceResult(workspace, null)),
    );
  }, [appliedIdentity, operationPending, readWorkspace, run, snapshot, state.workspace]);

  useEffect(() => {
    if (state.workspace === null || published === null) return;
    // Never let a lagging publication revert a just-committed post-image.
    if (!publicationSupersedesProgram(published, state.workspace)) return;
    // Publish the shared authoring half into the projection this hook displays.
    // The publication is the only authoring writer, so this can never resurrect
    // an older Draft over a newer one.
    const merged = withSharedProgramAuthoring(
      state.workspace,
      published.snapshot,
    );
    if (
      merged.draft === state.workspace.draft
      && merged.saved === state.workspace.saved
      && merged.hasUnsavedChanges === state.workspace.hasUnsavedChanges
    ) {
      return;
    }
    dispatch({ type: 'authoring_synced', workspace: merged });
  }, [published, state.workspace]);

  const requireWorkspace = useCallback((): ProgramWorkspaceResponse | null =>
    state.workspace, [state.workspace]);

  /**
   * The one local Program fence, or ``null`` while the action is gated (an
   * `edit` is also admitted while a held run has all motion stopped). With
   * `expected`, the fence's Draft identity must be exactly that content;
   * otherwise nothing may be sent (`ProgramDraftChangedError`).
   */
  const admissionGeneration = snapshot?.authoringAdmission.admissionGeneration ?? null;
  const applicationStartId = snapshot?.applicationStartId ?? null;
  const localFence = useCallback((
    kind: 'edit' | 'document',
    expected?: ExpectedProgramDraft,
  ): LocalWorkspaceFenceRequest | null => {
    if (
      (kind === 'edit' ? mutationUnavailableReason : documentUnavailableReason) !== null
      || published === null
      || admissionGeneration === null
      || applicationStartId === null
    ) return null;
    const mismatch = expected === undefined ? null : programDraftMismatch(published, expected);
    if (mismatch !== null) throw new ProgramDraftChangedError(mismatch);
    return programLocalFenceFromAdmission(
      published,
      admissionGeneration,
      applicationStartId,
      newRequestKey(),
    );
  }, [documentUnavailableReason, mutationUnavailableReason, published, admissionGeneration, applicationStartId]);

  /**
   * The one Scenario document fence.
   *
   * Both Draft halves come from the same installed publication the backend
   * rechecks, so this needs no second workspace read.
   */
  const scenarioFence = useCallback((): ScenarioDocumentFenceRequest | null => {
    if (
      documentUnavailableReason !== null
      || published === null
      || snapshot === null
    ) return null;
    return scenarioLocalFence(published, snapshot, newRequestKey());
  }, [documentUnavailableReason, published, snapshot]);

  const mutate = useCallback(async (
    mutation: string,
    args: Record<string, JsonValue>,
  ): Promise<DraftMutationResponse | null> => {
    const requestFence = localFence('edit');
    if (!requestFence) return null;
    let response: DraftMutationResponse | null = null;
    const result = await run(
      'mutate',
      async () => {
        response = await api.programWorkspace.mutate(requestFence, mutation, args);
        return workspaceResult(response.workspace, null);
      },
    );
    return result === null ? null : response;
  }, [localFence, run]);

  const openDraft = useCallback((path: string, confirmDiscard: boolean) => {
    const requestFence = localFence('document');
    if (!requestFence) return;
    void run(
      'open_draft',
      () => api.programWorkspace.openDraft(
        requestFence,
        path,
        confirmDiscard,
      ).then((next) => workspaceResult(
        next,
        'Program opened as Draft; runtime Applied state was not changed.',
      )),
    );
  }, [localFence, run]);

  const save = useCallback(() => {
    const requestFence = localFence('document');
    if (!requestFence) return;
    void run(
      'save',
      () => api.programWorkspace.save(requestFence).then((next) =>
        workspaceResult(
          next,
          'Draft saved; runtime Applied state was not changed.',
        )),
    );
  }, [localFence, run]);

  const saveAs = useCallback((path: string, confirmOverwrite: boolean) => {
    const requestFence = localFence('document');
    if (!requestFence) return;
    void run(
      'save_as',
      () => api.programWorkspace.saveAs(
        requestFence,
        path,
        confirmOverwrite,
      ).then((next) => workspaceResult(
        next,
        'Draft saved to the selected path; runtime Applied state was not changed.',
      )),
    );
  }, [localFence, run]);

  /**
   * The ordinary Apply of the current Draft (Idle only; with a run, the
   * Apply-after-Reset chain calls this once the run has ended). Resolves to
   * the owner's result, including the exact accepted Manager Apply identity
   * for the existing terminal tracker, or to null when not sent or refused.
   * With `expectedDraft`, only exactly that Draft content is Applied;
   * otherwise it rejects with `ProgramDraftChangedError` before anything is
   * sent.
   */
  const apply = useCallback((
    options: { expectedDraft?: ExpectedProgramDraft } = {},
  ): Promise<ProgramWorkspaceRunResult | null> => {
    let requestFence: LocalWorkspaceFenceRequest | null;
    try {
      requestFence = localFence('document', options.expectedDraft);
    } catch (error: unknown) {
      return Promise.reject(error);
    }
    if (!requestFence) return Promise.resolve(null);
    return run(
      'apply',
      () => programApplyCommand('apply', () => api.programWorkspace.apply(requestFence),
        requestFence.applicationStartId),
    );
  }, [localFence, programApplyCommand, run]);

  const discard = useCallback(() => {
    const requestFence = localFence('edit');
    if (!requestFence) return;
    void run(
      'discard',
      () => api.programWorkspace.discard(requestFence).then((next) =>
        workspaceResult(
          next,
          'Draft discarded to the owner-issued Applied Program; Saved was unchanged.',
        )),
    );
  }, [localFence, run]);

  /** Discard unapplied Draft changes of the whole Scenario (Program and Deployment). */
  const discardScenario = useCallback(() => {
    if (mutationUnavailableReason !== null || published === null || snapshot === null) return;
    const requestFence = scenarioLocalFence(published, snapshot, newRequestKey());
    void run(
      'discard_scenario',
      () => api.scenarioDocuments.discard(requestFence).then((next) =>
        workspaceResult(
          next,
          'Unapplied Draft changes discarded to Applied; Saved was unchanged.',
        )),
    );
  }, [mutationUnavailableReason, published, run, snapshot]);

  /**
   * Revert Draft and Applied to Saved, like `apply`: also the step a
   * Reset-and-Revert chain sends once the run has ended, then with
   * `expectedDraft` (the Draft the operator confirmed it on).
   */
  const revert = useCallback((
    options: { expectedDraft?: ExpectedProgramDraft } = {},
  ): Promise<ProgramWorkspaceRunResult | null> => {
    let requestFence: LocalWorkspaceFenceRequest | null;
    try {
      requestFence = localFence('document', options.expectedDraft);
    } catch (error: unknown) {
      return Promise.reject(error);
    }
    if (!requestFence) return Promise.resolve(null);
    return run(
      'revert',
      () => programApplyCommand('revert', () => api.programWorkspace.revert(requestFence),
        requestFence.applicationStartId),
    );
  }, [localFence, programApplyCommand, run]);

  const saveScenarioDocument = useCallback((input: SaveScenarioDocumentInput) => {
    void run('save_scenario_document', async () => {
      const requestFence = scenarioFence();
      if (requestFence === null) throw new Error('Scenario Save is not admitted.');
      const response = await api.scenarioDocuments.save(requestFence, input);
      return workspaceResult(
        response.workspace,
        `Scenario saved (${response.lastKnownStatus}); runtime state was not changed.`,
      );
    });
  }, [run, scenarioFence]);

  const exportScenarioDocument = useCallback((destinationPath: string) => {
    const requestFence = scenarioFence();
    const current = requireWorkspace();
    if (requestFence === null || current === null) return;
    void run('export_scenario_document', async () => {
      const response = await api.scenarioDocuments.export(
        requestFence, destinationPath,
      );
      return workspaceResult(
        current,
        `Portable Scenario exported to ${response.path}.`,
      );
    });
  }, [requireWorkspace, run, scenarioFence]);

  /**
   * New and Load are the server's one Apply path: New is
   * the Apply of the empty cell; Load is New, then Apply of the file. The
   * affected components come back prepared; Enable stays the operator's.
   */
  const activateScenarioDocument = useCallback((
    kind: 'new' | 'load',
    path: string | null,
    onRefusal?: (message: string, failed?: boolean) => void,
  ) => {
    const label = kind === 'new' ? 'New Scenario' : 'Scenario Load';
    return run(`${kind}_scenario_document`, async () => {
      let accepted;
      try {
        accepted = kind === 'new' ? await api.cellApply.newScenario()
          : await api.cellApply.load(path as string);
      } catch (error: unknown) {
        // A dropped connection is no refusal: the request may have taken
        // effect, so the current Scenario is shown. Nothing is retried.
        if (connectionLost(error)) {
          throw new Error(`${label}: the connection dropped before the reply arrived. ${await currentScenarioText()} Nothing was retried.`, { cause: error });
        }
        throw new Error(`${label} refused: ${cellApplyErrorText(error, `${label} failed.`)}`, { cause: error });
      }
      // Answered at acceptance; the outcome is the row's end (OD-214). A new
      // application start keeps no row of it: the current state is read.
      const ended = await awaitCommandEnd(accepted, applyRows);
      if (ended === null) {
        return { workspace: await api.programWorkspace.get(), message: null, transitionStatus: null };
      }
      const refusal = applyRowRefusal(ended);
      if (refusal !== null) throw new Error(`${label} refused: ${codedErrorText(refusal)}`);
      const result = applyResultOfRow(ended);
      // The row states the failure: the outcome is known, not unconfirmed.
      if (result.outcome === 'failed') throw new KnownOutcomeError(`${label}: ${applyResultText(result)}`);
      scenarioOpened.current?.(kind);
      return {
        workspace: await api.programWorkspace.get(),
        message: kind === 'new' ? `${label}: ${applyResultText(result)}`
          : `${label}: ${applyResultText(result)} Enable the components to run.`,
        transitionStatus: 'applied',
      };
    }, onRefusal);
  }, [applyRows, run]);
  /**
   * `onRefusal` receives the reason when Load is refused before any effect,
   * or (`failed`) when the Load ran and its reply stated a failed outcome.
   */
  const loadScenarioDocument = useCallback(
    (path: string, onRefusal?: (message: string, failed?: boolean) => void) => activateScenarioDocument('load', path, onRefusal),
    [activateScenarioDocument],
  );
  /** New always works; the Scene is initialized. */
  const newScenarioDocument = useCallback(
    (onRefusal?: (message: string) => void) => activateScenarioDocument('new', null, onRefusal),
    [activateScenarioDocument],
  );

  const updateNodePolicies = useCallback((
    instanceId: string,
    recoveryPolicy: ProgramNodeRecoveryPolicy,
    operatorStopPolicy: ProgramOperatorStopPolicy,
    executionGroups: NonNullable<ProgramTreeNode['execution_groups']>,
  ) => {
    mutate('program_draft_update_node_policies', {
      instance_id: instanceId,
      recovery_policy: recoveryPolicy as unknown as JsonValue,
      operator_stop_policy: operatorStopPolicy,
      execution_groups: executionGroups,
    });
  }, [mutate]);
  // Editor-only "show collapsed by default" flag, kept in the node's metadata.
  const updateNodePresentation = useCallback((
    instanceId: string,
    collapsedByDefault: boolean,
  ) => {
    mutate('program_draft_update_node_presentation', {
      instance_id: instanceId,
      collapsed_by_default: collapsedByDefault,
    });
  }, [mutate]);
  const clearFeedback = useCallback(() => {
    dispatch({ type: 'clear_feedback' });
  }, []);

  const busyOperation = withApplyStep(authoring.operation, snapshot?.applyCommands);
  return useMemo(() => ({
    state,
    programDraftInputResetEpoch,
    scenarioDraftInputResetEpoch,
    /** The authoring operation in progress (busy label and tooltip), or null. */
    busyOperation,
    acceptedApply,
    mutationAvailable,
    mutationUnavailableReason,
    documentUnavailableReason,
    scenarioReplacementUnavailableReason,
    admissionObserved,
    fenceDraft,
    refresh,
    refreshApplied,
    mutate,
    newScenarioDocument,
    openDraft,
    save,
    saveAs,
    apply,
    discard,
    discardScenario,
    revert,
    saveScenarioDocument,
    loadScenarioDocument,
    exportScenarioDocument,
    // The selected project path and its source diagnostics are shared project
    // facts of the one publication, not per-hook state.
    scenarioDocumentPath: project === null ? null : project.documentPath,
    scenarioDocument: {
      id: project?.documentId ?? null,
      name: project?.documentName ?? null,
      description: project?.documentDescription ?? null,
      readOnly: project?.readOnly === true,
    },
    scenarioSourceDiagnostics: project === null ? [] : project.sourceDiagnostics,
    updateNodePolicies,
    updateNodePresentation,
    clearFeedback,
  }), [
    acceptedApply,
    admissionObserved,
    busyOperation,
    apply,
    clearFeedback,
    discard,
    discardScenario,
    documentUnavailableReason,
    programDraftInputResetEpoch,
    scenarioDraftInputResetEpoch,
    scenarioReplacementUnavailableReason,
    loadScenarioDocument,
    exportScenarioDocument,
    fenceDraft,
    mutate,
    mutationAvailable,
    mutationUnavailableReason,
    newScenarioDocument,
    openDraft,
    project,
    refresh,
    refreshApplied,
    revert,
    save,
    saveAs,
    saveScenarioDocument,
    state,
    updateNodePolicies,
    updateNodePresentation,
  ]);
}

export type ProgramWorkspaceController = ReturnType<typeof useProgramWorkspace>;
