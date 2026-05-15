import { useEffect, useMemo, useState } from 'react';
import { useStatusPolling, useStatusStore } from '../store/statusStore';
import type { RuntimeComponentStatus, RuntimeStatus } from '../types/api';

function readinessClasses(readiness: string): string {
  switch (readiness) {
    case 'ready':
      return 'border-emerald-400/70 bg-emerald-600 text-white hover:bg-emerald-500 focus:ring-emerald-300';
    case 'fault':
      return 'border-red-400/70 bg-red-700 text-white hover:bg-red-600 focus:ring-red-300';
    case 'degraded':
    case 'waiting':
      return 'border-amber-300/70 bg-amber-600 text-white hover:bg-amber-500 focus:ring-amber-200';
    case 'offline':
      return 'border-gray-500/70 bg-gray-700 text-gray-100 hover:bg-gray-600 focus:ring-gray-300';
    default:
      return 'border-slate-500/70 bg-slate-700 text-gray-100 hover:bg-slate-600 focus:ring-slate-300';
  }
}

function componentStatusClasses(status: string): string {
  switch (status) {
    case 'ready':
    case 'healthy':
      return 'bg-emerald-500';
    case 'fault':
    case 'error':
      return 'bg-red-500';
    case 'degraded':
    case 'waiting':
      return 'bg-amber-400';
    case 'disabled':
      return 'bg-gray-500';
    default:
      return 'bg-slate-500';
  }
}

function authorityLabel(authority: string): string {
  if (authority === 'backend_authored') return 'Backend-authored state';
  if (authority === 'controller_observed') return 'Controller-observed state';
  return authority || 'Unknown';
}

function capabilityLabel(status: RuntimeStatus): string {
  const parts = [
    status.canRender ? 'render' : null,
    status.canPlan ? 'plan' : null,
    status.canExecute ? 'execute' : null,
  ].filter(Boolean);
  return parts.length ? `Ready to ${parts.join(', ')}` : 'No runtime capability ready';
}

function compactSummaryLabel(readiness: string, summaryLabel: string): string {
  switch (readiness) {
    case 'ready':
      return 'Ready';
    case 'fault':
      return 'Fault';
    case 'degraded':
      return 'Warn';
    case 'offline':
      return 'Off';
    case 'waiting':
      return 'Wait';
    default:
      return summaryLabel || 'Status';
  }
}

function ComponentRow({ component }: { component: RuntimeComponentStatus }) {
  const requiredFor = component.requiredFor?.length ? component.requiredFor.join(', ') : 'optional';
  return (
    <div className="grid grid-cols-[1fr_auto] gap-3 border-b border-gray-800 py-2 last:border-b-0">
      <div className="min-w-0">
        <div className="flex items-center gap-2">
          <span className={`h-2.5 w-2.5 shrink-0 rounded-full ${componentStatusClasses(component.status)}`} />
          <span className="truncate text-sm font-medium text-gray-100">{component.label}</span>
        </div>
        <div className="mt-1 truncate text-xs text-gray-500">{component.detail || requiredFor}</div>
      </div>
      <div className="flex flex-col items-end gap-1 text-right">
        <span className="text-xs font-semibold uppercase text-gray-300">{component.status}</span>
        <span className="text-[11px] text-gray-500">{requiredFor}</span>
      </div>
    </div>
  );
}

