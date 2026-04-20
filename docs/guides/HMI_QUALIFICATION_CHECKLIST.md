> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# HMI Qualification Checklist

This checklist defines the current visual qualification boundary for ManyForge.

## Why This Exists

The backend already exposes machine-readable scene truth and reconciliation
surfaces:

- `/api/scene/diagnostics`
- `/api/debug/scene-trace`
- `/api/scene/snapshot`

Those surfaces catch a large class of visual regressions automatically, but not
all of them. The remaining browser/HMI qualification still needs an explicit
manual release checklist.

## Automated Baseline

Every qualification run should capture and retain:

- execution digest
- execution history
- blackboard snapshot
- scene diagnostics
- scene trace
- scene snapshot response summary when HMI rendering is available

Automated pass criteria:

- backend and HMI scene diagnostics report no mismatches
- required rendered objects are present when HMI diagnostics are available
- scene snapshot route returns `200` with an image payload when rendering is
  available
- no unexpected reconciliation or kernel errors appear in execution history

## Manual Release Checklist

Run this checklist for release candidates, conference showcase runs, and major HMI
changes.

1. Launch the target scenario and verify the HMI connects without manual refresh
   loops or stale state.
2. Confirm the robot pose shown in the HMI matches backend motion progress.
3. Confirm attached objects visually follow the expected parent frame through
   pickup, transport, and detach.
4. Confirm detached objects return to the expected world pose instead of
   remaining visually attached.
5. Trigger one interrupt scenario and verify the HMI clearly reflects the stop
   or hold state.
6. Resume or reset from the interrupt path and confirm the HMI exits the
   latched state cleanly.
7. Capture one scene snapshot or screenshot per critical phase for retained
   evidence.

## Required Evidence

Store all of the following with the qualification batch:

- scenario artifact ids
- git commit
- container/runtime identifier
- retained debug artifacts
- operator name for the manual pass
- timestamp of the manual run
- notes for any accepted visual deviation

## Current Limitation

ManyForge can automate backend/HMI reconciliation and snapshot availability, but
it still does not have a full unattended browser-assertion framework for
operator-facing layout and interaction semantics. This checklist remains the
release gate for that layer until a stronger browser qualification stack exists.
