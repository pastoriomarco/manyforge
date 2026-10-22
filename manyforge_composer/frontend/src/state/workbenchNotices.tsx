import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from 'react';

/**
 * Typed presentation model for the workbench notification rows.
 *
 * Severity is always stated by the producer; nothing here infers it from
 * message text. Warning and information notices describe *current* state: a
 * producer republishes its complete list whenever that state changes and the
 * notice disappears with its cause. Nothing is retained after resolution; the
 * durable record of what happened is the backend Historian.
 */
export type NoticeSeverity = 'error' | 'warning' | 'info';

export type NoticeDomain =
  | 'runtime'
  | 'transport'
  | 'viewer'
  | 'activation'
  | 'recovery'
  | 'program_authoring'
  | 'settings';

/**
 * Bounded browser-only viewer conditions that Headless cannot observe.
 * They are reported to the Historian only while presented as errors.
 */
export type ViewerIncidentKind =
  | 'viewer_model_unavailable'
  | 'viewer_scene_unverified'
  | 'viewer_observation_stale'
  | 'viewer_geometry_unavailable';

export interface NoticeAction {
  id: string;
  label: string;
  title?: string;
  disabled?: boolean;
  run: () => void;
}

export interface WorkbenchNotice {
  id: string;
  severity: NoticeSeverity;
  domain: NoticeDomain;
  title: string;
  message: string;
  details?: string;
  actions?: NoticeAction[];
  busy?: boolean;
  /**
   * Operator-alarm definitions this notice represents. The Errors overlay
   * shows their attention alarms inside the notice, as one item per failure;
   * without the notice they are shown on their own.
   */
  alarmDefinitionIds?: readonly string[];
  /**
   * The condition is the expected, related consequence of a known controlled
   * transition (the model or Scene being replaced). Only while such a
   * transition is active is an error of this kind presented as information.
   * Failures, unrelated losses and lost authoritative status never set it.
   */
  expectedDuringTransition?: boolean;
  incidentKind?: ViewerIncidentKind;
  /**
   * performance.now() when a held viewer condition began; set once it has
   * lasted past the hold time and is presented (and reported) as an error.
   */
  firstObservedMs?: number;
}

/**
 * Viewer conditions routinely have short catch-up gaps (a Scene update, a
 * reconnect). One becomes an error, and is reported, only once it has lasted
 * this long; until then it is information marking the view as not current.
 */
export const VIEWER_CONDITION_HOLD_MS = 500;

const SEVERITY_ORDER: Record<NoticeSeverity, number> = {
  error: 0,
  warning: 1,
  info: 2,
};

/**
 * One observed controlled transition: an activation whose owner status has
 * been read as pending (not merely advertised, submitted or unobservable),
 * with the conditions that were already present when it began. Those are
 * not caused by it and are never downgraded.
 */
export interface TransitionScope {
  key: string;
  preexisting: ReadonlySet<string>;
}

/** Apply the one transition rule, then order errors, warnings, information. */
export function classifyWorkbenchNotices(
  notices: readonly WorkbenchNotice[],
  transition: TransitionScope | null,
): WorkbenchNotice[] {
  return notices
    .map((notice) => (
      transition !== null
      && notice.severity === 'error'
      && notice.expectedDuringTransition === true
      && !transition.preexisting.has(notice.id)
        ? { ...notice, severity: 'info' as const, incidentKind: undefined }
        : notice
    ))
    .map((notice, index) => ({ notice, index }))
    .sort((left, right) => (
      SEVERITY_ORDER[left.notice.severity] - SEVERITY_ORDER[right.notice.severity]
      || left.index - right.index
    ))
    .map(({ notice }) => notice);
}

function expectedErrorIds(notices: readonly WorkbenchNotice[]): Set<string> {
  return new Set(notices
    .filter((notice) => notice.severity === 'error' && notice.expectedDuringTransition === true)
    .map((notice) => notice.id));
}

/**
 * The scope of the transition identified by ``transitionKey`` (null when no
 * observed transition is pending). Captured in the render in which the key
 * first appears, so a condition already present is never downgraded.
 */
export function useTransitionScope(
  transitionKey: string | null,
  notices: readonly WorkbenchNotice[],
): TransitionScope | null {
  const [scope, setScope] = useState<TransitionScope | null>(null);
  if (transitionKey === null ? scope !== null : scope?.key !== transitionKey) {
    // Derived state adjusted during render (React re-renders before commit).
    setScope(transitionKey === null
      ? null
      : { key: transitionKey, preexisting: expectedErrorIds(notices) });
  }
  return scope !== null && scope.key === transitionKey ? scope : null;
}

