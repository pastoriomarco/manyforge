import { useSyncExternalStore } from 'react';
import type { HeadlessStateSource } from '../hooks/useHeadlessState';
import type { OperationObservation } from '../types/robotControl';
import { EMPTY_OPERATION_PROGRESS, operationProgressValue } from '../utils/operationProgress';

const subscribeNothing = () => () => undefined;
const emptySnapshot = () => EMPTY_OPERATION_PROGRESS;

export function OperationProgress({ operation, source }: {
  operation: OperationObservation;
  source?: HeadlessStateSource | null;
}) {
  const samples = useSyncExternalStore(
    source?.subscribeOperationProgress ?? subscribeNothing,
    source?.getOperationProgress ?? emptySnapshot,
    emptySnapshot,
  );
  const progress = operationProgressValue(operation, samples);
  return <>
    <progress aria-label="Operation progress" max={1} value={progress} />
    <span>
      {operation.state.replaceAll('_', ' ')} · {Math.round(progress * 100)}%
    </span>
  </>;
}
