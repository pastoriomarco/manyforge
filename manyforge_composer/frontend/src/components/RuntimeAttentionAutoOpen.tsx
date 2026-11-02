import { useEffect, useRef } from 'react';

export interface RuntimeAttentionItem {
  identity: string;
}

export function unpresentedRuntimeAttention(
  presented: ReadonlySet<string>,
  items: RuntimeAttentionItem[],
): RuntimeAttentionItem[] {
  return items.filter((item) => !presented.has(item.identity));
}

export function retainActiveRuntimeAttention(
  presented: Set<string>,
  items: RuntimeAttentionItem[],
): void {
  const active = new Set(items.map((item) => item.identity));
  presented.forEach((identity) => {
    if (!active.has(identity)) presented.delete(identity);
  });
}

export function shouldCloseAutomaticRuntimeAttention(
  automatic: boolean,
  activePanel: 'runtime' | 'assistant' | null,
  attentionCount: number,
): boolean {
  return automatic && activePanel === 'runtime' && attentionCount === 0;
}

/**
 * Opens the one runtime/alarms surface once for each new acknowledgement- or
 * dismissal-required item. Ordinary warning state is deliberately absent.
 */
export function RuntimeAttentionAutoOpen({
  items,
  onOpen,
}: {
  items: RuntimeAttentionItem[];
  onOpen: (identity: string) => void;
}) {
  const presented = useRef(new Set<string>());

  useEffect(() => {
    retainActiveRuntimeAttention(presented.current, items);
    const unpresented = unpresentedRuntimeAttention(presented.current, items);
    items.forEach((item) => presented.current.add(item.identity));
    if (unpresented.length > 0) onOpen(unpresented[0].identity);
  }, [items, onOpen]);

  return null;
}
