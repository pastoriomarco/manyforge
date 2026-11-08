import { createContext, createElement, useContext, type ReactNode } from 'react';

import type { AuthoringUnavailableCause } from '../state/authoringWorkspace';
import {
  ROBOT_CONTROL_MODE_LABELS,
  operationStateIsTerminal,
  robotControlAcceptedOperationMode,
  robotControlHasActiveOperation,
  type RobotControlState,
} from '../state/robotControl';

export const AUTHORING_ADMISSION_TOOLTIP = 'The cell is finishing a background step; this is available again in a moment.';

export interface ManualOperationAdmissionCopy {
  ownerEpoch: string;
  operationId: string;
  targetName: string | null;
  modeLabel: string | null;
  title: string;
  message: string;
  tooltip: string;
}

/** Use the operation's owner target and only a mode tied to its accepted id. */
export function manualOperationAdmissionCopy(
  state: RobotControlState,
): ManualOperationAdmissionCopy | null {
  const operation = state.operation;
  if (operation === null || operationStateIsTerminal(operation.state)
    || !robotControlHasActiveOperation(state)) return null;
  const targetName = typeof operation.controllableComponentId === 'string'
    && operation.controllableComponentId !== ''
    && typeof operation.commandTargetId === 'string'
    && operation.commandTargetId !== ''
    ? `${operation.controllableComponentId} · ${operation.commandTargetId}`
    : null;
  const mode = robotControlAcceptedOperationMode(state);
  const modeLabel = mode === null ? null : ROBOT_CONTROL_MODE_LABELS[mode];
  const activity = targetName === null
    ? `Manual motion is running${modeLabel === null ? '' : ` (${modeLabel})`}.`
    : modeLabel === null
      ? `Manual motion on ${targetName} is running.`
      : `${targetName} is moving (${modeLabel}).`;
  const message = 'Program editing resumes when it ends.';
  return {
    ownerEpoch: state.status?.ownerEpoch ?? 'unknown-owner',
    operationId: operation.operationId,
    targetName,
    modeLabel,
    title: activity,
    message,
    tooltip: `${activity} ${message}`,
  };
}

const AuthoringAdmissionTooltipContext = createContext(AUTHORING_ADMISSION_TOOLTIP);

export function AuthoringAdmissionTooltipProvider({
  tooltip,
  children,
}: {
  tooltip: string;
  children: ReactNode;
}) {
  return createElement(AuthoringAdmissionTooltipContext.Provider, { value: tooltip }, children);
}

export function useAuthoringAdmissionTooltip(): string {
  return useContext(AuthoringAdmissionTooltipContext);
}

/** Why authoring controls are disabled, in the order the causes are known. */
export function authoringAdmissionTooltip(
  manualOperation: ManualOperationAdmissionCopy | null,
  backgroundStep: boolean,
  authoringCause: AuthoringUnavailableCause | null,
): string {
  if (manualOperation !== null) return manualOperation.tooltip;
  if (backgroundStep) return AUTHORING_ADMISSION_TOOLTIP;
  if (authoringCause === 'execution_active') return 'Program execution is active; Program editing resumes when it ends.';
  return 'Authoring is currently unavailable.';
}

/**
 * Admission for New and Load. They wait while a background step (an Apply,
 * Reset or recovery) holds the cell, like any edit. A Program run whose
 * motion is all stopped doesn't block them: the server Resets it first
 * (maintainer, 2026-11-06). Moving robots block them separately.
 */
export function replacementAdmissionAllowed(
  authoringAdmissionAllowed: boolean | null,
  authoringCause: AuthoringUnavailableCause | null,
): boolean | null {
  return authoringAdmissionAllowed === false && authoringCause === 'execution_active'
    ? true : authoringAdmissionAllowed;
}
