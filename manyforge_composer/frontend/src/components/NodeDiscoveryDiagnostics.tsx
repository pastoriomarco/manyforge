import type { NodeDiscoveryReport } from '../types/api';

export function NodeDiscoveryDiagnostics({
  report,
  error,
}: {
  report: NodeDiscoveryReport | null;
  error: string | null;
}) {
  return (
    <>
      {error && (
        <div className="rounded border border-red-800 bg-red-950/40 p-2 text-xs text-red-200">
          {error}
        </div>
      )}
      {report && (
        <div className="space-y-1 rounded border border-gray-700 bg-gray-900/40 p-2 text-xs text-gray-300">
          <div className="flex justify-between gap-2">
            <span className="font-mono">{report.status.toUpperCase()}</span>
            <span>{report.summary.accepted} accepted · {report.summary.rejected} rejected</span>
          </div>
          <div className="break-all font-mono text-[10px] text-gray-500">
            {report.activeEffectiveCatalogHash}
          </div>
          {report.results.map((result) => (
            <div
              key={`${result.path}:${result.nodeId ?? ''}:${result.status}`}
              className={result.status === 'rejected' ? 'text-red-300' : result.status === 'warning' ? 'text-amber-300' : 'text-gray-400'}
            >
              <span className="font-mono">{result.nodeId ?? result.path}</span>
              {result.messages.length > 0 ? `: ${result.messages.join('; ')}` : ''}
            </div>
          ))}
        </div>
      )}
    </>
  );
}
