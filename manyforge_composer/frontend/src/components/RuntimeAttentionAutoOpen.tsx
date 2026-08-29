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
    const unpresented = unpresentedRuntimeAttention(presented.current, items);
    items.forEach((item) => presented.current.add(item.identity));
    if (unpresented.length > 0) onOpen(unpresented[0].identity);
  }, [items, onOpen]);

  return null;
}
