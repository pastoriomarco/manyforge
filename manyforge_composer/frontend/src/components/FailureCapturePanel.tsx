import { useEffect, useRef, useState } from 'react';

import { api } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import type {
  HistorianCapture,
  HistorianCaptureUncertainty,
  HistorianStoredEvent,
} from '../types/headless';

const UNCERTAINTY_TEXT: Record<HistorianCaptureUncertainty, string> = {
  context_pruned_before_capture: 'older context had already been pruned when it opened',
  writer_delivery_loss_before_capture: 'the Historian writer dropped events earlier in this process; their position is unknown',
  writer_delivery_loss_during_capture: 'the Historian writer dropped events while it was open',
  context_spans_writer_restart: 'its context includes events from before a Historian restart',
  tail_interrupted_by_restart: 'a restart ended it before its tail completed',
};

function utcText(nanoseconds: string | null): string {
  return nanoseconds === null
    ? 'time unavailable'
    : new Date(Number(BigInt(nanoseconds) / 1_000_000n)).toISOString();
}

/** One bounded page of events, bound to the capture it was read for. */
export interface CaptureEventPage {
  captureId: string;
  events: HistorianStoredEvent[];
  olderCursor: number | null;
  showingOlder: boolean;
}

/**
 * The page shown under a capture: only one read for that exact capture, so
 * switching captures (or a failed read) never shows another capture's events.
 */
export function captureEventPageFor(
  page: CaptureEventPage | null,
  captureId: string,
): CaptureEventPage | null {
  return page !== null && page.captureId === captureId ? page : null;
}

export function captureStateText(capture: HistorianCapture): string {
  if (capture.state === 'open') return 'collecting context';
  if (capture.state === 'closed_by_restart') return 'ended by restart';
  if (capture.state === 'truncated') return 'truncated at its byte limit';
  return capture.closeReason === 'tail_duration' ? 'complete (tail time elapsed)' : 'complete';
}

/**
 * Retained failure captures: the durable events around an error, protected
 * from ordinary pruning. Each capture states what it contains and what it
 * cannot prove; it is retained context, not a complete state dump.
 */
