import { useState, type ReactNode } from 'react';

import {
  noticeRowSummary,
  type NoticeSeverity,
  type WorkbenchNotice,
} from '../state/workbenchNotices';

const SEVERITY_LABEL: Record<NoticeSeverity, string> = {
  error: 'Errors',
  warning: 'Warnings',
  info: 'Activity',
};

/**
 * The collapsed header shared by the error, warning and activity rows. The
 * severity is always stated in text; colour only supplements it.
 */
export function NotificationRail({
  className = '',
  label,
  summary,
  busy = false,
  actions,
  expanded,
  onToggle,
  expandLabel,
  onClose,
  closeLabel,
}: {
  className?: string;
  label: string;
  summary: string;
  busy?: boolean;
  actions?: ReactNode;
  expanded: boolean;
  onToggle?: () => void;
  expandLabel: string;
  onClose?: () => void;
  closeLabel?: string;
}) {
  return (
    <header className={`notification-rail ${className}`.trim()}>
      <strong>{label}</strong>
      {busy && <span className="notification-busy" aria-hidden="true" />}
      <span className="notification-rail-summary" aria-live="polite">
        {summary}
      </span>
      <div className="notification-rail-actions">
        {actions}
        {onToggle !== undefined && (
          <button
            type="button"
            className="icon-button"
            aria-label={expanded ? `Collapse ${expandLabel}` : `Expand ${expandLabel}`}
            aria-expanded={expanded}
            title={expanded ? `Collapse ${expandLabel}` : `Expand ${expandLabel}`}
            onClick={onToggle}
          >
            {expanded ? '⌃' : '⌄'}
          </button>
        )}
        {onClose !== undefined && (
          <button
            type="button"
            className="icon-button"
            aria-label={closeLabel}
            title={closeLabel}
            onClick={onClose}
          >×</button>
        )}
      </div>
    </header>
  );
}

export function NoticeActions({ notice }: { notice: WorkbenchNotice }) {
  if (notice.actions === undefined || notice.actions.length === 0) return null;
  return (
    <>
      {notice.actions.map((action) => (
        <button
          key={action.id}
          type="button"
          disabled={action.disabled}
          title={action.title}
          onClick={action.run}
        >
          {action.label}
        </button>
      ))}
    </>
  );
}

/**
 * One notice with its source-supported actions; never an acknowledgement.
 * `children` are the alarms it represents, each with its own acknowledgement.
 */
export function NoticeItem({ notice, children }: { notice: WorkbenchNotice; children?: ReactNode }) {
  return (
    <div
      className={`workbench-notice severity-${notice.severity}${notice.busy ? ' busy' : ''}`}
      data-notice-id={notice.id}
      role={notice.severity === 'error' ? 'alert' : 'status'}
    >
      <p>
        <strong>{notice.title}</strong>
        {notice.message && <span>{notice.message}</span>}
      </p>
      {notice.details && (
        <details><summary>Details</summary><p>{notice.details}</p></details>
      )}
      {notice.actions !== undefined && notice.actions.length > 0 && (
        <div className="workbench-notice-actions">
          <NoticeActions notice={notice} />
        </div>
      )}
      {children && <div className="workbench-notice-alarms">{children}</div>}
    </div>
  );
}

/**
 * A warning or activity row. It exists only while it has notices, has no
 * acknowledgement, and expands independently of every other row.
 */
export function NoticeRow({
  severity,
  notices,
}: {
  severity: 'warning' | 'info';
  notices: readonly WorkbenchNotice[];
}) {
  const [expanded, setExpanded] = useState(false);
  if (notices.length === 0) return null;
  const single = notices.length === 1 ? notices[0] : undefined;
  const summary = noticeRowSummary(notices, severity === 'warning'
    ? { one: 'warning', many: 'warnings' }
    : { one: 'activity', many: 'activities' });
  return (
    <section
      className={`workbench-notice-row severity-${severity}${expanded ? ' expanded' : ' collapsed'}`}
      aria-label={SEVERITY_LABEL[severity]}
    >
      <NotificationRail
        label={SEVERITY_LABEL[severity]}
        summary={summary}
        busy={notices.some((notice) => notice.busy)}
        actions={single !== undefined && !expanded
          ? <NoticeActions notice={single} />
          : undefined}
        expanded={expanded}
        onToggle={single !== undefined && single.message.length < 90
          && (single.actions?.length ?? 0) === 0
          ? undefined
          : () => setExpanded((current) => !current)}
        expandLabel={`${SEVERITY_LABEL[severity].toLowerCase()} details`}
      />
      {expanded && (
        <div className="workbench-notice-list">
          {notices.map((notice) => <NoticeItem key={notice.id} notice={notice} />)}
        </div>
      )}
    </section>
  );
}

/**
 * The single notification location inside the viewer stage: errors (with the
 * Runtime/history view), then warnings, then activity.
 */
export function WorkbenchNotificationStack({
  errorRow,
  warnings,
  activity,
}: {
  errorRow: ReactNode;
  warnings: readonly WorkbenchNotice[];
  activity: readonly WorkbenchNotice[];
}) {
  if (errorRow === null && warnings.length === 0 && activity.length === 0) return null;
  return (
    <div className="workbench-notification-stack" aria-label="Workbench notifications">
      {errorRow}
      <NoticeRow severity="warning" notices={warnings} />
      <NoticeRow severity="info" notices={activity} />
    </div>
  );
}
