import { useMemo } from 'react';

import { useStatusPolling, useStatusStore } from '../store/statusStore';

const DEFAULT_HMI_PORT = '8081';

// Optional Viser device-pixel-ratio cap, passed to Viser's R3F Canvas
// via `dpr={fixedDpr}` (see DevSettingsStore.ts in the installed
// viser client). Default is **no cap** — empty string suppresses the
// query parameter entirely, so Viser uses its native DPR. Diagnostic
// outcome (2026-05-15): stepping down from 1.5 → 1.0 → 0.5 produced
// zero measurable responsiveness gain on Thor (aarch64, integrated
// GPU, unified memory). The dominant cost there is JS-per-frame work
// (36 useFrame callbacks in Viser's continuous 60 fps render loop),
// not pixel shading; capping DPR only reduces fragment-shader work
// which isn't the bottleneck. See
// `manyforge_specs/docs/implementation/composer-native-3d-visualizer-migration.md`
// and the open-points entry under "Composer / program artifacts /
// orchestration" for the migration plan.
//
// Operators on bandwidth-constrained displays (very high DPR + weak
// GPU + low memory) can still set `VITE_HMI_DPR=1.5` (or whatever
// value works) at build time. The in-Viser DevSettings panel also
// exposes this at runtime if you need to A/B compare.
const DEFAULT_VISER_DPR =
  (import.meta.env.VITE_HMI_DPR as string | undefined) ?? '';

function getViserUrl(port: string): string {
  const base = (() => {
    if (import.meta.env.VITE_HMI_URL) {
      return import.meta.env.VITE_HMI_URL as string;
    }
    if (typeof window === 'undefined') {
      return `http://localhost:${port}`;
    }
    const { protocol, hostname } = window.location;
    return `${protocol}//${hostname}:${port}`;
  })();
  if (!DEFAULT_VISER_DPR) {
    return base;
  }
  const separator = base.includes('?') ? '&' : '?';
  return `${base}${separator}fixedDpr=${encodeURIComponent(DEFAULT_VISER_DPR)}`;
}

interface ViserPanelProps {
  onCollapse: () => void;
  blocked: boolean;
  resizing?: boolean;
}

export function ViserPanel({ onCollapse, blocked, resizing = false }: ViserPanelProps) {
  // Subscribe to the shared status poll. The first subscriber across the
  // whole app starts the poll loop; this component just reads the latest
  // snapshot from the zustand store via tight selectors.
  useStatusPolling();
  const infra = useStatusStore((state) => state.infra);
  const runtime = useStatusStore((state) => state.runtime);

  const hmiPort = useMemo(() => {
    const port = infra?.components.hmi?.port;
    if (typeof port === 'number' && port > 0) {
      return String(port);
    }
    return DEFAULT_HMI_PORT;
  }, [infra]);

  let hmiAvailable: boolean | null;
  let hmiDetail: string | null = null;
  if (infra === null) {
    // First poll hasn't returned yet — fall through to the iframe at the
    // default port; same behavior as before the shared store.
    hmiAvailable = null;
  } else if (infra.components.hmi?.status === 'healthy') {
    hmiAvailable = true;
  } else {
    hmiAvailable = false;
    hmiDetail =
      runtime?.userMessage
      || 'Load a program/deployment to start the embedded Viser view.';
  }

  const viserUrl = useMemo(() => getViserUrl(hmiPort), [hmiPort]);

  return (
    <div className="flex h-full w-full flex-col overflow-hidden bg-gray-900">
      <div className="flex h-10 items-center justify-between border-b border-gray-700 bg-gray-800 px-3">
        <div className="min-w-0">
          <div className="text-xs font-semibold uppercase tracking-wide text-gray-300">Viser HMI</div>
          <div className="truncate text-[10px] text-gray-500">
            {hmiAvailable === false ? 'HMI unavailable' : viserUrl}
          </div>
        </div>
        <div className="flex items-center gap-2">
          {hmiAvailable === false ? (
            <span className="rounded bg-gray-800 px-2 py-1 text-[11px] font-medium text-gray-500">
              Open
            </span>
          ) : (
            <a
              href={viserUrl}
              target="_blank"
              rel="noreferrer"
              className="rounded bg-gray-700 px-2 py-1 text-[11px] font-medium text-gray-200 hover:bg-gray-600"
            >
              Open
            </a>
          )}
          <button
            type="button"
            onClick={onCollapse}
            className="rounded bg-gray-700 px-2 py-1 text-sm font-medium text-gray-200 hover:bg-gray-600"
            title="Collapse Viser panel"
          >
            ›
          </button>
        </div>
      </div>
      <div className="relative flex-1 bg-gray-950">
        {resizing && (
          <div
            className="absolute inset-0 z-10 cursor-col-resize bg-transparent"
            aria-hidden="true"
          />
        )}
        {blocked && (
          <div className="absolute inset-0 z-20 flex items-center justify-center bg-gray-950/90 p-6 text-center">
            <div className="max-w-sm rounded-lg border border-orange-700 bg-gray-900/95 p-4 shadow-xl">
              <div className="mb-2 text-sm font-semibold text-orange-300">
                Runtime Out Of Date
              </div>
              <p className="text-sm text-gray-300">
                The editor has unapplied changes. Apply, save, or discard them in the editor bar
                before using the embedded runtime controls.
              </p>
            </div>
          </div>
        )}
        {hmiAvailable === false ? (
          <div className="flex h-full items-center justify-center p-6 text-center">
            <div className="max-w-sm rounded-lg border border-gray-700 bg-gray-900/95 p-4 shadow-xl">
              <div className="mb-2 text-sm font-semibold text-gray-200">
                Viser HMI Not Running
              </div>
              <p className="text-sm text-gray-400">
                {hmiDetail}
              </p>
            </div>
          </div>
        ) : (
          <iframe
            title="Viser HMI"
            src={viserUrl}
            className={`h-full w-full border-0 bg-gray-950 ${resizing ? 'pointer-events-none' : ''}`}
          />
        )}
      </div>
    </div>
  );
}
