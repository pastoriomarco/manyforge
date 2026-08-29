import { useEffect, useRef, useState } from 'react';

export interface RuntimeAttentionItem {
  identity: string;
  title: string;
  source: 'backend' | 'composer';
}

/** A compact presentation cue. Dismissal has no acknowledgement side effect. */
export function RuntimeAttentionAlert({
  enabled,
  items,
  onOpen,
}: {
  enabled: boolean;
  items: RuntimeAttentionItem[];
  onOpen: () => void;
}) {
  const presented = useRef(new Set<string>());
  const [visible, setVisible] = useState<RuntimeAttentionItem | null>(null);

  useEffect(() => {
    if (!enabled) {
      setVisible(null);
      return;
    }
    const currentIdentities = new Set(items.map((item) => item.identity));
    presented.current.forEach((identity) => {
      if (!currentIdentities.has(identity)) presented.current.delete(identity);
    });
    setVisible((current) => (
      current !== null && items.some((item) => item.identity === current.identity)
        ? current
        : null
    ));
    const unpresented = items.filter((item) => !presented.current.has(item.identity));
    if (unpresented.length === 0) return;
    items.forEach((item) => presented.current.add(item.identity));
    setVisible(unpresented[0]);
  }, [enabled, items]);

  if (visible === null) return null;
  return (
    <aside className="runtime-attention-alert" role="status" aria-live="polite">
      <span className="runtime-attention-dot" aria-hidden="true" />
      <button type="button" className="runtime-attention-open" onClick={onOpen}>
        <strong>{visible.title}</strong>
        <small>{visible.source === 'backend' ? 'Backend alarm' : 'Composer session error'}</small>
      </button>
      <button
        type="button"
        className="icon-button"
        aria-label="Dismiss attention alert"
        title="Dismiss this alert without acknowledging the underlying condition"
        onClick={() => setVisible(null)}
      >×</button>
    </aside>
  );
}