/** The held conditions: the viewer incident kinds, while they are errors. */
function isHeldCondition(notice: WorkbenchNotice): boolean {
  return notice.severity === 'error' && notice.incidentKind !== undefined;
}

interface HoldEpisodes {
  next: number;
  /** Notice id -> episode of its current, uninterrupted occurrence. */
  episodes: ReadonlyMap<string, number>;
}

/**
 * Apply the viewer-condition hold. A condition that clears before
 * ``holdMs`` never becomes an error and is never reported; one that lasts is
 * an error carrying the time it began. Each uninterrupted occurrence is its
 * own episode, so a condition that returns is held again.
 */
export function useViewerConditionHold(
  notices: readonly WorkbenchNotice[],
  holdMs: number = VIEWER_CONDITION_HOLD_MS,
): WorkbenchNotice[] {
  const held = notices.filter(isHeldCondition).map((notice) => notice.id);
  const [hold, setHold] = useState<HoldEpisodes>({ next: 1, episodes: new Map() });
  const [confirmed, setConfirmed] = useState<ReadonlyMap<string, number>>(new Map());
  if (held.length !== hold.episodes.size || held.some((id) => !hold.episodes.has(id))) {
    // Derived state adjusted during render: episodes follow the current set.
    let next = hold.next;
    const episodes = new Map<string, number>();
    for (const id of held) episodes.set(id, hold.episodes.get(id) ?? next++);
    setHold({ next, episodes });
    const live = new Set([...episodes].map(([id, episode]) => `${id}#${episode}`));
    if ([...confirmed.keys()].some((key) => !live.has(key))) {
      setConfirmed(new Map([...confirmed].filter(([key]) => live.has(key))));
    }
  }
  const timers = useRef(new Map<string, ReturnType<typeof setTimeout>>());
  useEffect(() => {
    const pending = timers.current;
    const live = new Set<string>();
    for (const [id, episode] of hold.episodes) {
      const key = `${id}#${episode}`;
      live.add(key);
      if (pending.has(key)) continue;
      const onset = performance.now();
      pending.set(key, setTimeout(() => {
        pending.delete(key);
        setConfirmed((current) => new Map(current).set(key, onset));
      }, holdMs));
    }
    for (const [key, timer] of pending) {
      if (!live.has(key)) {
        clearTimeout(timer);
        pending.delete(key);
      }
    }
  }, [hold, holdMs]);
  useEffect(() => {
    const pending = timers.current;
    return () => {
      for (const timer of pending.values()) clearTimeout(timer);
      pending.clear();
    };
  }, []);
  return notices.map((notice) => {
    if (!isHeldCondition(notice)) return notice;
    const episode = hold.episodes.get(notice.id);
    const onset = episode === undefined ? undefined : confirmed.get(`${notice.id}#${episode}`);
    return onset !== undefined
      ? { ...notice, firstObservedMs: onset }
      : { ...notice, severity: 'info' as const, incidentKind: undefined, busy: true, actions: notice.actions };
  });
}

export function noticesOfSeverity(
  notices: readonly WorkbenchNotice[],
  severity: NoticeSeverity,
): WorkbenchNotice[] {
  return notices.filter((notice) => notice.severity === severity);
}

/** The collapsed one-line summary of a row. */
export function noticeRowSummary(
  notices: readonly WorkbenchNotice[],
  noun: { one: string; many: string },
): string {
  if (notices.length === 0) return '';
  if (notices.length === 1) {
    const [notice] = notices;
    return notice.message ? `${notice.title} · ${notice.message}` : notice.title;
  }
  // Activity says how much is still running, so finished results are not
  // mistaken for progress.
  if (noun.many === 'activities') {
    const running = notices.filter((notice) => notice.busy);
    return running.length === 0
      ? `${notices.length} ${noun.many} · none in progress · ${notices[0].title}`
      : `${notices.length} ${noun.many} · ${running.length} in progress · ${running[0].title}`;
  }
  return `${notices.length} ${noun.many} · ${notices[0].title}`;
}

/** Active viewer incident kinds, one per kind, in presentation order. */
export function activeViewerIncidentKinds(
  notices: readonly WorkbenchNotice[],
): ViewerIncidentKind[] {
  const kinds: ViewerIncidentKind[] = [];
  for (const notice of notices) {
    if (notice.severity !== 'error' || notice.incidentKind === undefined) continue;
    if (!kinds.includes(notice.incidentKind)) kinds.push(notice.incidentKind);
  }
  return kinds;
}

