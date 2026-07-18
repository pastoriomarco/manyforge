import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import type { NodeDiscoveryReport } from '../types/api';
import { NodeDiscoveryDiagnostics } from './NodeDiscoveryDiagnostics';

const report: NodeDiscoveryReport = {
  status: 'rejected',
  attemptedAtNs: 123,
  roots: ['/workspace/manyforge_nodes'],
  strict: true,
  previousCatalogHash: 'sha256:old',
  effectiveCatalogHash: null,
  activeEffectiveCatalogHash: 'sha256:active',
  activeGeneration: 4,
  activated: false,
  conflict: 'active_artifact_validation_failed',
  summary: {
    accepted: 1,
    rejected: 1,
    added: 0,
    changed: 0,
    removed: 1,
    unchanged: 40,
  },
  results: [
    {
      path: '<active-artifacts>',
      nodeId: 'vendor_removed',
      version: '0.1.0',
      status: 'rejected',
      errorCode: 'active_artifact_validation_failed',
      messages: ['loaded program still uses vendor_removed'],
    },
  ],
};

describe('NodeDiscoveryDiagnostics', () => {
  it('renders active provenance and structured rejection details', () => {
    const markup = renderToStaticMarkup(
      <NodeDiscoveryDiagnostics report={report} error="Reload rejected" />,
    );

    expect(markup).toContain('Reload rejected');
    expect(markup).toContain('REJECTED');
    expect(markup).toContain('1 accepted · 1 rejected');
    expect(markup).toContain('sha256:active');
    expect(markup).toContain('vendor_removed');
    expect(markup).toContain('loaded program still uses vendor_removed');
    expect(markup).toContain('text-red-300');
  });
});
