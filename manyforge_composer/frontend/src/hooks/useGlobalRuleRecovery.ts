import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { api, ApiError } from '../api/client';
import {
  globalRuleLatchFromOffer,
  type GlobalRuleLatch,
  type GlobalRuleLatchClearStatus,
} from '../state/globalRuleLatches';
import type { GlobalRuleRecoveryOffer } from '../types/robotControl';

const OBSERVATION_INTERVAL_MS = 1000;

interface ClearEntry extends GlobalRuleLatchClearStatus {
  /** Offer reads started up to this count predate the result. */
  settledAtRead: number;
}

export interface GlobalRuleRecoveryController {
  latches: GlobalRuleLatch[];
  /** Absent while no execution owner epoch is known. */
  clear?: (latch: GlobalRuleLatch) => void;
}

function requestId(): string {
  return globalThis.crypto?.randomUUID?.()
    ?? `composer-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

function errorText(error: unknown): string {
  return error instanceof Error && error.message.trim() ? error.message : 'no detail';
}

/**
 * Latched global rules (contracts 78/86) from the owner's get_stop_fault
 * offers, and the one operator Clear latch command. The clear sends the
 * current offer's exact selector once; an unknown outcome is reported and
 * blocks another Clear until a later offer read, it is never replayed.
 */
export function useGlobalRuleRecovery(
  enabled: boolean,
  ownerEpoch: string | null,
): GlobalRuleRecoveryController {
  const [offers, setOffers] = useState<GlobalRuleRecoveryOffer[]>([]);
  const [clears, setClears] = useState<Record<string, ClearEntry>>({});
  const [reloadToken, setReloadToken] = useState(0);
  const offersRef = useRef<GlobalRuleRecoveryOffer[]>([]);
  const clearsRef = useRef<Record<string, ClearEntry>>({});
  const readsStarted = useRef(0);

  useEffect(() => {
    if (!enabled) {
      offersRef.current = [];
      setOffers([]);
      return undefined;
    }
    let active = true;
    let timeout: ReturnType<typeof setTimeout> | null = null;
    const controller = new AbortController();
    const observe = async () => {
      const read = ++readsStarted.current;
      try {
        const next = await api.execution.globalRuleRecoveryOffers(controller.signal);
        if (!active) return;
        offersRef.current = next;
        setOffers(next);
        const offered = new Set(next.map((offer) => offer.selector.rule_id));
        const kept: Record<string, ClearEntry> = {};
        for (const [ruleId, entry] of Object.entries(clearsRef.current)) {
          if (entry.pending) kept[ruleId] = entry;
          else if (offered.has(ruleId)) {
            kept[ruleId] = read > entry.settledAtRead
              ? { ...entry, outcomeUnknown: false } : entry;
          }
        }
        clearsRef.current = kept;
        setClears(kept);
      } catch {
        if (!active) return;
        // Unreadable offers are not proof of no latch; show none (the robot
        // status notice reports an unavailable owner) and retry.
        offersRef.current = [];
        setOffers([]);
      }
      if (active) timeout = setTimeout(observe, OBSERVATION_INTERVAL_MS);
    };
    void observe();
    return () => {
      active = false;
      controller.abort();
      if (timeout !== null) clearTimeout(timeout);
    };
  }, [enabled, reloadToken]);

  const clear = useCallback((latch: GlobalRuleLatch) => {
    if (ownerEpoch === null) return;
    const offer = offersRef.current.find((row) => row.selector.rule_id === latch.ruleId);
    if (offer === undefined || offer.currentCondition !== false) return;
    const ruleId = latch.ruleId;
    const existing = clearsRef.current[ruleId];
    if (existing?.pending || existing?.outcomeUnknown) return;
    const update = (entry: ClearEntry) => {
      clearsRef.current = { ...clearsRef.current, [ruleId]: entry };
      setClears(clearsRef.current);
    };
    update({ pending: true, message: null, outcomeUnknown: false, settledAtRead: Infinity });
    const settle = (entry: Omit<ClearEntry, 'pending' | 'settledAtRead'>) => update({
      ...entry, pending: false, settledAtRead: readsStarted.current,
    });
    void api.execution.clearGlobalRuleLatch(ownerEpoch, requestId(), offer).then((result) => {
      settle({
        outcomeUnknown: false,
        message: result.accepted
          ? `Clear accepted${result.ruleState === null ? '' : `; the rule is now ${result.ruleState}`}.`
          : `Clear rejected: ${result.error ?? 'no reason given'}.`,
      });
    }, (error: unknown) => {
      const known = error instanceof ApiError && error.status > 0 && error.status < 500;
      settle(known
        ? { outcomeUnknown: false, message: `Clear rejected: ${errorText(error)}.` }
        : {
          outcomeUnknown: true,
          message: `Clear request outcome unknown (${errorText(error)}); it was not resent.`,
        });
    }).finally(() => setReloadToken((value) => value + 1));
  }, [ownerEpoch]);

  const latches = useMemo(
    () => offers.map((offer) => globalRuleLatchFromOffer(offer, clears[offer.selector.rule_id])),
    [clears, offers],
  );
  return ownerEpoch === null ? { latches } : { latches, clear };
}
