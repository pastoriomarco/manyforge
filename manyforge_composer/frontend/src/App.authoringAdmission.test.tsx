import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import { WorkbenchNotificationStack } from './components/WorkbenchNotifications';
import {
  AuthoringAdmissionTooltipProvider,
  manualOperationAdmissionCopy,
  useAuthoringAdmissionTooltip,
} from './components/authoringAdmission';
import {
  classifyWorkbenchNotices,
  noticeRowSummary,
  useViewerConditionHold,
  type WorkbenchNotice,
} from './state/workbenchNotices';
import { readyState, statusFixture } from './testFixtures/robotControl';
import { robotControlReducer } from './state/robotControl';
import { authoringAdmissionNotices } from './App';
import { authoringAdmissionTooltip, replacementAdmissionAllowed } from './components/authoringAdmission';

function ViewerHoldProbe({ notices }: { notices: WorkbenchNotice[] }) {
  const held = useViewerConditionHold(notices);
  return <ul>{held.map((notice) => <li key={notice.id}>{notice.id}:{notice.severity}</li>)}</ul>;
}

function TooltipProbe() {
  return <button title={useAuthoringAdmissionTooltip()}>Run</button>;
}

describe('authoring admission status', () => {
  it('shows the generic Activity status only during a known background step', () => {
    const activity = authoringAdmissionNotices(false, null, true);
    expect(activity).toEqual([{
      id: 'authoring-admission',
      severity: 'info',
      domain: 'program_authoring',
      title: 'The cell is finishing a background step.',
      message: '',
      busy: true,
    }]);

    const markup = renderToStaticMarkup(
      <WorkbenchNotificationStack errorRow={null} warnings={[]} activity={activity} />,
    );
    expect(markup).toContain('<span class="notification-rail-summary" aria-live="polite">The cell is finishing a background step.</span>');
    expect(markup).toContain('<span class="notification-busy" aria-hidden="true"></span>');
    expect(authoringAdmissionNotices(false)).toEqual([]);
    expect(authoringAdmissionNotices(true, null, true)).toEqual([]);
  });

  it('names the owner target and accepted mode for a manual operation', () => {
    const activity = authoringAdmissionNotices(false, {
      ownerEpoch: 'owner.1',
      operationId: 'op.1',
      targetName: 'lite6_robot · arm',
      modeLabel: 'Move To',
      title: 'lite6_robot · arm is moving (Move To).',
      message: 'Program editing resumes when it ends.',
      tooltip: 'lite6_robot · arm is moving (Move To). Program editing resumes when it ends.',
    });
    expect(activity).toEqual([{
      id: 'authoring-admission.manual.owner.1.op.1',
      severity: 'info',
      domain: 'program_authoring',
      title: 'lite6_robot · arm is moving (Move To).',
      message: 'Program editing resumes when it ends.',
      busy: true,
    }]);
    const markup = renderToStaticMarkup(
      <WorkbenchNotificationStack errorRow={null} warnings={[]} activity={activity} />,
    );
    expect(markup).toContain('lite6_robot · arm is moving (Move To).');
    expect(markup).toContain('Program editing resumes when it ends.');
    expect(activity[0].actions).toBeUndefined();
  });

  it('uses an accepted mode only for the exact observed operation and leaves external mode unknown', () => {
    const base = readyState();
    const intent = base.intent;
    const status = base.status ?? statusFixture();
    if (intent === null) throw new Error('ready fixture needs a preview intent');
    const accepted = robotControlReducer(base, {
      type: 'operation_mode_accepted', generation: base.generation, ownerEpoch: status.ownerEpoch,
      requestId: intent.request_context.request_id, operationId: 'op.1', mode: 'move_to',
    });
    const participant = intent.participants[0];
    const operation = {
      operationId: 'op.1', operationRevision: 1, resident: true, state: 'executing' as const,
      progress: 0, activeStopId: null, activeStopGeneration: null, terminalError: null,
      controllableComponentId: participant.target.component_id, commandTargetId: participant.target.command_target_id,
    };
    const running = { ...accepted, status, phase: 'executing' as const, operation };
    expect(manualOperationAdmissionCopy(running)).toMatchObject({
      operationId: 'op.1', targetName: 'robot-a · arm', modeLabel: 'Move To',
      title: 'robot-a · arm is moving (Move To).',
      tooltip: 'robot-a · arm is moving (Move To). Program editing resumes when it ends.',
    });
    expect(manualOperationAdmissionCopy({ ...running, acceptedOperationMode: null })).toMatchObject({
      targetName: 'robot-a · arm', modeLabel: null,
      title: 'Manual motion on robot-a · arm is running.',
      tooltip: 'Manual motion on robot-a · arm is running. Program editing resumes when it ends.',
    });
    expect(manualOperationAdmissionCopy({
      ...running, acceptedOperationMode: null,
      operation: { ...operation, controllableComponentId: undefined, commandTargetId: undefined },
    })).toMatchObject({ targetName: null, title: 'Manual motion is running.' });
  });

  it('provides the operation-specific admission tooltip to blocked controls', () => {
    const tooltip = 'lite6_robot · arm is moving (Move To). Program editing resumes when it ends.';
    expect(renderToStaticMarkup(
      <AuthoringAdmissionTooltipProvider tooltip={tooltip}><TooltipProbe /></AuthoringAdmissionTooltipProvider>,
    )).toBe(`<button title="${tooltip}">Run</button>`);
  });

  it('lets New and Load through a held run but not through a background step', () => {
    // A run with all motion stopped: the server Resets it first.
    expect(replacementAdmissionAllowed(false, 'execution_active')).toBe(true);
    // An Apply, Reset or recovery holding the cell still blocks them.
    expect(replacementAdmissionAllowed(false, 'not_admitted')).toBe(false);
    expect(replacementAdmissionAllowed(false, null)).toBe(false);
    expect(replacementAdmissionAllowed(true, null)).toBe(true);
    expect(replacementAdmissionAllowed(null, null)).toBeNull();
  });

  it('uses generic background copy only for a known background step', () => {
    expect(authoringAdmissionTooltip(null, true, null))
      .toBe('The cell is finishing a background step; this is available again in a moment.');
    expect(authoringAdmissionTooltip(null, false, 'execution_active'))
      .toBe('Program execution is active; Program editing resumes when it ends.');
    expect(authoringAdmissionTooltip(null, false, null))
      .toBe('Authoring is currently unavailable.');
  });

  it('passes through transition and viewer-hold logic immediately, while a prior busy activity leads the collapsed summary', () => {
    const existing: WorkbenchNotice = {
      id: 'existing-operation', severity: 'info', domain: 'runtime',
      title: 'Existing operation', message: '', busy: true,
    };
    const classified = classifyWorkbenchNotices(
      [existing, ...authoringAdmissionNotices(false, null, true)],
      { key: 'activation.1', preexisting: new Set() },
    );
    expect(classified.map((notice) => notice.id)).toEqual(['existing-operation', 'authoring-admission']);
    expect(renderToStaticMarkup(<ViewerHoldProbe notices={classified} />)).toBe(
      '<ul><li>existing-operation:info</li><li>authoring-admission:info</li></ul>',
    );
    expect(noticeRowSummary(classified, { one: 'activity', many: 'activities' }))
      .toBe('2 activities · 2 in progress · Existing operation');
    const markup = renderToStaticMarkup(
      <WorkbenchNotificationStack errorRow={null} warnings={[]} activity={classified} />,
    );
    expect(markup).toContain('2 activities · 2 in progress · Existing operation');
  });

  it.each([true, null, undefined])('hides the status when admission is %s', (authoringAllowed) => {
    const activity = authoringAdmissionNotices(authoringAllowed, null, true);
    expect(activity).toEqual([]);
    expect(renderToStaticMarkup(
      <WorkbenchNotificationStack errorRow={null} warnings={[]} activity={activity} />,
    )).toBe('');
  });
});