export function RuntimeStatusOverlay() {
  const [open, setOpen] = useState(false);
  // Single shared poll loop in ../store/statusStore.ts. This component
  // is one of two subscribers (ViserPanel is the other); together they
  // replace what used to be two independent setInterval timers fetching
  // the same /api/runtime/status endpoint every 2 s.
  useStatusPolling();
  const status = useStatusStore((state) => state.runtime);
  const error = useStatusStore((state) => state.runtimeError);

  useEffect(() => {
    if (!open) return;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setOpen(false);
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [open]);

  const readiness = status?.readiness ?? (error ? 'offline' : 'waiting');
  const summaryLabel = status?.summaryLabel ?? (error ? 'Offline' : 'Waiting');
  const buttonLabel = compactSummaryLabel(readiness, summaryLabel);
  const sortedComponents = useMemo(() => status?.components ?? [], [status]);

  return (
    <>
      {!open && (
        <button
          type="button"
          onClick={() => setOpen(true)}
          className={`fixed bottom-3 right-6 z-40 flex h-12 w-12 items-center justify-center rounded-md border text-[11px] font-semibold shadow-xl shadow-black/40 transition focus:outline-none focus:ring-2 ${readinessClasses(readiness)}`}
          title={status?.userMessage || error || 'Runtime status'}
        >
          {buttonLabel}
        </button>
      )}

      {open && (
        <div className="pointer-events-none fixed inset-x-0 bottom-16 z-40 px-6 pb-4">
          <section className="pointer-events-auto ml-auto flex max-h-[min(62vh,640px)] w-[min(520px,calc(100vw-3rem))] flex-col overflow-hidden rounded-lg border border-gray-700/90 bg-gray-950/90 text-white shadow-2xl shadow-black/50 backdrop-blur-md">
            <header className="flex shrink-0 items-center gap-3 border-b border-gray-800 bg-gray-950/75 px-4 py-3">
              <div className={`flex h-10 w-16 shrink-0 items-center justify-center rounded-md text-xs font-semibold ${readinessClasses(readiness)}`}>
                {summaryLabel}
              </div>
              <div className="min-w-0 flex-1">
                <h2 className="truncate text-sm font-semibold text-gray-100">Runtime</h2>
                <p className="truncate text-xs text-gray-500">
                  {status ? `${status.profileLabel} / ${authorityLabel(status.authority)}` : 'Status unavailable'}
                </p>
              </div>
              <button
                type="button"
                onClick={() => setOpen(false)}
                className="rounded border border-gray-700 bg-gray-900 px-3 py-1 text-xs text-gray-300 hover:bg-gray-800"
                title="Close runtime status"
              >
                Close
              </button>
            </header>

            <div className="min-h-0 overflow-y-auto px-4 py-3">
              {error ? (
                <div className="rounded border border-red-800 bg-red-950/50 px-3 py-2 text-sm text-red-200">
                  {error}
                </div>
              ) : status ? (
                <>
                  <div className="mb-3 rounded border border-gray-800 bg-gray-900/80 px-3 py-2">
                    <div className="text-sm font-medium text-gray-100">{capabilityLabel(status)}</div>
                    <div className="mt-1 text-xs text-gray-400">{status.userMessage}</div>
                  </div>
                  <div className="mb-2 grid grid-cols-3 gap-2 text-center text-xs">
                    <div className={`rounded border px-2 py-1 ${status.canRender ? 'border-emerald-700 bg-emerald-950/50 text-emerald-200' : 'border-gray-800 bg-gray-900 text-gray-500'}`}>
                      Render
                    </div>
                    <div className={`rounded border px-2 py-1 ${status.canPlan ? 'border-emerald-700 bg-emerald-950/50 text-emerald-200' : 'border-gray-800 bg-gray-900 text-gray-500'}`}>
                      Plan
                    </div>
                    <div className={`rounded border px-2 py-1 ${status.canExecute ? 'border-emerald-700 bg-emerald-950/50 text-emerald-200' : 'border-gray-800 bg-gray-900 text-gray-500'}`}>
                      Execute
                    </div>
                  </div>
                  <div className="rounded border border-gray-800 bg-gray-950/60 px-3 py-1">
                    {sortedComponents.map((component) => (
                      <ComponentRow key={component.id} component={component} />
                    ))}
                  </div>
                </>
              ) : (
                <div className="rounded border border-gray-800 bg-gray-900 px-3 py-2 text-sm text-gray-300">
                  Loading runtime status...
                </div>
              )}
            </div>
          </section>
        </div>
      )}
    </>
  );
}
