import {
  useCallback,
  useEffect,
  useMemo,
  useReducer,
  useRef,
  useState,
} from 'react';

import { api, authoringRefusalMessage, isStaleWorkspaceError, savedNotAdoptedMessage } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import { parseManagerApplyAvailability } from '../api/parsers';
import { managerPublicationMatches } from '../state/scenarioClose';
import type { ManagerApplyIdentity } from '../state/managerApply';
import {
  programLocalFence,
  publicationSupersedesProgram,
  programActionUnavailableReason,
  scenarioLocalFence,
  withSharedProgramAuthoring,
  authoringUnavailableCause,
  requiredComponentFaultNotices,
  type PublishedAuthoringWorkspace,
} from '../state/authoringWorkspace';
import {
  INITIAL_PROGRAM_WORKSPACE_STATE,
  programWorkspaceReducer,
  type ProgramWorkspaceActionName,
} from '../state/programWorkspace';
import type {
  DraftMutationResponse,
  SaveScenarioDocumentInput,
  JsonValue,
  LocalWorkspaceFenceRequest,
  ProgramNodeRecoveryPolicy,
  ProgramOperatorStopPolicy,
  ProgramTreeNode,
  ProgramWorkspaceResponse,
  ProgramWorkspaceTransitionResponse,
  ProgramWorkspaceTransitionStatus,
  ScenarioDocumentFenceRequest,
  ManagerActiveProjection,
} from '../types/api';
import type { AuthoringWorkspaceIdentity, ComposerHeadlessState } from '../types/headless';
import type { AuthoringWorkspaceController } from './useAuthoringWorkspace';
import { browserStableIdentity, sameAuthoringWorkspaceIdentity } from './useHeadlessState';
import { programRuntimeWakeKey } from './useProgramRuntime';

function identity(prefix: string): string {
  const random = globalThis.crypto?.randomUUID?.()
    ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  return `${prefix}.${random}`;
}

/** The exact Program Draft content a caller acts on (a confirmed Apply). */
export interface ExpectedProgramDraft {
  draftId: string;
  draftRevision: number;
  programRevision: string;
}

/** Read identities captured by one confirmed GUI replacement, never admission. */
export interface ScenarioReplacementIdentity {
  authoring: AuthoringWorkspaceIdentity;
  receiverEpoch: string;
  managerOwnerId: string | null;
  managerOwnerEpoch: string | null;
  managerActive: unknown;
  closeCycle?: ComposerHeadlessState['programCycle'];
  closeScene?: ComposerHeadlessState['runtime']['sections']['logicalScene'];
}

type ScenarioCloseHandoff = { status: 'waiting' | 'ready' | 'refused'; message: string | null };

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
  return typeof applyId === 'string' && applyId.length > 0
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

export function programWorkspaceTransitionResult(
  action: 'apply' | 'load' | 'revert',
  response: ProgramWorkspaceTransitionResponse,
): ProgramWorkspaceRunResult {
  if (response.status === 'accepted'
      && (action === 'apply' || action === 'revert')) {
    const applyId = response.headless?.apply_id;
    return {
      workspace: response.workspace,
      message: `${action === 'apply' ? 'Apply' : 'Revert'} accepted (${String(applyId)}); configuration progress is shown under Activity.`,
      transitionStatus: response.status,
      acceptedApply: acceptedApplyIdentity(applyId, response.headless?.apply_generation),
    };
  }
  if (action === 'revert' && response.status === 'reverted') {
    return {
      workspace: response.workspace,
      message: response.headless === null
        ? 'Saved already matched Applied; Draft now matches Saved. No owner Apply was issued.'
        : 'Revert completed through authoritative Apply; Draft now matches Saved.',
      transitionStatus: response.status,
    };
  }
  if (action === 'load' && response.status === 'loaded') {
    return {
      workspace: response.workspace,
      message: 'Scenario configured; activate components separately before execution.',
      transitionStatus: response.status,
    };
  }
  if (action === 'apply' && response.status === 'applied') {
    return {
      workspace: response.workspace,
      message: 'Apply completed; the owner-issued Applied state was replaced.',
      transitionStatus: response.status,
    };
  }
  throw new Error(`Unexpected ${response.status} status for ${action}.`);
}

