import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { useViewerSelectionStore } from '../store/viewerSelectionStore';
import { RobotControlPanel } from './RobotControlPanel';

const candidateStoreState = vi.hoisted(() => ({
  candidate: {
    candidateId: 'joint-candidate',
    generation: 1,
    sourceSurface: 'joint_slider',
    inputMode: 'joint',
    status: 'ready',
    commitEligible: true,
    diagnostics: { reasonCode: 'ready', summary: 'Ready to execute.' },
    ghost: {
      blocked: false,
      finalJointNames: ['joint_1'],
      finalJointPositionsRad: [0.2],
    },
    maxAgeNs: 30_000_000_000,
    expiresAtNs: 0,
    createdAtNs: 0,
    updatedAtNs: 0,
  },
  busy: false,
  approving: false,
  rejection: null,
  error: null,
  author: vi.fn(),
  rejectDraft: vi.fn(),
  approve: vi.fn(),
  revert: vi.fn(),
  clear: vi.fn(),
}));

vi.mock('../store/robotControlCandidateStore', () => {
  return {
    useRobotControlCandidateStore: <T,>(
      selector: (state: typeof candidateStoreState) => T,
    ): T => selector(candidateStoreState),
  };
});

describe('RobotControlPanel layout', () => {
  it('keeps a ready joint candidate footer outside the bounded scroll region', () => {
    useViewerSelectionStore.setState({ robotMode: 'joint' });

    const markup = renderToStaticMarkup(
      <RobotControlPanel onClose={() => undefined} />,
    );

    expect(markup).toContain('data-testid="robot-control-panel"');
    expect(markup).toContain('min-h-0 max-h-');
    expect(markup).toContain('data-testid="robot-control-joint-scroll"');
    expect(markup).toContain('min-h-0 flex-1 overflow-y-auto');
    expect(markup).toContain('data-testid="robot-control-candidate-footer"');
    expect(markup).toContain('shrink-0 border-t');
    expect(markup).toContain('Approve &amp; Execute');
  });
});