export function FailureCapturePanel({ open }: { open: boolean }) {
  const [captures, setCaptures] = useState<HistorianCapture[]>([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [selected, setSelected] = useState<string | null>(null);
  const [eventPage, setEventPage] = useState<CaptureEventPage | null>(null);
  const [eventsLoading, setEventsLoading] = useState(false);
  const listRequest = useRef<AbortController | null>(null);
  const eventRequest = useRef<AbortController | null>(null);

  const load = () => {
    listRequest.current?.abort();
    const request = new AbortController();
    listRequest.current = request;
    setLoading(true);
    setError(null);
    api.history.captures(null, request.signal).then((page) => {
      if (request.signal.aborted) return;
      setCaptures(page.captures);
      setLoading(false);
    }, (reason: unknown) => {
      if (request.signal.aborted) return;
      setError(errorMessage(reason, 'Failure captures could not be read.'));
      setLoading(false);
    });
  };

  const loadEvents = (captureId: string, beforeSequence: number | null) => {
    eventRequest.current?.abort();
    const request = new AbortController();
    eventRequest.current = request;
    setEventsLoading(true);
    setError(null);
    // Another capture's page is dropped at once; the same capture keeps its
    // current page while an older/newer page loads.
    setEventPage((current) => captureEventPageFor(current, captureId));
    api.history.list({
      captureId: Number(captureId),
      ...(beforeSequence === null ? {} : { beforeSequence }),
    }, request.signal).then((page) => {
      if (request.signal.aborted) return;
      // One bounded page replaces the previous one (spec 450 §5.3); pages
      // are never appended to an ever-growing mounted list.
      setEventPage({
        captureId,
        events: page.events,
        olderCursor: page.nextBeforeSequence,
        showingOlder: beforeSequence !== null,
      });
      setEventsLoading(false);
    }, (reason: unknown) => {
      if (request.signal.aborted) return;
      setError(errorMessage(reason, 'Failure capture events could not be read.'));
      setEventsLoading(false);
    });
  };

  useEffect(() => {
    if (!open) {
      listRequest.current?.abort();
      eventRequest.current?.abort();
      return undefined;
    }
    load();
    return () => {
      listRequest.current?.abort();
      eventRequest.current?.abort();
    };
    // Loading is an explicit read when the view opens, never a poll.
  }, [open]);

  if (!open) return null;
  return (
    <div className="runtime-observability-section failure-captures" aria-label="Failure captures">
      <header className="runtime-observability-section-header">
        <div>
          <strong>Failure captures</strong>
          <small>
            Events around each error, protected from ordinary pruning within a bounded budget.
            Retained context, not a complete state dump.
          </small>
        </div>
        <button type="button" disabled={loading} onClick={load}>Refresh</button>
      </header>
      {error !== null && <p className="error-state notice severity-error" role="alert">{error}</p>}
      {loading && captures.length === 0 && <p className="muted">Loading failure captures…</p>}
      {!loading && error === null && captures.length === 0 && (
        <p className="muted">No failure capture is retained.</p>
      )}
      {captures.map((capture) => {
        const shown = captureEventPageFor(eventPage, capture.captureId);
        const events = shown?.events ?? [];
        const olderCursor = shown?.olderCursor ?? null;
        const showingOlder = shown?.showingOlder ?? false;
        return (
        <article
          key={capture.captureId}
          className={`failure-capture coverage-${capture.coverage}${selected === capture.captureId ? ' selected' : ''}`}
        >
          <p>
            <strong>
              {capture.trigger?.message ?? capture.trigger?.eventType ?? `Event ${capture.triggerSequence}`}
            </strong>
            {' · '}{capture.trigger?.conditionSeverity ?? 'ERROR'}
            {' · '}{utcText(capture.openedAtUtcNs)}
          </p>
          <p className="muted">
            Capture {capture.captureId}: {captureStateText(capture)}; events {capture.firstSequence}–{capture.lastSequence}
            {' '}({capture.retainedRows} retained, {capture.precedingRows} before the error, {capture.tailRows} after);
            {' '}{capture.triggerCount} error{capture.triggerCount === 1 ? '' : 's'} in this window.
          </p>
          <p className={capture.coverage === 'complete' ? 'muted' : 'failure-capture-uncertain'}>
            {capture.coverage === 'complete'
              ? 'Coverage: every event in this range is retained.'
              : `Coverage uncertain: ${capture.uncertainty.map((reason) => UNCERTAINTY_TEXT[reason]).join('; ')}${
                capture.missingSequences > 0 ? `; ${capture.missingSequences} events in the range were already pruned` : ''}.`}
          </p>
          <div className="failure-capture-actions">
            <button
              type="button"
              aria-pressed={selected === capture.captureId}
              onClick={() => {
                setSelected(capture.captureId);
                loadEvents(capture.captureId, null);
              }}
            >Show events</button>
            <a href={api.history.captureExportUrl(Number(capture.captureId))} download={`manyforge-capture-${capture.captureId}.json`}>
              Export capture
            </a>
          </div>
          {selected === capture.captureId && (
            <ol className="failure-capture-events" aria-label={`Capture ${capture.captureId} events`}>
              {eventsLoading && events.length === 0 && <li className="muted">Loading events…</li>}
              {events.map((stored) => (
                <li
                  key={stored.sequence}
                  className={stored.sequence === capture.triggerSequence ? 'trigger' : undefined}
                >
                  <span>{stored.sequence}</span>
                  <span>{stored.event.occurredAtWall}</span>
                  <span>{stored.event.conditionSeverity}</span>
                  <span>{stored.event.component}</span>
                  <span>{stored.event.message}</span>
                </li>
              ))}
              {(olderCursor !== null || showingOlder) && (
                <li className="failure-capture-paging">
                  {showingOlder && (
                    <button
                      type="button"
                      disabled={eventsLoading}
                      onClick={() => loadEvents(capture.captureId, null)}
                    >Newest events</button>
                  )}
                  {olderCursor !== null && (
                    <button
                      type="button"
                      disabled={eventsLoading}
                      onClick={() => loadEvents(capture.captureId, olderCursor)}
                    >Older events</button>
                  )}
                </li>
              )}
            </ol>
          )}
        </article>
        );
      })}
    </div>
  );
}
