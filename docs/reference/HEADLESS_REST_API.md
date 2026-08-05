<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd <repository-root>
    python3 scripts/regen-public-contracts.py headless-rest
  Last regenerated: 2026-08-05 15:47:39 UTC
  Generator source: scripts/regen-public-contracts.py:gen_headless_rest_api
-->

# Headless Runtime REST API

This is the independent I5/H0 projection over Manager, logical-scene, scene-runtime, Alarm Lifecycle, and Historian owners. Reads return owner-issued facts. Alarm acknowledgement and Manager Apply are attributed intents; Apply remains a typed `kernel_owner_unavailable` operation until I6/N supplies the execution-host owner. No condition mutation endpoint exists. `/api/health` reports only `headless_projection_ready`; robot readiness has no H0 owner and is explicitly unavailable. The alarm page defaults to 32 records and is bounded at 128, history defaults to and is bounded at 32 records, request bodies are bounded at 1 MiB, and native/HTTP responses are bounded at 64 MiB.

The live schema is available at `/openapi.json` and `/docs` while the API process is running.

OpenAPI version: `3.1.0`. Title: `ManyForge Headless Runtime API`.

## Routes

| Method | Path | Tag | Summary |
|---|---|---|---|
| `GET` | `/api/execution/alarms` |  | Alarms |
| `POST` | `/api/execution/alarms/acknowledge-all` |  | Acknowledge All |
| `GET` | `/api/execution/alarms/metrics` |  | Alarm Metrics |
| `POST` | `/api/execution/alarms/{condition_id}/acknowledge` |  | Acknowledge Alarm |
| `GET` | `/api/execution/history` |  | History |
| `GET` | `/api/execution/history/health` |  | Historian Health |
| `POST` | `/api/execution/history/reconcile` |  | Historian Reconcile |
| `GET` | `/api/health` |  | Health |
| `POST` | `/api/manager/apply` |  | Manager Apply |
| `GET` | `/api/manager/diff` |  | Manager Diff |
| `GET` | `/api/manager/graph` |  | Manager Graph |
| `GET` | `/api/manager/status` |  | Manager Status |
| `GET` | `/api/node-environment` |  | Node Environment |
| `GET` | `/api/runtime` |  | Runtime |
| `GET` | `/api/scene` |  | Scene |
