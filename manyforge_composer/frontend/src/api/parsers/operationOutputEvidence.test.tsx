import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import { OperationProgress } from '../../components/OperationProgress';
import { parseOperationObservation } from './execution';

const provider = {
  progress: 1,
  terminal_error: null,
  control_binding_id: null,
  control_binding_revision: null,
  capability_provider_binding_id: 'io.1.provider',
  capability_provider_binding_revision: 'sha256:provider',
};

const read = (participant: Record<string, unknown>) => ({
  ok: true,
  command: 'read',
  resident: true,
  operation: {
    operation_id: 'operation-1',
    operation_revision: 2,
    state: 'completed',
    participants: [participant],
    active_stop_id: null,
    active_stop_generation: null,
  },
});

describe('provider participants and output evidence (SX-2119)', () => {
  it.each([
    'not_submitted', 'submitted', 'source_accepted', 'source_rejected', 'acceptance_unknown',
  ])('reads %s exactly', (evidence) => {
    expect(parseOperationObservation(read({ ...provider, output_evidence: evidence })).outputEvidence)
      .toBe(evidence);
  });

  it('omits absent evidence and keeps controller participants unchanged', () => {
    expect('outputEvidence' in parseOperationObservation(read(provider))).toBe(false);
    expect(parseOperationObservation(read({
      progress: 1,
      terminal_error: null,
      control_binding_id: 'arm.binding',
      control_binding_revision: 'sha256:binding',
      capability_provider_binding_id: null,
      capability_provider_binding_revision: null,
    })).state).toBe('completed');
  });

  it.each([
    ['explicit null evidence', { ...provider, output_evidence: null }],
    ['unknown evidence', { ...provider, output_evidence: 'feedback_observed' }],
    ['provider without binding', { ...provider, capability_provider_binding_id: null }],
    ['half-null control binding', { ...provider, control_binding_revision: 'sha256:binding' }],
  ])('refuses %s', (_name, participant) => {
    expect(() => parseOperationObservation(read(participant))).toThrow();
  });

  it('shows the evidence beside the existing operation state', () => {
    const operation = parseOperationObservation(read({ ...provider, output_evidence: 'source_accepted' }));
    const html = renderToStaticMarkup(<OperationProgress operation={operation} ownerEpoch="epoch" />);
    expect(html).toContain('completed · 100%');
    expect(html).toContain('output source accepted');
    const plain = parseOperationObservation(read(provider));
    expect(renderToStaticMarkup(<OperationProgress operation={plain} ownerEpoch="epoch" />))
      .not.toContain('output');
  });
});
