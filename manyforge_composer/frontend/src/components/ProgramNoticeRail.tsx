import { useEffect, useLayoutEffect, useRef, useState } from 'react';

export type ProgramNoticeKind = 'info' | 'error';

export interface ProgramNotice {
  id: string;
  key: string;
  kind: ProgramNoticeKind;
  message: string;
  dismissible: boolean;
}

export interface ProgramNoticeInput {
  key: string;
  message: string;
  kind?: ProgramNoticeKind;
  dismissible?: boolean;
}

interface ProgramNoticeRailProps {
  notices: ProgramNotice[];
  onDismiss: (id: string) => void;
  onDismissAll: () => void;
}

/** Bounded Program-canvas feedback. Backend truth remains in its owning surface. */
export function ProgramNoticeRail({
  notices,
  onDismiss,
  onDismissAll,
}: ProgramNoticeRailProps) {
  const [expanded, setExpanded] = useState(false);
  const [singleOverflow, setSingleOverflow] = useState(false);
  const compactText = useRef<HTMLSpanElement>(null);
  const expandable = notices.length > 1 || singleOverflow;
  useEffect(() => {
    if (!expandable) setExpanded(false);
  }, [expandable]);
  const restoreFocus = () => window.requestAnimationFrame(() => {
    const target = document.querySelector<HTMLElement>(
      '.program-notice-rail button, .program-canvas-corner-actions button',
    );
    target?.focus();
  });
  const dismiss = (id: string) => {
    onDismiss(id);
    restoreFocus();
  };
  useLayoutEffect(() => {
    const element = compactText.current;
    if (element === null || notices.length !== 1) {
      setSingleOverflow(false);
      return undefined;
    }
    const measure = () => {
      const next = element.scrollWidth > element.clientWidth + 1;
      setSingleOverflow((current) => current === next ? current : next);
    };
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(element);
    return () => observer.disconnect();
  }, [notices]);
  if (notices.length === 0) return null;
  const hasErrors = notices.some((notice) => notice.kind === 'error');
  const primary = notices.find((notice) => notice.kind === 'error') ?? notices[0];
  const dismissible = notices.filter((notice) => notice.dismissible);
  return (
    <section
      className={`program-notice-rail${expanded ? ' expanded' : ''}${hasErrors ? ' has-error' : ''}`}
      aria-label="Program canvas notices"
    >
      <div
        className="program-notice-summary"
        role={hasErrors ? 'alert' : 'status'}
        aria-live={hasErrors ? 'assertive' : 'polite'}
      >
        <span ref={compactText} className="program-notice-summary-text">
          {notices.length > 1 ? `${notices.length} notices · ${primary.message}` : primary.message}
        </span>
        {expandable && (
          <button
            type="button"
            className="program-notice-expand"
            aria-expanded={expanded}
            aria-label={expanded ? 'Collapse Program notices' : 'Expand Program notices'}
            onClick={() => setExpanded((current) => !current)}
          >
            {expanded ? '▴' : '▾'}
          </button>
        )}
        {notices.length === 1 ? (
          primary.dismissible && (
            <button
              type="button"
              className="program-notice-dismiss"
              aria-label="Dismiss Program notice"
              onClick={() => dismiss(primary.id)}
            >
              ×
            </button>
          )
        ) : dismissible.length > 0 && (
          <button type="button" className="program-notice-dismiss-all" onClick={() => {
            onDismissAll();
            restoreFocus();
          }}>
            Dismiss all
          </button>
        )}
      </div>
      {expanded && (
        <div className="program-notice-list">
          {notices.map((notice) => (
            <div
              key={notice.id}
              className={`program-notice-item notice-${notice.kind}`}
              role={notice.kind === 'error' ? 'alert' : 'status'}
            >
              <span>{notice.message}</span>
              {notice.dismissible && (
                <button
                  type="button"
                  aria-label={`Dismiss notice: ${notice.message}`}
                  onClick={() => dismiss(notice.id)}
                >
                  ×
                </button>
              )}
            </div>
          ))}
        </div>
      )}
    </section>
  );
}