function noticeSignature(notices: readonly WorkbenchNotice[]): string {
  return JSON.stringify(notices.map((notice) => [
    notice.id,
    notice.severity,
    notice.domain,
    notice.title,
    notice.message,
    notice.busy === true,
    notice.expectedDuringTransition === true,
    notice.incidentKind ?? null,
    (notice.actions ?? []).map((action) => [
      action.id, action.label, action.title ?? null, action.disabled === true,
    ]),
  ]));
}

interface WorkbenchNoticeRegistry {
  notices: WorkbenchNotice[];
  publish: (source: string, notices: WorkbenchNotice[]) => void;
  withdraw: (source: string) => void;
}

const WorkbenchNoticeContext = createContext<WorkbenchNoticeRegistry>({
  notices: [],
  publish: () => undefined,
  withdraw: () => undefined,
});

/**
 * Collects the current notices of producers mounted anywhere in the tree
 * (viewer, transport controls) for the one notification stack. Each source
 * owns exactly its latest list; there is no history, retry or timer here.
 */
export function WorkbenchNoticeProvider({ children }: { children: ReactNode }) {
  const [sources, setSources] = useState<ReadonlyMap<string, WorkbenchNotice[]>>(
    () => new Map(),
  );
  const publish = useCallback((source: string, notices: WorkbenchNotice[]) => {
    setSources((current) => {
      if (notices.length === 0 && !current.has(source)) return current;
      const next = new Map(current);
      if (notices.length === 0) next.delete(source);
      else next.set(source, notices);
      return next;
    });
  }, []);
  const withdraw = useCallback((source: string) => {
    setSources((current) => {
      if (!current.has(source)) return current;
      const next = new Map(current);
      next.delete(source);
      return next;
    });
  }, []);
  const value = useMemo<WorkbenchNoticeRegistry>(() => ({
    notices: [...sources.values()].flat(),
    publish,
    withdraw,
  }), [publish, sources, withdraw]);
  return (
    <WorkbenchNoticeContext.Provider value={value}>
      {children}
    </WorkbenchNoticeContext.Provider>
  );
}

export function usePublishedWorkbenchNotices(): WorkbenchNotice[] {
  return useContext(WorkbenchNoticeContext).notices;
}

/**
 * Publish one source's current notices. Republishing happens only when the
 * visible content changes; action callbacks always call the latest handler,
 * so a published notice never runs a stale closure.
 */
export function usePublishWorkbenchNotices(
  source: string,
  notices: readonly WorkbenchNotice[],
): void {
  const { publish, withdraw } = useContext(WorkbenchNoticeContext);
  const latest = useRef(notices);
  const signature = noticeSignature(notices);
  // Runs before the publishing effect below, so actions always call the
  // handlers from the latest render.
  useEffect(() => {
    latest.current = notices;
  });
  useEffect(() => {
    publish(source, latest.current.map((notice) => ({
      ...notice,
      actions: notice.actions?.map((action) => ({
        ...action,
        run: () => latest.current
          .find((candidate) => candidate.id === notice.id)
          ?.actions?.find((candidate) => candidate.id === action.id)
          ?.run(),
      })),
    })));
  }, [publish, signature, source]);
  useEffect(() => () => withdraw(source), [source, withdraw]);
}

/** Presentation handoff only: the existing alarm owner retains ACK authority. */
export function previousSessionAlarmHandoff(
  currentSessionId: string | null,
  previousHandoff: string | null,
  receipt: unknown,
): string | null {
  if (currentSessionId !== null && receipt !== null && typeof receipt === 'object'
    && Object.keys(receipt).length === 2
    && 'session_id' in receipt && receipt.session_id === currentSessionId
    && 'status' in receipt
    && (receipt.status === 'registered' || receipt.status === 'acknowledged'
      || receipt.status === 'quiet')) return currentSessionId;
  return previousHandoff;
}

/** How long a brief, event-triggered notice stays presented. */
export const BRIEF_NOTICE_MS = 8_000;

/**
 * Presents one completed-operation message briefly, then removes it from
 * presentation. The backend keeps the durable record; nothing is stored here.
 */
export function useBriefNotice(notice: WorkbenchNotice | null): WorkbenchNotice | null {
  const [visible, setVisible] = useState<string | null>(null);
  const key = notice === null ? null : `${notice.id}\n${notice.message}`;
  useEffect(() => {
    setVisible(key);
    if (key === null) return undefined;
    const timer = window.setTimeout(() => setVisible(null), BRIEF_NOTICE_MS);
    return () => window.clearTimeout(timer);
  }, [key]);
  return notice !== null && visible === key ? notice : null;
}