export function useProgramWorkspace(
  snapshot: ComposerHeadlessState | null,
  authoring: AuthoringWorkspaceController,
  /** Called after a successful Scenario Load or New reply. */
  onScenarioOpened?: () => void,
) {
  const [state, dispatch] = useReducer(
    programWorkspaceReducer,
    INITIAL_PROGRAM_WORKSPACE_STATE,
  );
  const [acceptedApply, setAcceptedApply] = useState<ManagerApplyIdentity | null>(null);
  const scenarioOpened = useRef(onScenarioOpened);
  useEffect(() => { scenarioOpened.current = onScenarioOpened; }, [onScenarioOpened]);
  const generation = useRef(0);
  const mounted = useRef(true);
  const workspaceRead = useRef<Promise<ProgramWorkspaceResponse> | null>(null);
  const workspaceReadGeneration = useRef<number | null>(null);
  const projectionIdentityRead = useRef<string | null>(null);
  const appliedIdentityRead = useRef<string | null>(null);
  const requestOwnerEpoch = useRef(browserStableIdentity(
    'manyforge.composer.request_owner_epoch',
    'composer-authoring-owner',
  ));
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
  const admissionObserved = snapshot !== null;
  const published = authoring.published;
  const project = authoring.project;
  const scenarioReplacementIdentity = useMemo((): ScenarioReplacementIdentity | null => (
    published === null || snapshot === null || !authoring.current || state.workspace === null
      || !sameAuthoringWorkspaceIdentity(state.workspace.authoringWorkspace, published.identity)
      || !snapshot.runtime.sections.manager.sourceOwnerId || !snapshot.runtime.sections.manager.sourceOwnerEpoch ? null : {
      authoring: published.identity,
      receiverEpoch: snapshot.receiverEpoch,
      managerOwnerId: snapshot.runtime.sections.manager.sourceOwnerId,
      managerOwnerEpoch: snapshot.runtime.sections.manager.sourceOwnerEpoch,
      managerActive: snapshot.runtime.sections.manager.payload?.active,
      closeCycle: snapshot.programCycle, closeScene: snapshot.runtime.sections.logicalScene,
    }
  ), [authoring.current, published, snapshot, state.workspace]);

  const scenarioCloseHandoff = useCallback((
    predecessor: ManagerActiveProjection,
    closed: ManagerActiveProjection,
    confirmed: ScenarioReplacementIdentity,
  ): ScenarioCloseHandoff => {
    const waiting = (message = 'Scenario closed. Waiting for current authoring admission.'): ScenarioCloseHandoff => (
      { status: 'waiting', message }
    );
    const refused = (message: string): ScenarioCloseHandoff => ({ status: 'refused', message });
    if (snapshot === null) return waiting();
    const manager = snapshot.runtime.sections.manager;
    if (snapshot.receiverEpoch !== confirmed.receiverEpoch
      || manager.sourceOwnerId !== confirmed.managerOwnerId
      || manager.sourceOwnerEpoch !== confirmed.managerOwnerEpoch) {
      return refused('The backend owner changed after confirmation. No replacement was sent.');
    }
    if (!sameAuthoringWorkspaceIdentity(snapshot.authoringWorkspace, confirmed.authoring)
      || (published !== null && !sameAuthoringWorkspaceIdentity(published.identity, confirmed.authoring))) {
      return refused('The confirmed workspace or Draft changed. No replacement was sent.');
    }
    if (authoring.error !== null || authoring.submissionUnconfirmed) {
      return refused(authoring.error ?? authoring.unavailableReason ?? 'An authoring outcome is unconfirmed.');
    }
    if (state.error !== null) return refused(state.error);
    if (!manager.available || manager.freshness !== 'fresh' || manager.knownGaps.length > 0
      || manager.payload === null) return waiting();
    if (!managerPublicationMatches(manager.payload.active, closed)) {
      return managerPublicationMatches(manager.payload.active, predecessor)
        ? waiting()
        : refused('The Applied publication changed after confirmation. No replacement was sent.');
    }
    const transaction = closed.publication.transaction as Record<string, unknown>;
    try {
      const availability = parseManagerApplyAvailability(manager.payload.apply, 'manager.payload.apply');
      if (availability.records.some((record) => record.incomplete
        && (record.applyId !== transaction.apply_id || record.applyGeneration !== transaction.apply_generation))) {
        return refused('Another Manager operation intervened. No replacement was sent.');
      }
    } catch (error: unknown) {
      return refused(errorMessage(error, 'Current Manager operation status is invalid.'));
    }
    const fault = requiredComponentFaultNotices(snapshot)[0];
    if (fault !== undefined) return refused(`${fault.title}: ${fault.message}`);
    if (snapshot.componentOverview.readiness === 'fault') {
      return refused(snapshot.componentOverview.userMessage);
    }
    const cause = authoringUnavailableCause(published, snapshot);
    if (cause === 'contained' || cause === 'execution_active') {
      return refused(authoring.unavailableReason ?? 'Authoring is not admitted. No replacement was sent.');
    }
    if (documentUnavailableReason !== null) return waiting(documentUnavailableReason);
    if (state.status === 'loading' || state.pendingAction !== null) return waiting();
    return { status: 'ready', message: null };
  }, [authoring, documentUnavailableReason, published, snapshot, state.error, state.pendingAction, state.status]);
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

  const run = useCallback(async (
    action: ProgramWorkspaceActionName,
    request: () => Promise<ProgramWorkspaceRunResult>,
  ): Promise<ProgramWorkspaceRunResult | null> => {
    // A Scenario Discard is a Draft edit; whether any Scenario document has
    // unapplied changes is decided by its route and by the backend, never by
    // the Program-only Discard gate below.
    const edit = action === 'mutate' || action === 'discard' || action === 'discard_scenario';
    if ((action === 'apply' || action === 'revert' || action === 'discard')
      && programActionUnavailableReason(
        state.workspace, action, edit ? mutationUnavailableReason : documentUnavailableReason,
      ) !== null) return null;
    if (action !== 'refresh' && !authoring.beginMutation(edit ? 'edit' : 'document')) return null;
    const current = generation.current + 1;
    generation.current = current;
    dispatch({ type: 'request_started', generation: current, action });
    try {
      const result = await request();
      if (action !== 'refresh') authoring.finishMutation(result.acceptedApply ?? null);
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
      }
      return result;
    } catch (error: unknown) {
      if (action !== 'refresh') authoring.finishMutation(null, error);
      // A stale fence is also refused before any effect, but it needs the
      // refresh below, so it is never handled as a plain refusal.
      const refusal = action === 'refresh' || isStaleWorkspaceError(error)
        ? null : authoringRefusalMessage(error);
      if (refusal !== null) {
        if (mounted.current) dispatch({ type: 'request_refused', generation: current, message: refusal });
        return null;
      }
      if (mounted.current) {
        dispatch({
          type: 'request_failed',
          generation: current,
          error: errorMessage(error, 'Program workspace request failed.'),
        });
      }
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
        void authoring.refresh();
      }
      return null;
    }
  }, [authoring, state.workspace, documentUnavailableReason, mutationUnavailableReason, readWorkspace]);

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
  const localFence = useCallback((
    kind: 'edit' | 'document',
    expected?: ExpectedProgramDraft,
  ): LocalWorkspaceFenceRequest | null => {
    if (
      (kind === 'edit' ? mutationUnavailableReason : documentUnavailableReason) !== null
      || published === null
      || snapshot === null
    ) return null;
    const mismatch = expected === undefined ? null : programDraftMismatch(published, expected);
    if (mismatch !== null) throw new ProgramDraftChangedError(mismatch);
    return programLocalFence(
      published,
      snapshot,
      requestOwnerEpoch.current,
      identity('composer-authoring'),
    );
  }, [documentUnavailableReason, mutationUnavailableReason, published, snapshot]);

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
    return scenarioLocalFence(
      published,
      snapshot,
      requestOwnerEpoch.current,
      identity('composer-document'),
    );
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
    options: { correlationId?: string; expectedDraft?: ExpectedProgramDraft } = {},
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
      () => api.programWorkspace.apply(requestFence, undefined, options.correlationId)
        .then((response) => programWorkspaceTransitionResult('apply', response)),
    );
  }, [localFence, run]);

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
    const requestFence = scenarioLocalFence(
      published,
      snapshot,
      requestOwnerEpoch.current,
      identity('composer-authoring'),
    );
    void run(
      'discard_scenario',
      () => api.scenarioDocuments.discard(requestFence).then((next) =>
        workspaceResult(
          next,
          'Unapplied Draft changes discarded to Applied; Saved was unchanged.',
        )),
    );
  }, [mutationUnavailableReason, published, run, snapshot]);

  const revert = useCallback(() => {
    const requestFence = localFence('document');
    if (!requestFence) return;
    void run(
      'revert',
      () => api.programWorkspace.revert(requestFence)
        .then((response) => programWorkspaceTransitionResult('revert', response)),
    );
  }, [localFence, run]);

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

  // New is the ordinary Load of the built-in Untitled Scenario (path null).
  const activateScenarioDocument = useCallback((
    path: string | null,
    confirmDiscard: boolean,
  ) => {
    const label = path === null ? 'New Scenario' : 'Scenario Load';
    return run(path === null ? 'new_scenario_document' : 'load_scenario_document', async () => {
      const requestFence = scenarioFence();
      if (requestFence === null) throw new Error(`${label} is not admitted.`);
      const response = path === null
        ? await api.scenarioDocuments.new(requestFence, confirmDiscard)
        : await api.scenarioDocuments.load(requestFence, path, confirmDiscard);
      scenarioOpened.current?.();
      if (response.status === 'applied') {
        return {
          workspace: response.workspace,
          message: 'Scenario loaded; its Deployment and Program already match the Applied configuration. No configuration change was needed.',
          transitionStatus: 'applied',
        };
      }
      const applyId = response.activation.apply_id;
      return {
        workspace: response.workspace,
        message: `${label} accepted (${applyId}); configuration progress is shown under Activity. Motion does not start automatically.`,
        transitionStatus: 'accepted',
        acceptedApply: acceptedApplyIdentity(
          applyId,
          response.activation.apply_generation,
        ),
      };
    });
  }, [run, scenarioFence]);
  const loadScenarioDocument = useCallback(
    (path: string, confirmDiscard: boolean) => activateScenarioDocument(path, confirmDiscard),
    [activateScenarioDocument],
  );
  const newScenarioDocument = useCallback(
    (confirmDiscard: boolean) => activateScenarioDocument(null, confirmDiscard),
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
  const reportCloseCancellation = useCallback((message: string) => {
    dispatch({ type: 'close_canceled', message });
  }, []);

  const clearFeedback = useCallback(() => {
    dispatch({ type: 'clear_feedback' });
  }, []);

  return useMemo(() => ({
    state,
    acceptedApply,
    mutationAvailable,
    mutationUnavailableReason,
    documentUnavailableReason,
    scenarioReplacementIdentity,
    scenarioCloseHandoff,
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
    reportCloseCancellation,
  }), [
    acceptedApply,
    admissionObserved,
    apply,
    clearFeedback,
    reportCloseCancellation,
    discard,
    discardScenario,
    documentUnavailableReason,
    scenarioReplacementIdentity,
    scenarioCloseHandoff,
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
