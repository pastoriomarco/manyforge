import { useCallback, useEffect, useRef, useState } from 'react';

import {
  subscribePointCloudObserver,
  type PointCloudFrame,
} from '../api/pointcloudObserverClient';
import {
  consumeLatestFrame,
  emptyLatestFrameSlot,
  invalidateLatestFrame,
  publishLatestFrame,
} from '../state/latestFrameSlot';

export interface PointCloudFrameState {
  frame: PointCloudFrame | null;
  error: string | null;
  clear: () => void;
}

export function usePointCloudFrame(
  enabled: boolean,
  cellId: string | undefined,
): PointCloudFrameState {
  const [frame, setFrame] = useState<PointCloudFrame | null>(null);
  const [error, setError] = useState<string | null>(null);
  const slot = useRef(emptyLatestFrameSlot<PointCloudFrame>());
  const animation = useRef<number | null>(null);
  const clear = useCallback(() => {
    slot.current = invalidateLatestFrame(slot.current);
    setFrame(null);
  }, []);

  useEffect(() => {
    if (!enabled || cellId === undefined) {
      slot.current = invalidateLatestFrame(slot.current);
      setFrame(null);
      setError(null);
      return undefined;
    }
    const flush = () => {
      animation.current = null;
      const consumed = consumeLatestFrame(slot.current);
      slot.current = consumed.state;
      if (consumed.changed) setFrame(consumed.frame);
    };
    const unsubscribe = subscribePointCloudObserver(
      cellId,
      (observation) => {
        slot.current = publishLatestFrame(slot.current, observation);
        setError(null);
        if (animation.current === null) {
          animation.current = requestAnimationFrame(flush);
        }
      },
      (reason) => setError(reason.message),
    );
    return () => {
      unsubscribe();
      if (animation.current !== null) cancelAnimationFrame(animation.current);
      animation.current = null;
      slot.current = invalidateLatestFrame(slot.current);
      setFrame(null);
    };
  }, [cellId, enabled]);

  return { frame, error, clear };
}
