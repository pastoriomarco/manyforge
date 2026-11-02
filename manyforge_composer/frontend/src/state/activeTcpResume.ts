import type { ProgramBindingDocument } from '../types/programBindingTransport.generated';
import type { DirectControlMotionStatus, ExecutionOwnerStatus } from '../types/robotControl';

export interface ResumeTcpChange {
  robot: string;
  oldDisplay: string;
  newDisplay: string;
}

type ActiveTcp = NonNullable<DirectControlMotionStatus['activeTcp']>;
type RobotTcp = { robot: string; active: ActiveTcp };
type TcpSnapshot = Map<string, RobotTcp>;

function snapshot(
  binding: ProgramBindingDocument | null,
  status: ExecutionOwnerStatus | null,
  graphRevision: string,
  retainedBundleId: string,
): TcpSnapshot {
  const result: TcpSnapshot = new Map();
  if (binding === null || status === null || status.graphRevision !== graphRevision
      || status.retainedBundleId !== retainedBundleId) return result;
  for (const row of binding.target_role_bindings) {
    if (row.robot_tcps === undefined) continue;
    const matches = status.directControlTargets.filter((target) => (
      target.controllableComponentId === row.controllable_component_id
      && target.commandTargetId === row.command_target_id
      && target.operationCapabilityId === 'execution.trajectory.execute'
    ));
    if (matches.length !== 1) continue;
    const active = matches[0].activeTcp;
    if (active === null || active.ownerEpoch !== status.ownerEpoch) continue;
    result.set(row.controllable_component_id, { robot: row.controllable_component_id,
      active: { ...active, entry: active.entry === null ? null : { ...active.entry } } });
  }
  return result;
}

function entryKey(active: ActiveTcp): string | null {
  return active.entry === null ? null : JSON.stringify([
    active.entry.componentId, active.entry.targetId, active.entry.frameRoleId,
  ]);
}

/** Browser-memory only: the first observation of a run is never a hold edge. */
export class ActiveTcpHoldHistory {
  private runId: string | null = null;
  private held = new Set<string>();
  private snapshots = new Map<string, TcpSnapshot>();
  private tainted = new Set<string>();

  clear(): void {
    this.runId = null;
    this.held.clear();
    this.snapshots.clear();
    this.tainted.clear();
  }

  observe(runId: string | null, held: readonly string[], current: TcpSnapshot,
    unknown: readonly string[] = [], locallyStarted = false): void {
    const next = new Set(held);
    if (runId !== this.runId) {
      this.runId = runId;
      this.held = next;
      this.snapshots.clear();
      this.tainted.clear();
      if (locallyStarted) for (const key of next) this.snapshots.set(key, current);
      this.invalidateOverlaps(next);
      return;
    }
    for (const key of next) {
      if (!this.held.has(key) && !this.tainted.has(key)) this.snapshots.set(key, current);
    }
    for (const key of this.held) {
      if (!next.has(key) && !unknown.includes(key)) {
        this.snapshots.delete(key);
        this.tainted.delete(key);
      }
    }
    this.held = new Set([...next, ...[...this.held].filter((key) => unknown.includes(key))]);
    for (const baseline of this.snapshots.values()) {
      for (const [robot, old] of baseline) {
        const now = current.get(robot);
        if (now !== undefined && old.active.ownerEpoch !== now.active.ownerEpoch) baseline.delete(robot);
      }
    }
    this.invalidateOverlaps(this.held);
  }

  isOverlapping(held: readonly string[], unknown: readonly string[] = []): boolean {
    const effective = new Set([...held, ...[...this.held].filter((key) => unknown.includes(key))]);
    const named = [...effective].filter((key) => key.startsWith('group:') && key !== 'group:all');
    return named.length > 1 || (effective.has('full') && (named.length > 0 || effective.has('group:all')));
  }

  private invalidateOverlaps(held: Set<string>): void {
    if (this.isOverlapping([...held])) {
      for (const key of held) {
        this.snapshots.delete(key);
        this.tainted.add(key);
      }
    }
  }

  changes(key: string, current: TcpSnapshot, runId = this.runId): ResumeTcpChange[] {
    if (runId !== this.runId) return [];
    const previous = this.snapshots.get(key);
    if (previous === undefined || !this.held.has(key)) return [];
    const changes: ResumeTcpChange[] = [];
    for (const [robot, old] of previous) {
      const now = current.get(robot);
      if (now === undefined || old.active.ownerEpoch !== now.active.ownerEpoch) continue;
      if (entryKey(old.active) !== entryKey(now.active)) {
        changes.push({ robot, oldDisplay: old.active.display ?? 'No TCP',
          newDisplay: now.active.display ?? 'No TCP' });
      }
    }
    return changes.sort((a, b) => a.robot.localeCompare(b.robot));
  }
}

export function activeTcpSnapshot(
  binding: ProgramBindingDocument | null, status: ExecutionOwnerStatus | null,
  graphRevision: string, retainedBundleId: string,
): TcpSnapshot {
  return snapshot(binding, status, graphRevision, retainedBundleId);
}
