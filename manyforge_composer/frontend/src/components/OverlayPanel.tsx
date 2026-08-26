import {
  useCallback,
  useEffect,
  useRef,
  type MouseEvent,
  type ReactNode,
  type RefObject,
} from 'react';

interface OverlayPanelProps {
  open: boolean;
  title: string;
  titleId: string;
  kind?: 'dialog' | 'info' | 'status';
  compact?: boolean;
  onClose: () => void;
  returnFocusRef?: RefObject<HTMLElement | null>;
  children: ReactNode;
}

/**
 * One bounded, dismissible presentation layer for document workflows and
 * information/status detail. It owns no workflow or backend state.
 */
export function OverlayPanel({
  open,
  title,
  titleId,
  kind = 'dialog',
  compact = false,
  onClose,
  returnFocusRef,
  children,
}: OverlayPanelProps) {
  const closeButton = useRef<HTMLButtonElement>(null);
  const close = useCallback(() => {
    onClose();
    window.requestAnimationFrame(() => returnFocusRef?.current?.focus());
  }, [onClose, returnFocusRef]);

  useEffect(() => {
    if (!open) return undefined;
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return;
      event.preventDefault();
      close();
    };
    document.addEventListener('keydown', handleKeyDown);
    window.requestAnimationFrame(() => closeButton.current?.focus());
    return () => document.removeEventListener('keydown', handleKeyDown);
  }, [close, open]);

  if (!open) return null;
  const dismissOutside = (event: MouseEvent<HTMLDivElement>) => {
    if (event.target === event.currentTarget) close();
  };

  return (
    <div
      className={`overlay-scrim overlay-${kind}`}
      onMouseDown={dismissOutside}
      role="presentation"
    >
      <section
        className={`overlay-panel${compact ? ' overlay-panel-compact' : ''}`}
        role="dialog"
        aria-modal={kind === 'dialog'}
        aria-labelledby={titleId}
      >
        <header className="overlay-panel-header">
          <h2 id={titleId}>{title}</h2>
          <button
            ref={closeButton}
            type="button"
            className="icon-button overlay-close"
            aria-label={`Close ${title}`}
            title={`Close ${title}`}
            onClick={close}
          >
            ×
          </button>
        </header>
        <div className="overlay-panel-body">{children}</div>
      </section>
    </div>
  );
}
