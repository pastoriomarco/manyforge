import { useEffect, useMemo, useState } from 'react';
import { api } from '../api/client';

const DEFAULT_HMI_PORT = '8081';

function getViserUrl(port: string): string {
  if (import.meta.env.VITE_HMI_URL) {
    return import.meta.env.VITE_HMI_URL;
  }

  if (typeof window === 'undefined') {
    return `http://localhost:${port}`;
  }

  const { protocol, hostname } = window.location;
  return `${protocol}//${hostname}:${port}`;
}

interface ViserPanelProps {
  onCollapse: () => void;
  blocked: boolean;
  resizing?: boolean;
}

export function ViserPanel({ onCollapse, blocked, resizing = false }: ViserPanelProps) {
  const [hmiPort, setHmiPort] = useState(DEFAULT_HMI_PORT);
  const [hmiAvailable, setHmiAvailable] = useState<boolean | null>(null);
  const [hmiDetail, setHmiDetail] = useState<string | null>(null);

  useEffect(() => {
    let active = true;

    void api.infra.status()
      .then((status) => {
        if (!active) {
          return;
        }
        const hmi = status.components.hmi;
        const port = hmi?.port;
        if (hmi?.status === 'healthy' && typeof port === 'number' && port > 0) {
          setHmiPort(String(port));
          setHmiAvailable(true);
          setHmiDetail(null);
          return;
        }
        setHmiAvailable(false);
        setHmiDetail('Start Composer with --hmi-port 8081 to enable the embedded Viser view.');
      })
      .catch(() => {
        if (active) {
          // Fall back to the default HMI port when infra status is unavailable.
          setHmiAvailable(null);
          setHmiDetail(null);
        }
      });

    return () => {
      active = false;
    };
  }, []);

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
